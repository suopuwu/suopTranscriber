#pragma once

#include <QList>
#include <QString>
#include <QStringList>

// One persisted USB "grab rule" — see CLAUDE.md's "USB Grabber" feature.
// Plain struct (not QObject/Q_GADGET), matching Segment.h's precedent for a
// small, store-internal value type; UsbGrabberController (a later step) is
// responsible for exposing this to QML (e.g. as a QVariantMap).
struct UsbGrabRule {
    QString deviceSerial;        // stable device identity from UsbDeviceMonitor, or "*" for the any-device catch-all rule
    bool enabled = false;
    QStringList extensionFilter; // e.g. {"wav","mp3","m4a"} — empty means "all files"
    bool preserveStructure = true;
    QString startFolder;         // relative path from device root to start recursive search; "" = device root
    QString deviceLabel;         // human-readable vendor/model string, for display purposes only, not identity
    QString destinationFolder;   // relative path from vault root to grab into; "" = vault root, created if missing
};

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
