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
    // Base title plus a " - <Page>" suffix for whichever top-level page is
    // open (priority matches MainPanel's page precedence: recording >
    // usbGrabber > benchmark). The custom TitleBar mirrors this via
    // `title: window.title`, so both update together. mainPanel is declared
    // later in this file — a forward id reference in a binding resolves lazily.
    title: qsTr("Suop Transcriber")
           + (mainPanel.recording ? qsTr(" - Recorder")
              : mainPanel.usbGrabberOpen ? qsTr(" - USB Grabber")
              : mainPanel.benchmarkOpen ? qsTr(" - Benchmark") : "")
    flags: AppSettings.useSystemTitlebar ? Qt.Window : (Qt.Window | Qt.FramelessWindowHint)

    VaultTreeModel {
        id: vaultModel
    }

    AudioPlayer {
        id: player
    }

    AudioRecorder {
        id: audioRecorder
    }

    TranscriptionController {
        id: transcriptionController
        // So a trash/delete/restore task can call back to the model on the GUI
        // thread (applyRemovalOutcome/applyRestoreOutcome) to rebuild the tree +
        // emit the per-file delete/restore signals MainPanel reacts to — the
        // model owns the orphaned-vs-audio classification. Same wiring as
        // UsbGrabberController's vaultTreeModel binding.
        vaultTreeModel: vaultModel
    }

    // Auto undo-snackbar for ANY undoable task (Task::undoLabel/makeUndoTask) —
    // rename today, move/trash later — so an undoable op gets consistent undo
    // UI for free, no per-kind QML. Undo just enqueues the inverse task (which
    // is itself reported and re-undoable).
    Connections {
        target: transcriptionController
        function onUndoAvailable(taskId, label) {
            Notifications.post({
                text: label,
                kind: "info",
                // Dropped while the Tasks popup is open — that popup already
                // lists this task and carries its own per-row Undo.
                taskRelated: true,
                actions: [{
                    label: qsTr("Undo"),
                    onTriggered: function() { transcriptionController.undoTask(taskId) }
                }]
            })
        }
    }

    UsbGrabberController {
        id: usbGrabberController
        vaultTreeModel: vaultModel
        // Grabs are queued tasks on the SAME controller everything else uses,
        // so they share one queue (and one disk) with transcribes/imports
        // rather than racing them — see UsbGrabberController's class doc.
        transcriptionController: transcriptionController
        // So a grab skips parts whose split-recording group was already
        // combined (persisted per-vault, unlike the C++-side cache) — see
        // UsbGrabJob.h's runUsbGrabScan() doc comment.
        combineRules: AppSettings.combineRules
        // The auto-combine after a CLEAN auto-grab batch runs in C++ now
        // (UsbGrabberController::maybeAutoCombineAfterGrab), gated on these two —
        // bound live like combineRules so the decision reads the same persisted
        // settings the manual combine path does.
        autoCombineAfterGrab: AppSettings.autoCombineAfterGrab
        deleteOriginalsAfterCombine: AppSettings.deleteOriginalsAfterCombine
    }

    // A grab is invisible unless the USB Grabber page happens to be open — and
    // an AUTO grab is invisible by definition, since the user didn't ask for
    // it. That's exactly what a snackbar is for ("post only what is otherwise
    // silent", StyleGuide.md). Cancelled grabs never reach here (the controller
    // doesn't emit for them — a partial tally isn't a result), and the Tasks
    // row already tells that story.
    Connections {
        target: usbGrabberController

        function onGrabFinished(deviceSerial, filesCopied, filesSkipped, errors, autoTriggered) {
            if (errors.length > 0) {
                Notifications.error(qsTr("Grab finished with %n problem(s): %1", "", errors.length)
                                    .arg(errors.join("; ")))
                return
            }
            // A manual grab that copied nothing is a silent no-op the user is
            // watching for a result of — worth saying. An auto grab that copied
            // nothing is the common, boring case every time an already-emptied
            // device is replugged; announcing it would train the user to ignore
            // the corner the interesting messages land in.
            if (filesCopied === 0) {
                if (!autoTriggered)
                    Notifications.info(qsTr("Nothing new to grab (%n file(s) already in the vault).", "",
                                            filesSkipped))
                return
            }
            Notifications.info(autoTriggered
                               ? qsTr("Auto-grabbed %n new file(s) from USB.", "", filesCopied)
                               : qsTr("Grabbed %n new file(s).", "", filesCopied))
        }

        // The auto-combine itself runs in C++ now (UsbGrabberController::
        // maybeAutoCombineAfterGrab, fired once per CLEAN auto-grab batch after
        // the LAST auto-grab leaves the queue, and gated on a batch that wasn't
        // cancelled/aborted/errored — an aborted grab can leave a truncated part
        // set, and combining that would drop a recording's tail). All that
        // remains here is announcing it: the one combine the user didn't click,
        // which moves their original parts into .combined/, must never happen
        // with no account of itself.
        function onAutoCombineStarted(groupCount) {
            Notifications.info(qsTr("Auto-combining %n recording group(s) after grab.", "", groupCount))
        }
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
    onClosing: {
        // Flush any debounced settings save before the hard ::_exit(0) below —
        // that exit never lets AppSettings' saveTimer fire, so a setting changed
        // in the last ~250ms before closing would otherwise be lost.
        AppSettings.flushPendingSave()
        transcriptionController.terminateApplicationImmediately()
    }

    function openVaultDialog() {
        if (vaultModel.vaultPath.toString() !== "")
            folderDialog.currentFolder = vaultModel.vaultPath
        folderDialog.open()
    }

    // Drag-and-drop import: dropping audio file(s) anywhere on the window
    // copies them into the vault ROOT (never a subfolder, regardless of
    // where over the window the drop lands — see fileDropArea below). Item
    // 37: the copy is now ASYNC — each file is validated synchronously
    // (VaultTreeModel::inspectImportCandidate(), instant feedback for wrong
    // type / collision / no vault) then its byte-copy is enqueued as an
    // "import" task on TranscriptionController's worker thread
    // (importAudio()), so dropping a large file no longer freezes the UI and
    // every import appears in the Tasks queue. Validation failures aren't
    // fatal to the whole drop — every dropped file is attempted
    // independently, and any that failed validation are named in a snackbar
    // (StyleGuide.md "Snackbars"); a copy that fails on the worker surfaces
    // as a Failed row in the Tasks popup instead. A NAME COLLISION is not
    // treated as a hard failure — it routes to a separate sticky
    // Overwrite/Skip snackbar instead of the plain error one.
    function importDroppedUrls(urls) {
        if (vaultModel.vaultPath.toString() === "") {
            Notifications.error(qsTr("Open a vault before dropping in audio files."))
            return
        }
        const failed = []
        const collisions = []   // [{ url, dest, name }]
        for (const url of urls) {
            const v = vaultModel.inspectImportCandidate(url)
            if (v.reason === "ok")
                transcriptionController.importAudio(url, "file://" + v.dest, vaultModel.vaultPath)
            else if (v.reason === "collision")
                collisions.push({ url: url, dest: v.dest, name: v.name })
            else
                failed.push(v.name)
        }
        if (failed.length > 0)
            Notifications.error(qsTr("Couldn't import: %1").arg(failed.join(", ")))
        if (collisions.length > 0) {
            // Sticky (timeout: 0) — the user must choose. NOT taskRelated: this
            // is a pre-enqueue prompt, not a duplicate of a Tasks-popup row, so
            // it must survive with the popup open.
            Notifications.post({
                text: qsTr("%n file(s) already in the vault.", "", collisions.length),
                kind: "warning",
                timeout: 0,
                actions: [{
                    label: qsTr("Overwrite"),
                    onTriggered: () => {
                        for (const c of collisions)
                            transcriptionController.importAudio(c.url, "file://" + c.dest, vaultModel.vaultPath, true)
                    }
                }, {
                    label: qsTr("Skip"),
                    onTriggered: () => {}   // Notifications.trigger() dismisses after the action
                }]
            })
        }
    }

    // Reverted to the native/portal dialog (was replaced by
    // VaultFolderDialog.qml because its "Open" button is
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
            Qt.rect(sidebarRect.x, sidebarRect.y, sidebarRect.width, sidebarRect.height), AppSettings.sidebarBlur && AppSettings.animationsEnabled,
            Qt.rect(mainRect.x, mainRect.y, mainRect.width, mainRect.height), AppSettings.mainBlur && AppSettings.animationsEnabled)

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
        function onAnimationsEnabledChanged() { updateBlurRegions() }
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

    // Ctrl+Y => redo inside whichever text box has focus. Window-wide (one
    // ApplicationWindow, so the default Qt.WindowShortcut context covers
    // every field in every page and popup) — see TextRedoShortcut.qml for why
    // Qt doesn't already give us this chord on Linux.
    TextRedoShortcut {}

    onWidthChanged: Qt.callLater(updateBlurRegions)
    onHeightChanged: Qt.callLater(updateBlurRegions)

    Connections {
        target: vaultModel
        function onVaultPathChanged() {
            AppSettings.lastVaultPath = vaultModel.vaultPath
            // Settings are vault-scoped now — load the
            // newly-opened vault's own .transcriber/settings.json (falling back to
            // defaults for a vault that doesn't have one yet) rather than
            // keep whatever the PREVIOUS vault had in memory.
            if (vaultModel.vaultPath.toString() !== "")
                AppSettings.loadFromVault(vaultModel)
            // Re-queues whatever was still
            // Queued/Active in this vault's tasks.json when the app last
            // closed — see TranscriptionController::loadPersistedQueue()'s
            // doc comment for exactly what "resume" means here (the queue
            // survives a restart; no true mid-file resume exists yet).
            if (vaultModel.vaultPath.toString() !== "")
                transcriptionController.loadPersistedQueue(vaultModel.vaultPath)
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

        // A left-click that lands while a TEXT FIELD holds keyboard focus
        // drops that focus (its own onActiveFocusChanged then commits it),
        // matching the platform expectation that clicking off a field
        // deselects it. Gated on a text field actually being focused —
        // duck-typed on cursorPosition (only TextInput/TextEdit expose it),
        // the same idiom as Sidebar's textFieldHasFocus — because this
        // handler spans the whole window and, being a passive TapHandler,
        // ALSO fires when a deeper passive handler already handled the same
        // tap. An unconditional forceActiveFocus() here therefore clobbered a
        // sidebar tree row's own focus grab the instant it happened, leaving
        // tree.activeFocus false and killing Up/Down note navigation. The gate
        // makes it a no-op unless there's genuinely a text editor to blur, so
        // it can never steal focus a row/button/tree deliberately took.
        //
        // Same hazard, one step worse for the sidebar's INLINE RENAME: the very
        // click that opens the rename editor leaves that editor (a text field)
        // focused, so this passive handler fired on that same tap and blurred it
        // instantly — the editor's blur-commit then closed it. The rename could
        // not stick until a click's handler ordering happened to spare it, so it
        // took several clicks. Gating on sidebar.renamingPath === "" leaves an
        // open rename alone (beginRename sets renamingPath synchronously during
        // the tap, before this passive handler runs).
        // Deliberately an Item-attached TapHandler, NOT a MouseArea: it carries
        // no implicit cursor, so it can span the whole window without poisoning
        // the I-beam/pointer cursor resolution of everything above it (see
        // MainPanel's own deselect TapHandler and the MouseArea cursor-bleed
        // notes). DragThreshold policy means a click-drag (a text selection, a
        // slider, a window move) is not treated as a tap, so it never blurs
        // mid-gesture. The DropArea below only handles drag-drop, never a
        // press, so it does not intercept this.
        TapHandler {
            acceptedButtons: Qt.LeftButton
            onTapped: {
                // `window` is the root ApplicationWindow; activeFocusItem is a
                // real QQuickWindow property, so read it off the window rather
                // than via the Window attached type (which doesn't resolve on a
                // non-Item handler like this one).
                const fi = window.activeFocusItem
                if (fi && fi.cursorPosition !== undefined && sidebar.renamingPath === "")
                    rootContent.forceActiveFocus()
            }
        }

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
                border.color: Theme.success
            }
        }

        // Sidebar/MainPanel are independently semi-transparent (blur-behind
        // glass) — these fillers sit only behind their divider-facing rounded
        // corners (sized/positioned in updateBlurRegions), not full-bleed,
        // so they don't sit behind the panels' own translucent bodies and
        // defeat the blur-behind effect. These were once filled with a
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
                recording: mainPanel.recording
                recordingActive: audioRecorder.recording
                usbGrabberOpen: mainPanel.usbGrabberOpen
                benchmarkOpen: mainPanel.benchmarkOpen
                visible: !AppSettings.useSystemTitlebar
                onSettingsRequested: settingsPopup.open()
                onForceRetranscribeAllRequested: sidebar.forceRetranscribeAll()
                // Each button toggles ITS screen: on if it wasn't showing,
                // otherwise back to the transcript. setTopLevelScreen closes
                // whatever else was open, so leaving one screen can never
                // reveal a stale one underneath.
                onRecordRequested: mainPanel.setTopLevelScreen(
                    mainPanel.recording ? "" : "recorder")
                onUsbGrabberRequested: mainPanel.setTopLevelScreen(
                    mainPanel.usbGrabberOpen ? "" : "usbGrabber")
                onBenchmarkRequested: mainPanel.setTopLevelScreen(
                    mainPanel.benchmarkOpen ? "" : "benchmark")
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
                    // Mirrors BottomBar's currentFilePath binding below —
                    // orphaned .md and normal audio-player views are
                    // mutually exclusive (see MainPanel's showAudioFile()/
                    // showOrphanedMd()), so exactly one of these is ever the
                    // "currently open" path.
                    currentPath: mainPanel.orphanedMdPath !== "" ? mainPanel.orphanedMdPath
                                 : (mainPanel.player && mainPanel.player.source.toString() !== ""
                                    ? mainPanel.player.source.toString().replace("file://", "") : "")
                    onFileActivated: (path) => mainPanel.showAudioFile(path)
                    onOrphanedMdActivated: (path) => mainPanel.showOrphanedMd(path)
                    // Open the file (or orphaned .md) then
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
                    audioRecorder: audioRecorder
                    transcriptionController: transcriptionController
                    vaultTreeModel: vaultModel
                    usbGrabberController: usbGrabberController
                    onOpenVaultRequested: window.openVaultDialog()
                    onTagActivated: (tag) => sidebar.applyTagFilter(tag)
                    onSpeakerActivated: (name) => sidebar.applySpeakerFilter(name)
                    onWidthChanged: Qt.callLater(window.updateBlurRegions)
                    onHeightChanged: Qt.callLater(window.updateBlurRegions)
                }
            }

            BottomBar {
                id: bottomBar
                Layout.fillWidth: true
                vaultLocalPath: vaultModel.vaultLocalPath
                currentFilePath: mainPanel.orphanedMdPath !== "" ? mainPanel.orphanedMdPath
                                 : (mainPanel.player.source.toString() !== ""
                                    ? mainPanel.player.source.toString().replace("file://", "") : "")
                transcriptionController: transcriptionController
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
            // While capture is running the rim becomes the app-wide "you are
            // recording" signal: it takes recordingActiveColor, forces itself
            // visible even when the rim is switched off, and widens to at
            // least 2px so a 1px default rim is actually noticeable.
            border.width: audioRecorder.recording ? Math.max(AppSettings.rimWidth, 2) : AppSettings.rimWidth
            border.color: audioRecorder.recording ? AppSettings.recordingActiveColor : AppSettings.rimColor
            visible: (AppSettings.rimEnabled || audioRecorder.recording) && !AppSettings.useSystemTitlebar

            Behavior on color { ColorAnimation { duration: AppSettings.animationsEnabled ? 150 : 0 } }
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

        // The app's ONE floating-notification surface — every transient
        // message (failed import, failed move/rename/delete, rejected speaker
        // rename, rejected transcript edit) lands here rather than in four
        // separate per-panel banners. Posts come from the Notifications
        // singleton, so nothing has to route a signal up to this file. See
        // StyleGuide.md "Snackbars"; it does its own Overlay.overlay reparent.
        SnackbarHost {}
    }
}
