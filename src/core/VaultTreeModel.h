#pragma once

#include <QAbstractItemModel>
#include <QFileSystemWatcher>
#include <QStringList>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>
#include <qqml.h>

#include <memory>
#include <vector>

// Sidebar tree over a vault root: directories that contain no audio anywhere
// in their subtree are hidden. Each audio file's status reflects whether a
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

    // Copies an external audio file (e.g. dragged in from the desktop) into
    // the vault root — never a subfolder, regardless of where in the window
    // the drop happened. A copy, not a move: the source may be on removable
    // media or otherwise not the user's to lose (mirrors DESIGN.md's "USB
    // import" language, "copy only new audio in"). Refuses (rather than
    // silently overwriting or auto-renaming) if a file with the same name
    // already exists at the destination, or if fileUrl isn't a recognized
    // audio extension. Returns false and sets *errorOut on any failure.
    Q_INVOKABLE bool importFile(const QUrl &fileUrl, QString *errorOut = nullptr);

    // Reveals fileUrl in the desktop's file manager with it selected (Wave D
    // item 2 — BottomBar's current-file path is clickable). Uses the
    // freedesktop.org FileManager1 D-Bus interface's ShowItems method (the
    // standard cross-DE way to select-and-reveal, vs. Qt.openUrlExternally
    // which can only open a folder, never select something inside it).
    // Fire-and-forget: if no file manager implements this interface, this is
    // silently a no-op rather than an error the user needs to handle.
    Q_INVOKABLE void revealInFileManager(const QUrl &fileUrl) const;

    // Item 9 (Wave E): vault-scoped settings file (`.transcriber/settings.json`,
    // JSON) — lives inside the hidden `.transcriber/` config dir, so it's
    // naturally invisible to rebuild()/scanDirectory()'s scan (QDir's default
    // filters exclude hidden entries). writeVaultSettings() creates
    // `.transcriber/` if missing. Empty map / false+*errorOut if there's no
    // vault open or the file can't be read/written — callers (AppSettings)
    // fall back to defaults in that case.
    Q_INVOKABLE QVariantMap readVaultSettings() const;
    Q_INVOKABLE bool writeVaultSettings(const QVariantMap &settings, QString *errorOut = nullptr) const;

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

signals:
    void vaultPathChanged();

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
    std::unique_ptr<Node> scanDirectory(const QString &dirPath, Node *parent) const;
    Node *nodeFromIndex(const QModelIndex &index) const;
    static void collectAudioPaths(const Node *node, QStringList &out);
    static void collectAudioLastModified(const Node *node, QVariantMap &out);
    // Backs searchResults() — not static (unlike collectAudioPaths above)
    // because it needs transcriptBody(), which consults m_transcriptBodyCache.
    void collectSearchResults(const Node *node, const QString &queryLower, bool includeTranscriptions,
                               QVariantList &out) const;

    // Backing for LabelRole/TagsRole/TranscriptBodyRole: read fresh from the
    // sidecar .md on every call rather than cached on Node/at rebuild() time,
    // since these are only needed on-demand (search sort/highlight, Item 9)
    // and staying lazy avoids re-parsing every .md file on every directory
    // change the QFileSystemWatcher reports.
    static QString sidecarMdPath(const Node *node);
    static QString frontmatterField(const QString &mdPath, const QString &key);
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

    QUrl m_vaultPath;
    std::unique_ptr<Node> m_root;
    QFileSystemWatcher m_watcher;
    // Item P12: keyed by sidecar .md path — see transcriptBody()'s doc
    // comment. mutable so transcriptBody() can stay callable from data()
    // const.
    mutable QHash<QString, CachedBody> m_transcriptBodyCache;
};
