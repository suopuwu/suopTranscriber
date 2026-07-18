#pragma once

#include <QString>
#include <QUrl>

#include "Task.h"

// Renames a vault folder in place (parent unchanged) — the folder analogue of
// RenameFileTask. Instant lane: one QDir::rename, no GPU. The pure filesystem
// work is renameVaultFolder(); the tree refreshes via VaultTreeModel's
// QFileSystemWatcher, so unlike trash/delete there are no per-file model signals
// to re-emit. Undoable: the inverse renames it back.
class FolderRenameTask : public Task {
    Q_OBJECT
public:
    FolderRenameTask(QUrl folderUrl, QString newName, QUrl vaultUrl, QObject *parent = nullptr);

    QString kind() const override { return QStringLiteral("renameFolder"); }
    QString kindLabel() const override { return QStringLiteral("Rename folder"); }
    QString displayName() const override;
    Lane lane() const override { return Lane::Instant; }

    QString undoLabel() const override;
    std::unique_ptr<Task> makeUndoTask() const override;

    void run(TaskContext &ctx) override;

signals:
    void folderRenamed(const QUrl &oldUrl, const QUrl &newUrl, bool ok, const QString &error);

private:
    QString m_newName;
    QUrl m_resultUrl; // captured on success for makeUndoTask()
};
