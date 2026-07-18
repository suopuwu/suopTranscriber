#pragma once

#include <QString>
#include <QUrl>

#include "Task.h"

// Renames a recording file (and its sibling .md) — the file-level rename, not
// the speaker-label rename (RenameSpeakerTask). Instant lane: a couple of
// QFile::rename syscalls, no GPU, so it must not wait behind a transcription —
// but the scheduler still defers it if a transcribe of THIS file is active
// (renaming would move the .md out from under a running transcription). The
// actual filesystem work is VaultTreeModel::renameFileOnDisk(); the tree
// model refreshes itself via its QFileSystemWatcher when the files move.
class RenameFileTask : public Task {
    Q_OBJECT
public:
    RenameFileTask(QUrl fileUrl, QString newBaseName, QObject *parent = nullptr);

    QString kind() const override { return QStringLiteral("renameFile"); }
    QString kindLabel() const override { return QStringLiteral("Rename"); }
    QString displayName() const override;
    Lane lane() const override { return Lane::Instant; }
    // Not persisted: a rename is an instant, in-session action; there's nothing
    // to resume across a restart (the file is either renamed or it isn't).
    QVariantMap toVariantMap() const override;

    // Undoable: the inverse is a rename-back of the now-renamed file to its
    // original base name. makeUndoTask() runs after run() so it uses the new
    // path captured there.
    QString undoLabel() const override;
    std::unique_ptr<Task> makeUndoTask() const override;

    void run(TaskContext &ctx) override;

signals:
    // oldUrl is this task's original fileUrl; newUrl is the renamed primary
    // file (empty on failure). ok/error distinguish. QML repoints the player
    // to newUrl on success and rolls back its optimistic field on failure.
    void fileRenamed(const QUrl &oldUrl, const QUrl &newUrl, bool ok, const QString &error);

private:
    QString m_newBaseName;
    // The renamed primary file's URL, captured on a successful run() so
    // makeUndoTask() can rename it back. Empty until run() succeeds.
    QUrl m_resultUrl;
};
