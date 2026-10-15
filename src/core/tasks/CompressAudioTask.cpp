#include "CompressAudioTask.h"

#include <QDateTime>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QLocale>
#include <QProcess>

#include <algorithm>
#include <cmath>
#include <optional>

#include "../AudioDecoder.h"
#include "../ProcessRunner.h"
#include "../VaultFileOps.h"
#include "RevertCompressionTask.h"

namespace {

constexpr qint64 kMaxErrTextBytes = 64 * 1024;

// Runs one ffmpeg invocation under the pause/cancel/stall watchdog. Every
// call passes `-progress pipe:1`, so stdout ticks roughly twice a second for
// the whole run — silence on it for isDecodeStalled()'s window means wedged.
// onProgressSeconds receives each out_time the encoder reports.
void runFfmpeg(const QStringList &args, TaskContext &ctx, const std::function<void(double)> &onProgressSeconds,
               const QString &cleanupPath)
{
    QProcess ffmpeg;
    ProcessRunner::guardChildLifetime(ffmpeg);
    ffmpeg.start(QStringLiteral("ffmpeg"), args);
    if (!ffmpeg.waitForStarted()) {
        QFile::remove(cleanupPath);
        throw TaskError(QStringLiteral("failed to start ffmpeg"));
    }

    QElapsedTimer quietClock;
    quietClock.start();
    QByteArray errText;
    QByteArray pending;
    const auto drain = [&] {
        const QByteArray out = ffmpeg.readAllStandardOutput();
        if (!out.isEmpty())
            quietClock.restart();
        pending += out;
        qsizetype nl;
        while ((nl = pending.indexOf('\n')) >= 0) {
            const QByteArray line = pending.left(nl).trimmed();
            pending.remove(0, nl + 1);
            if (line.startsWith("out_time_us=") && onProgressSeconds) {
                bool ok = false;
                const qint64 us = line.mid(12).toLongLong(&ok);
                if (ok && us >= 0)
                    onProgressSeconds(static_cast<double>(us) / 1e6);
            }
        }
        const QByteArray err = ffmpeg.readAllStandardError();
        errText += err.left(std::max<qsizetype>(0, kMaxErrTextBytes - errText.size()));
    };
    const auto stalled = [&] {
        if (ctx.pauseRequested()) {
            quietClock.restart();
            return false;
        }
        return AudioDecoder::isDecodeStalled(quietClock.elapsed());
    };

    const auto outcome = ProcessRunner::runPausable(
        ffmpeg,
        {[&ctx] { return ctx.cancelRequested(); }, [&ctx] { return ctx.pauseRequested(); },
         [&ctx](bool paused) { ctx.reportPaused(paused); }, stalled},
        drain);
    drain();

    if (outcome == ProcessRunner::RunOutcome::Cancelled) {
        QFile::remove(cleanupPath);
        throw TaskError(QStringLiteral("Cancelled"));
    }
    if (outcome == ProcessRunner::RunOutcome::Stalled) {
        QFile::remove(cleanupPath);
        throw TaskError(QStringLiteral("ffmpeg stalled — no progress for 60s; the WAV was kept"));
    }
    if (ffmpeg.exitStatus() != QProcess::NormalExit || ffmpeg.exitCode() != 0) {
        QFile::remove(cleanupPath);
        const QString err = QString::fromUtf8(errText).trimmed();
        throw TaskError(err.isEmpty() ? QStringLiteral("ffmpeg failed; the WAV was kept") : err);
    }
}

} // namespace

CompressAudioTask::CompressAudioTask(QUrl audio, QUrl vault, Target target, QObject *parent)
    : Task(parent)
    , m_target(target)
{
    fileUrl = std::move(audio);
    vaultUrl = std::move(vault);
}

std::optional<CompressAudioTask::Target> CompressAudioTask::targetFromString(const QString &target)
{
    if (target == QStringLiteral("voice"))
        return Target::Voice;
    if (target == QStringLiteral("lossless"))
        return Target::Lossless;
    return std::nullopt;
}

bool CompressAudioTask::isCompressible(const QString &path, Target target)
{
    if (path.endsWith(QStringLiteral(".wav"), Qt::CaseInsensitive))
        return true;
    if (target == Target::Lossless || !path.endsWith(QStringLiteral(".flac"), Qt::CaseInsensitive))
        return false;
    // "fLaC", a 4-byte block header, then STREAMINFO (always first): 10 bytes
    // of block/frame sizes, then a 20-bit sample rate and 3-bit channels-1.
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return false;
    const QByteArray h = f.read(21);
    if (h.size() < 21 || !h.startsWith("fLaC") || (h.at(4) & 0x7f) != 0)
        return true; // unreadable header: let the task's own probe decide
    const auto b = [&h](int i) { return static_cast<quint32>(static_cast<quint8>(h.at(i))); };
    const quint32 sampleRate = (b(18) << 12) | (b(19) << 4) | (b(20) >> 4);
    const quint32 channels = ((b(20) >> 1) & 0x7) + 1;
    return !(sampleRate == 16000 && channels == 1);
}

QString CompressAudioTask::destinationPath(const QString &audioPath)
{
    const QFileInfo info(audioPath);
    return info.absolutePath() + QLatin1Char('/') + info.completeBaseName() + QStringLiteral(".flac");
}

QString CompressAudioTask::displayName() const
{
    return QStringLiteral("Compress to %1: %2")
        .arg(m_target == Target::Voice ? QStringLiteral("FLAC voice") : QStringLiteral("lossless FLAC"),
             QFileInfo(fileUrl.toLocalFile()).fileName());
}

namespace {
QString savedText(qint64 bytes)
{
    return QStringLiteral("saved %1")
        .arg(QLocale().formattedDataSize(std::max<qint64>(bytes, 0), 1, QLocale::DataSizeSIFormat));
}
} // namespace

QString CompressAudioTask::undoLabel() const
{
    if (m_trashEntry.isEmpty())
        return {};
    return QStringLiteral("Compressed %1 — %2")
        .arg(QFileInfo(m_compressedUrl.toLocalFile()).fileName(), savedText(m_savedBytes));
}

QString CompressAudioTask::batchUndoLabel(const QList<const Task *> &undoableMembers) const
{
    if (undoableMembers.size() <= 1)
        return undoLabel();
    qint64 saved = 0;
    for (const Task *t : undoableMembers) {
        if (const auto *c = qobject_cast<const CompressAudioTask *>(t))
            saved += c->savedBytes();
    }
    return QStringLiteral("Compressed %1 files — %2").arg(undoableMembers.size()).arg(savedText(saved));
}

std::unique_ptr<Task> CompressAudioTask::makeUndoTask() const
{
    if (m_trashEntry.isEmpty())
        return nullptr;
    return std::make_unique<RevertCompressionTask>(m_compressedUrl, m_trashEntry, vaultUrl);
}

bool CompressAudioTask::undoStillApplicable() const
{
    return QFileInfo::exists(m_compressedUrl.toLocalFile())
        && QFileInfo::exists(m_trashEntry.value(QStringLiteral("trashPath")).toString())
        && (m_compressedUrl == fileUrl || !QFileInfo::exists(fileUrl.toLocalFile()));
}

void CompressAudioTask::run(TaskContext &ctx)
{
    const QString srcPath = fileUrl.toLocalFile();
    if (!QFileInfo::exists(srcPath))
        throw TaskError(QStringLiteral("file not found: %1").arg(srcPath));
    if (!isCompressible(srcPath, m_target)) {
        ctx.reportWarning(m_target == Target::Voice ? QStringLiteral("Already FLAC voice — nothing to compress")
                                                    : QStringLiteral("Not a WAV — nothing to compress"));
        return;
    }
    if (m_contended) {
        ctx.reportWarning(QStringLiteral("Not compressed — another task uses this file"));
        return;
    }

    const QString destPath = destinationPath(srcPath);
    const bool inPlace = destPath == srcPath;
    if (!inPlace && QFileInfo::exists(destPath))
        throw TaskError(QStringLiteral("%1 already exists; the original was kept").arg(QFileInfo(destPath).fileName()));

    const std::optional<double> srcSeconds = AudioDecoder::probeDurationSeconds(srcPath);
    if (!srcSeconds)
        throw TaskError(QStringLiteral("could not read the file's duration (ffprobe)"));

    // Hidden (the vault tree skips dotfiles) and fixed-name, so a leftover from
    // a crashed attempt is ours to clear rather than a user file.
    const QFileInfo srcInfo(srcPath);
    const QString tempPath =
        srcInfo.absolutePath() + QStringLiteral("/.") + srcInfo.completeBaseName() + QStringLiteral(".compressing.flac");
    QFile::remove(tempPath);

    const QStringList common = {QStringLiteral("-hide_banner"), QStringLiteral("-nostdin"),
                                QStringLiteral("-loglevel"),    QStringLiteral("error"),
                                QStringLiteral("-nostats"),     QStringLiteral("-progress"),
                                QStringLiteral("pipe:1")};

    // Encode is the bulk of the bar; the verification decode is the rest.
    constexpr double kEncodeShare = 0.8;
    const double total = std::max(*srcSeconds, 0.001);
    emit phase(QStringLiteral("encoding"));
    QStringList encode = {QStringLiteral("-i"), srcPath, QStringLiteral("-map"), QStringLiteral("0:a")};
    if (m_target == Target::Voice)
        encode << QStringLiteral("-ar") << QStringLiteral("16000") << QStringLiteral("-ac") << QStringLiteral("1");
    encode << QStringLiteral("-c:a") << QStringLiteral("flac") << QStringLiteral("-f") << QStringLiteral("flac")
           << QStringLiteral("-y") << tempPath;
    runFfmpeg(common + encode,
              ctx, [this, total](double s) { emit progress(std::clamp(s / total, 0.0, 1.0) * kEncodeShare); },
              tempPath);

    // Exit 0 doesn't prove a playable file: decode the whole output, then
    // hold its duration against the original's.
    emit phase(QStringLiteral("verifying"));
    runFfmpeg(common + QStringList{QStringLiteral("-xerror"), QStringLiteral("-i"), tempPath, QStringLiteral("-f"),
                                   QStringLiteral("null"), QStringLiteral("-")},
              ctx,
              [this, total](double s) {
                  emit progress(kEncodeShare + std::clamp(s / total, 0.0, 1.0) * (1.0 - kEncodeShare));
              },
              tempPath);
    const std::optional<double> outSeconds = AudioDecoder::probeDurationSeconds(tempPath);
    if (!outSeconds || std::abs(*outSeconds - *srcSeconds) > kMaxDurationDriftSeconds) {
        QFile::remove(tempPath);
        throw TaskError(QStringLiteral("compressed output failed verification (duration %1s vs original %2s); "
                                       "the original was kept")
                            .arg(outSeconds ? QString::number(*outSeconds, 'f', 2) : QStringLiteral("?"),
                                 QString::number(*srcSeconds, 'f', 2)));
    }
    if (ctx.cancelRequested()) {
        QFile::remove(tempPath);
        throw TaskError(QStringLiteral("Cancelled"));
    }

    // Keep the recording time: the tree's date sort and the combine scanner
    // both order by mtime.
    {
        QFile out(tempPath);
        if (out.open(QIODevice::ReadWrite))
            out.setFileTime(srcInfo.lastModified(), QFileDevice::FileModificationTime);
    }

    const qint64 srcBytes = srcInfo.size();
    const qint64 outBytes = QFileInfo(tempPath).size();

    // The original goes FIRST: a refused trash (no trash on this filesystem)
    // must leave it where it was, not beside its replacement — and for an
    // in-place replace it is what frees the name.
    QFile original(srcPath);
    if (!original.moveToTrash()) {
        QFile::remove(tempPath);
        throw TaskError(QStringLiteral("could not move the original to the trash (%1); it was kept")
                            .arg(original.errorString()));
    }
    const QVariantMap trashEntry = {{QStringLiteral("trashPath"), original.fileName()},
                                    {QStringLiteral("originalPath"), srcPath}};

    // QFile::rename never replaces an existing destination (on Linux it is
    // renameat2(RENAME_NOREPLACE)), so a file that appeared since the check
    // above is refused here atomically rather than overwritten.
    QFile staged(tempPath);
    if (!staged.rename(destPath)) {
        const QString why = staged.errorString();
        QFile::remove(tempPath);
        restoreVaultEntries({trashEntry}, vaultUrl.toLocalFile());
        throw TaskError(QStringLiteral("could not move the compressed file into place (%1); the original was kept")
                            .arg(why));
    }

    m_savedBytes = srcBytes - outBytes;
    m_compressedUrl = QUrl::fromLocalFile(destPath);
    m_trashEntry = trashEntry;
    emit progress(1.0);
    emit audioCompressed(fileUrl, m_compressedUrl);
}
