// Living spec: UsbGrabJob — the background scan+copy for one USB grab rule
// against one already-mounted device (see CLAUDE.md's "USB Grabber" feature,
// DESIGN.md's "USB import" section: "diff by filename + hash against the
// vault and copy only new audio in").
//
// Runs entirely on its own worker thread (mirrors AudioRecorder's threading
// shape) — tests drive it through its public start()/finished() surface and
// QSignalSpy::wait(), same as any other cross-thread Qt signal in this suite.

#include <QtTest>

#include <QDir>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QUrl>

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

    // Same basename + same content already in the vault -> skipped. Same
    // basename + DIFFERENT content -> still copied (never silently
    // overwritten), landing under a suffixed name.
    void hashDiff_skipsIdenticalContent_copiesDifferingContentUnderSuffix()
    {
        QTemporaryDir device;
        QTemporaryDir vault;
        QVERIFY(device.isValid() && vault.isValid());
        QVERIFY(writeFile(vault.filePath("clip.wav"), "same-content"));
        QVERIFY(writeFile(device.filePath("clip.wav"), "same-content"));       // identical -> skip
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

    void hashDiff_sameBasenameDifferentContent_copiedUnderNumericSuffix()
    {
        QTemporaryDir device;
        QTemporaryDir vault;
        QVERIFY(device.isValid() && vault.isValid());
        QVERIFY(writeFile(vault.filePath("clip.wav"), "vault-version"));
        QVERIFY(writeFile(device.filePath("clip.wav"), "device-version")); // same name, different bytes

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
};

QTEST_GUILESS_MAIN(TstUsbGrabJob)
#include "tst_usbgrabjob.moc"
