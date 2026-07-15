#include "UsbDeviceMonitor.h"

#include <QByteArrayList>
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusMetaType>
#include <QDBusReply>
#include <QTimer>

namespace {

QVariantMap toVariantMap(const UsbDeviceInfo &info)
{
    QVariantMap map;
    map.insert(QStringLiteral("id"), info.id);
    map.insert(QStringLiteral("serial"), info.serial);
    map.insert(QStringLiteral("vendor"), info.vendor);
    map.insert(QStringLiteral("model"), info.model);
    map.insert(QStringLiteral("mountPath"), info.mountPath);
    return map;
}

const QString kFilesystemInterface = QStringLiteral("org.freedesktop.UDisks2.Filesystem");
const QString kBlockInterface = QStringLiteral("org.freedesktop.UDisks2.Block");
const QString kDriveInterface = QStringLiteral("org.freedesktop.UDisks2.Drive");
const QString kUDisks2Service = QStringLiteral("org.freedesktop.UDisks2");
const QString kUDisks2RootPath = QStringLiteral("/org/freedesktop/UDisks2");
const QString kObjectManagerInterface = QStringLiteral("org.freedesktop.DBus.ObjectManager");
const QString kPropertiesInterface = QStringLiteral("org.freedesktop.DBus.Properties");

} // namespace

UsbDeviceMonitor::UsbDeviceMonitor(QObject *parent)
    : QObject(parent)
{
    qDBusRegisterMetaType<UsbDbusInterfaces>();
    qDBusRegisterMetaType<UsbDbusManagedObjects>();

    QDBusConnection::systemBus().connect(kUDisks2Service, kUDisks2RootPath, kObjectManagerInterface,
                                          QStringLiteral("InterfacesAdded"), this,
                                          SLOT(handleInterfacesAdded(QDBusObjectPath, UsbDbusInterfaces)));

    QDBusConnection::systemBus().connect(kUDisks2Service, kUDisks2RootPath, kObjectManagerInterface,
                                          QStringLiteral("InterfacesRemoved"), this,
                                          SLOT(handleInterfacesRemoved(QDBusObjectPath, QStringList)));

    // Seed m_knownObjectPaths from what's already mounted so a later unmount of a
    // device that was present before this monitor was constructed can still be
    // resolved back to its device id (see m_knownObjectPaths' doc comment).
    currentlyMountedDevices();
}

std::optional<UsbDeviceInfo> UsbDeviceMonitor::readMountedFilesystem(const QDBusObjectPath &objectPath,
                                                                      const UsbDbusInterfaces &interfaces) const
{
    if (!interfaces.contains(kFilesystemInterface))
        return std::nullopt;

    QByteArrayList mountPoints;
    const QVariantMap fsProps = interfaces.value(kFilesystemInterface);
    if (fsProps.contains(QStringLiteral("MountPoints"))) {
        QDBusArgument arg = fsProps.value(QStringLiteral("MountPoints")).value<QDBusArgument>();
        arg >> mountPoints;
    }
    if (mountPoints.isEmpty())
        return std::nullopt; // Filesystem interface present, but not mounted anywhere right now.

    // Each mount point is a raw byte array ending in a trailing '\0' per the UDisks2
    // wire format; fromUtf8(const char *) stops at that terminator on its own.
    const QString mountPath = QString::fromUtf8(mountPoints.first().constData());

    QDBusObjectPath drivePath;
    if (interfaces.contains(kBlockInterface))
        drivePath = interfaces.value(kBlockInterface).value(QStringLiteral("Drive")).value<QDBusObjectPath>();

    QString vendor, model, serial;
    if (!drivePath.path().isEmpty() && drivePath.path() != QStringLiteral("/")) {
        const QVariantMap drive = driveProperties(drivePath);
        vendor = drive.value(QStringLiteral("Vendor")).toString().trimmed();
        model = drive.value(QStringLiteral("Model")).toString().trimmed();
        serial = drive.value(QStringLiteral("Serial")).toString().trimmed();
    }

    UsbDeviceInfo info;
    info.mountPath = mountPath;
    info.vendor = vendor;
    info.model = model;
    info.serial = serial;
    // Serial is the stable identity UDisks2 exposes; some drives (cheap USB flash in
    // particular) report it empty, so fall back to a best-effort synthetic id from
    // vendor+model+object path rather than leaving the device unidentifiable — not
    // guaranteed unique across two identical-model drives that both have blank
    // serials, but that's a rare edge case not worth more machinery here.
    info.id = !serial.isEmpty() ? serial : (vendor + model + objectPath.path());
    return info;
}

QVariantMap UsbDeviceMonitor::driveProperties(const QDBusObjectPath &drivePath) const
{
    QDBusInterface propsInterface(kUDisks2Service, drivePath.path(), kPropertiesInterface,
                                   QDBusConnection::systemBus());
    const QDBusReply<QVariantMap> reply = propsInterface.call(QStringLiteral("GetAll"), kDriveInterface);
    return reply.isValid() ? reply.value() : QVariantMap();
}

QVariantList UsbDeviceMonitor::currentlyMountedDevices() const
{
    QVariantList result;

    QDBusInterface objectManager(kUDisks2Service, kUDisks2RootPath, kObjectManagerInterface,
                                  QDBusConnection::systemBus());
    const QDBusReply<UsbDbusManagedObjects> reply = objectManager.call(QStringLiteral("GetManagedObjects"));
    if (!reply.isValid())
        return result;

    const UsbDbusManagedObjects objects = reply.value();
    for (auto it = objects.constBegin(); it != objects.constEnd(); ++it) {
        const std::optional<UsbDeviceInfo> info = readMountedFilesystem(it.key(), it.value());
        if (!info)
            continue;
        m_knownObjectPaths.insert(it.key().path(), info->id);
        result.append(toVariantMap(*info));
    }
    return result;
}

void UsbDeviceMonitor::handleInterfacesAdded(const QDBusObjectPath &objectPath, const UsbDbusInterfaces &interfaces)
{
    const std::optional<UsbDeviceInfo> info = readMountedFilesystem(objectPath, interfaces);
    if (info) {
        m_knownObjectPaths.insert(objectPath.path(), info->id);
        emit deviceAdded(toVariantMap(*info));
        return;
    }

    // Not mounted yet — if this object at least has a Filesystem interface, an
    // auto-mount policy may still mount it moments from now (see
    // rescanForNewlyMountedDevices()'s doc comment). One retry is enough: this is a
    // one-shot race with a fast desktop-side mount call, not a slow/uncertain wait.
    if (interfaces.contains(kFilesystemInterface))
        QTimer::singleShot(1500, this, &UsbDeviceMonitor::rescanForNewlyMountedDevices);
}

void UsbDeviceMonitor::rescanForNewlyMountedDevices()
{
    QDBusInterface objectManager(kUDisks2Service, kUDisks2RootPath, kObjectManagerInterface,
                                  QDBusConnection::systemBus());
    const QDBusReply<UsbDbusManagedObjects> reply = objectManager.call(QStringLiteral("GetManagedObjects"));
    if (!reply.isValid())
        return;

    const UsbDbusManagedObjects objects = reply.value();
    for (auto it = objects.constBegin(); it != objects.constEnd(); ++it) {
        if (m_knownObjectPaths.contains(it.key().path()))
            continue; // already reported (or not a mounted filesystem last time we looked)
        const std::optional<UsbDeviceInfo> info = readMountedFilesystem(it.key(), it.value());
        if (!info)
            continue;
        m_knownObjectPaths.insert(it.key().path(), info->id);
        emit deviceAdded(toVariantMap(*info));
    }
}

void UsbDeviceMonitor::handleInterfacesRemoved(const QDBusObjectPath &objectPath, const QStringList &interfaces)
{
    if (!interfaces.contains(kFilesystemInterface))
        return;
    const QString id = m_knownObjectPaths.take(objectPath.path());
    if (!id.isEmpty())
        emit deviceRemoved(id);
}
