#include "AudioRecorder.h"

#include <QAudioDevice>
#include <QAudioFormat>
#include <QAudioSource>
#include <QDataStream>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QMediaDevices>
#include <QProcess>
#include <QTimer>
#include <QVariantMap>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>

namespace AudioRecorderIO {

qint16 clampToInt16(qint32 value)
{
    if (value > std::numeric_limits<qint16>::max())
        return std::numeric_limits<qint16>::max();
    if (value < std::numeric_limits<qint16>::min())
        return std::numeric_limits<qint16>::min();
    return static_cast<qint16>(value);
}

QByteArray gainScalePcm16(const QByteArray &pcm, double gain)
{
    const int sampleCount = pcm.size() / 2;
    QByteArray out(sampleCount * 2, Qt::Uninitialized);
    const qint16 *in = reinterpret_cast<const qint16 *>(pcm.constData());
    qint16 *dst = reinterpret_cast<qint16 *>(out.data());
    for (int i = 0; i < sampleCount; ++i)
        dst[i] = clampToInt16(static_cast<qint32>(std::lround(static_cast<double>(in[i]) * gain)));
    return out;
}

QByteArray mixPcm16(const QByteArray &a, double gainA, const QByteArray &b, double gainB)
{
    const int sampleCount = std::min(a.size(), b.size()) / 2;
    QByteArray out(sampleCount * 2, Qt::Uninitialized);
    const qint16 *inA = reinterpret_cast<const qint16 *>(a.constData());
    const qint16 *inB = reinterpret_cast<const qint16 *>(b.constData());
    qint16 *dst = reinterpret_cast<qint16 *>(out.data());
    for (int i = 0; i < sampleCount; ++i) {
        const qint32 mixed = static_cast<qint32>(std::lround(static_cast<double>(inA[i]) * gainA))
            + static_cast<qint32>(std::lround(static_cast<double>(inB[i]) * gainB));
        dst[i] = clampToInt16(mixed);
    }
    return out;
}

qreal rmsPcm16(const QByteArray &pcm)
{
    const int sampleCount = pcm.size() / 2;
    if (sampleCount <= 0)
        return 0.0;
    const qint16 *samples = reinterpret_cast<const qint16 *>(pcm.constData());
    double sumSquares = 0.0;
    for (int i = 0; i < sampleCount; ++i) {
        const double normalized = static_cast<double>(samples[i]) / 32768.0;
        sumSquares += normalized * normalized;
    }
    return static_cast<qreal>(std::sqrt(sumSquares / sampleCount));
}

qreal displayLevelFromRms(qreal rms)
{
    constexpr qreal kFloorDb = -60.0;
    if (rms <= 0.0)
        return 0.0;
    const qreal db = 20.0 * std::log10(static_cast<double>(rms));
    if (db <= kFloorDb)
        return 0.0;
    return std::min(qreal(1.0), (db - kFloorDb) / -kFloorDb);
}

QByteArray buildWavHeader(quint32 dataSize, quint32 sampleRate, quint16 channelCount, quint16 bitsPerSample)
{
    const quint32 byteRate = sampleRate * channelCount * (bitsPerSample / 8);
    const quint16 blockAlign = channelCount * (bitsPerSample / 8);
    const quint32 riffChunkSize = 36 + dataSize;

    QByteArray header;
    QDataStream out(&header, QIODevice::WriteOnly);
    out.setByteOrder(QDataStream::LittleEndian);
    out.writeRawData("RIFF", 4);
    out << riffChunkSize;
    out.writeRawData("WAVE", 4);
    out.writeRawData("fmt ", 4);
    out << static_cast<quint32>(16); // fmt subchunk size (PCM)
    out << static_cast<quint16>(1); // audio format: 1 = PCM
    out << channelCount;
    out << sampleRate;
    out << byteRate;
    out << blockAlign;
    out << bitsPerSample;
    out.writeRawData("data", 4);
    out << dataSize;
    return header;
}

void patchWavHeader(QFile &file, quint32 dataSize, quint32 sampleRate, quint16 channelCount, quint16 bitsPerSample)
{
    const QByteArray header = buildWavHeader(dataSize, sampleRate, channelCount, bitsPerSample);
    // Restore the write cursor: this is called periodically DURING capture,
    // where leaving the position at 44 would make the next payload append
    // overwrite the recording from its first sample onward.
    const qint64 resumePos = file.pos();
    file.seek(0);
    file.write(header);
    file.flush();
    file.seek(resumePos);
}

QString buildOutputFilePath(const QUrl &vaultUrl, const QString &outputRelativeFolder, const QDateTime &timestamp)
{
    QString base = vaultUrl.toLocalFile();
    if (!outputRelativeFolder.isEmpty())
        base += QLatin1Char('/') + outputRelativeFolder;
    return base + QStringLiteral("/suopAudio_") + timestamp.toString(QStringLiteral("yyyyMMdd-HHmmss"))
        + QStringLiteral(".wav");
}

QVariantList parseSystemSources(const QByteArray &pwDumpJson)
{
    QVariantList result;
    QVariantMap defaultEntry;
    defaultEntry[QStringLiteral("id")] = QStringLiteral("@DEFAULT_MONITOR@");
    defaultEntry[QStringLiteral("name")] = QStringLiteral("Default output");
    result.append(defaultEntry);

    const QJsonDocument doc = QJsonDocument::fromJson(pwDumpJson);
    if (!doc.isArray())
        return result; // malformed/empty/non-array pw-dump output — default only, never a crash.

    QVariantList monitors;
    QVariantList streams;
    for (const QJsonValue &nodeValue : doc.array()) {
        if (!nodeValue.isObject())
            continue;
        const QJsonObject props = nodeValue.toObject().value(QStringLiteral("info")).toObject()
                                       .value(QStringLiteral("props")).toObject();
        if (props.isEmpty())
            continue;
        const QString mediaClass = props.value(QStringLiteral("media.class")).toString();

        if (mediaClass == QLatin1String("Audio/Sink")) {
            const QString nodeName = props.value(QStringLiteral("node.name")).toString();
            if (nodeName.isEmpty())
                continue;
            const QString description = props.value(QStringLiteral("node.description")).toString();
            QVariantMap entry;
            entry[QStringLiteral("id")] = QStringLiteral("monitor:") + nodeName;
            entry[QStringLiteral("name")] = description.isEmpty() ? nodeName : description;
            monitors.append(entry);
        } else if (mediaClass == QLatin1String("Stream/Output/Audio")) {
            // pw-record --target accepts either the numeric object.serial or
            // the node.name; prefer the serial (unambiguous, and what the
            // plan calls the stable target handle), falling back to
            // node.name when a stream has no serial. A stream with neither
            // has nothing usable to target and is skipped.
            QString id;
            if (props.contains(QStringLiteral("object.serial")))
                id = QStringLiteral("stream:") + QString::number(props.value(QStringLiteral("object.serial")).toVariant().toLongLong());
            else if (const QString nodeName = props.value(QStringLiteral("node.name")).toString(); !nodeName.isEmpty())
                id = QStringLiteral("stream:") + nodeName;
            if (id.isEmpty())
                continue;

            QString name = props.value(QStringLiteral("application.name")).toString();
            if (name.isEmpty())
                name = props.value(QStringLiteral("media.name")).toString();
            if (name.isEmpty())
                continue; // nothing worth showing the user.

            QVariantMap entry;
            entry[QStringLiteral("id")] = id;
            entry[QStringLiteral("name")] = name;
            streams.append(entry);
        }
    }

    result += monitors;
    result += streams;
    return result;
}

}

// Lives on AudioRecorder's dedicated QThread; never touches the GUI thread.
// Owns the QAudioSource (mic, pull-mode via the QIODevice* start() returns),
// the parec QProcess (system audio, via its monitor source — see
// AudioDecoder.cpp for this codebase's QProcess shell-out conventions this
// mirrors), two byte-queue buffers (one per source), and a periodic mixing
// QTimer that pops matched-duration chunks from both, mixes them per
// AudioRecorderIO::mixPcm16 (or gain-scales a single source directly if only
// one is enabled — see gainScalePcm16()), and appends the result to the
// in-progress WAV file.
class AudioRecorderWorker : public QObject {
    Q_OBJECT

public:
    using QObject::QObject;
    ~AudioRecorderWorker() override
    {
        if (m_running)
            finalizeAndStop();
    }

public slots:
    void start(const QString &outputPath, bool useMic, const QString &micDeviceId, bool useSystemAudio,
                double micGain, double systemGain, const QString &systemSourceId = QString())
    {
        if (m_running) {
            emit started(false, QStringLiteral("already recording"));
            return;
        }
        if (!useMic && !useSystemAudio) {
            emit started(false, QStringLiteral("no audio source selected"));
            return;
        }

        m_useMic = useMic;
        m_useSystemAudio = useSystemAudio;
        m_micGain = micGain;
        m_systemGain = systemGain;
        m_systemSourceId = systemSourceId;
        m_micQueue.clear();
        m_sysQueue.clear();
        m_bytesWritten = 0;

        const QFileInfo info(outputPath);
        QDir().mkpath(info.absolutePath());
        m_outputFile.setFileName(outputPath);
        if (!m_outputFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            emit started(false, QStringLiteral("failed to open output file: %1").arg(outputPath));
            return;
        }
        // Placeholder header — patched with the real size in finalizeAndStop()
        // once every chunk has been appended (see AudioRecorderIO::
        // buildWavHeader()'s doc comment for why this is written up front
        // rather than the whole recording being buffered to compute a size).
        m_outputFile.write(AudioRecorderIO::buildWavHeader(0, kSampleRate, kChannelCount, kBitsPerSample));

        if (useMic && !startMic(micDeviceId)) {
            m_outputFile.close();
            m_outputFile.remove();
            return; // startMic() already emitted started(false, ...)
        }

        if (useSystemAudio && !startSystemAudio()) {
            if (m_micSource) {
                m_micSource->stop();
                m_micSource.reset();
                m_micDevice = nullptr;
            }
            m_outputFile.close();
            m_outputFile.remove();
            return; // startSystemAudio() already emitted started(false, ...)
        }

        m_elapsed.start();
        m_sinceLastLevelTick.invalidate();
        m_sinceLastCheckpoint.invalidate();
        m_running = true;

        m_mixTimer = new QTimer(this);
        m_mixTimer->setInterval(kMixTimerIntervalMs);
        connect(m_mixTimer, &QTimer::timeout, this, [this] {
            if (m_running)
                mixOnce();
        });
        m_mixTimer->start();

        emit started(true, QString());
    }

    void stop()
    {
        if (!m_running)
            return;
        finalizeAndStop();
    }

private slots:
    void onMicReadyRead()
    {
        if (m_micDevice)
            m_micQueue += m_micDevice->readAll();
    }

    void onParecReadyRead()
    {
        if (m_parec)
            m_sysQueue += m_parec->readAllStandardOutput();
    }

    void onParecErrorOccurred(QProcess::ProcessError)
    {
        if (!m_running)
            return;
        emit recordingError(QStringLiteral("system audio capture (parec) failed unexpectedly"));
        finalizeAndStop();
    }

signals:
    // Result of a start() call — ok=false means nothing is recording and
    // `error` explains why (see AudioRecorder::lastError's doc comment).
    void started(bool ok, const QString &error);
    // Throttled to ~kLevelUpdateIntervalMs — see mixOnce()'s doc comment.
    void levelTick(qreal level, qint64 elapsedMs);
    // A fatal mid-recording failure occurred (parec died, mic device error);
    // finalizeAndStop() (and its own stopped() signal) always follows right
    // behind this on the same call.
    void recordingError(const QString &error);
    // The WAV file at `filePath` is fully finalized and closed.
    void stopped(const QString &filePath);

private:
    static constexpr quint32 kSampleRate = 48000;
    static constexpr quint16 kChannelCount = 2;
    static constexpr quint16 kBitsPerSample = 16;
    static constexpr int kMixTimerIntervalMs = 25;
    static constexpr int kLevelUpdateIntervalMs = 75;
    // How often the WAV header is re-stamped with the size captured so far.
    // This is the crash-durability granularity: an abnormal exit loses at
    // most this much audio. One second is cheap (a 44-byte rewrite plus a
    // flush) against a 192 KB/s payload.
    static constexpr int kHeaderCheckpointIntervalMs = 1000;
    // Bytes per one L+R sample pair — every chunk popped off a queue is
    // trimmed to a multiple of this so a mix never splits a sample across
    // two ticks.
    static constexpr int kFrameSize = (kChannelCount * kBitsPerSample) / 8;

    bool startMic(const QString &micDeviceId)
    {
        QAudioFormat format;
        format.setSampleRate(kSampleRate);
        format.setChannelCount(kChannelCount);
        format.setSampleFormat(QAudioFormat::Int16);

        QAudioDevice device = QMediaDevices::defaultAudioInput();
        if (!micDeviceId.isEmpty()) {
            bool found = false;
            const auto devices = QMediaDevices::audioInputs();
            for (const QAudioDevice &d : devices) {
                if (QString::fromUtf8(d.id()) == micDeviceId) {
                    device = d;
                    found = true;
                    break;
                }
            }
            if (!found) {
                emit started(false, QStringLiteral("microphone device not found: %1").arg(micDeviceId));
                return false;
            }
        }
        if (device.isNull()) {
            emit started(false, QStringLiteral("no microphone device available"));
            return false;
        }

        m_micSource = std::make_unique<QAudioSource>(device, format);
        m_micDevice = m_micSource->start();
        if (!m_micDevice) {
            emit started(false, QStringLiteral("failed to open microphone device"));
            m_micSource.reset();
            return false;
        }
        connect(m_micDevice, &QIODevice::readyRead, this, &AudioRecorderWorker::onMicReadyRead);
        return true;
    }

    // Branches on m_systemSourceId (one of AudioRecorderIO::parseSystemSources()'s
    // opaque ids, or empty — see AudioRecorder::startRecording()'s doc
    // comment):
    //  - empty or the synthetic "@DEFAULT_MONITOR@" → today's unchanged
    //    default-sink monitor path via `parec`.
    //  - "monitor:<node.name>" → that specific sink's monitor via `parec -d
    //    <node.name>.monitor` (same format/rate/channels as the default path).
    //  - "stream:<target>" → that one application's playback stream, tapped
    //    non-invasively via `pw-record --target <target>` (no null-sink /
    //    move-sink-input graph mutation — see item 036's background) with
    //    `--raw` so stdout is bare s16le PCM, matching parec's `--raw` output
    //    exactly for the shared downstream mixer.
    //  - anything else unrecognized → falls back to the default-monitor path
    //    rather than failing outright.
    bool startSystemAudio()
    {
        QString program = QStringLiteral("parec");
        QStringList args = { QStringLiteral("-d"), QStringLiteral("@DEFAULT_MONITOR@"), QStringLiteral("--format=s16le"),
                              QStringLiteral("--rate=48000"), QStringLiteral("--channels=2"), QStringLiteral("--raw") };

        if (m_systemSourceId.startsWith(QStringLiteral("monitor:"))) {
            const QString sinkName = m_systemSourceId.mid(8);
            args = { QStringLiteral("-d"), sinkName + QStringLiteral(".monitor"), QStringLiteral("--format=s16le"),
                      QStringLiteral("--rate=48000"), QStringLiteral("--channels=2"), QStringLiteral("--raw") };
        } else if (m_systemSourceId.startsWith(QStringLiteral("stream:"))) {
            program = QStringLiteral("pw-record");
            const QString target = m_systemSourceId.mid(7);
            args = { QStringLiteral("--target"), target,       QStringLiteral("--rate"),   QStringLiteral("48000"),
                      QStringLiteral("--channels"), QStringLiteral("2"), QStringLiteral("--format"), QStringLiteral("s16"),
                      QStringLiteral("--raw"), QStringLiteral("-") };
        }

        m_parec = std::make_unique<QProcess>();
        connect(m_parec.get(), &QProcess::readyReadStandardOutput, this, &AudioRecorderWorker::onParecReadyRead);
        connect(m_parec.get(), &QProcess::errorOccurred, this, &AudioRecorderWorker::onParecErrorOccurred);
        m_parec->start(program, args);
        if (!m_parec->waitForStarted(3000)) {
            emit started(false,
                         QStringLiteral("failed to start %1 (is PulseAudio/PipeWire-pulse installed and running?)")
                             .arg(program));
            m_parec.reset();
            return false;
        }
        return true;
    }

    // Pops one matched-duration chunk from whichever source(s) are enabled,
    // mixes/gain-scales it, and appends it to the output file — used both by
    // the periodic mixing timer (while m_running) and by finalizeAndStop()'s
    // final drain loop (after m_running has already been cleared, so it
    // bypasses that guard directly). Returns true if it wrote anything, so
    // finalizeAndStop() can loop until the queues are drained below one
    // frame.
    bool mixOnce()
    {
        QByteArray mixed;
        if (m_useMic && m_useSystemAudio) {
            int n = std::min(m_micQueue.size(), m_sysQueue.size());
            n -= n % kFrameSize;
            if (n <= 0)
                return false;
            const QByteArray micChunk = m_micQueue.left(n);
            const QByteArray sysChunk = m_sysQueue.left(n);
            m_micQueue.remove(0, n);
            m_sysQueue.remove(0, n);
            mixed = AudioRecorderIO::mixPcm16(micChunk, m_micGain, sysChunk, m_systemGain);
        } else if (m_useMic) {
            int n = m_micQueue.size();
            n -= n % kFrameSize;
            if (n <= 0)
                return false;
            const QByteArray chunk = m_micQueue.left(n);
            m_micQueue.remove(0, n);
            mixed = AudioRecorderIO::gainScalePcm16(chunk, m_micGain);
        } else if (m_useSystemAudio) {
            int n = m_sysQueue.size();
            n -= n % kFrameSize;
            if (n <= 0)
                return false;
            const QByteArray chunk = m_sysQueue.left(n);
            m_sysQueue.remove(0, n);
            mixed = AudioRecorderIO::gainScalePcm16(chunk, m_systemGain);
        } else {
            return false;
        }

        if (mixed.isEmpty())
            return false;

        m_outputFile.write(mixed);
        m_bytesWritten += static_cast<quint32>(mixed.size());

        // Crash durability. The header written at start() claims dataSize 0,
        // so until it is stamped the file reads as an empty WAV to every
        // player even though the PCM is physically on disk — and QFile
        // buffers, so an unflushed tail is lost outright on a crash.
        // Re-stamping here (patchWavHeader also flushes, and restores the
        // write position) means an abnormal exit leaves a valid recording up
        // to the last checkpoint rather than an unreadable file.
        if (!m_sinceLastCheckpoint.isValid() || m_sinceLastCheckpoint.elapsed() >= kHeaderCheckpointIntervalMs) {
            m_sinceLastCheckpoint.restart();
            AudioRecorderIO::patchWavHeader(m_outputFile, m_bytesWritten, kSampleRate, kChannelCount, kBitsPerSample);
        }

        // Level meter / elapsed time throttling (CLAUDE.md convention, same
        // shape as TranscriptionController's liveUpdateTick): computed every
        // mixing tick (kMixTimerIntervalMs, ~20-30ms) but only pushed to the
        // GUI-thread-visible property at ~kLevelUpdateIntervalMs.
        if (!m_sinceLastLevelTick.isValid() || m_sinceLastLevelTick.elapsed() >= kLevelUpdateIntervalMs) {
            m_sinceLastLevelTick.restart();
            emit levelTick(AudioRecorderIO::rmsPcm16(mixed), m_elapsed.elapsed());
        }
        return true;
    }

    void finalizeAndStop()
    {
        m_running = false;
        if (m_mixTimer) {
            m_mixTimer->stop();
            m_mixTimer->deleteLater();
            m_mixTimer = nullptr;
        }

        // Drain whatever's left in the OS-level buffers one last time so the
        // final few ms of audio aren't lost to a device that hadn't fired
        // readyRead yet at the exact moment stop() was requested.
        if (m_micDevice)
            m_micQueue += m_micDevice->readAll();
        if (m_parec)
            m_sysQueue += m_parec->readAllStandardOutput();
        while (mixOnce()) { }

        if (m_micSource) {
            m_micSource->stop();
            m_micSource.reset();
            m_micDevice = nullptr;
        }
        if (m_parec) {
            m_parec->kill();
            m_parec->waitForFinished(1000);
            m_parec.reset();
        }

        const QString path = m_outputFile.fileName();
        if (m_bytesWritten == 0) {
            // Captured nothing at all (stopped before the first mix tick, or a
            // source that died on open). Writing the 44-byte header alone
            // would drop a zero-length suopAudio_*.wav into the vault, where
            // it shows up in the tree as a real untranscribed audio file.
            // Discard it and report an empty path instead.
            m_outputFile.close();
            m_outputFile.remove();
            emit stopped(QString());
            return;
        }
        AudioRecorderIO::patchWavHeader(m_outputFile, m_bytesWritten, kSampleRate, kChannelCount, kBitsPerSample);
        m_outputFile.close();
        emit stopped(path);
    }

    std::unique_ptr<QAudioSource> m_micSource;
    QIODevice *m_micDevice = nullptr; // owned by m_micSource, not us
    std::unique_ptr<QProcess> m_parec;
    QTimer *m_mixTimer = nullptr;

    QByteArray m_micQueue;
    QByteArray m_sysQueue;

    QFile m_outputFile;
    bool m_useMic = false;
    bool m_useSystemAudio = false;
    double m_micGain = 1.0;
    double m_systemGain = 1.0;
    QString m_systemSourceId;
    quint32 m_bytesWritten = 0;

    QElapsedTimer m_elapsed;
    QElapsedTimer m_sinceLastLevelTick;
    QElapsedTimer m_sinceLastCheckpoint;
    bool m_running = false;
};

AudioRecorder::AudioRecorder(QObject *parent)
    : QObject(parent)
{
    m_worker = new AudioRecorderWorker;
    m_worker->moveToThread(&m_thread);
    connect(&m_thread, &QThread::finished, m_worker, &QObject::deleteLater);

    connect(m_worker, &AudioRecorderWorker::started, this, [this](bool ok, const QString &error) {
        // ok == true needs no action: startRecording() already set m_recording
        // optimistically. Only a genuine failure (device busy, no such mic,
        // parec missing) rolls it back — and it must NOT be written as
        // `m_recording = ok`, which would also flip a live recording to false
        // if a stray rejected start ever reached the worker.
        if (ok)
            return;
        m_recording = false;
        m_lastError = error;
        emit lastErrorChanged();
        emit recordingChanged();
    });
    connect(m_worker, &AudioRecorderWorker::levelTick, this, [this](qreal level, qint64 elapsedMs) {
        m_currentLevel = level;
        m_elapsedMs = elapsedMs;
        emit levelUpdated();
    });
    connect(m_worker, &AudioRecorderWorker::recordingError, this, [this](const QString &error) {
        m_lastError = error;
        emit lastErrorChanged();
    });
    connect(m_worker, &AudioRecorderWorker::stopped, this, [this](const QString &filePath) {
        m_recording = false;
        m_currentLevel = 0.0;
        emit recordingChanged();
        emit levelUpdated();
        emit recordingFinished(filePath);
    });

    m_thread.start();
}

AudioRecorder::~AudioRecorder()
{
    // Same "never leave real work running in the background after this
    // object is gone" requirement as TranscriptionController's destructor,
    // but reached differently: the worker's members (QFile/QProcess/
    // QAudioSource) aren't safe to touch from this (GUI) thread directly, so
    // stop() is posted as a queued call rather than invoked in place. Posted
    // events for one target are delivered in FIFO order, so this is
    // guaranteed to run before the quit() event below stops the worker's
    // event loop.
    if (m_worker)
        QMetaObject::invokeMethod(m_worker, "stop", Qt::QueuedConnection);
    m_thread.quit();
    m_thread.wait();
}

QVariantList AudioRecorder::availableMicDevices() const
{
    QVariantList result;
    const auto devices = QMediaDevices::audioInputs();
    for (const QAudioDevice &device : devices) {
        QVariantMap entry;
        entry[QStringLiteral("id")] = QString::fromUtf8(device.id());
        entry[QStringLiteral("name")] = device.description();
        result.append(entry);
    }
    return result;
}

QVariantList AudioRecorder::availableSystemSources() const
{
    // Short-lived synchronous probe (mirrors GpuProbe.cpp's runProbe()) — a
    // Q_INVOKABLE called on page load/refresh, not on any hot path. Missing
    // `pw-dump` (plain PulseAudio host) or a timeout both fall through to
    // parseSystemSources(QByteArray()), which degrades to the synthetic
    // default entry alone.
    QProcess process;
    process.start(QStringLiteral("pw-dump"), {});
    QByteArray output;
    if (process.waitForStarted(1000) && process.waitForFinished(3000)
        && process.exitStatus() == QProcess::NormalExit) {
        output = process.readAllStandardOutput();
    } else {
        process.kill();
        process.waitForFinished(500);
    }
    return AudioRecorderIO::parseSystemSources(output);
}

void AudioRecorder::startRecording(const QUrl &vaultUrl, const QString &outputRelativeFolder, bool useMic,
                                    const QString &micDeviceId, bool useSystemAudio, double micGain,
                                    double systemGain, const QString &systemSourceId)
{
    if (m_recording) {
        m_lastError = QStringLiteral("already recording");
        emit lastErrorChanged();
        return;
    }
    const QString outputPath =
        AudioRecorderIO::buildOutputFilePath(vaultUrl, outputRelativeFolder, QDateTime::currentDateTime());
    m_lastError.clear();
    emit lastErrorChanged();
    // Reflect the click NOW rather than when the worker's started() gets back
    // across the thread boundary — opening a QAudioSource (and parec's
    // waitForStarted, up to 3s) makes that round trip long enough to see. A
    // stale `recording` here is not just a laggy icon: the QML toggle branches
    // on it, so a second tap during the gap used to post a duplicate start
    // that the worker rejected, and the rejection clobbered this flag while
    // capture was genuinely running. The guard above plus this line close
    // that window. Rolled back below if started() reports a real failure.
    m_recording = true;
    emit recordingChanged();
    QMetaObject::invokeMethod(m_worker, "start", Qt::QueuedConnection, Q_ARG(QString, outputPath),
                               Q_ARG(bool, useMic), Q_ARG(QString, micDeviceId), Q_ARG(bool, useSystemAudio),
                               Q_ARG(double, micGain), Q_ARG(double, systemGain), Q_ARG(QString, systemSourceId));
}

void AudioRecorder::stopRecording()
{
    if (!m_recording)
        return;
    // Symmetric with startRecording(): the button flips back on the click, not
    // when finalization (which includes parec's kill + up to a 1s
    // waitForFinished) reports in. Worker slots for one receiver run FIFO, so
    // an immediately-following start still sees a fully finalized file.
    m_recording = false;
    emit recordingChanged();
    QMetaObject::invokeMethod(m_worker, "stop", Qt::QueuedConnection);
}

#include "AudioRecorder.moc"
