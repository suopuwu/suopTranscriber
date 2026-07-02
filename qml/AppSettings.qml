pragma Singleton
import QtQuick
import QtCore

Settings {
    id: appSettings
    category: "app"

    property url lastVaultPath

    property real borderRadius: 8
    property bool useSystemTitlebar: true

    property color sidebarBackgroundColor: "#1e1e1e"
    property real sidebarOpacity: 1.0
    property bool sidebarBlur: false

    property color mainBackgroundColor: "#252525"
    property real mainOpacity: 1.0
    property bool mainBlur: false

    property string lastModel: "base.en"

    // Thin white translucent edge by default — "liquid glass" look.
    property bool rimEnabled: true
    property real rimWidth: 1
    property color rimColor: "#99ffffff"

    // false = "forgo labels": ignore (but don't delete) any stored label
    // frontmatter and rename the actual audio+transcript files instead.
    property bool useLabels: true

    // Independently toggleable per surface (Milestone 5 item 20).
    property bool hideExtensionsSidebar: false
    property bool hideExtensionsMainPanel: false

    // Single source of truth for every per-setting/global "reset to
    // default" button — keep in sync with the property declarations above.
    // lastVaultPath/lastModel are intentionally excluded: they track
    // last-used state, not a user-adjustable appearance/behavior setting.
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
        useLabels: true,
        hideExtensionsSidebar: false,
        hideExtensionsMainPanel: false
    })

    function isDefault(name) {
        if (!(name in defaults))
            return true
        const current = appSettings[name]
        const def = defaults[name]
        if (current instanceof Object && "r" in current && "g" in current && "b" in current)
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
}
