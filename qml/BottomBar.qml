import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: bottomBar

    property url vaultPath
    // Placeholder task-progress state — no transcription engine exists yet.
    // Real wiring happens when the engine-protocol milestone lands.
    property real taskProgress: 0
    property int tasksCompleted: 0
    property int tasksTotal: 0

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

        Label {
            Layout.fillWidth: true
            elide: Text.ElideMiddle
            text: bottomBar.vaultPath.toString() === "" ? qsTr("No vault open") : bottomBar.vaultPath.toString()
            color: "#aaaaaa"
            font.pixelSize: 11
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
