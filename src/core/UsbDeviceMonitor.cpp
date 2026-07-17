#include "UsbDeviceMonitor.h"

#include <QByteArrayList>
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusError>
#include <QDBusInterface>
#include <QDBusMetaType>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
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
// What UDisks2 answers a Mount() for a volume that's already mounted — the desktop's
// automounter having won the race, not a failure. See mountVolume().
const QString kAlreadyMountedError = QStringLiteral("org.freedesktop.UDisks2.Error.AlreadyMounted");

} // namespace

bool usbDeviceIsExternalMedia(const QVariantMap &blockProps, const QVariantMap &driveProps)
{
    // Default true: a device whose HintSystem we couldn't read is treated as the
    // machine's own hardware and refused. See the header for why this can't be the
    // only check.
    if (blockProps.value(QStringLiteral("HintSystem"), true).toBool())
        return false;

    if (driveProps.value(QStringLiteral("Removable"), false).toBool())
        return true;

    // A drive can be non-Removable yet still genuinely external (some USB SSDs and
    // enclosures report Removable=false, since the MEDIA isn't removable from the
    // drive even though the drive is removable from the machine), so the connection
    // bus gets its own say. "sdio" covers internal card readers, which is worth
    // honouring here because field recorders routinely present their storage that
    // way; both are still gated behind HintSystem above.
    const QString connectionBus = driveProps.value(QStringLiteral("ConnectionBus")).toString();
    return connectionBus.compare(QStringLiteral("usb"), Qt::CaseInsensitive) == 0
        || connectionBus.compare(QStringLiteral("sdio"), Qt::CaseInsensitive) == 0;
}

std::optional<UsbDeviceInfo> usbVolumeFromProperties(const QString &objectPath, const QString &mountPath,
                                                      const QVariantMap &blockProps,
                                                      const QVariantMap &driveProps)
{
    if (!usbDeviceIsExternalMedia(blockProps, driveProps))
        return std::nullopt;

    UsbDeviceInfo info;
    info.mountPath = mountPath;
    info.vendor = driveProps.value(QStringLiteral("Vendor")).toString().trimmed();
    info.model = driveProps.value(QStringLiteral("Model")).toString().trimmed();
    // Per PHYSICAL DEVICE, so volumes of a multi-LUN device share it (they share one
    // Drive object) and it may be empty. It is the rule key, never a runtime key.
    info.serial = driveProps.value(QStringLiteral("Serial")).toString().trimmed();
    // Per VOLUME, and unique by construction: UDisks2 gives every block device its
    // own object path. Deliberately NOT derived from the serial — see UsbDeviceInfo's
    // doc comment for what that conflation broke.
    info.id = objectPath;
    return info;
}

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

std::optional<UsbDeviceInfo> UsbDeviceMonitor::readFilesystemVolume(const QDBusObjectPath &objectPath,
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

    // Empty MountPoints is NOT a rejection — it's the unmounted case, which callers
    // distinguish by the empty mountPath (see the header). Each mount point is a raw
    // byte array ending in a trailing '\0' per the UDisks2 wire format;
    // fromUtf8(const char *) stops at that terminator on its own.
    const QString mountPath =
        mountPoints.isEmpty() ? QString() : QString::fromUtf8(mountPoints.first().constData());

    // Every UDisks2 Filesystem sits on a Block object, and only the Block object
    // names the backing Drive whose Removable/ConnectionBus decide whether this is
    // external media. So a mount with no reachable Block/Drive can't be shown to be
    // external and is refused rather than assumed safe — that's what rules out
    // things like zram0, whose Block.Drive is the placeholder "/".
    const QVariantMap blockProps = interfaces.value(kBlockInterface);
    const QDBusObjectPath drivePath = blockProps.value(QStringLiteral("Drive")).value<QDBusObjectPath>();
    if (drivePath.path().isEmpty() || drivePath.path() == QStringLiteral("/"))
        return std::nullopt;

    // Everything that DECIDES anything lives in usbVolumeFromProperties(); this
    // function's remaining job is only to obtain the inputs it can't (the decoded
    // mount point above, and the Drive object's properties over D-Bus below).
    return usbVolumeFromProperties(objectPath.path(), mountPath, blockProps, driveProperties(drivePath));
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
        const std::optional<UsbDeviceInfo> info = readFilesystemVolume(it.key(), it.value());
        // Mounted volumes only: this list's contract is "what can be grabbed right
        // now", and m_knownObjectPaths tracks exactly what's been reported as such.
        if (!info || info->mountPath.isEmpty())
            continue;
        m_knownObjectPaths.insert(it.key().path());
        result.append(toVariantMap(*info));
    }
    return result;
}

void UsbDeviceMonitor::handleInterfacesAdded(const QDBusObjectPath &objectPath, const UsbDbusInterfaces &interfaces)
{
    const std::optional<UsbDeviceInfo> info = readFilesystemVolume(objectPath, interfaces);
    if (info && !info->mountPath.isEmpty()) {
        m_knownObjectPaths.insert(objectPath.path());
        emit deviceAdded(toVariantMap(*info));
        return;
    }

    // External media that nothing has mounted — offer it to whoever knows whether it's
    // worth mounting (UsbGrabberController, which holds the rules). Note this class
    // does NOT call mountVolume() itself off the back of its own signal: "may be
    // mounted" and "should be mounted" are different questions, and this class only
    // answers the first.
    if (info)
        emit unmountedVolumeDetected(toVariantMap(*info));

    // Either way, an auto-mount policy may still mount it moments from now (see
    // rescanForNewlyMountedDevices()'s doc comment) — including on a device with no
    // rule, which the line above just declined to mount but which must still show up
    // in the settings UI's device list once the desktop mounts it. One retry is
    // enough: this is a one-shot race with a fast desktop-side mount call, not a
    // slow/uncertain wait.
    if (interfaces.contains(kFilesystemInterface))
        QTimer::singleShot(1500, this, &UsbDeviceMonitor::rescanForNewlyMountedDevices);
}

void UsbDeviceMonitor::mountVolume(const QString &volumeId)
{
    QDBusInterface filesystem(kUDisks2Service, volumeId, kFilesystemInterface, QDBusConnection::systemBus());

    QVariantMap options;
    options.insert(QStringLiteral("auth.no_user_interaction"), true);

    auto *watcher = new QDBusPendingCallWatcher(
        filesystem.asyncCall(QStringLiteral("Mount"), QVariant::fromValue(options)), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, volumeId](QDBusPendingCallWatcher *call) {
        call->deleteLater();

        const QDBusPendingReply<QString> reply = *call;
        if (reply.isError() && reply.error().name() != kAlreadyMountedError) {
            emit mountFailed(volumeId, reply.error().message());
            return;
        }

        // Success, or someone else mounted it first — either way there is now a mount
        // to report, and the reply's own mount path is discarded in favour of the one
        // shared reporting path (see mountVolume()'s doc comment). AlreadyMounted also
        // covers the case where the desktop's automounter won: the rescan's
        // m_knownObjectPaths check means the volume is still only reported once.
        rescanForNewlyMountedDevices();
    });
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
        const std::optional<UsbDeviceInfo> info = readFilesystemVolume(it.key(), it.value());
        if (!info || info->mountPath.isEmpty())
            continue; // still unmounted — this rescan only ever reports NEWLY MOUNTED volumes
        m_knownObjectPaths.insert(it.key().path());
        emit deviceAdded(toVariantMap(*info));
    }
}

void UsbDeviceMonitor::handleInterfacesRemoved(const QDBusObjectPath &objectPath, const QStringList &interfaces)
{
    if (!interfaces.contains(kFilesystemInterface))
        return;
    // Reports exactly the volume that went away, never its siblings: the id emitted
    // is this object path alone, so unmounting one LUN of a multi-LUN device leaves
    // the others' ids untouched. remove() returning false means this path was never
    // one we reported (an internal drive, or a volume that never mounted) — stay
    // silent rather than announce the removal of something the consumer never saw.
    if (m_knownObjectPaths.remove(objectPath.path()))
        emit deviceRemoved(objectPath.path());
}
