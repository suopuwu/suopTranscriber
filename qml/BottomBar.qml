import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SuopTranscriber

Item {
    id: bottomBar

    property url vaultPath
    property string vaultLocalPath: ""
    property TranscriptionController transcriptionController
    signal settingsRequested()
    // Single-shot engine only for now (see DESIGN.md's durable/resumable
    // milestone) — at most one task is ever in flight, so totals are 0/1.
    readonly property real taskProgress: transcriptionController ? transcriptionController.progress : 0
    readonly property int tasksCompleted: 0
    readonly property int tasksTotal: transcriptionController && transcriptionController.busy ? 1 : 0

    implicitHeight: 28

    Rectangle {
        anchors.fill: parent
        color: "#181818"
    }

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: 8
        anchors.rightMargin: 8
        spacing: 12

        Item {
            id: settingsButton
            visible: AppSettings.useSystemTitlebar
            Layout.preferredWidth: 20
            Layout.preferredHeight: 20

            HoverHandler {
                id: settingsHover
                cursorShape: Qt.PointingHandCursor
            }
            TapHandler {
                onTapped: bottomBar.settingsRequested()
            }
            HoverBorder {
                anchors.fill: parent
                active: settingsHover.hovered
            }
            Text {
                anchors.centerIn: parent
                text: "⚙"
                color: "#aaaaaa"
                font.pixelSize: 12
            }
        }

        Item {
            Layout.fillWidth: true
            implicitHeight: vaultPathLabel.implicitHeight

            HoverHandler {
                id: vaultPathHover
                cursorShape: Qt.PointingHandCursor
                enabled: bottomBar.vaultLocalPath !== ""
            }
            TapHandler {
                enabled: bottomBar.vaultLocalPath !== ""
                onTapped: Qt.openUrlExternally(bottomBar.vaultPath)
            }
            Label {
                id: vaultPathLabel
                anchors.left: parent.left
                anchors.right: parent.right
                elide: Text.ElideMiddle
                text: bottomBar.vaultLocalPath === "" ? qsTr("No vault open") : bottomBar.vaultLocalPath
                color: "#aaaaaa"
                font.pixelSize: 11
                font.underline: vaultPathHover.hovered
            }
        }

        ProgressBar {
            Layout.preferredWidth: 140
            from: 0
            to: 1
            value: bottomBar.taskProgress
            visible: bottomBar.tasksTotal > 0
        }

        Label {
            text: qsTr("%1/%2 tasks").arg(bottomBar.tasksCompleted).arg(bottomBar.tasksTotal)
            color: "#aaaaaa"
            font.pixelSize: 11
            visible: bottomBar.tasksTotal > 0
        }
    }
}
