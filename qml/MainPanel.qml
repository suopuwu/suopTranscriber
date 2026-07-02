import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SuopTranscriber

Rectangle {
    id: mainPanel

    property url vaultPath
    property AudioPlayer player
    signal openVaultRequested()

    color: Qt.rgba(AppSettings.mainBackgroundColor.r, AppSettings.mainBackgroundColor.g,
                    AppSettings.mainBackgroundColor.b, AppSettings.mainOpacity)
    radius: AppSettings.borderRadius

    Item {
        anchors.fill: parent
        visible: mainPanel.vaultPath.toString() === ""

        Button {
            id: openVaultButton
            anchors.centerIn: parent
            text: qsTr("Open Vault")
            hoverEnabled: true
            onClicked: mainPanel.openVaultRequested()

            HoverHandler {
                cursorShape: Qt.PointingHandCursor
            }
            HoverBorder {
                anchors.fill: parent
                active: openVaultButton.hovered
            }
        }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 16
        spacing: 12
        visible: mainPanel.vaultPath.toString() !== ""

        Label {
            Layout.fillWidth: true
            elide: Text.ElideMiddle
            text: !mainPanel.player || mainPanel.player.source.toString() === "" ? qsTr("No file selected") : mainPanel.player.source.toString()
            color: "white"
        }

        RowLayout {
            spacing: 12

            Button {
                id: playButton
                text: mainPanel.player && mainPanel.player.playing ? qsTr("Pause") : qsTr("Play")
                enabled: mainPanel.player && mainPanel.player.source.toString() !== ""
                hoverEnabled: true
                onClicked: mainPanel.player.togglePlay()

                HoverHandler {
                    cursorShape: Qt.PointingHandCursor
                    enabled: playButton.enabled
                }
                HoverBorder {
                    anchors.fill: parent
                    active: playButton.hovered
                }
            }

            Slider {
                id: seekSlider
                Layout.fillWidth: true
                hoverEnabled: true
                from: 0
                to: mainPanel.player ? Math.max(mainPanel.player.duration, 1) : 1
                value: mainPanel.player ? mainPanel.player.position : 0
                onMoved: mainPanel.player.position = value

                HoverHandler {
                    cursorShape: Qt.PointingHandCursor
                }
                HoverBorder {
                    anchors.fill: parent
                    active: seekSlider.hovered
                }
            }
        }

        Item {
            Layout.fillHeight: true
        }
    }
}
