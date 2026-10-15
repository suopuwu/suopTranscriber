import QtQuick
import QtQuick.Controls
import SuopTranscriber

// The compress action's icon button: a tap opens a menu of the two targets
// (TranscriptionController::compressAudioFiles()'s "voice" / "lossless"), each
// enabled only when the call site has files it applies to. Shared by the
// ribbon, the search-results row and MainPanel's cluster so the three can't
// drift. The call site does the compressing on compressRequested(target).
AppIconButton {
    id: root

    property bool voiceEnabled: false
    property bool losslessEnabled: false

    signal compressRequested(string target)

    iconName: "compress"
    enabled: root.voiceEnabled || root.losslessEnabled
    onTapped: compressMenu.visible ? compressMenu.close() : compressMenu.open()

    Popup {
        id: compressMenu
        objectName: root.objectName + "Menu"
        x: 0
        y: root.height + 4
        width: 240
        padding: 4
        // Kept inside the window: near an edge the menu shifts rather than
        // clipping.
        margins: 8
        modal: true
        dim: true
        focus: false
        closePolicy: Popup.CloseOnPressOutside | Popup.CloseOnEscape

        background: Rectangle {
            color: Theme.surfaceDialog
            radius: AppSettings.borderRadius
            border.width: 1
            border.color: Theme.border
        }

        Column {
            width: compressMenu.width - compressMenu.padding * 2
            spacing: 2

            Repeater {
                model: [
                    { target: "voice", label: qsTr("FLAC voice (16 kHz mono)"), icon: "format-voice",
                      available: root.voiceEnabled },
                    { target: "lossless", label: qsTr("Lossless FLAC"), icon: "format-flac",
                      available: root.losslessEnabled }
                ]
                delegate: Item {
                    id: optionRow
                    required property var modelData
                    objectName: root.objectName + "_" + modelData.target
                    width: parent.width
                    height: 28
                    enabled: optionRow.modelData.available

                    HoverHandler {
                        id: optionHover
                        cursorShape: Qt.PointingHandCursor
                        enabled: optionRow.enabled
                    }
                    TapHandler {
                        // Exclusive press: see Sidebar.qml contextMenu.
                        gesturePolicy: TapHandler.WithinBounds
                        enabled: optionRow.enabled
                        onTapped: {
                            compressMenu.close()
                            root.compressRequested(optionRow.modelData.target)
                        }
                    }
                    Rectangle {
                        anchors.fill: parent
                        radius: AppSettings.borderRadius
                        color: optionHover.hovered ? Theme.surfaceHover : "transparent"
                    }
                    AppIcon {
                        id: optionIcon
                        anchors.left: parent.left
                        anchors.leftMargin: 8
                        anchors.verticalCenter: parent.verticalCenter
                        name: optionRow.modelData.icon
                        iconSize: 14
                        color: optionRow.enabled ? Theme.textPrimary : Theme.textDisabled
                    }
                    Text {
                        anchors.left: optionIcon.right
                        anchors.leftMargin: 8
                        anchors.verticalCenter: parent.verticalCenter
                        text: qsTr("Compress to %1").arg(optionRow.modelData.label)
                        color: optionRow.enabled ? "white" : Theme.textDisabled
                        font.pixelSize: 12
                    }
                }
            }
        }
    }
}
