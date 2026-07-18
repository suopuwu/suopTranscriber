#pragma once

#include <QList>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QVariantList>

#include <utility>

// Pure, model-free filesystem trash / permanent-delete / restore of vault
// recordings — extracted out of VaultTreeModel so a worker-thread Task
// (TrashTask / DeleteTask / RestoreTask) can run it OFF the GUI thread without
// the tree model, exactly as FileRename.h did for rename/move.
//
// VaultTreeModel's trash*/delete*/restoreFromTrash() become thin wrappers: the
// pure fn here does the filesystem work, then the model does rebuild() + the
// per-file signal emission (VaultTreeModel::applyRemovalOutcome() /
// applyRestoreOutcome()). The emission stays in the model on purpose — it owns
// the orphaned-.md-vs-audio classification that MainPanel's delete/restore
// handlers depend on (emitAudioOnlyDeleted/emitRestored) — while the async task
// path has the controller call those same apply* methods once run() completes.

// Which pieces of each entry a trash/delete batch affects.
enum class VaultFileScope {
    Files,      // the whole file: audio + <name>.md sidecar + .<name>.partial.jsonl
    Transcript, // just the transcript (.md + .partial), keeping the audio
    AudioOnly,  // just the audio (+ .partial), keeping the .md (becomes orphaned)
    Folder,     // a whole folder tree (single url), guarded against the vault root
};

// One trashed/removed entry: its url, plus — for AudioOnly ONLY — the transcript
// .md path resolved BEFORE the audio moved (whether that .md survived decides
// which signal the entry earns; see VaultTreeModel::emitAudioOnlyDeleted).
// mdPath is empty for every other scope.
using RemovedEntry = std::pair<QUrl, QString>;

// The pure result of a trash batch — everything the model needs to rebuild and
// emit the right per-file signals, with NO model dependency. Mirrors the
// {trashed,restorable,untrashable,invalid} result map VaultTreeModel::trashFiles()
// has always returned, but keeps `trashed` as typed RemovedEntry rather than a
// bare count so applyRemovalOutcome() can emit the correct per-entry signal.
struct TrashBatchResult {
    QList<RemovedEntry> trashed;
    QVariantList restorable;  // {trashPath, originalPath} maps — hand to restoreVaultEntries() to undo
    QVariantList untrashable; // url strings the FS refused — STILL on disk, untouched
    QVariantList invalid;     // url strings that failed a guard / didn't exist
};

// Pure filesystem trash of a batch (worker-thread-safe, no model). vaultLocalPath
// bounds the containment guard (every entry must stay inside the vault; a Folder
// is additionally never the vault root itself). Folder scope expects exactly one
// url. A failed guard/absent entry becomes `invalid`; an entry the filesystem
// refuses to trash (non-destructive — see FileRename/VaultTreeModel notes)
// becomes `untrashable`, left fully on disk for a caller to confirm-then-delete.
TrashBatchResult trashVaultEntries(const QList<QUrl> &urls, VaultFileScope scope,
                                   const QString &vaultLocalPath);

// Pure permanent (IRREVERSIBLE) delete of a batch — the counterpart to
// trashVaultEntries(), only ever reached after the user confirms the untrashable
// entries. Returns the entries actually removed (same RemovedEntry shape, for the
// post-delete signals).
QList<RemovedEntry> deleteVaultEntriesPermanently(const QList<QUrl> &urls, VaultFileScope scope,
                                                  const QString &vaultLocalPath);

// Pure restore of trashed entries (a TrashBatchResult::restorable list). Moves
// each {trashPath, originalPath} back, recreating a vanished parent dir and
// dropping the freedesktop .trashinfo record. Returns the original paths actually
// restored, in input order (parents before children). vaultLocalPath bounds the
// same containment guard as the trash side.
QStringList restoreVaultEntries(const QVariantList &entries, const QString &vaultLocalPath);
