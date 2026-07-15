#pragma once

#include <QHash>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>
#include <qqml.h>

#include "UsbDeviceMonitor.h"
#include "UsbRuleStore.h"
#include "VaultTreeModel.h"

class UsbGrabJob;

// QML-facing coordinator for the USB Grabber feature (see CLAUDE.md) — the
// single entry point QML binds against, mirroring TranscriptionController's
// role for transcription (see Main.qml's `TranscriptionController { id:
// transcriptionController }` + MainPanel's `transcriptionController:
// transcriptionController` wiring: a QML_ELEMENT instantiated once in QML,
// not a singleton or a setContextProperty()). Owns one UsbDeviceMonitor
// (hotplug detection, constructed internally — nothing else in this codebase
// needs to share it) and one UsbRuleStore (persisted per-device rules; plain
// value class, cheap to own directly).
//
// Hotplug behavior: when UsbDeviceMonitor reports a newly-mounted device,
// this class checks the device's own rule and, if that isn't enabled, the
// "*" catch-all rule — if EITHER is enabled AND a vault is currently open
// (vaultTreeModel is non-null with a non-empty vaultLocalPath), a UsbGrabJob
// is started immediately against whatever vault is open at that moment. If
// no vault is open, the device is skipped entirely — no queueing, no retry,
// per the confirmed product decision that grabs only ever target whatever
// vault happens to be open at the moment of detection.
class UsbGrabberController : public QObject {
    Q_OBJECT
    QML_ELEMENT
    // The currently open vault. Settable so QML can wire it the same way
    // Main.qml/MainPanel.qml already wire VaultTreeModel into
    // TranscriptionController/AudioRecorder's collaborators (e.g. MainPanel's
    // `property VaultTreeModel vaultTreeModel` bound from Main.qml's
    // `vaultTreeModel: vaultModel`) — Step G is expected to bind this
    // property to that same VaultTreeModel instance. Null (or an instance
    // with an empty vaultLocalPath) means "no vault open": manual grabs
    // no-op with lastError set, hotplug grabs are skipped silently.
    Q_PROPERTY(VaultTreeModel *vaultTreeModel READ vaultTreeModel WRITE setVaultTreeModel NOTIFY vaultTreeModelChanged)
    // Non-empty if the most recent triggerManualGrab() call couldn't
    // proceed (no vault open, device not currently mounted, or no enabled
    // rule for it) — mirrors AudioRecorder::lastError's surfaced-not-
    // swallowed convention. Cleared at the start of every
    // triggerManualGrab() call. Hotplug-triggered grabs don't touch this
    // (there's no user action to report failure back to); they're simply
    // skipped, per the class doc comment above.
    Q_PROPERTY(QString lastError READ lastError NOTIFY lastErrorChanged)

public:
    explicit UsbGrabberController(QObject *parent = nullptr);
    ~UsbGrabberController() override;

    VaultTreeModel *vaultTreeModel() const { return m_vaultTreeModel; }
    void setVaultTreeModel(VaultTreeModel *model);

    QString lastError() const { return m_lastError; }

    // Proxies UsbDeviceMonitor::currentlyMountedDevices() — for the
    // allowlist UI to show what's plugged in right now.
    Q_INVOKABLE QVariantList mountedDevices() const;

    // Rule CRUD for QML, projecting UsbGrabRule <-> QVariantMap field-by-
    // field (mirrors how UsbDeviceInfo is projected to a QVariantMap for
    // UsbDeviceMonitor's signals/currentlyMountedDevices(), since no
    // registered Q_GADGET value type exists anywhere in this codebase for
    // this kind of small struct).
    //
    // ruleForDevice() always returns a populated map (deviceSerial set, rest
    // default-disabled) even for a never-configured device, matching
    // UsbRuleStore::ruleForSerial()'s own always-returns-something contract.
    Q_INVOKABLE QVariantMap ruleForDevice(const QString &deviceSerial) const;
    // rule must contain at least "deviceSerial"; missing keys fall back to
    // UsbGrabRule's own default-constructed field values.
    Q_INVOKABLE void saveRule(const QVariantMap &rule);
    Q_INVOKABLE void removeRule(const QString &deviceSerial);
    // Every persisted rule (including the "*" catch-all if one was ever
    // saved), for listing every configured device in the UI.
    Q_INVOKABLE QVariantList allRules() const;

    // Looks up deviceSerial's rule (falling back to the "*" catch-all rule
    // if the device-specific one isn't enabled) and the device's current
    // mountPath (matched by "serial" against
    // UsbDeviceMonitor::currentlyMountedDevices()), then starts a
    // UsbGrabJob against the currently-open vault. No-op (with lastError
    // set) if: no vault is open, the device isn't currently mounted, or
    // neither the device-specific nor the catch-all rule is enabled.
    Q_INVOKABLE void triggerManualGrab(const QString &deviceSerial);

    // Proxies vaultTreeModel->readUsbGrabHistory(), or an empty list if no
    // vault is open.
    Q_INVOKABLE QVariantList history() const;

signals:
    void vaultTreeModelChanged();
    void lastErrorChanged();
    // Emitted once a grab (manual or hotplug-triggered) finishes, after its
    // history entry has already been appended (UsbGrabJob::finished's own
    // ordering guarantee). QML may surface this as a toast/notification —
    // that's Step G's decision, not this one.
    void grabFinished(const QString &deviceSerial, int filesCopied, int filesSkipped, const QStringList &errors);

private slots:
    // Bound to m_deviceMonitor's deviceAdded signal at construction — see
    // the class doc comment for the enable-check/vault-open-check logic.
    void handleDeviceAdded(const QVariantMap &info);

private:
    // Shared by triggerManualGrab() and handleDeviceAdded(): the device-
    // specific rule if enabled, else the "*" catch-all rule (with
    // deviceSerial/deviceLabel overridden to this device's own identity, so
    // a catch-all-triggered grab's history entry records which real device
    // ran rather than the literal "*" placeholder) if THAT is enabled.
    // Returns the device-specific (disabled) rule as-is if neither is
    // enabled — callers must check the returned rule's `enabled` field
    // before using it for anything.
    UsbGrabRule resolveEffectiveRule(const QString &deviceSerial, const QVariantMap &deviceInfo) const;

    // Finds deviceSerial's entry in currentlyMountedDevices() (matched
    // against each entry's "serial" key) — empty map if not currently
    // mounted.
    QVariantMap findMountedDeviceBySerial(const QString &deviceSerial) const;

    // Constructs and starts a UsbGrabJob against the currently-open vault,
    // keyed into m_inFlightJobs by deviceId so it isn't garbage-collected
    // mid-run. deviceRemoved is deliberately never connected to anything
    // here: an unplug mid-grab just accumulates copy errors in the job's
    // own per-file error handling and finishes naturally (per the confirmed
    // product decision — no early-abort-on-unmount logic).
    void startGrabJob(const UsbGrabRule &rule, const QString &mountPath, const QString &deviceId);

    void setLastError(const QString &error);

    UsbDeviceMonitor m_deviceMonitor;
    UsbRuleStore m_ruleStore;
    VaultTreeModel *m_vaultTreeModel = nullptr;
    QString m_lastError;

    // In-flight grab jobs keyed by device id (see startGrabJob()'s doc
    // comment). Entries are removed (and the job deleteLater()'d) once its
    // finished() signal fires.
    QHash<QString, UsbGrabJob *> m_inFlightJobs;
};
