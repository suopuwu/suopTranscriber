#include "VaultTreeModel.h"

#include "AudioExtensions.h"
#include "FileRename.h"
#include "MdFrontmatter.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QSaveFile>
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

} // namespace

VaultTreeModel::VaultTreeModel(QObject *parent)
    : QAbstractItemModel(parent)
    , m_root(std::make_unique<Node>())
{
    m_root->isDir = true;
    // A single vault mutation (esp. a multi-file move, which dirties both the
    // source and destination dirs per file) fires many directoryChanged events
    // in a burst. Rebuilding on each one is a full model reset apiece — N-file
    // move => ~2N resets, each triggering a full-vault walk + proxy re-sort +
    // whole-tree re-expand in Sidebar.qml. Coalesce the burst into one rebuild.
    m_rebuildDebounce.setSingleShot(true);
    m_rebuildDebounce.setInterval(50);
    // Lambda, not a PMF connect: rebuild() now takes a defaulted argument and
    // the new-style connect does not honour default arguments.
    connect(&m_rebuildDebounce, &QTimer::timeout, this, [this] { rebuild(); });
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged,
            this, [this] { m_rebuildDebounce.start(); });
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
    // Force: switching between two empty vaults is structurally identical, but
    // the view must still reset onto the new root.
    rebuild(true);
}

void VaultTreeModel::setPendingCombines(const QVariantList &pending)
{
    QList<PendingCombine> parsed;
    parsed.reserve(pending.size());
    for (const QVariant &entry : pending) {
        const QVariantMap map = entry.toMap();
        PendingCombine combine;
        combine.outputPath = map.value(QStringLiteral("outputPath")).toString();
        combine.sources = map.value(QStringLiteral("sources")).toStringList();
        // A combine with no output path has nothing to stand a placeholder at
        // and nothing to hide the sources in favour of — skip rather than
        // synthesize a node keyed on "".
        if (!combine.outputPath.isEmpty())
            parsed.append(combine);
    }

    // queueChanged fires on every queue transition, the overwhelming majority
    // of which touch no combine at all; a rebuild is a full recursive rescan +
    // model reset, so only pay it when this really changed.
    if (parsed == m_pendingCombines)
        return;

    m_pendingCombines = parsed;
    m_pendingCombineHidden.clear();
    for (const PendingCombine &combine : m_pendingCombines) {
        m_pendingCombineHidden.insert(combine.outputPath);
        for (const QString &source : combine.sources)
            m_pendingCombineHidden.insert(source);
    }
    rebuild();
}

bool VaultTreeModel::isHiddenByPendingCombine(const QString &path) const
{
    return m_pendingCombineHidden.contains(path);
}

void VaultTreeModel::appendCombinePlaceholders(Node *parent, const QString &dirPath) const
{
    for (const PendingCombine &combine : m_pendingCombines) {
        if (QFileInfo(combine.outputPath).absolutePath() != dirPath)
            continue;
        auto leaf = std::make_unique<Node>();
        // The path is the row's identity (it's what matches this row to the job
        // driving it), but nothing renders it — see setPendingCombines(). name
        // stays empty for the same reason: there is no file to name yet, and a
        // name derived from outputPath would advertise a file that does not
        // exist.
        leaf->path = combine.outputPath;
        leaf->isCombinePlaceholder = true;
        leaf->combineSourceCount = int(combine.sources.size());
        leaf->parent = parent;
        parent->children.push_back(std::move(leaf));
    }
}

void VaultTreeModel::rebuild(bool forceReset)
{
    // Scan into a DETACHED tree first — the reset can't be opened until we know
    // whether anything actually changed. See the header for why.
    auto fresh = std::make_unique<Node>();
    fresh->isDir = true;

    const QString rootPath = m_vaultPath.toLocalFile();
    // Hoisted above the block so the watcher update after the scan can see it.
    QStringList watchedDirs;
    if (!rootPath.isEmpty() && QFileInfo::exists(rootPath)) {
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
                auto child = scanDirectory(entry.absoluteFilePath(), fresh.get());
                watchedDirs << child->path;
                fresh->children.push_back(std::move(child));
            } else if (isHiddenByPendingCombine(entry.absoluteFilePath())) {
                // A pending combine's source, or its half-written output —
                // the placeholder below stands for the lot. Note this is
                // checked AFTER audioBaseNames was built above, deliberately:
                // a hidden source stays in that set, so its own .md sidecar
                // still reads as non-orphaned and stays hidden too, instead of
                // surfacing as a stray orphaned-.md row. See
                // setPendingCombines().
                continue;
            } else if (isAudioFile(entry)) {
                auto leaf = std::make_unique<Node>();
                leaf->name = entry.fileName();
                leaf->path = entry.absoluteFilePath();
                leaf->isAudio = true;
                const QString mdPath = entry.absolutePath() + '/' + entry.fileName() + ".md";
                const QFileInfo mdInfo(mdPath);
                leaf->status = mdInfo.exists() ? TranscribedStatus : UntranscribedStatus;
                // Same stat, one extra field — lets rebuild() refresh a row whose
                // transcript changed without resetting the whole model.
                leaf->mdModified = mdInfo.exists() ? mdInfo.lastModified().toMSecsSinceEpoch() : 0;
                leaf->parent = fresh.get();
                fresh->children.push_back(std::move(leaf));
            } else if (entry.suffix().toLower() == "md" && !audioBaseNames.contains(entry.completeBaseName())) {
                auto leaf = std::make_unique<Node>();
                leaf->name = entry.fileName();
                leaf->path = entry.absoluteFilePath();
                leaf->isAudio = false;
                leaf->isOrphanedMd = true;
                leaf->status = OrphanedMdStatus;
                leaf->mdModified = entry.lastModified().toMSecsSinceEpoch();
                leaf->parent = fresh.get();
                fresh->children.push_back(std::move(leaf));
            }
        }

        appendCombinePlaceholders(fresh.get(), rootPath);
    }

    // Watched dirs track the filesystem regardless of whether the MODEL changed
    // — a rescan that found nothing structural still has to keep watching.
    if (!m_watcher.directories().isEmpty())
        m_watcher.removePaths(m_watcher.directories());
    if (!watchedDirs.isEmpty())
        m_watcher.addPaths(watchedDirs);

    if (!forceReset && sameStructure(fresh.get(), m_root.get())) {
        // Nothing structural changed — this is the common case for the watcher
        // echo of a delete/undo we already rebuilt for synchronously. Refresh
        // any row whose sidecar .md was rewritten in place, and leave every
        // delegate, expansion state and scroll position alone.
        emitContentChanges(fresh.get(), m_root.get());
        return;
    }

    beginResetModel();
    m_root = std::move(fresh);
    endResetModel();
}

bool VaultTreeModel::sameStructure(const Node *a, const Node *b)
{
    if (!a || !b)
        return a == b;
    if (a->name != b->name || a->path != b->path || a->isDir != b->isDir
        || a->isAudio != b->isAudio || a->isOrphanedMd != b->isOrphanedMd
        || a->isCombinePlaceholder != b->isCombinePlaceholder
        || a->combineSourceCount != b->combineSourceCount)
        return false;
    if (a->children.size() != b->children.size())
        return false;
    for (size_t i = 0; i < a->children.size(); ++i) {
        if (!sameStructure(a->children[i].get(), b->children[i].get()))
            return false;
    }
    return true;
}

void VaultTreeModel::emitContentChanges(const Node *fresh, Node *current)
{
    int first = -1;
    int last = -1;
    for (size_t i = 0; i < current->children.size(); ++i) {
        Node *cur = current->children[i].get();
        const Node *nf = fresh->children[i].get();
        if (cur->mdModified != nf->mdModified || cur->status != nf->status) {
            cur->mdModified = nf->mdModified;
            // Status is deliberately NOT part of sameStructure(): a sidecar
            // appearing or disappearing changes this row's indicator colour,
            // never the tree's shape. Refresh it in place so a finished
            // transcription (or a Delete transcript) never resets the model
            // and never costs the tree its expansion state and scroll offset.
            cur->status = nf->status;
            if (first < 0)
                first = int(i);
            last = int(i);
        }
        emitContentChanges(nf, cur);
    }
    if (first < 0)
        return;
    // One dataChanged spanning the changed range, not one per row — the roles
    // that read the .md are LabelRole/TagsRole/SpeakersRole/TranscriptBodyRole,
    // plus StatusRole for a sidecar appearing/disappearing (transcribed dot),
    // which is intentionally content-not-structure here (see sameStructure());
    // everything else came from the structure we just proved equal.
    const QModelIndex parent = indexForNode(current);
    emit dataChanged(index(first, 0, parent), index(last, 0, parent),
                     {StatusRole, LabelRole, TagsRole, SpeakersRole, TranscriptBodyRole});
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
        } else if (isHiddenByPendingCombine(entry.absoluteFilePath())) {
            continue; // see rebuild()'s identical branch
        } else if (isAudioFile(entry)) {
            auto leaf = std::make_unique<Node>();
            leaf->name = entry.fileName();
            leaf->path = entry.absoluteFilePath();
            leaf->isAudio = true;
            const QString mdPath = entry.absolutePath() + '/' + entry.fileName() + ".md";
            const QFileInfo mdInfo(mdPath);
            leaf->status = mdInfo.exists() ? TranscribedStatus : UntranscribedStatus;
            // Same stat, one extra field — see rebuild()'s identical branch.
            leaf->mdModified = mdInfo.exists() ? mdInfo.lastModified().toMSecsSinceEpoch() : 0;
            leaf->parent = node.get();
            node->children.push_back(std::move(leaf));
        } else if (entry.suffix().toLower() == "md" && !audioBaseNames.contains(entry.completeBaseName())) {
            auto leaf = std::make_unique<Node>();
            leaf->name = entry.fileName();
            leaf->path = entry.absoluteFilePath();
            leaf->isAudio = false;
            leaf->isOrphanedMd = true;
            leaf->status = OrphanedMdStatus;
            leaf->mdModified = entry.lastModified().toMSecsSinceEpoch();
            leaf->parent = node.get();
            node->children.push_back(std::move(leaf));
        }
    }

    appendCombinePlaceholders(node.get(), dirPath);
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

VaultTreeModel::Status VaultTreeModel::effectiveStatus(const Node *node) const
{
    if (node->status != TranscribedStatus)
        return node->status;
    const QString mdPath = sidecarMdPath(node);
    if (!mdPath.isEmpty() && MdFrontmatter::isTrue(frontmatterField(mdPath, QStringLiteral("partial"))))
        return PartialStatus;
    return TranscribedStatus;
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
        return static_cast<int>(effectiveStatus(node));
    case IsOrphanedMdRole:
        return node->isOrphanedMd;
    case IsCombinePlaceholderRole:
        return node->isCombinePlaceholder;
    case CombineSourceCountRole:
        return node->combineSourceCount;
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
        {IsCombinePlaceholderRole, "isCombinePlaceholder"},
        {CombineSourceCountRole, "combineSourceCount"},
    };
}

bool VaultTreeModel::renameFile(const QUrl &fileUrl, const QString &newBaseName, QString *errorOut)
{
    // The filesystem rename now lives in renameRecordingFile() (FileRename.h)
    // so a worker thread (RenameFileTask) can share it; this instance method
    // is the synchronous wrapper that also refreshes the model.
    if (!renameRecordingFile(fileUrl, newBaseName, nullptr, errorOut))
        return false;
    rebuild();
    return true;
}

QVariantMap VaultTreeModel::inspectImportCandidate(const QUrl &fileUrl) const
{
    auto make = [](const char *reason, const QString &dest, const QString &name) {
        return QVariantMap{{QStringLiteral("reason"), QString::fromLatin1(reason)},
                           {QStringLiteral("dest"), dest},
                           {QStringLiteral("name"), name}};
    };

    const QString sourcePath = fileUrl.toLocalFile();
    const QFileInfo sourceInfo(sourcePath);
    const QString name = sourceInfo.fileName();

    const QString localPath = vaultLocalPath();
    if (localPath.isEmpty())
        return make("no-vault", QString(), name);
    if (!sourceInfo.exists() || !sourceInfo.isFile())
        return make("not-a-file", QString(), name);
    if (!isAudioFile(sourceInfo))
        return make("not-audio", QString(), name);

    const QString destPath = localPath + '/' + sourceInfo.fileName();
    if (QFileInfo::exists(destPath))
        return make("collision", destPath, name);
    return make("ok", destPath, name);
}

QString VaultTreeModel::importDestinationFor(const QUrl &fileUrl, QString *errorOut) const
{
    // No copy here — the actual (potentially slow, on a large file) byte-copy
    // runs on TranscriptionController's worker thread (see importAudio()), and
    // the new file appearing in the watched vault dir fires
    // QFileSystemWatcher::directoryChanged -> rebuild(), refreshing the tree
    // without an explicit rebuild() on this (const) validation path.
    const QVariantMap v = inspectImportCandidate(fileUrl);
    const QString reason = v.value(QStringLiteral("reason")).toString();
    const QString name = v.value(QStringLiteral("name")).toString();
    if (reason == QLatin1String("ok")) {
        if (errorOut)
            errorOut->clear();
        return v.value(QStringLiteral("dest")).toString();
    }
    if (errorOut) {
        if (reason == QLatin1String("no-vault"))
            *errorOut = QStringLiteral("No vault is open.");
        else if (reason == QLatin1String("not-a-file"))
            *errorOut = QStringLiteral("Not a file: %1").arg(name);
        else if (reason == QLatin1String("not-audio"))
            *errorOut = QStringLiteral("Not a recognized audio file: %1").arg(name);
        else // collision
            *errorOut = QStringLiteral("\"%1\" already exists in the vault.").arg(name);
    }
    return QString();
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

bool VaultTreeModel::renameFolder(const QUrl &folderUrl, const QString &newName, QString *errorOut)
{
    // Thin wrapper over the pure filesystem rename (shared with FolderRenameTask's
    // worker-thread path) + an inline rebuild — mirrors moveFile()/renameFile().
    // QML routes folder rename through TranscriptionController::renameFolder() (an
    // instant-lane task); this stays for direct/synchronous callers + tests.
    if (!renameVaultFolder(folderUrl, newName, vaultLocalPath(), nullptr, errorOut))
        return false;
    rebuild();
    return true;
}

bool VaultTreeModel::moveFile(const QUrl &fileUrl, const QUrl &destDirUrl)
{
    // Thin wrapper over the pure filesystem move (shared with MoveTask's
    // worker-thread path) + an inline rebuild — mirrors renameFile(). Direct
    // callers/tests and the Sidebar's no-controller fallback use this; the
    // controller's moveFiles() runs moveRecordingFile() itself off-thread.
    if (!moveRecordingFile(fileUrl, destDirUrl, vaultLocalPath(), nullptr, nullptr))
        return false;
    rebuild();
    return true;
}

bool VaultTreeModel::moveFolder(const QUrl &folderUrl, const QUrl &destParentUrl)
{
    // Thin wrapper over the pure filesystem move (shared with FolderMoveTask) +
    // an inline rebuild — mirrors moveFile(). QML routes folder move through
    // TranscriptionController::moveFolder() (an instant-lane task); this stays
    // for direct/synchronous callers + tests.
    if (!moveVaultFolder(folderUrl, destParentUrl, vaultLocalPath(), nullptr, nullptr))
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

QVariantMap VaultTreeModel::makeTrashResult(int trashed, const QVariantList &restorable,
                                            const QVariantList &untrashable, const QVariantList &invalid)
{
    return QVariantMap{ { QStringLiteral("trashed"), trashed },
                        { QStringLiteral("restorable"), restorable },
                        { QStringLiteral("untrashable"), untrashable },
                        { QStringLiteral("invalid"), invalid } };
}

QVariantMap VaultTreeModel::trashFiles(const QList<QUrl> &urls)
{
    // Thin wrapper: the pure batch does the filesystem work off any model, then
    // applyRemovalOutcome() does the single rebuild() + the per-file signals.
    TrashBatchResult r = trashVaultEntries(urls, VaultFileScope::Files, vaultLocalPath());
    applyRemovalOutcome(VaultFileScope::Files, r.trashed);
    return makeTrashResult(int(r.trashed.size()), r.restorable, r.untrashable, r.invalid);
}

int VaultTreeModel::deleteFilesPermanently(const QList<QUrl> &urls)
{
    auto removed = deleteVaultEntriesPermanently(urls, VaultFileScope::Files, vaultLocalPath());
    applyRemovalOutcome(VaultFileScope::Files, removed);
    return removed.size();
}

QVariantMap VaultTreeModel::trashFolder(const QUrl &folderUrl)
{
    TrashBatchResult r = trashVaultEntries({ folderUrl }, VaultFileScope::Folder, vaultLocalPath());
    applyRemovalOutcome(VaultFileScope::Folder, r.trashed);
    return makeTrashResult(int(r.trashed.size()), r.restorable, r.untrashable, r.invalid);
}

bool VaultTreeModel::deleteFolderPermanently(const QUrl &folderUrl)
{
    auto removed = deleteVaultEntriesPermanently({ folderUrl }, VaultFileScope::Folder, vaultLocalPath());
    applyRemovalOutcome(VaultFileScope::Folder, removed);
    return !removed.isEmpty();
}

QVariantMap VaultTreeModel::trashTranscript(const QList<QUrl> &urls)
{
    TrashBatchResult r = trashVaultEntries(urls, VaultFileScope::Transcript, vaultLocalPath());
    applyRemovalOutcome(VaultFileScope::Transcript, r.trashed);
    return makeTrashResult(int(r.trashed.size()), r.restorable, r.untrashable, r.invalid);
}

int VaultTreeModel::deleteTranscriptPermanently(const QList<QUrl> &urls)
{
    auto removed = deleteVaultEntriesPermanently(urls, VaultFileScope::Transcript, vaultLocalPath());
    applyRemovalOutcome(VaultFileScope::Transcript, removed);
    return removed.size();
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

QVariantMap VaultTreeModel::trashAudioOnly(const QList<QUrl> &urls)
{
    TrashBatchResult r = trashVaultEntries(urls, VaultFileScope::AudioOnly, vaultLocalPath());
    applyRemovalOutcome(VaultFileScope::AudioOnly, r.trashed);
    return makeTrashResult(int(r.trashed.size()), r.restorable, r.untrashable, r.invalid);
}

int VaultTreeModel::deleteAudioOnlyPermanently(const QList<QUrl> &urls)
{
    auto removed = deleteVaultEntriesPermanently(urls, VaultFileScope::AudioOnly, vaultLocalPath());
    applyRemovalOutcome(VaultFileScope::AudioOnly, removed);
    return int(removed.size());
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
    // Thin wrapper: the pure restore does the filesystem work off any model,
    // then applyRestoreOutcome() does the single rebuild() + restore signals.
    QStringList restored = restoreVaultEntries(entries, vaultLocalPath());
    applyRestoreOutcome(restored);
    return int(restored.size());
}

void VaultTreeModel::applyRemovalOutcome(VaultFileScope scope, const QList<RemovedEntry> &removed)
{
    if (removed.isEmpty())
        return; // nothing changed — no rebuild, no signals

    // One rebuild() for the whole batch, not one per entry: rebuild() rescans
    // the vault recursively AND resets the model (destroying/recreating every
    // realized delegate), so paying it per entry would cost 20x over for a
    // single 20-file user action. Signals come AFTER, so a listener that
    // re-reads the tree (MainPanel closing the deleted file) never sees a model
    // still advertising rows whose files are already gone.
    rebuild();
    for (const RemovedEntry &entry : removed) {
        const QUrl &url = entry.first;
        switch (scope) {
        case VaultFileScope::Files:
            // Per surviving file, not once for the batch — fileDeleted's
            // contract is one file per emission, and MainPanel matches it
            // against the file it has open.
            emit fileDeleted(url);
            break;
        case VaultFileScope::Transcript:
            emit transcriptDeleted(url);
            break;
        case VaultFileScope::AudioOnly:
            // audioDeletedTranscriptKept if the .md survived, plain fileDeleted
            // if there was nothing to keep — mdPath was resolved before the
            // audio moved (see VaultFileOps' trashAudioOnlyEntry()).
            emitAudioOnlyDeleted(url, entry.second);
            break;
        case VaultFileScope::Folder:
            break; // a folder delete emits no per-file signal — the rebuild covers it
        }
    }
}

void VaultTreeModel::applyRestoreOutcome(const QStringList &restoredPaths)
{
    if (restoredPaths.isEmpty())
        return;

    rebuild();
    // After the single rebuild, never between — the same ordering rule the
    // trash side follows: a listener that re-reads the tree must not see it
    // still advertising the pre-restore shape.
    emitRestored(restoredPaths);
}

void VaultTreeModel::applyRenameOutcome(const QUrl &oldUrl, const QUrl &newUrl)
{
    const QString oldPath = oldUrl.toLocalFile();
    const QString newPath = newUrl.toLocalFile();
    if (oldPath.isEmpty() || newPath.isEmpty() || oldPath == newPath)
        return;

    Node *node = findNodeByPath(m_root.get(), oldPath);
    // Unknown to the tree (e.g. spoken for by a pending combine, or a folder
    // rename which routes elsewhere): leave the watcher rebuild to reconcile.
    if (!node || !node->parent)
        return;

    Node *parent = node->parent;
    std::vector<std::unique_ptr<Node>> &siblings = parent->children;

    int oldRow = -1;
    for (size_t i = 0; i < siblings.size(); ++i) {
        if (siblings[i].get() == node) {
            oldRow = int(i);
            break;
        }
    }
    if (oldRow < 0)
        return;

    node->name = QFileInfo(newPath).fileName();
    node->path = newPath;

    // The row's new slot, matching scanDirectory()'s QDir::DirsFirst | QDir::Name
    // (case-sensitive) order: count the siblings that sort BEFORE the renamed
    // node. All others are already in that order and only this node's name
    // changed, so a single forward scan gives the insertion index.
    int newRow = 0;
    for (size_t i = 0; i < siblings.size(); ++i) {
        if (int(i) == oldRow)
            continue;
        const Node *other = siblings[i].get();
        const bool otherSortsFirst =
            (other->isDir != node->isDir)
                ? other->isDir
                : other->name.compare(node->name, Qt::CaseSensitive) < 0;
        if (otherSortsFirst)
            ++newRow;
        else
            break;
    }

    const QModelIndex parentIdx = indexForNode(parent);
    if (newRow != oldRow) {
        // beginMoveRows' destinationChild is in PRE-move coordinates: moving
        // down shifts by one because the source row vacates first.
        const int dest = (newRow > oldRow) ? newRow + 1 : newRow;
        if (beginMoveRows(parentIdx, oldRow, oldRow, parentIdx, dest)) {
            std::unique_ptr<Node> owned = std::move(siblings[oldRow]);
            siblings.erase(siblings.begin() + oldRow);
            siblings.insert(siblings.begin() + newRow, std::move(owned));
            endMoveRows();
        }
    }

    // Repaint the row's text at its (possibly new) position. All roles, since
    // both the display name and the path changed.
    const QModelIndex idx = indexForNode(node);
    if (idx.isValid())
        emit dataChanged(idx, idx);
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
    // Delegates to the pure free function (shared with the worker-thread move
    // helper in FileRename.cpp) so there is ONE implementation of the
    // canonical-prefix / not-yet-existing-path logic.
    return pathIsInsideVault(candidatePath, vaultLocalPath());
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

    // Atomic (QSaveFile): a crash/full-disk mid-write leaves the previous
    // settings.json intact rather than truncating it to nothing.
    QSaveFile file(configDir.filePath(QStringLiteral("settings.json")));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (errorOut)
            *errorOut = QStringLiteral("could not open %1 for writing").arg(file.fileName());
        return false;
    }

    const QJsonDocument doc(QJsonObject::fromVariantMap(settings));
    file.write(doc.toJson(QJsonDocument::Indented));
    if (!file.commit()) {
        if (errorOut)
            *errorOut = QStringLiteral("could not write %1 (%2)").arg(file.fileName(), file.errorString());
        return false;
    }
    return true;
}

QVariantList VaultTreeModel::readHistoryFile(const QString &fileName) const
{
    QVariantList result;
    const QString localPath = vaultLocalPath();
    if (localPath.isEmpty())
        return result;

    QFile file(localPath + QStringLiteral("/.transcriber/") + fileName);
    if (!file.open(QIODevice::ReadOnly))
        return result;

    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    if (!doc.isArray())
        return result;
    return doc.array().toVariantList();
}

bool VaultTreeModel::appendHistoryEntry(const QString &fileName, const QVariantMap &entry, QString *errorOut) const
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

    const QString historyPath = configDir.filePath(fileName);

    QJsonArray history;
    QFile readFile(historyPath);
    if (readFile.open(QIODevice::ReadOnly)) {
        const QJsonDocument existingDoc = QJsonDocument::fromJson(readFile.readAll());
        readFile.close();
        if (existingDoc.isArray())
            history = existingDoc.array();
    }

    history.append(QJsonObject::fromVariantMap(entry));

    QSaveFile writeFile(historyPath);
    if (!writeFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (errorOut)
            *errorOut = QStringLiteral("could not open %1 for writing").arg(writeFile.fileName());
        return false;
    }

    const QJsonDocument doc(history);
    writeFile.write(doc.toJson(QJsonDocument::Indented));
    if (!writeFile.commit()) {
        if (errorOut)
            *errorOut = QStringLiteral("could not write %1 (%2)").arg(writeFile.fileName(), writeFile.errorString());
        return false;
    }
    return true;
}

QVariantList VaultTreeModel::readUsbGrabHistory() const
{
    return readHistoryFile(QStringLiteral("usbGrabHistory.json"));
}

bool VaultTreeModel::appendUsbGrabHistoryEntry(const QVariantMap &entry, QString *errorOut) const
{
    return appendHistoryEntry(QStringLiteral("usbGrabHistory.json"), entry, errorOut);
}

QVariantList VaultTreeModel::readCombineHistory() const
{
    return readHistoryFile(QStringLiteral("combineHistory.json"));
}

bool VaultTreeModel::appendCombineHistoryEntry(const QVariantMap &entry, QString *errorOut) const
{
    return appendHistoryEntry(QStringLiteral("combineHistory.json"), entry, errorOut);
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
    QSaveFile file(configDir.filePath(QStringLiteral("benchmarkLastRun.json")));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (errorOut)
            *errorOut = QStringLiteral("could not open %1 for writing").arg(file.fileName());
        return false;
    }

    const QJsonDocument doc(QJsonObject::fromVariantMap(run));
    file.write(doc.toJson(QJsonDocument::Indented));
    if (!file.commit()) {
        if (errorOut)
            *errorOut = QStringLiteral("could not write %1 (%2)").arg(file.fileName(), file.errorString());
        return false;
    }
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
            entry[QStringLiteral("status")] = static_cast<int>(effectiveStatus(node));
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
