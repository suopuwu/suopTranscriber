#pragma once

#include <QString>
#include <QUrl>

#include "Task.h"

// Moves a recording file (audio + its .md sidecar + .partial.jsonl resume
// state, or a standalone orphaned .md) into another vault folder — the
// file-level move, the inverse of which is a move-back. Instant lane: a
// handful of QFile::rename syscalls, no GPU, so it must NOT wait behind a long
// transcription — but the scheduler still defers it if a transcribe of THIS
// file is active (moving the .md would pull it out from under a running
// transcription). The actual filesystem work is moveRecordingFile() (shared
// with VaultTreeModel::moveFile()); the tree model refreshes itself via its
// QFileSystemWatcher when the files move.
//
// Multi-select moves enqueue one MoveTask per file as a single undo BATCH (see
// TranscriptionController::moveFiles()/enqueueBatch()), so N files collapse
// into one "Moved N files" undo snackbar.
class MoveTask : public Task {
    Q_OBJECT
public:
    // vaultUrl bounds the inside-vault check moveRecordingFile() enforces on
    // both ends; it is stored on the Task base (Task::vaultUrl).
    MoveTask(QUrl fileUrl, QUrl destDirUrl, QUrl vaultUrl, QObject *parent = nullptr);

    QString kind() const override { return QStringLiteral("moveFile"); }
    QString kindLabel() const override { return QStringLiteral("Move"); }
    QString displayName() const override;
    Lane lane() const override { return Lane::Instant; }
    // Not persisted: an instant, in-session action; the file is either moved or
    // it isn't, nothing to resume across a restart.
    QVariantMap toVariantMap() const override;

    // Undoable: the inverse moves the now-moved file BACK to its original
    // folder. makeUndoTask() runs after run() so it uses the new location
    // captured there. batchUndoLabel() summarizes a multi-file move.
    QString undoLabel() const override;
    QString batchUndoLabel(int count) const override;
    std::unique_ptr<Task> makeUndoTask() const override;

    void run(TaskContext &ctx) override;

signals:
    // oldUrl is this task's original fileUrl; newUrl is the moved primary file
    // (empty on failure). QML repoints the player to newUrl when the moved
    // file was the open one; ok/error distinguish success from failure.
    void fileMoved(const QUrl &oldUrl, const QUrl &newUrl, bool ok, const QString &error);

private:
    QUrl m_destDir;
    // The moved primary file's URL, captured on a successful run() so
    // makeUndoTask() can move it back. Empty until run() succeeds.
    QUrl m_resultUrl;
};
