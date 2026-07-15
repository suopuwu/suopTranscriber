#pragma once

#include <QDBusObjectPath>
#include <QHash>
#include <QMap>
#include <QObject>
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

// One mounted USB filesystem, identified by its underlying UDisks2 Drive object
// (Vendor/Model/Serial) rather than its transient block-device path or mount point —
// both of which can change across replug/remount. Plain internal data (mirrors
// Segment/DiarizedTurn's style, see Segment.h/DiarizationEngine.h) — the QML-facing
// surface below projects this into a QVariantMap instead of exposing the struct
// itself, matching how the rest of the codebase hands structured data to QML (e.g.
// TranscriptionController's transcriptLoaded sentences, VaultTreeModel::
// searchResults) rather than registering a new Q_GADGET value type, since no such
// value-type pattern exists anywhere else in this codebase.
struct UsbDeviceInfo {
    QString id;
    QString serial;
    QString vendor;
    QString model;
    QString mountPath;
};

// Hotplug detection for USB mass-storage devices via UDisks2, over the D-Bus SYSTEM
// bus (UDisks2 is a system-bus service, unlike most Qt D-Bus session-bus examples).
// Subscribes to InterfacesAdded/InterfacesRemoved on UDisks2's ObjectManager
// (org.freedesktop.UDisks2 at /org/freedesktop/UDisks2) immediately at construction,
// and reports stable device identity (Vendor/Model/Serial off the underlying Drive
// object, not the transient block-device or mount path) whenever a filesystem is
// mounted or unmounted.
//
// Detection only — no recursive file scanning, copying, or rule-matching happens
// here; that's UsbGrabJob, a separate, not-yet-built consumer of these signals.
class UsbDeviceMonitor : public QObject {
    Q_OBJECT
    QML_ELEMENT

public:
    explicit UsbDeviceMonitor(QObject *parent = nullptr);

    // One QVariantMap per already-mounted UDisks2 filesystem at call time (keys: id,
    // serial, vendor, model, mountPath), via an initial GetManagedObjects()
    // enumeration — covers devices plugged in before this monitor was constructed,
    // not just future hotplug events. Reuses the same Filesystem/Block/Drive
    // property-reading path as the live InterfacesAdded handler
    // (readMountedFilesystem()) rather than duplicating it.
    Q_INVOKABLE QVariantList currentlyMountedDevices() const;

signals:
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
    // Filesystem/Block/Drive property-reading logic exists exactly once. Returns
    // std::nullopt for objects that aren't an actively-mounted filesystem (no
    // Filesystem interface present, or a present-but-empty MountPoints list).
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

    // Tracks object path -> device id for every currently-mounted filesystem this
    // monitor has seen (seeded at construction from the initial enumeration, kept
    // current by the live handlers) — InterfacesRemoved only reports which
    // interfaces went away on which object path, never the properties that were
    // there before removal, so this is the only way to resolve a removal back to a
    // device id. Mutable: currentlyMountedDevices() is logically const (it just
    // reports current state) but still needs to keep this bookkeeping current.
    mutable QHash<QString, QString> m_knownObjectPaths;
};
