#include "VaultTreeModel.h"

#include <QDir>
#include <QFile>
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

        // First pass: collect audio base names so the second pass can tell
        // whether a .md file is orphaned (no matching audio sibling) without
        // re-scanning the directory.
        QSet<QString> audioBaseNames;
        for (const QFileInfo &entry : entries) {
            if (!entry.isDir() && isAudioFile(entry))
                audioBaseNames.insert(entry.completeBaseName());
        }

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
            } else if (entry.suffix().toLower() == "md" && !audioBaseNames.contains(entry.completeBaseName())) {
                auto leaf = std::make_unique<Node>();
                leaf->name = entry.fileName();
                leaf->path = entry.absoluteFilePath();
                leaf->isAudio = false;
                leaf->isOrphanedMd = true;
                leaf->status = OrphanedMdStatus;
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

    // "hasContent" keeps this directory in the tree if it has audio, an
    // orphaned .md leaf, or a subdirectory that itself was kept for either
    // reason — not literally "has audio", so a directory containing only an
    // orphaned .md isn't incorrectly pruned.
    bool hasContent = false;
    QDir dir(dirPath);
    const QFileInfoList entries = dir.entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot, QDir::DirsFirst | QDir::Name);

    // First pass: collect audio base names so the second pass can tell
    // whether a .md file is orphaned (no matching audio sibling) without
    // re-scanning the directory.
    QSet<QString> audioBaseNames;
    for (const QFileInfo &entry : entries) {
        if (!entry.isDir() && isAudioFile(entry))
            audioBaseNames.insert(entry.completeBaseName());
    }

    for (const QFileInfo &entry : entries) {
        if (entry.isDir()) {
            if (auto child = scanDirectory(entry.absoluteFilePath(), node.get())) {
                node->children.push_back(std::move(child));
                hasContent = true;
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
            hasContent = true;
        } else if (entry.suffix().toLower() == "md" && !audioBaseNames.contains(entry.completeBaseName())) {
            auto leaf = std::make_unique<Node>();
            leaf->name = entry.fileName();
            leaf->path = entry.absoluteFilePath();
            leaf->isAudio = false;
            leaf->isOrphanedMd = true;
            leaf->status = OrphanedMdStatus;
            leaf->parent = node.get();
            node->children.push_back(std::move(leaf));
            hasContent = true;
        }
    }

    if (!hasContent)
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
    case IsOrphanedMdRole:
        return node->isOrphanedMd;
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
        {IsOrphanedMdRole, "isOrphanedMd"},
    };
}

bool VaultTreeModel::renameFile(const QUrl &fileUrl, const QString &newBaseName, QString *errorOut)
{
    auto fail = [&](const QString &msg) {
        if (errorOut)
            *errorOut = msg;
        return false;
    };

    if (newBaseName.isEmpty())
        return fail(QStringLiteral("New name must not be empty."));

    const QString trimmed = newBaseName.trimmed();
    if (trimmed.isEmpty())
        return fail(QStringLiteral("New name must not be empty."));

    static const QString invalidChars = QStringLiteral("/\\:*?\"<>|");
    for (const QChar &c : newBaseName) {
        if (invalidChars.contains(c))
            return fail(QStringLiteral("New name contains an invalid character: '%1'").arg(c));
    }

    const QString path = fileUrl.toLocalFile();
    const QFileInfo info(path);
    if (!info.exists())
        return fail(QStringLiteral("File does not exist."));

    const QString dirPath = info.absolutePath();
    const QString baseName = info.completeBaseName();

    // Determine which file(s) participate in the rename: an audio file (and
    // its sibling .md, if present) or a standalone orphaned .md file.
    QString audioOldPath;
    QString audioExt;
    QString mdOldPath;

    if (isAudioFile(info)) {
        audioOldPath = info.absoluteFilePath();
        audioExt = info.suffix();
        const QString candidateMd = dirPath + '/' + baseName + ".md";
        if (QFileInfo::exists(candidateMd))
            mdOldPath = candidateMd;
    } else if (info.suffix().toLower() == "md") {
        mdOldPath = info.absoluteFilePath();
    } else {
        return fail(QStringLiteral("Unsupported file type."));
    }

    const QString newAudioPath = audioOldPath.isEmpty() ? QString() : dirPath + '/' + trimmed + '.' + audioExt;
    const QString newMdPath = mdOldPath.isEmpty() ? QString() : dirPath + '/' + trimmed + ".md";

    if ((!newAudioPath.isEmpty() && QFileInfo::exists(newAudioPath))
        || (!newMdPath.isEmpty() && QFileInfo::exists(newMdPath))) {
        return fail(QStringLiteral("A file with that name already exists."));
    }

    bool audioRenamed = false;
    if (!audioOldPath.isEmpty()) {
        if (!QFile::rename(audioOldPath, newAudioPath))
            return fail(QStringLiteral("Failed to rename audio file."));
        audioRenamed = true;
    }

    if (!mdOldPath.isEmpty()) {
        if (!QFile::rename(mdOldPath, newMdPath)) {
            if (audioRenamed)
                QFile::rename(newAudioPath, audioOldPath); // best-effort rollback
            return fail(QStringLiteral("Failed to rename markdown file; changes rolled back."));
        }
    }

    rebuild();
    return true;
}
