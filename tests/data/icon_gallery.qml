// Visual parity/tuning sheet for the icon packs — rendered to a PNG by
// tst_icontheme's env-gated grabGallery slot (never runs in normal ctest).
// Shows every semantic icon of ONE pack (whichever AppSettings.iconPack is)
// on the app's real dark chrome, at the two canonical cluster sizes (15 for
// ribbons, 13 for row clusters). The grab slot renders this twice, flipping
// the pack, so material (default) and classic can be compared side by side
// and the material global scale eyeballed rather than guessed.
import QtQuick
import SuopTranscriber

Rectangle {
    id: sheet
    color: "#252525"

    // Set from the grab slot; drives the global setting via the Binding below
    // so every AppIcon re-resolves. Same pattern as icon_driver.qml.
    property string pack: "material"
    Binding { target: AppSettings; property: "iconPack"; value: sheet.pack }

    readonly property var names: IconTheme.iconNames()
    readonly property int cols: 4
    readonly property real cellW: 150
    readonly property real cellH: 34

    width: cols * cellW + 40
    height: Math.ceil(names.length / cols) * cellH + 70

    Text {
        id: title
        x: 20; y: 16
        text: "iconPack: " + sheet.pack
        color: "#8fdf9f"
        font.pixelSize: 14
        font.bold: true
    }

    Grid {
        x: 20
        y: 50
        columns: sheet.cols
        Repeater {
            model: sheet.names
            Row {
                width: sheet.cellW
                height: sheet.cellH
                spacing: 8
                // 22x22 ribbon button box at iconSize 15
                Rectangle {
                    width: 22; height: 22; radius: 4; color: "#2a2a2a"
                    anchors.verticalCenter: parent.verticalCenter
                    AppIcon { anchors.centerIn: parent; name: modelData; iconSize: 15; color: "#dddddd" }
                }
                // 20x20 row-cluster box at iconSize 13
                Rectangle {
                    width: 20; height: 20; radius: 4; color: "#2a2a2a"
                    anchors.verticalCenter: parent.verticalCenter
                    AppIcon { anchors.centerIn: parent; name: modelData; iconSize: 13; color: "#dddddd" }
                }
                Text {
                    text: modelData; color: "#888888"; font.pixelSize: 10
                    width: 92; height: parent.height
                    verticalAlignment: Text.AlignVCenter; wrapMode: Text.NoWrap; elide: Text.ElideRight
                }
            }
        }
    }
}
