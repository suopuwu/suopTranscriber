import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SuopTranscriber

Item {
    id: bottomBar

    property string vaultLocalPath: ""
    property TranscriptionController transcriptionController
    // Wave D item 2: the currently-open file's raw local path (audio file or
    // orphaned .md — whatever MainPanel currently shows), empty if none.
    property string currentFilePath: ""
    // The bottom-left path, split into independently clickable segments, in
    // display order: the vault root, then one per subfolder inside the vault,
    // then the open file itself. Each segment carries the absolute local path
    // its OWN click acts on, and `isFile` picks reveal-the-file-selected over
    // just-open-the-folder.
    //
    // The vault root stays ONE segment on purpose: the folders above it
    // (/home, /home/user, …) are outside the vault and get no target of their
    // own, so clicking anywhere in the root always lands on the vault folder.
    readonly property var pathSegments: {
        if (bottomBar.vaultLocalPath === "")
            return []
        const segments = [{
            text: bottomBar.vaultLocalPath, path: bottomBar.vaultLocalPath, isFile: false
        }]
        if (bottomBar.currentFilePath === "")
            return segments

        // An open file that isn't under the vault root can't be broken down
        // against it, so it stays one whole-path segment rather than being
        // split into segments whose paths would be fiction.
        const prefix = bottomBar.vaultLocalPath + "/"
        if (!bottomBar.currentFilePath.startsWith(prefix)) {
            segments.push({
                text: bottomBar.currentFilePath, path: bottomBar.currentFilePath, isFile: true
            })
            return segments
        }

        const parts = bottomBar.currentFilePath.slice(prefix.length).split("/")
        let path = bottomBar.vaultLocalPath
        for (let i = 0; i < parts.length; ++i) {
            path += "/" + parts[i]
            segments.push({ text: "/" + parts[i], path: path, isFile: i === parts.length - 1 })
        }
        return segments
    }
    signal settingsRequested()
    // Emitted when a Tasks-popup row is clicked — path is the job's audio
    // file's LOCAL path (fileUrl minus the "file://" prefix, same convention
    // Sidebar.qml's fileActivated uses), for the caller to open in MainPanel.
    signal taskActivated(string path)
    // Percentage TEXT (item 6): this phase's own 0..1 (see
    // TranscriptionController::progress's doc comment for why it's phase-
    // relative, not blended across the whole job).
    readonly property real taskProgress: transcriptionController ? transcriptionController.progress : 0
    // Item 6 (user correction): the PROGRESS BAR's fill instead sweeps
    // smoothly across the job's WHOLE lifetime — see
    // TranscriptionController::overallProgress's doc comment.
    readonly property real taskOverallProgress: transcriptionController ? transcriptionController.overallProgress : 0
    readonly property int tasksCompleted: transcriptionController ? transcriptionController.tasksCompleted : 0
    readonly property int tasksTotal: transcriptionController ? transcriptionController.tasksTotal : 0
    // Whether a job is actively running right now — distinct from tasksTotal
    // (which also counts Done/Failed jobs still sitting in the persistent,
    // dismissible history list, see TranscriptionController's class doc).
    // True while ANY task is Active — a benchmark/sweep included. Wave N: a
    // benchmark is a real queued task now (Presentation::Background), so it
    // sets `busy` and feeds `overallProgress` like everything else; the old
    // separate benchmarkActive/benchmarkProgress glow path is gone.
    readonly property bool busy: transcriptionController ? transcriptionController.busy : false
    // The Active job's kind/file — shown next to the progress bar so it's
    // clear at a glance WHAT is currently running, not just that something
    // is. currentFile here is the JOB's file (from TranscriptionController),
    // not currentFilePath above (whatever MainPanel happens to have open —
    // usually the same file, but not necessarily, e.g. bulk "transcribe all").
    readonly property string currentJobKind: transcriptionController ? transcriptionController.currentJobKind : ""
    readonly property string currentJobFileName: {
        if (!transcriptionController)
            return ""
        const s = transcriptionController.currentFile.toString()
        return s.substring(s.lastIndexOf("/") + 1)
    }
    // User correction (2026-07-09): restored (persisted) jobs no longer
    // auto-start — they just sit Queued until the user resumes them. While
    // idle, the bottom bar still shows the front-most Queued job (most
    // commonly the one that was Active when the app last closed) so there's
    // something to look at and a play button to resume it, instead of the
    // bar going blank just because nothing happens to be running yet. A
    // Paused job (see TranscriptionController::JobStatus::Paused's doc
    // comment — a pause during "loading" that had to restart rather than
    // truly suspend) takes priority, matching startNext()'s own priority —
    // there's at most one at a time.
    readonly property var frontQueuedJob: {
        if (bottomBar.busy || !bottomBar.transcriptionController)
            return null
        const jobs = bottomBar.transcriptionController.jobs
        for (let i = 0; i < jobs.length; i++) {
            if (jobs[i].status === "paused")
                return jobs[i]
        }
        for (let i = 0; i < jobs.length; i++) {
            if (jobs[i].status === "queued")
                return jobs[i]
        }
        return null
    }
    readonly property bool hasDisplayJob: bottomBar.busy || bottomBar.frontQueuedJob !== null
    readonly property string displayJobKind: bottomBar.busy ? bottomBar.currentJobKind
        : (bottomBar.frontQueuedJob ? bottomBar.frontQueuedJob.kind : "")
    // The human category LABEL for displayJobKind — authored by each Task
    // (Task::kindLabel()), read straight off the active task
    // (currentJobKindLabel) or the front queued job's row (jobs()' kindLabel
    // field), never re-derived from the slug in QML.
    readonly property string displayJobKindLabel: bottomBar.busy
        ? (bottomBar.transcriptionController ? bottomBar.transcriptionController.currentJobKindLabel : "")
        : (bottomBar.frontQueuedJob ? bottomBar.frontQueuedJob.kindLabel : "")
    readonly property string displayJobFileName: {
        if (bottomBar.busy)
            return bottomBar.currentJobFileName
        if (bottomBar.frontQueuedJob) {
            const s = bottomBar.frontQueuedJob.fileUrl ? bottomBar.frontQueuedJob.fileUrl.toString() : ""
            return s.substring(s.lastIndexOf("/") + 1)
        }
        return ""
    }
    // See TranscriptionController::currentPhase's doc comment — "loading"
    // and "decoding" have no real progress number behind them (whisper.cpp/
    // ffmpeg have no hook for model-load/decode time), so taskProgress is a
    // SYNTHESIZED fraction there (elapsed-vs-rolling-average — see
    // TranscriptionController::phaseRelativeSyntheticProgress()), not
    // indeterminate — 2026-07-10: every phase now occupies its own
    // historically-weighted slice of the bar (see overallProgress's doc
    // comment), which only reads correctly if every phase actually shows
    // real (if approximate) motion instead of a spinner with no width.
    readonly property string currentPhase: transcriptionController ? transcriptionController.currentPhase : ""
    // Wave N (task system): true while the Active job is paused (only
    // "transcribe"/"download" kinds are truly pausable — see
    // TranscriptionController::pauseActive()'s doc comment).
    readonly property bool activePaused: transcriptionController ? transcriptionController.activePaused : false
    // True between clicking pause during "loading" and that soft-cancel
    // actually resolving (see TranscriptionController::
    // activeStopping's doc comment) — the pause button disables itself
    // meanwhile so a second click can't land, and the phase label says so
    // instead of silently continuing to show the stale phase, which is what
    // made the first click look like it did nothing (user report,
    // 2026-07-10).
    readonly property bool activeStopping: transcriptionController ? transcriptionController.activeStopping : false
    // Wave N (task system): whether the Active task can pause AT ALL, read
    // straight off its Task::capabilities().pausable via the controller — the
    // hardcoded kind allowlist ("transcribe"/"download"/"combine"/"chapters")
    // is gone. The task is the single source of truth, so this can never drift
    // from the C++ pause logic the way a duplicated QML list could (that drift
    // was the 2026-07-09 pause bug). The finer "can pause RIGHT NOW" (phase
    // gating) stays in canPauseNow() below.
    readonly property bool activeCanPause: transcriptionController ? transcriptionController.activeCanPause : false
    // Item 9: whether the pause/play button should be ENABLED right now
    // (visibility is handled separately — the button is only SHOWN for a
    // pausable kind or a resumable queue). Starting a queued job (idle) and
    // resuming an already-paused job are always allowed; while a job is
    // actively running, pausing is only honorable once it's past the
    // uninterruptible "loading" phase (whisper.cpp's blocking model load has
    // no pause hook — see TranscriptionController::togglePauseActive()). A
    // greyed button during "loading" is deliberate: the OLD behavior turned a
    // pause tap there into a silent cancel ("pause broken again").
    function canPauseNow() {
        if (!bottomBar.busy)
            return true
        if (bottomBar.activePaused)
            return true
        if (bottomBar.activeStopping)
            return false
        return bottomBar.currentPhase !== "loading"
    }
    // Shared by the bottom bar's and the popup header's pause/play button —
    // kept in one place so the glyph and the tap behavior (see both
    // TapHandlers) can never drift out of sync with each other. Always the
    // ordinary pause/play glyph regardless of phase: item 9 (2026-07-14)
    // removed the old "loading = cancel-and-restart" behavior entirely, so
    // the button never doubles as a stop — during a phase that can't be
    // paused yet ("loading") it's simply greyed (see canPauseNow()), not
    // shown with a distinct stop affordance.
    function playPauseIconName() {
        if (!bottomBar.busy)
            return "play"
        return bottomBar.activePaused ? "play" : "pause"
    }
    // activeElapsedMs/estimatedRemainingMs are plain invokables (no NOTIFY
    // signal — see TranscriptionController::activeElapsedMs()'s doc
    // comment), so they need to be actively POLLED rather than bound —
    // deliberately push-based (Timer writes into a plain property) rather
    // than the more common "read a dummy property inside a binding to
    // fake a dependency" trick: that trick does NOT survive this project's
    // AOT QML compilation (qmlcachegen) — confirmed empirically (a real
    // launch showed the dummy-dependency version frozen at its initial
    // value forever, while directly calling the same invokable elsewhere
    // in the same file worked fine every tick) — presumably because the
    // compiler sees the dummy read's result is unused and drops it,
    // dropping the dependency registration along with it. A plain
    // imperative assignment has no such ambiguity: writing to a property
    // always fires ITS OWN notify signal.
    property int activeElapsedMs: 0
    property int estimatedRemainingMs: -1
    function refreshActiveElapsed() {
        bottomBar.activeElapsedMs = bottomBar.transcriptionController ? bottomBar.transcriptionController.activeElapsedMs() : 0
        bottomBar.estimatedRemainingMs = bottomBar.transcriptionController ? bottomBar.transcriptionController.estimatedRemainingMs() : -1
    }
    Timer {
        interval: 500
        repeat: true
        running: bottomBar.busy
        // Refresh immediately when a job starts (running flips true) rather
        // than leaving stale/zeroed values on screen for the first 500ms.
        onRunningChanged: if (running) bottomBar.refreshActiveElapsed()
        onTriggered: bottomBar.refreshActiveElapsed()
    }
    // "1:23" / "45s" — durationMs < 0 (unknown/not-yet-finished) -> "".
    function formatDuration(durationMs) {
        if (durationMs < 0)
            return ""
        const totalSeconds = Math.floor(durationMs / 1000)
        const h = Math.floor(totalSeconds / 3600)
        const m = Math.floor((totalSeconds % 3600) / 60)
        const s = totalSeconds % 60
        if (h > 0)
            return h + ":" + String(m).padStart(2, "0") + ":" + String(s).padStart(2, "0")
        if (m > 0)
            return m + ":" + String(s).padStart(2, "0")
        return s + "s"
    }
    // Item 4/7: "elapsed / ~ETA left" while the ETA is knowable (see
    // TranscriptionController::estimatedRemainingMs()'s doc comment for when
    // it isn't — e.g. the "loading" phase), otherwise just elapsed.
    readonly property string elapsedEtaText: {
        if (!bottomBar.busy)
            return ""
        const elapsed = bottomBar.formatDuration(bottomBar.activeElapsedMs)
        // O16: a real cooperative pause never changes Job::status (it stays
        // "active" the whole time — see JobStatus::Paused's doc comment for
        // the one narrow case where it doesn't), so this is the one place
        // that has to check activePaused directly rather than trusting the
        // status string. The ETA is meaningless while frozen, so drop it
        // entirely and say so instead of showing a stale "~Y left".
        if (bottomBar.activePaused)
            return qsTr("paused at %1").arg(elapsed)
        return bottomBar.estimatedRemainingMs >= 0
            ? (elapsed + " / ~" + bottomBar.formatDuration(bottomBar.estimatedRemainingMs) + " left")
            : elapsed
    }
    // Item 3 (reorder): position of jobId among Queued-only entries (the
    // ordering TranscriptionController::moveQueuedJob()'s newIndex counts
    // within) — -1 if jobId isn't currently Queued. Used by the popup's
    // per-row up/down buttons to compute the target index and to disable
    // the button at either end of the queued sublist.
    function queuedPositionOf(jobId) {
        if (!bottomBar.transcriptionController)
            return -1
        const jobs = bottomBar.transcriptionController.jobs
        let pos = 0
        for (let i = 0; i < jobs.length; i++) {
            if (jobs[i].id === jobId)
                return jobs[i].status === "queued" ? pos : -1
            if (jobs[i].status === "queued")
                pos++
        }
        return -1
    }
    function queuedCount() {
        if (!bottomBar.transcriptionController)
            return 0
        const jobs = bottomBar.transcriptionController.jobs
        let count = 0
        for (let i = 0; i < jobs.length; i++) {
            if (jobs[i].status === "queued")
                count++
        }
        return count
    }
    // Shared with the Tasks popup's per-row kind badge below — one case per
    // "kind" value (see TranscriptionController::jobKind()).
    // (The kind→label mapping now lives on the Task primitive — each kind
    // authors its own Task::kindLabel(), exposed as jobs()' "kindLabel" field
    // and the controller's currentJobKindLabel. This QML no longer maintains a
    // switch that a new kind could forget, silently mislabeling itself.)

    // Appended to the kind/filename label so the "huge delay" before
    // transcription starts on bigger models (or with speaker detection on)
    // reads as "doing something", not a hang. The phase→label mapping is the
    // controller's single source of truth (TranscriptionController::phaseLabel)
    // — this QML no longer keeps a switch a new emitted phase could forget,
    // rendering blank. Empty for an unknown/absent phase.
    readonly property string currentPhaseLabel: transcriptionController
        ? transcriptionController.phaseLabel(bottomBar.currentPhase) : ""

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

    // P11: alternative progress-bar style — a thin glowing line across the
    // top edge of the bar, sweeping across the job's WHOLE lifetime (same
    // taskOverallProgress the existing ProgressBar below uses, for a smooth
    // blended-across-phases sweep rather than progress's per-phase reset).
    // Opt-in via AppSettings.progressGlowEnabled — the existing percentage
    // text/ProgressBar are left completely alone either way. Deliberately
    // NOT using Qt5Compat.GraphicalEffects (Glow/RectangularGlow) — see
    // Main.qml's comment above rootContent (~line 154) documenting a
    // reproducible OpacityMask rendering bug on this Qt/RHI combination from
    // that same module; the same shader risk almost certainly applies here
    // too. Instead the "glow" is faked with layered, progressively wider and
    // more transparent plain Rectangles behind a thin, fully-opaque core
    // line — cheap, stock-QtQuick, no extra modules.
    Item {
        id: progressGlow
        visible: AppSettings.progressGlowEnabled && bottomBar.busy
        anchors.top: parent.top
        anchors.left: parent.left
        // Bug: taskOverallProgress is genuinely 0.0 at the exact instant a
        // job goes busy (see TranscriptionController::recomputeOverallProgress()
        // — the "loading" phase starts its synthetic progress at 0 and ramps
        // up from there), and unlike the plain ProgressBar below (which
        // always paints its own background groove regardless of value),
        // these Rectangles have no track — at width 0 they draw literally
        // nothing. For any job whose busy phases resolve quickly, that
        // could mean the glow never becomes visibly wider than 0 before the
        // job finishes, reading as "doesn't work" rather than "just started
        // at 0%". A small width floor keeps it visible as soon as busy
        // flips true.
        width: Math.max(4, bottomBar.width * bottomBar.taskOverallProgress)
        height: AppSettings.progressGlowWidth + AppSettings.progressGlowRadius

        // Halo layers — configurable count (AppSettings.progressGlowLayerCount),
        // stacked outermost-first so each successive (inner) layer paints on
        // top, with the opaque core line below drawn last of all. Each layer
        // interpolates thickness and opacity across the count:
        //   - spread (thickness beyond the core) runs from the full glow
        //     radius at the outermost layer down toward the core at the
        //     innermost, via (count - index) / count;
        //   - opacity runs linearly from 0.15 (outermost, faintest) to 0.35
        //     (innermost, strongest).
        // At count == 2 this reproduces the original hand-written pair exactly
        // (outer: radius*1.0 / 0.15, inner: radius*0.5 / 0.35); higher counts
        // add intermediate layers for a smoother, deeper glow.
        Repeater {
            model: AppSettings.progressGlowLayerCount
            Rectangle {
                required property int index
                readonly property int layerCount: AppSettings.progressGlowLayerCount
                anchors.top: parent.top
                anchors.left: parent.left
                width: parent.width
                height: AppSettings.progressGlowWidth
                        + AppSettings.progressGlowRadius * (layerCount - index) / layerCount
                color: AppSettings.progressGlowColor
                opacity: 0.15 + (0.35 - 0.15)
                         * (layerCount > 1 ? index / (layerCount - 1) : 1)
            }
        }
        // Core line — the real thin, fully-opaque edge.
        Rectangle {
            anchors.top: parent.top
            anchors.left: parent.left
            width: parent.width
            height: AppSettings.progressGlowWidth
            color: AppSettings.progressGlowColor
        }
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
            AppIcon {
                anchors.centerIn: parent
                name: "settings"
                iconSize: 12
                color: "#aaaaaa"
            }
        }

        // Item 4 (Wave E): the segments must read as ONE continuous
        // left-aligned string, so exactly ONE of them may claim
        // Layout.fillWidth and it must be the LAST. Give the claim to an
        // earlier segment and it takes the whole row, shoving everything after
        // it flush against the right edge instead of sitting immediately after
        // it. The fillWidth segment is also the one that elides, and eliding
        // belongs at the far end of the string, never mid-path.
        RowLayout {
            Layout.fillWidth: true
            spacing: 0

            Label {
                visible: bottomBar.vaultLocalPath === ""
                Layout.fillWidth: true
                text: qsTr("No vault open")
                color: "#aaaaaa"
                font.pixelSize: 11
            }

            Repeater {
                model: bottomBar.pathSegments
                delegate: Label {
                    id: segment

                    // Carries an objectName purely so tst_bottombar_pathsegments
                    // can find the segments and click them — they have no other
                    // stable handle (same trick as the tree's "expandGlyph").
                    objectName: "pathSegment"

                    required property var modelData
                    required property int index
                    readonly property bool isRoot: segment.index === 0

                    Layout.fillWidth: segment.index === bottomBar.pathSegments.length - 1
                    // The vault root is the one segment whose middle is the
                    // least interesting part of it (…/deeply/nested/vault), so
                    // it's the one that elides there rather than at its end.
                    elide: segment.isRoot ? Text.ElideMiddle : Text.ElideRight
                    text: segment.modelData.text
                    // Wave D item 2: the in-vault part is dimmer than the root.
                    color: segment.isRoot ? "#aaaaaa" : "#666666"
                    font.pixelSize: 11
                    // Underline only the hovered segment, not the whole path:
                    // each segment is its own target, so underlining all of
                    // them would misreport what the click is about to open.
                    font.underline: segmentHover.hovered

                    HoverHandler {
                        id: segmentHover
                        cursorShape: Qt.PointingHandCursor
                    }
                    TapHandler {
                        onTapped: {
                            // The file itself opens its folder with the file
                            // selected; a folder segment just opens that
                            // folder. Either way the file manager's window is
                            // raised and focused — see DesktopIntegration for
                            // the activation token that takes.
                            if (segment.modelData.isFile)
                                DesktopIntegration.revealInFileManager("file://" + segment.modelData.path)
                            else
                                DesktopIntegration.openInFileManager("file://" + segment.modelData.path)
                        }
                    }
                }
            }
        }

        Label {
            Layout.preferredWidth: 160
            elide: Text.ElideMiddle
            visible: bottomBar.hasDisplayJob
            text: bottomBar.displayJobKindLabel + ": " + bottomBar.displayJobFileName
            color: "#aaaaaa"
            font.pixelSize: 11
        }

        // Deliberately a SEPARATE, non-eliding Label — this is the one
        // piece of text telling the user what's actually happening during
        // the "huge delay" (loading/detecting-speakers/transcribing), so it
        // must never be the part that gets truncated. Only the filename
        // above (which can legitimately be long) elides; this one sizes to
        // its own content instead of sharing a fixed-width elide box with it.
        Label {
            visible: bottomBar.busy && bottomBar.currentPhaseLabel !== ""
            // 2026-07-10: taskProgress is now a real (if synthesized, for
            // "loading"/"decoding" — see currentPhase's doc comment) number
            // in every phase, so the percentage always shows instead of
            // being omitted for two of them.
            text: bottomBar.activeStopping
                ? qsTr("(stopping…)")
                : bottomBar.activePaused
                    ? qsTr("(paused)")
                    : "(" + bottomBar.currentPhaseLabel + " "
                      + (bottomBar.taskProgress * 100).toFixed(1) + "%)"
            color: "#888888"
            font.pixelSize: 11
        }

        // Item 4/7: elapsed/ETA for whatever's currently running.
        Label {
            visible: bottomBar.busy && bottomBar.elapsedEtaText !== ""
            text: bottomBar.elapsedEtaText
            color: "#888888"
            font.pixelSize: 11
        }

        // Item 2: pause/resume the Active job — only meaningful for
        // "transcribe"/"download" kinds (see canPause()'s doc comment);
        // hidden entirely rather than shown-but-inert for the others, so a
        // click never silently does nothing. Doubles as the "resume a
        // saved-but-not-yet-started queue" play button (user correction,
        // 2026-07-09) whenever nothing is Active but a Queued job is
        // waiting — that case is never kind-gated (any restored job kind
        // can be started, only ACTIVE jobs are pause-gated by kind).
        Item {
            id: pauseButton
            visible: (bottomBar.busy && bottomBar.activeCanPause)
                || (!bottomBar.busy && bottomBar.frontQueuedJob !== null)
            // Item 9: greyed (not just hidden) while the current phase can't
            // be paused — during "loading" the button is visible but disabled
            // rather than silently reinterpreting the tap as a cancel. Also
            // covers the activeStopping window (see canPauseNow()).
            enabled: bottomBar.canPauseNow()
            opacity: enabled ? 1 : 0.4
            Layout.preferredWidth: 16
            Layout.preferredHeight: 16

            HoverHandler {
                id: pauseHover
                cursorShape: Qt.PointingHandCursor
            }
            TapHandler {
                // Default gesturePolicy (DragThreshold) cancels the tap if
                // the pointer moves at all between press and release —
                // easy to trigger by accident on a small (16px) target,
                // where aiming precisely enough to land the click often
                // means a last-instant micro-adjustment. ReleaseWithinBounds
                // only cares that the release lands back inside the item,
                // regardless of what the pointer did in between (user
                // report, 2026-07-09: hover reliably highlighted this
                // button, but clicking it did nothing — consistent with taps
                // being silently cancelled here).
                gesturePolicy: TapHandler.ReleaseWithinBounds
                // See TranscriptionController::togglePauseActive()'s doc
                // comment — the pause-vs-cancel decision now lives entirely
                // in C++, off its own authoritative phase/pause state, so a
                // click can't race a phase transition QML hasn't heard
                // about yet.
                onTapped: bottomBar.transcriptionController.togglePauseActive()
            }
            HoverBorder {
                anchors.fill: parent
                active: pauseHover.hovered
            }
            AppIcon {
                anchors.centerIn: parent
                name: bottomBar.playPauseIconName()
                iconSize: 11
                color: "#dddddd"
            }
        }

        // Item 9: the bottom bar shows ONLY a pause/resume button now — the
        // separate stop/cancel button was removed as redundant. Cancelling
        // the active job still lives in the Tasks popup (per-row × button,
        // and "Cancel all"), which is where per-job cancel belongs.

        ProgressBar {
            Layout.preferredWidth: 140
            from: 0
            to: 1
            value: bottomBar.taskOverallProgress
            // Always determinate now (2026-07-10) — every phase, including
            // "loading"/"decoding" which have no real progress hook, gets a
            // real (synthesized for those two) taskOverallProgress number
            // and a historically-weighted slice of the bar (see
            // TranscriptionController::overallProgress's doc comment). A
            // paused job simply stops advancing rather than needing a
            // separate suppression (user report, 2026-07-10, about the old
            // indeterminate style looping regardless of pause state — moot
            // now that this bar is never indeterminate).
            // Only while something is ACTIVELY running — tasksTotal alone
            // would keep this visible (frozen, meaningless) after every job
            // finishes, since finished jobs stay listed until dismissed.
            // Also yields to the glow when it's on and configured to take over
            // (progressGlowHidesBar) — the glow then reports progress alone.
            visible: bottomBar.busy && !(AppSettings.progressGlowEnabled && AppSettings.progressGlowHidesBar)
        }

        // P22 / item 25: which backend actually loaded the active model —
        // reduced to just "gpu"/"cpu" (see TranscriptionController::
        // activeBackendKind) rather than the raw "Vulkan"/"CUDA"/"CPU" name,
        // per the user's request. Empty (hidden) until a model has loaded,
        // same visibility convention as before.
        Label {
            text: bottomBar.transcriptionController ? bottomBar.transcriptionController.activeBackendKind : ""
            visible: text !== ""
            color: "#888888"
            font.pixelSize: 11
        }

        Item {
            id: tasksButton
            // Item 36: always visible — the counter reads "0/0 tasks" when
            // idle (fresh start / after Clear) rather than vanishing, so the
            // Tasks popup stays reachable and the count is a stable fixture.
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
        // Item 29: wide enough that a typical "kind + filename + status" row
        // reads without the filename getting truncated mid-word — the kind
        // badge (~62) and status label (~96) are fixed, so the earlier 360
        // left the fillWidth filename too little room.
        width: 480
        // Item 21: one deliberate, symmetric inner padding used BOTH as the
        // ColumnLayout's margin (below) AND in this height calc, so the gap
        // below the task list is exactly the gap above the header/Clear-button
        // row. The height now hugs the content (header + capped ScrollView) —
        // no artificial min-height floor, which was the only thing that ever
        // left uneven vertical slack below the list.
        // P18: Math.min is a ceiling only — with many real tasks
        // jobColumn.implicitHeight (header + capped ScrollView content) could
        // exceed the window; the inner ScrollView's own Layout.preferredHeight
        // cap (below) keeps rows scrollable well before this outer cap is hit
        // in practice, but the cap is still a needed hard backstop.
        readonly property int innerPadding: 8
        height: Math.min(jobColumn.implicitHeight + 2 * innerPadding, 500)
        // Item: the height calc above assumes jobColumn (anchors.fill parent +
        // margins:innerPadding) spans the WHOLE popup, so the popup's own
        // default padding must be zeroed. Left at the style default (nonzero),
        // it inset the contentItem jobColumn fills, adding that padding to the
        // TOP while the unbudgeted overflow got clipped off the BOTTOM — the
        // "bottom padding is shorter than the top above the Clear button" bug.
        // With padding:0, top and bottom are both exactly innerPadding.
        padding: 0
        clip: true
        // O21: was modal:false, which gives Qt no dim rectangle and no real
        // block-plane — clicks/hover reached whatever was behind the popup
        // (both MouseArea- and TapHandler-based) at the same time as the
        // popup itself. CloseOnPressOutside still closes it on an outside
        // click same as before; the click just no longer ALSO lands on
        // whatever's behind it.
        modal: true
        dim: true
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
            // Item 21: symmetric with the popup height's 2*innerPadding, so
            // the padding below the list equals the padding above the header.
            anchors.margins: jobListPopup.innerPadding
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
                // Item 2: pause/resume the Active job — only one job is
                // ever Active at a time, so a single header-level button
                // (rather than a per-row one) covers it; only meaningful for
                // "transcribe"/"download" kinds (see canPause()'s doc
                // comment) — hidden entirely otherwise, same as the bottom
                // bar's own pause button. Also doubles as the "resume a
                // saved queue" play button — see the bottom bar's own
                // pauseButton doc comment for the full rationale.
                Item {
                    id: popupPauseButton
                    visible: (bottomBar.busy && bottomBar.activeCanPause)
                        || (!bottomBar.busy && bottomBar.frontQueuedJob !== null)
                    // Item 9: same enable gating as the bottom bar's own
                    // pauseButton (greyed during "loading"/stopping) — shared
                    // via canPauseNow() so the two never drift apart.
                    enabled: bottomBar.canPauseNow()
                    opacity: enabled ? 1 : 0.4
                    Layout.preferredWidth: 20
                    Layout.preferredHeight: 20

                    HoverHandler {
                        id: popupPauseHover
                        cursorShape: Qt.PointingHandCursor
                    }
                    TapHandler {
                        // See the bottom bar's own pauseButton TapHandler
                        // for why this is needed on a small icon target.
                        gesturePolicy: TapHandler.ReleaseWithinBounds
                        // See TranscriptionController::togglePauseActive()'s
                        // doc comment — deciding pause-vs-cancel here in QML
                        // (reading this file's own currentPhase copy) used to
                        // race the backend's real phase and could fire an
                        // accidental cancel right at the loading->diarizing
                        // boundary. The controller now makes this call itself
                        // off its own authoritative state.
                        onTapped: bottomBar.transcriptionController.togglePauseActive()
                    }
                    HoverBorder {
                        anchors.fill: parent
                        active: popupPauseHover.hovered
                    }
                    AppIcon {
                        anchors.centerIn: parent
                        name: bottomBar.playPauseIconName()
                        iconSize: 12
                        color: "#dddddd"
                    }
                }
                // Item 3: cancels every Queued job, then the Active one too
                // (same immediate-vs-next-checkpoint caveat as the per-row
                // cancel button below — see
                // TranscriptionController::cancelAll()'s doc comment).
                Button {
                    id: cancelAllButton
                    text: qsTr("Cancel all")
                    hoverEnabled: true
                    visible: bottomBar.tasksTotal > bottomBar.tasksCompleted
                    onClicked: bottomBar.transcriptionController.cancelAll()

                    background: Rectangle {
                        radius: AppSettings.borderRadius
                        color: (cancelAllButton.hovered && cancelAllButton.enabled) ? "#3a3a3a" : "#2a2a2a"
                        border.width: 1
                        border.color: "#3a3a3a"
                    }
                    HoverHandler {
                        cursorShape: Qt.PointingHandCursor
                    }
                    HoverBorder {
                        anchors.fill: parent
                        active: cancelAllButton.hovered
                    }
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
                    // Items 10/11: bind to the pre-partitioned sortedJobs view
                    // (see TranscriptionController::sortedJobs) — the
                    // currently-running/next-to-run jobs sit at the TOP in
                    // real run order, with finished (done/failed/cancelled)
                    // history demoted to the bottom. The old blind reversal of
                    // raw jobs() inverted run order (soonest-to-run at the
                    // BOTTOM) and never separated finished from pending.
                    // Nothing else (queuedPositionOf(), the move-up/down
                    // handlers, etc.) touches this binding — they all recompute
                    // position by walking transcriptionController.jobs (raw
                    // append order) from scratch, so they're unaffected.
                    model: bottomBar.transcriptionController ? bottomBar.transcriptionController.sortedJobs : []
                    spacing: 2

                    // Which batches are expanded, keyed by batchId. Held HERE,
                    // not on the delegate: sortedJobs returns a fresh array on
                    // every queueChanged (a member finishing, say), so the
                    // ListView recreates its delegates and any per-delegate
                    // state would reset — re-collapsing a batch the user just
                    // opened, repeatedly, mid-processing. Keyed by the stable
                    // batchId, expansion survives those rebuilds. (Reassigned,
                    // not mutated, so the delegates' bindings re-evaluate.)
                    property var expandedBatches: ({})
                    function toggleBatchExpanded(batchId) {
                        const next = {}
                        for (const k in jobListView.expandedBatches)
                            next[k] = jobListView.expandedBatches[k]
                        next[batchId] = !next[batchId]
                        jobListView.expandedBatches = next
                    }

                    // A multi-select bulk op (transcribe/relabel/chapters/
                    // detect-speakers/run-all/move over N files) collapses into
                    // ONE row via TranscriptionController::collapseBatches() —
                    // modelData.isBatch, with modelData.members holding the
                    // per-file task rows. This delegate is a Column: the batch
                    // header (or a standalone task) on top, and — for an
                    // EXPANDED batch — its member rows beneath. taskRowComponent
                    // renders all three cases (standalone / header / member) so
                    // the row layout lives in one place; index 0 is the header,
                    // index > 0 a member (see its isHeader/isMember).
                    delegate: Column {
                        id: entryColumn
                        required property var modelData
                        width: jobListView.width
                        spacing: 2

                        readonly property bool entryIsBatch: modelData.isBatch === true
                        // Read from jobListView.expandedBatches so it survives
                        // the model rebuilds sortedJobs triggers — see that
                        // property's comment.
                        readonly property bool expanded: entryColumn.entryIsBatch
                            && jobListView.expandedBatches[modelData.batchId] === true

                        readonly property var visibleRows: {
                            if (!entryColumn.entryIsBatch)
                                return [modelData]
                            const rows = [modelData] // header first
                            if (entryColumn.expanded) {
                                const ms = modelData.members
                                for (let i = 0; i < ms.length; ++i)
                                    rows.push(ms[i])
                            }
                            return rows
                        }

                        Repeater {
                            model: entryColumn.visibleRows
                            delegate: taskRowComponent
                        }

                        Component {
                            id: taskRowComponent
                            Item {
                        id: jobRow
                        required property var modelData
                        required property int index
                        width: entryColumn.width
                        height: 24

                        // Role within entryColumn: a batch's header is index 0
                        // (aggregate row), its members index > 0; a standalone
                        // task is a lone non-batch row.
                        readonly property bool isHeader: entryColumn.entryIsBatch && index === 0
                        readonly property bool isMember: entryColumn.entryIsBatch && index > 0

                        readonly property bool isDownload: modelData.kind === "download"
                        readonly property string fileName: {
                            if (jobRow.isHeader) {
                                // The batch header names the whole set by its
                                // distinct-file count (a run-all batch has
                                // several tasks per file, so this is files, not
                                // tasks — see collapseBatches()).
                                const n = modelData.batchCount
                                return n === 1 ? qsTr("1 file") : qsTr("%1 files").arg(n)
                            }
                            if (jobRow.isDownload) {
                                // Downloads have no fileUrl (nothing to
                                // open) — name the row after WHAT is being
                                // downloaded instead.
                                switch (modelData.downloadTarget) {
                                case "model": return modelData.modelId
                                case "label": return qsTr("auto-label model")
                                case "vad": return qsTr("voice-activity model")
                                case "segmentation": return qsTr("speaker segmentation model")
                                case "embedding": return qsTr("speaker embedding model")
                                default: return ""
                                }
                            }
                            const s = modelData.fileUrl ? modelData.fileUrl.toString() : ""
                            return s.substring(s.lastIndexOf("/") + 1)
                        }
                        // O16: same root cause as bottomBar.elapsedEtaText —
                        // a real cooperative pause never changes
                        // modelData.status away from "active" (only
                        // JobStatus::Paused's narrow loading-restart edge
                        // case does that), so this row has to check
                        // bottomBar.activePaused directly too, not just the
                        // status string, since this IS unambiguously the
                        // Active row whenever modelData.status === "active"
                        // (there's only ever one).
                        readonly property bool isActivePaused: modelData.status === "active" && bottomBar.activePaused
                        // O20: a cancelled job is reported as
                        // status: Failed, error: "Cancelled" by the backend
                        // (see TranscriptionController::jobs()'s doc
                        // comment) — same as any other failure otherwise, so
                        // without this check a user-initiated cancel and a
                        // genuine transcription failure render identically.
                        readonly property bool isCancelled: modelData.status === "failed" && modelData.error === "Cancelled"
                        readonly property string statusText: {
                            if (jobRow.isActivePaused)
                                return qsTr("paused")
                            if (jobRow.isCancelled)
                                return qsTr("cancelled")
                            return modelData.status
                        }
                        readonly property color statusColor: {
                            if (jobRow.isActivePaused)
                                return "#e67e22"
                            if (jobRow.isCancelled)
                                return "#888888"
                            switch (modelData.status) {
                            case "done": return "#2ecc71"
                            case "failed": return "#e74c3c"
                            case "active": return "#3498db"
                            case "paused": return "#e67e22"
                            default: return "#f1c40f"
                            }
                        }
                        // Distinguishes what kind of work this job actually
                        // is — the same queue/worker thread runs full
                        // transcriptions, "regenerate label" jobs, speaker
                        // renames, and (Wave N) model downloads, which
                        // otherwise look identical in this list (same
                        // status colors).
                        readonly property string kindLabel: modelData.kindLabel
                        // Item 4/7: elapsed/ETA while this IS the active
                        // row (there's only ever one, so bottomBar's own
                        // elapsedEtaText is unambiguously about THIS row
                        // whenever status is "active"); final duration once
                        // Done/Failed; nothing yet while Queued.
                        readonly property string detailText: {
                            // Batch header: show file-completion progress
                            // ("2/5 done") while anything's pending; the summed
                            // wall-clock once the whole batch is terminal.
                            if (jobRow.isHeader) {
                                if (modelData.status === "done" || modelData.status === "failed")
                                    return modelData.durationMs >= 0
                                        ? bottomBar.formatDuration(modelData.durationMs) : ""
                                return qsTr("%1/%2 done").arg(modelData.batchDoneCount).arg(modelData.batchCount)
                            }
                            if (modelData.status === "active") {
                                // Avoid "paused (paused at 45s)" — statusText
                                // above already says "paused"; just the bare
                                // elapsed time is needed here.
                                return jobRow.isActivePaused
                                    ? bottomBar.formatDuration(bottomBar.activeElapsedMs)
                                    : bottomBar.elapsedEtaText
                            }
                            if (modelData.status === "done" || modelData.status === "failed")
                                return bottomBar.formatDuration(modelData.durationMs)
                            return ""
                        }
                        // Item 3: up/down only make sense among Queued rows
                        // — see queuedPositionOf()'s doc comment. Never for a
                        // batch header or a member (a batch keeps its enqueue
                        // order; reorder stays a standalone-row affordance).
                        readonly property bool reorderable: !entryColumn.entryIsBatch
                        readonly property int queuedPosition: bottomBar.queuedPositionOf(modelData.id)

                        // A batch header's own row-tap toggles expand rather
                        // than opening a file (it has none); a member/standalone
                        // opens its note. Downloads open nothing.
                        readonly property bool rowClickable: jobRow.isHeader || !jobRow.isDownload

                        HoverHandler {
                            id: jobRowHover
                            cursorShape: Qt.PointingHandCursor
                            enabled: jobRow.rowClickable
                        }
                        // Clicking a task jumps straight to its note — same
                        // "row itself is clickable, inner buttons still work
                        // independently" pattern as Sidebar.qml's file rows;
                        // the buttons' own TapHandlers below (child Items)
                        // take priority over this one within their bounds.
                        // A batch header toggles its expansion instead.
                        TapHandler {
                            enabled: jobRow.rowClickable
                            onTapped: {
                                if (jobRow.isHeader) {
                                    jobListView.toggleBatchExpanded(modelData.batchId)
                                    return
                                }
                                bottomBar.taskActivated(jobRow.modelData.fileUrl.toString().replace("file://", ""))
                                jobListPopup.close()
                            }
                        }
                        HoverBorder {
                            anchors.fill: parent
                            active: jobRowHover.hovered && jobRow.rowClickable
                        }

                            RowLayout {
                                anchors.fill: parent
                                // Indent member rows so they read as children of
                                // the batch header above them.
                                anchors.leftMargin: jobRow.isMember ? 14 : 0
                                spacing: 6

                                // Batch header disclosure chevron (expanded vs
                                // collapsed) — a header reads as a disclosure
                                // row. Only headers show it (an invisible
                                // Layout child collapses, so standalone/member
                                // rows keep their status dot at the left edge).
                                Item {
                                    Layout.preferredWidth: 9
                                    Layout.preferredHeight: 9
                                    visible: jobRow.isHeader
                                    AppIcon {
                                        anchors.centerIn: parent
                                        name: entryColumn.expanded ? "collapse" : "expand"
                                        iconSize: 9
                                        color: "#8fa8bf"
                                    }
                                }
                                Rectangle {
                                    width: 8
                                    height: 8
                                    radius: 4
                                    color: jobRow.statusColor
                                }
                                Label {
                                    Layout.preferredWidth: 62
                                    elide: Text.ElideRight
                                    text: jobRow.kindLabel
                                    color: "#8fa8bf"
                                    font.pixelSize: 10
                                    font.bold: true
                                }
                                Label {
                                    Layout.fillWidth: true
                                    elide: Text.ElideMiddle
                                    text: jobRow.fileName
                                    color: "white"
                                    font.pixelSize: 11
                                }
                                Label {
                                    id: statusLabel
                                    Layout.preferredWidth: 96
                                    elide: Text.ElideRight
                                    horizontalAlignment: Text.AlignRight
                                    text: jobRow.statusText + (jobRow.detailText !== "" ? " (" + jobRow.detailText + ")" : "")
                                    color: "#888888"
                                    font.pixelSize: 9

                                    // O20: surface the real error message on
                                    // hover for a genuine failure (not a
                                    // cancel) — the row itself has no room
                                    // to show it inline without blowing past
                                    // the popup's fixed width.
                                    HoverHandler { id: statusHover }
                                    ToolTip.visible: statusHover.hovered
                                        && modelData.status === "failed" && !jobRow.isCancelled
                                        && modelData.error !== ""
                                    ToolTip.text: modelData.error
                                }


                                // Item 3: reorder — Queued standalone rows only
                                // (a batch reorders as a unit it doesn't expose).
                                Item {
                                    id: moveUpButton
                                    visible: jobRow.reorderable && modelData.status === "queued"
                                        && jobRow.queuedPosition > 0
                                    Layout.preferredWidth: 14
                                    Layout.preferredHeight: 14

                                    HoverHandler {
                                        id: moveUpHover
                                        cursorShape: Qt.PointingHandCursor
                                    }
                                    TapHandler {
                                        // See BottomBar.qml's main pauseButton
                                        // TapHandler for why this is needed on
                                        // a small icon target.
                                        gesturePolicy: TapHandler.ReleaseWithinBounds
                                        onTapped: bottomBar.transcriptionController.moveQueuedJob(
                                            modelData.id, jobRow.queuedPosition - 1)
                                    }
                                    HoverBorder {
                                        anchors.fill: parent
                                        active: moveUpHover.hovered
                                    }
                                    AppIcon {
                                        anchors.centerIn: parent
                                        name: "move-up"
                                        iconSize: 9
                                        color: "#dddddd"
                                    }
                                }
                                Item {
                                    id: moveDownButton
                                    visible: jobRow.reorderable && modelData.status === "queued"
                                        && jobRow.queuedPosition >= 0 && jobRow.queuedPosition < bottomBar.queuedCount() - 1
                                    Layout.preferredWidth: 14
                                    Layout.preferredHeight: 14

                                    HoverHandler {
                                        id: moveDownHover
                                        cursorShape: Qt.PointingHandCursor
                                    }
                                    TapHandler {
                                        gesturePolicy: TapHandler.ReleaseWithinBounds
                                        onTapped: bottomBar.transcriptionController.moveQueuedJob(
                                            modelData.id, jobRow.queuedPosition + 1)
                                    }
                                    HoverBorder {
                                        anchors.fill: parent
                                        active: moveDownHover.hovered
                                    }
                                    AppIcon {
                                        anchors.centerIn: parent
                                        name: "move-down"
                                        iconSize: 9
                                        color: "#dddddd"
                                    }
                                }

                                // Per-row Undo — the persistent twin of the
                                // auto undo snackbar (Main.qml's onUndoAvailable):
                                // a finished task that advertises an undo
                                // (modelData.undoable, set by jobs()) offers to
                                // enqueue its inverse right from the Tasks list,
                                // so undo stays reachable after the snackbar has
                                // expired. A batch header undoes every Done
                                // member at once via its batchId; a standalone/
                                // member undoes just itself via its own id —
                                // exactly the token undoTask() expects (it reads
                                // a non-zero token as either, unambiguously). The
                                // terminal gate keeps it hidden on a header still
                                // running (some members Done, batch not finished).
                                Item {
                                    id: undoButton
                                    visible: modelData.undoable === true
                                        && (modelData.status === "done" || modelData.status === "failed")
                                    Layout.preferredWidth: 16
                                    Layout.preferredHeight: 16

                                    HoverHandler {
                                        id: undoHover
                                        cursorShape: Qt.PointingHandCursor
                                    }
                                    TapHandler {
                                        // See the main pauseButton TapHandler for
                                        // why ReleaseWithinBounds on a small icon.
                                        gesturePolicy: TapHandler.ReleaseWithinBounds
                                        onTapped: bottomBar.transcriptionController.undoTask(
                                            jobRow.isHeader ? modelData.batchId : modelData.id)
                                    }
                                    HoverBorder {
                                        anchors.fill: parent
                                        active: undoHover.hovered
                                    }
                                    AppIcon {
                                        anchors.centerIn: parent
                                        name: "undo"
                                        iconSize: 13
                                        color: "#dddddd"
                                    }
                                    ToolTip.visible: undoHover.hovered
                                    ToolTip.text: qsTr("Undo")
                                }

                                // Item 3: one button for both "cancel"
                                // (Queued/Active — stops it, see
                                // TranscriptionController::cancelJob()'s doc
                                // comment for the immediate-vs-next-
                                // checkpoint caveat) and "dismiss"
                                // (Done/Failed — just removes the row);
                                // never both at once since they're keyed off
                                // mutually exclusive status values.
                                Item {
                                    id: closeButton
                                    Layout.preferredWidth: 16
                                    Layout.preferredHeight: 16

                                    HoverHandler {
                                        id: closeHover
                                        cursorShape: Qt.PointingHandCursor
                                    }
                                    TapHandler {
                                        gesturePolicy: TapHandler.ReleaseWithinBounds
                                        onTapped: {
                                            const terminal = modelData.status === "done"
                                                || modelData.status === "failed"
                                            // A batch header's one button acts on
                                            // the WHOLE batch (dismiss/cancel all
                                            // members); a member/standalone acts
                                            // on just itself.
                                            if (jobRow.isHeader) {
                                                if (terminal)
                                                    bottomBar.transcriptionController.dismissBatch(modelData.batchId)
                                                else
                                                    bottomBar.transcriptionController.cancelBatch(modelData.batchId)
                                            } else if (terminal) {
                                                bottomBar.transcriptionController.dismiss(modelData.fileUrl)
                                            } else {
                                                bottomBar.transcriptionController.cancelJob(modelData.id)
                                            }
                                        }
                                    }
                                    HoverBorder {
                                        anchors.fill: parent
                                        active: closeHover.hovered
                                    }
                                    AppIcon {
                                        anchors.centerIn: parent
                                        name: "close"
                                        iconSize: 13
                                        color: "#dddddd"
                                    }
                                }
                            }
                            }
                        }
                    }
                }
            }
        }
    }
}
