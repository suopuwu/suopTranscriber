#pragma once

#include <QList>
#include <QMetaType>
#include <QString>
#include <QStringList>

// One persisted USB "grab rule" — see DESIGN.md "USB import".
// Plain struct (not QObject/Q_GADGET), matching Segment.h's precedent for a
// small, store-internal value type; UsbGrabberController (a later step) is
// responsible for exposing this to QML (e.g. as a QVariantMap).
struct UsbGrabRule {
    QString deviceSerial;        // stable device identity from UsbDeviceMonitor, or "*" for the any-device catch-all rule
    bool enabled = false;
    // Grab this device the moment it mounts, with no user action. Strictly
    // NARROWER than `enabled`, never a substitute for it: `enabled` is what
    // makes the device grabbable at all (and is what gates the app's own
    // mounting — see UsbGrabberController's class doc comment), while this
    // says whether a hotplug ALSO fires the grab off by itself. So an
    // enabled+!autoGrab device still mounts, still appears in the UI, and
    // still grabs from "Grab now" — it just waits to be asked.
    //
    // Per-device rather than one global setting because the two devices a user
    // owns want opposite answers: the recorder they plug in to offload should
    // just do it, while the photo SD card that happens to match a catch-all
    // shouldn't start copying on its own. Default false — auto-grab is the
    // surprising behavior, so it's opted into per device, and an existing
    // enabled rule keeps behaving manually until its owner says otherwise.
    bool autoGrab = false;
    QStringList extensionFilter; // e.g. {"wav","mp3","m4a"} — empty means "all files"
    // Optional QRegularExpression source matched against a candidate's FILE
    // NAME only (never its path) — empty means "no regex constraint", not
    // "match nothing". Deliberately the same semantics as the split-recording
    // combine rules (see matchRuleCapturedKey() in TranscriptionController.cpp):
    // same engine, same basename-only target, and UNANCHORED unless the pattern
    // says otherwise — so a pattern reads identically in both features and can
    // be moved between them. ANDed with extensionFilter rather than replacing
    // it: a file is grabbed only if it satisfies both, and each field is
    // independently "no constraint" when blank.
    QString filenameRegex;
    bool preserveStructure = true;
    QString startFolder;         // relative path from device root to start recursive search; "" = device root
    QString deviceLabel;         // human-readable vendor/model string, for display purposes only, not identity
    QString destinationFolder;   // relative path from vault root to grab into; "" = vault root, created if missing
};

// So a whole rule can ride a queued cross-thread invoke as one Q_ARG (see
// UsbGrabPreviewJob::start(), and TranscriptionController's usbGrab job kind)
// instead of being shredded into six loose parallel arguments that a new rule
// field would have to be threaded through by hand every time.
Q_DECLARE_METATYPE(UsbGrabRule)

// Persists UsbGrabRule entries APP-SCOPED via QSettings (NOT vault-scoped
// .transcriber/settings.json) — the same USB device is plugged in across many
// vaults by the same user, so its rule must survive independent of which
// vault happens to be open. Mirrors TranscriptionController's phaseTimings
// storage (see loadPhaseTimings()/savePhaseTiming() in
// TranscriptionController.cpp): one QSettings key per rule under a fixed
// group, keyed by the (escaped) deviceSerial and enumerated via
// QSettings::childKeys() since the set of ever-configured devices isn't a
// fixed list known upfront. Uses the default QSettings() constructor, which
// resolves via the app identity QCoreApplication::setOrganizationName/
// setApplicationName set in main.cpp — the same identity TranscriptionController
// already relies on.
class UsbRuleStore
{
public:
    UsbRuleStore() = default;

    // Enumerates every persisted rule, including the "*" catch-all rule if
    // one was ever saved. Order is whatever QSettings::childKeys() returns.
    QList<UsbGrabRule> loadAllRules() const;

    // Upsert by deviceSerial: inserts if new, overwrites if already persisted.
    void saveRule(const UsbGrabRule &rule);

    void removeRule(const QString &deviceSerial);

    // Returns a default-constructed (disabled) rule — with deviceSerial set —
    // if this device has never been configured, so callers can always get a
    // rule to populate a settings form.
    UsbGrabRule ruleForSerial(const QString &deviceSerial) const;
};
