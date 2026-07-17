#include "UsbGrabberController.h"

#include "RegexValidation.h"
#include "UsbGrabJob.h"
#include "VaultTreeModel.h"

#include <QFileInfo>

#include <memory>

namespace {

const auto kCatchAllSerial = QStringLiteral("*");

QVariantMap ruleToVariantMap(const UsbGrabRule &rule)
{
    QVariantMap map;
    map.insert(QStringLiteral("deviceSerial"), rule.deviceSerial);
    map.insert(QStringLiteral("enabled"), rule.enabled);
    map.insert(QStringLiteral("extensionFilter"), rule.extensionFilter);
    map.insert(QStringLiteral("filenameRegex"), rule.filenameRegex);
    map.insert(QStringLiteral("preserveStructure"), rule.preserveStructure);
    map.insert(QStringLiteral("startFolder"), rule.startFolder);
    map.insert(QStringLiteral("deviceLabel"), rule.deviceLabel);
    map.insert(QStringLiteral("destinationFolder"), rule.destinationFolder);
    return map;
}

UsbGrabRule ruleFromVariantMap(const QVariantMap &map)
{
    UsbGrabRule rule;
    rule.deviceSerial = map.value(QStringLiteral("deviceSerial")).toString();
    rule.enabled = map.value(QStringLiteral("enabled"), false).toBool();
    rule.extensionFilter = map.value(QStringLiteral("extensionFilter")).toStringList();
    rule.filenameRegex = map.value(QStringLiteral("filenameRegex")).toString();
    rule.preserveStructure = map.value(QStringLiteral("preserveStructure"), true).toBool();
    rule.startFolder = map.value(QStringLiteral("startFolder")).toString();
    rule.deviceLabel = map.value(QStringLiteral("deviceLabel")).toString();
    rule.destinationFolder = map.value(QStringLiteral("destinationFolder")).toString();
    return rule;
}

} // namespace

UsbGrabberController::UsbGrabberController(QObject *parent)
    : QObject(parent)
{
    connect(&m_deviceMonitor, &UsbDeviceMonitor::deviceAdded, this, &UsbGrabberController::handleDeviceAdded);
    connect(&m_deviceMonitor, &UsbDeviceMonitor::unmountedVolumeDetected, this,
            &UsbGrabberController::handleUnmountedVolumeDetected);
    connect(&m_deviceMonitor, &UsbDeviceMonitor::mountFailed, this,
            [](const QString &id, const QString &error) {
                // Nowhere to surface this: lastError belongs to triggerManualGrab()
                // (see its doc comment), and a hotplug has no user action to report
                // back to — same reason a hotplug grab that can't run is skipped
                // silently rather than setting it.
                qWarning("UsbGrabberController: could not mount %s: %s", qUtf8Printable(id),
                          qUtf8Printable(error));
            });
}

UsbGrabberController::~UsbGrabberController() = default;

void UsbGrabberController::setVaultTreeModel(VaultTreeModel *model)
{
    if (model == m_vaultTreeModel)
        return;
    m_vaultTreeModel = model;
    emit vaultTreeModelChanged();
}

void UsbGrabberController::setLastError(const QString &error)
{
    if (error == m_lastError)
        return;
    m_lastError = error;
    emit lastErrorChanged();
}

QVariantList UsbGrabberController::mountedDevices() const
{
    return m_deviceMonitor.currentlyMountedDevices();
}

QVariantMap UsbGrabberController::ruleForDevice(const QString &deviceSerial) const
{
    return ruleToVariantMap(m_ruleStore.ruleForSerial(deviceSerial));
}

void UsbGrabberController::saveRule(const QVariantMap &rule)
{
    m_ruleStore.saveRule(ruleFromVariantMap(rule));
}

void UsbGrabberController::removeRule(const QString &deviceSerial)
{
    m_ruleStore.removeRule(deviceSerial);
}

QVariantList UsbGrabberController::allRules() const
{
    QVariantList list;
    const QList<UsbGrabRule> rules = m_ruleStore.loadAllRules();
    list.reserve(rules.size());
    for (const UsbGrabRule &rule : rules)
        list.append(ruleToVariantMap(rule));
    return list;
}

QString UsbGrabberController::regexValidationError(const QString &pattern) const
{
    return regexValidationErrorFor(pattern);
}

QVariantList UsbGrabberController::history() const
{
    if (!m_vaultTreeModel || m_vaultTreeModel->vaultLocalPath().isEmpty())
        return {};
    return m_vaultTreeModel->readUsbGrabHistory();
}

QVariantList usbVolumesForSerial(const QVariantList &mountedVolumes, const QString &deviceSerial)
{
    QVariantList volumes;
    for (const QVariant &volumeVariant : mountedVolumes) {
        const QVariantMap volume = volumeVariant.toMap();
        if (volume.value(QStringLiteral("serial")).toString() == deviceSerial)
            volumes.append(volume);
    }
    return volumes;
}

QVariantList UsbGrabberController::findMountedVolumesBySerial(const QString &deviceSerial) const
{
    return usbVolumesForSerial(m_deviceMonitor.currentlyMountedDevices(), deviceSerial);
}

UsbGrabRule UsbGrabberController::resolveEffectiveRule(const QString &deviceSerial, const QVariantMap &deviceInfo) const
{
    const UsbGrabRule deviceRule = m_ruleStore.ruleForSerial(deviceSerial);
    if (deviceRule.enabled)
        return deviceRule;

    const UsbGrabRule catchAllRule = m_ruleStore.ruleForSerial(kCatchAllSerial);
    if (!catchAllRule.enabled)
        return deviceRule;

    // The catch-all rule's own deviceSerial/deviceLabel are the literal "*"
    // placeholder and whatever generic label (if any) was set when
    // configuring it — override both with this specific device's real
    // identity so the resulting history entry (see UsbGrabJob::
    // handleWorkerFinished()) records which device actually ran, not "*".
    UsbGrabRule effective = catchAllRule;
    effective.deviceSerial = deviceSerial;
    const QString vendor = deviceInfo.value(QStringLiteral("vendor")).toString();
    const QString model = deviceInfo.value(QStringLiteral("model")).toString();
    const QString liveLabel = (vendor + QLatin1Char(' ') + model).trimmed();
    if (!liveLabel.isEmpty())
        effective.deviceLabel = liveLabel;
    return effective;
}

void UsbGrabberController::triggerManualGrab(const QString &deviceSerial)
{
    setLastError(QString());

    if (!m_vaultTreeModel || m_vaultTreeModel->vaultLocalPath().isEmpty()) {
        setLastError(QStringLiteral("No vault is open."));
        return;
    }

    const QVariantList volumes = findMountedVolumesBySerial(deviceSerial);
    if (volumes.isEmpty()) {
        setLastError(QStringLiteral("Device is not currently connected."));
        return;
    }

    const UsbGrabRule rule = resolveEffectiveRule(deviceSerial, volumes.first().toMap());
    if (!rule.enabled) {
        setLastError(QStringLiteral("No grab rule is enabled for this device."));
        return;
    }

    // One job per mounted volume, all under the device's single rule. A multi-LUN
    // recorder (internal memory + SD card as two block devices) would otherwise have
    // only its first volume grabbed and the rest silently never imported, since one
    // rule's worth of user intent covers the whole physical device.
    for (const QVariant &volumeVariant : volumes) {
        const QVariantMap volume = volumeVariant.toMap();
        startGrabJob(rule, volume.value(QStringLiteral("mountPath")).toString(),
                      volume.value(QStringLiteral("id")).toString());
    }
}

void UsbGrabberController::handleDeviceAdded(const QVariantMap &info)
{
    if (!m_vaultTreeModel || m_vaultTreeModel->vaultLocalPath().isEmpty())
        return;

    const QString deviceSerial = info.value(QStringLiteral("serial")).toString();
    const UsbGrabRule rule = resolveEffectiveRule(deviceSerial, info);
    if (!rule.enabled)
        return;

    startGrabJob(rule, info.value(QStringLiteral("mountPath")).toString(), info.value(QStringLiteral("id")).toString());
}

void UsbGrabberController::handleUnmountedVolumeDetected(const QVariantMap &info)
{
    // THE mount gate — an enabled rule, exactly the predicate handleDeviceAdded()
    // above grabs on. See the header for why this and not a setting of its own.
    const QString deviceSerial = info.value(QStringLiteral("serial")).toString();
    if (!resolveEffectiveRule(deviceSerial, info).enabled)
        return;

    // Deliberately NOT also gated on a vault being open, unlike handleDeviceAdded():
    // an unmounted volume can't be grabbed OR configured OR previewed, so a mount is
    // worth doing for a device the user has already vouched for whether or not a
    // vault happens to be open this second — the mount just makes it addressable, and
    // the grab that follows re-checks the vault on its own.
    m_deviceMonitor.mountVolume(info.value(QStringLiteral("id")).toString());
}

QString UsbGrabberController::resolveDestinationPath(const UsbGrabRule &rule) const
{
    QString path = m_vaultTreeModel->vaultLocalPath();
    if (!rule.destinationFolder.isEmpty())
        path += QLatin1Char('/') + rule.destinationFolder;
    return path;
}

void UsbGrabberController::startGrabJob(const UsbGrabRule &rule, const QString &mountPath, const QString &deviceId)
{
    auto *job = new UsbGrabJob(this);
    m_inFlightJobs.insert(deviceId, job);

    const QString deviceSerial = rule.deviceSerial;
    connect(job, &UsbGrabJob::finished, this,
            [this, job, deviceId, deviceSerial](int filesCopied, int filesSkipped, const QStringList &errors) {
                m_inFlightJobs.remove(deviceId);
                job->deleteLater();
                emit grabFinished(deviceSerial, filesCopied, filesSkipped, errors);
            });

    job->start(rule, mountPath, resolveDestinationPath(rule), m_vaultTreeModel);
}

void UsbGrabberController::requestGrabPreview(const QString &deviceSerial)
{
    setLastError(QString());

    if (!m_vaultTreeModel || m_vaultTreeModel->vaultLocalPath().isEmpty()) {
        setLastError(QStringLiteral("No vault is open."));
        emit grabPreviewReady(deviceSerial, QVariantList(), m_lastError);
        return;
    }

    const QVariantList volumes = findMountedVolumesBySerial(deviceSerial);
    if (volumes.isEmpty()) {
        setLastError(QStringLiteral("Device is not currently connected."));
        emit grabPreviewReady(deviceSerial, QVariantList(), m_lastError);
        return;
    }

    // Deliberately does NOT gate on rule.enabled the way triggerManualGrab()
    // does — a preview should show what a grab WOULD do even for a rule the
    // user hasn't switched on yet (e.g. checking a filter/destination before
    // committing to it). resolveEffectiveRule() still applies the same
    // device-then-catch-all resolution a real grab would use.
    const UsbGrabRule rule = resolveEffectiveRule(deviceSerial, volumes.first().toMap());

    // A preview must cover exactly what triggerManualGrab() would do, so it
    // previews EVERY mounted volume and emits grabPreviewReady ONCE with the
    // combined result. Emitting per-volume instead would be actively misleading:
    // QML replaces its entry list wholesale on each signal, so only the last
    // volume's files would ever be shown.
    //
    // The jobs are independent and finish in any order, so the shared accumulators
    // below are heap state owned jointly by the per-job lambdas rather than members
    // — two previews of different devices can be in flight at once and must not
    // collide. No locking needed: every lambda runs on this (the GUI) thread.
    auto remaining = std::make_shared<int>(volumes.size());
    auto combined = std::make_shared<QVariantList>();
    auto firstError = std::make_shared<QString>();
    const bool multiVolume = volumes.size() > 1;

    for (const QVariant &volumeVariant : volumes) {
        const QVariantMap volume = volumeVariant.toMap();
        const QString mountPath = volume.value(QStringLiteral("mountPath")).toString();
        // Preview paths are relative to their own volume's scan root, so on a
        // multi-volume device the same filename from two volumes would render as two
        // identical, unattributable rows. Prefix with the mount point's own folder
        // name to keep them distinguishable — only when it's actually ambiguous, so
        // the ordinary single-volume preview stays unprefixed.
        const QString volumeName = QFileInfo(mountPath).fileName();

        auto *job = new UsbGrabJob(this);
        connect(job, &UsbGrabJob::previewReady, this,
                [this, job, deviceSerial, volumeName, multiVolume, remaining, combined, firstError](
                    const QVariantList &entries, const QString &error) {
                    job->deleteLater();
                    if (!error.isEmpty() && firstError->isEmpty())
                        *firstError = error;
                    for (const QVariant &entryVariant : entries) {
                        QVariantMap entry = entryVariant.toMap();
                        if (multiVolume) {
                            entry.insert(QStringLiteral("path"),
                                          volumeName + QLatin1Char('/')
                                              + entry.value(QStringLiteral("path")).toString());
                        }
                        combined->append(entry);
                    }
                    if (--(*remaining) > 0)
                        return;
                    if (!firstError->isEmpty())
                        setLastError(*firstError);
                    emit grabPreviewReady(deviceSerial, *combined, *firstError);
                });
        job->startPreview(rule, mountPath, resolveDestinationPath(rule));
    }
}
