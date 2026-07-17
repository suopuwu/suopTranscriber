// Living spec: UsbGrabberController — the QML-facing coordinator for the USB
// Grabber feature (see CLAUDE.md). Covers the parts that don't require a live
// UDisks2/D-Bus session: rule CRUD (QVariantMap <-> UsbGrabRule projection),
// the no-vault-open and device-not-connected guard paths, and history
// proxying. UsbDeviceMonitor's own D-Bus hotplug detection isn't
// unit-testable without a fake D-Bus session/UDisks2 service — not covered
// here (see .task/plan.md's Decisions for that scope call).

#include <QtTest>

#include <QCoreApplication>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QUrl>

#include "UsbGrabberController.h"
#include "VaultTreeModel.h"

class TstUsbGrabberController : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase()
    {
        // UsbGrabberController owns a UsbRuleStore internally, which persists
        // through a plain QSettings() — same isolation as tst_usbrulestore.cpp,
        // per CLAUDE.md's "never touch the user's real vault or app config".
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setOrganizationName(QStringLiteral("suopTranscriber-tests"));
        QCoreApplication::setApplicationName(QStringLiteral("tst_usbgrabbercontroller"));
    }

    void init()
    {
        UsbGrabberController controller;
        for (const QVariant &ruleVariant : controller.allRules())
            controller.removeRule(ruleVariant.toMap().value(QStringLiteral("deviceSerial")).toString());
    }

    void noVaultOpen_triggerManualGrab_setsLastError()
    {
        UsbGrabberController controller;
        QVERIFY(controller.lastError().isEmpty());

        controller.triggerManualGrab(QStringLiteral("SOME-SERIAL"));

        QVERIFY(!controller.lastError().isEmpty());
    }

    void vaultOpenButDeviceNotConnected_triggerManualGrab_setsLastError()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        VaultTreeModel vaultModel;
        vaultModel.setVaultPath(QUrl::fromLocalFile(vault.path()));

        UsbGrabberController controller;
        controller.setVaultTreeModel(&vaultModel);

        // No real D-Bus/UDisks2 device will ever match this fabricated
        // serial, so this exercises the "not currently mounted" guard rather
        // than actually depending on live hardware.
        controller.triggerManualGrab(QStringLiteral("DEFINITELY-NOT-A-REAL-DEVICE-SERIAL"));

        QVERIFY(!controller.lastError().isEmpty());
    }

    void requestGrabPreview_noVaultOpen_emitsErrorAndEmptyList()
    {
        UsbGrabberController controller;
        QSignalSpy previewSpy(&controller, &UsbGrabberController::grabPreviewReady);

        controller.requestGrabPreview(QStringLiteral("SOME-SERIAL"));

        QVERIFY(!controller.lastError().isEmpty());
        QCOMPARE(previewSpy.count(), 1);
        QCOMPARE(previewSpy.first().at(0).toString(), QStringLiteral("SOME-SERIAL"));
        QVERIFY(previewSpy.first().at(1).toList().isEmpty());
        QVERIFY(!previewSpy.first().at(2).toString().isEmpty());
    }

    void requestGrabPreview_deviceNotConnected_emitsErrorAndEmptyList()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        VaultTreeModel vaultModel;
        vaultModel.setVaultPath(QUrl::fromLocalFile(vault.path()));

        UsbGrabberController controller;
        controller.setVaultTreeModel(&vaultModel);
        QSignalSpy previewSpy(&controller, &UsbGrabberController::grabPreviewReady);

        // Same fabricated-serial approach as vaultOpenButDeviceNotConnected_
        // triggerManualGrab_setsLastError() above — no real D-Bus/UDisks2
        // device will ever match it.
        controller.requestGrabPreview(QStringLiteral("DEFINITELY-NOT-A-REAL-DEVICE-SERIAL"));

        QVERIFY(!controller.lastError().isEmpty());
        QCOMPARE(previewSpy.count(), 1);
        QVERIFY(previewSpy.first().at(1).toList().isEmpty());
        QVERIFY(!previewSpy.first().at(2).toString().isEmpty());
    }

    void ruleForDevice_neverConfigured_returnsPopulatedDefaultMap()
    {
        UsbGrabberController controller;
        const QVariantMap rule = controller.ruleForDevice(QStringLiteral("NEVER-SEEN"));

        QCOMPARE(rule.value(QStringLiteral("deviceSerial")).toString(), QStringLiteral("NEVER-SEEN"));
        QCOMPARE(rule.value(QStringLiteral("enabled")).toBool(), false);
        QCOMPARE(rule.value(QStringLiteral("preserveStructure")).toBool(), true);
        QVERIFY(rule.value(QStringLiteral("extensionFilter")).toStringList().isEmpty());
        // Blank = "no regex constraint". The key must still be PRESENT: the
        // rule cards bind to it, and a missing key would read as undefined.
        QVERIFY(rule.contains(QStringLiteral("filenameRegex")));
        QVERIFY(rule.value(QStringLiteral("filenameRegex")).toString().isEmpty());
    }

    // Backs the live ✓/✗ indicator under the pattern field. Must agree with
    // the engine UsbGrabJob actually filters with (QRegularExpression), which
    // is why QML calls this instead of using JS RegExp — and it must treat an
    // empty pattern as valid, since blank means "no regex constraint".
    // Mirrors tst_filecombine.cpp's regexValidationError_* case; both now
    // share one definition (RegexValidation.h).
    void regexValidationError_emptyValidAndInvalidPatterns()
    {
        UsbGrabberController controller;
        QCOMPARE(controller.regexValidationError(QString()), QString());
        QCOMPARE(controller.regexValidationError(QStringLiteral("^REC_(\\d{4})\\.wav$")), QString());
        QVERIFY(!controller.regexValidationError(QStringLiteral("(unterminated[")).isEmpty());
    }

    void saveRule_thenRuleForDevice_roundTripsEveryField()
    {
        UsbGrabberController controller;
        QVariantMap rule;
        rule[QStringLiteral("deviceSerial")] = QStringLiteral("XYZ");
        rule[QStringLiteral("enabled")] = true;
        rule[QStringLiteral("extensionFilter")] = QStringList{QStringLiteral("wav"), QStringLiteral("mp3")};
        rule[QStringLiteral("filenameRegex")] = QStringLiteral("^REC_\\d+");
        rule[QStringLiteral("preserveStructure")] = false;
        rule[QStringLiteral("startFolder")] = QStringLiteral("Recordings");
        rule[QStringLiteral("deviceLabel")] = QStringLiteral("Zoom H1n");
        rule[QStringLiteral("destinationFolder")] = QStringLiteral("Interviews/USB");

        controller.saveRule(rule);

        const QVariantMap loaded = controller.ruleForDevice(QStringLiteral("XYZ"));
        QCOMPARE(loaded.value(QStringLiteral("enabled")).toBool(), true);
        QCOMPARE(loaded.value(QStringLiteral("extensionFilter")).toStringList(),
                 QStringList({QStringLiteral("wav"), QStringLiteral("mp3")}));
        // MainPanel.qml's rule cards read this key straight off the map, so a
        // missing projection here is an invisible "the field won't stick" bug.
        QCOMPARE(loaded.value(QStringLiteral("filenameRegex")).toString(), QStringLiteral("^REC_\\d+"));
        QCOMPARE(loaded.value(QStringLiteral("preserveStructure")).toBool(), false);
        QCOMPARE(loaded.value(QStringLiteral("startFolder")).toString(), QStringLiteral("Recordings"));
        QCOMPARE(loaded.value(QStringLiteral("deviceLabel")).toString(), QStringLiteral("Zoom H1n"));
        QCOMPARE(loaded.value(QStringLiteral("destinationFolder")).toString(), QStringLiteral("Interviews/USB"));
    }

    void removeRule_removesIt()
    {
        UsbGrabberController controller;
        QVariantMap rule;
        rule[QStringLiteral("deviceSerial")] = QStringLiteral("TO-REMOVE");
        rule[QStringLiteral("enabled")] = true;
        controller.saveRule(rule);
        QVERIFY(controller.ruleForDevice(QStringLiteral("TO-REMOVE")).value(QStringLiteral("enabled")).toBool());

        controller.removeRule(QStringLiteral("TO-REMOVE"));

        QCOMPARE(controller.ruleForDevice(QStringLiteral("TO-REMOVE")).value(QStringLiteral("enabled")).toBool(), false);
    }

    void allRules_listsEveryPersistedRuleIncludingCatchAll()
    {
        UsbGrabberController controller;
        QVariantMap deviceRule;
        deviceRule[QStringLiteral("deviceSerial")] = QStringLiteral("DEV-1");
        deviceRule[QStringLiteral("enabled")] = true;
        controller.saveRule(deviceRule);

        QVariantMap catchAll;
        catchAll[QStringLiteral("deviceSerial")] = QStringLiteral("*");
        catchAll[QStringLiteral("enabled")] = true;
        controller.saveRule(catchAll);

        const QVariantList all = controller.allRules();
        QCOMPARE(all.size(), 2);
        QStringList serials;
        for (const QVariant &r : all)
            serials << r.toMap().value(QStringLiteral("deviceSerial")).toString();
        QVERIFY(serials.contains(QStringLiteral("DEV-1")));
        QVERIFY(serials.contains(QStringLiteral("*")));
    }

    // A multi-LUN device (the real Acme Mic Tx exposes /dev/sda + /dev/sdb under
    // one Drive, hence one serial) must yield EVERY one of its mounted volumes.
    // Returning only the first is the bug this replaced: "Grab now" grabbed one
    // volume and the other's files were silently never imported.
    void usbVolumesForSerial_returnsEveryVolumeSharingTheSerial()
    {
        QVariantList mounted;
        mounted.append(QVariantMap {{QStringLiteral("id"), QStringLiteral("/block_devices/sda")},
                                     {QStringLiteral("serial"), QStringLiteral("20200101.1")},
                                     {QStringLiteral("mountPath"), QStringLiteral("/run/media/user/1234-ABCD")}});
        mounted.append(QVariantMap {{QStringLiteral("id"), QStringLiteral("/block_devices/sdb")},
                                     {QStringLiteral("serial"), QStringLiteral("20200101.1")},
                                     {QStringLiteral("mountPath"), QStringLiteral("/run/media/user/1234-ABCD1")}});
        mounted.append(QVariantMap {{QStringLiteral("id"), QStringLiteral("/block_devices/sdc")},
                                     {QStringLiteral("serial"), QStringLiteral("OTHER-DEVICE")},
                                     {QStringLiteral("mountPath"), QStringLiteral("/run/media/user/OTHER")}});

        const QVariantList volumes = usbVolumesForSerial(mounted, QStringLiteral("20200101.1"));

        QCOMPARE(volumes.size(), 2);
        QCOMPARE(volumes.at(0).toMap().value(QStringLiteral("mountPath")).toString(),
                 QStringLiteral("/run/media/user/1234-ABCD"));
        QCOMPARE(volumes.at(1).toMap().value(QStringLiteral("mountPath")).toString(),
                 QStringLiteral("/run/media/user/1234-ABCD1"));
    }

    void usbVolumesForSerial_ignoresOtherDevices()
    {
        QVariantList mounted;
        mounted.append(QVariantMap {{QStringLiteral("serial"), QStringLiteral("OTHER-DEVICE")}});

        QVERIFY(usbVolumesForSerial(mounted, QStringLiteral("20200101.1")).isEmpty());
        QVERIFY(usbVolumesForSerial(QVariantList(), QStringLiteral("20200101.1")).isEmpty());
    }

    void history_noVaultOpen_returnsEmptyList()
    {
        UsbGrabberController controller;
        QVERIFY(controller.history().isEmpty());
    }

    void history_reflectsEntriesAppendedToTheOpenVault()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        VaultTreeModel vaultModel;
        vaultModel.setVaultPath(QUrl::fromLocalFile(vault.path()));
        QVERIFY(vaultModel.appendUsbGrabHistoryEntry({{QStringLiteral("deviceSerial"), QStringLiteral("A")},
                                                       {QStringLiteral("filesCopied"), 2}}));

        UsbGrabberController controller;
        controller.setVaultTreeModel(&vaultModel);

        const QVariantList history = controller.history();
        QCOMPARE(history.size(), 1);
        QCOMPARE(history.first().toMap().value(QStringLiteral("deviceSerial")).toString(), QStringLiteral("A"));
    }
};

QTEST_GUILESS_MAIN(TstUsbGrabberController)
#include "tst_usbgrabbercontroller.moc"
