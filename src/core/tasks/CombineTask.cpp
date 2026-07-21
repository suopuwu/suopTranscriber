#include "CombineTask.h"

#include "../ProcessRunner.h"
#include "UncombineTask.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QProcess>
#include <QTemporaryFile>

#include <algorithm>
#include <memory>

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
    ffmpeg.start(QStringLiteral("ffmpeg"),
                  {QStringLiteral("-f"), QStringLiteral("concat"), QStringLiteral("-safe"), QStringLiteral("0"),
                   QStringLiteral("-i"), listFile.fileName(), QStringLiteral("-c"), QStringLiteral("copy"),
                   QStringLiteral("-y"), QStringLiteral("-hide_banner"), QStringLiteral("-loglevel"),
                   QStringLiteral("error"), outputPath});
    if (!ffmpeg.waitForStarted())
        throw TaskError(QStringLiteral("failed to start ffmpeg"));

    // Pausable poll (see ProcessRunner::runPausable). Progress is approximate
    // (output file size vs. summed input size); reportPaused mirrors the
    // SIGSTOP/SIGCONT state up to the task UI.
    const auto outcome = ProcessRunner::runPausable(
        ffmpeg,
        {[&ctx] { return ctx.cancelRequested(); }, [&ctx] { return ctx.pauseRequested(); },
         [&ctx](bool paused) { ctx.reportPaused(paused); }},
        [&] {
            if (totalInputBytes > 0) {
                const qint64 outBytes = QFileInfo(outputPath).size();
                emit progress(
                    std::clamp(static_cast<double>(outBytes) / static_cast<double>(totalInputBytes), 0.0, 1.0));
            }
        });

    if (outcome == ProcessRunner::RunOutcome::Cancelled) {
        QFile::remove(outputPath);
        throw TaskError(QStringLiteral("Cancelled"));
    }

    if (ffmpeg.exitStatus() != QProcess::NormalExit || ffmpeg.exitCode() != 0) {
        const QString err = QString::fromUtf8(ffmpeg.readAllStandardError());
        QFile::remove(outputPath);
        throw TaskError(err.isEmpty() ? QStringLiteral("ffmpeg concat failed") : err);
    }

    emit progress(1.0);

    // Restamp only now that ffmpeg has exited and closed the output.
    if (firstPartMtime.isValid()) {
        QFile out(outputPath);
        if (out.open(QIODevice::ReadWrite))
            out.setFileTime(firstPartMtime, QFileDevice::FileModificationTime);
    }

    // Move (or delete) every source now that the merge succeeded.
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
