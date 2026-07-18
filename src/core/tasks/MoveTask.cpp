#include "MoveTask.h"

#include <QFileInfo>

#include "../FileRename.h"

MoveTask::MoveTask(QUrl file, QUrl destDirUrl, QUrl vault, QObject *parent)
    : Task(parent)
    , m_destDir(std::move(destDirUrl))
{
    fileUrl = std::move(file);
    vaultUrl = std::move(vault);
}

QString MoveTask::displayName() const
{
    return QStringLiteral("Move %1 → %2")
        .arg(QFileInfo(fileUrl.toLocalFile()).fileName(), QFileInfo(m_destDir.toLocalFile()).fileName());
}

QVariantMap MoveTask::toVariantMap() const
{
    return {{QStringLiteral("destDir"), m_destDir}};
}

QString MoveTask::undoLabel() const
{
    return QStringLiteral("Moved %1").arg(QFileInfo(fileUrl.toLocalFile()).fileName());
}

QString MoveTask::batchUndoLabel(int count) const
{
    // One snackbar for the whole multi-select move; a batch of one keeps the
    // specific single-file label.
    if (count <= 1)
        return undoLabel();
    return QStringLiteral("Moved %1 files").arg(count);
}

std::unique_ptr<Task> MoveTask::makeUndoTask() const
{
    if (m_resultUrl.isEmpty())
        return nullptr; // run() hasn't succeeded — nothing to reverse
    // Move the now-moved file BACK to the folder it started in (the parent of
    // this task's original fileUrl).
    const QUrl originalDir = QUrl::fromLocalFile(QFileInfo(fileUrl.toLocalFile()).absolutePath());
    return std::make_unique<MoveTask>(m_resultUrl, originalDir, vaultUrl);
}

void MoveTask::run(TaskContext &ctx)
{
    Q_UNUSED(ctx);
    const QUrl oldUrl = fileUrl;
    QUrl newUrl;
    QString error;
    // Pure filesystem — safe on the worker thread (no model access). The tree
    // model refreshes itself via its QFileSystemWatcher once the files move.
    const bool ok = moveRecordingFile(oldUrl, m_destDir, vaultUrl.toLocalFile(), &newUrl, &error);
    if (ok)
        m_resultUrl = newUrl; // captured for makeUndoTask()
    emit fileMoved(oldUrl, newUrl, ok, error);
    if (!ok)
        throw TaskError(error);
}
