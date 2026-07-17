// Living spec: UsbGrabJob — the background scan+copy for one USB grab rule
// against one already-mounted device (see CLAUDE.md's "USB Grabber" feature
// and DESIGN.md's "USB import" section: diff against the vault by filename +
// size + mtime and copy only new audio in, never reading file content).
//
// Runs entirely on its own worker thread (mirrors AudioRecorder's threading
// shape) — tests drive it through its public start()/finished() surface and
// QSignalSpy::wait(), same as any other cross-thread Qt signal in this suite.

#include <QtTest>

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QHash>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>

#include "UsbGrabJob.h"
#include "UsbRuleStore.h"
#include "VaultTreeModel.h"

namespace {

bool writeFile(const QString &path, const QByteArray &content)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly))
        return false;
    f.write(content);
    return true;
}

// Dedup is by basename + size + mtime (see alreadyPresentAtDestination() in
// UsbGrabJob.cpp), so "already in the vault" can't be expressed by content
// alone the way it could under the old content-hash diff — two files written
// back-to-back get mtimes milliseconds apart and would NOT match. Tests that
// mean "this is the same file" must say so by pinning the mtime explicitly.
bool setMtime(const QString &path, const QDateTime &when)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadWrite))
        return false;
    const bool ok = f.setFileTime(when, QFileDevice::FileModificationTime);
    f.close();
    return ok;
}

// An arbitrary fixed instant, well in the past so it can never collide with a
// file's real creation time during the test run.
QDateTime fixedMtime()
{
    return QDateTime(QDate(2026, 3, 4), QTime(5, 6, 7));
}

}

class TstUsbGrabJob : public QObject
{
    Q_OBJECT

private slots:
    void matchingExtensionsAreCopied_nonMatchingAreIgnored()
    {
        QTemporaryDir device;
        QTemporaryDir vault;
        QVERIFY(device.isValid() && vault.isValid());
        QVERIFY(writeFile(device.filePath("interview.wav"), "audio-data"));
        QVERIFY(writeFile(device.filePath("readme.txt"), "not audio"));

        VaultTreeModel vaultModel;
        vaultModel.setVaultPath(QUrl::fromLocalFile(vault.path()));

        UsbGrabRule rule;
        rule.deviceSerial = QStringLiteral("TEST-SERIAL");
        rule.extensionFilter = {QStringLiteral("wav")};
        rule.preserveStructure = true;

        UsbGrabJob job;
        QSignalSpy finishedSpy(&job, &UsbGrabJob::finished);
        job.start(rule, device.path(), vault.path(), &vaultModel);
        QVERIFY(finishedSpy.wait(10000));

        QCOMPARE(finishedSpy.first().at(0).toInt(), 1); // filesCopied
        QCOMPARE(finishedSpy.first().at(1).toInt(), 0); // filesSkipped
        QVERIFY(QFileInfo::exists(vault.filePath("interview.wav")));
        QVERIFY(!QFileInfo::exists(vault.filePath("readme.txt")));
    }

    void emptyExtensionFilter_meansAllFilesNotZeroFiles()
    {
        QTemporaryDir device;
        QTemporaryDir vault;
        QVERIFY(device.isValid() && vault.isValid());
        QVERIFY(writeFile(device.filePath("a.wav"), "a"));
        QVERIFY(writeFile(device.filePath("b.txt"), "b"));

        VaultTreeModel vaultModel;
        vaultModel.setVaultPath(QUrl::fromLocalFile(vault.path()));

        UsbGrabRule rule;
        rule.deviceSerial = QStringLiteral("TEST-SERIAL");
        // extensionFilter left empty deliberately.

        UsbGrabJob job;
        QSignalSpy finishedSpy(&job, &UsbGrabJob::finished);
        job.start(rule, device.path(), vault.path(), &vaultModel);
        QVERIFY(finishedSpy.wait(10000));

        QCOMPARE(finishedSpy.first().at(0).toInt(), 2);
        QVERIFY(QFileInfo::exists(vault.filePath("a.wav")));
        QVERIFY(QFileInfo::exists(vault.filePath("b.txt")));
    }

    // Same basename + same size + same mtime already in the vault -> skipped.
    // A different basename is always new. (The same-basename-but-different-file
    // case is quickCheck_sameBasenameDifferentContent_... below.)
    void quickCheck_skipsAlreadyPresentFile_copiesNewOne()
    {
        QTemporaryDir device;
        QTemporaryDir vault;
        QVERIFY(device.isValid() && vault.isValid());
        QVERIFY(writeFile(vault.filePath("clip.wav"), "same-content"));
        QVERIFY(writeFile(device.filePath("clip.wav"), "same-content"));       // identical -> skip
        QVERIFY(setMtime(vault.filePath("clip.wav"), fixedMtime()));
        QVERIFY(setMtime(device.filePath("clip.wav"), fixedMtime()));
        QVERIFY(writeFile(device.filePath("nested/clip2.wav"), "different")); // different basename, always new

        VaultTreeModel vaultModel;
        vaultModel.setVaultPath(QUrl::fromLocalFile(vault.path()));

        UsbGrabRule rule;
        rule.deviceSerial = QStringLiteral("TEST-SERIAL");
        rule.preserveStructure = false; // flatten, so clip2.wav lands at vault root too

        UsbGrabJob job;
        QSignalSpy finishedSpy(&job, &UsbGrabJob::finished);
        job.start(rule, device.path(), vault.path(), &vaultModel);
        QVERIFY(finishedSpy.wait(10000));

        QCOMPARE(finishedSpy.first().at(0).toInt(), 1); // filesCopied: clip2.wav only
        QCOMPARE(finishedSpy.first().at(1).toInt(), 1); // filesSkipped: clip.wav (identical)
        QVERIFY(QFileInfo::exists(vault.filePath("clip2.wav")));
    }

    // Same basename and same mtime, but a different size -> a genuinely
    // different file, so it's copied rather than skipped. Pinning both mtimes
    // to the same instant is what makes this prove size is part of the
    // quick-check: with mtime held equal, size is the only thing left to tell
    // the two apart.
    void quickCheck_sameBasenameDifferentSize_copiedUnderNumericSuffix()
    {
        QTemporaryDir device;
        QTemporaryDir vault;
        QVERIFY(device.isValid() && vault.isValid());
        QVERIFY(writeFile(vault.filePath("clip.wav"), "vault-version"));
        QVERIFY(writeFile(device.filePath("clip.wav"), "a-longer-device-version")); // same name, bigger
        QVERIFY(setMtime(vault.filePath("clip.wav"), fixedMtime()));
        QVERIFY(setMtime(device.filePath("clip.wav"), fixedMtime()));

        VaultTreeModel vaultModel;
        vaultModel.setVaultPath(QUrl::fromLocalFile(vault.path()));

        UsbGrabRule rule;
        rule.deviceSerial = QStringLiteral("TEST-SERIAL");

        UsbGrabJob job;
        QSignalSpy finishedSpy(&job, &UsbGrabJob::finished);
        job.start(rule, device.path(), vault.path(), &vaultModel);
        QVERIFY(finishedSpy.wait(10000));

        QCOMPARE(finishedSpy.first().at(0).toInt(), 1); // copied, not skipped
        QCOMPARE(finishedSpy.first().at(1).toInt(), 0);
        // The original vault file must survive untouched (never overwritten).
        QFile original(vault.filePath("clip.wav"));
        QVERIFY(original.open(QIODevice::ReadOnly));
        QCOMPARE(original.readAll(), QByteArray("vault-version"));
        QVERIFY(QFileInfo::exists(vault.filePath("clip_1.wav")));
    }

    void preserveStructureFlag_nestsUnderRelativeSubfolder()
    {
        QTemporaryDir device;
        QTemporaryDir vault;
        QVERIFY(device.isValid() && vault.isValid());
        QVERIFY(writeFile(device.filePath("Voice/2026/session.wav"), "x"));

        VaultTreeModel vaultModel;
        vaultModel.setVaultPath(QUrl::fromLocalFile(vault.path()));

        UsbGrabRule rule;
        rule.deviceSerial = QStringLiteral("TEST-SERIAL");
        rule.preserveStructure = true;

        UsbGrabJob job;
        QSignalSpy finishedSpy(&job, &UsbGrabJob::finished);
        job.start(rule, device.path(), vault.path(), &vaultModel);
        QVERIFY(finishedSpy.wait(10000));

        QVERIFY(QFileInfo::exists(vault.filePath("Voice/2026/session.wav")));
    }

    void preserveStructureFalse_flattensIntoVaultRoot()
    {
        QTemporaryDir device;
        QTemporaryDir vault;
        QVERIFY(device.isValid() && vault.isValid());
        QVERIFY(writeFile(device.filePath("Voice/2026/session.wav"), "x"));

        VaultTreeModel vaultModel;
        vaultModel.setVaultPath(QUrl::fromLocalFile(vault.path()));

        UsbGrabRule rule;
        rule.deviceSerial = QStringLiteral("TEST-SERIAL");
        rule.preserveStructure = false;

        UsbGrabJob job;
        QSignalSpy finishedSpy(&job, &UsbGrabJob::finished);
        job.start(rule, device.path(), vault.path(), &vaultModel);
        QVERIFY(finishedSpy.wait(10000));

        QVERIFY(!QFileInfo::exists(vault.filePath("Voice/2026/session.wav")));
        QVERIFY(QFileInfo::exists(vault.filePath("session.wav")));
    }

    void startFolder_scansOnlyThatSubfolderOfTheDevice()
    {
        QTemporaryDir device;
        QTemporaryDir vault;
        QVERIFY(device.isValid() && vault.isValid());
        QVERIFY(writeFile(device.filePath("Podcasts/ep1.wav"), "x"));
        QVERIFY(writeFile(device.filePath("Other/ignored.wav"), "y"));

        VaultTreeModel vaultModel;
        vaultModel.setVaultPath(QUrl::fromLocalFile(vault.path()));

        UsbGrabRule rule;
        rule.deviceSerial = QStringLiteral("TEST-SERIAL");
        rule.startFolder = QStringLiteral("Podcasts");

        UsbGrabJob job;
        QSignalSpy finishedSpy(&job, &UsbGrabJob::finished);
        job.start(rule, device.path(), vault.path(), &vaultModel);
        QVERIFY(finishedSpy.wait(10000));

        QCOMPARE(finishedSpy.first().at(0).toInt(), 1);
        QVERIFY(QFileInfo::exists(vault.filePath("ep1.wav")));
        QVERIFY(!QFileInfo::exists(vault.filePath("ignored.wav")));
    }

    // startPreview() classifies every file (grab / duplicate / filtered)
    // without copying anything, and never appends a history entry — the
    // dry-run counterpart to matchingExtensionsAreCopied_nonMatchingAreIgnored()
    // and quickCheck_skipsAlreadyPresentFile_...() above, sharing the exact same
    // extension-filter + quick-check dedup logic.
    void dryRun_classifiesWithoutCopyingOrTouchingDisk()
    {
        QTemporaryDir device;
        QTemporaryDir vault;
        QVERIFY(device.isValid() && vault.isValid());
        QVERIFY(writeFile(vault.filePath("existing.wav"), "already-here"));
        QVERIFY(writeFile(device.filePath("existing.wav"), "already-here")); // same size+mtime -> duplicate
        QVERIFY(setMtime(vault.filePath("existing.wav"), fixedMtime()));
        QVERIFY(setMtime(device.filePath("existing.wav"), fixedMtime()));
        QVERIFY(writeFile(device.filePath("new.wav"), "brand-new"));        // not present -> grab
        QVERIFY(writeFile(device.filePath("readme.txt"), "not audio"));     // extension filtered out

        VaultTreeModel vaultModel;
        vaultModel.setVaultPath(QUrl::fromLocalFile(vault.path()));

        UsbGrabRule rule;
        rule.deviceSerial = QStringLiteral("TEST-SERIAL");
        rule.extensionFilter = {QStringLiteral("wav")};

        UsbGrabJob job;
        QSignalSpy previewSpy(&job, &UsbGrabJob::previewReady);
        job.startPreview(rule, device.path(), vault.path());
        QVERIFY(previewSpy.wait(10000));

        const QVariantList entries = previewSpy.first().at(0).toList();
        QCOMPARE(entries.size(), 3);

        QHash<QString, QString> statusByPath;
        for (const QVariant &entryVariant : entries) {
            const QVariantMap entry = entryVariant.toMap();
            statusByPath.insert(entry.value(QStringLiteral("path")).toString(),
                                entry.value(QStringLiteral("status")).toString());
        }
        QCOMPARE(statusByPath.value(QStringLiteral("existing.wav")), QStringLiteral("duplicate"));
        QCOMPARE(statusByPath.value(QStringLiteral("new.wav")), QStringLiteral("grab"));
        QCOMPARE(statusByPath.value(QStringLiteral("readme.txt")), QStringLiteral("filtered"));

        // Nothing was actually copied — "new.wav" being classified "grab"
        // must not have landed in the vault.
        QVERIFY(!QFileInfo::exists(vault.filePath("new.wav")));
        QVERIFY(!QFileInfo::exists(vault.filePath("readme.txt")));
        // The history log must stay untouched by a preview run.
        QVERIFY(vaultModel.readUsbGrabHistory().isEmpty());
    }

    // A preview must NEVER read file content — not for a file that's already
    // present, not for a new one. This is the whole point of the quick-check:
    // content hashing made a preview of a 13.8 GB recorder over USB take ~25
    // minutes (it read the entire device), which read as a hang. Enforced by
    // making every source file unreadable: if the scan still classifies them
    // all correctly, it provably never opened one.
    void preview_neverReadsFileContent()
    {
        QTemporaryDir device;
        QTemporaryDir vault;
        QVERIFY(device.isValid() && vault.isValid());
        QVERIFY(writeFile(device.filePath("unique.wav"), "content-never-read"));
        // Same basename+size+mtime as a vault file => "duplicate". This is the
        // case the old hash diff was forced to read in full, so it's the one
        // that matters most here.
        QVERIFY(writeFile(vault.filePath("known.wav"), "known-content"));
        QVERIFY(writeFile(device.filePath("known.wav"), "known-content"));
        QVERIFY(setMtime(vault.filePath("known.wav"), fixedMtime()));
        QVERIFY(setMtime(device.filePath("known.wav"), fixedMtime()));

        const QStringList sources {device.filePath("unique.wav"), device.filePath("known.wav")};
        for (const QString &path : sources)
            QVERIFY(QFile::setPermissions(path, QFileDevice::WriteOwner));

        VaultTreeModel vaultModel;
        vaultModel.setVaultPath(QUrl::fromLocalFile(vault.path()));

        UsbGrabRule rule;
        rule.deviceSerial = QStringLiteral("TEST-SERIAL");

        UsbGrabJob job;
        QSignalSpy previewSpy(&job, &UsbGrabJob::previewReady);
        job.startPreview(rule, device.path(), vault.path());
        QVERIFY(previewSpy.wait(10000));

        const QVariantList entries = previewSpy.first().at(0).toList();
        QCOMPARE(entries.size(), 2);
        QHash<QString, QString> statusByPath;
        for (const QVariant &entryVariant : entries) {
            const QVariantMap entry = entryVariant.toMap();
            statusByPath.insert(entry.value(QStringLiteral("path")).toString(),
                                entry.value(QStringLiteral("status")).toString());
        }
        QCOMPARE(statusByPath.value(QStringLiteral("unique.wav")), QStringLiteral("grab"));
        QCOMPARE(statusByPath.value(QStringLiteral("known.wav")), QStringLiteral("duplicate"));

        // Restore permissions so QTemporaryDir can clean up.
        for (const QString &path : sources)
            QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    }

    // THE regression test for the quick-check: QFile::copy() stamps the copy
    // with the time of the copy, not the source's mtime. Without UsbGrabJob
    // explicitly restoring it, a grabbed file would never match its source
    // again and every single re-grab would re-copy the whole device under
    // _1/_2/... suffixes — silently duplicating gigabytes.
    void copiedFile_keepsSourceMtime_soARegrabSkipsEverything()
    {
        QTemporaryDir device;
        QTemporaryDir vault;
        QVERIFY(device.isValid() && vault.isValid());
        QVERIFY(writeFile(device.filePath("take1.wav"), "audio"));
        QVERIFY(writeFile(device.filePath("nested/take2.wav"), "more-audio"));
        QVERIFY(setMtime(device.filePath("take1.wav"), fixedMtime()));
        QVERIFY(setMtime(device.filePath("nested/take2.wav"), fixedMtime()));

        VaultTreeModel vaultModel;
        vaultModel.setVaultPath(QUrl::fromLocalFile(vault.path()));

        UsbGrabRule rule;
        rule.deviceSerial = QStringLiteral("TEST-SERIAL");

        UsbGrabJob firstRun;
        QSignalSpy firstSpy(&firstRun, &UsbGrabJob::finished);
        firstRun.start(rule, device.path(), vault.path(), &vaultModel);
        QVERIFY(firstSpy.wait(10000));
        QCOMPARE(firstSpy.first().at(0).toInt(), 2); // both copied

        // The copies carry the source's mtime, not "now".
        QCOMPARE(QFileInfo(vault.filePath("take1.wav")).lastModified(), fixedMtime());
        QCOMPARE(QFileInfo(vault.filePath("nested/take2.wav")).lastModified(), fixedMtime());

        // Re-grabbing the same untouched device must copy nothing at all.
        UsbGrabJob secondRun;
        QSignalSpy secondSpy(&secondRun, &UsbGrabJob::finished);
        secondRun.start(rule, device.path(), vault.path(), &vaultModel);
        QVERIFY(secondSpy.wait(10000));
        QCOMPARE(secondSpy.first().at(0).toInt(), 0); // filesCopied
        QCOMPARE(secondSpy.first().at(1).toInt(), 2); // filesSkipped
        QVERIFY(!QFileInfo::exists(vault.filePath("take1_1.wav")));
        QVERIFY(!QFileInfo::exists(vault.filePath("nested/take2_1.wav")));
    }

    // A grab immediately followed by a preview must agree: the files just
    // copied all report "duplicate", never "grab". Pins the two modes to the
    // same dedup rule end-to-end (they share one code path, and this is what
    // stops them silently drifting apart).
    void previewAfterGrab_reportsEverythingAsDuplicate()
    {
        QTemporaryDir device;
        QTemporaryDir vault;
        QVERIFY(device.isValid() && vault.isValid());
        QVERIFY(writeFile(device.filePath("take1.wav"), "audio"));
        QVERIFY(setMtime(device.filePath("take1.wav"), fixedMtime()));

        VaultTreeModel vaultModel;
        vaultModel.setVaultPath(QUrl::fromLocalFile(vault.path()));

        UsbGrabRule rule;
        rule.deviceSerial = QStringLiteral("TEST-SERIAL");

        UsbGrabJob grab;
        QSignalSpy finishedSpy(&grab, &UsbGrabJob::finished);
        grab.start(rule, device.path(), vault.path(), &vaultModel);
        QVERIFY(finishedSpy.wait(10000));
        QCOMPARE(finishedSpy.first().at(0).toInt(), 1);

        UsbGrabJob preview;
        QSignalSpy previewSpy(&preview, &UsbGrabJob::previewReady);
        preview.startPreview(rule, device.path(), vault.path());
        QVERIFY(previewSpy.wait(10000));

        const QVariantList entries = previewSpy.first().at(0).toList();
        QCOMPARE(entries.size(), 1);
        QCOMPARE(entries.first().toMap().value(QStringLiteral("status")).toString(),
                 QStringLiteral("duplicate"));
    }

    // The accepted tradeoff of the quick-check, pinned deliberately so it's a
    // decision on record rather than a surprise: a file whose bytes changed
    // while its name, size AND mtime all stayed identical is wrongly seen as
    // already-grabbed. Field recorders don't produce that (a re-recording
    // changes size and stamps a new mtime), and the cost is a missed import
    // the user can see in the preview — never an overwrite or data loss.
    // If this test ever starts failing, the dedup got STRONGER; see
    // DESIGN.md's "Why not a content hash" note before changing it back.
    void quickCheck_identicalNameSizeAndMtime_differentBytes_isTreatedAsDuplicate()
    {
        QTemporaryDir device;
        QTemporaryDir vault;
        QVERIFY(device.isValid() && vault.isValid());
        QVERIFY(writeFile(vault.filePath("clip.wav"), "AAAAAAAAAA"));
        QVERIFY(writeFile(device.filePath("clip.wav"), "BBBBBBBBBB")); // same size, different bytes
        QVERIFY(setMtime(vault.filePath("clip.wav"), fixedMtime()));
        QVERIFY(setMtime(device.filePath("clip.wav"), fixedMtime()));

        VaultTreeModel vaultModel;
        vaultModel.setVaultPath(QUrl::fromLocalFile(vault.path()));

        UsbGrabRule rule;
        rule.deviceSerial = QStringLiteral("TEST-SERIAL");

        UsbGrabJob job;
        QSignalSpy finishedSpy(&job, &UsbGrabJob::finished);
        job.start(rule, device.path(), vault.path(), &vaultModel);
        QVERIFY(finishedSpy.wait(10000));

        QCOMPARE(finishedSpy.first().at(0).toInt(), 0); // copied nothing
        QCOMPARE(finishedSpy.first().at(1).toInt(), 1); // skipped as "already present"
        // The vault's original is untouched either way — nothing is overwritten.
        QFile original(vault.filePath("clip.wav"));
        QVERIFY(original.open(QIODevice::ReadOnly));
        QCOMPARE(original.readAll(), QByteArray("AAAAAAAAAA"));
    }

    // A destinationVaultPath that doesn't exist yet (i.e. UsbGrabberController
    // resolved a rule's destinationFolder to a not-yet-created subfolder) gets
    // created lazily by the existing per-file mkpath() call — no separate
    // folder-creation step is needed.
    void destinationSubfolder_createdLazilyOnFirstCopy()
    {
        QTemporaryDir device;
        QTemporaryDir vault;
        QVERIFY(device.isValid() && vault.isValid());
        QVERIFY(writeFile(device.filePath("clip.wav"), "x"));

        VaultTreeModel vaultModel;
        vaultModel.setVaultPath(QUrl::fromLocalFile(vault.path()));

        UsbGrabRule rule;
        rule.deviceSerial = QStringLiteral("TEST-SERIAL");

        const QString destination = vault.filePath("Interviews/USB");
        QVERIFY(!QFileInfo::exists(destination));

        UsbGrabJob job;
        QSignalSpy finishedSpy(&job, &UsbGrabJob::finished);
        job.start(rule, device.path(), destination, &vaultModel);
        QVERIFY(finishedSpy.wait(10000));

        QCOMPARE(finishedSpy.first().at(0).toInt(), 1);
        QVERIFY(QFileInfo::exists(destination + "/clip.wav"));
    }

    // On completion, one history entry lands in the destination vault's
    // .transcriber/usbGrabHistory.json (see VaultTreeModel::
    // appendUsbGrabHistoryEntry(), tested independently in
    // tst_vaulttreemodel.cpp) — verified here end-to-end from a real job run.
    void completion_recordsHistoryEntryInDestinationVault()
    {
        QTemporaryDir device;
        QTemporaryDir vault;
        QVERIFY(device.isValid() && vault.isValid());
        QVERIFY(writeFile(device.filePath("a.wav"), "x"));

        VaultTreeModel vaultModel;
        vaultModel.setVaultPath(QUrl::fromLocalFile(vault.path()));

        UsbGrabRule rule;
        rule.deviceSerial = QStringLiteral("HIST-SERIAL");
        rule.deviceLabel = QStringLiteral("Test Recorder");

        UsbGrabJob job;
        QSignalSpy finishedSpy(&job, &UsbGrabJob::finished);
        job.start(rule, device.path(), vault.path(), &vaultModel);
        QVERIFY(finishedSpy.wait(10000));

        const QVariantList history = vaultModel.readUsbGrabHistory();
        QCOMPARE(history.size(), 1);
        const QVariantMap entry = history.first().toMap();
        QCOMPARE(entry.value(QStringLiteral("deviceSerial")).toString(), QStringLiteral("HIST-SERIAL"));
        QCOMPARE(entry.value(QStringLiteral("deviceLabel")).toString(), QStringLiteral("Test Recorder"));
        QCOMPARE(entry.value(QStringLiteral("filesCopied")).toInt(), 1);
        QVERIFY(!entry.value(QStringLiteral("timestamp")).toString().isEmpty());
    }

    // Defence in depth behind UsbDeviceMonitor's external-media filter (tested in
    // tst_usbdevicemonitor.cpp): even if a scan root somehow resolves to "/", the
    // job itself refuses rather than recursively copying the entire filesystem
    // into the vault. Pure path logic, so it's checked directly here instead of
    // by actually pointing a job at "/".
    void refusalReason_rejectsFilesystemRoot()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(!usbGrabRefusalReason(QStringLiteral("/"), vault.path()).isEmpty());
    }

    // "/" reached via a symlink or ".." must be refused too — the check
    // canonicalises before comparing, so it can't be dodged lexically.
    void refusalReason_rejectsFilesystemRootReachedIndirectly()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QVERIFY(!usbGrabRefusalReason(QStringLiteral("/usr/.."), vault.path()).isEmpty());

        QTemporaryDir linkDir;
        QVERIFY(linkDir.isValid());
        const QString linkToRoot = linkDir.filePath("root-link");
        QVERIFY(QFile::link(QStringLiteral("/"), linkToRoot));
        QVERIFY(!usbGrabRefusalReason(linkToRoot, vault.path()).isEmpty());
    }

    // A destination vault sitting inside the scan root makes the walk feed itself
    // the files it is copying.
    void refusalReason_rejectsDestinationInsideScanRoot()
    {
        QTemporaryDir device;
        QVERIFY(device.isValid());
        QVERIFY(!usbGrabRefusalReason(device.path(), device.filePath("Vault")).isEmpty());
        // Same path on both sides is the degenerate case of the same problem.
        QVERIFY(!usbGrabRefusalReason(device.path(), device.path()).isEmpty());
    }

    // The containment check is a path-COMPONENT test: a sibling whose name merely
    // shares a string prefix with the scan root is not inside it, and refusing it
    // would block a legitimate grab.
    void refusalReason_allowsSiblingWithSharedNamePrefix()
    {
        QTemporaryDir parent;
        QVERIFY(parent.isValid());
        const QString scanRoot = parent.filePath("rec");
        const QString destination = parent.filePath("recordings");
        QVERIFY(QDir().mkpath(scanRoot));
        QVERIFY(QDir().mkpath(destination));
        QCOMPARE(usbGrabRefusalReason(scanRoot, destination), QString());
    }

    // A rule pointing at a startFolder that isn't on the device is a
    // misconfiguration, not an empty device — say so rather than silently
    // reporting a successful zero-file grab.
    void refusalReason_rejectsNonExistentScanRoot()
    {
        QTemporaryDir device;
        QTemporaryDir vault;
        QVERIFY(device.isValid() && vault.isValid());
        QVERIFY(!usbGrabRefusalReason(device.filePath("no-such-folder"), vault.path()).isEmpty());
    }

    // A not-yet-created destination subfolder is normal (mkpath happens at copy
    // time — see destinationSubfolder_createdLazilyOnFirstCopy above) and must not
    // itself trip the refusal.
    void refusalReason_allowsNotYetCreatedDestinationSubfolder()
    {
        QTemporaryDir device;
        QTemporaryDir vault;
        QVERIFY(device.isValid() && vault.isValid());
        QCOMPARE(usbGrabRefusalReason(device.path(), vault.filePath("Interviews/USB")), QString());
    }

    // ...but a not-yet-created destination that would land INSIDE the scan root is
    // still refused — "doesn't exist yet" must not become a hole in the check.
    void refusalReason_rejectsNotYetCreatedDestinationInsideScanRoot()
    {
        QTemporaryDir device;
        QVERIFY(device.isValid());
        QVERIFY(!usbGrabRefusalReason(device.path(), device.filePath("Nested/Vault")).isEmpty());
    }

    // End-to-end: a refused run reports the reason through finished()'s errors and
    // copies nothing. Uses a destination inside the scan root (a refusal that's
    // safe to actually execute, unlike pointing a real job at "/").
    void refusedRun_reportsErrorAndCopiesNothing()
    {
        QTemporaryDir device;
        QVERIFY(device.isValid());
        QVERIFY(writeFile(device.filePath("clip.wav"), "x"));
        const QString destination = device.filePath("Vault");

        VaultTreeModel vaultModel;
        vaultModel.setVaultPath(QUrl::fromLocalFile(device.path()));

        UsbGrabRule rule;
        rule.deviceSerial = QStringLiteral("REFUSE-SERIAL");

        UsbGrabJob job;
        QSignalSpy finishedSpy(&job, &UsbGrabJob::finished);
        job.start(rule, device.path(), destination, &vaultModel);
        QVERIFY(finishedSpy.wait(10000));

        QCOMPARE(finishedSpy.first().at(0).toInt(), 0); // copied
        QCOMPARE(finishedSpy.first().at(1).toInt(), 0); // skipped
        QCOMPARE(finishedSpy.first().at(2).toStringList().size(), 1);
        QVERIFY(!QFileInfo::exists(destination));
    }

    // A refused PREVIEW surfaces the reason through previewReady()'s error rather
    // than returning an empty list, which would be indistinguishable from a device
    // with nothing on it.
    void refusedPreview_reportsErrorRatherThanEmptyList()
    {
        QTemporaryDir device;
        QVERIFY(device.isValid());
        QVERIFY(writeFile(device.filePath("clip.wav"), "x"));

        UsbGrabRule rule;
        rule.deviceSerial = QStringLiteral("REFUSE-SERIAL");

        UsbGrabJob job;
        QSignalSpy previewSpy(&job, &UsbGrabJob::previewReady);
        job.startPreview(rule, device.path(), device.filePath("Vault"));
        QVERIFY(previewSpy.wait(10000));

        QVERIFY(previewSpy.first().at(0).toList().isEmpty());
        QVERIFY(!previewSpy.first().at(1).toString().isEmpty());
    }

    // --- filenameRegex (UsbGrabRule::filenameRegex) -----------------------
    // Same engine and same semantics as the split-recording combine rules:
    // QRegularExpression, matched against the FILE NAME only, unanchored.

    void filenameRegex_copiesOnlyMatchingFiles()
    {
        QTemporaryDir device;
        QTemporaryDir vault;
        QVERIFY(device.isValid() && vault.isValid());
        QVERIFY(writeFile(device.filePath("REC_0042.wav"), "keep"));
        QVERIFY(writeFile(device.filePath("MEMO_0042.wav"), "drop"));

        VaultTreeModel vaultModel;
        vaultModel.setVaultPath(QUrl::fromLocalFile(vault.path()));

        UsbGrabRule rule;
        rule.deviceSerial = QStringLiteral("TEST-SERIAL");
        rule.filenameRegex = QStringLiteral("^REC_\\d+");

        UsbGrabJob job;
        QSignalSpy finishedSpy(&job, &UsbGrabJob::finished);
        job.start(rule, device.path(), vault.path(), &vaultModel);
        QVERIFY(finishedSpy.wait(10000));

        QCOMPARE(finishedSpy.first().at(0).toInt(), 1); // filesCopied
        QVERIFY(QFileInfo::exists(vault.filePath("REC_0042.wav")));
        QVERIFY(!QFileInfo::exists(vault.filePath("MEMO_0042.wav")));
    }

    // Empty means "no regex constraint", NOT "match nothing" — the same
    // reading extensionFilter gets (see emptyExtensionFilter_... above).
    void emptyFilenameRegex_meansAllFilesNotZeroFiles()
    {
        QTemporaryDir device;
        QTemporaryDir vault;
        QVERIFY(device.isValid() && vault.isValid());
        QVERIFY(writeFile(device.filePath("a.wav"), "a"));
        QVERIFY(writeFile(device.filePath("b.txt"), "b"));

        VaultTreeModel vaultModel;
        vaultModel.setVaultPath(QUrl::fromLocalFile(vault.path()));

        UsbGrabRule rule;
        rule.deviceSerial = QStringLiteral("TEST-SERIAL");
        // filenameRegex left empty deliberately.

        UsbGrabJob job;
        QSignalSpy finishedSpy(&job, &UsbGrabJob::finished);
        job.start(rule, device.path(), vault.path(), &vaultModel);
        QVERIFY(finishedSpy.wait(10000));

        QCOMPARE(finishedSpy.first().at(0).toInt(), 2);
    }

    // The two filters AND: a file must clear BOTH. Each stays independently
    // "no constraint" when blank, so neither can silently widen the other.
    void filenameRegexAndExtensionFilter_bothMustPass()
    {
        QTemporaryDir device;
        QTemporaryDir vault;
        QVERIFY(device.isValid() && vault.isValid());
        QVERIFY(writeFile(device.filePath("REC_1.wav"), "both"));   // ext ok, regex ok  -> grab
        QVERIFY(writeFile(device.filePath("MEMO_1.wav"), "ext"));   // ext ok, regex no  -> filtered
        QVERIFY(writeFile(device.filePath("REC_2.txt"), "regex"));  // ext no, regex ok  -> filtered

        VaultTreeModel vaultModel;
        vaultModel.setVaultPath(QUrl::fromLocalFile(vault.path()));

        UsbGrabRule rule;
        rule.deviceSerial = QStringLiteral("TEST-SERIAL");
        rule.extensionFilter = {QStringLiteral("wav")};
        rule.filenameRegex = QStringLiteral("^REC_");

        UsbGrabJob job;
        QSignalSpy finishedSpy(&job, &UsbGrabJob::finished);
        job.start(rule, device.path(), vault.path(), &vaultModel);
        QVERIFY(finishedSpy.wait(10000));

        QCOMPARE(finishedSpy.first().at(0).toInt(), 1);
        QVERIFY(QFileInfo::exists(vault.filePath("REC_1.wav")));
        QVERIFY(!QFileInfo::exists(vault.filePath("MEMO_1.wav")));
        QVERIFY(!QFileInfo::exists(vault.filePath("REC_2.txt")));
    }

    // Matched against the file name, never the path — so a pattern naming a
    // FOLDER matches nothing, and a file deep in a matching folder is judged
    // only on its own name. Mirrors matchRuleCapturedKey()'s use of
    // QFileInfo::fileName() for combine rules.
    void filenameRegex_matchesFileNameNotPath()
    {
        QTemporaryDir device;
        QTemporaryDir vault;
        QVERIFY(device.isValid() && vault.isValid());
        QVERIFY(writeFile(device.filePath("DCIM/clip.wav"), "in-dcim"));

        VaultTreeModel vaultModel;
        vaultModel.setVaultPath(QUrl::fromLocalFile(vault.path()));

        UsbGrabRule rule;
        rule.deviceSerial = QStringLiteral("TEST-SERIAL");
        rule.preserveStructure = false;
        rule.filenameRegex = QStringLiteral("DCIM"); // a FOLDER name, not a file name

        UsbGrabJob job;
        QSignalSpy finishedSpy(&job, &UsbGrabJob::finished);
        job.start(rule, device.path(), vault.path(), &vaultModel);
        QVERIFY(finishedSpy.wait(10000));

        QCOMPARE(finishedSpy.first().at(0).toInt(), 0);
        QVERIFY(!QFileInfo::exists(vault.filePath("clip.wav")));
    }

    // Unanchored unless the pattern says otherwise, exactly like the combine
    // rules — "REC" finds REC anywhere in the name. This is why the field's
    // hint text calls out ^ and $.
    void filenameRegex_isUnanchoredUnlessAnchored()
    {
        QTemporaryDir device;
        QTemporaryDir vault;
        QVERIFY(device.isValid() && vault.isValid());
        QVERIFY(writeFile(device.filePath("my_REC_tail.wav"), "substring"));

        VaultTreeModel vaultModel;
        vaultModel.setVaultPath(QUrl::fromLocalFile(vault.path()));

        UsbGrabRule unanchored;
        unanchored.deviceSerial = QStringLiteral("TEST-SERIAL");
        unanchored.filenameRegex = QStringLiteral("REC");

        UsbGrabJob job;
        QSignalSpy finishedSpy(&job, &UsbGrabJob::finished);
        job.start(unanchored, device.path(), vault.path(), &vaultModel);
        QVERIFY(finishedSpy.wait(10000));
        QCOMPARE(finishedSpy.first().at(0).toInt(), 1);

        // ...and anchoring it excludes the very same file.
        QTemporaryDir vault2;
        QVERIFY(vault2.isValid());
        VaultTreeModel vaultModel2;
        vaultModel2.setVaultPath(QUrl::fromLocalFile(vault2.path()));

        UsbGrabRule anchored = unanchored;
        anchored.filenameRegex = QStringLiteral("^REC");

        UsbGrabJob job2;
        QSignalSpy finishedSpy2(&job2, &UsbGrabJob::finished);
        job2.start(anchored, device.path(), vault2.path(), &vaultModel2);
        QVERIFY(finishedSpy2.wait(10000));
        QCOMPARE(finishedSpy2.first().at(0).toInt(), 0);
    }

    // Fails CLOSED. An unparseable pattern must never be read as "no
    // constraint": on the "*" catch-all rule that would bulk-copy a whole
    // device on a typo. Copying nothing is recoverable and is what the
    // pattern field's own ✗ indicator already warns about.
    void invalidFilenameRegex_copiesNothing()
    {
        QTemporaryDir device;
        QTemporaryDir vault;
        QVERIFY(device.isValid() && vault.isValid());
        QVERIFY(writeFile(device.filePath("clip.wav"), "audio"));

        VaultTreeModel vaultModel;
        vaultModel.setVaultPath(QUrl::fromLocalFile(vault.path()));

        UsbGrabRule rule;
        rule.deviceSerial = QStringLiteral("TEST-SERIAL");
        rule.filenameRegex = QStringLiteral("(unterminated[");

        UsbGrabJob job;
        QSignalSpy finishedSpy(&job, &UsbGrabJob::finished);
        job.start(rule, device.path(), vault.path(), &vaultModel);
        QVERIFY(finishedSpy.wait(10000));

        QCOMPARE(finishedSpy.first().at(0).toInt(), 0);
        QVERIFY(!QFileInfo::exists(vault.filePath("clip.wav")));
    }

    // The preview must classify a regex-rejected file as "filtered" just like
    // an extension-rejected one — one status covers both filters, and the
    // preview still lists EVERY file the scan saw.
    void preview_regexRejectedFileIsFiltered()
    {
        QTemporaryDir device;
        QTemporaryDir vault;
        QVERIFY(device.isValid() && vault.isValid());
        QVERIFY(writeFile(device.filePath("REC_1.wav"), "keep"));
        QVERIFY(writeFile(device.filePath("MEMO_1.wav"), "drop"));

        UsbGrabRule rule;
        rule.deviceSerial = QStringLiteral("TEST-SERIAL");
        rule.filenameRegex = QStringLiteral("^REC_");

        UsbGrabJob job;
        QSignalSpy previewSpy(&job, &UsbGrabJob::previewReady);
        job.startPreview(rule, device.path(), vault.path());
        QVERIFY(previewSpy.wait(10000));

        const QVariantList entries = previewSpy.first().at(0).toList();
        QCOMPARE(entries.size(), 2);
        QHash<QString, QString> statusByPath;
        for (const QVariant &entryVariant : entries) {
            const QVariantMap entry = entryVariant.toMap();
            statusByPath.insert(entry.value(QStringLiteral("path")).toString(),
                                entry.value(QStringLiteral("status")).toString());
        }
        QCOMPARE(statusByPath.value(QStringLiteral("REC_1.wav")), QStringLiteral("grab"));
        QCOMPARE(statusByPath.value(QStringLiteral("MEMO_1.wav")), QStringLiteral("filtered"));

        // Nothing was copied — a preview never writes.
        QVERIFY(!QFileInfo::exists(vault.filePath("REC_1.wav")));
    }
};

QTEST_GUILESS_MAIN(TstUsbGrabJob)
#include "tst_usbgrabjob.moc"
