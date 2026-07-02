#include "VaultTreeModel.h"

#include <QDir>
#include <QFileInfo>
#include <QSet>

namespace {

const QSet<QString> &audioExtensions()
{
    static const QSet<QString> exts = {
        "mp3", "wav", "m4a", "flac", "ogg", "opus", "aac", "wma", "aiff", "alac",
    };
    return exts;
}

bool isAudioFile(const QFileInfo &info)
{
    return audioExtensions().contains(info.suffix().toLower());
}

} // namespace

VaultTreeModel::VaultTreeModel(QObject *parent)
    : QAbstractItemModel(parent)
    , m_root(std::make_unique<Node>())
{
    m_root->isDir = true;
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, &VaultTreeModel::rebuild);
}

QUrl VaultTreeModel::vaultPath() const
{
    return m_vaultPath;
}

QString VaultTreeModel::vaultLocalPath() const
{
    return m_vaultPath.toLocalFile();
}

void VaultTreeModel::setVaultPath(const QUrl &url)
{
    if (url == m_vaultPath)
        return;
    m_vaultPath = url;
    emit vaultPathChanged();
    rebuild();
}

void VaultTreeModel::rebuild()
{
    beginResetModel();

    m_root = std::make_unique<Node>();
    m_root->isDir = true;

    if (!m_watcher.directories().isEmpty())
        m_watcher.removePaths(m_watcher.directories());

    const QString rootPath = m_vaultPath.toLocalFile();
    if (!rootPath.isEmpty() && QFileInfo::exists(rootPath)) {
        QStringList watchedDirs;
        watchedDirs << rootPath;

        QDir dir(rootPath);
        const QFileInfoList entries = dir.entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot, QDir::DirsFirst | QDir::Name);
        for (const QFileInfo &entry : entries) {
            if (entry.isDir()) {
                if (auto child = scanDirectory(entry.absoluteFilePath(), m_root.get())) {
                    watchedDirs << child->path;
                    m_root->children.push_back(std::move(child));
                }
            } else if (isAudioFile(entry)) {
                auto leaf = std::make_unique<Node>();
                leaf->name = entry.fileName();
                leaf->path = entry.absoluteFilePath();
                leaf->isAudio = true;
                const QString mdPath = entry.absolutePath() + '/' + entry.completeBaseName() + ".md";
                leaf->status = QFileInfo::exists(mdPath) ? TranscribedStatus : UntranscribedStatus;
                leaf->parent = m_root.get();
                m_root->children.push_back(std::move(leaf));
            }
        }

        m_watcher.addPaths(watchedDirs);
    }

    endResetModel();
}

std::unique_ptr<VaultTreeModel::Node> VaultTreeModel::scanDirectory(const QString &dirPath, Node *parent) const
{
    auto node = std::make_unique<Node>();
    node->name = QFileInfo(dirPath).fileName();
    node->path = dirPath;
    node->isDir = true;
    node->parent = parent;

    bool hasAudio = false;
    QDir dir(dirPath);
    const QFileInfoList entries = dir.entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot, QDir::DirsFirst | QDir::Name);
    for (const QFileInfo &entry : entries) {
        if (entry.isDir()) {
            if (auto child = scanDirectory(entry.absoluteFilePath(), node.get())) {
                node->children.push_back(std::move(child));
                hasAudio = true;
            }
        } else if (isAudioFile(entry)) {
            auto leaf = std::make_unique<Node>();
            leaf->name = entry.fileName();
            leaf->path = entry.absoluteFilePath();
            leaf->isAudio = true;
            const QString mdPath = entry.absolutePath() + '/' + entry.completeBaseName() + ".md";
            leaf->status = QFileInfo::exists(mdPath) ? TranscribedStatus : UntranscribedStatus;
            leaf->parent = node.get();
            node->children.push_back(std::move(leaf));
            hasAudio = true;
        }
    }

    if (!hasAudio)
        return nullptr;
    return node;
}

VaultTreeModel::Node *VaultTreeModel::nodeFromIndex(const QModelIndex &index) const
{
    if (!index.isValid())
        return m_root.get();
    return static_cast<Node *>(index.internalPointer());
}

QModelIndex VaultTreeModel::index(int row, int column, const QModelIndex &parent) const
{
    if (!hasIndex(row, column, parent))
        return {};

    Node *parentNode = nodeFromIndex(parent);
    if (row < 0 || static_cast<size_t>(row) >= parentNode->children.size())
        return {};
    return createIndex(row, column, parentNode->children[row].get());
}

QModelIndex VaultTreeModel::parent(const QModelIndex &child) const
{
    if (!child.isValid())
        return {};

    Node *node = nodeFromIndex(child);
    Node *parentNode = node->parent;
    if (!parentNode || parentNode == m_root.get())
        return {};

    Node *grandparent = parentNode->parent;
    if (!grandparent)
        return {};

    int row = 0;
    for (size_t i = 0; i < grandparent->children.size(); ++i) {
        if (grandparent->children[i].get() == parentNode) {
            row = static_cast<int>(i);
            break;
        }
    }
    return createIndex(row, 0, parentNode);
}

int VaultTreeModel::rowCount(const QModelIndex &parent) const
{
    if (parent.column() > 0)
        return 0;
    return static_cast<int>(nodeFromIndex(parent)->children.size());
}

int VaultTreeModel::columnCount(const QModelIndex &parent) const
{
    Q_UNUSED(parent);
    return 1;
}

QVariant VaultTreeModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid())
        return {};

    const Node *node = static_cast<Node *>(index.internalPointer());
    switch (role) {
    case NameRole:
        return node->name;
    case PathRole:
        return node->path;
    case IsDirRole:
        return node->isDir;
    case IsAudioRole:
        return node->isAudio;
    case StatusRole:
        return static_cast<int>(node->status);
    default:
        return {};
    }
}

QHash<int, QByteArray> VaultTreeModel::roleNames() const
{
    return {
        {NameRole, "name"},
        {PathRole, "path"},
        {IsDirRole, "isDir"},
        {IsAudioRole, "isAudio"},
        {StatusRole, "status"},
    };
}
