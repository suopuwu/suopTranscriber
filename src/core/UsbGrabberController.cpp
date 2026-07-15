#include "UsbGrabberController.h"

#include "UsbGrabJob.h"
#include "VaultTreeModel.h"

namespace {

const auto kCatchAllSerial = QStringLiteral("*");

QVariantMap ruleToVariantMap(const UsbGrabRule &rule)
{
    QVariantMap map;
    map.insert(QStringLiteral("deviceSerial"), rule.deviceSerial);
    map.insert(QStringLiteral("enabled"), rule.enabled);
    map.insert(QStringLiteral("extensionFilter"), rule.extensionFilter);
    map.insert(QStringLiteral("preserveStructure"), rule.preserveStructure);
    map.insert(QStringLiteral("startFolder"), rule.startFolder);
    map.insert(QStringLiteral("deviceLabel"), rule.deviceLabel);
    return map;
}

UsbGrabRule ruleFromVariantMap(const QVariantMap &map)
{
    UsbGrabRule rule;
    rule.deviceSerial = map.value(QStringLiteral("deviceSerial")).toString();
    rule.enabled = map.value(QStringLiteral("enabled"), false).toBool();
    rule.extensionFilter = map.value(QStringLiteral("extensionFilter")).toStringList();
    rule.preserveStructure = map.value(QStringLiteral("preserveStructure"), true).toBool();
    rule.startFolder = map.value(QStringLiteral("startFolder")).toString();
    rule.deviceLabel = map.value(QStringLiteral("deviceLabel")).toString();
    return rule;
}

} // namespace

UsbGrabberController::UsbGrabberController(QObject *parent)
    : QObject(parent)
{
    connect(&m_deviceMonitor, &UsbDeviceMonitor::deviceAdded, this, &UsbGrabberController::handleDeviceAdded);
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

QVariantList UsbGrabberController::history() const
{
    if (!m_vaultTreeModel || m_vaultTreeModel->vaultLocalPath().isEmpty())
        return {};
    return m_vaultTreeModel->readUsbGrabHistory();
}

QVariantMap UsbGrabberController::findMountedDeviceBySerial(const QString &deviceSerial) const
{
    const QVariantList devices = m_deviceMonitor.currentlyMountedDevices();
    for (const QVariant &deviceVariant : devices) {
        const QVariantMap device = deviceVariant.toMap();
        if (device.value(QStringLiteral("serial")).toString() == deviceSerial)
            return device;
    }
    return {};
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

    const QVariantMap device = findMountedDeviceBySerial(deviceSerial);
    if (device.isEmpty()) {
        setLastError(QStringLiteral("Device is not currently connected."));
        return;
    }

    const UsbGrabRule rule = resolveEffectiveRule(deviceSerial, device);
    if (!rule.enabled) {
        setLastError(QStringLiteral("No grab rule is enabled for this device."));
        return;
    }

    startGrabJob(rule, device.value(QStringLiteral("mountPath")).toString(), device.value(QStringLiteral("id")).toString());
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

    job->start(rule, mountPath, m_vaultTreeModel->vaultLocalPath(), m_vaultTreeModel);
}
