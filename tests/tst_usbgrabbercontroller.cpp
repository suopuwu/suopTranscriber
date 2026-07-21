// Living spec: UsbGrabberController — the QML-facing coordinator for the USB
// Grabber feature (see CLAUDE.md). Covers the parts that don't require a live
// UDisks2/D-Bus session: rule CRUD (QVariantMap <-> UsbGrabRule projection),
// the no-vault-open and device-not-connected guard paths, history proxying,
// and — via GrabFixture below — the hotplug behavior itself: the two gates
// (enabled, then autoGrab), a grab becoming a QUEUED TASK on
// TranscriptionController rather than a thread of its own, the history entry,
// and the autoGrabsSettled() barrier the auto-combiner hangs off.
//
// UsbDeviceMonitor's own D-Bus hotplug DETECTION still isn't unit-testable
// without a fake D-Bus session/UDisks2 service; what's driven here is
// everything downstream of it, by invoking the handleDeviceAdded slot the
// monitor would have called (see GrabFixture::hotplug()).

#include <QtTest>

#include <QCoreApplication>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QUrl>
#include <QVariantMap>

#include "TranscriptionController.h"
#include "UsbGrabberController.h"
#include "VaultTreeModel.h"

namespace {

// A whole wired-up grabber: temp vault, temp "device" to stand in for a mount
// point, a REAL TranscriptionController (the queue is the thing under test — a
// stub would only agree with itself), and a controller with both wired in.
//
// Declaration order matters and is deliberate: the QTemporaryDirs and the two
// collaborators must all outlive `controller`, which unregisters from the
// transcription controller in its own teardown.
struct GrabFixture {
    QTemporaryDir vault;
    QTemporaryDir device;
    VaultTreeModel vaultModel;
    TranscriptionController transcription;
    UsbGrabberController controller;

    GrabFixture()
    {
        vaultModel.setVaultPath(QUrl::fromLocalFile(vault.path()));
        controller.setVaultTreeModel(&vaultModel);
        controller.setTranscriptionController(&transcription);
    }

    bool writeDeviceFile(const QString &name, const QByteArray &content)
    {
        QFile f(device.filePath(name));
        if (!f.open(QIODevice::WriteOnly))
            return false;
        return f.write(content) == content.size();
    }

    void saveRule(const QString &serial, bool enabled, bool autoGrab)
    {
        QVariantMap rule;
        rule[QStringLiteral("deviceSerial")] = serial;
        rule[QStringLiteral("enabled")] = enabled;
        rule[QStringLiteral("autoGrab")] = autoGrab;
        controller.saveRule(rule);
    }

    bool writeVaultFile(const QString &name, const QByteArray &content)
    {
        QFile f(vault.filePath(name));
        if (!f.open(QIODevice::WriteOnly))
            return false;
        return f.write(content) == content.size();
    }

    // Back-date a file's mtime well past the combine stability window, so a
    // group made of it counts as "ready" rather than "pending" (a recorder may
    // still be writing the newest part). A grab preserves the source mtime, so
    // an old device file lands old — modelling a recording captured a while ago.
    static bool backdate(const QString &absPath)
    {
        QFile f(absPath);
        if (!f.open(QIODevice::ReadWrite))
            return false;
        const bool ok = f.setFileTime(QDateTime::currentDateTime().addSecs(-3600),
                                       QFileDevice::FileModificationTime);
        f.close();
        return ok;
    }
    bool backdateDeviceFile(const QString &name) { return backdate(device.filePath(name)); }
    bool backdateVaultFile(const QString &name) { return backdate(vault.filePath(name)); }

    // A combine rule grouping REC<n>.wav parts: the digits are the sort
    // differentiator (group 1), so the grouping key is empty and every part
    // lands in one group ordered 1,2,3…
    static QVariantList recCombineRules()
    {
        QVariantMap rule;
        rule[QStringLiteral("name")] = QStringLiteral("Rec");
        rule[QStringLiteral("pattern")] = QStringLiteral("REC(\\d+)\\.wav");
        rule[QStringLiteral("enabled")] = true;
        rule[QStringLiteral("sortGroupIndex")] = 1;
        return QVariantList{rule};
    }

    int combineJobCount() const
    {
        int count = 0;
        for (const QVariant &jobVariant : transcription.jobs()) {
            if (jobVariant.toMap().value(QStringLiteral("kind")).toString() == QStringLiteral("combine"))
                ++count;
        }
        return count;
    }

    // Fake a newly-mounted volume. handleDeviceAdded is a private slot; moc
    // registers it anyway, which is what makes a hotplug drivable with no
    // UDisks2 session (the monitor is owned internally — nothing to inject).
    void hotplug(const QString &serial)
    {
        QVariantMap info;
        info[QStringLiteral("serial")] = serial;
        info[QStringLiteral("id")] = QStringLiteral("/block_devices/fake-") + serial;
        info[QStringLiteral("mountPath")] = device.path();
        QVERIFY(QMetaObject::invokeMethod(&controller, "handleDeviceAdded", Qt::DirectConnection,
                                           Q_ARG(QVariantMap, info)));
    }

    // Every "usbGrab" row in the queue, whatever its status.
    int usbGrabJobCount() const { return countUsbGrabJobs(/*pendingOnly=*/false); }

    // Only the ones still to run. A finished job STAYS in jobs() as a Done
    // history row until clearFinished(), so "the grabs have drained" is not
    // "no usbGrab rows" — it's "no unfinished ones".
    int pendingUsbGrabJobCount() const { return countUsbGrabJobs(/*pendingOnly=*/true); }

    // The id of the first "usbGrab" row in the given status, or -1. Ids aren't
    // knowable up front (enqueueUsbGrab's return goes to the controller), so
    // targeting a specific job means finding it by status.
    int firstUsbGrabJobIdWithStatus(const QString &status) const
    {
        for (const QVariant &jobVariant : transcription.jobs()) {
            const QVariantMap job = jobVariant.toMap();
            if (job.value(QStringLiteral("kind")).toString() == QStringLiteral("usbGrab")
                    && job.value(QStringLiteral("status")).toString() == status) {
                return job.value(QStringLiteral("id")).toInt();
            }
        }
        return -1;
    }

    int countUsbGrabJobs(bool pendingOnly) const
    {
        int count = 0;
        for (const QVariant &jobVariant : transcription.jobs()) {
            const QVariantMap job = jobVariant.toMap();
            if (job.value(QStringLiteral("kind")).toString() != QStringLiteral("usbGrab"))
                continue;
            const QString status = job.value(QStringLiteral("status")).toString();
            if (pendingOnly && (status == QStringLiteral("done") || status == QStringLiteral("failed")))
                continue;
            ++count;
        }
        return count;
    }
};

}

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

    // rescanForAutoGrab() (app-closed-while-plugged-in gap) routes every
    // already-mounted device through the SAME maybeAutoGrab() gate a hotplug
    // uses, so its enabled/autoGrab/vault-open guards are exactly the ones the
    // handleDeviceAdded_* tests above already cover. What's NOT unit-testable
    // is the enumeration itself (currentlyMountedDevices() hits real UDisks2 —
    // no fake-monitor injection point). This guards the null-safety: rescan
    // with no vault/controller wired must be a silent no-op, not a crash.
    void rescanForAutoGrab_withNothingWired_isSafeNoOp()
    {
        UsbGrabberController controller;
        controller.rescanForAutoGrab(); // no vault, no transcription controller
        QVERIFY(controller.lastError().isEmpty());

        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        VaultTreeModel vaultModel;
        vaultModel.setVaultPath(QUrl::fromLocalFile(vault.path()));
        controller.setVaultTreeModel(&vaultModel);
        controller.rescanForAutoGrab(); // vault but no transcription controller -> still safe
        QVERIFY(controller.lastError().isEmpty());
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
        // Same load-bearing default as the store's (see tst_usbrulestore.cpp),
        // re-pinned at the projection layer the auto-grab Switch actually binds
        // to: the key must be PRESENT and false, or the switch reads undefined.
        QVERIFY(rule.contains(QStringLiteral("autoGrab")));
        QCOMPARE(rule.value(QStringLiteral("autoGrab")).toBool(), false);
        QCOMPARE(rule.value(QStringLiteral("preserveStructure")).toBool(), true);
        QVERIFY(rule.value(QStringLiteral("extensionFilter")).toStringList().isEmpty());
        // Blank = "no regex constraint". The key must still be PRESENT: the
        // rule cards bind to it, and a missing key would read as undefined.
        QVERIFY(rule.contains(QStringLiteral("filenameRegex")));
        QVERIFY(rule.value(QStringLiteral("filenameRegex")).toString().isEmpty());
    }

    // Backs the live ✓/✗ indicator under the pattern field. Must agree with
    // the engine runUsbGrabScan() actually filters with (QRegularExpression), which
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
        rule[QStringLiteral("autoGrab")] = true;
        rule[QStringLiteral("extensionFilter")] = QStringList{QStringLiteral("wav"), QStringLiteral("mp3")};
        rule[QStringLiteral("filenameRegex")] = QStringLiteral("^REC_\\d+");
        rule[QStringLiteral("preserveStructure")] = false;
        rule[QStringLiteral("startFolder")] = QStringLiteral("Recordings");
        rule[QStringLiteral("deviceLabel")] = QStringLiteral("Zoom H1n");
        rule[QStringLiteral("destinationFolder")] = QStringLiteral("Interviews/USB");

        controller.saveRule(rule);

        const QVariantMap loaded = controller.ruleForDevice(QStringLiteral("XYZ"));
        QCOMPARE(loaded.value(QStringLiteral("enabled")).toBool(), true);
        QCOMPARE(loaded.value(QStringLiteral("autoGrab")).toBool(), true);
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

    // ---- Hotplug: the two gates, and grabs as queued tasks ----
    //
    // handleDeviceAdded() is a private slot, which moc still registers — so it
    // can be invoked directly here with a fabricated device map. That's the
    // whole test seam: the controller owns its UsbDeviceMonitor internally, so
    // there's no way to inject a fake hotplug through the real D-Bus path (the
    // same constraint that gave usbVolumesForSerial() its free-function shape).
    // A QTemporaryDir stands in for the mount point; nothing here needs UDisks2.

    void hotplug_enabledButAutoGrabOff_doesNotGrab()
    {
        GrabFixture fx;
        fx.saveRule(QStringLiteral("DEV-A"), /*enabled=*/true, /*autoGrab=*/false);

        fx.hotplug(QStringLiteral("DEV-A"));

        // Both gates must pass. `enabled` alone means "grabbable" (and is what
        // let the volume mount at all) — never "grab it without being asked".
        QCOMPARE(fx.usbGrabJobCount(), 0);
    }

    void hotplug_notEnabled_doesNotGrab_evenWithAutoGrabOn()
    {
        GrabFixture fx;
        fx.saveRule(QStringLiteral("DEV-A"), /*enabled=*/false, /*autoGrab=*/true);

        fx.hotplug(QStringLiteral("DEV-A"));

        // autoGrab is strictly narrower than enabled, never a way around it:
        // a disabled rule is not grabbable by any path.
        QCOMPARE(fx.usbGrabJobCount(), 0);
    }

    void hotplug_enabledAndAutoGrab_enqueuesGrabAsAQueuedTask()
    {
        GrabFixture fx;
        fx.saveRule(QStringLiteral("DEV-A"), /*enabled=*/true, /*autoGrab=*/true);

        fx.hotplug(QStringLiteral("DEV-A"));

        // The point of the feature: a grab is a job in the ONE task queue, not
        // a thread of its own — so it gets a Tasks row, the glow, pause/cancel.
        QCOMPARE(fx.usbGrabJobCount(), 1);
    }

    void hotplug_noVaultOpen_doesNotGrab()
    {
        GrabFixture fx;
        fx.controller.setVaultTreeModel(nullptr);
        fx.saveRule(QStringLiteral("DEV-A"), /*enabled=*/true, /*autoGrab=*/true);

        fx.hotplug(QStringLiteral("DEV-A"));

        // A grab only ever targets whatever vault is open at detection time —
        // with none open there's nowhere to put the files, so it's skipped
        // outright rather than queued against a vault chosen later.
        QCOMPARE(fx.usbGrabJobCount(), 0);
    }

    // ---- The auto-combine barrier ----

    void autoGrab_emitsAutoGrabsSettledOnceTheLastOneFinishes()
    {
        GrabFixture fx;
        QVERIFY(fx.writeDeviceFile(QStringLiteral("rec.wav"), "audio"));
        fx.saveRule(QStringLiteral("DEV-A"), /*enabled=*/true, /*autoGrab=*/true);

        QSignalSpy settledSpy(&fx.controller, &UsbGrabberController::autoGrabsSettled);
        QSignalSpy grabbedSpy(&fx.controller, &UsbGrabberController::grabFinished);

        fx.hotplug(QStringLiteral("DEV-A"));

        // Not before the grab has actually run: the whole contract is that the
        // combine waits for the files to be on disk.
        QCOMPARE(settledSpy.count(), 0);

        QVERIFY(grabbedSpy.wait(10000));
        QCOMPARE(grabbedSpy.first().at(1).toInt(), 1);          // filesCopied
        QCOMPARE(grabbedSpy.first().at(4).toBool(), true);       // autoTriggered
        QVERIFY(QFileInfo::exists(fx.vault.filePath(QStringLiteral("rec.wav"))));

        // Exactly once — a second firing would run the combine scan twice over
        // the same vault.
        QTRY_COMPARE(settledSpy.count(), 1);
        QCOMPARE(settledSpy.count(), 1);
    }

    void autoGrab_settledFiresOnlyAfterEveryVolumeOfTheBatchIsDone()
    {
        GrabFixture fx;
        QVERIFY(fx.writeDeviceFile(QStringLiteral("rec.wav"), "audio"));
        fx.saveRule(QStringLiteral("DEV-A"), /*enabled=*/true, /*autoGrab=*/true);
        fx.saveRule(QStringLiteral("DEV-B"), /*enabled=*/true, /*autoGrab=*/true);

        QSignalSpy settledSpy(&fx.controller, &UsbGrabberController::autoGrabsSettled);

        // Two auto-grabs in flight at once (a two-volume recorder, or two
        // devices plugged together). The barrier must hold until BOTH drain —
        // combining after the first would merge half a split recording.
        fx.hotplug(QStringLiteral("DEV-A"));
        fx.hotplug(QStringLiteral("DEV-B"));
        QCOMPARE(fx.usbGrabJobCount(), 2);

        QTRY_VERIFY_WITH_TIMEOUT(settledSpy.count() == 1, 10000);
        // One settle for the whole batch, not one per grab.
        QCOMPARE(settledSpy.count(), 1);
        // ...and it really did wait for both, rather than firing early and
        // leaving one still to run.
        QCOMPARE(fx.pendingUsbGrabJobCount(), 0);
    }

    void grab_writesHistoryEntryForTheDeviceThatRan()
    {
        GrabFixture fx;
        QVERIFY(fx.writeDeviceFile(QStringLiteral("rec.wav"), "audio"));
        fx.saveRule(QStringLiteral("DEV-A"), /*enabled=*/true, /*autoGrab=*/true);

        QSignalSpy grabbedSpy(&fx.controller, &UsbGrabberController::grabFinished);
        fx.hotplug(QStringLiteral("DEV-A"));
        QVERIFY(grabbedSpy.wait(10000));

        // History moved out of the grab worker (which can't touch the
        // GUI-thread-owned VaultTreeModel) into the controller — it must still
        // land, and grabFinished must arrive AFTER it so a handler that re-reads
        // history() on that signal already sees this run.
        const QVariantList history = fx.controller.history();
        QCOMPARE(history.size(), 1);
        QCOMPARE(history.first().toMap().value(QStringLiteral("deviceSerial")).toString(),
                 QStringLiteral("DEV-A"));
        QCOMPARE(history.first().toMap().value(QStringLiteral("filesCopied")).toInt(), 1);
    }

    void cancelledGrab_writesNoHistoryAndAnnouncesNothing_butStillSettles()
    {
        GrabFixture fx;
        QVERIFY(fx.writeDeviceFile(QStringLiteral("rec.wav"), "audio"));
        fx.saveRule(QStringLiteral("DEV-A"), /*enabled=*/true, /*autoGrab=*/true);

        QSignalSpy grabbedSpy(&fx.controller, &UsbGrabberController::grabFinished);
        QSignalSpy settledSpy(&fx.controller, &UsbGrabberController::autoGrabsSettled);

        fx.hotplug(QStringLiteral("DEV-A"));
        // Deterministic, not a race: startJob() calls resetControl() and then
        // dispatches the work QUEUED, so it hasn't run yet. cancelActive() sets
        // the cancel atomic synchronously, landing AFTER that reset and BEFORE
        // the worker's first abort check.
        fx.transcription.cancelActive();

        QTRY_COMPARE_WITH_TIMEOUT(settledSpy.count(), 1, 10000);

        // A cancelled grab's counts are a partial tally. Recording it in
        // history, or telling the user "grabbed N files", would misreport a run
        // they stopped as one that completed.
        QCOMPARE(grabbedSpy.count(), 0);
        QVERIFY(fx.controller.history().isEmpty());

        // ...but it must still release the barrier, or one cancelled auto-grab
        // would strand the batch and block the auto-combine forever.
        QCOMPARE(settledSpy.count(), 1);
    }

    // Regression: a grab cancelled while still QUEUED never reaches the worker,
    // so it never reports a result. Tracking it purely off that result left its
    // id in the auto-grab barrier forever — which didn't just lose one combine,
    // it permanently disabled the auto-combiner for the rest of the session,
    // silently. The barrier is reconciled against the queue for this reason.
    void autoGrabCancelledWhileStillQueued_doesNotStrandTheBarrier()
    {
        GrabFixture fx;
        QVERIFY(fx.writeDeviceFile(QStringLiteral("rec.wav"), "audio"));
        fx.saveRule(QStringLiteral("DEV-A"), /*enabled=*/true, /*autoGrab=*/true);
        fx.saveRule(QStringLiteral("DEV-B"), /*enabled=*/true, /*autoGrab=*/true);

        QSignalSpy settledSpy(&fx.controller, &UsbGrabberController::autoGrabsSettled);

        // Two auto-grabs: the first goes Active, the second waits behind it.
        fx.hotplug(QStringLiteral("DEV-A"));
        fx.hotplug(QStringLiteral("DEV-B"));
        QCOMPARE(fx.usbGrabJobCount(), 2);

        // Cancel the one that is still merely Queued — the queue drops it
        // outright, so no worker ever runs it and no result is ever reported
        // for it.
        const int queuedId = fx.firstUsbGrabJobIdWithStatus(QStringLiteral("queued"));
        QVERIFY2(queuedId >= 0, "expected the second grab to be waiting its turn");
        fx.transcription.cancelJob(queuedId);

        // The batch must still settle off the back of the OTHER grab finishing.
        QTRY_COMPARE_WITH_TIMEOUT(settledSpy.count(), 1, 10000);
        QCOMPARE(fx.pendingUsbGrabJobCount(), 0);
    }

    // ---- The auto-combine, now owned by the controller (Issue 1+2) ----

    // A clean auto-grab that lands a complete split recording combines it: the
    // controller runs the combine itself (moved out of Main.qml), rule-driven
    // and 'ready'-gated, once the batch settles.
    void autoCombine_cleanBatchCombinesReadySplitRecording()
    {
        GrabFixture fx;
        QVERIFY(fx.writeDeviceFile(QStringLiteral("REC001.wav"), "aaaa"));
        QVERIFY(fx.writeDeviceFile(QStringLiteral("REC002.wav"), "bbbb"));
        QVERIFY(fx.backdateDeviceFile(QStringLiteral("REC001.wav")));
        QVERIFY(fx.backdateDeviceFile(QStringLiteral("REC002.wav")));
        fx.saveRule(QStringLiteral("DEV-A"), /*enabled=*/true, /*autoGrab=*/true);
        fx.controller.setCombineRules(GrabFixture::recCombineRules());
        fx.controller.setAutoCombineAfterGrab(true);

        QSignalSpy settledSpy(&fx.controller, &UsbGrabberController::autoGrabsSettled);
        QSignalSpy combineSpy(&fx.controller, &UsbGrabberController::autoCombineStarted);

        fx.hotplug(QStringLiteral("DEV-A"));
        QTRY_COMPARE_WITH_TIMEOUT(settledSpy.count(), 1, 10000);

        // Both parts landed clean, so the ready group is combined: one group
        // announced, one combine task enqueued.
        QCOMPARE(combineSpy.count(), 1);
        QCOMPARE(combineSpy.first().at(0).toInt(), 1);
        QVERIFY(fx.combineJobCount() >= 1);
    }

    // The regression this whole change exists for: a DIRTY batch (here, a
    // cancelled auto-grab) must NOT trigger the auto-combine, even when a ready
    // split-recording group is sitting right there in the vault. The aborted
    // grab means the on-disk part set can't be trusted as complete — combining
    // it could merge a truncated recording, dropping its tail. The barrier still
    // releases (a cancelled grab must never strand it); only the combine is held.
    void autoCombine_dirtyBatchDoesNotCombineEvenWithAReadyGroupPresent()
    {
        GrabFixture fx;
        // A ready split recording already sits in the vault (say, from an
        // earlier grab), both parts past the stability window.
        QVERIFY(fx.writeVaultFile(QStringLiteral("REC001.wav"), "aaaa"));
        QVERIFY(fx.writeVaultFile(QStringLiteral("REC002.wav"), "bbbb"));
        QVERIFY(fx.backdateVaultFile(QStringLiteral("REC001.wav")));
        QVERIFY(fx.backdateVaultFile(QStringLiteral("REC002.wav")));

        // The device has an unrelated in-flight file; its auto-grab is cancelled,
        // which is what makes THIS batch dirty.
        QVERIFY(fx.writeDeviceFile(QStringLiteral("misc.wav"), "cccc"));
        fx.saveRule(QStringLiteral("DEV-A"), /*enabled=*/true, /*autoGrab=*/true);
        fx.controller.setCombineRules(GrabFixture::recCombineRules());
        fx.controller.setAutoCombineAfterGrab(true);

        QSignalSpy settledSpy(&fx.controller, &UsbGrabberController::autoGrabsSettled);
        QSignalSpy combineSpy(&fx.controller, &UsbGrabberController::autoCombineStarted);

        fx.hotplug(QStringLiteral("DEV-A"));
        // Deterministic (see cancelledGrab_* above): the cancel atomic is set
        // after startJob()'s reset and before the worker's first abort check.
        fx.transcription.cancelActive();

        QTRY_COMPARE_WITH_TIMEOUT(settledSpy.count(), 1, 10000);

        // Barrier released, but the ready group is left alone.
        QCOMPARE(settledSpy.count(), 1);
        QCOMPARE(combineSpy.count(), 0);
        QCOMPARE(fx.combineJobCount(), 0);
    }

    // The gate must not be sticky: a dirty batch does not poison the NEXT batch.
    // After the cancelled grab above, a fresh clean auto-grab of the same ready
    // group combines normally.
    void autoCombine_freshCleanBatchAfterADirtyOneStillCombines()
    {
        GrabFixture fx;
        QVERIFY(fx.writeDeviceFile(QStringLiteral("REC001.wav"), "aaaa"));
        QVERIFY(fx.writeDeviceFile(QStringLiteral("REC002.wav"), "bbbb"));
        QVERIFY(fx.backdateDeviceFile(QStringLiteral("REC001.wav")));
        QVERIFY(fx.backdateDeviceFile(QStringLiteral("REC002.wav")));
        fx.saveRule(QStringLiteral("DEV-A"), /*enabled=*/true, /*autoGrab=*/true);
        fx.controller.setCombineRules(GrabFixture::recCombineRules());
        fx.controller.setAutoCombineAfterGrab(true);

        QSignalSpy settledSpy(&fx.controller, &UsbGrabberController::autoGrabsSettled);
        QSignalSpy combineSpy(&fx.controller, &UsbGrabberController::autoCombineStarted);

        // First batch: cancelled → dirty → no combine.
        fx.hotplug(QStringLiteral("DEV-A"));
        fx.transcription.cancelActive();
        QTRY_COMPARE_WITH_TIMEOUT(settledSpy.count(), 1, 10000);
        QCOMPARE(combineSpy.count(), 0);

        // Second batch: clean → the fresh batch resets the clean flag and the
        // group (parts now on disk from the retry) combines.
        fx.hotplug(QStringLiteral("DEV-A"));
        QTRY_COMPARE_WITH_TIMEOUT(settledSpy.count(), 2, 10000);
        QCOMPARE(combineSpy.count(), 1);
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
