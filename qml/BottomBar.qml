import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SuopTranscriber

Item {
    id: bottomBar

    property url vaultPath
    property string vaultLocalPath: ""
    property TranscriptionController transcriptionController
    property VaultTreeModel vaultTreeModel
    // Wave D item 2: the currently-open file's raw local path (audio file or
    // orphaned .md — whatever MainPanel currently shows), empty if none.
    property string currentFilePath: ""
    readonly property string currentFileRelativePath: {
        if (bottomBar.currentFilePath === "" || bottomBar.vaultLocalPath === "")
            return ""
        const prefix = bottomBar.vaultLocalPath + "/"
        return bottomBar.currentFilePath.startsWith(prefix)
            ? bottomBar.currentFilePath.slice(prefix.length) : bottomBar.currentFilePath
    }
    signal settingsRequested()
    readonly property real taskProgress: transcriptionController ? transcriptionController.progress : 0
    readonly property int tasksCompleted: transcriptionController ? transcriptionController.tasksCompleted : 0
    readonly property int tasksTotal: transcriptionController ? transcriptionController.tasksTotal : 0
    // Whether a job is actively running right now — distinct from tasksTotal
    // (which also counts Done/Failed jobs still sitting in the persistent,
    // dismissible history list, see TranscriptionController's class doc).
    readonly property bool busy: transcriptionController ? transcriptionController.busy : false

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
            implicitHeight: vaultPathRow.implicitHeight

            HoverHandler {
                id: vaultPathHover
                cursorShape: Qt.PointingHandCursor
                enabled: bottomBar.vaultLocalPath !== ""
            }
            TapHandler {
                enabled: bottomBar.vaultLocalPath !== ""
                onTapped: {
                    // Reveal the open file (selected) when there is one;
                    // otherwise just open the vault folder itself.
                    if (bottomBar.currentFilePath !== "" && bottomBar.vaultTreeModel)
                        bottomBar.vaultTreeModel.revealInFileManager("file://" + bottomBar.currentFilePath)
                    else
                        Qt.openUrlExternally(bottomBar.vaultPath)
                }
            }
            RowLayout {
                id: vaultPathRow
                anchors.left: parent.left
                anchors.right: parent.right
                spacing: 0

                // Item 4 (Wave E): both parts must read as ONE continuous
                // left-aligned string. Previously the vault-root Label alone
                // had Layout.fillWidth, which claimed the whole row and shoved
                // the subpath Label flush against the right edge instead of
                // immediately after it. Now the (variable-width, non-elided)
                // vault-root Label sits at its natural width, and the elided
                // fillWidth claim moves to the subpath Label after it, so
                // eliding happens at the far end of the combined string, not
                // in the middle of the vault path.
                Label {
                    Layout.fillWidth: bottomBar.currentFileRelativePath === ""
                    elide: Text.ElideMiddle
                    text: bottomBar.vaultLocalPath === "" ? qsTr("No vault open") : bottomBar.vaultLocalPath
                    color: "#aaaaaa"
                    font.pixelSize: 11
                    font.underline: vaultPathHover.hovered
                }
                // Wave D item 2: subfolder/filename part of the currently-
                // open file, dimmer than the vault-root part above.
                Label {
                    visible: bottomBar.currentFileRelativePath !== ""
                    Layout.fillWidth: true
                    elide: Text.ElideRight
                    text: "/" + bottomBar.currentFileRelativePath
                    color: "#666666"
                    font.pixelSize: 11
                    font.underline: vaultPathHover.hovered
                }
            }
        }

        ProgressBar {
            Layout.preferredWidth: 140
            from: 0
            to: 1
            value: bottomBar.taskProgress
            // Only while something is ACTIVELY running — tasksTotal alone
            // would keep this visible (frozen, meaningless) after every job
            // finishes, since finished jobs stay listed until dismissed.
            visible: bottomBar.busy
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
                        color: (clearButton.hovered && clearButton.enabled) ? "#3a3a3a" : "#2a2a2a"
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
