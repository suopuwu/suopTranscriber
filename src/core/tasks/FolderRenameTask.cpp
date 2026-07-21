#include "FolderRenameTask.h"

#include <QFileInfo>

#include "../FileRename.h"

FolderRenameTask::FolderRenameTask(QUrl folder, QString newName, QUrl vault, QObject *parent)
    : Task(parent)
    , m_newName(std::move(newName))
{
    fileUrl = std::move(folder);
    vaultUrl = std::move(vault);
}

QString FolderRenameTask::displayName() const
{
    return QStringLiteral("Rename folder %1 → %2").arg(QFileInfo(fileUrl.toLocalFile()).fileName(), m_newName);
}

QString FolderRenameTask::undoLabel() const
{
    return QStringLiteral("Renamed folder to %1").arg(m_newName);
}

std::unique_ptr<Task> FolderRenameTask::makeUndoTask() const
{
    if (m_resultUrl.isEmpty())
        return nullptr;
    const QString oldName = QFileInfo(fileUrl.toLocalFile()).fileName();
    return std::make_unique<FolderRenameTask>(m_resultUrl, oldName, vaultUrl);
}

bool FolderRenameTask::undoStillApplicable() const
{
    // The inverse renames m_resultUrl back; if the folder is no longer there (it
    // was moved/renamed again), the undo can't run.
    return !m_resultUrl.isEmpty() && QFileInfo::exists(m_resultUrl.toLocalFile());
}

void FolderRenameTask::run(TaskContext &ctx)
{
    Q_UNUSED(ctx);
    const QUrl oldUrl = fileUrl;
    QUrl newUrl;
    QString error;
    const bool ok = renameVaultFolder(oldUrl, m_newName, vaultUrl.toLocalFile(), &newUrl, &error);
    if (ok)
        m_resultUrl = newUrl;
    emit folderRenamed(oldUrl, newUrl, ok, error);
    if (!ok)
        throw TaskError(error);
}
