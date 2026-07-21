#pragma once

#include <QUrl>

#include "Task.h"

// Moves a vault folder into another folder — the folder analogue of MoveTask.
// Instant lane: one QDir::rename, no GPU. The pure filesystem work is
// moveVaultFolder(); the tree refreshes via VaultTreeModel's QFileSystemWatcher.
// Undoable: the inverse moves it back to its original parent.
class FolderMoveTask : public Task {
    Q_OBJECT
public:
    FolderMoveTask(QUrl folderUrl, QUrl destParentUrl, QUrl vaultUrl, QObject *parent = nullptr);

    QString kind() const override { return QStringLiteral("moveFolder"); }
    QString kindLabel() const override { return QStringLiteral("Move folder"); }
    QString displayName() const override;
    Lane lane() const override { return Lane::Instant; }
    // Moves files on disk -> the watcher rebuild resets the model, which is the
    // authoritative reflow (see Task::isStructuralFileOp).
    bool isStructuralFileOp() const override { return true; }

    QString undoLabel() const override;
    std::unique_ptr<Task> makeUndoTask() const override;
    bool undoStillApplicable() const override;

    void run(TaskContext &ctx) override;

signals:
    void folderMoved(const QUrl &oldUrl, const QUrl &newUrl, bool ok, const QString &error);

private:
    QUrl m_destParent;
    QUrl m_resultUrl; // captured on success for makeUndoTask()
};
