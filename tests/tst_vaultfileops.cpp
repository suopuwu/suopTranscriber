// VaultFileOps — the pure filesystem trash/permanent-delete/restore layer
// underneath every Trash/Delete/Restore Task. Everything else that touches it
// goes through a Task (tst_task_trash), which only ever drives the happy path
// of the Files and Transcript scopes. This file exercises the load-bearing
// safety properties directly: the containment guard set (outside-the-vault,
// the vault root itself, a folder/file crossing scopes, a `..`-traversal),
// the recursive Folder scope, AudioOnly's kept-.md contract, the untrashable
// lane, and restoreVaultEntries()'s own containment + skip cases.
//
// Pure functions taking vaultLocalPath explicitly, so no model, no engine, no
// QML — QTEST_GUILESS_MAIN, no TaskContext. Every operation below is confined
// to a QTemporaryDir; the tests that actually move something to the trash
// (real system trash — VaultFileOps calls QFile::moveToTrash() directly, with
// no seam to fake it) restore it before returning, mirroring tst_task_trash.cpp.

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QUrl>
#include <QtTest>

#include "VaultFileOps.h"

namespace {

QString writeFile(const QString &path, const QByteArray &content)
{
    QFile f(path);
    f.open(QIODevice::WriteOnly);
    f.write(content);
    f.close();
    return path;
}

// Mirrors VaultFileOps.cpp's file-local trashInfoPathFor() (not exported) so
// tests can find and clean up the freedesktop .trashinfo record a real trash
// leaves behind. Only meaningful for a spec-shaped "<root>/files/<name>"
// trashPath; returns empty otherwise, same as the production helper.
QString trashInfoPathForTest(const QString &trashedFilePath)
{
    const QFileInfo info(trashedFilePath);
    QDir filesDir(info.absolutePath());
    if (filesDir.dirName() != QLatin1String("files"))
        return {};
    filesDir.cdUp();
    return filesDir.absoluteFilePath(QStringLiteral("info/") + info.fileName() + QStringLiteral(".trashinfo"));
}

} // namespace

class TestVaultFileOps : public QObject {
    Q_OBJECT
private slots:
    void cleanup()
    {
        // The untrashable-lane test locks a directory's write permission to
        // force a deterministic moveToTrash() failure; restore it here (even
        // on an early QVERIFY failure) so QTemporaryDir's own teardown can
        // still remove the tree instead of leaking a permission-mangled dir.
        if (!m_permissionLockedDir.isEmpty()) {
            QFile::setPermissions(m_permissionLockedDir,
                                  QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner
                                  | QFileDevice::ReadGroup | QFileDevice::ExeGroup
                                  | QFileDevice::ReadOther | QFileDevice::ExeOther);
            m_permissionLockedDir.clear();
        }
    }

    // ---- Containment guards (guardedLocalPath(), VaultFileOps.cpp:127-160) ----

    void trash_refusesAPathOutsideTheVault_asInvalid()
    {
        QTemporaryDir vault;
        QTemporaryDir outside;
        QVERIFY(vault.isValid());
        QVERIFY(outside.isValid());
        const QString file = writeFile(outside.path() + "/intruder.wav", "x");

        TrashBatchResult result = trashVaultEntries({QUrl::fromLocalFile(file)}, VaultFileScope::Files, vault.path());

        QCOMPARE(result.trashed.size(), 0);
        QCOMPARE(result.invalid.size(), 1);
        QCOMPARE(result.invalid.first().toString(), QUrl::fromLocalFile(file).toString());
        QVERIFY(QFileInfo::exists(file));
    }

    void trash_refusesTheVaultRootItself()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());

        TrashBatchResult result = trashVaultEntries({QUrl::fromLocalFile(vault.path())}, VaultFileScope::Folder,
                                                     vault.path());

        QCOMPARE(result.trashed.size(), 0);
        QCOMPARE(result.invalid.size(), 1);
        QVERIFY(QDir(vault.path()).exists());
    }

    void trash_refusesAFolderPassedThroughTheFileScope()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        const QString folder = vault.path() + "/subfolder";
        QVERIFY(QDir().mkpath(folder));

        TrashBatchResult result = trashVaultEntries({QUrl::fromLocalFile(folder)}, VaultFileScope::Files,
                                                     vault.path());

        QCOMPARE(result.trashed.size(), 0);
        QCOMPARE(result.invalid.size(), 1);
        QVERIFY(QDir(folder).exists());
    }

    void trash_refusesAFilePassedThroughTheFolderScope()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        const QString file = writeFile(vault.path() + "/lonely.wav", "x");

        TrashBatchResult result = trashVaultEntries({QUrl::fromLocalFile(file)}, VaultFileScope::Folder,
                                                     vault.path());

        QCOMPARE(result.trashed.size(), 0);
        QCOMPARE(result.invalid.size(), 1);
        QVERIFY(QFileInfo::exists(file));
    }

    void trash_refusesANonexistentPath_asInvalid()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        const QString ghost = vault.path() + "/never-existed.wav";

        TrashBatchResult result = trashVaultEntries({QUrl::fromLocalFile(ghost)}, VaultFileScope::Files,
                                                     vault.path());

        QCOMPARE(result.trashed.size(), 0);
        QCOMPARE(result.invalid.size(), 1);
    }

    void delete_refusesAPathOutsideTheVault()
    {
        QTemporaryDir vault;
        QTemporaryDir outside;
        QVERIFY(vault.isValid());
        QVERIFY(outside.isValid());
        const QString file = writeFile(outside.path() + "/intruder.wav", "x");

        const QList<RemovedEntry> removed = deleteVaultEntriesPermanently(
            {QUrl::fromLocalFile(file)}, VaultFileScope::Files, vault.path());

        QCOMPARE(removed.size(), 0);
        QVERIFY(QFileInfo::exists(file));
    }

    void delete_refusesTheVaultRootItself()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());

        const QList<RemovedEntry> removed = deleteVaultEntriesPermanently(
            {QUrl::fromLocalFile(vault.path())}, VaultFileScope::Folder, vault.path());

        QCOMPARE(removed.size(), 0);
        QVERIFY(QDir(vault.path()).exists());
    }

    // A `../` traversal: the guard canonicalises (VaultFileOps.cpp:146-147),
    // so a path that only LOOKS like it's inside the vault by string prefix
    // must still be refused once resolved.
    void trash_refusesADotDotTraversalOutsideTheVault()
    {
        QTemporaryDir base;
        QVERIFY(base.isValid());
        const QString vaultPath = base.path() + "/vault";
        const QString siblingPath = base.path() + "/sibling";
        QVERIFY(QDir().mkpath(vaultPath));
        QVERIFY(QDir().mkpath(siblingPath));
        const QString file = writeFile(siblingPath + "/escape.wav", "x");

        const QUrl traversalUrl = QUrl::fromLocalFile(vaultPath + "/../sibling/escape.wav");
        TrashBatchResult result = trashVaultEntries({traversalUrl}, VaultFileScope::Files, vaultPath);

        QCOMPARE(result.trashed.size(), 0);
        QCOMPARE(result.invalid.size(), 1);
        QVERIFY(QFileInfo::exists(file));
    }

    // ---- Folder scope ----------------------------------------------------

    void trash_folderScope_takesTheWholeTreeAndIsRestorable()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        const QString folder = vault.path() + "/sessions";
        const QString sub = folder + "/nested";
        QVERIFY(QDir().mkpath(sub));
        const QString a = writeFile(folder + "/a.wav", "a");
        writeFile(folder + "/a.wav.md", "---\n---\n\n[0] a\n");
        const QString b = writeFile(sub + "/b.wav", "b");
        writeFile(sub + "/b.wav.md", "---\n---\n\n[0] b\n");

        TrashBatchResult result = trashVaultEntries({QUrl::fromLocalFile(folder)}, VaultFileScope::Folder,
                                                     vault.path());

        QCOMPARE(result.invalid.size(), 0);
        QCOMPARE(result.trashed.size(), 1); // one user-visible entry: the folder itself
        QVERIFY(!QDir(folder).exists());
        QVERIFY(!result.restorable.isEmpty());

        const QStringList restored = restoreVaultEntries(result.restorable, vault.path());
        QCOMPARE(restored.size(), result.restorable.size());
        QVERIFY(QDir(folder).exists());
        QVERIFY(QFileInfo::exists(a));
        QVERIFY(QFileInfo::exists(folder + "/a.wav.md"));
        QVERIFY(QDir(sub).exists());
        QVERIFY(QFileInfo::exists(b));
        QVERIFY(QFileInfo::exists(sub + "/b.wav.md"));
    }

    // ---- AudioOnly scope ---------------------------------------------------

    void trash_audioOnlyScope_keepsTheMdAndReportsItsPreMovePath()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        const QString audio = writeFile(vault.path() + "/orphan.wav", "audio");
        const QString md = writeFile(vault.path() + "/orphan.wav.md", "---\n---\n\n[0] hi\n");

        TrashBatchResult result = trashVaultEntries({QUrl::fromLocalFile(audio)}, VaultFileScope::AudioOnly,
                                                     vault.path());

        QCOMPARE(result.invalid.size(), 0);
        QCOMPARE(result.trashed.size(), 1);
        QVERIFY(!QFileInfo::exists(audio));   // audio gone
        QVERIFY(QFileInfo::exists(md));       // .md deliberately kept
        QCOMPARE(result.trashed.first().second, md); // pre-move .md path reported

        // Restore for cleanliness — don't leave the audio in the real trash.
        const QStringList restored = restoreVaultEntries(result.restorable, vault.path());
        QVERIFY(!restored.isEmpty());
        QVERIFY(QFileInfo::exists(audio));

        // Contrast: mdPath is meaningful ONLY for AudioOnly — a Files-scope
        // trash of another file must report an EMPTY mdPath.
        const QString other = writeFile(vault.path() + "/plain.wav", "x");
        TrashBatchResult filesResult = trashVaultEntries({QUrl::fromLocalFile(other)}, VaultFileScope::Files,
                                                          vault.path());
        QCOMPARE(filesResult.trashed.size(), 1);
        QVERIFY(filesResult.trashed.first().second.isEmpty());
        restoreVaultEntries(filesResult.restorable, vault.path());
        QVERIFY(QFileInfo::exists(other));
    }

    // ---- Untrashable lane ---------------------------------------------------

    // A FAILED moveToTrash() is non-destructive: the entry must land in
    // `untrashable` (never `invalid`) and stay fully on disk. Forced
    // deterministically by stripping write permission from the containing
    // directory so plain `mv`/rename(2) out of it fails with EACCES (verified
    // locally with both `mv` and a raw os.rename()). NOT deterministic here,
    // though: on this machine's tmpfs /tmp, QFile::moveToTrash() falls back to
    // link()+unlink() rather than rename(), link() into the (writable) trash
    // dir succeeds, and the subsequent unlink() of the source silently fails
    // without moveToTrash() propagating that as an overall failure — so it
    // reports success while leaving a duplicate at the original path. That's
    // a platform/filesystem quirk of the test environment, not something this
    // guard can control — an anticipated non-determinism, so QSKIP with the
    // reason, after
    // cleaning up the leftover trash-side duplicate by hand (restoreVaultEntries
    // would refuse: the original is still present, so it can't "restore" atop it).
    void trash_untrashableEntry_isLeftFullyOnDisk_andNotCountedAsInvalid()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        const QString locked = vault.path() + "/locked";
        QVERIFY(QDir().mkpath(locked));
        const QString audio = writeFile(locked + "/stuck.wav", "audio");

        QVERIFY(QFile::setPermissions(locked, QFileDevice::ReadOwner | QFileDevice::ExeOwner
                                             | QFileDevice::ReadGroup | QFileDevice::ExeGroup
                                             | QFileDevice::ReadOther | QFileDevice::ExeOther));
        m_permissionLockedDir = locked; // cleanup() restores this even on failure/skip

        TrashBatchResult result = trashVaultEntries({QUrl::fromLocalFile(audio)}, VaultFileScope::Files,
                                                     vault.path());

        // Restore permissions now, BEFORE any assertion/QSKIP can return out of
        // this slot: `vault` is a stack-local QTemporaryDir that destructs on
        // that return, before the separate cleanup() slot ever runs, so cleanup()
        // alone can't save it — it's just the backstop for an early QVERIFY exit
        // above this line.
        QFile::setPermissions(locked, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner
                                      | QFileDevice::ReadGroup | QFileDevice::ExeGroup
                                      | QFileDevice::ReadOther | QFileDevice::ExeOther);
        m_permissionLockedDir.clear();

        if (!result.trashed.isEmpty()) {
            // moveToTrash() didn't actually fail on this filesystem — purge the
            // duplicate it left in the real trash before skipping.
            for (const QVariant &v : result.restorable) {
                const QString trashPath = v.toMap().value(QStringLiteral("trashPath")).toString();
                const QString infoPath = trashInfoPathForTest(trashPath);
                QFile::remove(trashPath);
                if (!infoPath.isEmpty())
                    QFile::remove(infoPath);
            }
            QSKIP("moveToTrash() did not fail under a read-only parent directory on this "
                  "filesystem (tmpfs /tmp falls back to link()+unlink(), whose unlink() "
                  "failure isn't propagated) — not a deterministic trigger here.");
        }

        QCOMPARE(result.trashed.size(), 0);
        QCOMPARE(result.invalid.size(), 0);
        QCOMPARE(result.untrashable.size(), 1);
        QCOMPARE(result.untrashable.first().toString(), QUrl::fromLocalFile(audio).toString());
        QVERIFY(QFileInfo::exists(audio)); // still on disk, untouched
    }

    // ---- Restore guards (restoreVaultEntries(), VaultFileOps.cpp:404-441) ----

    void restore_refusesAnOriginalPathOutsideTheVault()
    {
        QTemporaryDir vault;
        QTemporaryDir outside;
        QVERIFY(vault.isValid());
        QVERIFY(outside.isValid());
        // A real "trashed" file that restoreVaultEntries() must refuse to move
        // to an originalPath outside the vault, however the entry list got here.
        const QString fakeTrashPath = writeFile(outside.path() + "/would-be-restored.wav", "x");
        const QString outsideOriginal = outside.path() + "/escape-target.wav";

        const QVariantList entries{QVariantMap{{QStringLiteral("trashPath"), fakeTrashPath},
                                                {QStringLiteral("originalPath"), outsideOriginal}}};
        const QStringList restored = restoreVaultEntries(entries, vault.path());

        QVERIFY(restored.isEmpty());
        QVERIFY(!QFileInfo::exists(outsideOriginal));
        QVERIFY(QFileInfo::exists(fakeTrashPath)); // untouched — never moved
    }

    void restore_skipsWhenTheOriginalNameWasTakenBack()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        const QString path = writeFile(vault.path() + "/taken.wav", "original");

        TrashBatchResult result = trashVaultEntries({QUrl::fromLocalFile(path)}, VaultFileScope::Files,
                                                     vault.path());
        QCOMPARE(result.trashed.size(), 1);
        QVERIFY(!result.restorable.isEmpty());

        // A different file squats on the original name before restore runs.
        writeFile(path, "squatter");

        const QStringList restored = restoreVaultEntries(result.restorable, vault.path());
        QVERIFY(restored.isEmpty());
        QFile squatter(path);
        QVERIFY(squatter.open(QIODevice::ReadOnly));
        QCOMPARE(squatter.readAll(), QByteArray("squatter")); // never clobbered

        // The trashed original is now orphaned in the real trash (restore was
        // correctly skipped) — clean it up by hand so nothing lingers.
        for (const QVariant &v : result.restorable) {
            const QString trashPath = v.toMap().value(QStringLiteral("trashPath")).toString();
            const QString infoPath = trashInfoPathForTest(trashPath);
            QFile::remove(trashPath);
            if (!infoPath.isEmpty())
                QFile::remove(infoPath);
        }
    }

    void restore_recreatesAVanishedParentDirectory()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        const QString sub = vault.path() + "/gone-folder";
        QVERIFY(QDir().mkpath(sub));
        const QString path = writeFile(sub + "/only.wav", "x");

        TrashBatchResult result = trashVaultEntries({QUrl::fromLocalFile(path)}, VaultFileScope::Files,
                                                     vault.path());
        QCOMPARE(result.trashed.size(), 1);
        QVERIFY(QDir(sub).rmdir(sub)); // parent is now empty; remove it

        const QStringList restored = restoreVaultEntries(result.restorable, vault.path());
        QVERIFY(!restored.isEmpty());
        QVERIFY(QDir(sub).exists());
        QVERIFY(QFileInfo::exists(path));
    }

    void restore_dropsTheTrashinfoRecord()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        const QString path = writeFile(vault.path() + "/withinfo.wav", "x");

        TrashBatchResult result = trashVaultEntries({QUrl::fromLocalFile(path)}, VaultFileScope::Files,
                                                     vault.path());
        QCOMPARE(result.trashed.size(), 1);
        QVERIFY(!result.restorable.isEmpty());
        const QString trashPath = result.restorable.first().toMap().value(QStringLiteral("trashPath")).toString();
        const QString infoPath = trashInfoPathForTest(trashPath);

        const QStringList restored = restoreVaultEntries(result.restorable, vault.path());
        QVERIFY(!restored.isEmpty());
        QVERIFY(QFileInfo::exists(path));

        // Best-effort in production: only assert this if the platform's trash
        // is spec-shaped enough for a path to have been derivable at all.
        if (!infoPath.isEmpty())
            QVERIFY(!QFileInfo::exists(infoPath));
    }

private:
    QString m_permissionLockedDir;
};

QTEST_GUILESS_MAIN(TestVaultFileOps)
#include "tst_vaultfileops.moc"
