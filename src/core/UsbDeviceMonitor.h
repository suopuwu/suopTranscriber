#pragma once

#include <QDBusObjectPath>
#include <QMap>
#include <QObject>
#include <QSet>
#include <QString>
#include <QVariantList>
#include <QVariantMap>
#include <qqml.h>

#include <optional>

// D-Bus's a{sa{sv}} (interface name -> property name -> value) — the shape UDisks2's
// InterfacesAdded signal and GetManagedObjects() both use per object. Named as a
// typedef (rather than spelled out inline) because a bare template argument list with
// a comma can't survive the SLOT()/Q_DECLARE_METATYPE() macros used below.
typedef QMap<QString, QVariantMap> UsbDbusInterfaces;
Q_DECLARE_METATYPE(UsbDbusInterfaces)

// D-Bus's a{oa{sa{sv}}} — GetManagedObjects()'s full return shape, one
// UsbDbusInterfaces per UDisks2 object path.
typedef QMap<QDBusObjectPath, UsbDbusInterfaces> UsbDbusManagedObjects;
Q_DECLARE_METATYPE(UsbDbusManagedObjects)

// One mounted filesystem (VOLUME) on external media. Plain internal data (mirrors
// Segment/DiarizedTurn's style, see Segment.h/DiarizationEngine.h) — the QML-facing
// surface below projects this into a QVariantMap instead of exposing the struct
// itself, matching how the rest of the codebase hands structured data to QML (e.g.
// TranscriptionController's transcriptLoaded sentences, VaultTreeModel::
// searchResults) rather than registering a new Q_GADGET value type, since no such
// value-type pattern exists anywhere else in this codebase.
//
// `id` and `serial` are TWO DIFFERENT IDENTITIES and must not be conflated — doing
// so is a real bug this codebase already shipped once:
//
//   - `serial` identifies the PHYSICAL DEVICE and is stable across replug. It's the
//     rule key (UsbRuleStore::ruleForSerial()). One physical device = one rule, so
//     several volumes CAN legitimately share a serial: a multi-LUN recorder that
//     exposes internal memory and an SD card as two block devices reports one Drive
//     object, hence one Vendor/Model/Serial, for both. It is therefore NOT unique
//     per volume, and may be empty (cheap USB flash often reports no serial at all).
//
//   - `id` identifies THIS VOLUME's mount, uniquely, for as long as it stays
//     mounted — it's the UDisks2 block-device object path. Runtime bookkeeping only
//     (which mount went away, which in-flight job belongs to which volume), so
//     across-replug stability isn't needed and isn't offered: /dev/sda can come back
//     as /dev/sdc. Never persist it, and never use it as a rule key.
//
// Keying runtime state by `serial` is what broke before: on a multi-LUN device both
// volumes collapse to one key, so unmounting one reported BOTH as gone, and two
// concurrent grab jobs overwrote each other in the controller's in-flight map. The
// object path can't collide, because UDisks2 gives each block device its own.
struct UsbDeviceInfo {
    QString id;
    QString serial;
    QString vendor;
    QString model;
    QString mountPath;
};

// Whether one mounted filesystem sits on external, removable media rather than on
// the machine's own storage — the gate that keeps the system drive out of the
// grabber entirely. Without it, "is it mounted?" is the only test a device faces,
// which makes / and /boot ordinary grab targets that a catch-all rule (see
// UsbGrabberController::resolveEffectiveRule()) would happily recurse into.
//
// blockProps/driveProps are org.freedesktop.UDisks2.Block's and .Drive's property
// maps for the same device. Both signals are required, because each alone has a
// gap:
//   - Block.HintSystem is UDisks2's own "this is the machine's own hardware"
//     verdict and is what file managers gate on, but it is only a HINT — udev rules
//     (UDISKS_SYSTEM) can set it on anything, so it can't be the sole positive
//     evidence that a device is external.
//   - Drive.Removable / Drive.ConnectionBus positively identify external media, but
//     say nothing about whether the OS considers the device its own; HintSystem is
//     the backstop there.
// Absent properties are read as their most conservative value (HintSystem missing
// is treated as "system", Removable missing as "not removable"), so a device whose
// properties can't be read is refused rather than grabbed.
//
// Deliberately a free function taking plain maps, not a UsbDeviceMonitor member:
// this is the whole safety decision, and this shape lets tst_usbdevicemonitor
// exercise it against hand-built property maps with no D-Bus and no real hardware.
bool usbDeviceIsExternalMedia(const QVariantMap &blockProps, const QVariantMap &driveProps);

// One mounted volume's UsbDeviceInfo built from its UDisks2 properties, or nullopt
// if it isn't external media (usbDeviceIsExternalMedia() above). objectPath is the
// Block object's D-Bus path and becomes the volume's `id`; mountPath is its
// already-decoded mount point.
//
// Split out of UsbDeviceMonitor::readMountedFilesystem() — which keeps only the
// parts that genuinely need D-Bus (decoding the MountPoints byte-array wire format,
// and fetching driveProps for the Block's Drive object) — so that everything that
// DECIDES anything is a pure function over plain maps. That's what lets
// tst_usbdevicemonitor pin the id/serial split (see UsbDeviceInfo's doc comment)
// against hand-built properties, with no D-Bus and no multi-LUN hardware to hand.
std::optional<UsbDeviceInfo> usbVolumeFromProperties(const QString &objectPath, const QString &mountPath,
                                                      const QVariantMap &blockProps,
                                                      const QVariantMap &driveProps);

// Hotplug detection for USB mass-storage devices via UDisks2, over the D-Bus SYSTEM
// bus (UDisks2 is a system-bus service, unlike most Qt D-Bus session-bus examples).
// Subscribes to InterfacesAdded/InterfacesRemoved on UDisks2's ObjectManager
// (org.freedesktop.UDisks2 at /org/freedesktop/UDisks2) immediately at construction,
// and reports stable device identity (Vendor/Model/Serial off the underlying Drive
// object, not the transient block-device or mount path) whenever a filesystem is
// mounted or unmounted.
//
// Reports EXTERNAL REMOVABLE MEDIA ONLY — never the machine's own storage. Both the
// live signals and currentlyMountedDevices() run every candidate through
// usbDeviceIsExternalMedia() (see below), so internal drives are invisible to this
// class rather than merely deprioritised by callers. That filter is load-bearing,
// not cosmetic: "mounted" alone would make / and /boot grabbable devices.
//
// Detection only — no recursive file scanning, copying, or rule-matching happens
// here; that's UsbGrabJob, a separate, not-yet-built consumer of these signals.
class UsbDeviceMonitor : public QObject {
    Q_OBJECT
    QML_ELEMENT

public:
    explicit UsbDeviceMonitor(QObject *parent = nullptr);

    // One QVariantMap per already-mounted UDisks2 filesystem ON EXTERNAL REMOVABLE
    // MEDIA at call time (keys: id, serial, vendor, model, mountPath) — per VOLUME,
    // so several entries can share one `serial` (a multi-LUN device); callers that
    // group by device must expect that rather than assume serials are unique across
    // the list. Built via an initial GetManagedObjects() enumeration — covers devices plugged in before this monitor
    // was constructed, not just future hotplug events. Reuses the same
    // Filesystem/Block/Drive property-reading path as the live InterfacesAdded handler
    // (readMountedFilesystem()) rather than duplicating it, so the external-media
    // filter applies identically to both.
    Q_INVOKABLE QVariantList currentlyMountedDevices() const;

signals:
    // Both are per-VOLUME, not per-physical-device: a multi-LUN device that mounts
    // two filesystems fires two deviceAdded, and unmounting just one fires exactly
    // one deviceRemoved carrying only that volume's id. Consumers must therefore key
    // their own state by `id` (unique per volume) rather than `serial` (shared by a
    // device's volumes) — see UsbDeviceInfo's doc comment.
    //
    // info: {id, serial, vendor, model, mountPath} — see UsbDeviceInfo's fields.
    void deviceAdded(const QVariantMap &info);
    void deviceRemoved(const QString &id);

private slots:
    // Bound to UDisks2's ObjectManager InterfacesAdded/InterfacesRemoved signals (see
    // the constructor). Parameter types must match the D-Bus signals' marshalled
    // shape exactly (QDBusObjectPath + UsbDbusInterfaces / QStringList) for
    // QDBusConnection::connect()'s runtime signature matching to find them.
    void handleInterfacesAdded(const QDBusObjectPath &objectPath, const UsbDbusInterfaces &interfaces);
    void handleInterfacesRemoved(const QDBusObjectPath &objectPath, const QStringList &interfaces);

private:
    // Shared by handleInterfacesAdded() and currentlyMountedDevices() so the
    // Filesystem/Block/Drive property-reading logic — and the external-media filter
    // — exist exactly once. Returns std::nullopt for objects that aren't an
    // actively-mounted filesystem (no Filesystem interface present, or a
    // present-but-empty MountPoints list), and for those that are mounted but fail
    // usbDeviceIsExternalMedia() (the system drive, internal data disks, zram).
    std::optional<UsbDeviceInfo> readMountedFilesystem(const QDBusObjectPath &objectPath,
                                                        const UsbDbusInterfaces &interfaces) const;

    // UDisks2 commonly adds a block device's Block/Filesystem interfaces (this class's
    // InterfacesAdded handler) slightly BEFORE an auto-mount policy actually mounts it
    // — the mount itself lands moments later as a Properties.PropertiesChanged on
    // Filesystem's MountPoints, which this class does not subscribe to (that needs the
    // emitting object's path, which PropertiesChanged doesn't carry in its payload).
    // Rather than take on that plumbing, handleInterfacesAdded() schedules one retry of
    // this re-scan after a short delay for any object that had a Filesystem interface
    // but wasn't mounted yet — it just re-runs the same GetManagedObjects() enumeration
    // currentlyMountedDevices() already does and emits deviceAdded for anything newly
    // mounted that m_knownObjectPaths doesn't already have. Safe to call redundantly:
    // already-known object paths are skipped.
    void rescanForNewlyMountedDevices();

    // org.freedesktop.DBus.Properties.GetAll on the given Drive object
    // (interface org.freedesktop.UDisks2.Drive) — Vendor/Model/Serial. Empty map if
    // the call fails (e.g. the Drive object vanished mid-lookup).
    QVariantMap driveProperties(const QDBusObjectPath &drivePath) const;

    // Every object path this monitor currently considers a mounted external volume
    // (seeded at construction from the initial enumeration, kept current by the live
    // handlers). InterfacesRemoved reports only which interfaces went away on which
    // object path — never the properties that were there before removal — so without
    // this, a removal couldn't be resolved back to a device at all, nor distinguished
    // from the removal of something that was never reported in the first place (an
    // internal drive, or a volume that never mounted).
    //
    // A set, not a path -> id map, because UsbDeviceInfo::id IS the object path (see
    // its doc comment) — keeping a map here would be storing the key twice, and the
    // per-volume uniqueness that makes removals correct comes from the object path
    // itself. Mutable: currentlyMountedDevices() is logically const (it just reports
    // current state) but still needs to keep this bookkeeping current.
    mutable QSet<QString> m_knownObjectPaths;
};
