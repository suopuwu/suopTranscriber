// Living spec: UsbGrabberController — the QML-facing coordinator for the USB
// Grabber feature (see CLAUDE.md). Covers the parts that don't require a live
// UDisks2/D-Bus session: rule CRUD (QVariantMap <-> UsbGrabRule projection),
// the no-vault-open and device-not-connected guard paths, and history
// proxying. UsbDeviceMonitor's own D-Bus hotplug detection isn't
// unit-testable without a fake D-Bus session/UDisks2 service — not covered
// here (see .task/plan.md's Decisions for that scope call).

#include <QtTest>

#include <QCoreApplication>
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

    void ruleForDevice_neverConfigured_returnsPopulatedDefaultMap()
    {
        UsbGrabberController controller;
        const QVariantMap rule = controller.ruleForDevice(QStringLiteral("NEVER-SEEN"));

        QCOMPARE(rule.value(QStringLiteral("deviceSerial")).toString(), QStringLiteral("NEVER-SEEN"));
        QCOMPARE(rule.value(QStringLiteral("enabled")).toBool(), false);
        QCOMPARE(rule.value(QStringLiteral("preserveStructure")).toBool(), true);
        QVERIFY(rule.value(QStringLiteral("extensionFilter")).toStringList().isEmpty());
    }

    void saveRule_thenRuleForDevice_roundTripsEveryField()
    {
        UsbGrabberController controller;
        QVariantMap rule;
        rule[QStringLiteral("deviceSerial")] = QStringLiteral("XYZ");
        rule[QStringLiteral("enabled")] = true;
        rule[QStringLiteral("extensionFilter")] = QStringList{QStringLiteral("wav"), QStringLiteral("mp3")};
        rule[QStringLiteral("preserveStructure")] = false;
        rule[QStringLiteral("startFolder")] = QStringLiteral("Recordings");
        rule[QStringLiteral("deviceLabel")] = QStringLiteral("Zoom H1n");

        controller.saveRule(rule);

        const QVariantMap loaded = controller.ruleForDevice(QStringLiteral("XYZ"));
        QCOMPARE(loaded.value(QStringLiteral("enabled")).toBool(), true);
        QCOMPARE(loaded.value(QStringLiteral("extensionFilter")).toStringList(),
                 QStringList({QStringLiteral("wav"), QStringLiteral("mp3")}));
        QCOMPARE(loaded.value(QStringLiteral("preserveStructure")).toBool(), false);
        QCOMPARE(loaded.value(QStringLiteral("startFolder")).toString(), QStringLiteral("Recordings"));
        QCOMPARE(loaded.value(QStringLiteral("deviceLabel")).toString(), QStringLiteral("Zoom H1n"));
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
