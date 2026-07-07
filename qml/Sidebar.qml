import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SuopTranscriber

Rectangle {
    id: sidebar

    property alias model: tree.model
    property TranscriptionController transcriptionController
    property url vaultPath
    signal fileActivated(string path)
    signal orphanedMdActivated(string path)

    property string searchQuery: ""
    // Transcriptions are searched by default (labels/tags always are); the
    // checkbox lets the user EXCLUDE them, e.g. for a quick label-only scan.
    property bool searchIncludeTranscriptions: true
    property bool searchExpanded: false

    // path -> true, populated/depopulated by delegates as they realize/
    // destroy — used on-demand by "transcribe all" (item 15) to avoid
    // needing raw numeric model roles from QML for a manual tree walk.
    property var untranscribedPaths: ({})
    function setBulkEligible(path, eligible) {
        if (eligible)
            sidebar.untranscribedPaths[path] = true
        else
            delete sidebar.untranscribedPaths[path]
    }

    color: Qt.rgba(AppSettings.sidebarBackgroundColor.r, AppSettings.sidebarBackgroundColor.g,
                    AppSettings.sidebarBackgroundColor.b, AppSettings.sidebarOpacity)
    // Only the corners touching the SplitView divider are rounded; the
    // window-edge corners are left square so the outer window mask (which
    // already rounds the true window corners) isn't duplicated here.
    topRightRadius: AppSettings.borderRadius
    bottomRightRadius: AppSettings.borderRadius

    Shortcut {
        sequence: "Ctrl+F"
        enabled: sidebar.activeFocus || tree.activeFocus
        onActivated: {
            sidebar.searchExpanded = true
            searchField.forceActiveFocus()
        }
    }

    Item {
        id: ribbon
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        height: 32

        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 6
            anchors.rightMargin: 6
            spacing: 4
            visible: !sidebar.searchExpanded

            Item {
                id: transcribeAllButton
                Layout.preferredWidth: 22
                Layout.preferredHeight: 22

                HoverHandler {
                    id: transcribeAllHover
                    cursorShape: Qt.PointingHandCursor
                }
                TapHandler {
                    onTapped: {
                        if (!sidebar.transcriptionController)
                            return
                        const paths = Object.keys(sidebar.untranscribedPaths)
                        if (paths.length === 0)
                            return
                        // Model paths are raw local paths, not file:// URLs
                        // (see VaultTreeModel's PathRole) — Q_INVOKABLE
                        // QUrl params need the scheme or QUrl::toLocalFile()
                        // silently returns empty.
                        const urls = paths.map((p) => "file://" + p)
                        sidebar.transcriptionController.transcribeAll(urls, sidebar.vaultPath, AppSettings.lastModel,
                                                                        false, AppSettings.autoLabelEnabled)
                    }
                }
                HoverBorder {
                    anchors.fill: parent
                    active: transcribeAllHover.hovered
                }
                Text {
                    anchors.centerIn: parent
                    text: "⤒"
                    color: "#dddddd"
                    font.pixelSize: 15
                }

                ToolTip.visible: transcribeAllHover.hovered
                ToolTip.text: qsTr("Transcribe all untranscribed audio")
                ToolTip.delay: 500
            }

            Item {
                id: searchToggleButton
                Layout.preferredWidth: 22
                Layout.preferredHeight: 22

                HoverHandler {
                    id: searchToggleHover
                    cursorShape: Qt.PointingHandCursor
                }
                TapHandler {
                    onTapped: {
                        sidebar.searchExpanded = true
                        searchField.forceActiveFocus()
                    }
                }
                HoverBorder {
                    anchors.fill: parent
                    active: searchToggleHover.hovered
                }
                Text {
                    anchors.centerIn: parent
                    text: "⌕"
                    color: "#dddddd"
                    font.pixelSize: 15
                }
            }

            Item {
                Layout.fillWidth: true
            }
        }

        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 6
            anchors.rightMargin: 6
            spacing: 6
            visible: sidebar.searchExpanded

            TextField {
                id: searchField
                Layout.fillWidth: true
                Layout.preferredHeight: 24
                placeholderText: qsTr("Search labels & tags…")
                text: sidebar.searchQuery
                onTextChanged: sidebar.searchQuery = text

                HoverHandler {
                    cursorShape: Qt.IBeamCursor
                }
            }

            CheckBox {
                id: includeTranscriptionsBox
                text: qsTr("Search transcriptions")
                checked: sidebar.searchIncludeTranscriptions
                onToggled: sidebar.searchIncludeTranscriptions = checked

                HoverHandler {
                    cursorShape: Qt.PointingHandCursor
                }
            }

            Item {
                id: searchCloseButton
                Layout.preferredWidth: 20
                Layout.preferredHeight: 20

                HoverHandler {
                    id: searchCloseHover
                    cursorShape: Qt.PointingHandCursor
                }
                TapHandler {
                    onTapped: {
                        searchField.text = ""
                        sidebar.searchQuery = ""
                        sidebar.searchExpanded = false
                    }
                }
                HoverBorder {
                    anchors.fill: parent
                    active: searchCloseHover.hovered
                }
                Text {
                    anchors.centerIn: parent
                    text: "×"
                    color: "#dddddd"
                    font.pixelSize: 15
                }
            }
        }
    }

    TreeView {
        id: tree
        anchors.top: ribbon.bottom
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
            required property bool isOrphanedMd

            readonly property bool bulkEligible: treeDelegate.isAudio && treeDelegate.status !== 2
            onBulkEligibleChanged: sidebar.setBulkEligible(treeDelegate.path, treeDelegate.bulkEligible)
            Component.onCompleted: sidebar.setBulkEligible(treeDelegate.path, treeDelegate.bulkEligible)
            Component.onDestruction: sidebar.setBulkEligible(treeDelegate.path, false)

            readonly property var metadata: (treeDelegate.isAudio && sidebar.transcriptionController)
                ? sidebar.transcriptionController.readMetadata("file://" + treeDelegate.path) : ({})
            readonly property bool matchesSearch: {
                if (sidebar.searchQuery === "")
                    return true
                const q = sidebar.searchQuery.toLowerCase()
                if (treeDelegate.name.toLowerCase().indexOf(q) !== -1)
                    return true
                const label = treeDelegate.metadata && treeDelegate.metadata.label ? String(treeDelegate.metadata.label).toLowerCase() : ""
                if (label.indexOf(q) !== -1)
                    return true
                const tags = treeDelegate.metadata && treeDelegate.metadata.tags ? String(treeDelegate.metadata.tags).toLowerCase() : ""
                if (tags.indexOf(q) !== -1)
                    return true
                if (sidebar.searchIncludeTranscriptions && treeDelegate.status === 2 && sidebar.transcriptionController) {
                    const body = sidebar.transcriptionController.readTranscriptBody("file://" + treeDelegate.path).toLowerCase()
                    if (body.indexOf(q) !== -1)
                        return true
                }
                return false
            }

            opacity: treeDelegate.matchesSearch ? 1 : 0.3

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
                    } else if (treeDelegate.isOrphanedMd) {
                        sidebar.orphanedMdActivated(treeDelegate.path)
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

                Text {
                    // Fixed-width wrapper: always reserves space so file
                    // rows and dir rows stay aligned, even though the
                    // glyph itself only renders for expandable dirs.
                    anchors.verticalCenter: parent.verticalCenter
                    width: 14
                    horizontalAlignment: Text.AlignHCenter
                    text: (treeDelegate.isDir && treeDelegate.hasChildren)
                          ? (treeDelegate.expanded ? "−" : "+")
                          : ""
                    color: "#aaaaaa"
                }

                Rectangle {
                    anchors.verticalCenter: parent.verticalCenter
                    visible: treeDelegate.isAudio || treeDelegate.isOrphanedMd
                    width: 8
                    height: 8
                    radius: 4
                    color: treeDelegate.status === 2 ? "#2ecc71" : (treeDelegate.isOrphanedMd ? "#3498db" : "#f1c40f")
                }

                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: {
                        if (treeDelegate.isDir || !AppSettings.hideExtensionsSidebar)
                            return treeDelegate.name
                        const dot = treeDelegate.name.lastIndexOf(".")
                        return dot > 0 ? treeDelegate.name.substring(0, dot) : treeDelegate.name
                    }
                    color: "white"
                }
            }

            Item {
                id: rowTranscribeButton
                anchors.right: parent.right
                anchors.rightMargin: 8
                anchors.verticalCenter: parent.verticalCenter
                width: 20
                height: 20
                opacity: (rowHover.hovered && treeDelegate.bulkEligible) ? 1 : 0
                enabled: opacity > 0

                Behavior on opacity {
                    NumberAnimation { duration: 120 }
                }

                ToolTip.visible: rowTranscribeHover.hovered
                ToolTip.text: qsTr("Transcribe")
                ToolTip.delay: 500

                HoverHandler {
                    id: rowTranscribeHover
                    cursorShape: Qt.PointingHandCursor
                    enabled: rowTranscribeButton.enabled
                }
                TapHandler {
                    enabled: rowTranscribeButton.enabled
                    onTapped: sidebar.transcriptionController.transcribe(
                                  "file://" + treeDelegate.path, sidebar.vaultPath, AppSettings.lastModel, false,
                                  AppSettings.autoLabelEnabled)
                }
                HoverBorder {
                    anchors.fill: parent
                    active: rowTranscribeButton.enabled && rowTranscribeHover.hovered
                }
                Text {
                    anchors.centerIn: parent
                    text: "⤒"
                    color: "#dddddd"
                    font.pixelSize: 13
                }
            }
        }
    }
}
