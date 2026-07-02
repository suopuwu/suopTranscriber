import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs
import SuopTranscriber

ApplicationWindow {
    id: window
    width: 1024
    height: 720
    visible: true
    title: qsTr("suopTranscriber")

    VaultTreeModel {
        id: vaultModel
    }

    AudioPlayer {
        id: player
    }

    Component.onCompleted: {
        if (initialVaultPath.toString() !== "")
            vaultModel.vaultPath = initialVaultPath
    }

    FolderDialog {
        id: folderDialog
        title: qsTr("Open Vault")
        onAccepted: vaultModel.vaultPath = selectedFolder
    }

    header: ToolBar {
        RowLayout {
            anchors.fill: parent
            anchors.margins: 4

            ToolButton {
                text: qsTr("Open Vault")
                onClicked: folderDialog.open()
            }

            Label {
                Layout.fillWidth: true
                elide: Text.ElideMiddle
                text: vaultModel.vaultPath.toString() === "" ? qsTr("No vault open") : vaultModel.vaultPath.toString()
            }
        }
    }

    SplitView {
        anchors.fill: parent
        orientation: Qt.Horizontal

        Rectangle {
            SplitView.preferredWidth: 320
            SplitView.minimumWidth: 200
            color: window.palette.base

            TreeView {
                id: tree
                anchors.fill: parent
                clip: true
                model: vaultModel

                function expandAll() {
                    // Descending order: expanding a row only inserts rows
                    // after it, so earlier (lower) indices stay valid.
                    for (let r = rows - 1; r >= 0; r--)
                        expandRecursively(r, -1)
                }

                // rows isn't populated synchronously with modelReset — the
                // view's row mapping updates on a later event-loop pass, so
                // poll briefly until it shows up rather than racing it.
                Timer {
                    id: expandRetryTimer
                    interval: 16
                    repeat: true
                    onTriggered: {
                        if (tree.rows > 0) {
                            tree.expandAll()
                            stop()
                        }
                    }
                }

                Connections {
                    target: vaultModel
                    function onModelReset() {
                        expandRetryTimer.start()
                    }
                }

                delegate: Item {
                    id: treeDelegate
                    implicitWidth: tree.width
                    implicitHeight: 28

                    required property TreeView treeView
                    required property int row
                    required property int depth
                    required property bool isTreeNode
                    required property bool expanded
                    required property bool hasChildren
                    required property string name
                    required property string path
                    required property bool isDir
                    required property bool isAudio
                    required property int status

                    TapHandler {
                        onTapped: {
                            if (treeDelegate.isDir) {
                                treeDelegate.treeView.toggleExpanded(treeDelegate.row)
                            } else if (treeDelegate.isAudio) {
                                player.source = "file://" + treeDelegate.path
                                player.play()
                            }
                        }
                    }

                    Row {
                        anchors.verticalCenter: parent.verticalCenter
                        x: treeDelegate.depth * 16 + 6
                        spacing: 6

                        Rectangle {
                            anchors.verticalCenter: parent.verticalCenter
                            visible: treeDelegate.isAudio
                            width: 8
                            height: 8
                            radius: 4
                            color: treeDelegate.status === 2 ? "#2ecc71" : "#f1c40f"
                        }

                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            text: treeDelegate.name
                            color: window.palette.text
                        }
                    }
                }
            }
        }

        Rectangle {
            SplitView.fillWidth: true
            color: window.palette.window

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 16
                spacing: 12

                Label {
                    Layout.fillWidth: true
                    elide: Text.ElideMiddle
                    text: player.source.toString() === "" ? qsTr("No file selected") : player.source.toString()
                }

                RowLayout {
                    spacing: 12

                    Button {
                        text: player.playing ? qsTr("Pause") : qsTr("Play")
                        enabled: player.source.toString() !== ""
                        onClicked: player.togglePlay()
                    }

                    Slider {
                        id: seekSlider
                        Layout.fillWidth: true
                        from: 0
                        to: Math.max(player.duration, 1)
                        value: player.position
                        onMoved: player.position = value
                    }
                }

                Item {
                    Layout.fillHeight: true
                }
            }
        }
    }
}
