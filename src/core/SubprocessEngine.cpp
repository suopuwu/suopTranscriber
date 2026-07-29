#include "SubprocessEngine.h"
#include "ProcessRunner.h"

#include <QByteArray>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace {

// Serializes 16 kHz mono float PCM to a 16-bit PCM WAV at `path`. The chunk is
// already the exact format an engine wants (AudioDecoder produced it), so this
// is a plain header + sample write — no ffmpeg re-invocation. Returns false if
// the file can't be opened.
bool writeMono16kWav(const QString &path, const std::vector<float> &pcm)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;

    constexpr uint32_t sampleRate = 16000;
    constexpr uint16_t channels = 1;
    constexpr uint16_t bitsPerSample = 16;
    constexpr uint16_t blockAlign = channels * bitsPerSample / 8;
    constexpr uint32_t byteRate = sampleRate * blockAlign;
    const uint32_t dataBytes = static_cast<uint32_t>(pcm.size()) * sizeof(int16_t);
    const uint32_t riffBytes = 36 + dataBytes;

    const auto putU32 = [&file](uint32_t v) {
        char b[4] = {char(v & 0xff), char((v >> 8) & 0xff), char((v >> 16) & 0xff), char((v >> 24) & 0xff)};
        file.write(b, 4);
    };
    const auto putU16 = [&file](uint16_t v) {
        char b[2] = {char(v & 0xff), char((v >> 8) & 0xff)};
        file.write(b, 2);
    };

    file.write("RIFF", 4);
    putU32(riffBytes);
    file.write("WAVE", 4);
    file.write("fmt ", 4);
    putU32(16);      // PCM fmt chunk size
    putU16(1);       // PCM
    putU16(channels);
    putU32(sampleRate);
    putU32(byteRate);
    putU16(blockAlign);
    putU16(bitsPerSample);
    file.write("data", 4);
    putU32(dataBytes);

    QByteArray samples;
    samples.reserve(static_cast<int>(dataBytes));
    for (float f : pcm) {
        const float clamped = std::max(-1.0f, std::min(1.0f, f));
        const auto s = static_cast<int16_t>(std::lround(clamped * 32767.0f));
        samples.append(char(s & 0xff));
        samples.append(char((s >> 8) & 0xff));
    }
    file.write(samples);
    file.close();
    return true;
}

// Parses whatever whole '\n'-terminated lines are buffered, leaving any
// trailing partial line in `buffer` for the next read. Each line is one JSONL
// message; dispatch is by "type". Timestamps are taken verbatim (chunk-relative
// — the app applies the offset merge, not this engine). Sets *fatal when a
// non-recoverable error line arrives.
void drainLines(QByteArray &buffer, std::vector<Segment> &segments, const ProgressCallback &onProgress,
                const SegmentCallback &onSegment, bool *fatal)
{
    int newline;
    while ((newline = buffer.indexOf('\n')) >= 0) {
        const QByteArray line = buffer.left(newline).trimmed();
        buffer.remove(0, newline + 1);
        if (line.isEmpty())
            continue;

        QJsonParseError parseError{};
        const QJsonDocument doc = QJsonDocument::fromJson(line, &parseError);
        if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
            qWarning() << "SubprocessEngine: skipping unparseable line:" << line;
            continue;
        }
        const QJsonObject obj = doc.object();
        const QString type = obj.value(QStringLiteral("type")).toString();

        if (type == QLatin1String("segment")) {
            Segment seg;
            seg.t0 = obj.value(QStringLiteral("t0")).toDouble();
            seg.t1 = obj.value(QStringLiteral("t1")).toDouble();
            seg.text = obj.value(QStringLiteral("text")).toString().toStdString();
            segments.push_back(seg);
            if (onSegment)
                onSegment(seg);
        } else if (type == QLatin1String("progress")) {
            if (onProgress) {
                onProgress(obj.value(QStringLiteral("seconds_done")).toDouble(),
                           obj.value(QStringLiteral("seconds_total")).toDouble());
            }
        } else if (type == QLatin1String("error")) {
            const QString message = obj.value(QStringLiteral("message")).toString();
            const bool recoverable = obj.value(QStringLiteral("recoverable")).toBool(false);
            qWarning() << "SubprocessEngine: engine reported error:" << message
                       << (recoverable ? "(recoverable)" : "(fatal)");
            if (!recoverable && fatal)
                *fatal = true;
        }
        // "done" needs no handling here — the child exits after it and the
        // poll loop ends when the process leaves the Running state.
    }
}

} // namespace

SubprocessEngine::SubprocessEngine(EngineManifest manifest, QString manifestDir, QString scratchBaseDir,
                                   QString language)
    : m_manifest(std::move(manifest)), m_manifestDir(std::move(manifestDir)),
      m_scratchBaseDir(std::move(scratchBaseDir)), m_language(std::move(language))
{
}

std::vector<Segment> SubprocessEngine::transcribe(const AudioChunk &chunk, const ProgressCallback &onProgress,
                                                   const SegmentCallback &onSegment, const AbortCallback &onAbort)
{
    std::vector<Segment> segments;

    const QString scratchBase =
        m_scratchBaseDir.isEmpty() ? QStandardPaths::writableLocation(QStandardPaths::TempLocation) : m_scratchBaseDir;
    // Self-deleting per-call scratch dir (auto-removed on scope exit, including
    // every early return below) — never the vault or its .transcriber config.
    QTemporaryDir scratch(QDir(scratchBase).filePath(QStringLiteral("suopTranscriber-engine-XXXXXX")));
    if (!scratch.isValid()) {
        qWarning() << "SubprocessEngine: could not create scratch dir under" << scratchBase;
        return segments;
    }

    const QString wavPath = scratch.filePath(QStringLiteral("chunk.wav"));
    if (!writeMono16kWav(wavPath, chunk.pcm16kMono)) {
        qWarning() << "SubprocessEngine: could not write chunk WAV to" << wavPath;
        return segments;
    }

    if (m_manifest.command.isEmpty()) {
        qWarning() << "SubprocessEngine: manifest has no command";
        return segments;
    }
    const QString program = m_manifest.command.first();
    QStringList arguments = m_manifest.command.mid(1);
    arguments << QStringLiteral("--audio") << wavPath << QStringLiteral("--offset")
              << QString::number(chunk.offsetSeconds, 'f', 3) << QStringLiteral("--lang") << m_language;

    QProcess process;
    process.setProcessChannelMode(QProcess::SeparateChannels);
    if (!m_manifestDir.isEmpty())
        process.setWorkingDirectory(m_manifestDir);
    ProcessRunner::guardChildLifetime(process);
    process.start(program, arguments);
    if (!process.waitForStarted(5000)) {
        qWarning() << "SubprocessEngine: failed to start" << program << arguments << ":" << process.errorString();
        return segments;
    }
    process.closeWriteChannel();

    QByteArray buffer;
    bool fatal = false;
    bool cancelled = false;
    // 100ms poll so pause/cancel feel instant — same cadence and pipe-drain
    // discipline as AudioDecoder::decodeTo16kMono(). onAbort() blocks while
    // paused (stalling this drain, which back-pressures the child) and returns
    // true on cancel.
    while (process.state() != QProcess::NotRunning) {
        process.waitForReadyRead(100);
        buffer += process.readAllStandardOutput();
        drainLines(buffer, segments, onProgress, onSegment, &fatal);
        if (fatal) {
            process.kill();
            process.waitForFinished(2000);
            break;
        }
        if (onAbort && onAbort()) {
            cancelled = true;
            process.kill();
            process.waitForFinished(2000);
            break;
        }
    }
    // Final drain of anything buffered between the last poll and exit.
    buffer += process.readAllStandardOutput();
    if (!buffer.endsWith('\n'))
        buffer += '\n'; // let drainLines see a final unterminated line
    drainLines(buffer, segments, onProgress, onSegment, &fatal);

    if (cancelled)
        return segments; // partial results, same as WhisperCppEngine on abort

    if (!fatal && (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0)) {
        qWarning() << "SubprocessEngine:" << program << "exited abnormally (status"
                   << process.exitStatus() << "code" << process.exitCode()
                   << ") —" << QString::fromUtf8(process.readAllStandardError()).trimmed();
        // Degrade gracefully: return whatever segments streamed before the
        // failure rather than throwing or crashing the app.
    }
    return segments;
}
