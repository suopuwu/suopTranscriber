#pragma once

#include <QList>
#include <QUrl>

#include "../VaultFileOps.h"
#include "Task.h"

// Moves a batch of vault entries to the trash (recoverably) — the file/
// transcript/audio-only/folder variants all in one kind, distinguished by
// VaultFileScope. Instant lane: a handful of moveToTrash syscalls, no GPU, so it
// must not wait behind a long transcription (deferred only if a transcribe of
// the SAME file is active, like rename/move).
//
// The filesystem work (trashVaultEntries) is pure and runs on the worker thread;
// the tree rebuild + the per-file delete signals MainPanel reacts to
// (fileDeleted/transcriptDeleted/audioDeletedTranscriptKept) are applied by
// VaultTreeModel::applyRemovalOutcome(), which TranscriptionController calls on
// the GUI thread once run() completes (it owns the orphaned-vs-audio
// classification). The whole batch is ONE task ⇒ ONE undo snackbar; undo is a
// RestoreTask over the captured restorable entries.
//
// A trash is never a task FAILURE: entries the filesystem refuses ("untrashable")
// are the expected non-destructive case that drives the confirm-then-permanent-
// delete flow, carried back in the result, not thrown.
class TrashTask : public Task {
    Q_OBJECT
public:
    // For Files/Transcript/AudioOnly, urls is the selection; for Folder it's a
    // single folder url. vaultUrl bounds the containment guard (Task::vaultUrl).
    // fileUrl (the base's "one file") is set to the first url so the Tasks row /
    // same-file conflict gate have something to key on.
    TrashTask(VaultFileScope scope, QList<QUrl> urls, QUrl vaultUrl, QObject *parent = nullptr);

    QString kind() const override { return QStringLiteral("trash"); }
    QString kindLabel() const override { return QStringLiteral("Move to trash"); }
    QString displayName() const override;
    Lane lane() const override { return Lane::Instant; }
    // fileUrl is only the first of the selection — see Task::touchedFiles().
    QList<QUrl> touchedFiles() const override { return m_urls; }

    // Undoable iff something actually went to the trash (restorable non-empty).
    QString undoLabel() const override;
    std::unique_ptr<Task> makeUndoTask() const override;

    void run(TaskContext &ctx) override;

    // Read on the GUI thread by the controller AFTER run() completes (stable —
    // run() has returned by the time the completion signal's slot fires).
    VaultFileScope scope() const { return m_scope; }
    const TrashBatchResult &result() const { return m_result; }

signals:
    // Fired once run() finishes (carries no args — the controller reads scope()/
    // result() directly, which avoids registering a metatype for the queued
    // worker→GUI delivery). The controller applies the model outcome + drives
    // the untrashable-confirm flow from it.
    void trashDone();

private:
    VaultFileScope m_scope;
    QList<QUrl> m_urls;
    TrashBatchResult m_result;
};
