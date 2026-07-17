#pragma once

#include <QAbstractItemModel>
#include <QFileSystemWatcher>
#include <QList>
#include <QStringList>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>
#include <qqml.h>

#include <memory>
#include <vector>

// Sidebar tree over a vault root. Every directory is shown, including empty
// ones and ones holding only unrelated files — the tree mirrors the vault's
// real folder structure so a folder you make is somewhere you can see and
// drop into. Only leaf FILES are filtered: audio, plus orphaned .md files
// (a .md with no audio sibling). Each audio file's status reflects whether a
// sibling .md exists yet (see DESIGN.md "Vault / file layout").
class VaultTreeModel : public QAbstractItemModel {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QUrl vaultPath READ vaultPath WRITE setVaultPath NOTIFY vaultPathChanged)
    Q_PROPERTY(QString vaultLocalPath READ vaultLocalPath NOTIFY vaultPathChanged)

public:
    enum Roles {
        NameRole = Qt::UserRole + 1,
        PathRole,
        IsDirRole,
        IsAudioRole,
        StatusRole,
        IsOrphanedMdRole,
        LabelRole,
        TagsRole,
        SpeakersRole,
        TranscriptBodyRole,
    };
    Q_ENUM(Roles)

    enum Status {
        NoneStatus = 0,
        UntranscribedStatus = 1,
        TranscribedStatus = 2,
        OrphanedMdStatus = 3,
    };
    Q_ENUM(Status)

    explicit VaultTreeModel(QObject *parent = nullptr);

    QUrl vaultPath() const;
    void setVaultPath(const QUrl &url);
    QString vaultLocalPath() const;

    QModelIndex index(int row, int column, const QModelIndex &parent = {}) const override;
    QModelIndex parent(const QModelIndex &child) const override;
    int rowCount(const QModelIndex &parent = {}) const override;
    int columnCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    // Renames the audio file at fileUrl (and its sibling .md, if any) to
    // newBaseName, preserving each file's extension. Also handles orphaned
    // .md-only nodes (Feature: Milestone 5 item 19 "forgo labels" mode).
    // Returns false and sets *errorOut on validation or filesystem failure,
    // without leaving the audio/.md pair mismatched.
    Q_INVOKABLE bool renameFile(const QUrl &fileUrl, const QString &newBaseName, QString *errorOut = nullptr);

    // Item 37: synchronous, cheap VALIDATOR for a drag-and-drop / external
    // audio import — the actual byte-copy is now an async worker-thread job in
    // TranscriptionController (see importAudio()), so the freeze from a large
    // blocking QFile::copy on the GUI thread is gone. This half stays on the
    // GUI thread so the user gets IMMEDIATE feedback for the fast failure
    // modes (wrong type / collision / no vault) before anything is enqueued.
    // Returns the absolute destination path the file WOULD be copied to (the
    // vault root — never a subfolder, regardless of where in the window the
    // drop landed; a copy, not a move, since the source may be on removable
    // media, mirroring DESIGN.md's "USB import" "copy only new audio in"), or
    // an empty string (with *errorOut set) if fileUrl isn't a recognized audio
    // extension, a same-named file already exists at the destination, or no
    // vault is open. The caller (Main.qml) hands a non-empty result plus the
    // source url to TranscriptionController::importAudio() to do the copy off
    // the GUI thread as a tracked task.
    Q_INVOKABLE QString importDestinationFor(const QUrl &fileUrl, QString *errorOut = nullptr) const;

    // Item 40 (folder management): creates a new subfolder named `name`
    // inside parentDirUrl (QDir::mkpath). Rejects an empty/whitespace-only
    // name, a name containing a path separator or other filesystem-invalid
    // character (same character set renameFile() rejects), a parentDir that
    // isn't inside the open vault, and a name that would collide with an
    // existing entry — returning false rather than clobbering or escaping the
    // vault. rebuild()s on success so the watcher starts watching the new
    // folder and the (still empty) folder appears in the tree right away.
    Q_INVOKABLE bool createFolder(const QUrl &parentDirUrl, const QString &name);

    // Item 40: moves a single file (QFile::rename) into destDirUrl. When the
    // moved file is an AUDIO file it carries its sibling transcript data too —
    // the <fileName>.md sidecar (same full-filename convention as
    // sidecarMdPath()/renameFile(), NOT a stripped stem — see item P7) and, if
    // present, the .<fileName>.partial.jsonl resume-state file (CLAUDE.md
    // vault layout). A standalone orphaned .md (or any non-audio file) moves
    // on its own. Rejects a move whose source/destination escapes the vault,
    // a destination equal to the file's current directory (no-op), or any
    // destination collision (checked across the whole audio+sidecars set
    // before anything moves, so the pair is never left split across folders);
    // rolls back a partial move on failure. rebuild()s on success.
    Q_INVOKABLE bool moveFile(const QUrl &fileUrl, const QUrl &destDirUrl);

    // Inline rename: renames folderUrl in place, keeping it in its current
    // parent (moveFolder() below owns reparenting — this never changes where
    // the folder sits). Guarded like deleteFolder(): refuses the vault root
    // itself, anything outside the vault, a name that's empty/whitespace-only
    // or holds a filesystem-invalid character (the shared isValidEntryName()
    // check createFolder() uses — path separators included, so a name can
    // never escape its parent), and a collision with an existing entry.
    // Renaming to the name it already has is a no-op that returns true, not a
    // collision failure. Returns false and sets *errorOut otherwise.
    Q_INVOKABLE bool renameFolder(const QUrl &folderUrl, const QString &newName, QString *errorOut = nullptr);

    // Item 40: moves/renames a folder into destParentUrl (QDir::rename).
    // Guarded like deleteFolder() (never the vault root, never outside the
    // vault) plus: refuses to move a folder into itself or one of its own
    // descendants, and refuses a name collision at the destination. rebuild()s
    // on success.
    Q_INVOKABLE bool moveFolder(const QUrl &folderUrl, const QUrl &destParentUrl);

    // ---- Deletion: two explicitly separate paths, recoverable vs not --------
    //
    // The old deleteFile()/deleteFiles()/deleteFolder()/deleteTranscript() each
    // did "trash, or permanently remove if trashing fails" behind one call, so
    // the UI could never tell which of the two had happened and had to
    // confirm-then-delete on the assumption it might be permanent. They're
    // replaced by a trash-only attempt and an explicit permanent delete.
    //
    // What makes that split work is that a FAILED QFile::moveToTrash() is
    // non-destructive — it leaves the original fully intact (verified
    // empirically; see trashOnly()'s comment in the .cpp). So the UI never has
    // to PREDICT trash-ability: it calls trashFiles()/trashFolder() first, and only
    // the entries that come back "untrashable" (still on disk, untouched) need a
    // confirmation prompt, because only those would be irreversible. See
    // Sidebar.qml's requestEntryDelete().
    //
    // Naming, deliberately: these are NOT called deleteFiles() any more.
    // Repurposing that name would have silently turned every existing caller's
    // recoverable delete into a permanent one.

    // Trashes files and/or folders — one call for a whole selection, with a
    // SINGLE rebuild() for the batch rather than one per entry (rebuild()
    // rescans the vault recursively AND resets the model, so that cost is paid
    // once per user action). Per entry:
    //   - an AUDIO file carries its sidecars off with it (<fileName>.md and
    //     .<fileName>.partial.jsonl, whichever exist — same full-filename
    //     convention as sidecarMdPath()/moveFile());
    //   - an orphaned .md (or any non-audio file) goes on its own;
    //   - a FOLDER goes whole (moveToTrash takes the tree intact), guarded
    //     against the vault root itself.
    // Every entry is guarded against escaping the vault, and a guard failure
    // skips that entry rather than aborting the batch, so one bad entry can't
    // strand the valid ones. Emits fileDeleted() once per file actually
    // trashed, after the rebuild.
    //
    // Returns {
    //   "trashed":     int   — how many of fileUrls went to the trash,
    //   "restorable":  list of {"trashPath","originalPath"} — EVERY path
    //                  trashed, sidecars included; hand this straight back to
    //                  restoreFromTrash() to undo. Nothing else can reconstruct
    //                  it: the trash path is only knowable at trash time.
    //   "untrashable": list of url strings — the filesystem refused; these are
    //                  STILL ON DISK, untouched. Only these need confirming.
    //   "invalid":     list of url strings — failed a guard / didn't exist.
    // }
    Q_INVOKABLE QVariantMap trashFiles(const QList<QUrl> &urls);

    // Irreversible. Only ever called after the user confirms, and in practice
    // only for the "untrashable" entries trashFiles() handed back. Same
    // guards and same single-rebuild batching; returns how many were removed.
    Q_INVOKABLE int deleteFilesPermanently(const QList<QUrl> &urls);

    // Item 40: trashes folderUrl and everything under it (moveToTrash takes the
    // tree intact), same result map as trashFiles(). A SEPARATE call from
    // trashFiles() on purpose, not for tidiness: a folder delete is recursive
    // and takes every transcript under it, so it must never be reachable from
    // the call that handles a file SELECTION — one stray folder path leaking
    // into a multi-select would otherwise destroy a whole tree silently.
    // Guarded: refuses the vault root itself, and anything not strictly inside
    // the vault. rebuild()s when something was trashed (dropping the folder from
    // the watched set).
    Q_INVOKABLE QVariantMap trashFolder(const QUrl &folderUrl);

    // Irreversible counterpart to trashFolder(), same confirm-first flow.
    Q_INVOKABLE bool deleteFolderPermanently(const QUrl &folderUrl);

    // Item 2 (delete transcript): trashes an audio file's sibling transcript —
    // the <fileName>.md sidecar plus, if present, its .<fileName>.partial.jsonl
    // resume-state file — reverting the audio to "untranscribed" (yellow) while
    // KEEPING the audio itself. When fileUrl names an orphaned .md directly,
    // that .md IS the transcript, so it's the thing removed. Same result map as
    // trashFiles(); a no-op with trashed == 0 if there's no sidecar .md.
    // rebuild()s + emits transcriptDeleted(fileUrl) when something was trashed.
    Q_INVOKABLE QVariantMap trashTranscript(const QUrl &fileUrl);

    // Irreversible counterpart to trashTranscript(), same confirm-first flow.
    Q_INVOKABLE bool deleteTranscriptPermanently(const QUrl &fileUrl);

    // The exact complement of trashTranscript(): trashes an audio file but
    // DELIBERATELY LEAVES its <fileName>.md behind, where trashFiles() would
    // carry the .md off with it. The .md that survives isn't a stranded file —
    // scanDirectory() classifies a .md with no sibling audio as an orphaned .md
    // (OrphanedMdStatus), so the row simply changes kind in the rebuild. That's
    // the whole point of the action: keep the words, drop the audio.
    //
    // The .partial.jsonl DOES go with the audio. It's resume state for
    // transcribing THIS audio and means nothing once the audio is gone — the
    // .md is the durable artifact, the partial is scaffolding.
    //
    // fileUrl must name an audio file: on an orphaned .md "delete the audio,
    // keep the transcript" has no referent, so that's Invalid rather than a
    // silent no-op. Same result map as trashFiles(); rebuild()s + emits
    // fileDeleted(fileUrl) when the audio was trashed.
    Q_INVOKABLE QVariantMap trashAudioOnly(const QUrl &fileUrl);

    // Irreversible counterpart to trashAudioOnly(), same confirm-first flow.
    Q_INVOKABLE bool deleteAudioOnlyPermanently(const QUrl &fileUrl);

    // Undo: moves each {"trashPath","originalPath"} entry back where it came
    // from. Takes "restorable" from any trash* result
    // verbatim. Per entry it also deletes the freedesktop .trashinfo record
    // that moveToTrash() wrote — Qt has no restore API, and renaming only the
    // file back would orphan that record and leave a phantom row in the
    // desktop's trash UI.
    //
    // Skips (rather than fails) an entry whose original path is occupied again,
    // whose trashed file is gone (the user emptied the trash), or which would
    // land outside the vault. Recreates the original's parent directory if it
    // has since disappeared. rebuild()s if anything came back. Returns the
    // number restored — callers compare against entries.size() for partial
    // failure.
    Q_INVOKABLE int restoreFromTrash(const QVariantList &entries);

    // Item 40: every real directory inside the vault (walks the filesystem
    // directly, NOT the pruned tree — a freshly-created empty folder must be a
    // valid move destination even though it's hidden from the tree until it
    // has content), vault root first, then subfolders depth-first by name.
    // Backs Sidebar.qml's "Move to…" destination picker. Hidden dotdirs
    // (.transcriber) are excluded (QDir::Dirs skips hidden entries).
    Q_INVOKABLE QStringList vaultFolders() const;

    // Item 9 (Wave E): vault-scoped settings file (`.transcriber/settings.json`,
    // JSON) — lives inside the hidden `.transcriber/` config dir, so it's
    // naturally invisible to rebuild()/scanDirectory()'s scan (QDir's default
    // filters exclude hidden entries). writeVaultSettings() creates
    // `.transcriber/` if missing. Empty map / false+*errorOut if there's no
    // vault open or the file can't be read/written — callers (AppSettings)
    // fall back to defaults in that case.
    Q_INVOKABLE QVariantMap readVaultSettings() const;
    Q_INVOKABLE bool writeVaultSettings(const QVariantMap &settings, QString *errorOut = nullptr) const;

    // USB Grabber: vault-scoped historical-grabs log
    // (`.transcriber/usbGrabHistory.json`) — same `.transcriber/`-creates-if-
    // missing behavior as readVaultSettings()/writeVaultSettings() above, but
    // the JSON root is an array (one entry per past grab operation) rather
    // than an object. Empty list if there's no vault open or the file
    // doesn't exist yet (not an error — a vault simply hasn't had a grab
    // performed against it yet).
    Q_INVOKABLE QVariantList readUsbGrabHistory() const;

    // Appends one entry to the history array and rewrites the whole file
    // (read-whole-array, append, rewrite-whole-file — same whole-file-rewrite
    // shape as writeVaultSettings(), fine at this scale). entry is persisted
    // as given, without strict shape validation (same permissive/passthrough
    // convention as writeVaultSettings()); callers are expected to supply
    // timestamp/deviceSerial/deviceLabel/filesCopied/filesSkipped/errors.
    Q_INVOKABLE bool appendUsbGrabHistoryEntry(const QVariantMap &entry, QString *errorOut = nullptr) const;

    // Model-benchmark last-run cache (`.transcriber/benchmarkLastRun.json`) —
    // same `.transcriber/`-creates-if-missing behavior as the settings/history
    // files above, JSON root an object. Unlike the grab history this is
    // OVERWRITTEN each run (only the most recent benchmark is kept, per the
    // feature spec). Empty map if no vault is open or no benchmark has run yet.
    // The benchmark itself writes no transcripts — this cache is its only
    // on-disk footprint.
    Q_INVOKABLE QVariantMap readBenchmarkLastRun() const;
    Q_INVOKABLE bool writeBenchmarkLastRun(const QVariantMap &run, QString *errorOut = nullptr) const;

    // Every audio file's absolute path in the vault, transcribed or not —
    // walks the full tree rather than relying on which TreeView rows happen
    // to be realized (Sidebar's untranscribedPaths/transcribedPaths only
    // cover the current viewport). Used by "force retranscribe all".
    Q_INVOKABLE QStringList allAudioPaths() const;

    // Same traversal as allAudioPaths(), paired with each file's mtime —
    // Sidebar.qml's file-tree sort feature (Name / Date Modified asc/desc)
    // needs raw per-file mtimes to build its sort keys, and QML has no direct
    // filesystem access of its own. Keys are absolute paths (same strings
    // allAudioPaths() returns), values are QFileInfo::lastModified()'s
    // toMSecsSinceEpoch() (a double once crossed into JS, but well within
    // Number.MAX_SAFE_INTEGER for any realistic timestamp).
    Q_INVOKABLE QVariantMap audioLastModified() const;

    // Search UI rearchitecture: flat, already-computed search results for
    // Sidebar.qml's separate results ListView (shown instead of the normal
    // TreeView while a text search query is active) — as opposed to
    // VaultSearchProxyModel, which stays exactly as-is and still does
    // in-tree dim+sort for this same query AND genuine row-hiding for the
    // separate tagFilter feature (unrelated; not touched here).
    //
    // Recursively walks the whole tree (mirrors allAudioPaths()'s traversal
    // shape) and returns one QVariantMap per matching leaf node (audio file
    // or orphaned .md — directories are never results, they're not
    // clickable-to-open), sorted alphabetically by name. A leaf matches if
    // query (case-insensitive) is found in its filename, stored label, or
    // stored tags — or, only when includeTranscriptions is true and the
    // file is transcribed, in its transcript body.
    //
    // Deliberately returns RAW transcriptBody rather than a pre-built
    // snippet string: Sidebar.qml already has cleanTranscript()/
    // transcriptSnippet()/highlightMatch() for exactly this (used today by
    // the in-tree snippet row), and re-implementing that text-shaping logic
    // here in C++ would just duplicate it. QML calls those existing helpers
    // on each result's transcriptBody the same way the tree delegate already
    // does on its own TranscriptBodyRole value.
    //
    // Each entry's keys: path (absolute), name (filename), label (stored
    // label, or "" if none/untranscribed), status (VaultTreeModel::Status
    // int), isAudio, isOrphanedMd, transcriptBody (raw body, or "" if
    // includeTranscriptions is false or the file isn't transcribed).
    Q_INVOKABLE QVariantList searchResults(const QString &query, bool includeTranscriptions) const;

    // Recording feature: validates a user-chosen (or about-to-be-created)
    // output folder actually lives inside this vault, before it's persisted
    // to AppSettings.recordingOutputFolder. True iff candidatePath IS the
    // vault root or a subdirectory of it; false for anything outside the
    // vault root, including paths that use ".." to escape it. candidatePath
    // need not exist yet (e.g. a folder the user is about to create) — only
    // the vault root itself must exist.
    Q_INVOKABLE bool isPathInsideVault(const QString &candidatePath) const;

    // Bug fix: "label always delayed by one file in the file explorer".
    // rebuild() only ever runs from QFileSystemWatcher::directoryChanged,
    // which fires on a directory's file add/remove/rename but NEVER on an
    // in-place content rewrite of a file that already existed. Auto-label
    // (TranscriptionController::writeLabelingFields()), "generate chapters"
    // (writeChaptersField()), speaker rename/merge
    // (renameSpeakerInFile()/mergeSpeakersInFile()), and the manual
    // setLabel()/setTags()/setPrelude()/setSentenceTexts() editors all
    // rewrite an ALREADY-EXISTING sidecar .md in place — the .md's
    // directory-level existence never changes, so none of those writes
    // trigger a watcher event, and this row's LabelRole/TagsRole/etc. (read
    // fresh from disk on every data() call — see frontmatterField()'s doc
    // comment; nothing here is cached except TranscriptBodyRole) sit
    // correctly updated on disk but the view never re-queries them until
    // some LATER, unrelated directory event (typically the NEXT file's .md
    // being created) happens to force a full rebuild(). That's the "delayed
    // by one file" symptom (see Item P19's comment on
    // TranscriptionController::setLabel()).
    //
    // TranscriptionController::finished(fileUrl, ok, error) already fires
    // right after every one of those rewrites lands on disk (both from the
    // queued job pipeline's onWorkerFinished() and directly from the
    // synchronous const setters) — this method is the cheap, single-row
    // counterpart a `finished` handler is expected to call: it locates
    // fileUrl's row (fileUrl may name the audio file OR a standalone
    // orphaned .md — either way, whichever this Node's own path is) and
    // emits dataChanged() for just that row, without rescanning the
    // directory or resetting the whole model. A no-op if fileUrl doesn't
    // resolve to any node currently in the tree (e.g. a stale signal for a
    // file that's since been deleted/renamed, or one outside the vault).
    Q_INVOKABLE void refreshFile(const QUrl &fileUrl);

signals:
    void vaultPathChanged();
    // Item 2 (delete): a transcript (.md sidecar) was removed by
    // deleteTranscript(), leaving the audio in place (fileUrl is the audio
    // file whose transcript was deleted, or the orphaned .md that was
    // removed). MainPanel listens so the open file, if it's this one, reverts
    // to its untranscribed view rather than showing stale transcript data.
    void transcriptDeleted(const QUrl &fileUrl);
    // Item 2 (delete): a whole file (audio + sidecars, or an orphaned .md)
    // was removed by deleteFile(). MainPanel listens so it clears/closes the
    // view when the deleted file is the one currently open.
    void fileDeleted(const QUrl &fileUrl);
    // trashAudioOnly() removed `fileUrl`'s audio but its transcript SURVIVED,
    // and now stands alone at `transcriptUrl` as an orphaned .md.
    //
    // Deliberately NOT fileDeleted: that means "this is gone, drop it", and a
    // listener obeying it closes the view — which for this action throws away
    // the very transcript the user deleted the audio to keep. The open file
    // doesn't disappear here, it CHANGES KIND (audio row -> orphaned-.md row),
    // and no listener could work that out from a bare "the audio is gone" —
    // hence the second argument. MainPanel listens and follows the file over.
    //
    // Emitted INSTEAD OF fileDeleted, never alongside it: two signals for one
    // delete would leave the view's final state at the mercy of emission
    // order (fileDeleted's handler clears exactly what this one restores).
    // An audio file with no transcript to survive emits plain fileDeleted.
    void audioDeletedTranscriptKept(const QUrl &fileUrl, const QUrl &transcriptUrl);
    // restoreFromTrash() brought `fileUrl`'s audio back, and its transcript is
    // still standing at `transcriptUrl` — the exact inverse of
    // audioDeletedTranscriptKept, and emitted for the same reason: no listener
    // could work out from a bare "a file came back" that the row it is showing
    // as an orphaned .md just became an audio row again. MainPanel listens and
    // follows the file back, so undoing the delete lands it where it started
    // instead of stranding it in a text-only view of a file that has its audio.
    void audioRestoredForTranscript(const QUrl &fileUrl, const QUrl &transcriptUrl);
    // restoreFromTrash() brought a whole file back — the inverse of fileDeleted.
    // The one restore signal that must say what KIND of file it is: fileDeleted's
    // listener only had to close the view, which needs no kind, but REOPENING
    // does (the audio player view, or the text-only orphaned-.md one). True when
    // fileUrl is a standalone .md with no sibling audio — i.e. the row is the
    // transcript itself.
    void fileRestored(const QUrl &fileUrl, bool isOrphanedMd);
    // restoreFromTrash() brought a transcript back — the inverse of
    // transcriptDeleted, and `fileUrl` follows that signal's convention: the
    // audio file the transcript belongs to, or the .md itself when it stands
    // alone. MainPanel listens so the open file stops rendering as
    // untranscribed the moment its transcript is un-deleted.
    void transcriptRestored(const QUrl &fileUrl);

private:
    struct Node {
        QString name;
        QString path;
        bool isDir = false;
        bool isAudio = false;
        bool isOrphanedMd = false;
        Status status = NoneStatus;
        Node *parent = nullptr;
        std::vector<std::unique_ptr<Node>> children;
    };

    void rebuild();

    // What became of one entry in a batch. Untrashable is emphatically NOT a
    // failure: the entry is still on disk, untouched, and is exactly what the
    // UI must ask the user to confirm a permanent delete for.
    enum class TrashOutcome { Trashed, Untrashable, Invalid };

    // Files and folders are handled by separate public calls on purpose (see
    // guardedLocalPath()): a folder delete is recursive, so it must never be
    // reachable from the file-selection path.
    enum class EntryKind { File, Folder };

    // Ends an audio-only delete on the right signal — audioDeletedTranscriptKept
    // if `mdPath` survived, plain fileDeleted if there was nothing to keep.
    void emitAudioOnlyDeleted(const QUrl &fileUrl, const QString &mdPath);

    // Announces a batch of restored paths in the same terms the delete signals
    // used to announce their removal, so a listener that followed the delete can
    // follow the undo. Takes the whole batch, not one path: the batch is the only
    // record of WHICH delete is being undone — see the definition.
    void emitRestored(const QStringList &originalPaths);

    // The guards + trash of ONE entry (file or folder), WITHOUT the rebuild()/
    // fileDeleted() that must happen once for a whole batch — shared by
    // trashFiles()/trashFolder()/trashTranscript(). Appends a {trashPath, originalPath}
    // map to *restorable for every path it actually trashed, sidecars included.
    TrashOutcome trashEntry(const QUrl &url, EntryKind kind, QVariantList *restorable);
    // Irreversible counterpart, same guards and same sidecar set.
    bool removeEntryPermanently(const QUrl &url, EntryKind kind);
    // Shared guard set for both of the above: inside the vault, never the vault
    // root itself. Returns the resolved local path, or empty if refused.
    QString guardedLocalPath(const QUrl &url, EntryKind kind) const;
    std::unique_ptr<Node> scanDirectory(const QString &dirPath, Node *parent) const;
    Node *nodeFromIndex(const QModelIndex &index) const;
    // Backs refreshFile() — plain depth-first search by absolute path (the
    // tree has no path->Node lookup table; this stays cheap because it's
    // only called once per finished()/setLabel()-style event, not per
    // frame).
    static Node *findNodeByPath(Node *node, const QString &path);
    // Backs refreshFile() — the inverse of nodeFromIndex(): finds node's row
    // within its own parent's children and builds the matching QModelIndex.
    // Returns an invalid index for the root node (no parent) or an orphaned
    // node no longer reachable from m_root (shouldn't happen; defensive).
    QModelIndex indexForNode(const Node *node) const;
    static void collectAudioPaths(const Node *node, QStringList &out);
    static void collectAudioLastModified(const Node *node, QVariantMap &out);
    // Backs vaultFolders() — a raw filesystem walk (not a tree walk), so
    // empty/content-less folders the tree prunes are still listed as move
    // destinations.
    static void collectFolderPaths(const QString &dirPath, QStringList &out);
    // Backs searchResults() — not static (unlike collectAudioPaths above)
    // because it needs transcriptBody(), which consults m_transcriptBodyCache.
    void collectSearchResults(const Node *node, const QString &queryLower, bool includeTranscriptions,
                               QVariantList &out) const;

    // Backing for LabelRole/TagsRole/TranscriptBodyRole: read lazily from the
    // sidecar .md rather than cached on Node/at rebuild() time, since these
    // are only needed on-demand (search sort/highlight, Item 9) and staying
    // lazy avoids re-parsing every .md file on every directory change the
    // QFileSystemWatcher reports.
    static QString sidecarMdPath(const Node *node);
    // Cached per mdPath by mtime+size, exactly like transcriptBody() below —
    // this read is on far hotter paths than the body one and was the last
    // uncached file open left in data().
    //
    // It used to re-open + re-scan the sidecar on EVERY call, and data()
    // calls it once each for LabelRole/TagsRole/SpeakersRole — three separate
    // opens per row. VaultSearchProxyModel::matchesDirect() reads two of
    // those roles per row, inside lessThan()/filterAcceptsRow(), i.e. inside
    // an O(n log n) sort that setSearchQuery()/setTagFilter()/
    // setFileSortKeys() invalidate on every search keystroke — and
    // matchesSearch() recurses over a directory's whole subtree, re-opening
    // files at every level. So one keystroke was O(vault files x log n x 3)
    // synchronous file opens on the GUI thread.
    //
    // The cache holds the WHOLE parsed frontmatter map, not one key, so the
    // three roles now share a single parse instead of forcing three. Same
    // mtime+size correctness argument as transcriptBody(): an in-place
    // relabel/retag rewrite fires no watcher signal, but does change
    // mtime/size, which is checked on every call regardless of trigger.
    QString frontmatterField(const QString &mdPath, const QString &key) const;
    // Item P12: transcriptBody() backs search's transcript-content matching
    // (VaultSearchProxyModel::matchesDirect) — before this cache, every
    // keystroke re-opened and re-read every transcribed file's ENTIRE body
    // from disk, for every candidate row, on every filter pass. Now a
    // non-static const member (was static) so it can consult/populate
    // m_transcriptBodyCache. Cache correctness relies purely on the
    // mtime+size check below, NOT on any QFileSystemWatcher hook — an
    // in-place frontmatter/body rewrite (e.g. a relabel) changes neither the
    // directory listing nor triggers rebuild() (see item P19's finding on
    // this), but DOES change the file's mtime/size, which this cache checks
    // on every call regardless of what triggered it.
    QString transcriptBody(const QString &mdPath) const;

    struct CachedBody {
        qint64 mtimeMs = 0;
        qint64 size = -1;
        QString body;
    };

    // All of a sidecar's frontmatter keys, parsed in one pass — see
    // frontmatterField()'s doc comment.
    struct CachedFrontmatter {
        qint64 mtimeMs = 0;
        qint64 size = -1;
        QHash<QString, QString> fields;
    };

    QUrl m_vaultPath;
    std::unique_ptr<Node> m_root;
    QFileSystemWatcher m_watcher;
    // Item P12: keyed by sidecar .md path — see transcriptBody()'s doc
    // comment. mutable so transcriptBody() can stay callable from data()
    // const.
    mutable QHash<QString, CachedBody> m_transcriptBodyCache;
    // Keyed by sidecar .md path — see frontmatterField()'s doc comment.
    // mutable for the same reason as m_transcriptBodyCache.
    mutable QHash<QString, CachedFrontmatter> m_frontmatterCache;
};
