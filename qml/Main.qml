import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SuopTranscriber

ApplicationWindow {
    id: window
    width: 1024
    height: 720
    visible: true
    color: "transparent"
    title: qsTr("Suop Transcriber")
    flags: AppSettings.useSystemTitlebar ? Qt.Window : (Qt.Window | Qt.FramelessWindowHint)

    VaultTreeModel {
        id: vaultModel
    }

    AudioPlayer {
        id: player
    }

    TranscriptionController {
        id: transcriptionController
    }

    function openVaultDialog() {
        if (vaultModel.vaultPath.toString() !== "")
            folderDialog.currentFolder = vaultModel.vaultPath
        folderDialog.open()
    }

    VaultFolderDialog {
        id: folderDialog
        parent: window.contentItem
        onAccepted: (folder) => vaultModel.vaultPath = folder
    }

    SettingsPopup {
        id: settingsPopup
        parent: window.contentItem
        onChangeVaultRequested: window.openVaultDialog()
    }

    function updateBlurRegions() {
        if (!sidebar || !mainPanel || !window.contentItem)
            return
        const sidebarRect = sidebar.mapToItem(window.contentItem, 0, 0, sidebar.width, sidebar.height)
        const mainRect = mainPanel.mapToItem(window.contentItem, 0, 0, mainPanel.width, mainPanel.height)
        WindowEffects.updateBlur(window,
            Qt.rect(sidebarRect.x, sidebarRect.y, sidebarRect.width, sidebarRect.height), AppSettings.sidebarBlur,
            Qt.rect(mainRect.x, mainRect.y, mainRect.width, mainRect.height), AppSettings.mainBlur)

        // Sidebar/MainPanel only round the corners touching the SplitView
        // divider; those cutouts need a filler behind them (matching the
        // divider color) or the desktop shows through. Sized/positioned to
        // exactly the rounded corner itself so it never shows behind the
        // panels' own (independently opaque/blurred) bodies.
        const r = AppSettings.borderRadius
        cornerFillSidebarTR.x = sidebarRect.x + sidebarRect.width - r
        cornerFillSidebarTR.y = sidebarRect.y
        cornerFillSidebarBR.x = sidebarRect.x + sidebarRect.width - r
        cornerFillSidebarBR.y = sidebarRect.y + sidebarRect.height - r
        cornerFillMainTL.x = mainRect.x
        cornerFillMainTL.y = mainRect.y
        cornerFillMainBL.x = mainRect.x
        cornerFillMainBL.y = mainRect.y + mainRect.height - r
    }

    Connections {
        target: AppSettings
        function onSidebarBlurChanged() { updateBlurRegions() }
        function onMainBlurChanged() { updateBlurRegions() }
        function onBorderRadiusChanged() { Qt.callLater(updateBlurRegions) }

        function onUseSystemTitlebarChanged() {
            // Wayland doesn't redecorate an already-shown window in place;
            // force a hide/show cycle so the new frameless/framed flags
            // actually take effect (window position isn't persisted — x/y
            // is unreadable/unsettable on Wayland anyway).
            const w = window.width
            const h = window.height
            window.visible = false
            titlebarNudgeTimer.savedW = w
            titlebarNudgeTimer.savedH = h
            titlebarNudgeTimer.start()
        }
    }

    Timer {
        id: titlebarNudgeTimer
        property int savedW: 0
        property int savedH: 0
        interval: 50
        onTriggered: {
            window.visible = true
            window.width = savedW
            window.height = savedH
        }
    }

    onWidthChanged: Qt.callLater(updateBlurRegions)
    onHeightChanged: Qt.callLater(updateBlurRegions)

    Connections {
        target: vaultModel
        function onVaultPathChanged() {
            AppSettings.lastVaultPath = vaultModel.vaultPath
        }
    }

    Component.onCompleted: {
        if (initialVaultPath.toString() !== "")
            vaultModel.vaultPath = initialVaultPath
        else if (AppSettings.lastVaultPath.toString() !== "")
            vaultModel.vaultPath = AppSettings.lastVaultPath
        Qt.callLater(updateBlurRegions)
    }

    // Outer window corners are rounded WITHOUT an OpacityMask/layer effect:
    // TitleBar and BottomBar round their own top/bottom corners natively
    // (Rectangle.topLeftRadius etc.), and Sidebar/MainPanel's side edges are
    // already straight by design — so the four true window corners come out
    // rounded from native per-corner radii alone. (A prior OpacityMask-based
    // approach rounded only the right two corners and left the left two
    // square — a reproducible Qt5Compat.GraphicalEffects rendering quirk on
    // this Qt/RHI combination; native per-corner Rectangle radius sidesteps
    // it entirely instead of chasing the shader bug.)
    Item {
        id: rootContent
        anchors.fill: parent

        // Sidebar/MainPanel are independently semi-transparent (blur-behind
        // glass) — these fillers sit only behind their divider-facing rounded
        // corners (sized/positioned in updateBlurRegions), not full-bleed,
        // so they don't sit behind the panels' own translucent bodies and
        // defeat the blur-behind effect. Filled with the app's own dark
        // chrome color (matching TitleBar/BottomBar) rather than a generic
        // palette color, which didn't match anything and looked like a
        // stray colored square poking through (Milestone 4 Task 6).
        Rectangle {
            id: cornerFillSidebarTR
            width: AppSettings.borderRadius
            height: AppSettings.borderRadius
            color: "#1a1a1a"
        }
        Rectangle {
            id: cornerFillSidebarBR
            width: AppSettings.borderRadius
            height: AppSettings.borderRadius
            color: "#1a1a1a"
        }
        Rectangle {
            id: cornerFillMainTL
            width: AppSettings.borderRadius
            height: AppSettings.borderRadius
            color: "#1a1a1a"
        }
        Rectangle {
            id: cornerFillMainBL
            width: AppSettings.borderRadius
            height: AppSettings.borderRadius
            color: "#1a1a1a"
        }

        ColumnLayout {
            anchors.fill: parent
            spacing: 0

            TitleBar {
                id: customTitleBar
                Layout.fillWidth: true
                window: window
                title: window.title
                visible: !AppSettings.useSystemTitlebar
                onSettingsRequested: settingsPopup.open()
            }

            SplitView {
                Layout.fillWidth: true
                Layout.fillHeight: true
                orientation: Qt.Horizontal

                Sidebar {
                    id: sidebar
                    SplitView.preferredWidth: 320
                    SplitView.minimumWidth: 200
                    model: vaultModel
                    transcriptionController: transcriptionController
                    vaultPath: vaultModel.vaultPath
                    onFileActivated: (path) => mainPanel.showAudioFile(path)
                    onOrphanedMdActivated: (path) => mainPanel.showOrphanedMd(path)
                    onWidthChanged: Qt.callLater(window.updateBlurRegions)
                    onHeightChanged: Qt.callLater(window.updateBlurRegions)
                }

                MainPanel {
                    id: mainPanel
                    SplitView.fillWidth: true
                    vaultPath: vaultModel.vaultPath
                    vaultLocalPath: vaultModel.vaultLocalPath
                    player: player
                    transcriptionController: transcriptionController
                    vaultTreeModel: vaultModel
                    onOpenVaultRequested: window.openVaultDialog()
                    onWidthChanged: Qt.callLater(window.updateBlurRegions)
                    onHeightChanged: Qt.callLater(window.updateBlurRegions)
                }
            }

            BottomBar {
                id: bottomBar
                Layout.fillWidth: true
                vaultPath: vaultModel.vaultPath
                vaultLocalPath: vaultModel.vaultLocalPath
                transcriptionController: transcriptionController
                onSettingsRequested: settingsPopup.open()
            }
        }

        // Adjustable "rim" — thin translucent-white glass edge by default.
        // Only meaningful in frameless mode (a system-decorated window's
        // border is the compositor's concern, not ours). Border strokes are
        // drawn fully inside a Rectangle's own bounds, so this never bleeds
        // past the true window edge.
        Rectangle {
            anchors.fill: parent
            color: "transparent"
            radius: AppSettings.borderRadius
            border.width: AppSettings.rimWidth
            border.color: AppSettings.rimColor
            visible: AppSettings.rimEnabled && !AppSettings.useSystemTitlebar
        }

        // Frameless mode has no OS-drawn resize border, so nothing lets the
        // user resize the window (Qt::FramelessWindowHint removes it and
        // nothing replaced it) — these thin edge/corner grips call the same
        // startSystemResize() the compositor would normally drive, mirroring
        // TitleBar's existing startSystemMove() pattern. Declared last so
        // they paint/receive input on top of TitleBar/Sidebar/MainPanel/
        // BottomBar's own outermost pixels (e.g. the top strip overlaps
        // TitleBar's drag-to-move area — a resize takes priority right at
        // the true window edge). System-titlebar mode already gets OS resize
        // borders for free, so these are frameless-only.
        component ResizeGrip: Item {
            id: grip
            required property int edges
            visible: !AppSettings.useSystemTitlebar
            enabled: visible

            HoverHandler {
                cursorShape: {
                    const hasTop = (grip.edges & Qt.TopEdge) !== 0
                    const hasBottom = (grip.edges & Qt.BottomEdge) !== 0
                    const hasLeft = (grip.edges & Qt.LeftEdge) !== 0
                    const hasRight = (grip.edges & Qt.RightEdge) !== 0
                    if ((hasTop && hasLeft) || (hasBottom && hasRight))
                        return Qt.SizeFDiagCursor
                    if ((hasTop && hasRight) || (hasBottom && hasLeft))
                        return Qt.SizeBDiagCursor
                    return (hasLeft || hasRight) ? Qt.SizeHorCursor : Qt.SizeVerCursor
                }
            }
            DragHandler {
                target: null
                onActiveChanged: if (active) window.startSystemResize(grip.edges)
            }
        }

        ResizeGrip { // top
            edges: Qt.TopEdge
            anchors.top: parent.top; anchors.left: parent.left; anchors.right: parent.right
            anchors.leftMargin: 8; anchors.rightMargin: 8
            height: 4
        }
        ResizeGrip { // bottom
            edges: Qt.BottomEdge
            anchors.bottom: parent.bottom; anchors.left: parent.left; anchors.right: parent.right
            anchors.leftMargin: 8; anchors.rightMargin: 8
            height: 4
        }
        ResizeGrip { // left
            edges: Qt.LeftEdge
            anchors.left: parent.left; anchors.top: parent.top; anchors.bottom: parent.bottom
            anchors.topMargin: 8; anchors.bottomMargin: 8
            width: 4
        }
        ResizeGrip { // right
            edges: Qt.RightEdge
            anchors.right: parent.right; anchors.top: parent.top; anchors.bottom: parent.bottom
            anchors.topMargin: 8; anchors.bottomMargin: 8
            width: 4
        }
        ResizeGrip { // top-left
            edges: Qt.TopEdge | Qt.LeftEdge
            anchors.top: parent.top; anchors.left: parent.left
            width: 8; height: 8
        }
        ResizeGrip { // top-right
            edges: Qt.TopEdge | Qt.RightEdge
            anchors.top: parent.top; anchors.right: parent.right
            width: 8; height: 8
        }
        ResizeGrip { // bottom-left
            edges: Qt.BottomEdge | Qt.LeftEdge
            anchors.bottom: parent.bottom; anchors.left: parent.left
            width: 8; height: 8
        }
        ResizeGrip { // bottom-right
            edges: Qt.BottomEdge | Qt.RightEdge
            anchors.bottom: parent.bottom; anchors.right: parent.right
            width: 8; height: 8
        }
    }
}
