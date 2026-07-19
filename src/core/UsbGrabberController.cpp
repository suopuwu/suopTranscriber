#include "UsbGrabberController.h"

#include "RegexValidation.h"
#include "UsbGrabJob.h"
#include "VaultTreeModel.h"

#include <QDateTime>
#include <QFileInfo>
#include <QUrl>

#include <memory>

namespace {

const auto kCatchAllSerial = QStringLiteral("*");

QVariantMap ruleToVariantMap(const UsbGrabRule &rule)
{
    QVariantMap map;
    map.insert(QStringLiteral("deviceSerial"), rule.deviceSerial);
    map.insert(QStringLiteral("enabled"), rule.enabled);
    map.insert(QStringLiteral("autoGrab"), rule.autoGrab);
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
    rule.autoGrab = map.value(QStringLiteral("autoGrab"), false).toBool();
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
    if (m_vaultTreeModel)
        disconnect(m_vaultTreeModel, nullptr, this, nullptr);
    m_vaultTreeModel = model;
    if (m_vaultTreeModel) {
        // When a vault opens (path becomes non-empty), re-scan already-mounted
        // devices for auto-grabs — the app-was-closed-while-plugged-in gap (see
        // rescanForAutoGrab()). Queued via a zero-timer so the grab lands after
        // the vault model has fully settled its path, not mid-signal.
        connect(m_vaultTreeModel, &VaultTreeModel::vaultPathChanged, this, [this] {
            if (m_vaultTreeModel && !m_vaultTreeModel->vaultLocalPath().isEmpty())
                QMetaObject::invokeMethod(this, &UsbGrabberController::rescanForAutoGrab, Qt::QueuedConnection);
        });
        // The vault may ALREADY be open by the time this controller is wired up
        // (lastVaultPath restored at startup before this binding lands), so
        // vaultPathChanged won't fire again — rescan once now too.
        if (!m_vaultTreeModel->vaultLocalPath().isEmpty())
            QMetaObject::invokeMethod(this, &UsbGrabberController::rescanForAutoGrab, Qt::QueuedConnection);
    }
    emit vaultTreeModelChanged();
}

void UsbGrabberController::setTranscriptionController(TranscriptionController *controller)
{
    if (controller == m_transcriptionController)
        return;
    if (m_transcriptionController)
        disconnect(m_transcriptionController, nullptr, this, nullptr);
    m_transcriptionController = controller;
    if (m_transcriptionController) {
        connect(m_transcriptionController, &TranscriptionController::usbGrabFinished, this,
                &UsbGrabberController::handleUsbGrabFinished);
        connect(m_transcriptionController, &TranscriptionController::queueChanged, this,
                &UsbGrabberController::reconcileInFlightJobs);
        // Startup wiring order isn't guaranteed: if the vault opened before
        // THIS controller was set, setVaultTreeModel()'s rescan no-op'd
        // (startGrabJob needs the transcription controller) and won't retry —
        // so rescan once here too. Idempotent + queued, so a double-fire with
        // setVaultTreeModel()'s own rescan is harmless.
        if (m_vaultTreeModel && !m_vaultTreeModel->vaultLocalPath().isEmpty())
            QMetaObject::invokeMethod(this, &UsbGrabberController::rescanForAutoGrab, Qt::QueuedConnection);
    }
    emit transcriptionControllerChanged();
}

void UsbGrabberController::setCombineRules(const QVariantList &rules)
{
    if (rules == m_combineRules)
        return;
    m_combineRules = rules;
    emit combineRulesChanged();
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
    // identity so the resulting history entry (see handleUsbGrabFinished())
    // records which device actually ran, not "*".
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

    if (!m_transcriptionController) {
        setLastError(QStringLiteral("The task queue is unavailable."));
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
        startGrabJob(rule, volume.value(QStringLiteral("mountPath")).toString(), /*autoTriggered=*/false);
    }
}

void UsbGrabberController::handleDeviceAdded(const QVariantMap &info)
{
    maybeAutoGrab(info);
}

void UsbGrabberController::maybeAutoGrab(const QVariantMap &info)
{
    if (!m_vaultTreeModel || m_vaultTreeModel->vaultLocalPath().isEmpty())
        return;

    const QString deviceSerial = info.value(QStringLiteral("serial")).toString();
    const UsbGrabRule rule = resolveEffectiveRule(deviceSerial, info);
    // Both gates, in order — see the class doc comment. enabled says the device
    // is grabbable at all; autoGrab says it may be grabbed with no click.
    // An enabled-but-not-autoGrab device still mounts (that's enabled's job)
    // and shows up to be grabbed on demand; it just isn't touched here.
    if (!rule.enabled || !rule.autoGrab)
        return;

    const QString mountPath = info.value(QStringLiteral("mountPath")).toString();
    // A device present-but-unmounted has no path to walk yet — its own mount
    // (handleUnmountedVolumeDetected) will re-surface it as a mounted device
    // and this runs again. Skip rather than start a grab against an empty path.
    if (mountPath.isEmpty())
        return;

    startGrabJob(rule, mountPath, /*autoTriggered=*/true);
}

void UsbGrabberController::rescanForAutoGrab()
{
    // See the header doc comment: re-evaluate every already-mounted device so
    // one that stayed plugged in across an app restart still auto-grabs, even
    // though no hotplug fires for it. Idempotent (the grab skips already-
    // copied files).
    const QVariantList mounted = m_deviceMonitor.currentlyMountedDevices();
    for (const QVariant &v : mounted)
        maybeAutoGrab(v.toMap());
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

void UsbGrabberController::startGrabJob(const UsbGrabRule &rule, const QString &mountPath, bool autoTriggered)
{
    if (!m_transcriptionController)
        return;

    const QString destinationPath = resolveDestinationPath(rule);
    const int jobId = m_transcriptionController->enqueueUsbGrab(
        rule, mountPath, QUrl::fromLocalFile(destinationPath),
        QUrl::fromLocalFile(m_vaultTreeModel->vaultLocalPath()), autoTriggered, m_combineRules);

    m_inFlightJobs.insert(jobId, InFlightGrab { rule.deviceSerial, rule.deviceLabel, autoTriggered });
    if (autoTriggered)
        m_pendingAutoGrabJobIds.insert(jobId);
}

void UsbGrabberController::handleUsbGrabFinished(int jobId, int filesCopied, int filesSkipped,
                                                  const QStringList &errors, bool cancelled)
{
    // Not ours: this controller and the queue are both app-wide, and the queue
    // has no reason to believe every usbGrab job came from here.
    if (!m_inFlightJobs.contains(jobId))
        return;
    const InFlightGrab grab = m_inFlightJobs.take(jobId);

    // Retire from the auto-grab barrier BEFORE the early-return below, so a
    // cancelled auto-grab can't strand the batch and block the auto-combine
    // forever. "All auto-grabs have run" means none are left, not that they all
    // succeeded — see autoGrabsSettled()'s doc comment.
    const bool wasPendingAuto = m_pendingAutoGrabJobIds.remove(jobId);
    const bool settled = wasPendingAuto && m_pendingAutoGrabJobIds.isEmpty();

    // A cancelled grab's counts are a partial tally — recording it in history
    // or announcing it would misreport a run the user stopped as a completed
    // one. The Tasks row already shows it as cancelled, which is the honest
    // account of what happened.
    if (!cancelled) {
        QStringList combinedErrors = errors;
        if (m_vaultTreeModel) {
            QVariantMap entry;
            entry[QStringLiteral("timestamp")] = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
            entry[QStringLiteral("deviceSerial")] = grab.deviceSerial;
            entry[QStringLiteral("deviceLabel")] = grab.deviceLabel;
            entry[QStringLiteral("filesCopied")] = filesCopied;
            entry[QStringLiteral("filesSkipped")] = filesSkipped;
            entry[QStringLiteral("errors")] = errors;

            QString appendError;
            if (!m_vaultTreeModel->appendUsbGrabHistoryEntry(entry, &appendError) && !appendError.isEmpty())
                combinedErrors.append(QStringLiteral("Failed to record grab history: %1").arg(appendError));
        }
        // After the history append, so a QML handler that re-reads history()
        // on this signal already sees this run's entry.
        emit grabFinished(grab.deviceSerial, filesCopied, filesSkipped, combinedErrors, grab.autoTriggered);
    }

    // Last, so the auto-combine it kicks off is enqueued behind nothing and
    // every grabFinished for the batch has already been delivered.
    if (settled)
        emit autoGrabsSettled();
}

void UsbGrabberController::reconcileInFlightJobs()
{
    if (m_inFlightJobs.isEmpty() || !m_transcriptionController)
        return;

    QSet<int> liveIds;
    const QVariantList jobs = m_transcriptionController->jobs();
    liveIds.reserve(jobs.size());
    for (const QVariant &jobVariant : jobs)
        liveIds.insert(jobVariant.toMap().value(QStringLiteral("id")).toInt());

    bool settled = false;
    for (auto it = m_inFlightJobs.begin(); it != m_inFlightJobs.end();) {
        if (liveIds.contains(it.key())) {
            ++it;
            continue;
        }
        // Vanished from the queue without ever reporting a result — which means
        // it was cancelled while still QUEUED: cancelJob()/cancelAll() simply
        // drop such a job, so the worker never runs it and usbGrabFinished
        // never fires for it. Without this, its id would sit in
        // m_pendingAutoGrabJobIds forever and the barrier below would never
        // empty again, silently disabling the auto-combiner for the rest of the
        // session. Retired exactly like a cancelled run: no history entry, no
        // announcement.
        //
        // Reconciling against the queue (rather than teaching every removal
        // path to notify us) is deliberate: it's correct for cancel-while-
        // queued, cancelAll, and any future removal path, none of which can
        // forget to call it. A completed job is already out of m_inFlightJobs
        // by the time its queueChanged lands, so this never double-retires one.
        if (m_pendingAutoGrabJobIds.remove(it.key()) && m_pendingAutoGrabJobIds.isEmpty())
            settled = true;
        it = m_inFlightJobs.erase(it);
    }

    if (settled)
        emit autoGrabsSettled();
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

        auto *job = new UsbGrabPreviewJob(this);
        connect(job, &UsbGrabPreviewJob::previewReady, this,
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
        job->start(rule, mountPath, resolveDestinationPath(rule), m_combineRules);
    }
}
