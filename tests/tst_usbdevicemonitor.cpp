// Living spec: UsbDeviceMonitor's external-media filter — the gate that decides
// which mounted filesystems the USB grabber is even allowed to see (see
// CLAUDE.md's "USB Grabber" feature, DESIGN.md's "USB import" section).
//
// This is the safety-critical half of the class: without it, "does it have a
// mounted filesystem?" is the ONLY test a device faces, which makes the system
// drive (mounted at /) an ordinary grab target that a catch-all rule would
// recurse into and copy wholesale into the vault.
//
// UsbDeviceMonitor's D-Bus plumbing itself isn't exercised here — it needs a live
// UDisks2 on the system bus plus real hardware to hotplug, neither of which a unit
// test can stage. usbDeviceIsExternalMedia() is deliberately a free function over
// plain property maps precisely so the DECISION is testable without any of that:
// every map below is a verbatim capture of `udisksctl dump` output from a real
// machine, so these cases track what UDisks2 actually reports rather than what the
// docs claim it reports.

#include <QtTest>

#include <QFile>

#include <QVariantMap>

#include "UsbDeviceMonitor.h"

namespace {

// org.freedesktop.UDisks2.Block properties for /dev/nvme0n1p4 — the internal NVMe
// partition mounted at "/". The case this whole filter exists to reject.
QVariantMap systemDriveBlockProps()
{
    QVariantMap props;
    props.insert(QStringLiteral("HintSystem"), true);
    props.insert(QStringLiteral("HintAuto"), false);
    props.insert(QStringLiteral("HintIgnore"), false);
    return props;
}

// org.freedesktop.UDisks2.Drive properties for the Samsung SSD 990 PRO backing it.
// Note ConnectionBus is EMPTY (not "nvme" or "sata") for an internal drive.
QVariantMap systemDriveDriveProps()
{
    QVariantMap props;
    props.insert(QStringLiteral("ConnectionBus"), QString());
    props.insert(QStringLiteral("Removable"), false);
    props.insert(QStringLiteral("Ejectable"), false);
    props.insert(QStringLiteral("MediaRemovable"), false);
    props.insert(QStringLiteral("Model"), QStringLiteral("Samsung SSD 990 PRO 4TB"));
    props.insert(QStringLiteral("Serial"), QStringLiteral("S7EXAMPLE000000"));
    return props;
}

// org.freedesktop.UDisks2.Block properties for /dev/sda — a real USB recorder
// (Acme Mic Tx) mounted at /run/media/user/1234-ABCD. The case that must pass.
QVariantMap usbRecorderBlockProps()
{
    QVariantMap props;
    props.insert(QStringLiteral("HintSystem"), false);
    props.insert(QStringLiteral("HintAuto"), true);
    props.insert(QStringLiteral("HintIgnore"), false);
    return props;
}

QVariantMap usbRecorderDriveProps()
{
    QVariantMap props;
    props.insert(QStringLiteral("ConnectionBus"), QStringLiteral("usb"));
    props.insert(QStringLiteral("Removable"), true);
    props.insert(QStringLiteral("Ejectable"), true);
    props.insert(QStringLiteral("MediaRemovable"), true);
    props.insert(QStringLiteral("Vendor"), QStringLiteral("Acme"));
    props.insert(QStringLiteral("Model"), QStringLiteral("Mic Tx"));
    props.insert(QStringLiteral("Serial"), QStringLiteral("20200101.1"));
    return props;
}

}

class TstUsbDeviceMonitor : public QObject
{
    Q_OBJECT

private slots:
    // The headline guarantee: the drive the OS is running from is never a grab
    // target. If this ever regresses, a catch-all rule copies the root filesystem.
    void systemDriveIsNotExternalMedia()
    {
        QVERIFY(!usbDeviceIsExternalMedia(systemDriveBlockProps(), systemDriveDriveProps()));
    }

    // The other half: a genuine USB recorder must still come through, or the
    // feature does nothing at all.
    void usbRecorderIsExternalMedia()
    {
        QVERIFY(usbDeviceIsExternalMedia(usbRecorderBlockProps(), usbRecorderDriveProps()));
    }

    // HintSystem is the backstop and is checked FIRST: a device flagged as the
    // machine's own hardware is refused even when its Drive claims to be removable
    // USB media. Guards against a udev rule (or a lying enclosure) re-opening the
    // system drive as a grab target via the Removable/ConnectionBus path.
    void hintSystemVetoesRemovableUsbClaim()
    {
        QVariantMap block = usbRecorderBlockProps();
        block.insert(QStringLiteral("HintSystem"), true);
        QVERIFY(!usbDeviceIsExternalMedia(block, usbRecorderDriveProps()));
    }

    // Conversely HintSystem=false alone is NOT enough — it says "not the machine's
    // own hardware", which is not the same claim as "external removable media".
    // Some internal/virtual block devices land here, so positive evidence from the
    // Drive is still required.
    void nonSystemButNonRemovableInternalBusIsRejected()
    {
        QVariantMap block = systemDriveBlockProps();
        block.insert(QStringLiteral("HintSystem"), false);
        QVERIFY(!usbDeviceIsExternalMedia(block, systemDriveDriveProps()));
    }

    // A USB SSD / enclosure that reports Removable=false (the MEDIA isn't removable
    // from the drive, even though the drive is removable from the machine) is still
    // external — ConnectionBus carries the day. This is why the check is
    // Removable OR bus, not Removable AND bus.
    void nonRemovableUsbDriveIsStillExternalMedia()
    {
        QVariantMap drive = usbRecorderDriveProps();
        drive.insert(QStringLiteral("Removable"), false);
        drive.insert(QStringLiteral("MediaRemovable"), false);
        QVERIFY(usbDeviceIsExternalMedia(usbRecorderBlockProps(), drive));
    }

    // Internal SD card readers report ConnectionBus="sdio"; field recorders
    // routinely present their storage that way, so it counts as external media.
    void sdioCardReaderIsExternalMedia()
    {
        QVariantMap drive = usbRecorderDriveProps();
        drive.insert(QStringLiteral("ConnectionBus"), QStringLiteral("sdio"));
        drive.insert(QStringLiteral("Removable"), false);
        QVERIFY(usbDeviceIsExternalMedia(usbRecorderBlockProps(), drive));
    }

    // Fail closed. Properties can go missing when a Drive object vanishes
    // mid-lookup (driveProperties() returns an empty map on a failed D-Bus call),
    // and "we couldn't tell" must never resolve to "grab it".
    void missingPropertiesAreRefused()
    {
        QVERIFY(!usbDeviceIsExternalMedia(QVariantMap(), QVariantMap()));

        // Drive props readable, Block props gone: HintSystem unknown -> treated as
        // system hardware, refused despite the drive looking like clean USB media.
        QVERIFY(!usbDeviceIsExternalMedia(QVariantMap(), usbRecorderDriveProps()));

        // Block props readable and non-system, Drive props gone: no positive
        // evidence of external media, refused.
        QVERIFY(!usbDeviceIsExternalMedia(usbRecorderBlockProps(), QVariantMap()));
    }

    // UDisks2 reports lowercase "usb", but nothing in the wire format promises
    // that, and a case-sensitive compare would silently reject every device.
    void connectionBusMatchIsCaseInsensitive()
    {
        QVariantMap drive = usbRecorderDriveProps();
        drive.insert(QStringLiteral("Removable"), false);
        drive.insert(QStringLiteral("ConnectionBus"), QStringLiteral("USB"));
        QVERIFY(usbDeviceIsExternalMedia(usbRecorderBlockProps(), drive));
    }

    // THE multi-LUN bug, pinned. The real Acme Mic Tx exposes /dev/sda and
    // /dev/sdb as two Block objects sharing ONE Drive object — so both report the
    // identical serial "20200101.1". Deriving `id` from the serial gave both volumes
    // the same id, which made unmounting one report BOTH as gone (deviceRemoved
    // carries an id) and let two concurrent grab jobs evict each other from the
    // controller's in-flight map. Distinct ids come from the object path, which
    // UDisks2 guarantees is per-block-device.
    void twoVolumesSharingOneSerial_getDistinctIds()
    {
        const auto sda = usbVolumeFromProperties(QStringLiteral("/org/freedesktop/UDisks2/block_devices/sda"),
                                                  QStringLiteral("/run/media/user/1234-ABCD"),
                                                  usbRecorderBlockProps(), usbRecorderDriveProps());
        const auto sdb = usbVolumeFromProperties(QStringLiteral("/org/freedesktop/UDisks2/block_devices/sdb"),
                                                  QStringLiteral("/run/media/user/1234-ABCD1"),
                                                  usbRecorderBlockProps(), usbRecorderDriveProps());
        QVERIFY(sda.has_value());
        QVERIFY(sdb.has_value());

        // Same physical device => same rule key. This is correct and required: one
        // rule covers the whole recorder.
        QCOMPARE(sda->serial, sdb->serial);

        // ...but they are different volumes and must never collide at runtime.
        QVERIFY(sda->id != sdb->id);
        QCOMPARE(sda->mountPath, QStringLiteral("/run/media/user/1234-ABCD"));
        QCOMPARE(sdb->mountPath, QStringLiteral("/run/media/user/1234-ABCD1"));
    }

    // The id is the object path verbatim — not the serial, and not derived from it.
    // Spelled out because "id == serial" is the exact regression to prevent, and it
    // would look harmless on every single-volume device anyone tests by hand.
    void volumeId_isTheObjectPath_notTheSerial()
    {
        const auto volume = usbVolumeFromProperties(QStringLiteral("/org/freedesktop/UDisks2/block_devices/sda"),
                                                     QStringLiteral("/run/media/user/1234-ABCD"),
                                                     usbRecorderBlockProps(), usbRecorderDriveProps());
        QVERIFY(volume.has_value());
        QCOMPARE(volume->id, QStringLiteral("/org/freedesktop/UDisks2/block_devices/sda"));
        QCOMPARE(volume->serial, QStringLiteral("20200101.1"));
        QVERIFY(volume->id != volume->serial);
    }

    // Cheap USB flash often reports no serial at all. That's survivable — rules
    // keyed by an empty serial are a separate, pre-existing concern — but it must
    // not cost the volume its unique runtime id, which the old vendor+model+path
    // synthetic fallback couldn't fully guarantee and the object path does.
    void blankSerialVolumes_stillGetDistinctIds()
    {
        QVariantMap drive = usbRecorderDriveProps();
        drive.insert(QStringLiteral("Serial"), QString());

        const auto first = usbVolumeFromProperties(QStringLiteral("/org/freedesktop/UDisks2/block_devices/sdc"),
                                                    QStringLiteral("/run/media/user/FLASH1"),
                                                    usbRecorderBlockProps(), drive);
        const auto second = usbVolumeFromProperties(QStringLiteral("/org/freedesktop/UDisks2/block_devices/sdd"),
                                                     QStringLiteral("/run/media/user/FLASH2"),
                                                     usbRecorderBlockProps(), drive);
        QVERIFY(first.has_value());
        QVERIFY(second.has_value());
        QVERIFY(first->serial.isEmpty());
        QVERIFY(first->id != second->id);
    }

    // The external-media gate applies through this path too — a volume on the system
    // drive never becomes a UsbDeviceInfo at all, whatever its object path.
    void systemDriveVolume_yieldsNoDeviceInfo()
    {
        const auto volume = usbVolumeFromProperties(QStringLiteral("/org/freedesktop/UDisks2/block_devices/nvme0n1p4"),
                                                     QStringLiteral("/"), systemDriveBlockProps(),
                                                     systemDriveDriveProps());
        QVERIFY(!volume.has_value());
    }

    // An UNMOUNTED external volume still yields full identity — no mount is needed to
    // read Vendor/Model/Serial, since those come off the Drive object. That's what
    // makes the app-side mount possible at all: UsbGrabberController has to resolve
    // this volume's rule by serial BEFORE anything is mounted, to decide whether to
    // mount it (see its "Mount policy" doc comment). The empty mountPath is the ONLY
    // difference from the mounted case, and is how callers tell the two apart.
    void unmountedExternalVolume_yieldsIdentityWithEmptyMountPath()
    {
        const auto volume = usbVolumeFromProperties(QStringLiteral("/org/freedesktop/UDisks2/block_devices/sda"),
                                                     QString(), usbRecorderBlockProps(), usbRecorderDriveProps());
        QVERIFY(volume.has_value());
        QVERIFY(volume->mountPath.isEmpty());
        QCOMPARE(volume->serial, QStringLiteral("20200101.1"));
        QCOMPARE(volume->id, QStringLiteral("/org/freedesktop/UDisks2/block_devices/sda"));
        QCOMPARE(volume->vendor, QStringLiteral("Acme"));
        QCOMPARE(volume->model, QStringLiteral("Mic Tx"));
    }

    // The headline guarantee again, on the mount path this time: an unmounted volume
    // of the system drive is not a mount candidate, so no rule — not even an enabled
    // "*" catch-all — can ever get the app to mount a system partition and then grab
    // it. The gate runs BEFORE the mount, not just before the grab; if this regresses,
    // "we only ever mount external media" stops being true.
    void unmountedSystemDriveVolume_yieldsNoDeviceInfo()
    {
        const auto volume = usbVolumeFromProperties(QStringLiteral("/org/freedesktop/UDisks2/block_devices/nvme0n1p3"),
                                                     QString(), systemDriveBlockProps(), systemDriveDriveProps());
        QVERIFY(!volume.has_value());
    }

    // The resume-from-sleep detection (item 057) is pure D-Bus/logind plumbing
    // with no unit-test seam (needs a fake system bus + real hardware — see this
    // file's header), so these are source-scan guards, the sanctioned tool when
    // there's no direct test path (CLAUDE.md).
    void resumeFromSleep_subscribesToPrepareForSleep()
    {
        const QString src = readSource();
        QVERIFY2(!src.isEmpty(), "UsbDeviceMonitor.cpp must be readable via SUOP_SRC_DIR");
        QVERIFY2(src.contains(QStringLiteral("PrepareForSleep")) && src.contains(QStringLiteral("login1")),
                 "UsbDeviceMonitor must subscribe to logind's PrepareForSleep so a device plugged in "
                 "while asleep is detected on wake (item 057)");
    }

    void resumeFromSleep_reEnumeratesAndReoffersBothMountedAndUnmounted()
    {
        const QString src = readSource();
        QVERIFY(!src.isEmpty());
        QVERIFY2(src.contains(QStringLiteral("reEnumerateAfterResume")),
                 "resume must re-enumerate via reEnumerateAfterResume() (item 057)");
        QVERIFY2(src.contains(QStringLiteral("GetManagedObjects")),
                 "the resume re-enumeration must query UDisks2 GetManagedObjects");
        // Both halves matter: the unmounted re-offer is load-bearing because a
        // stock KDE (automount off) leaves a slept-through device unmounted on wake.
        QVERIFY2(src.contains(QStringLiteral("emit deviceAdded"))
                     && src.contains(QStringLiteral("emit unmountedVolumeDetected")),
                 "resume re-enumeration must re-fire BOTH deviceAdded (mounted) and "
                 "unmountedVolumeDetected (present-but-unmounted) — item 057");
    }

private:
    static QString readSource()
    {
        QFile f(QStringLiteral(SUOP_SRC_DIR "/core/UsbDeviceMonitor.cpp"));
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
            return QString();
        return QString::fromUtf8(f.readAll());
    }
};

QTEST_MAIN(TstUsbDeviceMonitor)
#include "tst_usbdevicemonitor.moc"
