#pragma once

#include <QHash>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>
#include <qqml.h>

#include "TranscriptionController.h"
#include "UsbDeviceMonitor.h"
#include "UsbRuleStore.h"
#include "VaultTreeModel.h"

// Every entry of mountedVolumes (the per-volume shape
// UsbDeviceMonitor::currentlyMountedDevices() returns) whose "serial" is
// deviceSerial — ALL of them, since one physical device can present several
// mounted volumes under a single serial (see UsbDeviceInfo's doc comment).
//
// Free function over a plain list so tst_usbgrabbercontroller can pin the
// returns-every-volume contract without a live UDisks2 session or multi-LUN
// hardware: the controller owns its UsbDeviceMonitor internally, so there's no
// seam to inject fake devices through. Matching only the first volume is exactly
// the bug this replaced — it left a two-volume recorder's second volume
// permanently un-grabbed.
QVariantList usbVolumesForSerial(const QVariantList &mountedVolumes, const QString &deviceSerial);

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
// A grab is a QUEUED TASK, not this class's own thread. startGrabJob() hands
// the resolved rule to TranscriptionController::enqueueUsbGrab(), so a grab
// takes its turn in the one task queue alongside transcribes/imports/combines
// and inherits that queue's Tasks row, progress glow, and pause/cancel with no
// bespoke plumbing here. This class keeps only what the queue can't know: which
// rule applies, which vault, and what to do with the result (history entry,
// snackbar, auto-combine). The dry-run PREVIEW is the one thing still on its
// own thread (UsbGrabPreviewJob) — it's an interactive answer while editing a
// rule and must not queue behind a running transcription.
//
// Hotplug behavior, two independent gates, in order:
//   1. resolveEffectiveRule(...).enabled — is this device grabbable at all?
//      Also the mount gate (see below).
//   2. rule.autoGrab — may a hotplug fire the grab BY ITSELF, with no click?
//      Default false, so an enabled-but-not-autoGrab device mounts, appears in
//      the UI, and waits for "Grab now". Auto-grab is the surprising behavior,
//      so it's opted into per device (see UsbGrabRule::autoGrab).
// Both must pass, AND a vault must be open (vaultTreeModel non-null with a
// non-empty vaultLocalPath), for a hotplug to enqueue anything. If no vault is
// open the device is skipped entirely — no queueing, no retry, per the
// confirmed product decision that grabs only ever target whatever vault happens
// to be open at the moment of detection.
//
// Auto-combine handoff: this class tracks the ids of the auto-grab jobs it has
// enqueued and emits autoGrabsSettled() once the LAST of them leaves the queue
// — the "only after all auto grabbings have run" barrier. It deliberately
// stops there and does not itself combine: the combine rules live in
// vault-scoped AppSettings and the vault's audio listing in the tree model,
// both of which are QML's to hand over (see Main.qml's onAutoGrabsSettled).
//
// Mount policy: a stock KDE install does not auto-mount removable media (its
// device_automounter kded module is off by default), so a device can be
// plugged in and never mount, which means never fire deviceAdded, which means
// never grab. This class therefore mounts unmounted external volumes itself,
// via UsbDeviceMonitor::mountVolume() on its unmountedVolumeDetected signal —
// gated on EXACTLY ONE THING: the same resolveEffectiveRule().enabled check
// the grab itself uses. So the app mounts precisely what it would grab from
// and nothing else, and there's no separate "auto-mount" setting to configure,
// explain, or leave inconsistent with the rules.
//
// That gate has one known cost: a device that has never been configured AND
// isn't covered by an enabled "*" catch-all has no rule, so the app won't
// mount it, so it never appears in the settings UI's device list (fed by
// mountedDevices()) to BE configured — it has to be mounted once by hand (a
// click in the file manager) to get set up. The alternatives were worse: a
// setting of its own is off by default and so leaves the out-of-box experience
// exactly as broken, while mounting every external volume unconditionally
// turns a transcription app into a general-purpose automounter for every photo
// SD card and flash drive the user owns. If this friction bites, the fix is to
// widen what the device LIST shows (identity is readable from an unmounted
// volume — that's what unmountedVolumeDetected carries), not to widen what the
// app mounts.
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
    // The task queue every grab runs through — wired from QML to the SAME
    // TranscriptionController instance the rest of the app uses (Main.qml), so
    // grabs share one queue with transcribes rather than racing them for the
    // disk. Null means grabs can't run at all: triggerManualGrab() no-ops with
    // lastError set, hotplug grabs are skipped silently (same shape as a
    // missing vault). Settable rather than constructed internally because the
    // instance is QML-owned and shared — unlike m_deviceMonitor, which nothing
    // else needs.
    Q_PROPERTY(TranscriptionController *transcriptionController READ transcriptionController WRITE
                   setTranscriptionController NOTIFY transcriptionControllerChanged)
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

    TranscriptionController *transcriptionController() const { return m_transcriptionController; }
    void setTranscriptionController(TranscriptionController *controller);

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

    // Why a rule's filenameRegex won't compile, as a user-facing sentence — or
    // an empty string if it's valid (an empty pattern is valid: it means "no
    // regex constraint"). Backs the live ✓/✗ indicator under the pattern field
    // in MainPanel.qml's usbGrabberPage.
    //
    // Exists so QML validates against the EXACT SAME QRegularExpression engine
    // UsbGrabJob's walk actually filters with, rather than QML/JS RegExp, whose
    // syntax isn't guaranteed identical — a pattern that looks valid in JS
    // could still match nothing at grab time. Shares one definition with the
    // combine rules' identically-shaped check (see RegexValidation.h).
    Q_INVOKABLE QString regexValidationError(const QString &pattern) const;

    // Looks up deviceSerial's rule (falling back to the "*" catch-all rule
    // if the device-specific one isn't enabled), then starts one UsbGrabJob
    // per currently-mounted VOLUME of that device against the currently-open
    // vault — a device with two mounted volumes runs two jobs under its single
    // rule, and so emits grabFinished (and writes a history entry) twice.
    // No-op (with lastError set) if: no vault is open, the device isn't
    // currently mounted, or neither the device-specific nor the catch-all rule
    // is enabled.
    Q_INVOKABLE void triggerManualGrab(const QString &deviceSerial);

    // Runs the same scan + extension-filter + quick-check dedup logic a real
    // grab would (via UsbGrabJob::startPreview(), a dry-run mode that never
    // calls QFile::copy()), so the result is exactly what "Grab now" would do
    // without actually doing it. Unlike triggerManualGrab(), this does NOT
    // require the resolved rule to be enabled — a preview is useful precisely
    // when deciding whether to enable a rule. No-op (with lastError set, and
    // grabPreviewReady emitted with an empty list) if no vault is open or the
    // device isn't currently mounted. Async: result arrives via
    // grabPreviewReady() — the walk itself still stats every file on an
    // external device, which is too slow for the GUI thread (mirrors why the
    // real grab itself runs on UsbGrabJob's worker thread).
    Q_INVOKABLE void requestGrabPreview(const QString &deviceSerial);

    // Proxies vaultTreeModel->readUsbGrabHistory(), or an empty list if no
    // vault is open.
    Q_INVOKABLE QVariantList history() const;

signals:
    void vaultTreeModelChanged();
    void transcriptionControllerChanged();
    void lastErrorChanged();
    // Emitted once a grab (manual or auto) finishes, after its history entry
    // has already been appended (see handleUsbGrabFinished()'s ordering).
    // Never emitted for a CANCELLED grab: its counts are a partial tally, and
    // announcing "grabbed 3 files" for a run the user just stopped would be a
    // lie about what happened. autoTriggered lets QML word the snackbar for
    // something the user didn't ask for versus something they clicked.
    void grabFinished(const QString &deviceSerial, int filesCopied, int filesSkipped, const QStringList &errors,
                       bool autoTriggered);
    // Emitted once the LAST auto-grab this class enqueued has left the queue —
    // the "only after all auto grabbings have run" barrier the auto-combine
    // hangs off (see the class doc comment for why the combine itself is QML's
    // job). Fires exactly once per batch, and only if at least one auto-grab
    // actually ran: a manual "Grab now" never triggers it, and neither does a
    // hotplug for a device whose autoGrab is off.
    //
    // Fires even if every grab in the batch copied nothing or was cancelled —
    // it reports that the grabs are DONE, not that they found anything. The
    // combine scan re-derives its own candidates from the vault either way, and
    // a scan over an unchanged vault finds no ready groups and does nothing.
    void autoGrabsSettled();
    // Emitted ONCE per requestGrabPreview() call, after every mounted volume of
    // the device has been previewed — entries is their combined result, so this
    // covers exactly what triggerManualGrab() would do. entries is a list of
    // {path, status} maps (status: "grab"/"duplicate"/"filtered" — see
    // UsbGrabJob::startPreview()'s doc comment); paths are relative to each
    // volume's scan root, prefixed with the volume's mount-point folder name
    // when the device has more than one mounted volume (otherwise the same
    // filename on two volumes would render as two identical rows). error is
    // non-empty only on a guard-check failure, in which case entries is empty.
    void grabPreviewReady(const QString &deviceSerial, const QVariantList &entries, const QString &error);

public slots:
    // Re-evaluates every CURRENTLY-mounted device for an auto-grab, for the
    // gap where the app was closed while a device stayed plugged in the whole
    // time: no hotplug (InterfacesAdded) fires on restart for a device that
    // never left, so handleDeviceAdded() never runs and the auto-grab a rule
    // asks for silently never happens. This closes that by walking
    // UsbDeviceMonitor::currentlyMountedDevices() through the SAME auto-grab
    // gate a hotplug uses. Idempotent — runUsbGrabScan() skips files already
    // copied — so re-grabbing an already-grabbed device is a no-op, and it
    // evaluates against the device that is ACTUALLY mounted now rather than a
    // remembered (possibly stale) path. Fired on vault-open (see
    // setVaultTreeModel()); also exposed as a slot so QML/tests can invoke it.
    void rescanForAutoGrab();

private slots:
    // Bound to m_deviceMonitor's deviceAdded signal at construction — see
    // the class doc comment for the enable-check/vault-open-check logic.
    void handleDeviceAdded(const QVariantMap &info);

    // Bound to m_deviceMonitor's unmountedVolumeDetected signal at
    // construction: mounts the volume, but ONLY if its resolved rule is
    // enabled — see the class doc comment for why that's the gate.
    //
    // Gated on `enabled` and NOT on `autoGrab`: mounting is what makes a device
    // addressable at all (previewable, configurable, grabbable by hand), so a
    // device the user has vouched for should mount whether or not they also
    // asked for hands-off grabbing. autoGrab decides what happens AFTER the
    // mount, not whether it happens.
    void handleUnmountedVolumeDetected(const QVariantMap &info);

    // Bound to m_transcriptionController's usbGrabFinished signal: records the
    // history entry, re-emits grabFinished, and retires the job from
    // m_inFlightJobs / m_pendingAutoGrabJobIds — emitting autoGrabsSettled()
    // when that last set empties.
    void handleUsbGrabFinished(int jobId, int filesCopied, int filesSkipped, const QStringList &errors,
                                bool cancelled);

    // Bound to m_transcriptionController's queueChanged: retires any tracked
    // grab that has left the queue WITHOUT reporting a result — i.e. one
    // cancelled while it was still Queued, which the queue drops outright
    // rather than running (so usbGrabFinished never fires for it). Without
    // this the auto-grab barrier would never empty again and the auto-combine
    // would silently stop working for the session.
    void reconcileInFlightJobs();

private:
    // The auto-grab gate shared by handleDeviceAdded() (hotplug) and
    // rescanForAutoGrab() (startup / already-mounted): both gates in order
    // (enabled + autoGrab), then startGrabJob(). Extracted so the two entry
    // points can never drift apart.
    void maybeAutoGrab(const QVariantMap &info);

    // Shared by triggerManualGrab() and handleDeviceAdded(): the device-
    // specific rule if enabled, else the "*" catch-all rule (with
    // deviceSerial/deviceLabel overridden to this device's own identity, so
    // a catch-all-triggered grab's history entry records which real device
    // ran rather than the literal "*" placeholder) if THAT is enabled.
    // Returns the device-specific (disabled) rule as-is if neither is
    // enabled — callers must check the returned rule's `enabled` field
    // before using it for anything.
    UsbGrabRule resolveEffectiveRule(const QString &deviceSerial, const QVariantMap &deviceInfo) const;

    // vaultLocalPath() + rule.destinationFolder (if non-empty) — the actual
    // directory a grab under this rule copies into. Doesn't need to exist yet:
    // UsbGrabJobWorker's copy loop already mkpath()s its destination directory
    // per file (see UsbGrabJob.cpp), so a not-yet-existing destinationFolder is
    // created lazily on the first real copy, same as AudioRecorder's
    // recordingOutputFolder.
    QString resolveDestinationPath(const UsbGrabRule &rule) const;

    // Every currently-mounted VOLUME belonging to deviceSerial (matched against
    // each entry's "serial" key in currentlyMountedDevices()) — empty if the
    // device isn't connected. Returns a list, not a single entry, because one
    // physical device can present several mounted volumes under one serial (see
    // UsbDeviceInfo's doc comment); taking only the first is how the first
    // volume-aware bug got in.
    QVariantList findMountedVolumesBySerial(const QString &deviceSerial) const;

    // Enqueues one grab task (TranscriptionController::enqueueUsbGrab) against
    // the currently-open vault and records what the queue can't: which device
    // and rule the resulting job id belongs to.
    //
    // autoTriggered marks a hotplug-initiated grab, which additionally joins
    // m_pendingAutoGrabJobIds so autoGrabsSettled() can fire when the batch
    // drains. deviceRemoved is deliberately never connected to anything here:
    // an unplug mid-grab just accumulates copy errors in runUsbGrabScan()'s
    // own per-file error handling and finishes naturally (per the confirmed
    // product decision — no early-abort-on-unmount logic).
    void startGrabJob(const UsbGrabRule &rule, const QString &mountPath, bool autoTriggered);

    void setLastError(const QString &error);

    UsbDeviceMonitor m_deviceMonitor;
    UsbRuleStore m_ruleStore;
    VaultTreeModel *m_vaultTreeModel = nullptr;
    TranscriptionController *m_transcriptionController = nullptr;
    QString m_lastError;

    // What one in-flight grab job needs at completion time that its id alone
    // can't answer — snapshotted at enqueue, since the rule can be edited (or
    // the device unplugged) while the job waits its turn in the queue, and the
    // history entry must record what actually RAN.
    struct InFlightGrab {
        QString deviceSerial;
        QString deviceLabel;
        bool autoTriggered = false;
    };
    // Keyed by JOB id, not by volume id or serial: the job id is the only thing
    // that's unique per run and stable across the wait in the queue. (A serial
    // is shared by a device's volumes — keying runtime state by it is a
    // shipped-and-fixed bug that let two concurrent jobs for one device evict
    // each other; a volume id is unique but runtime-only and would still
    // collide across two grabs of the same volume.)
    QHash<int, InFlightGrab> m_inFlightJobs;

    // The subset of m_inFlightJobs' ids that a hotplug started by itself. Emptying
    // this is what fires autoGrabsSettled() — see its doc comment. A set, not a
    // count, so a completion for an id that isn't ours (or arrives twice) can't
    // drive the barrier negative and fire the combine early.
    QSet<int> m_pendingAutoGrabJobIds;
};
