import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtCore
import Qt.labs.folderlistmodel
import SuopTranscriber

// In-app folder browser replacing the native/portal FolderDialog. The native
// dialog on this system (xdg-desktop-portal-kde) greys out "Open" whenever
// nothing inside the currently-browsed folder is explicitly selected, with
// no QML-level way to change that — so Open here always opens whichever
// folder is currently being browsed (see Milestone 4 Task 8).
Popup {
    id: vaultDialog

    property url currentFolder: StandardPaths.writableLocation(StandardPaths.HomeLocation)

    signal accepted(url folder)

    modal: true
    focus: true
    width: 520
    height: 420
    x: parent ? (parent.width - width) / 2 : 0
    y: parent ? (parent.height - height) / 2 : 0
    closePolicy: Popup.CloseOnEscape

    background: Rectangle {
        color: "#242424"
        radius: AppSettings.borderRadius
        border.width: 1
        border.color: "#3a3a3a"
    }

    FolderListModel {
        id: folderModel
        folder: vaultDialog.currentFolder
        showFiles: false
        showDirs: true
        showDotAndDotDot: false
        showHidden: false
        sortField: FolderListModel.Name
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 16
        spacing: 8

        Label {
            text: qsTr("Open Vault Folder")
            color: "white"
            font.bold: true
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            Button {
                id: upButton
                text: qsTr("Up")
                enabled: folderModel.parentFolder.toString() !== ""
                         && folderModel.parentFolder.toString() !== vaultDialog.currentFolder.toString()
                hoverEnabled: true
                onClicked: vaultDialog.currentFolder = folderModel.parentFolder

                background: Rectangle {
                    radius: AppSettings.borderRadius
                    color: upButton.hovered ? "#3a3a3a" : "#2a2a2a"
                    opacity: upButton.enabled ? 1 : 0.5
                    border.width: 1
                    border.color: "#3a3a3a"
                }
                HoverHandler {
                    cursorShape: Qt.PointingHandCursor
                    enabled: upButton.enabled
                }
                HoverBorder {
                    anchors.fill: parent
                    active: upButton.enabled && upButton.hovered
                }
            }

            Label {
                Layout.fillWidth: true
                elide: Text.ElideMiddle
                text: vaultDialog.currentFolder.toString().replace("file://", "")
                color: "#aaaaaa"
            }
        }

        ScrollView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

            ListView {
                id: listView
                model: folderModel

                delegate: Item {
                    id: rowItem
                    width: listView.width
                    height: 32

                    required property string fileName
                    required property url fileUrl

                    HoverHandler {
                        id: rowHover
                        cursorShape: Qt.PointingHandCursor
                    }
                    TapHandler {
                        onTapped: vaultDialog.currentFolder = rowItem.fileUrl
                    }
                    HoverBorder {
                        anchors.fill: parent
                        anchors.margins: 1
                        active: rowHover.hovered
                    }

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 8
                        spacing: 6

                        Text {
                            text: "📁"
                        }
                        Text {
                            text: rowItem.fileName
                            color: "white"
                        }
                    }
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            Item {
                Layout.fillWidth: true
            }

            Button {
                id: cancelButton
                text: qsTr("Cancel")
                hoverEnabled: true
                onClicked: vaultDialog.close()

                background: Rectangle {
                    radius: AppSettings.borderRadius
                    color: cancelButton.hovered ? "#3a3a3a" : "#2a2a2a"
                    border.width: 1
                    border.color: "#3a3a3a"
                }
                HoverHandler {
                    cursorShape: Qt.PointingHandCursor
                }
                HoverBorder {
                    anchors.fill: parent
                    active: cancelButton.hovered
                }
            }

            Button {
                id: openButton
                text: qsTr("Open")
                // Always enabled while browsing a real folder — opens the
                // currently-browsed folder itself, unlike the native dialog
                // this replaces.
                enabled: vaultDialog.currentFolder.toString() !== ""
                hoverEnabled: true
                onClicked: {
                    vaultDialog.accepted(vaultDialog.currentFolder)
                    vaultDialog.close()
                }

                background: Rectangle {
                    radius: AppSettings.borderRadius
                    color: openButton.hovered ? "#3a3a3a" : "#2a2a2a"
                    opacity: openButton.enabled ? 1 : 0.5
                    border.width: 1
                    border.color: "#3a3a3a"
                }
                HoverHandler {
                    cursorShape: Qt.PointingHandCursor
                    enabled: openButton.enabled
                }
                HoverBorder {
                    anchors.fill: parent
                    active: openButton.enabled && openButton.hovered
                }
            }
        }
    }
}
