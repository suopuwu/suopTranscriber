#include "UncombineTask.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>

namespace {
QString archivedPathFor(const QString &originalPath)
{
    const QFileInfo info(originalPath);
    return info.absolutePath() + QStringLiteral("/.combined/") + info.fileName();
}

// The sidecar convention is <fileName>.md — the FULL audio filename (extension
// included) plus ".md", NOT <completeBaseName>.md. Kept as a local derivation
// (matching TranscriptionController's sidecarPath()) so this task, which links
// into the task-only test targets, doesn't drag in the whole controller.
QString sidecarPathFor(const QString &audioPath)
{
    const QFileInfo info(audioPath);
    return info.absolutePath() + QStringLiteral("/") + info.fileName() + QStringLiteral(".md");
}
} // namespace

UncombineTask::UncombineTask(QStringList originalPaths, QUrl outputUrl, QUrl vault, QObject *parent)
    : Task(parent)
    , m_originalPaths(std::move(originalPaths))
{
    fileUrl = std::move(outputUrl);
    vaultUrl = std::move(vault);
}

QString UncombineTask::displayName() const
{
    return QStringLiteral("Restore %1 original parts").arg(m_originalPaths.size());
}

QVariantMap UncombineTask::toVariantMap() const
{
    return {{QStringLiteral("uncombineOriginalPaths"), m_originalPaths},
            {QStringLiteral("mergedPath"), fileUrl.toLocalFile()}};
}

void UncombineTask::run(TaskContext &ctx)
{
    Q_UNUSED(ctx);
    const QString mergedPath = fileUrl.toLocalFile();

    // Refuse rather than destroy work: if the merged file has been transcribed
    // since the combine, removing it would orphan its .md. Same principle as
    // the combine scanner's "alreadyTranscribed" group status.
    if (QFileInfo::exists(sidecarPathFor(mergedPath)))
        throw TaskError(QStringLiteral("the merged file has been transcribed — undo would discard that transcript"));

    QStringList failed;
    for (const QString &original : std::as_const(m_originalPaths)) {
        if (QFileInfo::exists(original))
            continue; // already back where it belongs
        if (!QFile::rename(archivedPathFor(original), original))
            failed << QFileInfo(original).fileName();
    }

    // Never remove the merge unless every part is back — a half-restore that
    // also deleted the merged file would lose audio outright.
    if (!failed.isEmpty())
        throw TaskError(QStringLiteral("could not restore: %1").arg(failed.join(QStringLiteral(", "))));

    QFile::remove(mergedPath);

    // Tidy the now-(possibly)-empty .combined/ dirs; rmdir is a no-op when
    // other groups' parts still live there.
    for (const QString &original : std::as_const(m_originalPaths))
        QDir().rmdir(QFileInfo(original).absolutePath() + QStringLiteral("/.combined"));
}
