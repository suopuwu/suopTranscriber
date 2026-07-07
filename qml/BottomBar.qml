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
    readonly property real taskProgress: transcriptionController ? transcriptionController.progress : 0
    readonly property int tasksCompleted: transcriptionController ? transcriptionController.tasksCompleted : 0
    readonly property int tasksTotal: transcriptionController ? transcriptionController.tasksTotal : 0

    implicitHeight: 28

    Rectangle {
        anchors.fill: parent
        color: "#181818"
        // Unlike TitleBar, BottomBar is always visible — only round its
        // outer corners when frameless (rounds the window's true bottom
        // corners natively instead of relying on an outer OpacityMask).
        bottomLeftRadius: AppSettings.useSystemTitlebar ? 0 : AppSettings.borderRadius
        bottomRightRadius: AppSettings.useSystemTitlebar ? 0 : AppSettings.borderRadius
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

        Item {
            id: tasksButton
            visible: bottomBar.tasksTotal > 0
            Layout.preferredWidth: tasksLabel.implicitWidth
            Layout.preferredHeight: tasksLabel.implicitHeight

            HoverHandler {
                id: tasksHover
                cursorShape: Qt.PointingHandCursor
            }
            TapHandler {
                onTapped: jobListPopup.visible ? jobListPopup.close() : jobListPopup.open()
            }
            HoverBorder {
                anchors.fill: parent
                active: tasksHover.hovered
            }
            Label {
                id: tasksLabel
                text: qsTr("%1/%2 tasks").arg(bottomBar.tasksCompleted).arg(bottomBar.tasksTotal)
                color: "#aaaaaa"
                font.pixelSize: 11
                font.underline: tasksHover.hovered
            }
        }
    }

    // Item 6 (Milestone 5): persistent, dismissible task list — like a
    // download manager. Completed/failed jobs stay listed (with status)
    // until individually dismissed or "Clear" is clicked, instead of
    // vanishing the instant they finish.
    Popup {
        id: jobListPopup
        y: -height - 4
        // tasksButton.x is relative to the RowLayout it's nested in, not to
        // bottomBar (this popup's actual positioning parent) — using it
        // directly put the popup mostly off the right edge of the window.
        // Anchor to bottomBar's own right edge instead.
        x: bottomBar.width - width - 8
        width: 320
        height: jobColumn.implicitHeight + 16
        modal: false
        focus: false
        closePolicy: Popup.CloseOnPressOutside | Popup.CloseOnEscape

        background: Rectangle {
            color: "#242424"
            radius: AppSettings.borderRadius
            border.width: 1
            border.color: "#3a3a3a"
        }

        ColumnLayout {
            id: jobColumn
            anchors.fill: parent
            anchors.margins: 8
            spacing: 4

            RowLayout {
                id: taskHeaderRow
                Layout.fillWidth: true

                Label {
                    Layout.fillWidth: true
                    text: qsTr("Tasks")
                    color: "white"
                    font.bold: true
                }
                Button {
                    id: clearButton
                    text: qsTr("Clear")
                    hoverEnabled: true
                    onClicked: bottomBar.transcriptionController.clearFinished()

                    background: Rectangle {
                        radius: AppSettings.borderRadius
                        color: clearButton.hovered ? "#3a3a3a" : "#2a2a2a"
                        border.width: 1
                        border.color: "#3a3a3a"
                    }
                    HoverHandler {
                        cursorShape: Qt.PointingHandCursor
                    }
                    HoverBorder {
                        anchors.fill: parent
                        active: clearButton.hovered
                    }
                }
            }

            ScrollView {
                Layout.fillWidth: true
                // Explicit preferredHeight (not Layout.fillHeight) — a
                // fillHeight ScrollView inside a Popup whose own height is
                // in turn derived from this ColumnLayout's implicitHeight
                // is a circular/ill-defined size request: the ScrollView
                // contributes ~0 to implicitHeight since "fill" isn't an
                // intrinsic size, which collapsed the whole popup down to
                // just the header row. Deriving from the ListView's real
                // contentHeight (capped) breaks that cycle.
                Layout.preferredHeight: Math.min(jobListView.contentHeight, 200)
                clip: true
                ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

                ListView {
                    id: jobListView
                    model: bottomBar.transcriptionController ? bottomBar.transcriptionController.jobs : []
                    spacing: 2

                    delegate: Item {
                        id: jobRow
                        required property var modelData
                        width: jobListView.width
                        height: 24

                        readonly property string fileName: {
                            const s = modelData.fileUrl ? modelData.fileUrl.toString() : ""
                            return s.substring(s.lastIndexOf("/") + 1)
                        }
                        readonly property color statusColor: {
                            switch (modelData.status) {
                            case "done": return "#2ecc71"
                            case "failed": return "#e74c3c"
                            case "active": return "#3498db"
                            default: return "#f1c40f"
                            }
                        }

                        RowLayout {
                            anchors.fill: parent
                            spacing: 6

                            Rectangle {
                                width: 8
                                height: 8
                                radius: 4
                                color: jobRow.statusColor
                            }
                            Label {
                                Layout.fillWidth: true
                                elide: Text.ElideMiddle
                                text: jobRow.fileName
                                color: "white"
                                font.pixelSize: 11
                            }
                            Label {
                                text: modelData.status
                                color: "#888888"
                                font.pixelSize: 10
                            }
                            Item {
                                id: dismissButton
                                visible: modelData.status === "done" || modelData.status === "failed"
                                Layout.preferredWidth: 16
                                Layout.preferredHeight: 16

                                HoverHandler {
                                    id: dismissHover
                                    cursorShape: Qt.PointingHandCursor
                                }
                                TapHandler {
                                    onTapped: bottomBar.transcriptionController.dismiss(jobRow.modelData.fileUrl)
                                }
                                HoverBorder {
                                    anchors.fill: parent
                                    active: dismissHover.hovered
                                }
                                Text {
                                    anchors.centerIn: parent
                                    text: "×"
                                    color: "#dddddd"
                                    font.pixelSize: 13
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}
