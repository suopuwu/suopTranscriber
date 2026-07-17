#include "VaultTreeModel.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QTextStream>

#include <algorithm>
#include <utility>
#include <vector>

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

// Shared name validator for a single filesystem entry the user typed
// (createFolder()'s new folder, renameFolder()'s new name). The invalid-char
// set is the one renameFile() rejects — it includes both path separators, so a
// validated name can never be multi-segment or escape its parent directory.
// renameFile() keeps its own copy of this check rather than calling here: it
// reports WHICH character was rejected through *errorOut, which this bool
// can't express.
bool isValidEntryName(const QString &name)
{
    const QString trimmed = name.trimmed();
    if (trimmed.isEmpty())
        return false;

    static const QString invalidChars = QStringLiteral("/\\:*?\"<>|");
    for (const QChar &c : name) {
        if (invalidChars.contains(c))
            return false;
    }
    // "." / ".." aren't separators but still name an existing directory rather
    // than a new child — reject explicitly rather than rely on a collision
    // check, which they'd pass.
    return trimmed != QStringLiteral(".") && trimmed != QStringLiteral("..");
}

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
                auto child = scanDirectory(entry.absoluteFilePath(), m_root.get());
                watchedDirs << child->path;
                m_root->children.push_back(std::move(child));
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
            node->children.push_back(scanDirectory(entry.absoluteFilePath(), node.get()));
        } else if (isAudioFile(entry)) {
            auto leaf = std::make_unique<Node>();
            leaf->name = entry.fileName();
            leaf->path = entry.absoluteFilePath();
            leaf->isAudio = true;
            const QString mdPath = entry.absolutePath() + '/' + entry.fileName() + ".md";
            leaf->status = QFileInfo::exists(mdPath) ? TranscribedStatus : UntranscribedStatus;
            leaf->parent = node.get();
            node->children.push_back(std::move(leaf));
        } else if (entry.suffix().toLower() == "md" && !audioBaseNames.contains(entry.completeBaseName())) {
            auto leaf = std::make_unique<Node>();
            leaf->name = entry.fileName();
            leaf->path = entry.absoluteFilePath();
            leaf->isAudio = false;
            leaf->isOrphanedMd = true;
            leaf->status = OrphanedMdStatus;
            leaf->parent = node.get();
            node->children.push_back(std::move(leaf));
        }
    }

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

// Mirrors TranscriptionController::readMetadata()'s frontmatter scan. Parses
// every key in one pass and caches the lot per mdPath, invalidated by
// mtime+size — see this function's doc comment in the header for why the old
// read-fresh-one-key-per-call version was a hot-path problem.
QString VaultTreeModel::frontmatterField(const QString &mdPath, const QString &key) const
{
    const QFileInfo info(mdPath);
    const qint64 mtimeMs = info.lastModified().toMSecsSinceEpoch();
    const qint64 size = info.size();

    const auto cached = m_frontmatterCache.constFind(mdPath);
    if (cached != m_frontmatterCache.constEnd() && cached->mtimeMs == mtimeMs && cached->size == size)
        return cached->fields.value(key);

    QFile mdFile(mdPath);
    if (!mdFile.open(QIODevice::ReadOnly | QIODevice::Text))
        return {}; // deliberately not cached — same as transcriptBody()

    QTextStream in(&mdFile);
    if (in.readLine() != QStringLiteral("---"))
        return {};

    QHash<QString, QString> fields;
    QString line;
    while (!(line = in.readLine()).isNull() && line != QStringLiteral("---")) {
        const int colon = line.indexOf(':');
        if (colon < 0)
            continue;
        const QString name = line.left(colon).trimmed();
        // First occurrence wins, matching the old early-return-on-first-match
        // scan (a duplicate key must not shadow the one callers used to get).
        if (!fields.contains(name))
            fields.insert(name, line.mid(colon + 1).trimmed());
    }
    m_frontmatterCache[mdPath] = CachedFrontmatter{mtimeMs, size, fields};
    return fields.value(key);
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
    case SpeakersRole: {
        // "speakers:" frontmatter is kept in sync with the body's actual
        // speaker tags by renameSpeakerInFile()/mergeSpeakersInFile()/
        // syncSpeakersFrontmatterToBody() (TranscriptionController.cpp), same
        // comma-separated shape as "tags:" — cheap frontmatter-only read,
        // same as TagsRole above, no body scan needed.
        const QString mdPath = sidecarMdPath(node);
        return mdPath.isEmpty() ? QString() : frontmatterField(mdPath, QStringLiteral("speakers"));
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
        {SpeakersRole, "speakers"},
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

QString VaultTreeModel::importDestinationFor(const QUrl &fileUrl, QString *errorOut) const
{
    auto fail = [&](const QString &msg) {
        if (errorOut)
            *errorOut = msg;
        return QString();
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

    // No copy here — the actual (potentially slow, on a large file) byte-copy
    // runs on TranscriptionController's worker thread (see importAudio()), and
    // the new file appearing in the watched vault dir fires
    // QFileSystemWatcher::directoryChanged -> rebuild(), refreshing the tree
    // without an explicit rebuild() on this (const) validation path.
    if (errorOut)
        errorOut->clear();
    return destPath;
}

bool VaultTreeModel::createFolder(const QUrl &parentDirUrl, const QString &name)
{
    if (!isValidEntryName(name))
        return false;
    const QString trimmed = name.trimmed();

    const QString parentPath = parentDirUrl.toLocalFile();
    if (parentPath.isEmpty())
        return false;
    const QFileInfo parentInfo(parentPath);
    if (!parentInfo.exists() || !parentInfo.isDir())
        return false;
    if (!isPathInsideVault(parentPath))
        return false;

    const QString newPath = parentPath + '/' + trimmed;
    if (QFileInfo::exists(newPath))
        return false; // collision — never clobber an existing entry

    if (!QDir().mkpath(newPath))
        return false;

    rebuild();
    return true;
}

QString VaultTreeModel::guardedLocalPath(const QUrl &url, EntryKind kind) const
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
    const QString vaultCanonical = QFileInfo(vaultLocalPath()).canonicalFilePath();
    if (canonical.isEmpty() || vaultCanonical.isEmpty())
        return {};
    // Never the vault root itself — deleting it would take the whole vault out
    // from under the open window...
    if (canonical == vaultCanonical)
        return {};
    // ...nor anything outside the vault (isPathInsideVault also accepts the
    // root, hence the explicit equality guard above).
    if (!isPathInsideVault(path))
        return {};

    return path;
}

bool VaultTreeModel::renameFolder(const QUrl &folderUrl, const QString &newName, QString *errorOut)
{
    auto fail = [&](const QString &msg) {
        if (errorOut)
            *errorOut = msg;
        return false;
    };

    if (!isValidEntryName(newName))
        return fail(QStringLiteral("Invalid folder name."));
    const QString trimmed = newName.trimmed();

    const QString path = folderUrl.toLocalFile();
    if (path.isEmpty())
        return fail(QStringLiteral("Folder does not exist."));

    const QFileInfo info(path);
    if (!info.exists() || !info.isDir())
        return fail(QStringLiteral("Folder does not exist."));

    const QString canonical = info.canonicalFilePath();
    const QString vaultCanonical = QFileInfo(vaultLocalPath()).canonicalFilePath();
    if (canonical.isEmpty() || vaultCanonical.isEmpty())
        return fail(QStringLiteral("Folder does not exist."));
    // Same two guards as deleteFolder(): never the vault root itself (renaming
    // it would move the whole vault out from under the open window), and never
    // anything outside the vault. isPathInsideVault() also accepts the root,
    // hence the explicit equality guard first.
    if (canonical == vaultCanonical)
        return fail(QStringLiteral("Can't rename the vault root."));
    if (!isPathInsideVault(path))
        return fail(QStringLiteral("Folder is outside the vault."));

    // A pure rename in place — the parent never changes (moveFolder() owns
    // reparenting), so a validated name can only ever land beside itself.
    const QString newPath = info.absolutePath() + '/' + trimmed;
    if (newPath == info.absoluteFilePath())
        return true; // no-op: same name, not a failure worth reporting
    if (QFileInfo::exists(newPath))
        return fail(QStringLiteral("A folder with that name already exists."));

    if (!QDir().rename(path, newPath))
        return fail(QStringLiteral("Failed to rename folder."));

    rebuild();
    return true;
}

bool VaultTreeModel::moveFile(const QUrl &fileUrl, const QUrl &destDirUrl)
{
    const QString srcPath = fileUrl.toLocalFile();
    const QString destDir = destDirUrl.toLocalFile();
    if (srcPath.isEmpty() || destDir.isEmpty())
        return false;

    const QFileInfo srcInfo(srcPath);
    if (!srcInfo.exists() || !srcInfo.isFile())
        return false;

    const QFileInfo destDirInfo(destDir);
    if (!destDirInfo.exists() || !destDirInfo.isDir())
        return false;

    // Both ends must stay inside the vault.
    if (!isPathInsideVault(srcPath) || !isPathInsideVault(destDir))
        return false;

    // No-op guard: destination is the file's current directory.
    if (QFileInfo(srcInfo.absolutePath()).canonicalFilePath() == destDirInfo.canonicalFilePath())
        return false;

    // The set of files that move together: the file itself, plus — when it's
    // an audio file — its <fileName>.md sidecar and, if present, its
    // .<fileName>.partial.jsonl resume state (same full-filename convention as
    // sidecarMdPath(); see item P7). A standalone .md moves alone.
    std::vector<std::pair<QString, QString>> moves;
    moves.emplace_back(srcPath, destDir + '/' + srcInfo.fileName());

    if (isAudioFile(srcInfo)) {
        const QString dirPath = srcInfo.absolutePath();
        const QString mdFrom = dirPath + '/' + srcInfo.fileName() + ".md";
        if (QFileInfo::exists(mdFrom))
            moves.emplace_back(mdFrom, destDir + '/' + srcInfo.fileName() + ".md");
        const QString partialFrom = dirPath + "/." + srcInfo.fileName() + ".partial.jsonl";
        if (QFileInfo::exists(partialFrom))
            moves.emplace_back(partialFrom, destDir + "/." + srcInfo.fileName() + ".partial.jsonl");
    }

    // Check every destination for a collision BEFORE moving anything, so a
    // half-done move never splits the audio/.md pair across two folders.
    for (const auto &m : moves) {
        if (QFileInfo::exists(m.second))
            return false;
    }

    std::vector<std::pair<QString, QString>> done;
    for (const auto &m : moves) {
        if (!QFile::rename(m.first, m.second)) {
            for (const auto &d : done)
                QFile::rename(d.second, d.first); // best-effort rollback
            return false;
        }
        done.push_back(m);
    }

    rebuild();
    return true;
}

bool VaultTreeModel::moveFolder(const QUrl &folderUrl, const QUrl &destParentUrl)
{
    const QString srcPath = folderUrl.toLocalFile();
    const QString destParent = destParentUrl.toLocalFile();
    if (srcPath.isEmpty() || destParent.isEmpty())
        return false;

    const QFileInfo srcInfo(srcPath);
    if (!srcInfo.exists() || !srcInfo.isDir())
        return false;
    const QFileInfo destParentInfo(destParent);
    if (!destParentInfo.exists() || !destParentInfo.isDir())
        return false;

    const QString srcCanonical = srcInfo.canonicalFilePath();
    const QString destParentCanonical = destParentInfo.canonicalFilePath();
    const QString vaultCanonical = QFileInfo(vaultLocalPath()).canonicalFilePath();
    if (srcCanonical.isEmpty() || destParentCanonical.isEmpty() || vaultCanonical.isEmpty())
        return false;

    // Never move the vault root itself, and keep both ends inside the vault.
    if (srcCanonical == vaultCanonical)
        return false;
    if (!isPathInsideVault(srcPath) || !isPathInsideVault(destParent))
        return false;

    // Refuse moving a folder into itself or one of its own descendants — that
    // would orphan the subtree.
    if (destParentCanonical == srcCanonical || destParentCanonical.startsWith(srcCanonical + '/'))
        return false;

    // No-op guard: already directly inside destParent.
    if (QFileInfo(srcInfo.absolutePath()).canonicalFilePath() == destParentCanonical)
        return false;

    const QString destPath = destParent + '/' + srcInfo.fileName();
    if (QFileInfo::exists(destPath))
        return false; // collision

    if (!QDir().rename(srcPath, destPath))
        return false;

    rebuild();
    return true;
}

// The transcript set for `path`: its .md sidecar and .partial.jsonl resume
// state. Same full-filename convention as sidecarMdPath()/moveFile() (item P7).
// Returns false if path isn't a thing that HAS a transcript.
static bool transcriptPathsFor(const QFileInfo &info, QString *mdOut, QString *partialOut)
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

// Records one trashed path so restoreFromTrash() can put it back. The trash
// path is only knowable at trash time — nothing can reconstruct it later.
static void recordRestorable(QVariantList *restorable, const QString &trashPath,
                             const QString &originalPath)
{
    if (!restorable || trashPath.isEmpty())
        return;
    restorable->append(QVariantMap{ { QStringLiteral("trashPath"), trashPath },
                                    { QStringLiteral("originalPath"), originalPath } });
}

VaultTreeModel::TrashOutcome VaultTreeModel::trashEntry(const QUrl &url, EntryKind kind,
                                                        QVariantList *restorable)
{
    const QString path = guardedLocalPath(url, kind);
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

bool VaultTreeModel::removeEntryPermanently(const QUrl &url, EntryKind kind)
{
    const QString path = guardedLocalPath(url, kind);
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

QVariantMap VaultTreeModel::trashFiles(const QList<QUrl> &urls)
{
    // One rebuild() for the whole batch, not one per entry: rebuild() rescans
    // the vault recursively AND resets the model (destroying/recreating every
    // realized delegate), so looping over a 20-file selection would pay both
    // costs 20 times over for a single user action.
    QVariantList restorable;
    QList<QUrl> trashed;
    QVariantList untrashable;
    QVariantList invalid;
    trashed.reserve(urls.size());

    for (const QUrl &url : urls) {
        switch (trashEntry(url, EntryKind::File, &restorable)) {
        case TrashOutcome::Trashed:
            trashed.append(url);
            break;
        case TrashOutcome::Untrashable:
            untrashable.append(url.toString());
            break;
        case TrashOutcome::Invalid:
            invalid.append(url.toString());
            break;
        }
    }

    if (!trashed.isEmpty()) {
        rebuild();
        // Emitted only after the single rebuild, so a listener that re-reads
        // the tree (MainPanel closing the deleted file) never sees a model
        // still advertising rows whose files are already gone. Per surviving
        // file, not once for the batch — fileDeleted's contract is one file per
        // emission, and MainPanel matches it against the file it has open.
        for (const QUrl &url : trashed)
            emit fileDeleted(url);
    }

    return QVariantMap{ { QStringLiteral("trashed"), int(trashed.size()) },
                        { QStringLiteral("restorable"), restorable },
                        { QStringLiteral("untrashable"), untrashable },
                        { QStringLiteral("invalid"), invalid } };
}

int VaultTreeModel::deleteFilesPermanently(const QList<QUrl> &urls)
{
    QList<QUrl> deleted;
    deleted.reserve(urls.size());
    for (const QUrl &url : urls) {
        if (removeEntryPermanently(url, EntryKind::File))
            deleted.append(url);
    }

    if (deleted.isEmpty())
        return 0;

    rebuild();
    for (const QUrl &url : deleted)
        emit fileDeleted(url);
    return deleted.size();
}

QVariantMap VaultTreeModel::trashFolder(const QUrl &folderUrl)
{
    QVariantList restorable;
    QVariantMap result{ { QStringLiteral("trashed"), 0 },
                        { QStringLiteral("restorable"), QVariantList{} },
                        { QStringLiteral("untrashable"), QVariantList{} },
                        { QStringLiteral("invalid"), QVariantList{} } };

    switch (trashEntry(folderUrl, EntryKind::Folder, &restorable)) {
    case TrashOutcome::Trashed:
        rebuild();
        result[QStringLiteral("trashed")] = 1;
        result[QStringLiteral("restorable")] = restorable;
        break;
    case TrashOutcome::Untrashable:
        result[QStringLiteral("untrashable")] = QVariantList{ folderUrl.toString() };
        break;
    case TrashOutcome::Invalid:
        result[QStringLiteral("invalid")] = QVariantList{ folderUrl.toString() };
        break;
    }
    return result;
}

bool VaultTreeModel::deleteFolderPermanently(const QUrl &folderUrl)
{
    if (!removeEntryPermanently(folderUrl, EntryKind::Folder))
        return false;
    rebuild();
    return true;
}

QVariantMap VaultTreeModel::trashTranscript(const QUrl &fileUrl)
{
    QVariantMap result{ { QStringLiteral("trashed"), 0 },
                        { QStringLiteral("restorable"), QVariantList{} },
                        { QStringLiteral("untrashable"), QVariantList{} },
                        { QStringLiteral("invalid"), QVariantList{} } };

    const QString path = fileUrl.toLocalFile();
    const QFileInfo info(path);
    QString mdPath, partialPath;
    // NOT guardedLocalPath(): the transcript of an audio file is what's being
    // removed, so it's the .md that must be inside the vault, and the audio
    // itself is never touched. isPathInsideVault covers it.
    if (path.isEmpty() || !info.exists() || !info.isFile() || !isPathInsideVault(path)
        || !transcriptPathsFor(info, &mdPath, &partialPath)) {
        result[QStringLiteral("invalid")] = QVariantList{ fileUrl.toString() };
        return result;
    }
    if (!QFileInfo::exists(mdPath))
        return result; // nothing transcribed here — a no-op, not a failure

    QVariantList restorable;
    QString trashPath;
    if (!trashOnly(mdPath, &trashPath)) {
        result[QStringLiteral("untrashable")] = QVariantList{ fileUrl.toString() };
        return result;
    }
    recordRestorable(&restorable, trashPath, mdPath);
    if (trashOnly(partialPath, &trashPath)) // best-effort; an absent partial is fine
        recordRestorable(&restorable, trashPath, partialPath);

    rebuild();
    emit transcriptDeleted(fileUrl);

    result[QStringLiteral("trashed")] = 1;
    result[QStringLiteral("restorable")] = restorable;
    return result;
}

bool VaultTreeModel::deleteTranscriptPermanently(const QUrl &fileUrl)
{
    const QString path = fileUrl.toLocalFile();
    const QFileInfo info(path);
    QString mdPath, partialPath;
    if (path.isEmpty() || !info.exists() || !info.isFile() || !isPathInsideVault(path)
        || !transcriptPathsFor(info, &mdPath, &partialPath))
        return false;
    if (!QFileInfo::exists(mdPath))
        return false;

    if (!removePermanently(mdPath))
        return false;
    removePermanently(partialPath); // best-effort

    rebuild();
    emit transcriptDeleted(fileUrl);
    return true;
}

// Which signal an audio-only delete ends on — shared by the trash and permanent
// paths so the two can't drift into disagreeing about it.
//
// Checked AFTER the delete, deliberately: "did the transcript survive" is a
// question about the state we're leaving behind, and the .md could have been
// absent all along (an untranscribed audio file — the UI doesn't offer this
// action there, but the invokable is callable regardless). Whether it survived
// is what decides whether a listener has somewhere to go.
void VaultTreeModel::emitAudioOnlyDeleted(const QUrl &fileUrl, const QString &mdPath)
{
    if (QFileInfo::exists(mdPath))
        emit audioDeletedTranscriptKept(fileUrl, QUrl::fromLocalFile(mdPath));
    else
        emit fileDeleted(fileUrl); // nothing survived — an ordinary delete
}

QVariantMap VaultTreeModel::trashAudioOnly(const QUrl &fileUrl)
{
    QVariantMap result{ { QStringLiteral("trashed"), 0 },
                        { QStringLiteral("restorable"), QVariantList{} },
                        { QStringLiteral("untrashable"), QVariantList{} },
                        { QStringLiteral("invalid"), QVariantList{} } };

    const QString path = guardedLocalPath(fileUrl, EntryKind::File);
    // Audio only: "drop the audio, keep the transcript" is meaningless for an
    // orphaned .md (there's no audio) and for a non-audio file (there's no
    // transcript), so both are rejected rather than silently doing something else.
    const QFileInfo info(path);
    if (path.isEmpty() || !isAudioFile(info)) {
        result[QStringLiteral("invalid")] = QVariantList{ fileUrl.toString() };
        return result;
    }

    // Same lazy-stat trap as trashEntry(): resolve the partial's path while the
    // audio still exists, not after it has moved to the trash.
    QString mdPath, partialPath;
    transcriptPathsFor(info, &mdPath, &partialPath);

    QVariantList restorable;
    QString trashPath;
    if (!trashOnly(path, &trashPath)) {
        result[QStringLiteral("untrashable")] = QVariantList{ fileUrl.toString() };
        return result;
    }
    recordRestorable(&restorable, trashPath, path);

    // The partial follows the audio; mdPath is deliberately never touched.
    if (trashOnly(partialPath, &trashPath)) // best-effort; an absent partial is fine
        recordRestorable(&restorable, trashPath, partialPath);

    rebuild();
    emitAudioOnlyDeleted(fileUrl, mdPath);

    result[QStringLiteral("trashed")] = 1;
    result[QStringLiteral("restorable")] = restorable;
    return result;
}

bool VaultTreeModel::deleteAudioOnlyPermanently(const QUrl &fileUrl)
{
    const QString path = guardedLocalPath(fileUrl, EntryKind::File);
    const QFileInfo info(path);
    if (path.isEmpty() || !isAudioFile(info))
        return false;

    QString mdPath, partialPath;
    transcriptPathsFor(info, &mdPath, &partialPath);

    if (!removePermanently(path))
        return false;
    removePermanently(partialPath); // best-effort; mdPath is left alone on purpose

    rebuild();
    emitAudioOnlyDeleted(fileUrl, mdPath);
    return true;
}

// Unlike the delete side, a restore isn't told what it is undoing: entries
// arrive as a flat list of paths, with an audio file, its .md and its
// .partial.jsonl all peers. The BATCH is what recovers the missing intent —
// hence taking the whole list rather than one path at a time:
//
//   audio + its .md coming back TOGETHER  -> the whole file returning, because
//                                            only a delete that took the .md
//                                            too could have trashed both
//                                            (fileDeleted -> fileRestored);
//   audio coming back ALONE to a .md that -> an audio-only delete being undone
//   never left                               (audioDeletedTranscriptKept ->
//                                            audioRestoredForTranscript).
//
// The paths are identical in both cases — only their company differs.
//
// Exactly one signal per user-visible file, never two, for the same reason the
// delete side picks one (see audioDeletedTranscriptKept's comment): a listener
// obeying both would leave the view's final state at the mercy of emission
// order. Anything the UI never showed leaving stays silent — a .partial.jsonl
// is invisible, and a restored folder is already covered by the rebuild, since
// trashFolder() reported no signal on the way out either.
void VaultTreeModel::emitRestored(const QStringList &originalPaths)
{
    const QSet<QString> batch(originalPaths.cbegin(), originalPaths.cend());
    for (const QString &path : originalPaths) {
        const QFileInfo info(path);
        if (isAudioFile(info)) {
            QString mdPath, partialPath;
            transcriptPathsFor(info, &mdPath, &partialPath);
            if (!batch.contains(mdPath) && QFileInfo::exists(mdPath)) {
                // The transcript never left — a listener is sitting on it.
                emit audioRestoredForTranscript(QUrl::fromLocalFile(path),
                                                QUrl::fromLocalFile(mdPath));
            } else {
                // The whole file came back: with its transcript, or with none to
                // begin with. Either way whatever closed on fileDeleted can now
                // reopen it, and an audio file is never an orphaned .md.
                emit fileRestored(QUrl::fromLocalFile(path), false);
            }
            continue;
        }
        if (info.suffix().toLower() != QLatin1String("md"))
            continue;
        // sidecarMdPath()'s full-filename rule read backwards: foo.mp3.md
        // belongs to foo.mp3.
        const QString audioPath = path.chopped(3);
        if (batch.contains(audioPath))
            continue; // already announced as its audio's fileRestored — same file
        const QFileInfo audioInfo(audioPath);
        if (audioInfo.isFile() && isAudioFile(audioInfo)) {
            emit transcriptRestored(QUrl::fromLocalFile(audioPath));
            continue;
        }
        // No audio anywhere: the .md stands alone and IS the file, exactly as
        // scanDirectory() will have just classified it.
        emit fileRestored(QUrl::fromLocalFile(path), true);
    }
}

int VaultTreeModel::restoreFromTrash(const QVariantList &entries)
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
        if (!isPathInsideVault(originalPath))
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

    if (!restoredPaths.isEmpty()) {
        rebuild();
        // After the single rebuild, never between — the same ordering rule the
        // trash side follows (see trashFiles()): a listener that re-reads the
        // tree must not see it still advertising the pre-restore shape.
        emitRestored(restoredPaths);
    }
    return int(restoredPaths.size());
}

QStringList VaultTreeModel::vaultFolders() const
{
    QStringList out;
    const QString root = vaultLocalPath();
    if (root.isEmpty() || !QFileInfo::exists(root))
        return out;
    out << root;
    collectFolderPaths(root, out);
    return out;
}

void VaultTreeModel::collectFolderPaths(const QString &dirPath, QStringList &out)
{
    // QDir::Dirs (without QDir::Hidden) skips dotdirs like .transcriber, same
    // as the tree scan; walks the real filesystem rather than the pruned tree
    // so empty folders are still offered as move destinations.
    QDir dir(dirPath);
    const QFileInfoList entries = dir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (const QFileInfo &entry : entries) {
        out << entry.absoluteFilePath();
        collectFolderPaths(entry.absoluteFilePath(), out);
    }
}

bool VaultTreeModel::isPathInsideVault(const QString &candidatePath) const
{
    const QString localPath = vaultLocalPath();
    if (localPath.isEmpty() || candidatePath.isEmpty())
        return false;

    // The vault root itself must exist (it always does once opened) for
    // canonicalization to mean anything.
    const QString vaultCanonical = QFileInfo(localPath).canonicalFilePath();
    if (vaultCanonical.isEmpty())
        return false;

    // candidatePath may not exist yet (e.g. a folder the user is about to
    // create/select in a dialog) — QFileInfo::canonicalFilePath() returns
    // empty for a nonexistent path, so walk up to the nearest existing
    // ancestor, canonicalize THAT (resolving any ".."/symlinks along the
    // existing portion), then re-append the not-yet-created tail.
    QString ancestor = QFileInfo(candidatePath).absoluteFilePath();
    QString tail;
    while (!QFileInfo::exists(ancestor)) {
        const QString parent = QFileInfo(ancestor).absolutePath();
        if (parent == ancestor)
            return false; // walked past the filesystem root without finding anything real
        const QString name = QFileInfo(ancestor).fileName();
        tail = tail.isEmpty() ? name : name + '/' + tail;
        ancestor = parent;
    }

    const QString ancestorCanonical = QFileInfo(ancestor).canonicalFilePath();
    if (ancestorCanonical.isEmpty())
        return false;
    const QString candidateCanonical = tail.isEmpty() ? ancestorCanonical : ancestorCanonical + '/' + tail;

    if (candidateCanonical == vaultCanonical)
        return true;

    const QString relative = QDir(vaultCanonical).relativeFilePath(candidateCanonical);
    return !relative.startsWith(QStringLiteral("..")) && !QDir::isAbsolutePath(relative);
}

QVariantMap VaultTreeModel::readVaultSettings() const
{
    QVariantMap result;
    const QString localPath = vaultLocalPath();
    if (localPath.isEmpty())
        return result;

    QFile file(localPath + QStringLiteral("/.transcriber/settings.json"));
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

    const QDir configDir(localPath + QStringLiteral("/.transcriber"));
    if (!configDir.mkpath(QStringLiteral("."))) {
        if (errorOut)
            *errorOut = QStringLiteral("could not create %1").arg(configDir.path());
        return false;
    }

    QFile file(configDir.filePath(QStringLiteral("settings.json")));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (errorOut)
            *errorOut = QStringLiteral("could not open %1 for writing").arg(file.fileName());
        return false;
    }

    const QJsonDocument doc(QJsonObject::fromVariantMap(settings));
    file.write(doc.toJson(QJsonDocument::Indented));
    return true;
}

QVariantList VaultTreeModel::readUsbGrabHistory() const
{
    QVariantList result;
    const QString localPath = vaultLocalPath();
    if (localPath.isEmpty())
        return result;

    QFile file(localPath + QStringLiteral("/.transcriber/usbGrabHistory.json"));
    if (!file.open(QIODevice::ReadOnly))
        return result;

    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    if (!doc.isArray())
        return result;
    return doc.array().toVariantList();
}

bool VaultTreeModel::appendUsbGrabHistoryEntry(const QVariantMap &entry, QString *errorOut) const
{
    const QString localPath = vaultLocalPath();
    if (localPath.isEmpty()) {
        if (errorOut)
            *errorOut = QStringLiteral("no vault open");
        return false;
    }

    const QDir configDir(localPath + QStringLiteral("/.transcriber"));
    if (!configDir.mkpath(QStringLiteral("."))) {
        if (errorOut)
            *errorOut = QStringLiteral("could not create %1").arg(configDir.path());
        return false;
    }

    const QString historyPath = configDir.filePath(QStringLiteral("usbGrabHistory.json"));

    QJsonArray history;
    QFile readFile(historyPath);
    if (readFile.open(QIODevice::ReadOnly)) {
        const QJsonDocument existingDoc = QJsonDocument::fromJson(readFile.readAll());
        readFile.close();
        if (existingDoc.isArray())
            history = existingDoc.array();
    }

    history.append(QJsonObject::fromVariantMap(entry));

    QFile writeFile(historyPath);
    if (!writeFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (errorOut)
            *errorOut = QStringLiteral("could not open %1 for writing").arg(writeFile.fileName());
        return false;
    }

    const QJsonDocument doc(history);
    writeFile.write(doc.toJson(QJsonDocument::Indented));
    return true;
}

QVariantMap VaultTreeModel::readBenchmarkLastRun() const
{
    QVariantMap result;
    const QString localPath = vaultLocalPath();
    if (localPath.isEmpty())
        return result;

    QFile file(localPath + QStringLiteral("/.transcriber/benchmarkLastRun.json"));
    if (!file.open(QIODevice::ReadOnly))
        return result;

    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    if (!doc.isObject())
        return result;
    return doc.object().toVariantMap();
}

bool VaultTreeModel::writeBenchmarkLastRun(const QVariantMap &run, QString *errorOut) const
{
    const QString localPath = vaultLocalPath();
    if (localPath.isEmpty()) {
        if (errorOut)
            *errorOut = QStringLiteral("no vault open");
        return false;
    }

    const QDir configDir(localPath + QStringLiteral("/.transcriber"));
    if (!configDir.mkpath(QStringLiteral("."))) {
        if (errorOut)
            *errorOut = QStringLiteral("could not create %1").arg(configDir.path());
        return false;
    }

    // Overwrite each run (not appended, unlike usbGrabHistory) — only the last
    // benchmark is kept.
    QFile file(configDir.filePath(QStringLiteral("benchmarkLastRun.json")));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (errorOut)
            *errorOut = QStringLiteral("could not open %1 for writing").arg(file.fileName());
        return false;
    }

    const QJsonDocument doc(QJsonObject::fromVariantMap(run));
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

QVariantMap VaultTreeModel::audioLastModified() const
{
    QVariantMap out;
    collectAudioLastModified(m_root.get(), out);
    return out;
}

void VaultTreeModel::collectAudioLastModified(const Node *node, QVariantMap &out)
{
    if (node->isAudio)
        out.insert(node->path, QVariant::fromValue(QFileInfo(node->path).lastModified().toMSecsSinceEpoch()));
    for (const auto &child : node->children)
        collectAudioLastModified(child.get(), out);
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

VaultTreeModel::Node *VaultTreeModel::findNodeByPath(Node *node, const QString &path)
{
    if (node->path == path)
        return node;
    for (const auto &child : node->children) {
        if (Node *hit = findNodeByPath(child.get(), path))
            return hit;
    }
    return nullptr;
}

QModelIndex VaultTreeModel::indexForNode(const Node *node) const
{
    if (!node || !node->parent)
        return {};
    const std::vector<std::unique_ptr<Node>> &siblings = node->parent->children;
    for (size_t i = 0; i < siblings.size(); ++i) {
        if (siblings[i].get() == node)
            return createIndex(static_cast<int>(i), 0, const_cast<Node *>(node));
    }
    return {};
}

void VaultTreeModel::refreshFile(const QUrl &fileUrl)
{
    const QString path = fileUrl.toLocalFile();
    if (path.isEmpty())
        return;

    Node *node = findNodeByPath(m_root.get(), path);
    if (!node)
        return;

    const QModelIndex idx = indexForNode(node);
    if (!idx.isValid())
        return;

    emit dataChanged(idx, idx);
}
