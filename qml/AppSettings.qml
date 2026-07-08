pragma Singleton
import QtQuick
import QtCore

// Item 9 (Wave E): appearance/behavior settings now live in the VAULT
// (<vault>/.suopTranscriber, JSON — VaultTreeModel::readVaultSettings/
// writeVaultSettings), not an app-scoped QSettings .conf file — different
// vaults can have different look/behavior preferences. lastVaultPath/
// lastModel stay app-scoped (small nested Settings below): they're
// last-used STATE needed to bootstrap which vault to open in the first
// place, not a vault-specific preference.
Item {
    id: appSettings

    // Set once by Main.qml (AppSettings.activeVaultModel = vaultModel) so
    // loadFromVault()/saveToVault() below don't need the caller to pass it
    // on every single settings change.
    property var activeVaultModel: null
    // True only while loadFromVault() is actively assigning properties —
    // suppresses the onXChanged-triggered save-back below, so loading never
    // immediately re-triggers a write of the same data it just read.
    property bool suppressSave: false

    Settings {
        id: appLevelSettings
        category: "app"
        property url lastVaultPath
        property string lastModel: "base.en"
    }
    property alias lastVaultPath: appLevelSettings.lastVaultPath
    property alias lastModel: appLevelSettings.lastModel

    property real borderRadius: 8
    property bool useSystemTitlebar: true

    property color sidebarBackgroundColor: "#1e1e1e"
    property real sidebarOpacity: 1.0
    property bool sidebarBlur: false

    property color mainBackgroundColor: "#252525"
    property real mainOpacity: 1.0
    property bool mainBlur: false

    // AI auto-labeling (DESIGN.md "Model management"): opt-in second pass
    // after transcription, populates label/summary/tags via LabelingEngine.
    // Off by default — it's an extra ~2.5GB model download plus GPU time
    // per file, not something to silently switch on for everyone.
    property bool autoLabelEnabled: false

    // Item 14: when autoLabelEnabled, tags freely generate new ones by
    // default (false). Restricting to the vault's existing tag vocabulary
    // is an explicit opt-in — auto-categorizing (leaving a file untagged
    // when nothing existing fits) is a deliberately different mode, not
    // the default behavior.
    property bool restrictTagsToExisting: false

    // Item 15 (Wave E): soft nudge (0.0-1.0) between inventing new tags vs.
    // reusing existing ones, when NOT hard-restricted (restrictTagsToExisting
    // above is the hard on/off switch; this is a gradient for when it's off).
    // Higher = more willing to reuse an existing tag even if it's only a
    // loose/"stretched" fit, rather than invent a precise new one.
    property real tagReuseBias: 0.5

    // Thin white translucent edge by default — "liquid glass" look.
    property bool rimEnabled: true
    property real rimWidth: 1
    property color rimColor: "#99ffffff"

    // false = "forgo labels": ignore (but don't delete) any stored label
    // frontmatter and rename the actual audio+transcript files instead.
    // Main-panel-only: it's the surface with rename UI (click-to-rename the
    // title) — Sidebar has no rename affordance, so its toggle (below) only
    // ever affects display, never rename behavior.
    property bool useLabelsMainPanel: true
    // Display-only (Sidebar rows have no rename UI): show each file's label
    // instead of its filename in the tree, when one exists.
    property bool useLabelsSidebar: true

    // Independently toggleable per surface (Milestone 5 item 20).
    property bool hideExtensionsSidebar: false
    property bool hideExtensionsMainPanel: false

    // Main panel's sentence-playback highlight (see MainPanel.qml's
    // sentenceHighlightOverlay) and Ctrl+F search-match highlight (Item 7,
    // Wave E: searchMatchOverlay) — both real overlay Rectangles. Color is
    // shared (highlightColor); radius has its OWN setting (highlightRadius),
    // separate from the generic UI borderRadius, since the user wants these
    // two independently adjustable.
    property color highlightColor: "#3a5a7a"
    property real highlightRadius: 8

    // Transcript gutter's displayed timestamp precision — NOT the stored
    // data (segment lines always keep full sub-second precision on disk,
    // see TranscriptionController::formatTimestamp); this only rounds what
    // MainPanel's gutter shows. 0 (default) hides fractional seconds
    // entirely ("3", "1:07"); each increment adds one more decimal digit.
    property int timestampDecimalPlaces: 0

    // Single source of truth for every per-setting/global "reset to
    // default" button, AND for loadFromVault()/saveToVault() below (every
    // key here round-trips through the vault's .suopTranscriber file).
    // lastVaultPath/lastModel are intentionally excluded: they track
    // last-used state, not a vault-specific appearance/behavior setting.
    readonly property var defaults: ({
        borderRadius: 8,
        useSystemTitlebar: true,
        sidebarBackgroundColor: "#1e1e1e",
        sidebarOpacity: 1.0,
        sidebarBlur: false,
        mainBackgroundColor: "#252525",
        mainOpacity: 1.0,
        mainBlur: false,
        rimEnabled: true,
        rimWidth: 1,
        rimColor: "#99ffffff",
        useLabelsMainPanel: true,
        useLabelsSidebar: true,
        hideExtensionsSidebar: false,
        hideExtensionsMainPanel: false,
        autoLabelEnabled: false,
        restrictTagsToExisting: false,
        tagReuseBias: 0.5,
        highlightColor: "#3a5a7a",
        highlightRadius: 8,
        timestampDecimalPlaces: 0
    })

    // QML `color` values need to be stringified before crossing into a
    // QVariantMap bound for JSON (QJsonValue::fromVariant doesn't know how
    // to serialize a QColor) — same detection idiom isDefault() already uses.
    function isColorValue(v) {
        return v instanceof Object && "r" in v && "g" in v && "b" in v
    }

    function isDefault(name) {
        if (!(name in defaults))
            return true
        const current = appSettings[name]
        const def = defaults[name]
        if (isColorValue(current))
            return Qt.colorEqual(current, def)
        return current === def
    }

    function resetProperty(name) {
        if (name in defaults)
            appSettings[name] = defaults[name]
    }

    function resetAll() {
        for (const key in defaults)
            resetProperty(key)
    }

    // Reads <vault>/.suopTranscriber (VaultTreeModel::readVaultSettings) and
    // applies every known key, falling back to its default if the file is
    // missing/empty (fresh vault) or doesn't have that particular key yet
    // (settings added in a later app version). suppressSave guards against
    // the property assignments below immediately triggering saveToVault().
    function loadFromVault(vaultModel) {
        appSettings.suppressSave = true
        appSettings.activeVaultModel = vaultModel
        const loaded = vaultModel ? vaultModel.readVaultSettings() : {}
        for (const key in appSettings.defaults)
            appSettings[key] = (key in loaded) ? loaded[key] : appSettings.defaults[key]
        appSettings.suppressSave = false
    }

    function saveToVault() {
        if (appSettings.suppressSave || !appSettings.activeVaultModel)
            return
        const data = ({})
        for (const key in appSettings.defaults) {
            const value = appSettings[key]
            data[key] = isColorValue(value) ? String(value) : value
        }
        appSettings.activeVaultModel.writeVaultSettings(data)
    }

    // Debounced so dragging a slider doesn't write the file on every pixel
    // of movement — coalesces into one write shortly after changes settle.
    Timer {
        id: saveTimer
        interval: 250
        onTriggered: appSettings.saveToVault()
    }
    function scheduleSave() {
        if (!appSettings.suppressSave)
            saveTimer.restart()
    }

    // One handler per persisted key (mechanical, mirrors the defaults map
    // above) — QML has no generic "any property on this object changed"
    // signal, so each needs its own connection.
    Connections {
        target: appSettings
        function onBorderRadiusChanged() { appSettings.scheduleSave() }
        function onUseSystemTitlebarChanged() { appSettings.scheduleSave() }
        function onSidebarBackgroundColorChanged() { appSettings.scheduleSave() }
        function onSidebarOpacityChanged() { appSettings.scheduleSave() }
        function onSidebarBlurChanged() { appSettings.scheduleSave() }
        function onMainBackgroundColorChanged() { appSettings.scheduleSave() }
        function onMainOpacityChanged() { appSettings.scheduleSave() }
        function onMainBlurChanged() { appSettings.scheduleSave() }
        function onRimEnabledChanged() { appSettings.scheduleSave() }
        function onRimWidthChanged() { appSettings.scheduleSave() }
        function onRimColorChanged() { appSettings.scheduleSave() }
        function onUseLabelsMainPanelChanged() { appSettings.scheduleSave() }
        function onUseLabelsSidebarChanged() { appSettings.scheduleSave() }
        function onHideExtensionsSidebarChanged() { appSettings.scheduleSave() }
        function onHideExtensionsMainPanelChanged() { appSettings.scheduleSave() }
        function onAutoLabelEnabledChanged() { appSettings.scheduleSave() }
        function onRestrictTagsToExistingChanged() { appSettings.scheduleSave() }
        function onTagReuseBiasChanged() { appSettings.scheduleSave() }
        function onHighlightColorChanged() { appSettings.scheduleSave() }
        function onHighlightRadiusChanged() { appSettings.scheduleSave() }
        function onTimestampDecimalPlacesChanged() { appSettings.scheduleSave() }
    }
}
