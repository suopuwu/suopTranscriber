pragma Singleton
import QtQuick
import QtCore

Settings {
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
}
