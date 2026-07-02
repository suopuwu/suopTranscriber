#pragma once

#include <QAbstractItemModel>
#include <QFileSystemWatcher>
#include <QUrl>
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

public:
    enum Roles {
        NameRole = Qt::UserRole + 1,
        PathRole,
        IsDirRole,
        IsAudioRole,
        StatusRole,
    };
    Q_ENUM(Roles)

    enum Status {
        NoneStatus = 0,
        UntranscribedStatus = 1,
        TranscribedStatus = 2,
    };
    Q_ENUM(Status)

    explicit VaultTreeModel(QObject *parent = nullptr);

    QUrl vaultPath() const;
    void setVaultPath(const QUrl &url);

    QModelIndex index(int row, int column, const QModelIndex &parent = {}) const override;
    QModelIndex parent(const QModelIndex &child) const override;
    int rowCount(const QModelIndex &parent = {}) const override;
    int columnCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

signals:
    void vaultPathChanged();

private:
    struct Node {
        QString name;
        QString path;
        bool isDir = false;
        bool isAudio = false;
        Status status = NoneStatus;
        Node *parent = nullptr;
        std::vector<std::unique_ptr<Node>> children;
    };

    void rebuild();
    std::unique_ptr<Node> scanDirectory(const QString &dirPath, Node *parent) const;
    Node *nodeFromIndex(const QModelIndex &index) const;

    QUrl m_vaultPath;
    std::unique_ptr<Node> m_root;
    QFileSystemWatcher m_watcher;
};
