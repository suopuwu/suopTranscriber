// Driver for tst_icontheme.cpp's QQuickView slots — the smallest thing that
// exercises AppIcon's real resolution path. `pack`/`iconName` are writable
// from the test so one view can be re-pointed at any pack/icon combination.
import QtQuick
import SuopTranscriber

Item {
    id: root
    width: 64
    height: 64

    property string pack: "material"
    property string iconName: "transcribe"

    // AppIcon reads AppSettings.iconPack directly (that's what makes a pack
    // switch re-resolve every icon app-wide), so driving the setting is how
    // the test drives the component.
    Binding {
        target: AppSettings
        property: "iconPack"
        value: root.pack
    }

    AppIcon {
        id: icon
        objectName: "icon"
        anchors.centerIn: parent
        name: root.iconName
        iconSize: 16
        color: "#dddddd"
    }

    // Exercises the Settings pack-picker preview path: forces a pack
    // regardless of AppSettings.iconPack.
    property string previewPack: ""
    AppIcon {
        id: previewIcon
        objectName: "previewIcon"
        name: root.iconName
        iconSize: 16
        packOverride: root.previewPack
    }
}
