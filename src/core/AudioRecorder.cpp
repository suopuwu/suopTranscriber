#include "AudioRecorder.h"

#include <QAudioDevice>
#include <QAudioFormat>
#include <QAudioSource>
#include <QDataStream>
#include <QDebug>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QMap>
#include <QMediaDevices>
#include <QProcess>
#include <QRegularExpression>
#include <QTimer>
#include <QVariantMap>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <vector>

namespace AudioRecorderIO {

qint16 clampToInt16(qint32 value)
{
    if (value > std::numeric_limits<qint16>::max())
        return std::numeric_limits<qint16>::max();
    if (value < std::numeric_limits<qint16>::min())
        return std::numeric_limits<qint16>::min();
    return static_cast<qint16>(value);
}

QByteArray mixPcm16N(const QList<QByteArray> &buffers, const QList<double> &gains)
{
    int sampleCount = 0;
    for (const QByteArray &buffer : buffers)
        sampleCount = std::max(sampleCount, static_cast<int>(buffer.size() / 2));
    if (sampleCount == 0)
        return QByteArray();

    QByteArray out(sampleCount * 2, Qt::Uninitialized);
    qint16 *dst = reinterpret_cast<qint16 *>(out.data());
    for (int i = 0; i < sampleCount; ++i) {
        qint32 mixed = 0;
        for (int k = 0; k < buffers.size(); ++k) {
            const int bufferSampleCount = buffers[k].size() / 2;
            if (i >= bufferSampleCount)
                continue; // shorter/empty buffer contributes silence past its end.
            const qint16 *in = reinterpret_cast<const qint16 *>(buffers[k].constData());
            mixed += static_cast<qint32>(std::lround(static_cast<double>(in[i]) * gains.value(k, 1.0)));
        }
        dst[i] = clampToInt16(mixed);
    }
    return out;
}

MixTickPlan planMixTick(const QList<int> &queuedBytes, const QList<bool> &wasStalled, int stallThresholdBytes,
                        int frameSize)
{
    MixTickPlan plan;
    if (queuedBytes.isEmpty())
        return plan;

    const int maxQ = *std::max_element(queuedBytes.begin(), queuedBytes.end());
    int minNonStalled = -1;
    for (int k = 0; k < queuedBytes.size(); ++k) {
        const bool stalled = (wasStalled.value(k) && queuedBytes[k] == 0) || (maxQ - queuedBytes[k] > stallThresholdBytes);
        plan.stalled.append(stalled);
        if (!stalled && (minNonStalled < 0 || queuedBytes[k] < minNonStalled))
            minNonStalled = queuedBytes[k];
    }

    if (minNonStalled < 0)
        return plan; // every source is stalled — nothing to mix yet.
    plan.bytes = minNonStalled - (minNonStalled % frameSize);
    return plan;
}

QVariantList filterAppStreams(const QVariantList &systemSources)
{
    QVariantList result;
    for (const QVariant &entry : systemSources) {
        const QVariantMap map = entry.toMap();
        if (map.value(QStringLiteral("id")).toString().startsWith(QStringLiteral("stream:")))
            result.append(entry);
    }
    return result;
}

QVariantList buildAppCaptureRows(const QVariantList &appStreams, const QStringList &rememberedNames)
{
    QMap<QString, QStringList> idsByName; // QMap: sorted by key, which IS the row order.
    for (const QString &name : rememberedNames) {
        if (!name.isEmpty())
            idsByName.insert(name, {});
    }
    for (const QVariant &entry : appStreams) {
        const QVariantMap map = entry.toMap();
        const QString name = map.value(QStringLiteral("name")).toString();
        const QString id = map.value(QStringLiteral("id")).toString();
        if (name.isEmpty() || id.isEmpty())
            continue;
        idsByName[name].append(id);
    }

    QVariantList rows;
    for (auto it = idsByName.cbegin(); it != idsByName.cend(); ++it) {
        QVariantMap row;
        row[QStringLiteral("name")] = it.key();
        row[QStringLiteral("ids")] = it.value();
        row[QStringLiteral("active")] = !it.value().isEmpty();
        rows.append(row);
    }
    return rows;
}

QVariantList pendingCaptureAdditions(const QVariantList &desired, const QStringList &activeIds)
{
    QVariantList pending;
    QStringList seen = activeIds;
    for (const QVariant &entry : desired) {
        const QVariantMap map = entry.toMap();
        const QString id = map.value(QStringLiteral("id")).toString();
        if (id.isEmpty() || seen.contains(id))
            continue;
        seen.append(id); // a duplicate WITHIN `desired` must not add twice either.
        pending.append(entry);
    }
    return pending;
}

QStringList captureCommand(const QString &sourceId)
{
    if (sourceId.startsWith(QStringLiteral("monitor:"))) {
        const QString sinkName = sourceId.mid(8);
        return { QStringLiteral("parec"), QStringLiteral("-d"), sinkName + QStringLiteral(".monitor"),
                 QStringLiteral("--format=s16le"), QStringLiteral("--rate=48000"), QStringLiteral("--channels=2"),
                 QStringLiteral("--raw") };
    }
    if (sourceId.startsWith(QStringLiteral("stream:"))) {
        const QString target = sourceId.mid(7);
        return { QStringLiteral("pw-record"), QStringLiteral("--target"), target, QStringLiteral("--rate"),
                 QStringLiteral("48000"), QStringLiteral("--channels"), QStringLiteral("2"), QStringLiteral("--format"),
                 QStringLiteral("s16"), QStringLiteral("--raw"), QStringLiteral("-") };
    }
    // Default/unrecognized case — today's default-sink monitor path.
    return { QStringLiteral("parec"), QStringLiteral("-d"), QStringLiteral("@DEFAULT_MONITOR@"),
             QStringLiteral("--format=s16le"), QStringLiteral("--rate=48000"), QStringLiteral("--channels=2"),
             QStringLiteral("--raw") };
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

qreal decayPeak(qreal previousPeak, qreal currentLevel, qreal decayPerTick)
{
    const qreal decayed = std::max(qreal(0.0), previousPeak - std::max(qreal(0.0), decayPerTick));
    return std::max(decayed, std::max(qreal(0.0), currentLevel));
}

QVariantMap holdPeak(qreal previousPeak, qint64 previousHoldUntilMs, qreal currentLevel,
                     qint64 nowMs, qint64 holdMs, qreal decayPerTick)
{
    const qreal level = std::max(qreal(0.0), currentLevel);
    QVariantMap result;
    if (level >= previousPeak) {
        // A new maximum: jump to it and restart the hold.
        result[QStringLiteral("peak")] = level;
        result[QStringLiteral("holdUntil")] = nowMs + std::max(qint64(0), holdMs);
        return result;
    }
    if (nowMs < previousHoldUntilMs) {
        // Still holding — the whole point: the line does not move.
        result[QStringLiteral("peak")] = previousPeak;
        result[QStringLiteral("holdUntil")] = previousHoldUntilMs;
        return result;
    }
    // Hold expired: release, never below the live level.
    result[QStringLiteral("peak")] = decayPeak(previousPeak, level, decayPerTick);
    result[QStringLiteral("holdUntil")] = previousHoldUntilMs;
    return result;
}

namespace {
// Same -60 dBFS floor displayLevelFromRms maps against, so a dBFS threshold
// converts to a display level by the identical formula.
constexpr qreal kDisplayFloorDb = -60.0;
constexpr qreal kGoodFloorDb = -27.0;
constexpr qreal kHotFloorDb = -6.0;

constexpr qreal displayForDb(qreal db)
{
    return (db - kDisplayFloorDb) / -kDisplayFloorDb;
}
} // namespace

qreal levelBandGoodFloor() { return displayForDb(kGoodFloorDb); }
qreal levelBandHotFloor() { return displayForDb(kHotFloorDb); }

qreal displayFloorDb() { return kDisplayFloorDb; }

qreal dbFromDisplayLevel(qreal displayLevel)
{
    const qreal clamped = std::min(qreal(1.0), std::max(qreal(0.0), displayLevel));
    return kDisplayFloorDb + clamped * -kDisplayFloorDb;
}

QString levelBand(qreal displayLevel)
{
    if (displayLevel >= levelBandHotFloor())
        return QStringLiteral("hot");
    if (displayLevel >= levelBandGoodFloor())
        return QStringLiteral("good");
    return QStringLiteral("low");
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

QString sanitizeRecordingName(const QString &requestedName)
{
    QString name = requestedName.trimmed();
    // A user who types "notes.wav" means the recording "notes", not
    // "notes.wav.wav".
    if (name.endsWith(QStringLiteral(".wav"), Qt::CaseInsensitive))
        name.chop(4);
    static const QRegularExpression invalid(QStringLiteral("[/\\\\:*?\"<>|\\x00-\\x1f]"));
    name.remove(invalid);
    // Leading/trailing dots go last, after the character strip: it is what
    // stops "." and ".." (names that mean a DIRECTORY, not a new child — the
    // same pair FileRename.h's isValidVaultEntryName() rejects) from
    // surviving, and stops a hidden ".foo" recording.
    while (name.startsWith(QLatin1Char('.')) || name.endsWith(QLatin1Char('.')))
        name = name.startsWith(QLatin1Char('.')) ? name.mid(1) : name.chopped(1);
    name = name.trimmed();
    // Most Linux filesystems cap a component at 255 bytes; 120 leaves room for
    // the "_N" suffix and a ".partial.jsonl" sidecar on the same base name.
    return name.left(120);
}

QString buildOutputFilePath(const QUrl &vaultUrl, const QString &outputRelativeFolder,
                            const QDateTime &timestamp, const QString &requestedName)
{
    QString base = vaultUrl.toLocalFile();
    if (!outputRelativeFolder.isEmpty())
        base += QLatin1Char('/') + outputRelativeFolder;
    const QString name = sanitizeRecordingName(requestedName);
    return base + QLatin1Char('/')
        + (name.isEmpty()
               ? QStringLiteral("suopAudio_") + timestamp.toString(QStringLiteral("yyyyMMdd-HHmmss"))
               : name)
        + QStringLiteral(".wav");
}

QString buildTestOutputFilePath(const QString &tempRoot, const QDateTime &timestamp,
                                 const QString &requestedName)
{
    return buildOutputFilePath(QUrl::fromLocalFile(tempRoot + QStringLiteral("/suopTranscriber-test")),
                               QString(), timestamp, requestedName);
}

QString uniquePathForRecording(const QString &desiredPath)
{
    const QFileInfo info(desiredPath);
    const QString dir = info.absolutePath();
    const QString baseName = info.completeBaseName();
    const QString suffix = info.suffix();
    const auto taken = [&dir, &suffix](const QString &candidateBase) {
        // The .md sibling counts: see uniquePathForRecording()'s declaration.
        return QFileInfo::exists(dir + QLatin1Char('/') + candidateBase + QLatin1Char('.') + suffix)
            || QFileInfo::exists(dir + QLatin1Char('/') + candidateBase + QStringLiteral(".md"));
    };
    if (!taken(baseName))
        return desiredPath;
    for (int n = 2;; ++n) {
        const QString candidate = QStringLiteral("%1_%2").arg(baseName).arg(n);
        if (!taken(candidate))
            return dir + QLatin1Char('/') + candidate + QLatin1Char('.') + suffix;
    }
}

int pruneTestRecordings(const QString &dir, int keepNewest)
{
    QDir directory(dir);
    if (!directory.exists())
        return 0;
    QStringList names = directory.entryList({ QStringLiteral("suopAudio_*.wav") }, QDir::Files, QDir::Name);
    int removed = 0;
    for (int i = 0; i < names.size() - std::max(0, keepNewest); ++i) {
        if (directory.remove(names.at(i)))
            ++removed;
    }
    return removed;
}

QVariantList parseSystemSources(const QByteArray &pwDumpJson, const std::function<QStringList(qint64)> &ancestryFor)
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

            QStringList ancestry;
            if (ancestryFor) {
                const qint64 pid = props.value(QStringLiteral("application.process.id")).toVariant().toLongLong();
                if (pid > 0)
                    ancestry = ancestryFor(pid);
            }
            const QString name = streamDisplayName(props.toVariantMap(), ancestry);
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

bool isGenericProcessName(const QString &name)
{
    // Chromium's own binary names plus the Electron shell. Kept deliberately
    // short: anything listed here can never be shown as a label, so a false
    // entry silently hides a real app's name.
    static const QStringList kGeneric = {
        QStringLiteral("chromium"),      QStringLiteral("chromium-browser"), QStringLiteral("chrome"),
        QStringLiteral("google-chrome"), QStringLiteral("google-chrome-stable"),
        QStringLiteral("electron"),      QStringLiteral("chrome_crashpad_handler"),
        // Launchers and runtimes that own the process but never name the app:
        // "exe" is what an AppImage/Electron re-exec through /proc/self/exe
        // leaves in argv[0], and node/python are wrappers a real app name sits
        // above in the ancestry.
        QStringLiteral("exe"),           QStringLiteral("node"), QStringLiteral("python"),
        QStringLiteral("python3"),       QStringLiteral("sh"),   QStringLiteral("bash"),
    };
    return kGeneric.contains(name.trimmed().toLower());
}

QStringList processAncestryNames(qint64 pid, int maxDepth)
{
    QStringList names;
    qint64 current = pid;
    for (int hop = 0; hop < maxDepth && current > 1; ++hop) {
        // /proc/<pid>/status reports size 0, so readLine()/atEnd() loops read
        // nothing at all — it must be slurped whole (suopPanel's mixer hit
        // exactly this).
        QFile statusFile(QStringLiteral("/proc/%1/status").arg(current));
        if (!statusFile.open(QIODevice::ReadOnly))
            break;
        const QString status = QString::fromUtf8(statusFile.readAll());
        statusFile.close();

        // status' Name: (the kernel's comm) is the RELIABLE one, measured
        // against real streams on this machine: Chromium rewrites its own
        // argv into a process title, so a Brave helper's argv[0] came back as
        // an 813-character command line, and an Electron app that re-execs
        // through /proc/self/exe reports argv[0] "exe". comm survives both —
        // those apps rewrite the title, not comm. Its one flaw is the
        // kernel's 15-char truncation, which argv[0] repairs below.
        const QStringList statusLines = status.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        QString name;
        for (const QString &line : statusLines) {
            if (line.startsWith(QLatin1String("Name:"))) {
                name = line.mid(5).trimmed();
                break;
            }
        }

        QFile cmdlineFile(QStringLiteral("/proc/%1/cmdline").arg(current));
        if (cmdlineFile.open(QIODevice::ReadOnly)) {
            const QByteArray cmdline = cmdlineFile.readAll();
            const QString base = QFileInfo(QString::fromUtf8(cmdline.left(cmdline.indexOf('\0')))).fileName();
            // Only ever UN-TRUNCATES comm, never overrides it: a rewritten
            // title fails the prefix test, and so does "exe".
            const bool clean = !base.isEmpty() && !base.contains(QLatin1Char(' ')) && !base.startsWith(QLatin1Char('-'));
            if (clean && (name.isEmpty() || (base.size() > name.size() && base.startsWith(name))))
                name = base;
        }
        if (!name.isEmpty())
            names.append(name);

        qint64 parent = 0;
        for (const QString &line : statusLines) {
            if (line.startsWith(QLatin1String("PPid:"))) {
                parent = line.mid(5).trimmed().toLongLong();
                break;
            }
        }
        if (parent <= 1 || parent == current)
            break; // reached init, or a bogus self-parent that would loop forever.
        current = parent;
    }
    return names;
}

QString streamDisplayName(const QVariantMap &props, const QStringList &ancestryNames)
{
    const auto titleCase = [](const QString &raw) {
        return raw.isEmpty() ? raw : raw.left(1).toUpper() + raw.mid(1);
    };

    const QString appName = props.value(QStringLiteral("application.name")).toString().trimmed();
    if (!appName.isEmpty() && !isGenericProcessName(appName))
        return appName;

    for (const QString &ancestor : ancestryNames) {
        if (!ancestor.isEmpty() && !isGenericProcessName(ancestor))
            return titleCase(ancestor);
    }

    const QString binary = QFileInfo(props.value(QStringLiteral("application.process.binary")).toString()).fileName();
    if (!binary.isEmpty() && !isGenericProcessName(binary))
        return titleCase(binary);

    const QString mediaName = props.value(QStringLiteral("media.name")).toString().trimmed();
    if (!mediaName.isEmpty())
        return mediaName;

    // A real Chromium browser genuinely IS "Chromium" — falling all the way
    // through here means no ancestor or binary disagreed, so the generic name
    // is the honest one.
    return appName;
}

}

// Lives on AudioRecorder's dedicated QThread; never touches the GUI thread.
// Owns the QAudioSource (mic, pull-mode via the QIODevice* start() returns),
// one `parec`/`pw-record` QProcess per system/app source (see
// AudioDecoder.cpp for this codebase's QProcess shell-out conventions this
// mirrors), and a periodic mixing QTimer that pops a frame-aligned chunk from
// every non-stalled source's byte-queue (see AudioRecorderIO::planMixTick())
// and mixes them per AudioRecorderIO::mixPcm16N, appending the result to the
// in-progress WAV file. N is however many of mic / system-audio / per-app
// streams the caller requested — there is no separate single-source code
// path, since mixPcm16N over one buffer already is a gain-scale.
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
                double micGain, double systemGain, const QString &systemSourceId = QString(),
                const QVariantList &appCaptureSources = {})
    {
        if (m_running) {
            emit started(false, QStringLiteral("already recording"));
            return;
        }
        if (!useMic && !useSystemAudio && appCaptureSources.isEmpty()) {
            emit started(false, QStringLiteral("no audio source selected"));
            return;
        }

        m_useMic = useMic;
        m_useSystemAudio = useSystemAudio;
        m_systemSourceId = systemSourceId;
        m_micDeviceId = micDeviceId;
        m_micGain = micGain;
        m_systemGain = systemGain;
        m_sources.clear();
        m_bytesWritten = 0;
        // An EMPTY outputPath is monitor mode: identical capture graph and
        // identical per-source level ticks, but nothing is written and no file
        // is created. It is what makes the recorder's meters live while the
        // page merely sits open, so gains can be set before arming a take.
        m_writing = !outputPath.isEmpty();

        if (m_writing) {
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
        }

        if (useMic && !startMic(micDeviceId, micGain)) {
            abortStart();
            return; // startMic() already emitted started(false, ...)
        }

        if (useSystemAudio) {
            auto source = std::make_unique<CaptureSource>();
            source->id = systemSourceId;
            source->kind = QStringLiteral("system");
            source->gain = systemGain;
            if (!startProcessSource(*source, /*fatal=*/true)) {
                abortStart();
                return; // startProcessSource() already emitted started(false, ...)
            }
            m_sources.push_back(std::move(source));
        }

        for (const QVariant &entryVariant : appCaptureSources) {
            const QVariantMap entry = entryVariant.toMap();
            const QString id = entry.value(QStringLiteral("id")).toString();
            if (id.isEmpty() || id == m_systemSourceId)
                continue; // already captured via the system-audio source above — no double-capture.
            auto source = std::make_unique<CaptureSource>();
            source->id = id;
            source->gain = entry.value(QStringLiteral("gain"), 1.0).toDouble();
            if (!startProcessSource(*source, /*fatal=*/true)) {
                abortStart();
                return;
            }
            m_sources.push_back(std::move(source));
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

        emit activeSourcesChanged(currentSourceIds());
        emit started(true, QString());
    }

    // Adds one application stream to a recording ALREADY in progress — see
    // AudioRecorder::addAppCaptureSource()'s doc comment for the user-facing
    // contract. Not routed through start()'s own source-building loop: that
    // path is fatal-on-failure (abortStart() unwinds the whole take), which
    // is wrong here — the take is already running on its other sources.
    void addAppSource(const QString &id, double gain)
    {
        if (!m_running || id.isEmpty())
            return;
        if (m_useSystemAudio)
            return; // a monitor tap already carries this app — see startRecording()'s doc comment.
        for (const auto &source : m_sources) {
            if (source->id == id) {
                // Already capturing it — a second tap would comb-filter against
                // the first. Re-trim it instead, so this doubles as the live
                // gain update for a source already in the mix.
                source->gain = gain;
                return;
            }
        }

        auto source = std::make_unique<CaptureSource>();
        source->id = id;
        source->kind = QStringLiteral("app");
        source->gain = gain;
        // Starts STALLED, and this is load-bearing. planMixTick() pops
        // min() over the non-stalled sources, so a brand-new source sitting
        // at 0 queued bytes would drag that min to zero and halt the whole
        // mix until either it delivers or the other queues back up past the
        // half-second stall threshold. Entering the sticky-stalled state up
        // front means it is mixed as silence and skipped by the min until
        // its first bytes land, at which point planMixTick un-stalls it on
        // its own.
        source->stalled = true;
        if (!startProcessSource(*source, /*fatal=*/false)) {
            // Unlike start()'s call, a failure here must NOT unwind the take:
            // the existing sources are recording fine and an app that could
            // not be tapped is not a reason to lose them. startProcessSource()
            // reports through recordingError() (fatal=false), which
            // AudioRecorder maps onto lastError without touching m_recording.
            return;
        }
        m_sources.push_back(std::move(source));
        emit activeSourcesChanged(currentSourceIds());
    }

    // Turns the microphone on or off WITHOUT interrupting the capture already
    // in flight — the other sources keep streaming into the same file. Failure
    // to open the device is non-fatal for the same reason addAppSource()'s is:
    // a take running on system audio is not worth losing because a mic went
    // missing.
    void setMicEnabled(bool enabled, const QString &micDeviceId, double gain)
    {
        if (!m_running)
            return;
        m_micDeviceId = micDeviceId;
        m_micGain = gain;
        if (enabled == (m_micCaptureSource != nullptr)) {
            if (enabled && m_micCaptureSource)
                m_micCaptureSource->gain = gain; // already on; just re-trim it.
            return;
        }
        if (enabled) {
            m_useMic = true;
            if (!startMic(micDeviceId, gain))
                return; // startMic() already reported through started(false, ...)
        } else {
            m_useMic = false;
            stopMic();
        }
        emit activeSourcesChanged(currentSourceIds());
    }

    // The system-audio counterpart. Adding mid-take is the mirror of
    // addAppSource(); removing it is the only way to hand the per-app rows
    // back their exclusivity without ending the take.
    void setSystemEnabled(bool enabled, const QString &sourceId, double gain)
    {
        if (!m_running)
            return;
        m_systemGain = gain;
        CaptureSource *existing = sourceOfKind(QStringLiteral("system"));
        if (enabled && existing) {
            if (existing->id == sourceId) {
                existing->gain = gain;
                return;
            }
            eraseSource(existing); // switched to a different sink; re-tap it.
            existing = nullptr;
        }
        if (!enabled) {
            m_useSystemAudio = false;
            m_systemSourceId = QString();
            if (existing)
                eraseSource(existing);
            emit activeSourcesChanged(currentSourceIds());
            return;
        }

        m_useSystemAudio = true;
        m_systemSourceId = sourceId;
        auto source = std::make_unique<CaptureSource>();
        source->id = sourceId;
        source->kind = QStringLiteral("system");
        source->gain = gain;
        source->stalled = true; // see addAppSource() for why a new source starts stalled.
        if (!startProcessSource(*source, /*fatal=*/false))
            return;
        m_sources.push_back(std::move(source));
        emit activeSourcesChanged(currentSourceIds());
    }

    // Disarming one application's row mid-capture. The inverse of addAppSource().
    void removeAppSource(const QString &id)
    {
        if (!m_running || id.isEmpty())
            return;
        for (const auto &source : m_sources) {
            if (source->kind == QStringLiteral("app") && source->id == id) {
                eraseSource(source.get());
                emit activeSourcesChanged(currentSourceIds());
                return;
            }
        }
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
        if (m_micDevice && m_micCaptureSource)
            m_micCaptureSource->queue += m_micDevice->readAll();
    }

signals:
    // Result of a start() call — ok=false means nothing is recording and
    // `error` explains why (see AudioRecorder::lastError's doc comment).
    void started(bool ok, const QString &error);
    // A MONITOR run ended (see finalizeAndStop) — never a finished take.
    void monitorStopped();
    // Throttled to ~kLevelUpdateIntervalMs — see mixOnce()'s doc comment.
    // sourceLevels carries one {kind, id, level} map per live capture source,
    // in m_sources order (see AudioRecorder::sourceLevels).
    void levelTick(qreal level, qint64 elapsedMs, const QVariantList &sourceLevels);
    // A capture source was lost — either a fatal mid-recording process error
    // (parec/pw-record died) or the source's own graceful exit (the app
    // closed). Recording continues on whatever sources remain (see
    // dropSource()); finalizeAndStop() (and its own stopped() signal) only
    // follows when NONE remain.
    void recordingError(const QString &error);
    // The WAV file at `filePath` is fully finalized and closed.
    void stopped(const QString &filePath);
    // m_sources changed — see AudioRecorder::activeCaptureSourceIds.
    void activeSourcesChanged(const QStringList &ids);

private:
    // One capture input feeding the mix. The microphone is just source 0 with
    // a null process (it arrives via m_micSource's QIODevice instead); every
    // other source is a `parec`/`pw-record` child — see
    // AudioRecorderIO::captureCommand().
    struct CaptureSource {
        QString id; // "" for the microphone
        // "mic" / "system" / "app" — carried purely so the per-source level
        // tick can be keyed unambiguously: a system source id and an app
        // source id come out of the same enumeration and the mic's id is
        // empty, so id alone is not a key.
        QString kind = QStringLiteral("app");
        double gain = 1.0;
        QByteArray queue;
        std::unique_ptr<QProcess> process;
        bool stalled = false; // last planMixTick() verdict (sticky while empty)
    };

    static constexpr quint32 kSampleRate = 48000;
    static constexpr quint16 kChannelCount = 2;
    static constexpr quint16 kBitsPerSample = 16;
    static constexpr int kMixTimerIntervalMs = 25;
    static constexpr int kLevelUpdateIntervalMs = 75;
    // How often the WAV header is re-stamped with the size captured so far —
    // and, because patchWavHeader() flushes, how often the payload itself
    // reaches the kernel. This is the crash-durability granularity, and it is
    // the ONLY thing standing between a hard kill and a lost take: nothing in
    // this app handles SIGINT/SIGTERM, so Ctrl+C in the launching terminal
    // never reaches finalizeAndStop(). Four checkpoints a second costs four
    // 44-byte rewrites and four flushes against a 192 KB/s payload — cheap
    // enough that the second it used to be was paying for nothing.
    static constexpr int kHeaderCheckpointIntervalMs = 250;
    // Bytes per one L+R sample pair — every chunk popped off a queue is
    // trimmed to a multiple of this so a mix never splits a sample across
    // two ticks.
    static constexpr int kFrameSize = (kChannelCount * kBitsPerSample) / 8;
    // A source lagging by more than half a second is treated as stalled
    // rather than being allowed to hold up every other source — see
    // AudioRecorderIO::planMixTick().
    static constexpr int kStallThresholdBytes = kSampleRate * kFrameSize / 2;

    // Unwinds a start() call that failed partway through: whatever sources
    // already started are torn down (a process source is killed explicitly —
    // relying on ~QProcess alone to reap an already-running child, rather
    // than one that merely failed to launch, isn't something to depend on)
    // and the just-created output file is discarded, so a failed start leaves
    // no half-open state behind. Safe to call before m_running is set (the
    // only time start() ever calls this): onSourceProcessGone()'s guard means
    // the kill below can't re-enter dropSource().
    // Shared by setMicEnabled(false) and the teardown paths — stopping the
    // QAudioSource and dropping its CaptureSource must happen together, or
    // m_micCaptureSource dangles into a freed vector element.
    void stopMic()
    {
        if (m_micDevice && m_micCaptureSource)
            m_micCaptureSource->queue += m_micDevice->readAll(); // don't lose the tail.
        if (m_micSource) {
            m_micSource->stop();
            m_micSource.reset();
        }
        m_micDevice = nullptr;
        if (m_micCaptureSource) {
            CaptureSource *mic = m_micCaptureSource;
            m_micCaptureSource = nullptr;
            eraseSource(mic);
        }
    }

    CaptureSource *sourceOfKind(const QString &kind) const
    {
        for (const auto &source : m_sources) {
            if (source->kind == kind)
                return source.get();
        }
        return nullptr;
    }

    // Stops a source's process (if any) and removes it. Unlike dropSource()
    // this is caller-initiated, so it reports nothing and never ends the take
    // — disarming your last source leaves the capture running with none, and
    // the next arm re-adds one.
    void eraseSource(CaptureSource *source)
    {
        if (!source)
            return;
        if (source->process) {
            source->process->kill();
            source->process->waitForFinished(1000);
        }
        if (source == m_micCaptureSource)
            m_micCaptureSource = nullptr;
        const auto it = std::find_if(m_sources.begin(), m_sources.end(),
                                      [source](const std::unique_ptr<CaptureSource> &s) { return s.get() == source; });
        if (it != m_sources.end())
            m_sources.erase(it);
    }

    void abortStart()
    {
        if (m_micSource) {
            m_micSource->stop();
            m_micSource.reset();
            m_micDevice = nullptr;
            m_micCaptureSource = nullptr;
        }
        for (const auto &source : m_sources) {
            if (source->process) {
                source->process->kill();
                source->process->waitForFinished(1000);
            }
        }
        m_sources.clear();
        if (m_writing) {
            m_outputFile.close();
            m_outputFile.remove();
        }
    }

    // Stream ids of every live capture source, in m_sources order (the mic's
    // empty id is skipped) — see AudioRecorder::activeCaptureSourceIds.
    QStringList currentSourceIds() const
    {
        QStringList ids;
        for (const auto &source : m_sources) {
            if (!source->id.isEmpty())
                ids.append(source->id);
        }
        return ids;
    }

    bool startMic(const QString &micDeviceId, double gain)
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

        auto source = std::make_unique<CaptureSource>();
        source->kind = QStringLiteral("mic");
        source->gain = gain;
        m_micCaptureSource = source.get();
        m_sources.push_back(std::move(source));

        connect(m_micDevice, &QIODevice::readyRead, this, &AudioRecorderWorker::onMicReadyRead);
        return true;
    }

    // Starts one process-backed capture source (system audio or a single app
    // stream — see AudioRecorderIO::captureCommand() for the id→command
    // mapping). `fatal` is true from start() (a failure to START unwinds the
    // whole take via abortStart(), reported through started(false, ...)) and
    // false from addAppSource() (a late-join failure must not tear down a
    // take that is otherwise running fine — reported through
    // recordingError()/lastError instead, which never touches m_recording).
    // A source dying AFTER a successful start is not fatal on its own either
    // way — see dropSource().
    bool startProcessSource(CaptureSource &source, bool fatal)
    {
        const QStringList cmd = AudioRecorderIO::captureCommand(source.id);
        source.process = std::make_unique<QProcess>();
        CaptureSource *sourcePtr = &source;
        connect(source.process.get(), &QProcess::readyReadStandardOutput, this,
                [sourcePtr] { sourcePtr->queue += sourcePtr->process->readAllStandardOutput(); });
        connect(source.process.get(), &QProcess::errorOccurred, this,
                [this, sourcePtr](QProcess::ProcessError) { onSourceProcessGone(sourcePtr, /*graceful=*/false); });
        connect(source.process.get(), &QProcess::finished, this,
                [this, sourcePtr](int, QProcess::ExitStatus status) {
                    onSourceProcessGone(sourcePtr, status == QProcess::NormalExit);
                });
        source.process->start(cmd.constFirst(), cmd.mid(1));
        if (!source.process->waitForStarted(3000)) {
            const QString error =
                QStringLiteral("failed to start %1 (is PulseAudio/PipeWire-pulse installed and running?)")
                    .arg(cmd.constFirst());
            if (fatal)
                emit started(false, error);
            else
                emit recordingError(error);
            source.process.reset();
            return false;
        }
        return true;
    }

    // A process source exited or errored, either mid-recording (drop it and
    // keep going — see dropSource()) or synchronously out of
    // startProcessSource()'s own start()/waitForStarted() call, which handles
    // its own failure inline; m_running is only ever true once every source
    // in this start() call has already succeeded, so that guard tells the two
    // cases apart without extra state. Also fires (and no-ops via the same
    // guard) while finalizeAndStop() kills every remaining process — that
    // teardown is already what erases m_sources.
    void onSourceProcessGone(CaptureSource *source, bool graceful)
    {
        if (!m_running)
            return;
        dropSource(source, graceful);
    }

    // Drops one capture source mid-recording without ending the take —
    // draining what it already buffered, reporting its loss, and only
    // finalizing the whole recording once no source is left at all.
    // `graceful` (a NormalExit QProcess::finished, as opposed to an
    // errorOccurred) distinguishes an application closing its own stream —
    // ordinary now that sources can join and leave during a take (see
    // addAppSource()) — from an actual fault; banner-ing every departure
    // would train the user to ignore the banner.
    void dropSource(CaptureSource *source, bool graceful)
    {
        if (source->process)
            source->queue += source->process->readAllStandardOutput();
        const QString name = source->id.isEmpty() ? QStringLiteral("system audio") : source->id;
        // A crash, or losing the LAST source (which ends the take), still
        // reports through the banner. This test runs BEFORE the erase below —
        // `source` is still in m_sources here.
        // Monitoring is best-effort and unattended — a source going away while
        // nothing is being kept is not something to alarm the user about; it
        // just stops metering. Only a real take reports it.
        if (m_writing && (!graceful || m_sources.size() <= 1))
            emit recordingError(QStringLiteral("capture source lost: %1").arg(name));
        else
            qWarning() << "recorder: capture source ended:" << name;

        const auto it = std::find_if(m_sources.begin(), m_sources.end(),
                                      [source](const std::unique_ptr<CaptureSource> &s) { return s.get() == source; });
        if (it != m_sources.end())
            m_sources.erase(it);
        emit activeSourcesChanged(currentSourceIds());

        if (m_sources.empty())
            finalizeAndStop();
    }

    // Pops the next mixable chunk per AudioRecorderIO::planMixTick() (a
    // stalled source is mixed as silence and nothing is popped from it — see
    // its doc comment for why a naive min() over all queues would freeze the
    // whole recording), mixes it via mixPcm16N, and appends it to the output
    // file — used both by the periodic mixing timer (while m_running) and by
    // finalizeAndStop()'s final drain loop (after m_running has already been
    // cleared, so it bypasses that guard directly). Returns true if it wrote
    // anything, so finalizeAndStop() can loop until every queue is drained
    // below one frame.
    bool mixOnce()
    {
        QList<int> queued;
        QList<bool> wasStalled;
        for (const auto &source : m_sources) {
            queued.append(source->queue.size());
            wasStalled.append(source->stalled);
        }
        const AudioRecorderIO::MixTickPlan plan =
            AudioRecorderIO::planMixTick(queued, wasStalled, kStallThresholdBytes, kFrameSize);
        for (int i = 0; i < static_cast<int>(m_sources.size()); ++i)
            m_sources[i]->stalled = plan.stalled.value(i);
        if (plan.bytes <= 0)
            return false;

        QList<QByteArray> buffers;
        QList<double> gains;
        for (const auto &source : m_sources) {
            gains.append(source->gain);
            if (source->stalled) {
                buffers.append(QByteArray()); // mixed as silence; nothing popped.
                continue;
            }
            buffers.append(source->queue.left(plan.bytes));
            source->queue.remove(0, plan.bytes);
        }

        // Post-gain RMS per source, taken from the SAME chunk that is about
        // to be mixed. RMS is linear in a scalar gain, so this needs no
        // second pass over the samples: rms(x * g) == rms(x) * g. The
        // clamp mirrors mixPcm16N's own clipping — a source driven past full
        // scale reads as full scale, not as 1.4.
        QVariantList levels;
        for (int i = 0; i < static_cast<int>(m_sources.size()); ++i) {
            QVariantMap entry;
            entry[QStringLiteral("kind")] = m_sources[i]->kind;
            entry[QStringLiteral("id")] = m_sources[i]->id;
            entry[QStringLiteral("level")] =
                std::min(qreal(1.0), AudioRecorderIO::rmsPcm16(buffers.at(i)) * m_sources[i]->gain);
            levels.append(entry);
        }

        const QByteArray mixed = AudioRecorderIO::mixPcm16N(buffers, gains);

        if (mixed.isEmpty())
            return false;

        // Monitor mode mixes and measures exactly as a take does, but keeps
        // nothing — see start()'s empty-outputPath contract.
        if (m_writing) {
            m_outputFile.write(mixed);
            m_bytesWritten += static_cast<quint32>(mixed.size());
        }

        // Crash durability. The header written at start() claims dataSize 0,
        // so until it is stamped the file reads as an empty WAV to every
        // player even though the PCM is physically on disk — and QFile
        // buffers, so an unflushed tail is lost outright on a crash.
        // Re-stamping here (patchWavHeader also flushes, and restores the
        // write position) means an abnormal exit leaves a valid recording up
        // to the last checkpoint rather than an unreadable file.
        // This block must stay AFTER the m_outputFile.write(mixed) above:
        // patchWavHeader()'s flush is what pushes that payload out of QFile's
        // buffer, so stamping a size first and writing the bytes second would
        // leave a header describing audio that is not on disk yet.
        if (m_writing
            && (!m_sinceLastCheckpoint.isValid()
                || m_sinceLastCheckpoint.elapsed() >= kHeaderCheckpointIntervalMs)) {
            m_sinceLastCheckpoint.restart();
            AudioRecorderIO::patchWavHeader(m_outputFile, m_bytesWritten, kSampleRate, kChannelCount, kBitsPerSample);
        }

        // Level meter / elapsed time throttling (same shape as
        // TranscriptionController's liveUpdateTick): computed every
        // mixing tick (kMixTimerIntervalMs, ~20-30ms) but only pushed to the
        // GUI-thread-visible property at ~kLevelUpdateIntervalMs.
        if (!m_sinceLastLevelTick.isValid() || m_sinceLastLevelTick.elapsed() >= kLevelUpdateIntervalMs) {
            m_sinceLastLevelTick.restart();
            emit levelTick(AudioRecorderIO::rmsPcm16(mixed), m_elapsed.elapsed(), levels);
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
        // final few ms of audio aren't lost to a device/process that hadn't
        // fired readyRead yet at the exact moment stop() was requested.
        if (m_micDevice && m_micCaptureSource)
            m_micCaptureSource->queue += m_micDevice->readAll();
        for (const auto &source : m_sources) {
            if (source->process)
                source->queue += source->process->readAllStandardOutput();
        }
        while (mixOnce()) { }

        if (m_micSource) {
            m_micSource->stop();
            m_micSource.reset();
            m_micDevice = nullptr;
            m_micCaptureSource = nullptr;
        }
        // m_running is already false here, so any finished()/errorOccurred()
        // this kill triggers no-ops via onSourceProcessGone()'s guard instead
        // of re-entering dropSource() mid-iteration.
        for (const auto &source : m_sources) {
            if (source->process) {
                source->process->kill();
                source->process->waitForFinished(1000);
            }
        }
        m_sources.clear();
        emit activeSourcesChanged(currentSourceIds()); // settles empty.

        if (!m_writing) {
            // Monitor mode kept nothing, so there is no file to stamp or
            // discard and no path to hand back. Deliberately NOT stopped():
            // startRecording() stops monitoring and starts a take in the same
            // breath, so a monitor teardown reported as a finished recording
            // arrives just after `recording` was set optimistically and clears
            // it — leaving the UI idle while the worker really is capturing,
            // and the next Record press rejected as "already recording".
            emit monitorStopped();
            return;
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
    CaptureSource *m_micCaptureSource = nullptr; // owned by m_sources, not us
    QTimer *m_mixTimer = nullptr;

    // unique_ptr elements (not value CaptureSource) so a vector reallocation
    // never invalidates m_micCaptureSource or the readyRead/errorOccurred
    // lambdas' captured CaptureSource* — both point at the heap object, not
    // its slot in this vector.
    std::vector<std::unique_ptr<CaptureSource>> m_sources;

    QFile m_outputFile;
    // False in monitor mode — see start()'s empty-outputPath contract. Gates
    // every write, the header checkpoint, and the file's creation/removal.
    bool m_writing = false;
    bool m_useMic = false;
    bool m_useSystemAudio = false;
    QString m_systemSourceId;
    // Remembered so a mid-capture mic toggle can re-open the same device at the
    // same trim without the caller having to re-supply them.
    QString m_micDeviceId;
    double m_micGain = 1.0;
    double m_systemGain = 1.0;
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
        clearActiveRecordingPath();
        m_lastError = error;
        emit lastErrorChanged();
        emit recordingChanged();
    });
    connect(m_worker, &AudioRecorderWorker::levelTick, this,
            [this](qreal level, qint64 elapsedMs, const QVariantList &sourceLevels) {
                m_currentLevel = level;
                m_elapsedMs = elapsedMs;
                m_sourceLevels = sourceLevels;
                emit levelUpdated();
            });
    connect(m_worker, &AudioRecorderWorker::recordingError, this, [this](const QString &error) {
        m_lastError = error;
        emit lastErrorChanged();
    });
    connect(m_worker, &AudioRecorderWorker::activeSourcesChanged, this, [this](const QStringList &ids) {
        m_activeCaptureSourceIds = ids;
        emit activeCaptureSourcesChanged();
    });
    connect(m_worker, &AudioRecorderWorker::stopped, this, [this](const QString &filePath) {
        m_recording = false;
        m_currentLevel = 0.0;
        m_sourceLevels.clear();
        // Released HERE, not in stopRecording(), and that asymmetry is the
        // point: stopRecording() flips `recording` on the click, while the
        // worker is still patching the WAV header and flushing. Freeing the
        // path any earlier would hand the file back to the vault UI while it
        // is still being written.
        clearActiveRecordingPath();
        emit recordingChanged();
        emit levelUpdated();
        emit recordingFinished(filePath);
    });

    // Monitoring ending must not touch `recording`: it is routinely torn down
    // one queued call BEFORE a take starts, so clearing the flag here would
    // undo startRecording()'s optimistic set.
    connect(m_worker, &AudioRecorderWorker::monitorStopped, this, [this] {
        m_currentLevel = 0.0;
        m_sourceLevels.clear();
        emit levelUpdated();
    });

    m_thread.start();
    refreshSources(); // warm the source lists so the recorder page opens populated.
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

void AudioRecorder::refreshSources()
{
    if (m_sourceProbe)
        return; // a dump is already in flight; polls must not stack up behind a slow one.

    m_sourceProbe = new QProcess(this);
    QProcess *probe = m_sourceProbe;
    // errorOccurred (pw-dump not installed) and finished can both fire for one
    // run, so whichever arrives first clears m_sourceProbe and the other sees
    // a null and bails.
    const auto settle = [this, probe](const QByteArray &output) {
        if (m_sourceProbe != probe)
            return;
        m_sourceProbe = nullptr;
        probe->deleteLater();
        applySources(AudioRecorderIO::parseSystemSources(output, [](qint64 pid) {
            return AudioRecorderIO::processAncestryNames(pid);
        }));
    };
    connect(probe, &QProcess::finished, this, [probe, settle](int, QProcess::ExitStatus status) {
        // A failed/killed dump still settles — with empty output, which
        // parseSystemSources() degrades to the synthetic default entry alone
        // (plain PulseAudio host, or no PipeWire server running).
        settle(status == QProcess::NormalExit ? probe->readAllStandardOutput() : QByteArray());
    });
    connect(probe, &QProcess::errorOccurred, this, [settle](QProcess::ProcessError) { settle(QByteArray()); });
    // A dump that never returns would otherwise wedge every later refresh on
    // the in-flight guard above.
    QTimer::singleShot(3000, probe, [probe] {
        if (probe->state() != QProcess::NotRunning)
            probe->kill();
    });
    probe->start(QStringLiteral("pw-dump"), {});
}

void AudioRecorder::clearActiveRecordingPath()
{
    if (m_activeRecordingPath.isEmpty())
        return;
    m_activeRecordingPath.clear();
    emit activeRecordingPathChanged();
}

void AudioRecorder::applySources(const QVariantList &sources)
{
    const QVariantList apps = AudioRecorderIO::filterAppStreams(sources);
    if (sources == m_systemSources && apps == m_appStreams)
        return; // same graph as last poll — see the systemSources property.
    m_systemSources = sources;
    m_appStreams = apps;
    emit sourcesChanged();
}

QString AudioRecorder::plannedRecordingFileName(const QUrl &vaultUrl, const QString &outputRelativeFolder,
                                                 const QString &requestedName, bool testMode) const
{
    const QString desired = testMode
        ? AudioRecorderIO::buildTestOutputFilePath(QDir::tempPath(), QDateTime::currentDateTime(), requestedName)
        : AudioRecorderIO::buildOutputFilePath(vaultUrl, outputRelativeFolder, QDateTime::currentDateTime(),
                                                requestedName);
    return QFileInfo(AudioRecorderIO::uniquePathForRecording(desired)).fileName();
}

// How many past test takes survive the next one. Enough to A/B a gain change
// against the take before it; not a growing pile of RAM on tmpfs.
static constexpr int kKeepTestRecordings = 3;

void AudioRecorder::startRecording(const QUrl &vaultUrl, const QString &outputRelativeFolder,
                                    const QString &requestedName, bool useMic,
                                    const QString &micDeviceId, bool useSystemAudio, double micGain,
                                    double systemGain, const QString &systemSourceId,
                                    const QVariantList &appCaptureSources, bool testMode)
{
    if (m_recording) {
        m_lastError = QStringLiteral("already recording");
        emit lastErrorChanged();
        return;
    }
    QString outputPath;
    if (testMode) {
        // Disposable by definition, and on tmpfs an unpruned pile of them is
        // RAM held for nothing. Bounded here, at the one place a test take is
        // created, rather than by a cleanup pass somebody has to remember to
        // run.
        AudioRecorderIO::pruneTestRecordings(QDir::tempPath() + QStringLiteral("/suopTranscriber-test"),
                                             kKeepTestRecordings);
        outputPath = AudioRecorderIO::buildTestOutputFilePath(QDir::tempPath(), QDateTime::currentDateTime(),
                                                              requestedName);
    } else {
        outputPath = AudioRecorderIO::buildOutputFilePath(vaultUrl, outputRelativeFolder,
                                                          QDateTime::currentDateTime(), requestedName);
    }
    // Resolved here rather than in the builders so they stay pure joins, and
    // resolved on the GUI thread because it is two stat calls, not real work.
    // The page previews the same decision via plannedRecordingFileName(); this
    // is the authoritative one, since the folder can change under it.
    outputPath = AudioRecorderIO::uniquePathForRecording(outputPath);
    // Published BEFORE the worker is told to start: the file appears in the
    // vault tree the instant the worker opens it, and the tree's own watcher
    // can surface it before any signal makes it back across the thread
    // boundary. Anything gating on this path has to be armed first or the row
    // is briefly live.
    m_activeRecordingPath = outputPath;
    emit activeRecordingPathChanged();
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
    // A take supersedes monitoring. Both are queued to the same worker, and
    // queued calls run in order, so posting the stop first guarantees the
    // worker is idle by the time start() arrives — without it the worker's own
    // "already recording" guard would reject the take outright.
    if (m_monitoring) {
        m_monitoring = false;
        emit monitoringChanged();
        QMetaObject::invokeMethod(m_worker, "stop", Qt::QueuedConnection);
    }
    QMetaObject::invokeMethod(m_worker, "start", Qt::QueuedConnection, Q_ARG(QString, outputPath),
                               Q_ARG(bool, useMic), Q_ARG(QString, micDeviceId), Q_ARG(bool, useSystemAudio),
                               Q_ARG(double, micGain), Q_ARG(double, systemGain), Q_ARG(QString, systemSourceId),
                               Q_ARG(QVariantList, appCaptureSources));
}

void AudioRecorder::startMonitoring(bool useMic, const QString &micDeviceId, bool useSystemAudio,
                                     double micGain, double systemGain, const QString &systemSourceId,
                                     const QVariantList &appCaptureSources)
{
    // A take always wins: never monitor over a recording, and never let a
    // monitor request restart one that is already running.
    if (m_recording || m_monitoring)
        return;
    if (!useMic && !useSystemAudio && appCaptureSources.isEmpty())
        return; // nothing armed — no reason to hold any device open.
    m_monitoring = true;
    emit monitoringChanged();
    // Empty outputPath == monitor mode; see AudioRecorderWorker::start().
    QMetaObject::invokeMethod(m_worker, "start", Qt::QueuedConnection, Q_ARG(QString, QString()),
                               Q_ARG(bool, useMic), Q_ARG(QString, micDeviceId), Q_ARG(bool, useSystemAudio),
                               Q_ARG(double, micGain), Q_ARG(double, systemGain), Q_ARG(QString, systemSourceId),
                               Q_ARG(QVariantList, appCaptureSources));
}

void AudioRecorder::stopMonitoring()
{
    if (!m_monitoring)
        return;
    m_monitoring = false;
    emit monitoringChanged();
    QMetaObject::invokeMethod(m_worker, "stop", Qt::QueuedConnection);
}

void AudioRecorder::setMicEnabled(bool enabled, const QString &micDeviceId, double gain)
{
    if (!m_recording && !m_monitoring)
        return;
    QMetaObject::invokeMethod(m_worker, "setMicEnabled", Qt::QueuedConnection, Q_ARG(bool, enabled),
                               Q_ARG(QString, micDeviceId), Q_ARG(double, gain));
}

void AudioRecorder::setSystemAudioEnabled(bool enabled, const QString &sourceId, double gain)
{
    if (!m_recording && !m_monitoring)
        return;
    QMetaObject::invokeMethod(m_worker, "setSystemEnabled", Qt::QueuedConnection, Q_ARG(bool, enabled),
                               Q_ARG(QString, sourceId), Q_ARG(double, gain));
}

void AudioRecorder::removeAppCaptureSource(const QString &id)
{
    if (!m_recording && !m_monitoring)
        return;
    QMetaObject::invokeMethod(m_worker, "removeAppSource", Qt::QueuedConnection, Q_ARG(QString, id));
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
    // Posted BEFORE the signal, unlike everywhere else: a QML handler runs
    // synchronously inside this emit, and RecorderPage's restarts monitoring
    // there. Worker slots run FIFO, so announcing first would let that
    // monitor `start` overtake this `stop` — the worker would reject it as
    // "already recording", raising a spurious error banner and leaving
    // `monitoring` true with nothing metering. The property is already false
    // above, so any reader in the handler still sees a stopped take.
    QMetaObject::invokeMethod(m_worker, "stop", Qt::QueuedConnection);
    emit recordingChanged();
}

void AudioRecorder::addAppCaptureSource(const QString &id, double gain)
{
    if (!m_recording && !m_monitoring)
        return;
    QMetaObject::invokeMethod(m_worker, "addAppSource", Qt::QueuedConnection, Q_ARG(QString, id),
                               Q_ARG(double, gain));
}

#include "AudioRecorder.moc"
