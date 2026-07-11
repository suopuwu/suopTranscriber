#pragma once

#include <QAbstractItemModel>
#include <QFileSystemWatcher>
#include <QStringList>
#include <QUrl>
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

    // Reveals fileUrl in the desktop's file manager with it selected (Wave D
    // item 2 — BottomBar's current-file path is clickable). Uses the
    // freedesktop.org FileManager1 D-Bus interface's ShowItems method (the
    // standard cross-DE way to select-and-reveal, vs. Qt.openUrlExternally
    // which can only open a folder, never select something inside it).
    // Fire-and-forget: if no file manager implements this interface, this is
    // silently a no-op rather than an error the user needs to handle.
    Q_INVOKABLE void revealInFileManager(const QUrl &fileUrl) const;

    // Item 9 (Wave E): vault-scoped settings file (`.suopTranscriber`, JSON)
    // at the vault root — a hidden dotfile, so it's naturally invisible to
    // rebuild()/scanDirectory()'s scan (QDir's default filters exclude
    // hidden entries; a suffix-less dotfile also never matches the ".md"
    // check even if that changed). Empty map / false+*errorOut if there's no
    // vault open or the file can't be read/written — callers (AppSettings)
    // fall back to defaults in that case.
    Q_INVOKABLE QVariantMap readVaultSettings() const;
    Q_INVOKABLE bool writeVaultSettings(const QVariantMap &settings, QString *errorOut = nullptr) const;

    // Every audio file's absolute path in the vault, transcribed or not —
    // walks the full tree rather than relying on which TreeView rows happen
    // to be realized (Sidebar's untranscribedPaths/transcribedPaths only
    // cover the current viewport). Used by "force retranscribe all".
    Q_INVOKABLE QStringList allAudioPaths() const;

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

    // Backing for LabelRole/TagsRole/TranscriptBodyRole: read fresh from the
    // sidecar .md on every call rather than cached on Node/at rebuild() time,
    // since these are only needed on-demand (search sort/highlight, Item 9)
    // and staying lazy avoids re-parsing every .md file on every directory
    // change the QFileSystemWatcher reports.
    static QString sidecarMdPath(const Node *node);
    static QString frontmatterField(const QString &mdPath, const QString &key);
    static QString transcriptBody(const QString &mdPath);

    QUrl m_vaultPath;
    std::unique_ptr<Node> m_root;
    QFileSystemWatcher m_watcher;
};
