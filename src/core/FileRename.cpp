#include "FileRename.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>

#include <utility>
#include <vector>

#include "AudioExtensions.h"

bool renameRecordingFile(const QUrl &fileUrl, const QString &newBaseName, QUrl *newUrlOut, QString *errorOut)
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
        // The sidecar's name is the FULL audio filename + ".md",
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

    // The renamed PRIMARY file's new URL: the audio if there was one, else the
    // orphaned .md that was renamed directly.
    if (newUrlOut)
        *newUrlOut = QUrl::fromLocalFile(newAudioPath.isEmpty() ? newMdPath : newAudioPath);
    return true;
}

bool isSameRecording(const QUrl &a, const QUrl &b)
{
    if (a == b)
        return true;
    if (a.isEmpty() || b.isEmpty())
        return false;
    const QFileInfo fa(a.toLocalFile());
    const QFileInfo fb(b.toLocalFile());
    // Same directory + same stem (completeBaseName strips only the final
    // extension: "foo.wav"->"foo", "foo.md"->"foo").
    return fa.absolutePath() == fb.absolutePath()
        && fa.completeBaseName() == fb.completeBaseName();
}

QString vaultRelativePathOf(const QString &candidatePath, const QString &vaultLocalPath)
{
    if (vaultLocalPath.isEmpty() || candidatePath.isEmpty())
        return QString();

    // The vault root itself must exist (it always does once opened) for
    // canonicalization to mean anything.
    const QString vaultCanonical = QFileInfo(vaultLocalPath).canonicalFilePath();
    if (vaultCanonical.isEmpty())
        return QString();

    // candidatePath may not exist yet (e.g. a folder about to be created) —
    // QFileInfo::canonicalFilePath() returns empty for a nonexistent path, so
    // walk up to the nearest existing ancestor, canonicalize THAT (resolving
    // any ".."/symlinks along the existing portion), then re-append the
    // not-yet-created tail.
    QString ancestor = QFileInfo(candidatePath).absoluteFilePath();
    QString tail;
    while (!QFileInfo::exists(ancestor)) {
        const QString parent = QFileInfo(ancestor).absolutePath();
        if (parent == ancestor)
            return QString(); // walked past the filesystem root without finding anything real
        const QString name = QFileInfo(ancestor).fileName();
        tail = tail.isEmpty() ? name : name + '/' + tail;
        ancestor = parent;
    }

    const QString ancestorCanonical = QFileInfo(ancestor).canonicalFilePath();
    if (ancestorCanonical.isEmpty())
        return QString();
    // Lexically cleaning is safe only here: the ancestor is symlink-free, and a
    // ".." in the not-yet-existing tail can't follow a link.
    const QString candidateCanonical =
        tail.isEmpty() ? ancestorCanonical : QDir::cleanPath(ancestorCanonical + '/' + tail);

    if (candidateCanonical == vaultCanonical)
        return QStringLiteral(".");

    const QString relative = QDir(vaultCanonical).relativeFilePath(candidateCanonical);
    // Component test, not a prefix test: "..foo" is a legal child name.
    if (relative == QStringLiteral("..") || relative.startsWith(QStringLiteral("../")) || QDir::isAbsolutePath(relative))
        return QString();
    return relative;
}

bool pathIsInsideVault(const QString &candidatePath, const QString &vaultLocalPath)
{
    return !vaultRelativePathOf(candidatePath, vaultLocalPath).isEmpty();
}

bool moveRecordingFile(const QUrl &fileUrl, const QUrl &destDirUrl, const QString &vaultLocalPath, QUrl *newUrlOut,
                       QString *errorOut)
{
    auto fail = [&](const QString &msg) {
        if (errorOut)
            *errorOut = msg;
        return false;
    };

    const QString srcPath = fileUrl.toLocalFile();
    const QString destDir = destDirUrl.toLocalFile();
    if (srcPath.isEmpty() || destDir.isEmpty())
        return fail(QStringLiteral("Missing source or destination."));

    const QFileInfo srcInfo(srcPath);
    if (!srcInfo.exists() || !srcInfo.isFile())
        return fail(QStringLiteral("File does not exist."));

    const QFileInfo destDirInfo(destDir);
    if (!destDirInfo.exists() || !destDirInfo.isDir())
        return fail(QStringLiteral("Destination folder does not exist."));

    // Both ends must stay inside the vault.
    if (!pathIsInsideVault(srcPath, vaultLocalPath) || !pathIsInsideVault(destDir, vaultLocalPath))
        return fail(QStringLiteral("Both source and destination must be inside the vault."));

    // No-op guard: destination is the file's current directory.
    if (QFileInfo(srcInfo.absolutePath()).canonicalFilePath() == destDirInfo.canonicalFilePath())
        return fail(QStringLiteral("File is already in that folder."));

    // The set of files that move together: the file itself, plus — when it's
    // an audio file — its <fileName>.md sidecar and, if present, its
    // .<fileName>.partial.jsonl resume state (same full-filename convention as
    // sidecarMdPath()). A standalone .md moves alone.
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
    // half-done move never splits the audio/.md set across two folders.
    for (const auto &m : moves) {
        if (QFileInfo::exists(m.second))
            return fail(QStringLiteral("A file with that name already exists in the destination."));
    }

    std::vector<std::pair<QString, QString>> done;
    for (const auto &m : moves) {
        if (!QFile::rename(m.first, m.second)) {
            for (const auto &d : done)
                QFile::rename(d.second, d.first); // best-effort rollback
            return fail(QStringLiteral("Failed to move file."));
        }
        done.push_back(m);
    }

    if (newUrlOut)
        *newUrlOut = QUrl::fromLocalFile(destDir + '/' + srcInfo.fileName());
    return true;
}

bool isValidVaultEntryName(const QString &name)
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
    // than a new child — reject explicitly rather than rely on a collision check.
    return trimmed != QStringLiteral(".") && trimmed != QStringLiteral("..");
}

bool renameVaultFolder(const QUrl &folderUrl, const QString &newName, const QString &vaultLocalPath, QUrl *newUrlOut,
                       QString *errorOut)
{
    auto fail = [&](const QString &msg) {
        if (errorOut)
            *errorOut = msg;
        return false;
    };

    if (!isValidVaultEntryName(newName))
        return fail(QStringLiteral("Invalid folder name."));
    const QString trimmed = newName.trimmed();

    const QString path = folderUrl.toLocalFile();
    if (path.isEmpty())
        return fail(QStringLiteral("Folder does not exist."));

    const QFileInfo info(path);
    if (!info.exists() || !info.isDir())
        return fail(QStringLiteral("Folder does not exist."));

    const QString canonical = info.canonicalFilePath();
    const QString vaultCanonical = QFileInfo(vaultLocalPath).canonicalFilePath();
    if (canonical.isEmpty() || vaultCanonical.isEmpty())
        return fail(QStringLiteral("Folder does not exist."));
    // Never the vault root itself (renaming it would move the whole vault out
    // from under the open window), and never anything outside the vault.
    if (canonical == vaultCanonical)
        return fail(QStringLiteral("Can't rename the vault root."));
    if (!pathIsInsideVault(path, vaultLocalPath))
        return fail(QStringLiteral("Folder is outside the vault."));

    // A pure rename in place — the parent never changes (moveVaultFolder() owns
    // reparenting), so a validated name can only ever land beside itself.
    const QString newPath = info.absolutePath() + '/' + trimmed;
    if (newPath == info.absoluteFilePath()) {
        if (newUrlOut)
            *newUrlOut = folderUrl;
        return true; // no-op: same name, not a failure worth reporting
    }
    if (QFileInfo::exists(newPath))
        return fail(QStringLiteral("A folder with that name already exists."));

    if (!QDir().rename(path, newPath))
        return fail(QStringLiteral("Failed to rename folder."));

    if (newUrlOut)
        *newUrlOut = QUrl::fromLocalFile(newPath);
    return true;
}

bool moveVaultFolder(const QUrl &folderUrl, const QUrl &destParentUrl, const QString &vaultLocalPath, QUrl *newUrlOut,
                     QString *errorOut)
{
    auto fail = [&](const QString &msg) {
        if (errorOut)
            *errorOut = msg;
        return false;
    };

    const QString srcPath = folderUrl.toLocalFile();
    const QString destParent = destParentUrl.toLocalFile();
    if (srcPath.isEmpty() || destParent.isEmpty())
        return fail(QStringLiteral("Missing source or destination."));

    const QFileInfo srcInfo(srcPath);
    if (!srcInfo.exists() || !srcInfo.isDir())
        return fail(QStringLiteral("Folder does not exist."));
    const QFileInfo destParentInfo(destParent);
    if (!destParentInfo.exists() || !destParentInfo.isDir())
        return fail(QStringLiteral("Destination folder does not exist."));

    const QString srcCanonical = srcInfo.canonicalFilePath();
    const QString destParentCanonical = destParentInfo.canonicalFilePath();
    const QString vaultCanonical = QFileInfo(vaultLocalPath).canonicalFilePath();
    if (srcCanonical.isEmpty() || destParentCanonical.isEmpty() || vaultCanonical.isEmpty())
        return fail(QStringLiteral("Folder does not exist."));

    // Never move the vault root itself, and keep both ends inside the vault.
    if (srcCanonical == vaultCanonical)
        return fail(QStringLiteral("Can't move the vault root."));
    if (!pathIsInsideVault(srcPath, vaultLocalPath) || !pathIsInsideVault(destParent, vaultLocalPath))
        return fail(QStringLiteral("Both source and destination must be inside the vault."));

    // Refuse moving a folder into itself or one of its own descendants — that
    // would orphan the subtree.
    if (destParentCanonical == srcCanonical || destParentCanonical.startsWith(srcCanonical + '/'))
        return fail(QStringLiteral("Can't move a folder into itself."));

    // No-op guard: already directly inside destParent.
    if (QFileInfo(srcInfo.absolutePath()).canonicalFilePath() == destParentCanonical)
        return fail(QStringLiteral("Folder is already there."));

    const QString destPath = destParent + '/' + srcInfo.fileName();
    if (QFileInfo::exists(destPath))
        return fail(QStringLiteral("A folder with that name already exists at the destination."));

    if (!QDir().rename(srcPath, destPath))
        return fail(QStringLiteral("Failed to move folder."));

    if (newUrlOut)
        *newUrlOut = QUrl::fromLocalFile(destPath);
    return true;
}
