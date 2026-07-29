#include "CombineTask.h"

#include "../AudioDecoder.h"
#include "../ProcessRunner.h"
#include "UncombineTask.h"

#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QProcess>
#include <QTemporaryFile>

#include <algorithm>
#include <cmath>
#include <memory>
#include <optional>

namespace {

// Runs ffprobe on `path` with the given extra arguments and returns trimmed
// stdout, or nullopt on any failure (missing binary, non-zero exit, timeout).
// The one ffprobe spawn site in this file — probeDurationSeconds() and
// probeAudioFormat() below both go through it, so tst_styleguide's
// everyProcessSpawnGuardsChildLifetime() only has one call to verify instead
// of two drifting copies.
std::optional<QString> runFfprobe(const QString &path, const QStringList &args)
{
    QProcess probe;
    ProcessRunner::guardChildLifetime(probe);
    QStringList fullArgs = args;
    fullArgs << path;
    probe.start(QStringLiteral("ffprobe"), fullArgs);
    if (!probe.waitForFinished(10'000) || probe.exitStatus() != QProcess::NormalExit || probe.exitCode() != 0)
        return std::nullopt;
    return QString::fromUtf8(probe.readAllStandardOutput()).trimmed();
}

// Reads a media file's duration in seconds via ffprobe. nullopt on any
// failure (missing binary, unparseable output) — validateCombinedOutput()
// treats "couldn't check" as a validation failure rather than silently
// skipping the check, since ffprobe ships in the same package as `ffmpeg`,
// which this app already hard-requires for decode/playback.
std::optional<double> probeDurationSeconds(const QString &path)
{
    const std::optional<QString> out =
        runFfprobe(path, {QStringLiteral("-v"), QStringLiteral("error"), QStringLiteral("-show_entries"),
                           QStringLiteral("format=duration"), QStringLiteral("-of"), QStringLiteral("csv=p=0")});
    if (!out)
        return std::nullopt;
    bool ok = false;
    const double seconds = out->toDouble(&ok);
    return (ok && seconds >= 0.0) ? std::make_optional(seconds) : std::nullopt;
}

// The stream properties that decide whether `-c copy` can validly
// concatenate two sources together (see ensureSourcesAreFormatCompatible's
// doc comment for what goes wrong when they differ).
struct AudioStreamFormat {
    QString codecName;
    QString sampleRate;
    QString channels;
    QString sampleFmt;

    bool operator==(const AudioStreamFormat &) const = default;
};

// Probes the first audio stream's codec/rate/channels/sample-format. nullopt
// if ffprobe fails or the file has no audio stream.
std::optional<AudioStreamFormat> probeAudioFormat(const QString &path)
{
    const std::optional<QString> out =
        runFfprobe(path, {QStringLiteral("-v"), QStringLiteral("error"), QStringLiteral("-select_streams"),
                           QStringLiteral("a:0"), QStringLiteral("-show_entries"),
                           QStringLiteral("stream=codec_name,sample_rate,channels,sample_fmt"), QStringLiteral("-of"),
                           QStringLiteral("default=nw=1")});
    if (!out)
        return std::nullopt;

    // default=nw=1 prints one KEY=VALUE line per requested field.
    AudioStreamFormat fmt;
    for (const QString &line : out->split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
        const qsizetype eq = line.indexOf(QLatin1Char('='));
        if (eq < 0)
            continue;
        const QString key = line.left(eq);
        const QString value = line.mid(eq + 1);
        if (key == QStringLiteral("codec_name"))
            fmt.codecName = value;
        else if (key == QStringLiteral("sample_rate"))
            fmt.sampleRate = value;
        else if (key == QStringLiteral("channels"))
            fmt.channels = value;
        else if (key == QStringLiteral("sample_fmt"))
            fmt.sampleFmt = value;
    }
    if (fmt.codecName.isEmpty()) // no audio stream, or ffprobe returned nothing usable
        return std::nullopt;
    return fmt;
}

// ffprobe's sample_fmt codes ("s16", "fltp", …) mean nothing to a user
// deciding what to do about a mismatch — translate the ones ffmpeg actually
// produces for PCM/float audio; an unrecognised code is shown raw rather
// than silently dropped.
QString bitDepthLabel(const QString &sampleFmt)
{
    if (sampleFmt == QStringLiteral("u8") || sampleFmt == QStringLiteral("u8p"))
        return QStringLiteral("8-bit");
    if (sampleFmt == QStringLiteral("s16") || sampleFmt == QStringLiteral("s16p"))
        return QStringLiteral("16-bit");
    if (sampleFmt == QStringLiteral("s32") || sampleFmt == QStringLiteral("s32p"))
        return QStringLiteral("32-bit");
    if (sampleFmt == QStringLiteral("flt") || sampleFmt == QStringLiteral("fltp"))
        return QStringLiteral("32-bit float");
    if (sampleFmt == QStringLiteral("dbl") || sampleFmt == QStringLiteral("dblp"))
        return QStringLiteral("64-bit float");
    return sampleFmt;
}

// "44100" -> "44.1 kHz", "48000" -> "48 kHz" — trims a trailing ".0" rather
// than always showing one decimal place.
QString khzLabel(const QString &sampleRateHz)
{
    bool ok = false;
    const double khz = sampleRateHz.toDouble(&ok) / 1000.0;
    if (!ok)
        return sampleRateHz + QStringLiteral(" Hz");
    QString label = QString::number(khz, 'f', 1);
    if (label.endsWith(QStringLiteral(".0")))
        label.chop(2);
    return label + QStringLiteral(" kHz");
}

// Describes only the fields where `fmt` differs from `other`, e.g. "16-bit
// 44.1 kHz" when sample format and rate both differ but channel count
// doesn't — used to build a mismatch error that names the actual difference
// instead of a bare "sources are incompatible".
QString describeDifference(const AudioStreamFormat &fmt, const AudioStreamFormat &other)
{
    QStringList parts;
    if (fmt.sampleFmt != other.sampleFmt)
        parts << bitDepthLabel(fmt.sampleFmt);
    if (fmt.sampleRate != other.sampleRate)
        parts << khzLabel(fmt.sampleRate);
    if (fmt.channels != other.channels)
        parts << QStringLiteral("%1ch").arg(fmt.channels);
    if (fmt.codecName != other.codecName)
        parts << fmt.codecName;
    return parts.join(QLatin1Char(' '));
}

// Stream copy is only valid when every source shares the same codec, sample
// rate, channel count and sample format: ffmpeg's concat demuxer copies each
// source's packets verbatim into a container carrying the FIRST source's
// header, so a later source in a different format gets its bytes
// reinterpreted under the wrong one. It exits 0 anyway — the result is a
// file that's clean audio up to the first mismatched source and structured
// garbage from there to EOF (the exact defect this function exists to
// refuse before it happens; validateCombinedOutput() below is the safety
// net for whatever slips past it). Compares every source against the FIRST,
// since that's the header the merge actually inherits.
void ensureSourcesAreFormatCompatible(const QStringList &sourcePaths)
{
    if (sourcePaths.isEmpty())
        return;
    const std::optional<AudioStreamFormat> reference = probeAudioFormat(sourcePaths.first());
    if (!reference)
        throw TaskError(QStringLiteral("could not read audio format (ffprobe): %1").arg(sourcePaths.first()));

    for (qsizetype i = 1; i < sourcePaths.size(); ++i) {
        const std::optional<AudioStreamFormat> fmt = probeAudioFormat(sourcePaths[i]);
        if (!fmt)
            throw TaskError(QStringLiteral("could not read audio format (ffprobe): %1").arg(sourcePaths[i]));
        if (*fmt == *reference)
            continue;

        // Parts before this one all matched the reference (else we'd have
        // thrown on an earlier iteration), so they can be reported as one run.
        const bool referenceIsPlural = i > 1;
        const QString referenceLabel =
            referenceIsPlural ? QStringLiteral("parts 1-%1").arg(i) : QStringLiteral("part 1");
        throw TaskError(QStringLiteral("cannot combine: %1 is %2, but %3 %4 %5 — stream copy needs identical "
                                        "codec, sample rate, channel count and sample format across every part")
                             .arg(QFileInfo(sourcePaths[i]).fileName(), describeDifference(*fmt, *reference),
                                  referenceLabel, referenceIsPlural ? QStringLiteral("are") : QStringLiteral("is"),
                                  describeDifference(*reference, *fmt)));
    }
}

// Decodes one window of `path` to mono f32le (format-agnostic — just enough
// to inspect samples) and reports whether every sample in it is finite.
// Deliberately NOT AudioDecoder::decodeTo16kMono: that pipeline always
// applies speechnorm, and speechnorm on a NaN/garbage region spins forever.
// Measured: the same garbage region decodes in ~0s once speechnorm is
// dropped, so this is a plain `-i ... -f f32le` pull.
bool windowIsFinite(const QString &path, double offsetSec, double windowSec, QString *errorOut)
{
    QProcess ffmpeg;
    ProcessRunner::guardChildLifetime(ffmpeg);
    ffmpeg.start(QStringLiteral("ffmpeg"),
                 {QStringLiteral("-v"), QStringLiteral("error"), QStringLiteral("-ss"), QString::number(offsetSec),
                  QStringLiteral("-t"), QString::number(windowSec), QStringLiteral("-i"), path, QStringLiteral("-ac"),
                  QStringLiteral("1"), QStringLiteral("-f"), QStringLiteral("f32le"), QStringLiteral("-hide_banner"),
                  QStringLiteral("-loglevel"), QStringLiteral("error"), QStringLiteral("-")});
    // No speechnorm in this decode (see doc comment above), so a garbage
    // region does not spin — 15s is generous slack for a 2s window, not a
    // real budget being spent.
    if (!ffmpeg.waitForFinished(15'000)) {
        ffmpeg.kill();
        if (errorOut)
            *errorOut = QStringLiteral("validation decode did not finish near %1s").arg(offsetSec);
        return false;
    }
    if (ffmpeg.exitStatus() != QProcess::NormalExit || ffmpeg.exitCode() != 0) {
        if (errorOut)
            *errorOut = QString::fromUtf8(ffmpeg.readAllStandardError());
        return false;
    }
    const QByteArray raw = ffmpeg.readAllStandardOutput();
    const auto *samples = reinterpret_cast<const float *>(raw.constData());
    const qsizetype count = raw.size() / static_cast<qsizetype>(sizeof(float));
    for (qsizetype i = 0; i < count; ++i) {
        if (!std::isfinite(samples[i])) {
            if (errorOut)
                *errorOut = QStringLiteral("non-finite sample near %1s").arg(offsetSec);
            return false;
        }
    }
    return true;
}

// 2s sampled every 60s: an exhaustive decode-and-scan of a multi-hour combine
// costs real minutes of CPU for a check that's a smoke test on top of the
// (much cheaper) duration cross-check below. The known real-world corrupt
// combines this validation exists for were clean up to a point and then
// garbage straight through to EOF, not scattered glitches — so any window
// landing after that point catches it, and a minute-long stride reliably
// lands one within any contiguous garbage run over a minute long. Verified
// directly against a preserved corrupt fixture: this stride catches it at
// its 120s and 180s windows, and passes clean on a known-good 3.3-hour file
// from a real vault in a few seconds.
constexpr double kValidationWindowSec = 2.0;
constexpr double kValidationStrideSec = 60.0;

// Empty return == passed. Two complementary, cheap checks — see the Root
// cause discussion this validation exists to close: `ffmpeg -f concat -c
// copy` exits 0 whenever it wrote packets, regardless of whether the sources
// were actually compatible for a byte-level copy.
QString validateCombinedOutput(const QString &outputPath, const QStringList &sourcePaths)
{
    const std::optional<double> outputDuration = probeDurationSeconds(outputPath);
    if (!outputDuration)
        return QStringLiteral("could not read merged output's duration (ffprobe)");

    double summedSourceDuration = 0.0;
    for (const QString &source : sourcePaths) {
        const std::optional<double> d = probeDurationSeconds(source);
        if (!d)
            return QStringLiteral("could not read source duration (ffprobe): %1").arg(source);
        summedSourceDuration += *d;
    }

    // A format-mismatched `-c copy` concat reliably produces a duration that
    // doesn't add up (the bytes get reinterpreted under the first stream's
    // format — confirmed empirically: two sources at different sample rates
    // concat to a duration ~1.9x the true summed length). 2% relative,
    // floored at 1s so container/frame rounding doesn't false-positive on
    // short clips.
    const double tolerance = std::max(1.0, summedSourceDuration * 0.02);
    if (std::abs(*outputDuration - summedSourceDuration) > tolerance) {
        return QStringLiteral("merged duration %1s doesn't match summed source duration %2s (tolerance %3s)")
            .arg(*outputDuration)
            .arg(summedSourceDuration)
            .arg(tolerance);
    }

    for (double offset = 0.0; offset < *outputDuration; offset += kValidationStrideSec) {
        QString err;
        if (!windowIsFinite(outputPath, offset, kValidationWindowSec, &err))
            return QStringLiteral("sample sanity check failed: %1").arg(err);
    }

    return {};
}

} // namespace

CombineTask::CombineTask(QStringList sourcePaths, QUrl outputUrl, bool deleteOriginals, QObject *parent)
    : Task(parent)
    , m_sourcePaths(std::move(sourcePaths))
    , m_deleteOriginals(deleteOriginals)
{
    fileUrl = std::move(outputUrl);
}

QString CombineTask::displayName() const
{
    return QStringLiteral("Combine %1 files").arg(m_sourcePaths.size());
}

QJsonObject CombineTask::toJson() const
{
    return {{QStringLiteral("combineSourcePaths"), QJsonArray::fromStringList(m_sourcePaths)},
            {QStringLiteral("deleteOriginalsAfterCombine"), m_deleteOriginals}};
}

QVariantMap CombineTask::toVariantMap() const
{
    // combineSourcePaths: the exact concatenation order, so a caller/test can
    // see WHICH files this merge covers and in what order.
    return {{QStringLiteral("combineSourcePaths"), m_sourcePaths},
            {QStringLiteral("outputPath"), fileUrl.toLocalFile()},
            {QStringLiteral("deleteOriginalsAfterCombine"), m_deleteOriginals}};
}

void CombineTask::run(TaskContext &ctx)
{
    const QString outputPath = fileUrl.toLocalFile();
    emit phase(QStringLiteral("combining"));

    QTemporaryFile listFile;
    if (!listFile.open())
        throw TaskError(QStringLiteral("could not create ffmpeg concat list"));

    // The merged file stands in for the original recording, so it inherits
    // the first part's mtime instead of "now". Read before ffmpeg runs,
    // because the sources move into .combined/ afterwards. m_sourcePaths is
    // in combine order, so .first() is the part the recording actually
    // starts with.
    const QDateTime firstPartMtime =
        m_sourcePaths.isEmpty() ? QDateTime() : QFileInfo(m_sourcePaths.first()).lastModified();

    // Refuse before spending a run's worth of ffmpeg time (or touching any
    // source) on a merge that can't be a valid stream copy — see the doc
    // comment on ensureSourcesAreFormatCompatible().
    ensureSourcesAreFormatCompatible(m_sourcePaths);

    qint64 totalInputBytes = 0;
    for (const QString &path : std::as_const(m_sourcePaths)) {
        // ffmpeg concat demuxer syntax: single-quoted paths, any literal
        // single quote escaped as '\''.
        QString escaped = path;
        escaped.replace(QStringLiteral("'"), QStringLiteral("'\\''"));
        listFile.write(QStringLiteral("file '%1'\n").arg(escaped).toUtf8());
        totalInputBytes += QFileInfo(path).size();
    }
    listFile.close();

    QProcess ffmpeg;
    ProcessRunner::guardChildLifetime(ffmpeg);
    // -loglevel warning (not error): a format-mismatched concat is refused
    // earlier now (see ensureSourcesAreFormatCompatible()), but ffmpeg's own
    // non-matching-stream warnings are still worth keeping for whatever slips
    // past that check — stderr is drained during the run, so they survive
    // into a failure's error text instead of being thrown away.
    ffmpeg.start(QStringLiteral("ffmpeg"),
                  {QStringLiteral("-f"), QStringLiteral("concat"), QStringLiteral("-safe"), QStringLiteral("0"),
                   QStringLiteral("-i"), listFile.fileName(), QStringLiteral("-c"), QStringLiteral("copy"),
                   QStringLiteral("-y"), QStringLiteral("-hide_banner"), QStringLiteral("-loglevel"),
                   QStringLiteral("warning"), outputPath});
    if (!ffmpeg.waitForStarted())
        throw TaskError(QStringLiteral("failed to start ffmpeg"));

    // Same watchdog as AudioDecoder (see isDecodeStalled's doc comment): the
    // concat is a straight copy, so the output file should grow continuously
    // the whole time ffmpeg works. Keyed on the OUTPUT file's size rather than
    // stdout, since concat writes straight to `outputPath`, not a pipe.
    QElapsedTimer noGrowthClock;
    noGrowthClock.start();
    qint64 lastOutBytes = 0;
    const auto stalled = [&] {
        if (ctx.pauseRequested()) {
            noGrowthClock.restart();
            return false;
        }
        return AudioDecoder::isDecodeStalled(noGrowthClock.elapsed());
    };

    // A pathological source is failing anyway — the first lines are the
    // informative ones, so cap accumulated stderr rather than let it grow
    // unbounded across the whole run (mirrors AudioDecoder's kMaxErrTextBytes).
    constexpr qint64 kMaxErrTextBytes = 64 * 1024;
    QByteArray errText;
    qint64 errTextDroppedBytes = 0;
    // stderr needs draining every iteration, same reasoning as AudioDecoder's
    // onPoll: its pipe buffer is finite, and an ffmpeg blocked writing a
    // flood of per-frame errors never exits, wedging this poll loop even
    // though it isn't watching stdout at all (concat writes straight to
    // `outputPath`, not a pipe — stderr is still a pipe).
    const auto appendErr = [&](const QByteArray &chunk) {
        if (chunk.isEmpty())
            return;
        const qint64 room = kMaxErrTextBytes - errText.size();
        if (room <= 0) {
            errTextDroppedBytes += chunk.size();
            return;
        }
        if (chunk.size() <= room) {
            errText += chunk;
        } else {
            errText += chunk.left(room);
            errTextDroppedBytes += chunk.size() - room;
        }
    };

    // Pausable poll (see ProcessRunner::runPausable). Progress is approximate
    // (output file size vs. summed input size); reportPaused mirrors the
    // SIGSTOP/SIGCONT state up to the task UI.
    const auto outcome = ProcessRunner::runPausable(
        ffmpeg,
        {[&ctx] { return ctx.cancelRequested(); }, [&ctx] { return ctx.pauseRequested(); },
         [&ctx](bool paused) { ctx.reportPaused(paused); }, stalled},
        [&] {
            appendErr(ffmpeg.readAllStandardError());
            const qint64 outBytes = QFileInfo(outputPath).size();
            if (outBytes != lastOutBytes) {
                lastOutBytes = outBytes;
                noGrowthClock.restart();
            }
            if (totalInputBytes > 0) {
                emit progress(
                    std::clamp(static_cast<double>(outBytes) / static_cast<double>(totalInputBytes), 0.0, 1.0));
            }
        });
    appendErr(ffmpeg.readAllStandardError()); // final drain after the last poll

    if (outcome == ProcessRunner::RunOutcome::Cancelled) {
        QFile::remove(outputPath);
        throw TaskError(QStringLiteral("Cancelled"));
    }

    if (outcome == ProcessRunner::RunOutcome::Stalled) {
        QFile::remove(outputPath);
        throw TaskError(QStringLiteral("combine stalled — ffmpeg produced no output for 60s "
                                        "(one of the source files may contain corrupt audio)"));
    }

    if (ffmpeg.exitStatus() != QProcess::NormalExit || ffmpeg.exitCode() != 0) {
        // Report the accumulated buffer, not a fresh readAllStandardError():
        // stderr was drained continuously above, so by now the pipe is empty
        // and a post-exit read would return almost nothing.
        QString err = QString::fromUtf8(errText);
        if (errTextDroppedBytes > 0)
            err += QStringLiteral(" [truncated, %1 more bytes of stderr omitted]").arg(errTextDroppedBytes);
        QFile::remove(outputPath);
        throw TaskError(err.isEmpty() ? QStringLiteral("ffmpeg concat failed") : err);
    }

    // ffmpeg exiting 0 is not proof of a coherent output — `-f concat -c
    // copy` exits 0 whenever it wrote packets, even across sources it
    // silently reinterpreted under the wrong format (see
    // validateCombinedOutput's doc comment). On failure, quarantine the
    // suspect output — rename, not delete, since it's the only evidence of
    // what went wrong — and leave every source untouched; the whole point of
    // this check is that a bad merge must never cost the user their
    // originals.
    const QString validationError = validateCombinedOutput(outputPath, m_sourcePaths);
    if (!validationError.isEmpty()) {
        const QFileInfo outInfo(outputPath);
        const QString suspectPath = outInfo.absolutePath() + QStringLiteral("/") + outInfo.completeBaseName()
            + QStringLiteral(".suspect.") + outInfo.suffix();
        QFile::remove(suspectPath); // clear a stale quarantine from a prior failed attempt
        QFile::rename(outputPath, suspectPath);
        throw TaskError(QStringLiteral("combine validation failed: %1").arg(validationError));
    }

    emit progress(1.0);

    // Restamp only now that ffmpeg has exited and closed the output.
    if (firstPartMtime.isValid()) {
        QFile out(outputPath);
        if (out.open(QIODevice::ReadWrite))
            out.setFileTime(firstPartMtime, QFileDevice::FileModificationTime);
    }

    // Move (or delete) every source now that the merge succeeded AND passed
    // validateCombinedOutput() above — never reached if validation rejected
    // the output, so a bad merge can't destroy the only remaining copies.
    for (const QString &path : std::as_const(m_sourcePaths)) {
        if (m_deleteOriginals) {
            QFile::remove(path);
            continue;
        }
        const QFileInfo info(path);
        QDir combinedDir(info.absolutePath() + QStringLiteral("/.combined"));
        if (!combinedDir.exists())
            combinedDir.mkpath(QStringLiteral("."));
        // Record only what really moved — a part left behind has no archived
        // copy, so undo must not claim it can restore it.
        if (QFile::rename(path, combinedDir.absoluteFilePath(info.fileName())))
            m_archivedSources << path;
    }

    // Success: let the controller log this merge durably (see the signal's doc
    // comment). Last, so it only fires once the sources are actually dealt with.
    emit combineCompleted(outputPath, m_sourcePaths, m_deleteOriginals);
}

QString CombineTask::undoLabel() const
{
    if (m_archivedSources.isEmpty())
        return {}; // deleteOriginals, or run() hasn't succeeded — no inverse
    return QStringLiteral("Combined %1 files").arg(m_archivedSources.size());
}

std::unique_ptr<Task> CombineTask::makeUndoTask() const
{
    if (m_archivedSources.isEmpty())
        return nullptr;
    return std::make_unique<UncombineTask>(m_archivedSources, fileUrl, vaultUrl);
}

bool CombineTask::undoStillApplicable() const
{
    if (m_archivedSources.isEmpty() || !QFileInfo::exists(fileUrl.toLocalFile()))
        return false;
    // Every archived part must still be sitting in .combined/, and the merge
    // must not have been transcribed since (undo would discard that .md). The
    // sidecar is <fileName>.md — full filename + ".md", NOT completeBaseName
    // (see TranscriptionController's sidecarPath()).
    const QFileInfo mergedInfo(fileUrl.toLocalFile());
    if (QFileInfo::exists(mergedInfo.absolutePath() + QStringLiteral("/") + mergedInfo.fileName()
                          + QStringLiteral(".md")))
        return false;
    for (const QString &original : m_archivedSources) {
        const QFileInfo info(original);
        if (!QFileInfo::exists(info.absolutePath() + QStringLiteral("/.combined/") + info.fileName()))
            return false;
    }
    return true;
}
