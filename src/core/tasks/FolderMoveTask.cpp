#include "FolderMoveTask.h"

#include <QFileInfo>

#include "../FileRename.h"

FolderMoveTask::FolderMoveTask(QUrl folder, QUrl destParentUrl, QUrl vault, QObject *parent)
    : Task(parent)
    , m_destParent(std::move(destParentUrl))
{
    fileUrl = std::move(folder);
    vaultUrl = std::move(vault);
}

QString FolderMoveTask::displayName() const
{
    return QStringLiteral("Move folder %1 → %2")
        .arg(QFileInfo(fileUrl.toLocalFile()).fileName(), QFileInfo(m_destParent.toLocalFile()).fileName());
}

QString FolderMoveTask::undoLabel() const
{
    return QStringLiteral("Moved folder %1").arg(QFileInfo(fileUrl.toLocalFile()).fileName());
}

std::unique_ptr<Task> FolderMoveTask::makeUndoTask() const
{
    if (m_resultUrl.isEmpty())
        return nullptr;
    // Move the folder back to the parent it started in.
    const QUrl originalParent = QUrl::fromLocalFile(QFileInfo(fileUrl.toLocalFile()).absolutePath());
    return std::make_unique<FolderMoveTask>(m_resultUrl, originalParent, vaultUrl);
}

bool FolderMoveTask::undoStillApplicable() const
{
    // The inverse moves m_resultUrl back; if the folder is no longer there (it
    // was moved/renamed again), the undo can't run.
    return !m_resultUrl.isEmpty() && QFileInfo::exists(m_resultUrl.toLocalFile());
}

void FolderMoveTask::run(TaskContext &ctx)
{
    Q_UNUSED(ctx);
    const QUrl oldUrl = fileUrl;
    QUrl newUrl;
    QString error;
    const bool ok = moveVaultFolder(oldUrl, m_destParent, vaultUrl.toLocalFile(), &newUrl, &error);
    if (ok)
        m_resultUrl = newUrl;
    emit folderMoved(oldUrl, newUrl, ok, error);
    if (!ok)
        throw TaskError(error);
}
