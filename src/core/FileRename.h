#pragma once

#include <QString>
#include <QUrl>

// The PURE filesystem rename of a recording (audio file + its sibling .md, or
// a standalone orphaned .md) — validation + the renames + rollback, NO model
// state. Extracted out of VaultTreeModel so it can run on a worker thread
// (RenameFileTask) without pulling in the tree model; VaultTreeModel::
// renameFile() is now a thin wrapper (this + rebuild()). The tree refreshes
// itself via VaultTreeModel's QFileSystemWatcher when the files move, so no
// explicit rebuild is required off the task path.
//
// *newUrlOut receives the renamed primary file's URL (the audio if there was
// one, else the renamed .md) on success. Returns false + sets *errorOut on a
// validation or filesystem failure, never leaving the audio/.md pair
// mismatched.
bool renameRecordingFile(const QUrl &fileUrl, const QString &newBaseName, QUrl *newUrlOut,
                          QString *errorOut = nullptr);

// True if candidatePath is the vault root or lies inside it. Pure (takes the
// vault root explicitly, no model state) so both VaultTreeModel::
// isPathInsideVault() and the worker-thread move helper below share ONE
// implementation. candidatePath need not exist yet — walks up to the nearest
// existing ancestor before canonicalizing, so a not-yet-created destination
// still resolves. Empty for either argument ⇒ false.
bool pathIsInsideVault(const QString &candidatePath, const QString &vaultLocalPath);

// The PURE filesystem MOVE of a recording (an audio file + its <name>.md
// sidecar + .<name>.partial.jsonl resume state, or a standalone orphaned .md)
// from its current folder into destDirUrl — validation + the renames +
// rollback, NO model state. Extracted out of VaultTreeModel::moveFile() for
// the same reason renameRecordingFile() was: MoveTask runs it on a worker
// thread without pulling in the tree model. vaultLocalPath is the vault root,
// needed for the inside-vault guard (both ends must stay inside it).
//
// *newUrlOut receives the moved primary file's new URL (destDir/<name>) on
// success. Returns false + sets *errorOut on a validation/collision/filesystem
// failure, never leaving the audio/.md set split across two folders.
bool moveRecordingFile(const QUrl &fileUrl, const QUrl &destDirUrl, const QString &vaultLocalPath,
                       QUrl *newUrlOut, QString *errorOut = nullptr);

// True if `name` is a valid new file/folder name — non-empty after trimming, no
// filesystem-invalid character (/\:*?"<>|), and not "."/".." (which name an
// existing directory rather than a new child). The shared validator behind the
// folder rename/move helpers below.
bool isValidVaultEntryName(const QString &name);

// Pure filesystem rename of a folder IN PLACE (parent unchanged) — the extracted
// core of VaultTreeModel::renameFolder(), so FolderRenameTask can run it off the
// GUI thread. Guards: valid name, never the vault root itself, never outside the
// vault, no collision at the new name. Renaming to the name it already has is a
// no-op success. *newUrlOut = the renamed folder's url on success.
bool renameVaultFolder(const QUrl &folderUrl, const QString &newName, const QString &vaultLocalPath,
                       QUrl *newUrlOut, QString *errorOut = nullptr);

// Pure filesystem move of a folder into destParentUrl — the extracted core of
// VaultTreeModel::moveFolder(). Guards: never the vault root, both ends inside
// the vault, never into itself or one of its own descendants, no collision.
// *newUrlOut = the folder's new url on success.
bool moveVaultFolder(const QUrl &folderUrl, const QUrl &destParentUrl, const QString &vaultLocalPath,
                     QUrl *newUrlOut, QString *errorOut = nullptr);
