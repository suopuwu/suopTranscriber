import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
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

    // Bug report (2026-07-09): closing the window must never leave a
    // transcription silently running in the background — see
    // TranscriptionController::terminateApplicationImmediately()'s doc
    // comment for why this is a hard process exit rather than the normal
    // Qt::quitOnLastWindowClosed path (that still graceful-waits on the
    // same slow-to-interrupt native diarization code this is specifically
    // avoiding). Covers every way the window can close — the custom
    // titlebar's × button, Alt+F4, the native decoration's close button
    // when useSystemTitlebar is on, a window-manager close request — since
    // they all funnel through this ONE signal, not just TitleBar.qml's
    // button.
    onClosing: transcriptionController.terminateApplicationImmediately()

    function openVaultDialog() {
        if (vaultModel.vaultPath.toString() !== "")
            folderDialog.currentFolder = vaultModel.vaultPath
        folderDialog.open()
    }

    // Drag-and-drop import: dropping audio file(s) anywhere on the window
    // copies them into the vault ROOT (never a subfolder, regardless of
    // where over the window the drop lands — see fileDropArea below) via
    // VaultTreeModel::importFile(). Failures (wrong file type, name
    // collision, no vault open) aren't fatal to the whole drop — every
    // dropped file is attempted independently, and any that failed are
    // named in a small auto-clearing banner (this app's established
    // no-toast-system convention, see MainPanel's speakerOpError).
    property string importError: ""
    function importDroppedUrls(urls) {
        if (vaultModel.vaultPath.toString() === "") {
            window.importError = qsTr("Open a vault before dropping in audio files.")
            importErrorClearTimer.restart()
            return
        }
        const failed = []
        for (const url of urls) {
            if (!vaultModel.importFile(url))
                failed.push(url.toString().split("/").pop())
        }
        if (failed.length > 0) {
            window.importError = qsTr("Couldn't import: %1").arg(failed.join(", "))
            importErrorClearTimer.restart()
        }
    }

    Timer {
        id: importErrorClearTimer
        interval: 4000
        onTriggered: window.importError = ""
    }

    // Wave D item 3: reverted to the native/portal dialog (was replaced by
    // VaultFolderDialog.qml in Milestone 4 because its "Open" button is
    // greyed out with nothing explicitly selected when browsing into a leaf
    // folder — confirmed via fresh research this session that there is no
    // portal/KDE config option to change that; it's KFileWidget's own
    // OK-button logic, not exposed as a setting. User chose the native
    // picker back anyway rather than keep the in-app browser as "bloat" —
    // the leaf-folder limitation is a known, accepted tradeoff.
    FolderDialog {
        id: folderDialog
        onAccepted: vaultModel.vaultPath = folderDialog.selectedFolder
    }

    SettingsPopup {
        id: settingsPopup
        parent: window.contentItem
        onChangeVaultRequested: window.openVaultDialog()
        // Combine-rule "test this rule" preview (see its own doc comment in
        // this file) needs both: detectCombineGroups() lives on the
        // controller, the current vault's file listing on the tree model.
        transcriptionController: transcriptionController
        vaultTreeModel: vaultModel
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
            // Item 9 (Wave E): settings are vault-scoped now — load the
            // newly-opened vault's own .suopTranscriber (falling back to
            // defaults for a vault that doesn't have one yet) rather than
            // keep whatever the PREVIOUS vault had in memory.
            if (vaultModel.vaultPath.toString() !== "")
                AppSettings.loadFromVault(vaultModel)
            // Wave N (task system, item 1): re-queues whatever was still
            // Queued/Active in this vault's tasks.json when the app last
            // closed — see TranscriptionController::loadPersistedQueue()'s
            // doc comment for exactly what "resume" means here (the queue
            // survives a restart; no true mid-file resume exists yet).
            if (vaultModel.vaultPath.toString() !== "")
                transcriptionController.loadPersistedQueue(vaultModel.vaultPath)
        }
        // Split-recording auto-combine (see TranscriptionController::
        // findCombineGroups()'s doc comment): modelReset fires from every
        // VaultTreeModel::rebuild() — both the very first one (via
        // onVaultPathChanged -> setVaultPath -> rebuild(), which runs AFTER
        // the loadFromVault() above, so AppSettings.combineRules is already
        // current for a freshly-opened vault) and every later
        // QFileSystemWatcher-triggered rescan (a recorder dropping in a new
        // part) — so this single hook covers "check at startup and at every
        // file addition" without a second bespoke signal.
        function onModelReset() {
            if (vaultModel.vaultPath.toString() === "")
                return
            transcriptionController.autoCombine(AppSettings.combineRules, vaultModel.allAudioPaths(),
                                                 vaultModel.vaultPath, AppSettings.deleteOriginalsAfterCombine)
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

        // Item: drag-and-drop import — dropping OS file(s) anywhere over the
        // window imports them into the vault root (see importDroppedUrls()
        // above). Covers the whole window rather than just Sidebar, matching
        // "anywhere on screen" — the destination is always the vault root
        // regardless of drop position, so there's no benefit to scoping this
        // to a smaller target. z above the panels (but below the resize
        // grips' 1000) purely so its hover highlight paints on top of
        // whichever panel the cursor happens to be over.
        DropArea {
            id: fileDropArea
            anchors.fill: parent
            z: 500
            onEntered: (drag) => { if (!drag.hasUrls) drag.accepted = false }
            onDropped: (drop) => window.importDroppedUrls(drop.urls)

            Rectangle {
                anchors.fill: parent
                visible: fileDropArea.containsDrag
                color: "transparent"
                radius: AppSettings.borderRadius
                border.width: 2
                border.color: "#2ecc71"
            }
        }

        // Sidebar/MainPanel are independently semi-transparent (blur-behind
        // glass) — these fillers sit only behind their divider-facing rounded
        // corners (sized/positioned in updateBlurRegions), not full-bleed,
        // so they don't sit behind the panels' own translucent bodies and
        // defeat the blur-behind effect. Milestone 4 filled these with a
        // fixed dark chrome color ("#1a1a1a") to fix a mismatched-palette-gray
        // square — but a FIXED opaque color still doesn't match either
        // panel's own (user-configurable, and possibly translucent) color —
        // reported again as "squares poking into the panels" whenever
        // transparency is enabled, since a solid patch next to a blurred
        // translucent panel always reads as a mismatch. Each filler now uses
        // the SAME Qt.rgba(color, opacity) formula as the panel it sits next
        // to, so it blends into the same blurred backdrop instead of
        // guessing a fixed shade.
        Rectangle {
            id: cornerFillSidebarTR
            width: AppSettings.borderRadius
            height: AppSettings.borderRadius
            color: Qt.rgba(AppSettings.sidebarBackgroundColor.r, AppSettings.sidebarBackgroundColor.g,
                           AppSettings.sidebarBackgroundColor.b, AppSettings.sidebarOpacity)
        }
        Rectangle {
            id: cornerFillSidebarBR
            width: AppSettings.borderRadius
            height: AppSettings.borderRadius
            color: Qt.rgba(AppSettings.sidebarBackgroundColor.r, AppSettings.sidebarBackgroundColor.g,
                           AppSettings.sidebarBackgroundColor.b, AppSettings.sidebarOpacity)
        }
        Rectangle {
            id: cornerFillMainTL
            width: AppSettings.borderRadius
            height: AppSettings.borderRadius
            color: Qt.rgba(AppSettings.mainBackgroundColor.r, AppSettings.mainBackgroundColor.g,
                           AppSettings.mainBackgroundColor.b, AppSettings.mainOpacity)
        }
        Rectangle {
            id: cornerFillMainBL
            width: AppSettings.borderRadius
            height: AppSettings.borderRadius
            color: Qt.rgba(AppSettings.mainBackgroundColor.r, AppSettings.mainBackgroundColor.g,
                           AppSettings.mainBackgroundColor.b, AppSettings.mainOpacity)
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
                onForceRetranscribeAllRequested: sidebar.forceRetranscribeAll()
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
                    // O7: mirrors BottomBar's currentFilePath binding below —
                    // orphaned .md and normal audio-player views are
                    // mutually exclusive (see MainPanel's showAudioFile()/
                    // showOrphanedMd()), so exactly one of these is ever the
                    // "currently open" path.
                    currentPath: mainPanel.orphanedMdPath !== "" ? mainPanel.orphanedMdPath
                                 : (mainPanel.player && mainPanel.player.source.toString() !== ""
                                    ? mainPanel.player.source.toString().replace("file://", "") : "")
                    onFileActivated: (path) => mainPanel.showAudioFile(path)
                    onOrphanedMdActivated: (path) => mainPanel.showOrphanedMd(path)
                    // Item 2 (Wave E): open the file (or orphaned .md) then
                    // jump to/highlight the phrase the sidebar snippet
                    // matched on, in MainPanel's own search mechanism.
                    onTranscriptMatchActivated: (path, isOrphanedMd, query) => {
                        if (isOrphanedMd)
                            mainPanel.showOrphanedMd(path)
                        else
                            mainPanel.showAudioFile(path)
                        mainPanel.searchExpanded = true
                        mainPanel.searchQuery = query
                        mainPanel.jumpToNextMatch()
                    }
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
                    onTagActivated: (tag) => sidebar.applyTagFilter(tag)
                    onWidthChanged: Qt.callLater(window.updateBlurRegions)
                    onHeightChanged: Qt.callLater(window.updateBlurRegions)
                }
            }

            BottomBar {
                id: bottomBar
                Layout.fillWidth: true
                vaultPath: vaultModel.vaultPath
                vaultLocalPath: vaultModel.vaultLocalPath
                vaultTreeModel: vaultModel
                currentFilePath: mainPanel.orphanedMdPath !== "" ? mainPanel.orphanedMdPath
                                 : (mainPanel.player.source.toString() !== ""
                                    ? mainPanel.player.source.toString().replace("file://", "") : "")
                transcriptionController: transcriptionController
                // Item 4/P28: same AudioPlayer instance MainPanel already
                // plays through — only used for a read-only position/
                // duration readout in the bottom bar.
                player: player
                onSettingsRequested: settingsPopup.open()
                onTaskActivated: (path) => mainPanel.showAudioFile(path)
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
        // TitleBar's existing startSystemMove() pattern. Reparented onto
        // Overlay.overlay (same coordinate space as rootContent, since
        // Overlay.overlay fills the ApplicationWindow's contentItem) with an
        // explicit z above SettingsPopup's own block-plane/content, so they
        // keep painting/receiving input above it while settingsPopup's
        // `modal: true` blocks everything else in the window. System-
        // titlebar mode already gets OS resize borders for free, so these
        // are frameless-only.
        component ResizeGrip: Item {
            id: grip
            required property int edges
            parent: Overlay.overlay
            z: 1000
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

        // Drag-and-drop import failure feedback — same reparent-onto-
        // Overlay.overlay + high-z treatment as ResizeGrip above, so it
        // floats above every panel regardless of where the drop happened.
        // Auto-clears via importErrorClearTimer; success needs no banner
        // since the imported file simply appears in the sidebar tree.
        Label {
            parent: Overlay.overlay
            z: 1001
            anchors.top: parent.top
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.topMargin: 12
            visible: window.importError !== ""
            text: window.importError
            color: "white"
            padding: 8
            background: Rectangle {
                color: "#c0392b"
                radius: AppSettings.borderRadius
            }
        }
    }
}
