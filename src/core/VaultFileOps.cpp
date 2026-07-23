#include "VaultFileOps.h"

#include "AudioExtensions.h"
#include "FileRename.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSet>
#include <QVariantMap>

// Pure, model-free filesystem trash / permanent-delete / restore of vault
// recordings — extracted verbatim out of VaultTreeModel. See VaultFileOps.h for
// the contract; the file-local helpers below are the ones that used to live in
// VaultTreeModel.cpp's anonymous namespace (trashOnly/removePermanently/
// trashInfoPathFor) plus its transcriptPathsFor()/recordRestorable() statics and
// its guardedLocalPath() member, made pure by taking vaultLocalPath explicitly.

namespace {

// Files and folders are handled by separate scopes on purpose (see
// guardedLocalPath()): a folder delete is recursive, so it must never be
// reachable from the file-selection path.
enum class EntryKind { File, Folder };

// What became of one entry in a batch. Untrashable is emphatically NOT a
// failure: the entry is still on disk, untouched, and is exactly what the
// UI must ask the user to confirm a permanent delete for.
enum class TrashOutcome { Trashed, Untrashable, Invalid };

// Trash a path, RECOVERABLY, and report where it landed. Never falls back to a
// permanent delete — that's removePermanently()'s job, and keeping the two
// apart is what lets the UI offer Undo for the recoverable case and ask for
// confirmation only for the irreversible one.
//
// On success *trashPathOut is the file's new location in the trash, which is
// the ONLY handle a later restoreFromTrash() has. QFile::moveToTrash() rewrites
// fileName() to it, and that value is unrecoverable once the QFile goes out of
// scope, so it's captured here rather than rediscovered.
//
// A FAILED moveToTrash() is non-destructive — the original is left fully
// intact (verified empirically on Qt 6.11; an older comment here claimed
// fileName() could be left pointing at a partial trash path on failure, which
// did not reproduce). That guarantee is load-bearing: it's what makes
// "attempt the trash, and only ask about a permanent delete if it fails" safe,
// and it's why nothing here tries to PREDICT trash-ability (which would mean
// reimplementing the freedesktop spec's device/top-dir rules and hoping we
// match Qt's).
//
// Takes directories as well as files — moveToTrash() trashes a whole tree
// intact. Returns false if the path doesn't exist to begin with (an absent
// optional sidecar isn't a failure — callers ignore the result for those).
bool trashOnly(const QString &path, QString *trashPathOut = nullptr)
{
    const QFileInfo info(path);
    if (!info.exists())
        return false;
    QFile file(path);
    if (!file.moveToTrash())
        return false;
    if (trashPathOut)
        *trashPathOut = file.fileName();
    return true;
}

// Irreversible. Only ever reached once the user has confirmed a delete that
// could NOT be trashed. QFile::remove() refuses a directory, so this branches.
bool removePermanently(const QString &path)
{
    const QFileInfo info(path);
    if (!info.exists())
        return false;
    return info.isDir() ? QDir(path).removeRecursively() : QFile::remove(path);
}

// The freedesktop trash spec pairs every trashed file with an info entry:
//   <trashroot>/files/<name>  <->  <trashroot>/info/<name>.trashinfo
// Qt gives us the former and has no restore API at all, so a restore has to
// derive and delete the latter itself — renaming only the file back would
// orphan the info entry and leave a phantom row in the desktop's trash UI.
QString trashInfoPathFor(const QString &trashedFilePath)
{
    const QFileInfo info(trashedFilePath);
    QDir filesDir(info.absolutePath());
    if (filesDir.dirName() != QLatin1String("files"))
        return {}; // not a spec-shaped trash location — nothing to clean up
    filesDir.cdUp();
    return filesDir.absoluteFilePath(QStringLiteral("info/") + info.fileName()
                                     + QStringLiteral(".trashinfo"));
}

// The transcript set for `path`: its .md sidecar and .partial.jsonl resume
// state. Same full-filename convention as sidecarMdPath()/moveFile().
// Returns false if path isn't a thing that HAS a transcript.
bool transcriptPathsFor(const QFileInfo &info, QString *mdOut, QString *partialOut)
{
    const QString dirPath = info.absolutePath();
    if (isAudioFile(info)) {
        *mdOut = dirPath + '/' + info.fileName() + ".md";
        *partialOut = dirPath + "/." + info.fileName() + ".partial.jsonl";
        return true;
    }
    if (info.suffix().toLower() == "md") {
        // An orphaned .md passed directly: it IS the transcript.
        *mdOut = info.absoluteFilePath();
        *partialOut = dirPath + "/." + info.fileName() + ".partial.jsonl";
        return true;
    }
    return false;
}

// Records one trashed path so restoreVaultEntries() can put it back. The trash
// path is only knowable at trash time — nothing can reconstruct it later.
void recordRestorable(QVariantList *restorable, const QString &trashPath,
                      const QString &originalPath)
{
    if (!restorable || trashPath.isEmpty())
        return;
    restorable->append(QVariantMap{ { QStringLiteral("trashPath"), trashPath },
                                    { QStringLiteral("originalPath"), originalPath } });
}

// Shared guard set for trash + permanent delete of a whole file or folder:
// inside the vault, never the vault root itself. Returns the resolved local
// path, or empty if refused. Pure — the vault root arrives explicitly rather
// than from the model.
QString guardedLocalPath(const QUrl &url, EntryKind kind, const QString &vaultLocalPath)
{
    const QString path = url.toLocalFile();
    if (path.isEmpty())
        return {};

    const QFileInfo info(path);
    if (!info.exists())
        return {};
    // Files and folders stay strictly separate, and this is a safety guard, not
    // tidiness: a folder delete is recursive and takes every transcript under
    // it, so it must never be reachable from the call that handles a file
    // SELECTION — one stray folder path leaking into a multi-select would
    // otherwise silently destroy a whole tree.
    if (kind == EntryKind::File && !info.isFile())
        return {};
    if (kind == EntryKind::Folder && !info.isDir())
        return {};

    const QString canonical = info.canonicalFilePath();
    const QString vaultCanonical = QFileInfo(vaultLocalPath).canonicalFilePath();
    if (canonical.isEmpty() || vaultCanonical.isEmpty())
        return {};
    // Never the vault root itself — deleting it would take the whole vault out
    // from under the open window...
    if (canonical == vaultCanonical)
        return {};
    // ...nor anything outside the vault (pathIsInsideVault also accepts the
    // root, hence the explicit equality guard above).
    if (!pathIsInsideVault(path, vaultLocalPath))
        return {};

    return path;
}

// ---- Per-entry trash halves -------------------------------------------------

// The guards + trash of ONE file/folder entry, WITHOUT the rebuild()/signal
// that must happen once for a whole batch. Appends a {trashPath, originalPath}
// map to *restorable for every path it actually trashed, sidecars included.
TrashOutcome trashEntry(const QUrl &url, EntryKind kind, const QString &vaultLocalPath,
                        QVariantList *restorable)
{
    const QString path = guardedLocalPath(url, kind, vaultLocalPath);
    if (path.isEmpty())
        return TrashOutcome::Invalid;

    // Resolve the sidecar set BEFORE trashing anything. QFileInfo stats lazily,
    // so asking info.isFile() after the file has already moved to the trash
    // stats a path that no longer exists and answers false — silently stranding
    // every .md sidecar in the vault. Read what we need while it's still there.
    const QFileInfo info(path);
    const bool isAudio = info.isFile() && isAudioFile(info);
    QString mdPath, partialPath;
    if (isAudio)
        transcriptPathsFor(info, &mdPath, &partialPath);

    // The entry itself first. If the filesystem refuses, STOP — the entry is
    // untouched and the caller will ask the user about a permanent delete. In
    // particular don't trash the sidecars of a file that itself survived: that
    // would leave the audio stranded without its transcript.
    QString trashPath;
    if (!trashOnly(path, &trashPath))
        return TrashOutcome::Untrashable;
    recordRestorable(restorable, trashPath, path);

    // Carry the sidecars off with an audio file, mirroring moveFile()'s set. A
    // folder took its contents with it; an orphaned .md has none.
    if (isAudio) {
        // Best-effort: an absent sidecar isn't a failure, and a sidecar that
        // won't trash isn't worth stranding the whole delete over — it just
        // doesn't come back on undo.
        QString sidecarTrash;
        if (trashOnly(mdPath, &sidecarTrash))
            recordRestorable(restorable, sidecarTrash, mdPath);
        if (trashOnly(partialPath, &sidecarTrash))
            recordRestorable(restorable, sidecarTrash, partialPath);
    }

    return TrashOutcome::Trashed;
}

// Irreversible counterpart to trashEntry(), same guards and same sidecar set.
bool removeEntryPermanently(const QUrl &url, EntryKind kind, const QString &vaultLocalPath)
{
    const QString path = guardedLocalPath(url, kind, vaultLocalPath);
    if (path.isEmpty())
        return false;

    // Same lazy-stat trap as trashEntry(): resolve the sidecar set while the
    // file still exists, not after removePermanently() has unlinked it.
    const QFileInfo info(path);
    const bool isAudio = info.isFile() && isAudioFile(info);
    QString mdPath, partialPath;
    if (isAudio)
        transcriptPathsFor(info, &mdPath, &partialPath);

    if (!removePermanently(path))
        return false;

    if (isAudio) {
        removePermanently(mdPath);      // best-effort; absent sidecars aren't failures
        removePermanently(partialPath);
    }
    return true;
}

// The per-entry half of the Transcript scope: trashes an audio file's sidecar
// transcript (.md + .partial), keeping the audio. A no-op entry (audio with no
// .md to remove) reports Invalid with an empty *restorable rather than
// Untrashable — the filesystem refused nothing.
TrashOutcome trashTranscriptEntry(const QUrl &fileUrl, const QString &vaultLocalPath,
                                  QVariantList *restorable)
{
    const QString path = fileUrl.toLocalFile();
    const QFileInfo info(path);
    QString mdPath, partialPath;
    // NOT guardedLocalPath(): the transcript of an audio file is what's being
    // removed, so it's the .md that must be inside the vault, and the audio
    // itself is never touched. isPathInsideVault covers it.
    if (path.isEmpty() || !info.exists() || !info.isFile() || !pathIsInsideVault(path, vaultLocalPath)
        || !transcriptPathsFor(info, &mdPath, &partialPath))
        return TrashOutcome::Invalid;
    // Nothing transcribed here — a no-op, not a failure, and emphatically not
    // Untrashable: the filesystem refused nothing, so there is nothing for the
    // caller to offer a permanent delete of.
    if (!QFileInfo::exists(mdPath))
        return TrashOutcome::Invalid;

    QString trashPath;
    if (!trashOnly(mdPath, &trashPath))
        return TrashOutcome::Untrashable;
    recordRestorable(restorable, trashPath, mdPath);
    if (trashOnly(partialPath, &trashPath)) // best-effort; an absent partial is fine
        recordRestorable(restorable, trashPath, partialPath);
    return TrashOutcome::Trashed;
}

// The per-entry half of the AudioOnly scope: trashes the audio (+ .partial) and
// DELIBERATELY LEAVES its <fileName>.md. *mdPathOut carries the .md path back
// for the caller's post-rebuild emitAudioOnlyDeleted(): whether the transcript
// survived decides which signal the entry earns, and the path must be resolved
// before the audio moves (see trashEntry()'s lazy-stat comment).
TrashOutcome trashAudioOnlyEntry(const QUrl &fileUrl, const QString &vaultLocalPath,
                                 QVariantList *restorable, QString *mdPathOut)
{
    const QString path = guardedLocalPath(fileUrl, EntryKind::File, vaultLocalPath);
    // Audio only: "drop the audio, keep the transcript" is meaningless for an
    // orphaned .md (there's no audio) and for a non-audio file (there's no
    // transcript), so both are rejected rather than silently doing something else.
    const QFileInfo info(path);
    if (path.isEmpty() || !isAudioFile(info))
        return TrashOutcome::Invalid;

    // Same lazy-stat trap as trashEntry(): resolve the partial's path while the
    // audio still exists, not after it has moved to the trash.
    QString partialPath;
    transcriptPathsFor(info, mdPathOut, &partialPath);

    QString trashPath;
    if (!trashOnly(path, &trashPath))
        return TrashOutcome::Untrashable;
    recordRestorable(restorable, trashPath, path);

    // The partial follows the audio; *mdPathOut is deliberately never touched.
    if (trashOnly(partialPath, &trashPath)) // best-effort; an absent partial is fine
        recordRestorable(restorable, trashPath, partialPath);
    return TrashOutcome::Trashed;
}

} // namespace

TrashBatchResult trashVaultEntries(const QList<QUrl> &urls, VaultFileScope scope,
                                   const QString &vaultLocalPath)
{
    TrashBatchResult result;
    result.trashed.reserve(urls.size());

    auto classify = [&](TrashOutcome outcome, const QUrl &url, const QString &mdPath) {
        switch (outcome) {
        case TrashOutcome::Trashed:
            result.trashed.append({ url, mdPath });
            break;
        case TrashOutcome::Untrashable:
            result.untrashable.append(url.toString());
            break;
        case TrashOutcome::Invalid:
            result.invalid.append(url.toString());
            break;
        }
    };

    switch (scope) {
    case VaultFileScope::Files:
        for (const QUrl &url : urls)
            classify(trashEntry(url, EntryKind::File, vaultLocalPath, &result.restorable), url, QString());
        break;
    case VaultFileScope::Folder:
        // Folder scope expects exactly one url; the folder goes whole. mdPath is
        // meaningless for a folder (empty), same as for Files/Transcript.
        for (const QUrl &url : urls)
            classify(trashEntry(url, EntryKind::Folder, vaultLocalPath, &result.restorable), url, QString());
        break;
    case VaultFileScope::Transcript:
        for (const QUrl &url : urls)
            classify(trashTranscriptEntry(url, vaultLocalPath, &result.restorable), url, QString());
        break;
    case VaultFileScope::AudioOnly:
        for (const QUrl &url : urls) {
            // Each surviving entry's .md, resolved before the audio moves — which
            // signal the entry earns can only be decided later, but the path has
            // to be carried across (mdPath is meaningful ONLY for this scope).
            QString mdPath;
            classify(trashAudioOnlyEntry(url, vaultLocalPath, &result.restorable, &mdPath), url, mdPath);
        }
        break;
    }

    return result;
}

QList<RemovedEntry> deleteVaultEntriesPermanently(const QList<QUrl> &urls, VaultFileScope scope,
                                                  const QString &vaultLocalPath)
{
    QList<RemovedEntry> removed;
    removed.reserve(urls.size());

    switch (scope) {
    case VaultFileScope::Files:
        for (const QUrl &url : urls) {
            if (removeEntryPermanently(url, EntryKind::File, vaultLocalPath))
                removed.append({ url, QString() });
        }
        break;
    case VaultFileScope::Folder:
        for (const QUrl &url : urls) {
            if (removeEntryPermanently(url, EntryKind::Folder, vaultLocalPath))
                removed.append({ url, QString() });
        }
        break;
    case VaultFileScope::Transcript:
        for (const QUrl &url : urls) {
            const QString path = url.toLocalFile();
            const QFileInfo info(path);
            QString mdPath, partialPath;
            if (path.isEmpty() || !info.exists() || !info.isFile() || !pathIsInsideVault(path, vaultLocalPath)
                || !transcriptPathsFor(info, &mdPath, &partialPath))
                continue;
            if (!QFileInfo::exists(mdPath))
                continue;
            if (!removePermanently(mdPath))
                continue;
            removePermanently(partialPath); // best-effort
            removed.append({ url, QString() });
        }
        break;
    case VaultFileScope::AudioOnly:
        for (const QUrl &url : urls) {
            const QString path = guardedLocalPath(url, EntryKind::File, vaultLocalPath);
            const QFileInfo info(path);
            if (path.isEmpty() || !isAudioFile(info))
                continue;

            QString mdPath, partialPath;
            transcriptPathsFor(info, &mdPath, &partialPath);

            if (!removePermanently(path))
                continue;
            removePermanently(partialPath); // best-effort; mdPath is left alone on purpose
            removed.append({ url, mdPath });
        }
        break;
    }

    return removed;
}

QStringList restoreVaultEntries(const QVariantList &entries, const QString &vaultLocalPath)
{
    QStringList restoredPaths;
    for (const QVariant &v : entries) {
        const QVariantMap entry = v.toMap();
        const QString trashPath = entry.value(QStringLiteral("trashPath")).toString();
        const QString originalPath = entry.value(QStringLiteral("originalPath")).toString();
        if (trashPath.isEmpty() || originalPath.isEmpty())
            continue;
        // Same containment guard as the delete side — a restore must not be a
        // way to write outside the vault, however the entry list got here.
        if (!pathIsInsideVault(originalPath, vaultLocalPath))
            continue;
        if (!QFileInfo::exists(trashPath))
            continue; // the user emptied the trash in the meantime
        if (QFileInfo::exists(originalPath))
            continue; // something took the name back — never clobber it

        // The original's folder can be gone: undoing a folder delete restores
        // the folder itself first (entries are in trash order, parents before
        // children), but a file whose parent was separately removed needs it
        // recreated or the rename fails.
        QDir().mkpath(QFileInfo(originalPath).absolutePath());
        if (!QFile::rename(trashPath, originalPath))
            continue;

        // Drop the freedesktop info record that moveToTrash() wrote alongside
        // it — see trashInfoPathFor(). Best-effort: a leftover .trashinfo is
        // cosmetic (a phantom row in the desktop's trash UI), never a reason to
        // report the restore itself as failed.
        const QString infoPath = trashInfoPathFor(trashPath);
        if (!infoPath.isEmpty())
            QFile::remove(infoPath);

        restoredPaths << originalPath;
    }
    return restoredPaths;
}
