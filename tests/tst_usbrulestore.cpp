// Living spec: UsbRuleStore — persisted USB grab-rule config (see CLAUDE.md's
// "USB Grabber" feature, DESIGN.md's "USB import" section).
//
// App-scoped via QSettings (NOT vault-scoped .transcriber/settings.json) —
// the same physical device is used across many vaults by the same user. See
// tst_transcription_queue.cpp's rollingAveragePhaseWeighting test for the
// same QStandardPaths::setTestModeEnabled()-based isolation precedent this
// file follows, per CLAUDE.md's "never touch the user's real vault or app
// config" rule.

#include <QtTest>

#include <QCoreApplication>
#include <QStandardPaths>

#include "UsbRuleStore.h"

class TstUsbRuleStore : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
        // Isolated org/app identity on top of setTestModeEnabled(), so this
        // suite's assertions on QSettings' exact contents can't collide with
        // any other test binary that also happens to touch "usbGrabRules"
        // (mirrors tst_transcription_queue.cpp's rollingAveragePhaseWeighting
        // precedent).
        QCoreApplication::setOrganizationName(QStringLiteral("suopTranscriber-tests"));
        QCoreApplication::setApplicationName(QStringLiteral("tst_usbrulestore"));
    }

    void init()
    {
        // Each test starts from a clean slate — remove every persisted rule
        // left over from a previous test in this same run.
        UsbRuleStore store;
        for (const UsbGrabRule &rule : store.loadAllRules())
            store.removeRule(rule.deviceSerial);
    }

    void neverConfiguredDevice_returnsDisabledDefaultWithSerialSet()
    {
        UsbRuleStore store;
        const UsbGrabRule rule = store.ruleForSerial(QStringLiteral("NEVER-SEEN-SERIAL"));
        QCOMPARE(rule.deviceSerial, QStringLiteral("NEVER-SEEN-SERIAL"));
        QCOMPARE(rule.enabled, false);
        // Load-bearing default, not incidental: an unconfigured device (or one
        // swept up by an enabled "*" catch-all) must never start copying itself
        // into the vault on plug-in. Auto-grab is opted into per device.
        QCOMPARE(rule.autoGrab, false);
        QCOMPARE(rule.preserveStructure, true);
        QVERIFY(rule.extensionFilter.isEmpty());
        QVERIFY(rule.startFolder.isEmpty());
        QVERIFY(rule.destinationFolder.isEmpty());
    }

    void saveRule_roundTripsEveryField()
    {
        UsbRuleStore store;
        UsbGrabRule rule;
        rule.deviceSerial = QStringLiteral("ABC123");
        rule.enabled = true;
        rule.autoGrab = true;
        rule.extensionFilter = {QStringLiteral("wav"), QStringLiteral("mp3")};
        // Backslashes and quantifiers must survive QSettings' own escaping
        // intact — a pattern that came back mangled would silently filter
        // differently than the one the user validated in the field.
        rule.filenameRegex = QStringLiteral("^REC_(\\d{4})\\.wav$");
        rule.preserveStructure = false;
        rule.startFolder = QStringLiteral("Recordings/Voice");
        rule.deviceLabel = QStringLiteral("Zoom H1n");
        rule.destinationFolder = QStringLiteral("Interviews/USB");
        store.saveRule(rule);

        const UsbGrabRule loaded = store.ruleForSerial(QStringLiteral("ABC123"));
        QCOMPARE(loaded.deviceSerial, rule.deviceSerial);
        QCOMPARE(loaded.enabled, rule.enabled);
        QCOMPARE(loaded.autoGrab, rule.autoGrab);
        QCOMPARE(loaded.extensionFilter, rule.extensionFilter);
        QCOMPARE(loaded.filenameRegex, rule.filenameRegex);
        QCOMPARE(loaded.preserveStructure, rule.preserveStructure);
        QCOMPARE(loaded.startFolder, rule.startFolder);
        QCOMPARE(loaded.deviceLabel, rule.deviceLabel);
        QCOMPARE(loaded.destinationFolder, rule.destinationFolder);
    }

    void saveRule_upsertsRatherThanDuplicating()
    {
        UsbRuleStore store;
        UsbGrabRule rule;
        rule.deviceSerial = QStringLiteral("DUP-1");
        rule.enabled = false;
        store.saveRule(rule);
        rule.enabled = true;
        rule.deviceLabel = QStringLiteral("Renamed");
        store.saveRule(rule);

        QCOMPARE(store.loadAllRules().size(), 1);
        const UsbGrabRule loaded = store.ruleForSerial(QStringLiteral("DUP-1"));
        QCOMPARE(loaded.enabled, true);
        QCOMPARE(loaded.deviceLabel, QStringLiteral("Renamed"));
    }

    void removeRule_deletesIt()
    {
        UsbRuleStore store;
        UsbGrabRule rule;
        rule.deviceSerial = QStringLiteral("TO-REMOVE");
        rule.enabled = true;
        store.saveRule(rule);
        QVERIFY(store.ruleForSerial(QStringLiteral("TO-REMOVE")).enabled);

        store.removeRule(QStringLiteral("TO-REMOVE"));
        // Removed means "back to the never-configured default", not merely
        // disabled — ruleForSerial() can't distinguish "removed" from
        // "always disabled" by design (see UsbRuleStore.h), so the only
        // observable contract is: enabled reverts to false.
        QCOMPARE(store.ruleForSerial(QStringLiteral("TO-REMOVE")).enabled, false);
    }

    // The literal "*" catch-all rule is just a normal rule keyed by that
    // sentinel serial — no special-cased storage path (see UsbRuleStore.h's
    // class doc comment).
    void catchAllRule_persistsLikeAnyOtherSerial()
    {
        UsbRuleStore store;
        UsbGrabRule rule;
        rule.deviceSerial = QStringLiteral("*");
        rule.enabled = true;
        rule.extensionFilter = {QStringLiteral("m4a")};
        store.saveRule(rule);

        const UsbGrabRule loaded = store.ruleForSerial(QStringLiteral("*"));
        QCOMPARE(loaded.enabled, true);
        QCOMPARE(loaded.extensionFilter, QStringList{QStringLiteral("m4a")});

        bool foundCatchAll = false;
        for (const UsbGrabRule &r : store.loadAllRules()) {
            if (r.deviceSerial == QStringLiteral("*"))
                foundCatchAll = true;
        }
        QVERIFY(foundCatchAll);
    }

    // A serial containing '/' would otherwise be silently split into nested
    // QSettings groups (see UsbRuleStore.cpp's encodeSerialKey() doc
    // comment) — round-tripping one exercises the percent-encoding path.
    void serialContainingSlash_roundTripsCorrectly()
    {
        UsbRuleStore store;
        UsbGrabRule rule;
        rule.deviceSerial = QStringLiteral("VEN/DOR-123");
        rule.enabled = true;
        store.saveRule(rule);

        const UsbGrabRule loaded = store.ruleForSerial(QStringLiteral("VEN/DOR-123"));
        QCOMPARE(loaded.deviceSerial, QStringLiteral("VEN/DOR-123"));
        QCOMPARE(loaded.enabled, true);
    }

    void loadAllRules_enumeratesEveryPersistedRule()
    {
        UsbRuleStore store;
        UsbGrabRule a;
        a.deviceSerial = QStringLiteral("DEV-A");
        a.enabled = true;
        UsbGrabRule b;
        b.deviceSerial = QStringLiteral("DEV-B");
        b.enabled = false;
        store.saveRule(a);
        store.saveRule(b);

        const QList<UsbGrabRule> all = store.loadAllRules();
        QCOMPARE(all.size(), 2);
        QStringList serials;
        for (const UsbGrabRule &r : all)
            serials << r.deviceSerial;
        QVERIFY(serials.contains(QStringLiteral("DEV-A")));
        QVERIFY(serials.contains(QStringLiteral("DEV-B")));
    }
};

QTEST_MAIN(TstUsbRuleStore)
#include "tst_usbrulestore.moc"
