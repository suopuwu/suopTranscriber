import QtQuick
import QtQuick.Controls
import SuopTranscriber

Rectangle {
    id: sidebar

    property alias model: tree.model
    signal fileActivated(string path)

    color: Qt.rgba(AppSettings.sidebarBackgroundColor.r, AppSettings.sidebarBackgroundColor.g,
                    AppSettings.sidebarBackgroundColor.b, AppSettings.sidebarOpacity)
    // Only the corners touching the SplitView divider are rounded; the
    // window-edge corners are left square so the outer window mask (which
    // already rounds the true window corners) isn't duplicated here.
    topRightRadius: AppSettings.borderRadius
    bottomRightRadius: AppSettings.borderRadius

    TreeView {
        id: tree
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        clip: true

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
            target: tree.model
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

            HoverHandler {
                id: rowHover
                cursorShape: Qt.PointingHandCursor
            }

            TapHandler {
                onTapped: {
                    if (treeDelegate.isDir) {
                        treeDelegate.treeView.toggleExpanded(treeDelegate.row)
                    } else if (treeDelegate.isAudio) {
                        sidebar.fileActivated(treeDelegate.path)
                    }
                }
            }

            HoverBorder {
                anchors.fill: parent
                anchors.margins: 1
                active: rowHover.hovered
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
                    color: "white"
                }
            }
        }
    }
}
