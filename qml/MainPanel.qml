import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SuopTranscriber

Rectangle {
    id: mainPanel

    property url vaultPath
    property string vaultLocalPath: ""
    property AudioPlayer player
    property TranscriptionController transcriptionController
    property var storedMetadata: ({})
    readonly property bool hasStoredMetadata: Object.keys(mainPanel.storedMetadata).length > 0
    signal openVaultRequested()

    function relativeToVault(fileUrl) {
        const filePath = fileUrl.toString()
        const prefix = "file://" + mainPanel.vaultLocalPath + "/"
        return filePath.startsWith(prefix) ? filePath.slice(prefix.length) : filePath
    }

    function refreshMetadata() {
        storedMetadata = (player && player.source.toString() !== "" && transcriptionController)
            ? transcriptionController.readMetadata(player.source) : ({})
        speakerDetectionToggle.checked = mainPanel.hasStoredMetadata && storedMetadata.speakerDetection === true
    }

    Connections {
        target: mainPanel.player
        function onSourceChanged() { mainPanel.refreshMetadata() }
    }
    Connections {
        target: mainPanel.transcriptionController
        function onFinished() { mainPanel.refreshMetadata() }
    }

    color: Qt.rgba(AppSettings.mainBackgroundColor.r, AppSettings.mainBackgroundColor.g,
                    AppSettings.mainBackgroundColor.b, AppSettings.mainOpacity)
    // Only the corners touching the SplitView divider are rounded; the
    // window-edge corners are left square so the outer window mask (which
    // already rounds the true window corners) isn't duplicated here.
    topLeftRadius: AppSettings.borderRadius
    bottomLeftRadius: AppSettings.borderRadius

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
            text: !mainPanel.player || mainPanel.player.source.toString() === "" ? qsTr("No file selected") : mainPanel.relativeToVault(mainPanel.player.source)
            color: "white"
        }

        RowLayout {
            spacing: 12
            visible: mainPanel.hasStoredMetadata

            Label {
                text: qsTr("Previously transcribed — model: %1, speaker detection: %2")
                      .arg(mainPanel.storedMetadata.model || "?")
                      .arg(mainPanel.storedMetadata.speakerDetection ? qsTr("on") : qsTr("off"))
                color: "#aaaaaa"
                font.pixelSize: 11
            }
        }

        RowLayout {
            spacing: 12

            CheckBox {
                id: speakerDetectionToggle
                text: qsTr("Speaker detection")

                HoverHandler {
                    cursorShape: Qt.PointingHandCursor
                }
            }
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

            Button {
                id: transcribeButton
                text: mainPanel.transcriptionController && mainPanel.transcriptionController.busy
                      ? qsTr("Transcribing…") : qsTr("Transcribe")
                enabled: mainPanel.player && mainPanel.player.source.toString() !== ""
                         && mainPanel.transcriptionController && !mainPanel.transcriptionController.busy
                         && (!mainPanel.hasStoredMetadata
                             || speakerDetectionToggle.checked !== (mainPanel.storedMetadata.speakerDetection === true))
                hoverEnabled: true
                onClicked: mainPanel.transcriptionController.transcribe(mainPanel.player.source, mainPanel.vaultPath,
                                                                         speakerDetectionToggle.checked)

                HoverHandler {
                    cursorShape: Qt.PointingHandCursor
                    enabled: transcribeButton.enabled
                }
                HoverBorder {
                    anchors.fill: parent
                    active: transcribeButton.hovered
                }
            }
        }

        Item {
            Layout.fillHeight: true
        }
    }
}
