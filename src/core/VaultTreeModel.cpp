#include "VaultTreeModel.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QSet>
#include <QTextStream>

#include <algorithm>

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

        // First pass: collect audio FULL filenames (extension included) so
        // the second pass can tell whether a .md file is orphaned (no
        // matching audio sibling) without re-scanning the directory. Item P7:
        // the sidecar convention is <fileName>.md (the full filename, not
        // just the stripped stem) — otherwise two audio files sharing a stem
        // with different extensions (e.g. "meeting.wav" + "meeting.mp3")
        // would collide on one "meeting.md".
        QSet<QString> audioBaseNames;
        for (const QFileInfo &entry : entries) {
            if (!entry.isDir() && isAudioFile(entry))
                audioBaseNames.insert(entry.fileName());
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
                const QString mdPath = entry.absolutePath() + '/' + entry.fileName() + ".md";
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

    // First pass: collect audio FULL filenames (extension included) — see
    // rebuild()'s identical first pass for why (Item P7: sidecar convention
    // is <fileName>.md, not <completeBaseName>.md).
    QSet<QString> audioBaseNames;
    for (const QFileInfo &entry : entries) {
        if (!entry.isDir() && isAudioFile(entry))
            audioBaseNames.insert(entry.fileName());
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
            const QString mdPath = entry.absolutePath() + '/' + entry.fileName() + ".md";
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

QString VaultTreeModel::sidecarMdPath(const Node *node)
{
    if (node->isOrphanedMd)
        return node->path;
    if (node->isAudio) {
        // Item P7: derived from the FULL filename (extension included), not
        // just the stripped stem — otherwise two audio files sharing a stem
        // with different extensions would collide on one sidecar .md.
        const QFileInfo info(node->path);
        return info.absolutePath() + '/' + info.fileName() + ".md";
    }
    return {};
}

// Mirrors TranscriptionController::readMetadata()'s frontmatter scan, but
// reads a single key rather than the whole map, since LabelRole/TagsRole
// only ever need one field each.
QString VaultTreeModel::frontmatterField(const QString &mdPath, const QString &key)
{
    QFile mdFile(mdPath);
    if (!mdFile.open(QIODevice::ReadOnly | QIODevice::Text))
        return {};

    QTextStream in(&mdFile);
    if (in.readLine() != QStringLiteral("---"))
        return {};

    QString line;
    while (!(line = in.readLine()).isNull() && line != QStringLiteral("---")) {
        const int colon = line.indexOf(':');
        if (colon < 0 || line.left(colon).trimmed() != key)
            continue;
        return line.mid(colon + 1).trimmed();
    }
    return {};
}

// Mirrors TranscriptionController::readTranscriptBody(). Item P12: cached
// per mdPath, invalidated purely by mtime+size (see m_transcriptBodyCache's
// doc comment) rather than by any file-change signal.
QString VaultTreeModel::transcriptBody(const QString &mdPath) const
{
    const QFileInfo info(mdPath);
    const qint64 mtimeMs = info.lastModified().toMSecsSinceEpoch();
    const qint64 size = info.size();

    const auto cached = m_transcriptBodyCache.constFind(mdPath);
    if (cached != m_transcriptBodyCache.constEnd() && cached->mtimeMs == mtimeMs && cached->size == size)
        return cached->body;

    QFile mdFile(mdPath);
    if (!mdFile.open(QIODevice::ReadOnly | QIODevice::Text))
        return {};

    QTextStream in(&mdFile);
    if (in.readLine() != QStringLiteral("---"))
        return {};

    QString line;
    while (!(line = in.readLine()).isNull() && line != QStringLiteral("---")) {
        // skip frontmatter lines
    }
    const QString body = in.readAll();
    m_transcriptBodyCache[mdPath] = CachedBody{mtimeMs, size, body};
    return body;
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
    case LabelRole: {
        const QString mdPath = sidecarMdPath(node);
        return mdPath.isEmpty() ? QString() : frontmatterField(mdPath, QStringLiteral("label"));
    }
    case TagsRole: {
        const QString mdPath = sidecarMdPath(node);
        return mdPath.isEmpty() ? QString() : frontmatterField(mdPath, QStringLiteral("tags"));
    }
    case TranscriptBodyRole: {
        if (node->status != TranscribedStatus)
            return QString();
        const QString mdPath = sidecarMdPath(node);
        return mdPath.isEmpty() ? QString() : transcriptBody(mdPath);
    }
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
        {LabelRole, "label"},
        {TagsRole, "tags"},
        {TranscriptBodyRole, "transcriptBody"},
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

    // Determine which file(s) participate in the rename: an audio file (and
    // its sibling .md, if present) or a standalone orphaned .md file.
    QString audioOldPath;
    QString audioExt;
    QString mdOldPath;

    if (isAudioFile(info)) {
        audioOldPath = info.absoluteFilePath();
        audioExt = info.suffix();
        // Item P7: the sidecar's name is the FULL audio filename + ".md",
        // not just the stripped stem — see sidecarMdPath()'s doc comment.
        const QString candidateMd = dirPath + '/' + info.fileName() + ".md";
        if (QFileInfo::exists(candidateMd))
            mdOldPath = candidateMd;
    } else if (info.suffix().toLower() == "md") {
        mdOldPath = info.absoluteFilePath();
    } else {
        return fail(QStringLiteral("Unsupported file type."));
    }

    const QString newAudioPath = audioOldPath.isEmpty() ? QString() : dirPath + '/' + trimmed + '.' + audioExt;
    // When renaming an audio file with an existing sidecar, the new sidecar
    // name must track the new FULL audio filename (stem+ext+".md"), matching
    // the fileName()+".md" convention above — NOT just trimmed+".md", which
    // would silently revert to the old (collision-prone) stripped-stem
    // naming. A standalone orphaned .md being renamed directly (no audio
    // file involved) keeps the simple trimmed+".md" form, since there's no
    // audio extension to fold in.
    const QString newMdPath = mdOldPath.isEmpty()
        ? QString()
        : (audioOldPath.isEmpty() ? dirPath + '/' + trimmed + ".md" : newAudioPath + ".md");

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

bool VaultTreeModel::importFile(const QUrl &fileUrl, QString *errorOut)
{
    auto fail = [&](const QString &msg) {
        if (errorOut)
            *errorOut = msg;
        return false;
    };

    const QString localPath = vaultLocalPath();
    if (localPath.isEmpty())
        return fail(QStringLiteral("No vault is open."));

    const QString sourcePath = fileUrl.toLocalFile();
    const QFileInfo sourceInfo(sourcePath);
    if (!sourceInfo.exists() || !sourceInfo.isFile())
        return fail(QStringLiteral("Not a file: %1").arg(sourceInfo.fileName()));
    if (!isAudioFile(sourceInfo))
        return fail(QStringLiteral("Not a recognized audio file: %1").arg(sourceInfo.fileName()));

    const QString destPath = localPath + '/' + sourceInfo.fileName();
    if (QFileInfo::exists(destPath))
        return fail(QStringLiteral("\"%1\" already exists in the vault.").arg(sourceInfo.fileName()));

    if (!QFile::copy(sourcePath, destPath))
        return fail(QStringLiteral("Failed to copy \"%1\" into the vault.").arg(sourceInfo.fileName()));

    rebuild();
    return true;
}

QVariantMap VaultTreeModel::readVaultSettings() const
{
    QVariantMap result;
    const QString localPath = vaultLocalPath();
    if (localPath.isEmpty())
        return result;

    QFile file(localPath + QStringLiteral("/.suopTranscriber"));
    if (!file.open(QIODevice::ReadOnly))
        return result;

    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    if (!doc.isObject())
        return result;
    return doc.object().toVariantMap();
}

bool VaultTreeModel::writeVaultSettings(const QVariantMap &settings, QString *errorOut) const
{
    const QString localPath = vaultLocalPath();
    if (localPath.isEmpty()) {
        if (errorOut)
            *errorOut = QStringLiteral("no vault open");
        return false;
    }

    QFile file(localPath + QStringLiteral("/.suopTranscriber"));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (errorOut)
            *errorOut = QStringLiteral("could not open %1 for writing").arg(file.fileName());
        return false;
    }

    const QJsonDocument doc(QJsonObject::fromVariantMap(settings));
    file.write(doc.toJson(QJsonDocument::Indented));
    return true;
}

QStringList VaultTreeModel::allAudioPaths() const
{
    QStringList out;
    collectAudioPaths(m_root.get(), out);
    return out;
}

void VaultTreeModel::collectAudioPaths(const Node *node, QStringList &out)
{
    if (node->isAudio)
        out << node->path;
    for (const auto &child : node->children)
        collectAudioPaths(child.get(), out);
}

QVariantList VaultTreeModel::searchResults(const QString &query, bool includeTranscriptions) const
{
    QVariantList out;
    const QString queryLower = query.trimmed().toLower();
    if (queryLower.isEmpty())
        return out;

    collectSearchResults(m_root.get(), queryLower, includeTranscriptions, out);

    std::sort(out.begin(), out.end(), [](const QVariant &a, const QVariant &b) {
        return a.toMap().value(QStringLiteral("name")).toString().compare(
                   b.toMap().value(QStringLiteral("name")).toString(), Qt::CaseInsensitive)
            < 0;
    });
    return out;
}

// Only audio files and orphaned .md leaves are ever results (mirrors
// collectAudioPaths()'s traversal shape, but doesn't prune directories —
// they're walked purely to reach their children, never emitted themselves).
void VaultTreeModel::collectSearchResults(const Node *node, const QString &queryLower, bool includeTranscriptions,
                                           QVariantList &out) const
{
    if (node->isAudio || node->isOrphanedMd) {
        const QString mdPath = sidecarMdPath(node);
        const QString label = mdPath.isEmpty() ? QString() : frontmatterField(mdPath, QStringLiteral("label"));
        const QString tags = mdPath.isEmpty() ? QString() : frontmatterField(mdPath, QStringLiteral("tags"));

        // Only read the (possibly large) transcript body when the caller
        // asked for body matching AND the file is actually transcribed —
        // same gating TranscriptBodyRole itself uses in data() above.
        QString body;
        if (includeTranscriptions && node->status == TranscribedStatus && !mdPath.isEmpty())
            body = transcriptBody(mdPath);

        const bool matched = node->name.toLower().contains(queryLower) || label.toLower().contains(queryLower)
            || tags.toLower().contains(queryLower) || body.toLower().contains(queryLower);

        if (matched) {
            QVariantMap entry;
            entry[QStringLiteral("path")] = node->path;
            entry[QStringLiteral("name")] = node->name;
            entry[QStringLiteral("label")] = label;
            entry[QStringLiteral("status")] = static_cast<int>(node->status);
            entry[QStringLiteral("isAudio")] = node->isAudio;
            entry[QStringLiteral("isOrphanedMd")] = node->isOrphanedMd;
            entry[QStringLiteral("transcriptBody")] = body;
            out.append(entry);
        }
    }

    for (const auto &child : node->children)
        collectSearchResults(child.get(), queryLower, includeTranscriptions, out);
}

void VaultTreeModel::revealInFileManager(const QUrl &fileUrl) const
{
    // org.freedesktop.FileManager1.ShowItems(as URIs, s StartupID) — the
    // standard cross-desktop "select this file in the file manager" call.
    // Detached: this is a fire-and-forget UI action, not something the app
    // needs to track the lifetime of or report failure for.
    QProcess::startDetached(QStringLiteral("dbus-send"),
                             {QStringLiteral("--session"), QStringLiteral("--dest=org.freedesktop.FileManager1"),
                              QStringLiteral("--type=method_call"), QStringLiteral("/org/freedesktop/FileManager1"),
                              QStringLiteral("org.freedesktop.FileManager1.ShowItems"),
                              QStringLiteral("array:string:%1").arg(fileUrl.toString()), QStringLiteral("string:")});
}
