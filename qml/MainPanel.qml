import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import SuopTranscriber
import "TextFormat.js" as TextFormat

Rectangle {
    id: mainPanel

    property url vaultPath
    property string vaultLocalPath: ""
    property AudioPlayer player
    property AudioRecorder audioRecorder
    property TranscriptionController transcriptionController

    // availableModels()/availableSegmentationModels()/availableEmbeddingModels()
    // are untracked Q_INVOKABLEs (the trap BenchmarkPage.qml documents): a
    // declarative model: binding evaluates once and never again, so a download
    // triggered by transcribing an undownloaded model would never flip the
    // picker's status dot green. Fetch imperatively, re-fetch on modelsChanged()
    // — the same fix ModelsPage/BenchmarkPage already use.
    property var transcriptionModelList: []
    property var segmentationModelList: []
    property var embeddingModelList: []
    function refreshModelLists() {
        const c = mainPanel.transcriptionController
        mainPanel.transcriptionModelList = c ? c.availableModels() : []
        mainPanel.segmentationModelList = c ? c.availableSegmentationModels() : []
        mainPanel.embeddingModelList = c ? c.availableEmbeddingModels() : []
    }
    Component.onCompleted: mainPanel.refreshModelLists()
    Connections {
        target: mainPanel.transcriptionController
        enabled: mainPanel.transcriptionController !== null
        function onModelsChanged() { mainPanel.refreshModelLists() }
    }
    property VaultTreeModel vaultTreeModel
    property UsbGrabberController usbGrabberController
    // Set true/false only via setTopLevelScreen() below — gates the recorder
    // page below and hides the normal per-file content while it's showing (see
    // the two `visible:` bindings just above the recorder page block).
    property bool recording: false
    // Set true/false only via setTopLevelScreen() below — gates the USB
    // grabber page below, mirroring `recording` above.
    property bool usbGrabberOpen: false
    // Set true/false only via setTopLevelScreen() below — gates the benchmark
    // page below, mirroring usbGrabberOpen.
    property bool benchmarkOpen: false

    // The ONE writer of the three top-level-screen flags above. Opening any
    // screen closes every other one, so "two screens open at once" is not a
    // representable state — pass "" to close them all. Nothing outside this
    // function may assign recording/usbGrabberOpen/benchmarkOpen (guarded by
    // tst_styleguide's topLevelScreenFlags_writtenOnlyBySetTopLevelScreen).
    // Adding a fourth screen means one flag + one line here, not a new
    // precedence chain in every consumer.
    function setTopLevelScreen(name) {
        mainPanel.recording = (name === "recorder")
        mainPanel.usbGrabberOpen = (name === "usbGrabber")
        mainPanel.benchmarkOpen = (name === "benchmark")
    }

    // True when any top-level screen is showing — the one expression consumers
    // should read instead of re-deriving a three-flag chain.
    readonly property bool topLevelScreenOpen: mainPanel.recording
                                               || mainPanel.usbGrabberOpen
                                               || mainPanel.benchmarkOpen
    // The frontmatter/metadata of the transcript currently open, whether it's
    // an audio file's sibling .md or a standalone orphaned .md — one store for
    // both modes (see currentMdUrl / refreshMetadata). hasMetadata is true the
    // moment that map has any keys, so an orphaned .md with frontmatter reads
    // as transcribed exactly like an audio-backed one (this is what makes the
    // transcript editor editable for orphaned files — see transcriptArea.readOnly).
    property var metadata: ({})
    readonly property bool hasMetadata: Object.keys(mainPanel.metadata).length > 0
    // The .md URL to READ/WRITE the open transcript's data (metadata, prelude,
    // chapters, sentence/label/rename edits), regardless of mode: the orphaned
    // .md directly, or the audio file's URL (the controller resolves its sibling
    // .md), or "" when nothing is open. This is the single identity every
    // non-audio-specific read/write funnels through, so orphaned and audio files
    // travel identical code paths. (readMetadata/readChapters/setLabel/renameFile/
    // setPrelude/setSentenceTexts all accept either an audio URL or an .md URL.)
    //
    // Computed by resolvedMdUrl() from the raw source properties, NOT inlined
    // here, because refreshMetadata()/displayTitle()/currentFileStem() must call
    // that function DIRECTLY: they run from onOrphanedMdPathChanged, and reading
    // this cached property inside its own dependency's change handler can still
    // return the STALE value (QML re-evaluates the binding lazily, only after the
    // handler returns). The raw properties the function reads are always current.
    function resolvedMdUrl() {
        return mainPanel.orphanedMdPath !== ""
            ? "file://" + mainPanel.orphanedMdPath
            : (mainPanel.player ? mainPanel.player.source.toString() : "")
    }
    readonly property string currentMdUrl: mainPanel.resolvedMdUrl()
    // True from the moment a rename of the open file is fired — from this
    // panel's fields OR from elsewhere (set in onPendingRenamesRevisionChanged
    // when the controller's pending-rename store gains an entry for this file) —
    // until its RenameFileTask result comes back on onFileRenamed. renameFile()
    // is async (instant lane, second worker thread), so a field that both
    // commits on Enter AND on blur would otherwise fire a SECOND rename against
    // the already-moved old path before the first result repointed the panel —
    // the second task then hits "File does not exist." Cleared in onFileRenamed
    // (both ok and failure), which also resyncs the fields to on-disk truth.
    property bool renameInFlight: false
    // True only for an audio-backed file — gates the genuinely audio-specific
    // chrome (playback/seek row, model + sensitivity pickers, add-chapter-at-
    // playhead). NOT the same question as "is this transcribed": an orphaned .md
    // is transcribed but has no audio. Everything that is really about the
    // transcript (body, chapters, tags, prelude, editing) keys off hasMetadata
    // instead, so it behaves the same in both modes.
    readonly property bool hasAudio: mainPanel.player && mainPanel.player.source.toString() !== ""
    // "There is a transcript body to act on" — the gate for the mode-agnostic
    // transcript actions (relabel, generate chapters, copy, open). An orphaned
    // .md IS a transcript, so it always qualifies; an audio file qualifies once
    // it's transcribed (hasMetadata). This is the one place the orphaned-vs-
    // audio distinction is spelled out for "is there a transcript"; every such
    // gate reads this instead of re-deriving it.
    readonly property bool hasTranscript: mainPanel.orphanedMdPath !== "" || mainPanel.hasMetadata
    // Gates transcriptArea's editability below — mirrors
    // transcribeIconButton.isActive's own busy+currentFile check further
    // down this file. Editing while this file's transcription job is
    // actively appending live segments would fight the same text the user
    // is typing into.
    // activeIsForeground (Wave N, task system): the blocking overlay must NOT
    // appear for a Background task (a benchmark/sweep on this same file — see
    // Task::Presentation). `busy` alone is true for those too now that they're
    // real queued tasks, so gating on presentation is what keeps a benchmark
    // from freezing the panel the user is reading. This is the concrete reason
    // Presentation exists as an axis separate from "is a task running".
    // True only when an actual audio file is loaded in the player (not the
    // orphaned-md text view, and not the nothing-open state). Drives the
    // playback controls' enabled state so they can't be operated when there's
    // nothing to play. A plain binding on player.source, so it re-evaluates on
    // ANY source change — not just user open/close, but also a deletion closing
    // the current file (onFileDeleted sets player.source = "") — with no
    // imperative sync needed.
    readonly property bool hasPlayableSource: mainPanel.player
        && mainPanel.player.source.toString() !== ""
    readonly property bool transcribingThisFile: mainPanel.transcriptionController && mainPanel.player
        && mainPanel.transcriptionController.busy
        && mainPanel.transcriptionController.activeIsForeground
        && mainPanel.transcriptionController.currentFile.toString() === mainPanel.player.source.toString()
    // Item 55: true only in the window between THIS file's (re)transcription
    // job starting and the first NEW segment actually landing on disk — i.e.
    // while the OLD transcript is still all that's visible and nothing live
    // has streamed in yet. Set true whenever transcribingThisFile flips true,
    // cleared the instant the first live tick arrives for the open file (see
    // onLiveUpdateTickChanged below) — live content strictly postdates the
    // frontmatter/job start, so that tick is the reliable "the new transcript
    // has started being written" edge. Drives showTranscribingOverlay below,
    // which blurs/dims + disables the stale transcript and shows a spinner
    // until real output begins streaming.
    property bool awaitingFirstSegment: false
    readonly property bool showTranscribingOverlay: mainPanel.transcribingThisFile
        && mainPanel.awaitingFirstSegment
    // Item: the blocking overlay must name the ACTUAL task, not always
    // "Transcribing…". Keyed off currentPhase (whose currentPhaseChanged
    // fires on every phase transition, unlike currentJobKind which only
    // re-notifies on currentFileChanged and so would go stale across a
    // runAllJobs pipeline on one file). The phase→title mapping is the
    // controller's single source of truth (phaseOverlayLabel — the overlay
    // column of the one phase registry), not a switch here that a new phase
    // could forget; empty/unknown phases fall back to "Transcribing…".
    readonly property string transcribingOverlayLabel: {
        const label = mainPanel.transcriptionController
            ? mainPanel.transcriptionController.phaseOverlayLabel(mainPanel.transcriptionController.currentPhase) : ""
        return label !== "" ? label : qsTr("Transcribing…")
    }
    onTranscribingThisFileChanged: {
        mainPanel.awaitingFirstSegment = mainPanel.transcribingThisFile
        // Start following the tail the moment this file begins transcribing.
        // NOT cleared when transcribingThisFile flips false again — the final
        // batch of segments (and Parakeet's whole last window) lands via
        // onFinished AFTER busy drops, and must still scroll into view; the
        // flag is reset instead on the next file open (see onSourceChanged /
        // onOrphanedMdPathChanged). See the transcriptScrollView follow below.
        if (mainPanel.transcribingThisFile)
            mainPanel.followingTail = true
    }

    // Sticky "keep the newest line in view while this file transcribes." A
    // persistent flag + a reaction to the flickable's own contentHeight (see
    // the notePage.bodyScrollView.contentItem Connections below) — NOT a
    // per-body-change Qt.callLater, which silently stopped following the moment
    // one update added more than the near-bottom threshold (a multi-segment
    // whisper tick, or a whole Parakeet window burst): it measured "am I at the
    // bottom?" against the ALREADY-grown contentHeight, so any big jump read as
    // "the user scrolled up." Reset on file switch; set at transcription start.
    property bool followingTail: false
    // The contentHeight as of the previous growth — the follow tests
    // at-bottom-ness against THIS, not the live (already-grown) value.
    property real lastFollowContentHeight: 0

    // The per-file action icons below (regenerate label, generate chapters,
    // detect speakers, the delete trio) grey out while a job for THEIR file is
    // queued/active/paused and must re-enable the instant it clears. They used
    // to gate their `enabled` on transcriptionController.isQueued(url)
    // directly — but that is a plain Q_INVOKABLE, and reading it inside a
    // binding subscribes the binding to NOTHING. So the binding was evaluated
    // once (while the file was still Active → greyed) and then never re-ran
    // when the job left the queue, leaving the button greyed until some
    // UNRELATED re-eval happened to fire it (e.g. reopening the file — exactly
    // the "leave and re-enter to enable them" the user hit). This list is the
    // same Queued/Active/Paused set isQueued() reports, but built entirely from
    // reactive Q_PROPERTYs so every binding that reads it re-evaluates on
    // queueChanged/busyChanged/currentFileChanged: queuedFileUrls covers the
    // Queued jobs; busy+currentFile covers the single Active-or-Paused one
    // (only the running job is ever busy, and a pause leaves busy true, see
    // TranscriptionController — so this pair captures both Active and Paused).
    readonly property var busyOrQueuedUrls: {
        const c = mainPanel.transcriptionController
        if (!c)
            return []
        const urls = c.queuedFileUrls.map(u => u.toString())
        if (c.busy && c.activeOccupiesFile && c.currentFile.toString() !== "")
            urls.push(c.currentFile.toString())
        return urls
    }

    // Per-KIND twin of busyOrQueuedUrls: "<fileUrl> <kind>" for every
    // Queued/Active/Paused task, so a derived-pass button can gate on ITS OWN
    // kind (a transcribe in flight leaves label/chapters/diarize enabled). Reads
    // c.jobs (NOTIFY queueChanged) for the data, and also touches c.busy /
    // c.currentFile so the binding re-evaluates on the Queued->Active flip that
    // queueChanged alone can miss — the same reactivity fix busyOrQueuedUrls uses.
    readonly property var busyOrQueuedKeys: {
        const c = mainPanel.transcriptionController
        if (!c)
            return []
        void c.busy; void c.currentFile
        return c.jobs
            .filter(j => j.status === "Queued" || j.status === "Active" || j.status === "Paused")
            .map(j => j.fileUrl.toString() + " " + j.kind)
    }
    // "A transcribe for the open audio file is Queued/Active/Paused" — the gate
    // that makes the derived-pass buttons appear DURING transcription, before a
    // transcript exists. See busyOrQueuedKeys.
    readonly property bool transcribeInFlight: mainPanel.hasAudio
        && mainPanel.busyOrQueuedKeys.indexOf(mainPanel.player.source.toString() + " " + "transcribe") !== -1

    signal openVaultRequested()
    // Item 16 (Wave E): a tag pill was clicked — Main.qml routes this to
    // sidebar.applyTagFilter(tag).
    signal tagActivated(string tag)
    // A speaker pill was clicked — Main.qml routes this to
    // sidebar.applySpeakerFilter(name), mirroring tagActivated above.
    signal speakerActivated(string name)

    // Text-only view for an orphaned .md (no matching audio) — mutually
    // exclusive with player.source; showAudioFile()/showOrphanedMd() below
    // are the only places that should touch either. Its metadata is NOT a
    // separate store: like an audio file's, it loads into the one `metadata`
    // property via currentMdUrl, so every downstream read is mode-agnostic.
    property string orphanedMdPath: ""

    // The file this panel closed because it was deleted out from under it (see
    // the vaultTreeModel Connections' onFileDeleted) — the only memory that
    // survives the close, and so the only thing that lets an undo reopen it.
    // Without it, a restore can't tell a file the user was reading from one they
    // deleted from the tree without ever opening, and reopening the latter would
    // be the panel yanking itself onto a file it was never on.
    //
    // Cleared the instant ANY file opens (see onSourceChanged /
    // onOrphanedMdPathChanged, which reset the rest of the per-file state):
    // reopening is only the right move while the panel is still sitting in the
    // state the delete left it in. Move on to another file and an undo puts the
    // file back in the vault without dragging the panel off what you're reading.
    property url closedByDeleteUrl: ""

    // Feature 2: multi-paragraph, editable prelude (upgrades O26's old
    // read-only single-line summary) — lives in the .md BODY, not
    // metadata/frontmatter (see TranscriptionController::readPrelude()),
    // so it needs its own property, populated alongside metadata in
    // refreshMetadata() below.
    property string prelude: ""

    // Feature 3b/3c: chapter markers (see
    // TranscriptionController::readChapters()) — shared by the seek-bar tick
    // marks (3b) and the chapter list (3c), populated once alongside
    // prelude/metadata in refreshMetadata() below rather than each
    // calling readChapters() separately.
    property var chapters: []
    property bool chaptersExpanded: true

    // Item 34/35: at most ONE chapter title is in click-to-edit/selectable
    // state at a time — keyed by the chapter's REAL t0 (chapters[i].t0, the
    // same timestamp setChapterTitle/deleteChapter expect), shared by BOTH
    // the TOC row and the in-transcript heading so activating one exits the
    // other. -1 = none editing (no chapter has a negative timestamp, so it's
    // a safe sentinel even for the t0==0 first chapter). This single-state
    // approach is what kills item 34's multiple-simultaneous-highlights bug:
    // a title is a plain, non-selectable label until it becomes the one
    // active edit field, so no two rows can hold lingering selections at once.
    property real editingChapterTs: -1

    // Item 33: which chapters are collapsed in the transcript body, keyed by
    // String(chapterT0) → true. displayData below drops a collapsed chapter's
    // body sentences from BOTH the flowing text and displaySentences (keeping
    // the heading itself), so the monolithic transcriptArea string, its
    // overlay Repeaters, and searchCorpus() all stay offset-consistent.
    property var collapsedChapters: ({})

    property string searchQuery: ""
    property bool searchExpanded: false
    property int searchMatchIndex: -1
    // Item 13: total match count + this match's 1-based ordinal, for the
    // "a/b" display next to the search field. Item 7: raw start/length of
    // the CURRENT match (in the same position space as parseSentences'
    // start/length), feeding searchMatchRect below.
    property int searchMatchCount: 0
    property int searchMatchOrdinal: 0
    property int searchMatchStart: -1
    property int searchMatchLength: 0

    // Item 7: replace state. There is no separate find pattern — searchQuery
    // above IS the pattern, so the one search field drives both
    // search-as-you-type and replace and the two can never disagree about
    // what "the match" is. Consequently the mode toggles below govern search
    // too: search is a regex when findRegexEnabled and case-SENSITIVE unless
    // findCaseInsensitive (it used to be an unconditionally case-insensitive
    // literal). replaceEnabled is the checkbox that arms replace — while it's
    // false the replace row is hidden AND replaceNext()/replaceAll() no-op,
    // so the bar is a pure find until you ask for more.
    property bool replaceEnabled: false
    property string replaceText: ""
    property bool findRegexEnabled: false
    property bool findCaseInsensitive: false
    property string findReplaceError: ""

    function relativeToVault(fileUrl) {
        const filePath = fileUrl.toString()
        const prefix = "file://" + mainPanel.vaultLocalPath + "/"
        return filePath.startsWith(prefix) ? filePath.slice(prefix.length) : filePath
    }

    function stripExtension(name) {
        const dot = name.lastIndexOf(".")
        return dot > 0 ? name.substring(0, dot) : name
    }

    // Recorder page's live duration counter — audioRecorder.elapsedMs is
    // wall-clock ms since capture actually began (see AudioRecorder.h).
    function formatDuration(ms) {
        const totalSeconds = Math.floor(ms / 1000)
        const minutes = Math.floor(totalSeconds / 60)
        const seconds = totalSeconds % 60
        return (minutes < 10 ? "0" : "") + minutes + ":" + (seconds < 10 ? "0" : "") + seconds
    }

    function refreshMetadata() {
        // resolvedMdUrl() not currentMdUrl — see resolvedMdUrl()'s doc comment
        // (this runs from onOrphanedMdPathChanged, where the property is stale).
        const url = mainPanel.resolvedMdUrl()
        metadata = (url !== "" && transcriptionController)
            ? transcriptionController.readMetadata(url) : ({})
        // titleField is always-editable (no display/edit swap — see its doc
        // comment), so its `text: displayTitle()` binding is a one-shot: it's
        // severed the first time the user types. Resync explicitly on every
        // metadata refresh, skipped while the user is actively editing so
        // this never fights their in-progress typing.
        if (!titleField.activeFocus)
            titleField.text = mainPanel.displayTitle()
        // The filename subtitle is the same always-editable one-shot binding
        // (text: currentFileBase()) — resync it here too, skipping while the
        // user is typing into it, so a rename/file-switch refreshes it.
        if (!filenameSubtitle.activeFocus)
            filenameSubtitle.text = mainPanel.currentFileBase()
    }

    // Force-resync the two always-editable identity fields (title + filename
    // subtitle) to on-disk truth, IGNORING focus — unlike refreshMetadata()
    // above, which skips a focused field so it never fights in-progress typing.
    // Call this ONLY when the open file's identity actually changed underneath
    // the panel (a rename/move that landed, or the undo of one). Committing a
    // rename from titleField leaves it focused (onAccepted doesn't blur) and the
    // snackbar's Undo is a TapHandler that never takes focus — so without this
    // the focus-gated resync never fires and the panel keeps showing the old
    // typed name while the sidebar correctly reverts.
    function resyncIdentityFields() {
        titleField.text = mainPanel.displayTitle()
        filenameSubtitle.text = mainPanel.currentFileBase()
    }

    // Item P23: split out of refreshMetadata() above — prelude/chapters live
    // in the transcript BODY (not frontmatter), so populating them calls
    // readPrelude()/readChapters(), which themselves call the SAME
    // readTranscriptBody() transcriptBody/sentences now load asynchronously
    // on a cold file-open (see beginTranscriptLoad()'s doc comment) — if
    // this stayed inside refreshMetadata() and refreshMetadata() were still
    // called synchronously from onSourceChanged, a large file would still
    // block the GUI thread right here, defeating that fix entirely. So this
    // is called EXPLICITLY (not nested inside refreshMetadata()) from each
    // of its safe call sites, each with a different (all cheap) cache state:
    //  - commitRename()/commitPrelude(): editing an already-open file the
    //    user is actively looking at — body's already warm.
    //  - onFinished/onProgressChanged: the file just finished/is actively
    //    transcribing — already warm from live-tick reads.
    //  - the transcriptLoaded Connections handler below, AFTER a cold
    //    open's background load completes: also cheap by then, since that
    //    same background pass already populated TranscriptionController's
    //    body cache for this exact file.
    // It is deliberately NEVER called from onSourceChanged directly (unlike
    // metadata above, which is a cheap frontmatter-only read either
    // way) — see beginTranscriptLoad()'s doc comment for why that path
    // defers to the async completion instead.
    function refreshPreludeAndChapters() {
        // Prelude and chapters both live in the .md BODY (not frontmatter) and
        // read the same for an orphaned .md or an audio file's sibling .md —
        // readPrelude()/readChapters() only ever touch the .md, never audio —
        // so both go through the one currentMdUrl.
        mainPanel.prelude = (mainPanel.currentMdUrl !== "" && transcriptionController)
            ? transcriptionController.readPrelude(mainPanel.currentMdUrl) : ""
        mainPanel.chapters = (mainPanel.currentMdUrl !== "" && transcriptionController)
            ? transcriptionController.readChapters(mainPanel.currentMdUrl) : []
        // The prelude field is always-editable — same one-shot-binding caveat
        // as titleField, resynced here when it isn't focused.
        if (!preludeEditor.fieldFocused)
            preludeEditor.fieldText = mainPanel.prelude
    }

    // Item 17: insert a MANUAL chapter marker at the current playback
    // position and drop straight into renaming it. Player-backed only (adding
    // "at the playhead" is meaningless for an orphaned .md with no audio) —
    // the add affordance is hidden in that case. addChapter/setChapterTitle/
    // deleteChapter all emit finished(), which the Connections block below
    // routes back into refreshPreludeAndChapters(), so mainPanel.chapters
    // (and the interleaved transcript headings) repopulate on their own.
    function addChapterAtPlayhead() {
        const url = mainPanel.currentFileUrl()
        if (!mainPanel.transcriptionController || String(url) === "" || !mainPanel.player)
            return
        const seconds = mainPanel.player.position / 1000
        const title = qsTr("New chapter")
        // Item 20: undo of an add is a delete at the same timestamp.
        undoStack.push({type: "chapterEdit", op: "add", fileUrl: url,
                            timestamp: seconds, title: title, manual: true})
        mainPanel.transcriptionController.addChapter(url, seconds, title)
        mainPanel.editingChapterTs = seconds
    }

    // Item 35: commit a chapter rename via setChapterTitle (which promotes an
    // auto chapter to manual on edit, per the backend). No-op on an empty or
    // unchanged title, mirroring commitRename()/commitPrelude(). Keyed by the
    // chapter's real t0 — shared by the TOC row and the transcript heading.
    function commitChapterTitle(timestamp, oldTitle, newValue) {
        const url = mainPanel.currentFileUrl()
        if (!mainPanel.transcriptionController || String(url) === "")
            return
        const trimmed = newValue.trim()
        if (trimmed === "" || trimmed === oldTitle)
            return
        // Item 20: rename is symmetric — undo restores oldTitle, redo re-applies.
        undoStack.push({type: "chapterEdit", op: "rename", fileUrl: url,
                            timestamp: timestamp, oldTitle: oldTitle, newTitle: trimmed})
        mainPanel.transcriptionController.setChapterTitle(url, timestamp, trimmed)
    }

    // Item 17: remove a chapter marker. manual is carried into the undo entry
    // so an undo (add-back) can be recorded honestly, though addChapter always
    // re-creates a manual marker regardless.
    function deleteChapterAt(timestamp, title, manual) {
        const url = mainPanel.currentFileUrl()
        if (!mainPanel.transcriptionController || String(url) === "")
            return
        // Item 20: undo of a delete re-adds the chapter with its old title.
        undoStack.push({type: "chapterEdit", op: "delete", fileUrl: url,
                            timestamp: timestamp, title: title, manual: manual})
        mainPanel.transcriptionController.deleteChapter(url, timestamp)
        if (mainPanel.editingChapterTs === timestamp)
            mainPanel.editingChapterTs = -1
    }

    // Item 33: toggle a chapter's collapsed state in the transcript (keyed by
    // its real t0). Reassigns the whole map so collapsedChaptersChanged fires
    // and displayData re-evaluates (a plain in-place mutation wouldn't notify).
    function toggleChapterCollapse(chapterT0) {
        // Collapsing rewrites transcriptArea's WHOLE document (displayData
        // drops the collapsed chapter's sentences from `text`), but that
        // resync is skipped while the area has focus so it never fights an
        // in-progress edit (see onDisplayTextChanged). Blurring first is what
        // makes the collapse actually visible: the overlay heading sits ON the
        // transcript, so any earlier click in the body left transcriptArea
        // focused — and with it focused, the collapse used to update only the
        // overlays (headings/gutters, bound to displaySentences) while the
        // body text stayed, which read as "collapse moves the next chapter but
        // doesn't hide anything". Blur also commits any pending edit through
        // onActiveFocusChanged, so this can never drop one either.
        if (transcriptArea.activeFocus)
            transcriptArea.focus = false
        const key = String(chapterT0)
        const updated = Object.assign({}, mainPanel.collapsedChapters)
        if (updated[key])
            delete updated[key]
        else
            updated[key] = true
        mainPanel.collapsedChapters = updated
        mainPanel.persistCollapsedChapters()
    }

    // Item: persist which chapters are collapsed into the .md frontmatter, so
    // it survives reopening the file. Writes the collapse map's live keys via
    // setCollapsedChapters (which does NOT emit finished(), so this can't
    // trigger a reload cycle).
    function persistCollapsedChapters() {
        if (!mainPanel.transcriptionController)
            return
        const url = mainPanel.currentMdUrl
        if (url === "")
            return
        const keys = []
        for (const k in mainPanel.collapsedChapters)
            if (mainPanel.collapsedChapters[k])
                keys.push(k)
        mainPanel.transcriptionController.setCollapsedChapters(url, keys)
    }

    // Item: rebuild the collapse map from the file's frontmatter — called once
    // per file open (from onTranscriptLoaded, AFTER onSourceChanged/
    // onOrphanedMdPathChanged reset it to {}), so a reopened file restores its
    // collapsed chapters. Reads the `collapsedChapters:` frontmatter string
    // (comma-joined String(t0) keys) from whichever metadata map backs the
    // current view.
    function initCollapsedFromFrontmatter() {
        const meta = mainPanel.metadata
        const raw = (meta && meta.collapsedChapters) ? meta.collapsedChapters : ""
        const map = ({})
        if (raw && typeof raw === "string") {
            for (const part of raw.split(",")) {
                const k = part.trim()
                if (k !== "")
                    map[k] = true
            }
        }
        mainPanel.collapsedChapters = map
    }

    // Does a transcript selection sweep across the heading whose reserved line
    // starts at `headingStart` (a displaySentences entry's `start`)?
    //
    // A chapter title isn't text in transcriptArea's document — displayData
    // reserves a ZERO-LENGTH blank line for it and an overlay field draws the
    // title — so a native selection can never span both, and "is this title
    // inside the selection" has to be decided against that empty line instead.
    // Strictly-greater on the end is the whole subtlety: a selection stopping
    // exactly AT the line (i.e. one that ends with the previous sentence's
    // newline) hasn't reached the heading, and `>=` there would make every such
    // selection silently drag in the next chapter's title.
    function selectionSweepsHeading(headingStart) {
        return transcriptArea.selectionStart <= headingStart
            && transcriptArea.selectionEnd > headingStart
    }

    // Ctrl+C over a selection that swept across chapter headings. The titles
    // are overlay fields, not characters in transcriptArea (see
    // selectionSweepsHeading above), so the native copy silently drops every
    // title the user can plainly see highlighted — this splices them back in at
    // the offset of the blank line each one reserves. Returns false when
    // there's nothing selected, letting the native copy handle it.
    function copySelectionWithChapters() {
        const selStart = transcriptArea.selectionStart
        if (selStart === transcriptArea.selectionEnd)
            return false
        let out = transcriptArea.selectedText
        const headings = mainPanel.chapterHeadingModel
        // Descending: each splice shifts every offset after it, so inserting
        // back-to-front leaves the not-yet-used offsets correct.
        for (let i = headings.length - 1; i >= 0; --i) {
            const heading = headings[i]
            if (!mainPanel.selectionSweepsHeading(heading.start))
                continue
            const at = heading.start - selStart
            out = out.slice(0, at) + heading.chapterTitle + out.slice(at)
        }
        Clipboard.setText(out)
        return true
    }

    // Opening a file always drops any open top-level screen (recorder / USB
    // grabber / benchmark). The two show* funnels below are the ONE path every
    // file-open takes — the sidebar's onFileActivated/onOrphanedMdActivated/
    // onTranscriptMatchActivated and the bottom bar's onTaskActivated all land
    // here — so clearing the flags here covers every way a file opens. Routes
    // through setTopLevelScreen("") so there stays exactly ONE writer of the
    // three flags (see setTopLevelScreen).
    function exitTopLevelScreens() {
        mainPanel.setTopLevelScreen("")
    }

    function showAudioFile(path) {
        mainPanel.exitTopLevelScreens()
        mainPanel.orphanedMdPath = ""
        mainPanel.player.source = "file://" + path
    }

    function showOrphanedMd(path) {
        mainPanel.exitTopLevelScreens()
        mainPanel.player.source = ""
        mainPanel.orphanedMdPath = path
        // Setting orphanedMdPath fires onOrphanedMdPathChanged (which runs the
        // shared reset), but that does NOT read frontmatter — refresh the
        // unified metadata here so the title/tags/editability populate at once,
        // exactly as the player's onSourceChanged does for an audio file.
        mainPanel.refreshMetadata()
    }

    // Item 13: tags are comma-separated (see TranscriptionController's
    // frontmatter format — not a YAML list), so split+trim+drop-empties here.
    function currentTags() {
        const meta = mainPanel.metadata
        const raw = meta && meta.tags ? String(meta.tags) : ""
        if (raw === "")
            return []
        return raw.split(",").map(t => t.trim()).filter(t => t.length > 0)
    }

    // Distinct speaker tags actually present in THIS transcript's BODY, in
    // first-appearance order — the tag text itself IS the speaker's name
    // (see TranscriptionController::parseSentences' "speaker" field; there
    // is no separate frontmatter name-lookup layer any more, see
    // renameSpeaker()'s 2026-07-07 rewrite). Not shown at all for a
    // transcript with no speaker detection data (sentences[i].speaker is ""
    // in that case). This is always the TRUE on-disk identity, deliberately
    // NOT overridden by pendingSpeakerRenames below — see
    // effectiveSpeakerName()'s doc comment for why that matters.
    readonly property var distinctSpeakerIds: {
        const ids = []
        const seen = ({})
        for (const s of mainPanel.sentences) {
            if (s.speaker !== "" && !seen[s.speaker]) {
                seen[s.speaker] = true
                ids.push(s.speaker)
            }
        }
        return ids
    }

    // Optimistic-UI overlay for an in-flight renameSpeaker() call: maps the
    // speaker's TRUE on-disk name (as commitSpeakerRename() sent it) to the
    // name the user just typed, so the UI updates instantly while the
    // background file rewrite is still in progress. Deliberately keyed by
    // the ORIGINAL on-disk name rather than whatever's currently displayed —
    // that way distinctSpeakerIds/commitSpeakerRename's "oldName" argument
    // never has to chase a moving target if the user renames the same
    // speaker again before the first rewrite confirms; both requests still
    // resolve against the same real identity. Cleared (per-entry) once
    // TranscriptionController.speakerRenamed fires for that name, whether it
    // succeeded (sentencesRevision bump means the real reparse now shows the
    // new name directly) or failed (falling back to showing the true
    // on-disk name is the correct revert).
    property var pendingSpeakerRenames: ({})

    function effectiveSpeakerName(rawName) {
        return (rawName in mainPanel.pendingSpeakerRenames) ? mainPanel.pendingSpeakerRenames[rawName] : rawName
    }

    // Bumped whenever background state changes in a way that the `sentences`
    // binding wouldn't otherwise notice (see its own doc comment for why
    // metadata reference churn alone doesn't reliably re-trigger it) — a
    // successful speaker rename is exactly that case, since it rewrites the
    // transcript body sentences reads from.
    property int sentencesRevision: 0

    // A rejected speaker rename OR a failed merge (see commitSpeakerRename()/
    // onSpeakersMerged() below) and commitTranscriptEdits()'s rejection paths
    // all post to the Notifications singleton now (StyleGuide.md "Snackbars")
    // rather than each carrying its own auto-clearing inline label — they're
    // transient reports about an action that just failed, not descriptions of
    // a control's current state, so they belong in the one floating queue.

    // The file a speaker rename/merge (and tag edit) should be applied to.
    // Just currentMdUrl — kept as a function so the many .toString()-compare
    // call sites (against a signal's fileUrl) read unchanged; "" when no file
    // is open so those compares are null-safe.
    function currentFileUrl() {
        return mainPanel.currentMdUrl
    }

    // Item 2 (delete): the open file's two destructive actions. Both TRY THE
    // TRASH FIRST and only confirm if the trash refuses — the same flow, and
    // the same reasoning, as Sidebar.qml's beginDeleteFile() (see its comment
    // for why confirming a recoverable delete was friction that cried wolf).
    // A successful trash posts a snackbar with Undo instead of a prompt.
    //
    // The actual view refresh/clear happens off VaultTreeModel's
    // transcriptDeleted/fileDeleted signals (the Connections block below), so it
    // works whether the delete was triggered here OR from the Sidebar for the
    // same open file.
    property string pendingDeleteMode: "" // "transcript" | "audioOnly" | "file"

    // Shared tail, mirroring Sidebar's handleTrashResult(): Undo for whatever
    // trashed, a prompt only for whatever didn't.
    function handleTrashResult(result, mode, trashedMessage) {
        const restorable = result.restorable || []
        if (result.trashed > 0) {
            Notifications.post({
                text: trashedMessage,
                kind: "info",
                actions: restorable.length > 0 ? [{
                    label: qsTr("Undo"),
                    onTriggered: function() {
                        const back = mainPanel.vaultTreeModel.restoreFromTrash(restorable)
                        if (back < restorable.length)
                            Notifications.warn(qsTr("Only part of that could be restored — " +
                                                    "some items are no longer in the trash."))
                    }
                }] : []
            })
        }
        if ((result.untrashable || []).length > 0) {
            mainPanel.pendingDeleteMode = mode
            deleteFileConfirmPopup.open()
        }
    }

    function currentFileName() {
        const url = String(mainPanel.currentFileUrl())
        return url.substring(url.lastIndexOf("/") + 1)
    }

    // Each single-open-file delete: with a controller it's an async, undoable
    // TrashTask (generic undo snackbar; the SIDEBAR's onTrashResult owns the
    // untrashable-confirm prompt — trashResult is one global signal, so only one
    // listener may prompt or the confirm would double-open). Without a controller
    // (plain-model harnesses) it falls back to the synchronous model.trash* +
    // handleTrashResult here. A 1-element list: these calls are batch-shaped and
    // MainPanel's cluster is the single-open-file half of the CLAUDE.md parity.
    function requestDeleteTranscript() {
        const url = mainPanel.currentFileUrl()
        if (String(url) === "")
            return
        if (mainPanel.transcriptionController) {
            mainPanel.transcriptionController.trash("transcript", [url], mainPanel.vaultTreeModel.vaultPath)
            return
        }
        if (!mainPanel.vaultTreeModel)
            return
        mainPanel.handleTrashResult(
            mainPanel.vaultTreeModel.trashTranscript([url]), "transcript",
            qsTr("Moved the transcript for %1 to the trash.").arg(mainPanel.currentFileName()))
    }

    // The complement of requestDeleteTranscript(): drops the audio, keeps the
    // .md. fileDeleted (not transcriptDeleted) closes the open file — see
    // trashAudioOnly()'s comment for why the surviving .md is a different row.
    function requestDeleteAudioOnly() {
        const url = mainPanel.currentFileUrl()
        if (String(url) === "")
            return
        if (mainPanel.transcriptionController) {
            mainPanel.transcriptionController.trash("audioOnly", [url], mainPanel.vaultTreeModel.vaultPath)
            return
        }
        if (!mainPanel.vaultTreeModel)
            return
        mainPanel.handleTrashResult(
            mainPanel.vaultTreeModel.trashAudioOnly([url]), "audioOnly",
            qsTr("Moved the audio for %1 to the trash — the transcript is still here.")
                .arg(mainPanel.currentFileName()))
    }

    function requestDeleteFile() {
        const url = mainPanel.currentFileUrl()
        if (String(url) === "")
            return
        if (mainPanel.transcriptionController) {
            mainPanel.transcriptionController.trash("files", [url], mainPanel.vaultTreeModel.vaultPath)
            return
        }
        if (!mainPanel.vaultTreeModel)
            return
        mainPanel.handleTrashResult(
            mainPanel.vaultTreeModel.trashFiles([url]), "files",
            qsTr("Moved %1 to the trash.").arg(mainPanel.currentFileName()))
    }

    // Reached ONLY when the trash refused (no-controller fallback path — with a
    // controller the Sidebar owns the confirm). This really is the irreversible one.
    function confirmFileDelete() {
        if (mainPanel.vaultTreeModel) {
            const url = mainPanel.currentFileUrl()
            if (String(url) !== "") {
                if (mainPanel.pendingDeleteMode === "transcript")
                    mainPanel.vaultTreeModel.deleteTranscriptPermanently([url])
                else if (mainPanel.pendingDeleteMode === "audioOnly")
                    mainPanel.vaultTreeModel.deleteAudioOnlyPermanently([url])
                else
                    mainPanel.vaultTreeModel.deleteFilesPermanently([url])
            }
        }
        deleteFileConfirmPopup.close()
    }

    // oldName must be the speaker's TRUE on-disk name (i.e. an entry from
    // distinctSpeakerIds) — see pendingSpeakerRenames' doc comment above for
    // why the pill Repeater passes that instead of the currently-DISPLAYED
    // (possibly already-overridden) name.
    function commitSpeakerRename(oldName, newValue) {
        const trimmed = newValue.trim()
        if (trimmed === "" || !mainPanel.transcriptionController)
            return
        if (trimmed === mainPanel.effectiveSpeakerName(oldName))
            return // no-op: unchanged

        // Disallow duplicates client-side (case-insensitive) against every
        // OTHER speaker's current effective name — fast feedback without
        // waiting on the background rewrite. The C++ side re-checks against
        // the actual file too (authoritative; catches the rare case where
        // the file changed since sentences was last parsed).
        const trimmedLower = trimmed.toLowerCase()
        for (const id of mainPanel.distinctSpeakerIds) {
            if (id !== oldName && mainPanel.effectiveSpeakerName(id).toLowerCase() === trimmedLower) {
                Notifications.error(qsTr("A speaker named \"%1\" already exists.").arg(trimmed))
                return
            }
        }

        // Item P16: reverse of this op is renaming trimmed back to oldName
        // — stored ready-to-replay (not just "oldName") so undoLast() can
        // invoke renameSpeaker() directly without re-deriving anything.
        undoStack.push({type: "speakerRename", fileUrl: mainPanel.currentFileUrl(),
                            reverseOld: trimmed, reverseNew: oldName})

        const updated = Object.assign({}, mainPanel.pendingSpeakerRenames)
        updated[oldName] = trimmed
        mainPanel.pendingSpeakerRenames = updated
        mainPanel.transcriptionController.renameSpeaker(mainPanel.currentFileUrl(), oldName, trimmed)
    }

    // Reconciles a renameSpeaker() call once its background rewrite
    // completes — see commitSpeakerRename()/pendingSpeakerRenames above.
    Connections {
        target: mainPanel.transcriptionController
        // File rename is an instant-lane task now (TranscriptionController::
        // renameFile) — the call sites fire it and this repoints on the
        // result. The filename field / title stay optimistic (they already
        // show the typed name); on success we repoint the player (or the
        // orphaned-md path) to the real renamed file, on failure we revert.
        function onFileRenamed(oldUrl, newUrl, ok, error) {
            mainPanel.renameInFlight = false
            const oldStr = oldUrl.toString()
            const isOpenAudio = mainPanel.player && mainPanel.player.source.toString() === oldStr
            const isOrphaned = mainPanel.orphanedMdPath !== ""
                && ("file://" + mainPanel.orphanedMdPath) === oldStr
            if (!ok) {
                // Error snackbar is owned by Sidebar.onFileRenamed (fires for
                // every rename, panel open or not) — reporting it here too gave
                // the user TWO identical snackbars for one failure. Here we only
                // revert this panel's optimistic fields to on-disk truth.
                if (isOpenAudio || isOrphaned)
                    mainPanel.resyncIdentityFields()
                return
            }
            if (isOpenAudio)
                mainPanel.player.source = newUrl // fires onSourceChanged -> refreshMetadata
            if (isOrphaned) {
                mainPanel.orphanedMdPath = newUrl.toString().replace("file://", "")
                mainPanel.refreshMetadata()
            }
            // AFTER the repoint above (source/orphanedMdPath are what
            // displayTitle()/currentFileStem() read), and unconditionally on
            // focus — see resyncIdentityFields(). This is what makes the
            // snackbar's Undo land in the panel, not just in the sidebar.
            if (isOpenAudio || isOrphaned)
                mainPanel.resyncIdentityFields()
        }
        // Optimistic cross-panel rename: a rename fired from the SIDEBAR (or
        // anywhere) for the file open here must show the new name AT ONCE, not
        // wait for the DEFERRED fileRenamed. The controller's pending-rename
        // store is the single source; this fires on every revision bump but only
        // acts while a stem is actually pending — completion (empty store) is
        // owned by onFileRenamed above, which repoints then resyncs. A rename
        // fired from THIS panel already shows the typed name in its focused
        // field, so the focus guard (as in refreshMetadata) skips it.
        function onPendingRenamesRevisionChanged() {
            const url = mainPanel.resolvedMdUrl()
            if (url === "" || !mainPanel.transcriptionController)
                return
            const stem = mainPanel.transcriptionController.pendingRenameStem(url)
            if (stem === "")
                return // completion/failure resync is owned by onFileRenamed
            mainPanel.renameInFlight = true
            const oldBase = mainPanel.currentFileBase()
            const dot = oldBase.lastIndexOf(".")
            const ext = dot > 0 ? oldBase.substring(dot) : ""
            if (!filenameSubtitle.activeFocus)
                filenameSubtitle.text = stem + ext
            const titleIsLabel = AppSettings.useLabelsMainPanel && !!mainPanel.metadata.label
            if (!titleIsLabel && !titleField.activeFocus)
                titleField.text = AppSettings.hideExtensionsMainPanel ? stem : (stem + ext)
        }
        // File move is an instant-lane task like rename (see Sidebar's
        // performRowMove/moveNoteTo). A move changes the file's FOLDER, not its
        // name, so there's no optimistic filename field to revert on failure —
        // The title/subtitle now show only the base filename (see displayTitle/
        // currentFileBase), so an in-vault move leaves them unchanged — the
        // resync below is a harmless no-op kept for symmetry with the rename
        // path. We only repoint the player / orphaned-md path when the file that
        // moved is the one currently open. Failure is surfaced by the Sidebar's
        // own onFileMoved; nothing to do here.
        function onFileMoved(oldUrl, newUrl, ok, error) {
            if (!ok)
                return
            const oldStr = oldUrl.toString()
            const isOpenAudio = mainPanel.player && mainPanel.player.source.toString() === oldStr
            const isOrphaned = mainPanel.orphanedMdPath !== ""
                && ("file://" + mainPanel.orphanedMdPath) === oldStr
            if (isOpenAudio)
                mainPanel.player.source = newUrl // fires onSourceChanged -> refreshMetadata
            if (isOrphaned) {
                mainPanel.orphanedMdPath = newUrl.toString().replace("file://", "")
                mainPanel.refreshMetadata()
            }
            // The title/subtitle show only the base filename now, so an in-vault
            // move doesn't change them; resync stays as a harmless no-op that
            // keeps the always-editable field bindings coherent after the move.
            if (isOpenAudio || isOrphaned)
                mainPanel.resyncIdentityFields()
        }
        function onSpeakerRenamed(fileUrl, oldName, ok, error) {
            if (fileUrl.toString() !== mainPanel.currentFileUrl().toString())
                return
            if (ok) {
                mainPanel.sentencesRevision += 1
            } else {
                Notifications.error(error)
            }
            // Either way the optimistic override is no longer needed: on
            // success the reparse (sentencesRevision above) now shows the
            // real new name directly; on failure, removing the override
            // reverts the display back to oldName, the correct on-disk
            // truth.
            const updated = Object.assign({}, mainPanel.pendingSpeakerRenames)
            delete updated[oldName]
            mainPanel.pendingSpeakerRenames = updated
        }
    }

    // --- Speaker MERGE (drag one pill onto another) ---
    //
    // Optimistic-UI overlay for an in-flight mergeSpeakers() call: raw
    // on-disk ids currently mid-merge map to true here. Unlike
    // pendingSpeakerRenames, this only dims/disables the pill (see
    // speakerPillItem.pendingMerge below) rather than changing its
    // displayed name — a merge SHRINKS the real distinct-speaker count, and
    // distinctSpeakerIds is deliberately always the true on-disk set (same
    // reasoning as pendingSpeakerRenames' own doc comment), so the pill only
    // actually disappears once the background rewrite completes and
    // sentencesRevision's reparse reflects it.
    property var pendingSpeakerMerges: ({})

    // Raw id of the pill currently showing its inline "Merge into X?"
    // confirmation in place of its normal label, and the raw id it would
    // merge into if confirmed — set only for the one ambiguous case (see
    // resolveSpeakerMerge() below): both speakers involved were manually
    // renamed, so neither name obviously "wins".
    property string mergeConfirmSourceId: ""
    property string mergeConfirmTargetId: ""

    // Drag state for the pill row's manual (non-Drag/DropArea) hit-testing
    // — see speakerPillItem's DragHandler. draggingSpeakerId is the raw id
    // being dragged ("" when idle); dragHoverSpeakerId is whichever OTHER
    // pill the pointer is currently over. dragGhostX/Y (mainPanel-space)
    // position the floating name label near the end of this file that
    // visually represents the drag — the real pill is deliberately never
    // moved itself (it lives in a Flow, which doesn't defend a child's x/y
    // against being fought mid-drag, unlike a manually-positioned ghost).
    property string draggingSpeakerId: ""
    property string dragHoverSpeakerId: ""
    property real dragGhostX: 0
    property real dragGhostY: 0

    // isDefaultSpeakerName() ("Speaker N" ⇒ never manually renamed) moved to
    // TextFormat.js — a pure regex test, used just below to decide which side
    // of a merge "wins" the resulting name.

    // Resolves a drag of sourceId onto targetId (both raw on-disk ids from
    // distinctSpeakerIds) into who wins the resulting name: if BOTH were
    // manually renamed, it's ambiguous — needsConfirm, defaulting to the
    // drop target's name (the user can re-drag the other direction, or
    // reject, to keep the source's name instead). Otherwise whichever one
    // is manually renamed wins outright, even if that's the one just
    // dragged (so a custom name can never be silently clobbered by dropping
    // it onto an un-renamed "Speaker N" pill); if NEITHER was renamed, the
    // drop target wins. Returns null for a no-op self-drop.
    function resolveSpeakerMerge(sourceId, targetId) {
        if (sourceId === targetId)
            return null
        const sourceNamed = !TextFormat.isDefaultSpeakerName(mainPanel.effectiveSpeakerName(sourceId))
        const targetNamed = !TextFormat.isDefaultSpeakerName(mainPanel.effectiveSpeakerName(targetId))
        const winnerIsSource = sourceNamed && !targetNamed
        return {
            needsConfirm: sourceNamed && targetNamed,
            loserId: winnerIsSource ? targetId : sourceId,
            winnerId: winnerIsSource ? sourceId : targetId
        }
    }

    function startSpeakerMerge(loserId, winnerId) {
        if (!mainPanel.transcriptionController)
            return
        const updated = Object.assign({}, mainPanel.pendingSpeakerMerges)
        updated[loserId] = true
        mainPanel.pendingSpeakerMerges = updated
        mainPanel.transcriptionController.mergeSpeakers(mainPanel.currentFileUrl(), loserId, winnerId)
    }

    // Called when a drag lands on a DIFFERENT pill (see speakerPillItem's
    // DragHandler.onActiveChanged).
    function handleSpeakerDrop(sourceId, targetId) {
        const resolution = mainPanel.resolveSpeakerMerge(sourceId, targetId)
        if (!resolution)
            return
        if (resolution.needsConfirm) {
            mainPanel.mergeConfirmSourceId = sourceId
            mainPanel.mergeConfirmTargetId = resolution.winnerId
        } else {
            mainPanel.startSpeakerMerge(resolution.loserId, resolution.winnerId)
        }
    }

    function confirmSpeakerMerge(confirmed) {
        const sourceId = mainPanel.mergeConfirmSourceId
        const targetId = mainPanel.mergeConfirmTargetId
        mainPanel.mergeConfirmSourceId = ""
        mainPanel.mergeConfirmTargetId = ""
        if (confirmed && sourceId !== "")
            mainPanel.startSpeakerMerge(sourceId, targetId)
    }

    // Manual hit-test used by the drag handler above — checks every OTHER
    // pill's bounds (mainPanel-space) for the given point. speakersRow's
    // Repeater delegates are direct children of speakersRow itself, each
    // exposing its raw id as .modelData.
    function speakerIdAtScenePos(sceneX, sceneY, excludeId) {
        for (const child of speakersRow.children) {
            if (child.modelData === undefined || child.modelData === excludeId)
                continue
            const local = child.mapFromItem(mainPanel, sceneX, sceneY)
            if (local.x >= 0 && local.x <= child.width && local.y >= 0 && local.y <= child.height)
                return child.modelData
        }
        return ""
    }

    // Reconciles a mergeSpeakers() call once its background rewrite
    // completes — mirrors onSpeakerRenamed above.
    Connections {
        target: mainPanel.transcriptionController
        function onSpeakersMerged(fileUrl, sourceName, targetName, ok, error) {
            if (fileUrl.toString() !== mainPanel.currentFileUrl().toString())
                return
            if (ok) {
                mainPanel.sentencesRevision += 1
            } else {
                Notifications.error(error)
            }
            const updated = Object.assign({}, mainPanel.pendingSpeakerMerges)
            delete updated[sourceName]
            mainPanel.pendingSpeakerMerges = updated
        }
    }

    // Item P16 / Item 46: undo+redo for the small manual edits this panel
    // can make directly (label, prelude, tags, transcript text, speaker
    // rename) — a stack of {type, ...} entries, each recording BOTH its
    // pre-edit (oldValue) and post-edit (newValue) state so the SAME entry can
    // be replayed in either direction: undoLast() applies the "undo" side and
    // moves it to the redo stack; redoLast() applies the "redo" side and moves
    // it back. Entries are pushed BEFORE the mutation they guard. Both stacks
    // reset whenever the open file changes (see resetForFileOpen below) so
    // Ctrl+Z/Ctrl+Y never reach across a file switch into a different note's
    // history.
    // Speaker MERGE is deliberately NOT tracked here — unlike a rename, a
    // merge collapses two distinct speaker identities into one (which
    // sentences originally belonged to the losing speaker is no longer
    // recoverable from the rewritten transcript alone), so there is no
    // simple reverse operation to record.
    // The STORAGE (both stacks + push/pop + redo-invalidation) moved to the
    // C++ UndoStack (src/core/UndoStack.{h,cpp}); only the view-entangled
    // APPLY step (applyHistoryEntry below) stayed in QML. Instantiated per
    // panel (not a singleton) because an undo stack is per-panel STATE — two
    // panels must never share one history.
    UndoStack { id: undoStack }

    // Applies one history entry in the given direction ("undo" restores its
    // pre-edit value; "redo" re-applies the edit). Undo and redo share this
    // one dispatch — direction alone decides which stored value is written —
    // so the two can never drift apart, and any new entry type added here is
    // automatically redoable too (e.g. a future "chapterEdit" just needs its
    // own case; nothing else changes). Speaker rename is a from→to op rather
    // than a value set, so its two directions are the swapped ends of the
    // one rename.
    // Re-reads the open file's metadata after an undo/redo that wrote
    // frontmatter (label/prelude/tags) — mirrors the exact refresh
    // commitRename()/commitPrelude() already run after a normal edit, routed
    // by orphaned-vs-player the same way they are. Undo/redo is per-file, so
    // "current file" is always the one the entry's fileUrl refers to.
    function refreshCurrentFileMetadata() {
        mainPanel.refreshMetadata()
        mainPanel.refreshPreludeAndChapters()
    }

    function applyHistoryEntry(entry, direction) {
        if (!mainPanel.transcriptionController)
            return
        const undo = (direction === "undo")
        switch (entry.type) {
        case "label":
            mainPanel.transcriptionController.setLabel(entry.fileUrl, undo ? entry.oldValue : entry.newValue)
            // Same refresh commitRename() runs after a normal label edit, so
            // undo/redo of a label visibly re-titles the panel too — not just
            // the transcriptText/speakerRename cases, which self-refresh via
            // sentencesRevision/pendingSpeakerRenames.
            mainPanel.refreshCurrentFileMetadata()
            break
        case "prelude":
            mainPanel.transcriptionController.setPrelude(entry.fileUrl, undo ? entry.oldValue : entry.newValue)
            mainPanel.refreshCurrentFileMetadata()
            break
        case "tags":
            mainPanel.transcriptionController.setTags(entry.fileUrl, undo ? entry.oldValue : entry.newValue)
            mainPanel.refreshCurrentFileMetadata()
            break
        case "transcriptText":
            if (mainPanel.transcriptionController.setSentenceTexts(entry.fileUrl, undo ? entry.oldValue : entry.newValue))
                mainPanel.sentencesRevision += 1
            break
        case "speakerRename": {
            // reverseOld→reverseNew is the UNDO of the original rename (see
            // commitSpeakerRename); the REDO is simply the swapped ends.
            // Mirrors commitSpeakerRename()'s own optimistic overlay so the
            // pill updates instantly here too, not just via the eventual
            // background rewrite + sentencesRevision bump.
            const from = undo ? entry.reverseOld : entry.reverseNew
            const to = undo ? entry.reverseNew : entry.reverseOld
            const updated = Object.assign({}, mainPanel.pendingSpeakerRenames)
            updated[from] = to
            mainPanel.pendingSpeakerRenames = updated
            mainPanel.transcriptionController.renameSpeaker(entry.fileUrl, from, to)
            break
        }
        case "chapterEdit": {
            // Item 20: chapter add/rename/delete, undoable/redoable like every
            // other entry type. All three backend calls emit finished(), which
            // the Connections block refreshes chapters from — so, unlike the
            // label/prelude/tags cases above, no explicit
            // refreshCurrentFileMetadata() is needed here.
            const tc = mainPanel.transcriptionController
            if (entry.op === "rename") {
                tc.setChapterTitle(entry.fileUrl, entry.timestamp, undo ? entry.oldTitle : entry.newTitle)
            } else if (entry.op === "add") {
                // undo an add = delete it; redo = add it back.
                if (undo)
                    tc.deleteChapter(entry.fileUrl, entry.timestamp)
                else
                    tc.addChapter(entry.fileUrl, entry.timestamp, entry.title)
            } else if (entry.op === "delete") {
                // undo a delete = add it back with its old title; redo = delete again.
                if (undo)
                    tc.addChapter(entry.fileUrl, entry.timestamp, entry.title)
                else
                    tc.deleteChapter(entry.fileUrl, entry.timestamp)
            }
            break
        }
        }
    }

    function undoLast() {
        // The no-transcriptionController guard stays HERE (not in UndoStack):
        // the original left both stacks untouched when there was no controller
        // to apply against, so the take must not happen either. takeUndo()
        // returns an empty map (and moves nothing) when the undo stack is
        // empty — the length check the JS did.
        if (!mainPanel.transcriptionController)
            return
        const entry = undoStack.takeUndo()
        if (Object.keys(entry).length === 0)
            return
        mainPanel.applyHistoryEntry(entry, "undo")
    }

    function redoLast() {
        if (!mainPanel.transcriptionController)
            return
        const entry = undoStack.takeRedo()
        if (Object.keys(entry).length === 0)
            return
        mainPanel.applyHistoryEntry(entry, "redo")
    }

    // Item 51: clears the selection in the panel's main selectable text
    // bodies (transcript + prelude). deselect() is a no-op when there is no
    // selection, so this is always safe — it never disturbs cursor position
    // or focus. Called by the root deselect TapHandler below when a click
    // lands on a non-interactive region.
    function deselectTextSelections() {
        transcriptArea.deselect()
        preludeEditor.deselect()
        // Clicking empty background also blurs the identity fields, committing
        // any pending rename/label edit through their onActiveFocusChanged
        // (a TapHandler on a sibling does NOT move focus off a TextField on its
        // own). This is issue 1's "can't deselect by clicking empty space" and
        // issue 3's "clicking outside the checkmark commits too".
        if (titleField.activeFocus)
            titleField.focus = false
        if (filenameSubtitle.activeFocus)
            filenameSubtitle.focus = false
        // Item 34: clicking a non-interactive region also commits+exits any
        // in-progress chapter-title edit — the active title's onEditingChanged
        // handler forces its own blur (→ commit) when this clears.
        mainPanel.editingChapterTs = -1
    }

    function displayTitle() {
        // resolvedMdUrl() not currentMdUrl — refreshMetadata() calls this from
        // the file-open change handler, where the property is stale.
        const url = mainPanel.resolvedMdUrl()
        if (url === "")
            return qsTr("No file selected")
        if (AppSettings.useLabelsMainPanel && mainPanel.metadata.label)
            return mainPanel.metadata.label
        // Only ever the base filename (+ extension), never containing folders —
        // relativeToVault() gives the vault-relative path; take its last segment
        // (mirrors currentFileStem()/currentFileBase()).
        const rel = mainPanel.relativeToVault(url)
        const base = rel.substring(rel.lastIndexOf("/") + 1)
        return AppSettings.hideExtensionsMainPanel ? mainPanel.stripExtension(base) : base
    }

    function commitRename(newValue) {
        const trimmed = newValue.trim()
        if (trimmed === "" || !mainPanel.transcriptionController) {
            // titleField.text lost its declarative binding to displayTitle()
            // the moment the user typed into it (QML severs a `text: expr`
            // binding on the first imperative write) — resync explicitly here
            // and at every other exit path below so it always reflects the
            // real on-disk value once the field loses focus.
            titleField.text = mainPanel.displayTitle()
            return
        }

        const url = mainPanel.currentMdUrl
        if (url === "")
            return

        // The title shows the AI label ONLY when labels mode is on AND this
        // file actually has one (mirrors displayTitle()/filenameSubtitle.visible).
        // In that case the field edits the label; otherwise the title IS the
        // filename, so editing renames the file — a raw audio file (issue 1) or
        // a transcribed file with no label yet (issue 2), never inventing a label.
        if (AppSettings.useLabelsMainPanel && mainPanel.metadata.label) {
            const oldLabel = mainPanel.metadata.label || ""
            if (trimmed !== oldLabel)
                undoStack.push({type: "label", fileUrl: url, oldValue: oldLabel, newValue: trimmed})
            mainPanel.transcriptionController.setLabel(url, trimmed)
            mainPanel.refreshCurrentFileMetadata()
        } else {
            // titleField shows the base filename WITH extension (see
            // displayTitle), but renameFile() takes a STEM and re-appends the
            // file's real extension — so strip the typed extension before
            // renaming (a changed extension is ignored; the file keeps its own),
            // and no-op when the stem is unchanged so merely focusing+blurring
            // the title never fires a rename (unguarded that renames X.ext ->
            // X.ext.ext). Mirrors filenameSubtitle's commitFilenameRename().
            const newStem = mainPanel.stripExtension(trimmed)
            if (newStem === "" || newStem === mainPanel.currentFileStem()) {
                titleField.text = mainPanel.displayTitle()
                return
            }
            // Instant-lane rename task — onFileRenamed repoints the player (for
            // an audio file) or orphanedMdPath (for an orphaned .md) and
            // refreshes on success. Guard against a blur firing a second rename
            // against the already-moved old path while the first is in flight
            // (see renameInFlight).
            if (mainPanel.renameInFlight) {
                titleField.text = mainPanel.displayTitle()
                return
            }
            mainPanel.renameInFlight = true
            mainPanel.transcriptionController.renameFile(url, newStem)
        }
    }

    // The base filename stem (no directory, no extension) of the open file —
    // what renameFile() takes as its new-name argument. The editable filename
    // subtitle shows currentFileBase() (stem + extension) instead and strips
    // the extension back off on commit (see commitFilenameRename).
    function currentFileStem() {
        // resolvedMdUrl() not currentMdUrl — refreshMetadata() resyncs the
        // filename subtitle from this at file-open time, where the property is
        // stale (see resolvedMdUrl()'s doc comment).
        const url = mainPanel.resolvedMdUrl()
        if (url === "")
            return ""
        const rel = mainPanel.relativeToVault(url)
        const base = rel.substring(rel.lastIndexOf("/") + 1)
        return mainPanel.stripExtension(base)
    }

    // The base filename WITH extension (no directory) — what the filename
    // subtitle displays, so it names the real on-disk file. renameFile() still
    // takes a stem, so commitFilenameRename() strips the extension back off.
    function currentFileBase() {
        const url = mainPanel.resolvedMdUrl()
        if (url === "")
            return ""
        const rel = mainPanel.relativeToVault(url)
        return rel.substring(rel.lastIndexOf("/") + 1)
    }

    // Item: the filename subtitle (shown when the title is an AI label) is an
    // editable rename control for the REAL file. renameFile() renames the
    // audio file AND its sibling .md together (or a standalone orphaned .md),
    // then onFileRenamed repoints player.source / orphanedMdPath, which
    // refreshes both the title and this subtitle. The subtitle is disabled
    // while busy (see its `enabled`), so a rename can't race a running job; on
    // any no-op/failure we revert the field to the real stem. No undo entry —
    // file renames aren't on the undo stack (matches commitRename's file-rename
    // branch).
    function commitFilenameRename(newValue) {
        // The field shows the full base name (stem + ext), but renameFile()
        // takes a STEM and re-appends the real extension itself — so strip the
        // extension the user typed back off before renaming (any changed
        // extension is ignored; the file keeps its real one).
        const newStem = mainPanel.stripExtension(newValue.trim())
        const oldStem = mainPanel.currentFileStem()
        if (mainPanel.renameInFlight || !mainPanel.transcriptionController || !mainPanel.vaultTreeModel
                || mainPanel.currentMdUrl === ""
                || newStem === "" || newStem === oldStem) {
            filenameSubtitle.text = mainPanel.currentFileBase()
            return
        }
        mainPanel.renameInFlight = true
        // Instant-lane rename task (see onFileRenamed above): the field is
        // already optimistic; the player/orphanedMdPath repoints on success and
        // the field reverts on failure, both in onFileRenamed.
        mainPanel.transcriptionController.renameFile(mainPanel.currentMdUrl, newStem)
    }

    // Autosaves the prelude editor below on blur (see preludeArea's
    // onActiveFocusChanged) — no explicit Save/Cancel; plain Enter just
    // inserts a newline since the prelude is multi-line, same as
    // transcriptArea's own blur-commit below. Writes through currentMdUrl, so
    // it works for an orphaned .md exactly as for an audio file. setLabel()'s
    // failures are already silently ignored by commitRename() above; matched
    // here for consistency.
    function commitPrelude(newValue) {
        if (!mainPanel.transcriptionController || mainPanel.currentMdUrl === "") {
            preludeEditor.fieldText = mainPanel.prelude
            return
        }
        if (newValue === mainPanel.prelude)
            return
        undoStack.push({type: "prelude", fileUrl: mainPanel.currentMdUrl, oldValue: mainPanel.prelude, newValue: newValue})
        mainPanel.transcriptionController.setPrelude(mainPanel.currentMdUrl, newValue)
        mainPanel.refreshMetadata()
        mainPanel.refreshPreludeAndChapters()
    }

    // Commits transcriptArea's edited text back to the vault file — called
    // on blur (see transcriptArea's onActiveFocusChanged). newText is the
    // WHOLE document; Return is blocked from inserting a newline and pasted
    // newlines are collapsed (see transcriptArea's Keys.onReturnPressed and
    // onTextChanged) specifically to preserve the one-line-per-sentence
    // invariant displaySentences' char offsets depend on — the line-count
    // check below is a defensive fallback, not the primary guard.
    function commitTranscriptEdits(newText) {
        if (!mainPanel.transcriptionController || mainPanel.currentMdUrl === "")
            return
        const lines = newText.split("\n")
        const entries = mainPanel.displaySentences
        if (lines.length !== entries.length) {
            Notifications.error(qsTr("Edit not saved: line count changed unexpectedly."))
            transcriptArea.text = mainPanel.displayText
            return
        }

        const edits = []
        const oldValues = []
        for (let i = 0; i < entries.length; i++) {
            const entry = entries[i]
            if (entry.isChapterHeading || entry.isSilenceMarker)
                continue // blank reserved line — not editable content, see displayData
            // entry.text may carry a trailing " (Nx)" repeat-count marker
            // beyond entry.length (see displayData's doc comment) — derive
            // it generically and strip it back off the (possibly edited)
            // line before treating the rest as the new spoken-words text, so
            // the marker itself is never sent as part of an edit.
            const suffix = entry.text.length > entry.length ? entry.text.slice(entry.length) : ""
            let line = lines[i]
            if (suffix !== "" && line.endsWith(suffix))
                line = line.slice(0, line.length - suffix.length)
            const original = entry.text.slice(0, entry.length)
            if (line !== original) {
                edits.push({t0: entry.t0, text: line})
                oldValues.push({t0: entry.t0, text: original})
            }
        }
        if (edits.length === 0)
            return // no-op: nothing actually changed

        undoStack.push({type: "transcriptText", fileUrl: mainPanel.currentMdUrl, oldValue: oldValues, newValue: edits})
        const ok = mainPanel.transcriptionController.setSentenceTexts(mainPanel.currentMdUrl, edits)
        if (ok) {
            mainPanel.sentencesRevision += 1
        } else {
            Notifications.error(qsTr("Edit not saved: could not write the file."))
            transcriptArea.text = mainPanel.displayText
        }
    }

    // Item P15/P20: adds/removes one tag from the current file's list and
    // rewrites the whole list via setTags() (there is no single-tag add/
    // remove call — TranscriptionController always writes the full,
    // comma-joined set). Works for both the orphaned-md and player-backed
    // views via currentFileUrl(), same as the speaker rename/merge helpers
    // above.
    function addTag(raw) {
        const trimmed = raw.trim()
        if (!mainPanel.transcriptionController || trimmed === "")
            return
        const current = mainPanel.currentTags()
        if (current.some(t => t.toLowerCase() === trimmed.toLowerCase()))
            return // no-op: duplicate (case-insensitive)
        undoStack.push({type: "tags", fileUrl: mainPanel.currentFileUrl(), oldValue: current, newValue: current.concat([trimmed])})
        mainPanel.transcriptionController.setTags(mainPanel.currentFileUrl(), current.concat([trimmed]))
    }

    function removeTag(tag) {
        if (!mainPanel.transcriptionController)
            return
        const current = mainPanel.currentTags()
        undoStack.push({type: "tags", fileUrl: mainPanel.currentFileUrl(), oldValue: current, newValue: current.filter(t => t !== tag)})
        mainPanel.transcriptionController.setTags(mainPanel.currentFileUrl(), current.filter(t => t !== tag))
    }

    // Inline rename of an existing tag pill (mirrors commitSpeakerRename's
    // shape) — maps onto the same bulk setTags(wholeArray) API rather than a
    // dedicated C++ call, since there is no single-tag rename primitive (nor
    // any need for one: the whole array is small and already rewritten
    // wholesale by add/removeTag above).
    function renameTag(oldTag, newValue) {
        const trimmed = newValue.trim()
        if (trimmed === "" || !mainPanel.transcriptionController)
            return
        if (trimmed === oldTag)
            return // no-op: unchanged
        const current = mainPanel.currentTags()
        if (current.some(t => t !== oldTag && t.toLowerCase() === trimmed.toLowerCase()))
            return // no-op: duplicate (case-insensitive) — silent, matches addTag()'s own dup handling
        const idx = current.indexOf(oldTag)
        if (idx < 0)
            return
        const updated = current.slice()
        updated[idx] = trimmed
        undoStack.push({type: "tags", fileUrl: mainPanel.currentFileUrl(), oldValue: current, newValue: updated})
        mainPanel.transcriptionController.setTags(mainPanel.currentFileUrl(), updated)
    }

    // Item P23: transcriptBody/sentences used to be readonly computed
    // properties that called readTranscriptBody()/parseSentences()
    // DIRECTLY inside their binding — on a large already-transcribed file,
    // that meant a full synchronous regex-parse-the-whole-file pass ran on
    // the GUI thread the instant the file was opened (the binding
    // re-evaluates the moment player.source/orphanedMdPath changes),
    // blocking the UI for however long that took. Now plain stored
    // properties, populated three different ways depending on WHY they're
    // changing — a cold file-open goes through the async path (large-file
    // case, must not block); a live-tick/small-edit refresh stays
    // synchronous (already cheap via the existing prefix-diff/stat caches,
    // see TranscriptionController's m_bodyCache/m_sentenceCache doc
    // comments — no benefit to making these async too):
    //   1. beginTranscriptLoad() — called on file switch (see
    //      onSourceChanged/onOrphanedMdPathChanged below). Clears both to
    //      empty immediately (so the panel doesn't show the PREVIOUS file's
    //      content while the new one loads) and kicks off
    //      transcriptionController.loadTranscriptAsync() in the background,
    //      UNLESS the file has no stored metadata yet (nothing to load —
    //      matches the original binding's instant "" / [] for an
    //      untranscribed file).
    //   2. The transcriptLoaded Connections handler below applies the
    //      result — but only if fileUrl still matches the file that's
    //      CURRENTLY open (mainPanel.transcriptLoadingFor), in case the user
    //      switched files again before the background pass finished.
    //   3. refreshTranscriptSync() — called on liveUpdateTick (gated on
    //      currentFile matching, same condition the original binding used),
    //      on sentencesRevision bumps, and on hasMetadata flipping
    //      true (frontmatter now exists — matches the moment the original
    //      binding's dependency on hasMetadata would have started
    //      returning real content instead of ""/[]). All three are either
    //      small edits to an already-open file or an early/small live
    //      transcript — cheap synchronous calls, exactly as before.
    property string transcriptBody: ""
    property var sentences: []
    // Which fileUrl (string form) the most recent beginTranscriptLoad() is
    // waiting on — "" when nothing is in flight (untranscribed file, no
    // transcriptionController, etc.).
    property string transcriptLoadingFor: ""

    // The .md URL to load the transcript body/sentences from, or "" when
    // there's nothing to load yet. An orphaned .md IS the open file, so it
    // always loads; an audio file loads only once its sibling .md exists
    // (hasMetadata) — before it's transcribed there is no transcript to read
    // and loadTranscriptAsync(audioUrl) would find no .md.
    function transcriptLoadKey() {
        if (mainPanel.orphanedMdPath !== "")
            return "file://" + mainPanel.orphanedMdPath
        if (mainPanel.hasAudio && mainPanel.hasMetadata)
            return mainPanel.player.source.toString()
        return ""
    }

    function beginTranscriptLoad() {
        mainPanel.transcriptBody = ""
        mainPanel.sentences = []
        mainPanel.transcriptLoadingFor = ""
        // Item P23: prelude/chapters are per-file BODY content, populated only
        // by refreshPreludeAndChapters() — which deliberately can't run on this
        // path (see its doc comment) and never runs at all for a file with
        // nothing to load (untranscribed, or no file). Clearing here is what
        // keeps the previous file's prelude/chapters from surviving the switch
        // and rendering as if they belonged to the newly opened file; the
        // transcriptLoaded handler below repopulates them for files that do
        // load.
        mainPanel.prelude = ""
        mainPanel.chapters = []
        if (!preludeEditor.fieldFocused)
            preludeEditor.fieldText = ""
        if (!mainPanel.transcriptionController)
            return
        const key = mainPanel.transcriptLoadKey()
        if (key !== "") {
            mainPanel.transcriptLoadingFor = key
            mainPanel.transcriptionController.loadTranscriptAsync(key)
        }
        // else: no file, or an untranscribed audio file with nothing to load
        // yet — stays cleared until hasMetadata flips true (see
        // onHasMetadataChanged below).
    }

    function refreshTranscriptSync() {
        if (!mainPanel.transcriptionController) {
            mainPanel.transcriptBody = ""
            mainPanel.sentences = []
            return
        }
        const key = mainPanel.transcriptLoadKey()
        if (key === "") {
            mainPanel.transcriptBody = ""
            mainPanel.sentences = []
            return
        }
        mainPanel.transcriptBody = mainPanel.transcriptionController.readTranscriptBody(key)
        mainPanel.sentences = mainPanel.transcriptionController.parseSentences(key)
    }

    onSentencesRevisionChanged: mainPanel.refreshTranscriptSync()
    onHasMetadataChanged: mainPanel.refreshTranscriptSync()

    Connections {
        target: mainPanel.transcriptionController
        function onTranscriptLoaded(fileUrl, body, loadedSentences) {
            // isSameFile compares decoded local paths, not toString(): the
            // orphaned-md load key is a raw "file://"+path string (literal ' '),
            // but the emitted fileUrl round-trips through QUrl and stringifies a
            // space as "%20" — a toString() compare read them as different files
            // and discarded EVERY load for any file whose path has a space, so it
            // rendered permanently blank. Same seam as the live-reload guard
            // below; see isSameFile()'s doc comment.
            if (!mainPanel.transcriptionController.isSameFile(fileUrl, mainPanel.transcriptLoadingFor))
                return // superseded by a later file switch — discard
            mainPanel.transcriptBody = body
            mainPanel.sentences = loadedSentences
            // Item P23: the cold-open path defers prelude/chapters to here (see
            // refreshPreludeAndChapters()'s doc comment) — this background load
            // just warmed the body cache for this exact file, so the reads it
            // does are cheap and can't block the GUI thread.
            mainPanel.refreshPreludeAndChapters()
            // Restore which chapters were collapsed from frontmatter, now that
            // chapters exist and the per-file reset (onSourceChanged/
            // onOrphanedMdPathChanged) has run. Once per open — not in
            // refreshPreludeAndChapters(), which also fires on every chapter
            // edit and would clobber live toggles.
            mainPanel.initCollapsedFromFrontmatter()
        }
        // Item 10/P23: same live-update gating the old computed bindings
        // used (currentFile matching the open file) — a synchronous
        // refresh here is cheap (small incremental parse via the existing
        // prefix-diff cache), unlike the cold-open case above.
        function onLiveUpdateTickChanged() {
            if (mainPanel.player
                    && mainPanel.transcriptionController.currentFile.toString() === mainPanel.player.source.toString()) {
                // Item 32: a live tick means the first segment has been
                // written, which strictly postdates the frontmatter written
                // at job START — so hasMetadata is guaranteed readable
                // by now. onProgressChanged (the other metadata-refresh site)
                // can fire during model-load, BEFORE the frontmatter flush,
                // so on a file's very FIRST transcription its regenerate
                // label/prelude/chapters buttons (gated on hasMetadata)
                // could stay hidden until the next progress tick or a full
                // reopen. Refreshing metadata here — on the definitely-
                // post-frontmatter content edge — closes that hole and also
                // guarantees refreshTranscriptSync() below sees hasMetadata
                // true (it gates the body read on it), so live output shows.
                mainPanel.refreshMetadata()
                // Item 55: the new transcript has started being written — drop
                // the blur/spinner overlay so live output streams in visibly.
                mainPanel.awaitingFirstSegment = false
                mainPanel.refreshTranscriptSync()
            }
        }
    }

    // User request: selection should skip over timestamps entirely — the
    // only way to make a contiguous text selection truly SKIP characters
    // (not just visually de-emphasize them) is to not have them in the
    // selectable text at all. So transcriptArea's actual text content is
    // this cleaned, timestamp-AND-speaker-free version (one sentence per
    // line, spoken words only) — timestamps and speaker names both render
    // separately, in their OWN gutter columns (see timestampGutterRepeater/
    // speakerGutterRepeater below), neither of which is part of the
    // TextArea's text and so can never be selected. start/length here are
    // offsets into THIS string (recomputed — the raw C++ parseSentences()
    // start/length index the RAW body, which includes the timestamp/speaker
    // text this display intentionally omits).
    //
    // showGutter: one timestamp+speaker-name pair per SECTION, not per raw
    // whisper segment — a "section" is a run of consecutive same-speaker
    // sentences (a turn); only the first sentence of a turn shows the
    // gutters, later ones in the same turn show neither (they're still each
    // their own line/paragraph for accurate sentence-highlight sync, just
    // without repeating the same timestamp+name down the column). Files
    // with no speaker detection (speaker is always "") have no "turns" to
    // group by, so every sentence counts as its own section there.
    //
    // speaker here is the EFFECTIVE (pending-rename-applied) display name —
    // distinctSpeakerIds/commitSpeakerRename still work off the raw on-disk
    // sentences[i].speaker directly (see effectiveSpeakerName()'s doc
    // comment for why the override is deliberately NOT baked in there).
    // Item P32: chapter headings (mainPanel.chapters, each {t0, title}) are
    // interleaved into this SAME list in time order, as zero-length pseudo-
    // "sentence" entries (isChapterHeading: true) that reserve one real
    // blank line in `text` for themselves — a genuine document line, not
    // just an overlay, is what gives the heading its own row of vertical
    // space in the flowing transcriptArea (the same reason gutter
    // timestamps/speaker names DON'T get their own line: those are pure
    // decoration with nothing to reserve space for; a chapter heading is a
    // real structural break, so it needs the opposite treatment here).
    // Being zero-length (text:"", length:0) means it contributes nothing to
    // searchCorpus() or transcriptArea's selectable content — it only ever
    // shows up via chapterHeadingRepeater below, keyed off the same
    // displaySentences index as the gutter Repeaters.
    //
    // A heading's t0 is clamped to never exceed the sentence it precedes,
    // so ties always resolve in the REAL sentence's favor: C++'s
    // activeSentenceIndex() is a plain std::upper_bound over t0 (see its
    // own doc comment) — for two list entries sharing the same t0, that
    // resolves to the LATER one once playback reaches it, so a heading can
    // only ever be "active" during genuine silence before its own
    // chapter's first real sentence, never stealing an already-playing
    // sentence's highlight.
    //
    // The ~140 lines of JS that assembled this list (chapter/silence
    // interleaving, collapse filtering, speaker-override application, the
    // "(Nx)" repeat marker, the char-offset bookkeeping) now live in
    // src/core/TranscriptDisplayBuilder.cpp — see that file for the full
    // per-step rationale; it's a straight, comment-for-comment port. This
    // is the single hottest binding in the file (re-run on every sentence
    // edit, chapter change, collapse toggle, speaker rename and silence
    // parse), and it was pure data-in/data-out, so it moved wholesale.
    //
    // It deliberately stays a `readonly property var` over exactly the same
    // inputs it always read, rather than becoming a function called from
    // handlers: MainPanel has a documented stale-derived-binding hazard (see
    // resolvedMdUrl) and every downstream consumer — displayText,
    // displaySentences, searchCorpus, the four overlay Repeaters — depends on
    // this re-evaluating at precisely the moments it used to.
    //
    // pendingSpeakerRenames is now passed EXPLICITLY (the old JS reached it
    // indirectly through effectiveSpeakerName(), which is nothing but a
    // lookup in that map). That makes it an unconditional dependency where it
    // used to be one only when some sentence actually had a speaker — a
    // strict superset, and inert in practice: a transcript with no speaker
    // detection also has no pending renames to change.
    readonly property var displayData: TranscriptDisplayBuilder.build(
        mainPanel.sentences,
        mainPanel.chapters,
        mainPanel.collapsedChapters,
        mainPanel.pendingSpeakerRenames,
        mainPanel.silenceSpans,
        mainPanel.silenceMarkerMinSeconds)
    readonly property string displayText: mainPanel.displayData.text
    readonly property var displaySentences: mainPanel.displayData.sentences

    // The four overlay Repeaters below (timestamp gutter, speaker gutter,
    // speaker-dim, chapter headings) used to take displaySentences DIRECTLY
    // as their model and hide the entries they don't want with `visible:`.
    // That instantiated 4 delegates per sentence — 16k QQuickItems on a 4k-
    // sentence transcript — synchronously on the GUI thread the moment a file
    // opened, and `visible: false` doesn't save any of that: a Repeater builds
    // every delegate regardless, `visible` only skips the painting.
    //
    // Measured (offscreen QQuickView reproduction of this exact structure, 4k
    // sentences, cold open): 661ms with the raw model vs 130ms with these
    // filtered ones — ~88% of a large note's open cost was delegate
    // instantiation, and most of it was for items that could never be seen:
    //   - speakerDim is gated on gutterSpeakerFilter, which resets to "" on
    //     every file open (see onSourceChanged/onOrphanedMdPathChanged) — so
    //     it built N Rectangles that were ALL invisible, every single open.
    //   - chapterHeading showed only the handful of real chapter markers, but
    //     built a full RowLayout+AppIcon+TextEdit for every sentence.
    //   - speakerGutter needs speaker !== "", so on a transcript with no
    //     speaker detection it built N TextEdits that were all invisible.
    // Only the timestamp gutter was ever genuinely one-per-sentence.
    //
    // NOTE this is emphatically NOT something a worker thread could have
    // fixed — QQuickItems can only be created on the GUI thread. The file
    // read + regex parse were already moved off-thread (see item P23 /
    // loadTranscriptAsync); this was the rest of the cost.
    //
    // Each entry carries `idx` (its own index into displaySentences, assigned
    // in displayData above) so a filtered delegate can still look up
    // transcriptArea.gutterLineRects[modelData.idx] — the delegate's own
    // `index` is its position in the FILTERED list and must never be used for
    // that lookup. Every filter condition below is exactly the `visible:`
    // expression the corresponding delegate used to carry.
    // Item: timestamps display on EVERY sentence line (one per segment), while
    // speaker names stay one-per-turn (only when the speaker changes). So the
    // timestamp gutter takes every non-heading, non-silence-marker sentence
    // (silence phase 3's divider is decoration too, same as a heading — see
    // pushSilenceMarker's doc comment); the speaker gutter keeps the
    // showGutter (turn-boundary) gate, which a silence marker already fails
    // on its own (showGutter: false), so it needs no explicit exclusion here.
    readonly property var gutterTimestampModel:
        mainPanel.displaySentences.filter(s => !s.isChapterHeading && !s.isSilenceMarker)
    readonly property var gutterSpeakerModel:
        mainPanel.displaySentences.filter(s => s.showGutter && s.speaker !== "")
    // Recomputes when the user toggles a speaker filter (empty model while no
    // filter is active, which is the state every file open starts in).
    readonly property var speakerDimModel:
        mainPanel.gutterSpeakerFilter === "" ? []
            : mainPanel.displaySentences.filter(
                s => !s.isChapterHeading && !s.isSilenceMarker && s.speaker !== mainPanel.gutterSpeakerFilter)
    readonly property var chapterHeadingModel:
        mainPanel.displaySentences.filter(s => s.isChapterHeading)
    // Silence phase 3 (queue item 085): one entry per divider reserved by
    // pushSilenceMarker above, keyed off the same idx the timestamp/speaker
    // gutters use (see silenceMarkerRepeater below the chapter-heading one).
    readonly property var silenceMarkerModel:
        mainPanel.displaySentences.filter(s => s.isSilenceMarker)
    // transcriptArea is always-editable (see its doc comment) — its
    // `text: mainPanel.displayText` binding is a one-shot, severed the
    // moment the user types (same caveat as titleField/preludeArea above).
    // displayText is a plain property (unlike displayTitle()/prelude's
    // refresh-function-driven resync), so its own change signal is the
    // simplest resync hook — skipped while focused so this never fights an
    // in-progress edit.
    onDisplayTextChanged: if (!transcriptArea.activeFocus) transcriptArea.text = mainPanel.displayText

    // Gutter column layout (Item: separate gutter areas for timestamp vs.
    // speaker name, kept as named constants so both are easy to re-style/
    // resize independently later).
    // Sized to the widest timestamp label actually shown (one per turn, see
    // showGutter) — same approach as speakerGutterWidth below, so a file
    // with only short "in-file" timestamps ("3.5") gets a narrow column
    // instead of always reserving room for "1:23:45.6".
    readonly property real timestampGutterWidth: {
        let maxWidth = 0
        for (const s of mainPanel.displaySentences) {
            if (!s.showGutter)
                continue
            maxWidth = Math.max(maxWidth, timestampGutterFontMetrics.advanceWidth(mainPanel.formatGutterTimestamp(s.t0)))
        }
        return maxWidth
    }
    // Sized to the widest speaker name actually shown (renamed or not) —
    // 0 when the transcript has no speaker detection at all, so no gutter
    // (and no spacing gap) is reserved for a column nothing will use.
    // Clicking a speaker name IN THE GUTTER (as opposed to clicking a
    // speaker PILL above, which filters the sidebar — see speakerActivated)
    // dims every OTHER speaker's lines in the currently-open transcript,
    // toggled off by clicking the same name again. Deliberately does not
    // touch transcriptArea's actual text/selection at all (see
    // speakerDimOverlayRepeater below) — the document stays one continuous
    // editable string throughout, only a visual overlay changes, so this
    // can't conflict with Step C's always-editable transcript.
    property string gutterSpeakerFilter: ""

    readonly property real speakerGutterWidth: {
        if (mainPanel.distinctSpeakerIds.length === 0)
            return 0
        let maxWidth = 0
        for (const id of mainPanel.distinctSpeakerIds)
            maxWidth = Math.max(maxWidth, speakerGutterFontMetrics.advanceWidth(mainPanel.effectiveSpeakerName(id)))
        return maxWidth
    }
    readonly property real gutterSpacing: 8

    FontMetrics {
        id: timestampGutterFontMetrics
        font.pixelSize: 10
    }

    FontMetrics {
        id: speakerGutterFontMetrics
        font.pixelSize: 10
        font.bold: true
    }

    // Largest displayed segment start in the open file — governs how far the
    // gutter timestamps below are zero-padded so every row aligns to one
    // column width (see formatGutterTimestamp). Every chapter t0 is also a
    // segment boundary, so displaySentences alone bounds the max the gutter
    // ever shows.
    readonly property real gutterMaxT0: {
        let maxT0 = 0
        for (const s of mainPanel.displaySentences)
            maxT0 = Math.max(maxT0, s.t0)
        return maxT0
    }

    // Compact gutter timestamp: only the start time (no range), zero-padded to
    // fit the file's LARGEST timestamp (gutterMaxT0) so the column reads as an
    // aligned ruler. The full padding-tier spec, and the note that
    // AppSettings.timestampDecimalPlaces is DISPLAY-only (stored data always
    // keeps full precision — see formatTimestamp() in
    // TranscriptionController.cpp, do not conflate the two), live with the
    // body in TextFormat.js. This wrapper is where its two former
    // HIDDEN reads become explicit: AppSettings.timestampDecimalPlaces, and the
    // derived mainPanel.gutterMaxT0 above — the latter was consumed only deep in
    // the padding-tier block, not near the top with the rest of the inputs.
    // Kept as a mainPanel method (not inlined at the three call sites) so the
    // gutterMaxT0 read stays in exactly one place, and so
    // tst_mainpanel_guttertimestamp can still invoke it with a bare t0.
    function formatGutterTimestamp(t0) {
        return TextFormat.formatGutterTimestamp(t0, mainPanel.gutterMaxT0,
                                                AppSettings.timestampDecimalPlaces)
    }

    // Index of the first REAL (spoken) sentence in displaySentences, skipping
    // any chapter heading(s) that open the transcript; -1 when there are no
    // spoken sentences at all (only headings, or an empty transcript). The
    // playback highlight's front-edge clamp uses this so a leading chapter
    // heading never gets lit.
    readonly property int firstRealSentenceIndex: {
        const list = mainPanel.displaySentences
        for (let i = 0; i < list.length; ++i)
            if (!list[i].isChapterHeading && !list[i].isSilenceMarker)
                return i
        return -1
    }

    // player.position is milliseconds; segment timestamps are seconds.
    // This binding re-evaluates automatically on player.positionChanged
    // (QML property binding dependency tracking) — binary search lives in
    // C++ (activeSentenceIndex), not linear-scanned here, per DESIGN.md.
    readonly property int activeSentenceIndex: {
        // Must index into displaySentences (the FILTERED list — blank
        // segments dropped, see displayData), not the raw C++ sentences,
        // since activeSentenceRect below looks up this same index in
        // displaySentences — using the raw list here would misalign the
        // index the moment any blank segment got filtered out ahead of the
        // active one.
        if (!mainPanel.transcriptionController || mainPanel.displaySentences.length === 0)
            return -1
        // The highlight tracks the audio playhead, so it only means anything
        // when there's audio. An orphaned .md has no player.source; gating on
        // hasAudio (not just reading a leftover player.position of 0) stops the
        // top sentence from being spuriously highlighted for any transcript
        // whose first segment starts at t0=0 (i.e. essentially all of them).
        if (!mainPanel.hasAudio)
            return -1
        const positionSeconds = mainPanel.player.position / 1000.0
        const idx = mainPanel.transcriptionController.activeSentenceIndex(mainPanel.displaySentences, positionSeconds)
        // Keep exactly one sentence highlighted the whole time audio is loaded:
        // the C++ binary search returns -1 before playback reaches the first
        // real segment's t0, so clamp up to the first REAL sentence. Clamping to
        // index 0 would be wrong when a chapter heading opens the transcript
        // (displaySentences[0] is then a zero-length heading pseudo-entry and
        // the highlight would paint a stray sliver over its title), so skip past
        // any leading heading(s) via firstRealSentenceIndex. This extends the
        // already-sticky-through-gaps / sticky-past-the-end behavior to the
        // front edge, so the highlight never blanks out mid-file either.
        return idx >= 0 ? idx : mainPanel.firstRealSentenceIndex
    }

    // The active sentence just advanced (or a new file's first sentence lit
    // up) — re-pin it to the top if follow mode is on and audio is playing.
    // Reading activeSentenceRect inside scrollActiveSentenceToTop forces its
    // recompute against THIS new index, so there's no stale-rect race.
    onActiveSentenceIndexChanged: mainPanel.scrollActiveSentenceToTop()

    // Silence phase 2: the open file's stored `silences:` spans (phase 1),
    // parsed once per metadata load via TranscriptionController.parseSilences
    // — a pure read of the ALREADY-STORED raw VAD spans, never a
    // re-analysis. Feeds the auto-skip Connections below (and, phase 3, the
    // visual marking layer). metadata.silences is falsy for both "key
    // missing" (VAD never ran) and "key present but empty" (VAD ran, found
    // nothing) — parseSilences only needs to run when there's actually a
    // non-empty raw value to parse.
    readonly property var silenceSpans: (mainPanel.transcriptionController && mainPanel.metadata.silences)
        ? mainPanel.transcriptionController.parseSilences(mainPanel.metadata.silences)
        : []

    // Silence phase 3 (queue item 085): minimum span length worth a VISUAL
    // between-segment divider. Deliberately separate from
    // AppSettings.skipSilenceThresholdSeconds — that one governs PLAYBACK
    // SKIPPING (phase 2) and is user-configurable down to sub-second values
    // via its slider. Marking is purely cosmetic: every ordinary
    // inter-sentence breathing gap would earn its own "··· Ns silence ···"
    // line if this were 0, which is noise, not signal — so it's a fixed
    // constant, not a setting. See StyleGuide.md "Transcript silence
    // divider" for the locked value and rationale.
    readonly property real silenceMarkerMinSeconds: 5

    // StyleGuide.md "Transcript silence divider": U+00B7 MIDDLE DOT x3 either
    // side of a compact duration label, e.g. "··· 34s silence ···". One
    // function keeps the glyphs/spacing/wording in a SINGLE place rather
    // than string-building them ad hoc at the (one) call site.
    // Lives in TextFormat.js (pure — qsTr() works from a .pragma library
    // module, verified on this Qt build).
    function formatSilenceMarkerText(seconds) {
        return TextFormat.formatSilenceMarkerText(seconds)
    }

    // Skip-silence (silence phase 2, AppSettings.skipSilenceEnabled): while
    // playing, the instant the head enters a STORED silence span whose
    // length is >= AppSettings.skipSilenceThresholdSeconds, jump straight to
    // that span's end — via the same player.position assignment every other
    // seek in this file uses (chapter ticks, the slider), so it drives
    // activeSentenceIndex/scrollActiveSentenceToTop exactly like a manual
    // seek and never fights follow-scroll with bespoke scroll code (see
    // memory feedback_livescroll_followtail_contentheight_trap for why a
    // second hand-rolled scroll path is a trap here). nextSilenceSkipTarget()
    // is a PURE lookup over the already-parsed silenceSpans above — changing
    // the threshold alone changes what counts as skip-worthy, it never
    // re-runs VAD or any other task (the whole point of storing raw spans in
    // phase 1). A dedicated Connections on player.positionChanged (rather
    // than folding this into the activeSentenceIndex binding above) keeps a
    // real seek side effect out of a property binding, and fires at full
    // position-tick granularity rather than only on sentence-boundary
    // changes.
    Connections {
        target: mainPanel.player
        function onPositionChanged() {
            if (!AppSettings.skipSilenceEnabled)
                return
            if (!mainPanel.player || !mainPanel.player.playing)
                return
            if (!mainPanel.transcriptionController)
                return
            const positionSeconds = mainPanel.player.position / 1000.0
            const skipTo = mainPanel.transcriptionController.nextSilenceSkipTarget(
                mainPanel.silenceSpans, positionSeconds, AppSettings.skipSilenceThresholdSeconds)
            if (skipTo >= 0)
                mainPanel.player.position = skipTo * 1000
        }
    }

    // Sentence highlight is a real overlay Rectangle (see
    // sentenceHighlightOverlay below), not an HTML <span style="background">
    // — Qt's rich-text engine silently ignores border-radius on inline
    // elements (confirmed via a throwaway render check), so a genuinely
    // rounded highlight needs actual QML geometry, not CSS. That also let
    // transcriptArea go back to plain TextFormat (simpler, and the HTML
    // escaping/div-wrapping this used to need is gone entirely).
    //
    // Reactive property (not a plain function) so every dependency read
    // inside — activeSentenceIndex, sentences, and transcriptArea's own
    // layout (width/text, via positionToRectangle) — is tracked once, and
    // the 4 geometry bindings below each reuse the SAME computed rect
    // instead of recomputing it redundantly.
    // Safe positionToRectangle: same out-of-range hazard as the gutters
    // (see transcriptArea.gutterLineRects' doc comment) — querying a
    // position past the document's CURRENT actual length (before layout has
    // caught up to a just-changed text) logs "position out of range" and
    // returns a zeroed rect. Explicit dependency on transcriptArea.length
    // forces correct re-evaluation once layout genuinely settles.
    function safePositionToRectangle(pos) {
        // positionToRectangle()'s C++ result depends on width/padding/font,
        // but those aren't QML property reads from ITS perspective, so they
        // never register as dependencies of whichever binding called this —
        // reading them here (even unconditionally, unlike the ternaries
        // below that only read leftPadding/width on some branches) makes
        // that dependency real for every caller, propagating up the JS call
        // stack to activeSentenceRect/searchMatchRect's own evaluation.
        void transcriptArea.width
        void transcriptArea.leftPadding
        void transcriptArea.topPadding
        void transcriptArea.font
        return pos <= transcriptArea.length ? transcriptArea.positionToRectangle(pos) : Qt.rect(0, 0, 0, 0)
    }

    readonly property rect activeSentenceRect: {
        void transcriptArea.length
        void transcriptArea.width
        const idx = mainPanel.activeSentenceIndex
        const list = mainPanel.displaySentences
        if (idx < 0 || idx >= list.length)
            return Qt.rect(0, 0, 0, 0)
        const active = list[idx]
        if (active.isChapterHeading) {
            // A collapsed chapter's heading holds the playback highlight
            // directly (its body sentences are hidden — there is no spoken line
            // to light; see activeSentenceIndex in TranscriptionController.cpp).
            // Paint the WHOLE heading row, matching chapterHeadingRow's own
            // geometry (gutterLineRects[idx], full transcript width), rather
            // than the zero-width sliver active.start/length would give over the
            // blank reserved line.
            const rowRect = transcriptArea.gutterLineRects[idx] || Qt.rect(0, 0, 0, 0)
            return Qt.rect(0, rowRect.y, transcriptArea.width, rowRect.height)
        }
        const startRect = mainPanel.safePositionToRectangle(active.start)
        const endRect = mainPanel.safePositionToRectangle(active.start + active.length)
        // Sentences that wrap across multiple visual lines only get an
        // approximate box (start's row through end's row) — a precise
        // per-line multi-rect highlight would need walking each wrapped
        // line individually, not attempted here.
        const y = startRect.y
        const height = Math.max((endRect.y + endRect.height) - startRect.y, startRect.height)
        const x = (endRect.y > startRect.y) ? transcriptArea.leftPadding : startRect.x
        const width = (endRect.y > startRect.y) ? (transcriptArea.width - transcriptArea.leftPadding)
                                                  : Math.max(endRect.x - startRect.x, 4)
        return Qt.rect(x, y, width, height)
    }

    // Item 7: same shape as activeSentenceRect above, but for the current
    // Ctrl+F search match instead of the playing sentence.
    readonly property rect searchMatchRect: {
        void transcriptArea.length
        void transcriptArea.width
        if (mainPanel.searchMatchIndex < 0 || mainPanel.searchMatchStart < 0)
            return Qt.rect(0, 0, 0, 0)
        const startRect = mainPanel.safePositionToRectangle(mainPanel.searchMatchStart)
        const endRect = mainPanel.safePositionToRectangle(mainPanel.searchMatchStart + mainPanel.searchMatchLength)
        const y = startRect.y
        const height = Math.max((endRect.y + endRect.height) - startRect.y, startRect.height)
        const x = (endRect.y > startRect.y) ? transcriptArea.leftPadding : startRect.x
        const width = (endRect.y > startRect.y) ? (transcriptArea.width - transcriptArea.leftPadding)
                                                  : Math.max(endRect.x - startRect.x, 4)
        return Qt.rect(x, y, width, height)
    }

    // Item 12: search ignores segment-line formatting entirely — built from
    // mainPanel.displaySentences (already timestamp-free/reactive), joining
    // just the spoken text with single spaces so a query can start at the
    // end of one segment and finish at the start of the next. `map` keeps
    // each sentence's position in BOTH the clean corpus and displayText, so
    // a match can be translated back to a raw select() range for display.
    // searchMatchIndex now indexes into this clean corpus, not displayText.
    function searchCorpus() {
        let clean = ""
        const map = []
        for (const s of mainPanel.displaySentences) {
            const cleanStart = clean.length
            clean += s.text
            map.push({cleanStart: cleanStart, cleanEnd: clean.length, rawStart: s.start, rawLength: s.length})
            clean += " "
        }
        return {clean: clean, map: map}
    }

    // Item 13/7: every non-overlapping match of the current pattern in the
    // clean corpus, left to right, as {index, length}. One global-RegExp scan
    // now answers the "a/b" count, the current match's ordinal, AND the match
    // range itself — the three separate indexOf scans this used before the
    // search field became the find pattern can't work anymore, because a
    // regex match's length varies per match instead of always being
    // searchQuery.length.
    //
    // Returns [] for an empty query, an empty corpus, or an invalid regex —
    // all three mean "nothing to highlight". buildFindRegex() is what
    // clears/sets the inline error banner, so every caller gets that for free.
    function findMatches(clean) {
        const re = mainPanel.buildFindRegex(true)
        if (!re || clean === "")
            return []
        const out = []
        let m
        while ((m = re.exec(clean)) !== null) {
            out.push({index: m.index, length: m[0].length})
            // A zero-length match (regex mode, e.g. "a*") leaves lastIndex
            // parked, so exec() would return it forever — step past it.
            if (m[0].length === 0)
                re.lastIndex++
        }
        return out
    }

    function resetSearchMatch() {
        mainPanel.searchMatchIndex = -1
        mainPanel.searchMatchCount = 0
        mainPanel.searchMatchOrdinal = 0
        mainPanel.searchMatchStart = -1
        mainPanel.searchMatchLength = 0
    }

    // Shared match-finding core for both "jump to next" (Enter, searches
    // from just after the current match) and "search as you type"
    // (searchQuery changing, searches from the very start) — locates the
    // first occurrence of searchQuery at or after fromIndex in the search
    // corpus, wrapping around to the start if none is found from there,
    // then updates all the match-state properties and select()s the raw
    // range.
    function locateMatch(fromIndex) {
        const corpus = mainPanel.searchCorpus()
        const matches = mainPanel.findMatches(corpus.clean)
        if (matches.length === 0) {
            mainPanel.resetSearchMatch()
            return
        }
        // First match starting at or after fromIndex, wrapping to the first
        // overall — the list is already in left-to-right order, so its own
        // position in the list IS the 1-based ordinal, no second scan.
        let ord = matches.findIndex(m => m.index >= Math.max(0, fromIndex))
        if (ord === -1)
            ord = 0
        const idx = matches[ord].index
        mainPanel.searchMatchIndex = idx
        mainPanel.searchMatchCount = matches.length
        mainPanel.searchMatchOrdinal = ord + 1

        // Translate [idx, matchEnd) in the clean corpus back to a raw
        // select() range (into displayText) — spanning every sentence the
        // match touches, so a match crossing a segment boundary still
        // highlights correctly (the intervening newline/"[Speaker N]" text
        // between two sentences ends up included in the raw selection too —
        // there's no single contiguous range that would exclude it).
        const matchEnd = idx + matches[ord].length
        let firstEntry = null
        let lastEntry = null
        for (const entry of corpus.map) {
            if (entry.cleanEnd <= idx)
                continue
            if (entry.cleanStart >= matchEnd)
                break
            if (!firstEntry)
                firstEntry = entry
            lastEntry = entry
        }
        if (!firstEntry)
            return
        // Clamped to transcriptArea.length, same guard used everywhere else
        // in this file that feeds a raw position into Qt's cursor API
        // (safePositionToRectangle/gutterLineRects) — the corpus can
        // momentarily be built from a still-stale displaySentences a frame
        // before transcriptArea's text has caught up, and select() has no
        // bounds guard of its own.
        const selStart = Math.min(firstEntry.rawStart + Math.max(0, idx - firstEntry.cleanStart), transcriptArea.length)
        const selEnd = Math.min(lastEntry.rawStart + Math.min(lastEntry.rawLength, matchEnd - lastEntry.cleanStart), transcriptArea.length)
        mainPanel.searchMatchStart = selStart
        mainPanel.searchMatchLength = selEnd - selStart
        // select() alone still scrolls the match into view — no need to
        // steal focus from the search field, which would stop repeated
        // Enter presses from working (each jumps to the next match without
        // needing to re-click into it). The visible highlight itself is
        // searchMatchOverlay (Item 7), not the native selection.
        transcriptArea.select(selStart, selEnd)
        Qt.callLater(mainPanel.scrollMatchIntoView)
    }

    // Item P10: select() alone does NOT scroll the match into view here —
    // transcriptArea is readOnly and never takes focus in locateMatch()
    // (deliberately, so repeated Enter keeps working from the search
    // field), and QQC2's automatic cursor-follow scrolling only engages for
    // a focused, editable control. searchMatchRect (computed above from the
    // same searchMatchStart/Length this function just set) gives the
    // match's rect in transcriptArea's own local coordinates; mapping that
    // into the transcript viewport's Flickable frame (notePage.bodyScrollView.
    // contentItem — the same one used by the live-transcription auto-scroll in
    // onTranscriptBodyChanged and by ScrollPage's built-in middle-drag) gives
    // its on-screen position relative to the
    // current viewport, which is then minimally clamped into view — no
    // centering, just enough scroll to reveal it, same as a browser's
    // "scroll into view".
    function scrollMatchIntoView() {
        const rect = mainPanel.searchMatchRect
        if (rect.width === 0 && rect.height === 0)
            return
        const flick = notePage.bodyScrollView.contentItem
        const viewportPos = transcriptArea.mapToItem(flick, rect.x, rect.y)
        const maxContentY = Math.max(0, flick.contentHeight - flick.height)
        if (viewportPos.y < 0)
            flick.contentY = Math.max(0, flick.contentY + viewportPos.y)
        else if (viewportPos.y + rect.height > flick.height)
            flick.contentY = Math.min(maxContentY, flick.contentY + (viewportPos.y + rect.height - flick.height))
    }

    // Auto-scroll: while audio plays and follow mode is on
    // (AppSettings.followPlaybackScroll — the toggle beside the play button),
    // keep the active sentence pinned near the top of the transcript viewport.
    // Called on every activeSentenceIndex change (i.e. each sentence boundary),
    // when playback starts, and when follow mode is toggled back on.
    //
    // Mirrors scrollMatchIntoView's offset-safe mapping — activeSentenceRect is
    // in transcriptArea-local coords, which sit at a variable offset below the
    // Flickable origin (tags row, prelude, speaker rows, etc. above it), so map
    // through mapToItem rather than adding contentY to rect.y — but targets the
    // TOP edge instead of a minimal scroll-into-view.
    function scrollActiveSentenceToTop() {
        if (!AppSettings.followPlaybackScroll)
            return
        // Only chase the playhead while it's actually moving; a paused user is
        // free to scroll away and read elsewhere without being yanked back.
        if (!mainPanel.player || !mainPanel.player.playing)
            return
        if (mainPanel.activeSentenceIndex < 0)
            return
        const rect = mainPanel.activeSentenceRect
        if (rect.width === 0 && rect.height === 0)
            return
        const flick = notePage.bodyScrollView.contentItem
        const viewportPos = transcriptArea.mapToItem(flick, rect.x, rect.y)
        const maxContentY = Math.max(0, flick.contentHeight - flick.height)
        // A little breathing room above the pinned sentence rather than jamming
        // it flush against the very top edge.
        const topMargin = 12
        flick.contentY = Math.max(0, Math.min(maxContentY, flick.contentY + viewportPos.y - topMargin))
    }

    function jumpToNextMatch() {
        mainPanel.locateMatch(mainPanel.searchMatchIndex < 0 ? 0 : mainPanel.searchMatchIndex + 1)
    }

    // Item 7: compiles searchQuery into a JS RegExp — treated as literal text
    // (special chars escaped) unless findRegexEnabled, always with "g" when
    // `global`, plus "i" when findCaseInsensitive. An invalid pattern sets
    // the inline findReplaceError banner and returns null instead of
    // throwing, so a half-typed regex never crashes the binding.
    //
    // This is the SINGLE place the pattern is compiled — the live search (via
    // findMatches) and replaceNext()/replaceAll() all go through it, which is
    // what keeps the "a/b" count honest about what Replace all would touch.
    function buildFindRegex(global) {
        mainPanel.findReplaceError = ""
        if (mainPanel.searchQuery === "")
            return null
        let pattern = mainPanel.searchQuery
        if (!mainPanel.findRegexEnabled)
            pattern = pattern.replace(/[.*+?^${}()|[\]\\]/g, "\\$&")
        let flags = global ? "g" : ""
        if (mainPanel.findCaseInsensitive)
            flags += "i"
        try {
            return new RegExp(pattern, flags)
        } catch (e) {
            mainPanel.findReplaceError = qsTr("Invalid pattern: %1").arg(e.message)
            return null
        }
    }

    // Routes a replaced whole-transcript string back through the SAME commit
    // path the editable transcriptArea uses on blur. commitTranscriptEdits()
    // diffs it per-sentence-line against displaySentences and — critically —
    // REJECTS + reverts (with its own inline banner) any edit that changes
    // the line count. So a replacement that only alters characters WITHIN
    // existing segment lines round-trips cleanly, while one that would add or
    // remove lines (e.g. a replacement containing a newline, or a pattern
    // matching across a segment boundary) is refused rather than corrupting
    // the one-line-per-segment structure. No parallel persistence path.
    function applyTranscriptReplacement(result) {
        if (result === transcriptArea.text)
            return // nothing matched / no change
        mainPanel.commitTranscriptEdits(result)
    }

    // Replace only the FIRST match: a non-global RegExp makes String.replace
    // hit just the first occurrence, honoring $1/$& backreferences.
    //
    // The replaceEnabled gate here is the real one, not just UI polish: the
    // bar is a find bar until the Replace checkbox is ticked, so nothing may
    // mutate the transcript while it's unticked, no matter how the call was
    // reached.
    function replaceNext() {
        if (!mainPanel.replaceEnabled)
            return
        const re = mainPanel.buildFindRegex(false)
        if (!re)
            return
        let result
        try {
            result = transcriptArea.text.replace(re, mainPanel.replaceText)
        } catch (e) {
            mainPanel.findReplaceError = qsTr("Invalid replacement: %1").arg(e.message)
            return
        }
        mainPanel.applyTranscriptReplacement(result)
    }

    // Replace every match (global RegExp). Same replaceEnabled gate as
    // replaceNext() above.
    function replaceAll() {
        if (!mainPanel.replaceEnabled)
            return
        const re = mainPanel.buildFindRegex(true)
        if (!re)
            return
        let result
        try {
            result = transcriptArea.text.replace(re, mainPanel.replaceText)
        } catch (e) {
            mainPanel.findReplaceError = qsTr("Invalid replacement: %1").arg(e.message)
            return
        }
        mainPanel.applyTranscriptReplacement(result)
    }

    // The shared "a different file was just opened" reset, run by BOTH file
    // identities' change handlers (player.source for audio, orphanedMdPath for
    // an orphaned .md) so the two modes reset identically.
    //
    // Item P8: restart the repaint nudge on EVERY file open, not just the
    // empty->populated transition (see transcriptRepaintNudgeTimer's own doc
    // comment below) — switching directly between two already-transcribed files
    // never passes through an empty state, so onTranscriptBodyChanged's own
    // trigger alone would miss it. Item P16: reset per-file undo history so
    // Ctrl+Z can never reach across a file switch into a different note's edits.
    // Item P23: refreshMetadata() is done by each caller BEFORE this (cheap
    // frontmatter read) but refreshPreludeAndChapters() is NOT — this is the
    // cold-open case, see beginTranscriptLoad()'s doc comment.
    function resetForFileOpen() {
        mainPanel.beginTranscriptLoad()
        transcriptRepaintNudgeTimer.restart()
        undoStack.clear()
        mainPanel.gutterSpeakerFilter = ""
        // A pending "undo could reopen this" only holds while the panel
        // stays where the delete left it — see closedByDeleteUrl.
        mainPanel.closedByDeleteUrl = ""
        // Item 33/34: collapse + which-chapter-is-editing are per-file
        // view state keyed by this file's chapter timestamps — reset on a
        // file switch so they never leak into a different note's chapters.
        mainPanel.collapsedChapters = ({})
        mainPanel.editingChapterTs = -1
        // A fresh file opens at the top, not following — re-armed only when
        // THIS file's own transcription starts (see onTranscribingThisFileChanged).
        mainPanel.followingTail = false
    }

    Connections {
        target: mainPanel.player
        function onSourceChanged() {
            mainPanel.refreshMetadata()
            mainPanel.resetForFileOpen()
        }
        // Pressing play snaps the current sentence to the top immediately,
        // rather than waiting for the next sentence boundary to advance.
        // (No-ops on pause: scrollActiveSentenceToTop bails when !playing.)
        function onPlayingChanged() {
            mainPanel.scrollActiveSentenceToTop()
        }
    }
    // showOrphanedMd() switches files by setting orphanedMdPath directly
    // (player.source is cleared to "" first, which fires onSourceChanged above
    // for the "no file" state, not the orphaned file itself) — this is the
    // other half of "a different file was just opened", covering the orphaned
    // .md view, and is symmetric with onSourceChanged.
    onOrphanedMdPathChanged: {
        mainPanel.refreshMetadata()
        mainPanel.resetForFileOpen()
    }
    Connections {
        target: mainPanel.transcriptionController
        function onFinished(fileUrl, ok, error) {
            mainPanel.refreshMetadata()
            mainPanel.refreshPreludeAndChapters()
            // Neither of the two refreshes above touches the transcript BODY
            // (transcriptBody/sentences) — during a live job that's driven only
            // by the throttled liveUpdateTick (onLiveUpdateTickChanged below).
            // An engine that streams segments one at a time (whisper.cpp) keeps
            // that current, but one that emits a whole VAD window's segments in
            // a single burst after a blocking decode (Parakeet via sherpa-onnx)
            // collapses that burst to ONE tick — so every segment written after
            // it, up to the whole transcript for a sub-3-minute single-window
            // file, never lands on screen until a file switch reloads it. Reload
            // the finished file's body here so the tail always shows. The
            // scroll-to-bottom is NOT done here: the reload's contentHeight
            // growth drives the transcriptScrollView follow above, which is
            // still armed (followingTail stays set until the next file open), so
            // the final segments scroll into view through the one code path.
            // isSameFile compares decoded local paths, not toString(): a
            // detect-speakers run on a file whose name has spaces emits a
            // fileUrl that can stringify differently from player.source (literal
            // ' ' vs "%20"), which a toString() compare would read as "a
            // different file" and silently skip this reload — so the just-added
            // speaker tags never appear live. See isSameFile()'s doc comment.
            if (ok && mainPanel.player
                    && mainPanel.transcriptionController.isSameFile(fileUrl, mainPanel.player.source))
                mainPanel.refreshTranscriptSync()
        }
        // Item 10: frontmatter is written at job START (durability, see
        // TranscriptionController), before any segment lines exist — so
        // hasMetadata can flip true well before onFinished fires.
        // Re-checking on every progress tick (for the file currently open)
        // is what lets transcriptBody's binding start reading a live,
        // in-progress file instead of waiting for completion.
        function onProgressChanged() {
            if (mainPanel.player
                    && mainPanel.transcriptionController.currentFile.toString() === mainPanel.player.source.toString()) {
                mainPanel.refreshMetadata()
                mainPanel.refreshPreludeAndChapters()
            }
        }
    }
    // Item 2 (delete): react to a transcript/file deletion of the OPEN file —
    // whether triggered here (MainPanel's delete icons) or from the Sidebar's
    // context menu for the same file. transcriptDeleted keeps the audio, so
    // just re-read metadata (it reverts to untranscribed); fileDeleted removes
    // the whole thing, so clear the view; audioDeletedTranscriptKept removes
    // only the audio, so FOLLOW the surviving transcript instead of clearing.
    // All three compare against currentFileUrl() so an unrelated file's
    // deletion never disturbs what's open.
    Connections {
        target: mainPanel.vaultTreeModel
        function onTranscriptDeleted(fileUrl) {
            if (fileUrl.toString() === mainPanel.currentFileUrl().toString()) {
                mainPanel.refreshMetadata()
                mainPanel.refreshPreludeAndChapters()
            }
        }
        function onFileDeleted(fileUrl) {
            if (fileUrl.toString() === mainPanel.currentFileUrl().toString()) {
                mainPanel.player.source = ""
                mainPanel.orphanedMdPath = ""
                // AFTER the two lines above, never before: each fires the
                // per-file reset that clears this very property.
                mainPanel.closedByDeleteUrl = fileUrl
            }
        }
        // The point of "delete audio only" is to keep the transcript, so
        // closing it here would undo the user's actual intent. The words stay
        // on screen; only the audio (and the player) goes away. showOrphanedMd()
        // is the same call that opens any orphaned .md from the tree, so the
        // file lands in the identical state as clicking it there would.
        function onAudioDeletedTranscriptKept(fileUrl, transcriptUrl) {
            if (fileUrl.toString() === mainPanel.currentFileUrl().toString())
                mainPanel.showOrphanedMd(transcriptUrl.toString().replace("file://", ""))
        }
        // The undo half of the two above (a snackbar's Undo -> restoreFromTrash).
        // rebuild() alone fixes the tree but not this panel: the view state a
        // delete drove us into is ours, not the model's, so nothing walks it
        // back unless we do it here.
        //
        // Matched against the TRANSCRIPT, not the audio: after the delete this
        // panel is showing the orphaned .md, so the surviving transcript is what
        // currentFileUrl() reports — the restored audio's url is a file we were
        // never on. showAudioFile() is the same call the tree makes, so undo
        // lands on the identical state the delete started from.
        function onAudioRestoredForTranscript(fileUrl, transcriptUrl) {
            if (transcriptUrl.toString() === mainPanel.currentFileUrl().toString())
                mainPanel.showAudioFile(fileUrl.toString().replace("file://", ""))
        }
        // Mirrors onTranscriptDeleted: the file stayed open and only its
        // metadata/body changed underneath, so the same two refreshes put the
        // transcript back on screen.
        function onTranscriptRestored(fileUrl) {
            if (fileUrl.toString() === mainPanel.currentFileUrl().toString()) {
                mainPanel.refreshMetadata()
                mainPanel.refreshPreludeAndChapters()
            }
        }
        // The undo of onFileDeleted. Matched against closedByDeleteUrl rather
        // than currentFileUrl(), because the whole point is that there IS no
        // current file — the delete closed it. That's also what keeps this from
        // opening files on its own: undoing a delete of something the panel
        // never had open leaves the panel alone, since only the file it closed
        // itself is ever in closedByDeleteUrl.
        function onFileRestored(fileUrl, isOrphanedMd) {
            if (fileUrl.toString() !== mainPanel.closedByDeleteUrl.toString())
                return
            // The same two calls the tree makes, chosen the same way it chooses
            // (the model's own orphaned-vs-audio classification, not a guess
            // from the extension), so an undo lands the panel in the identical
            // state the file was in before the delete.
            const path = fileUrl.toString().replace("file://", "")
            if (isOrphanedMd)
                mainPanel.showOrphanedMd(path)
            else
                mainPanel.showAudioFile(path)
        }
    }
    // Item O10: typing now searches immediately (not just on Enter) —
    // reset first (clears the stale match's highlight/count instantly) then
    // locate the FIRST match from the very start of the corpus. Enter
    // (searchField.onAccepted -> jumpToNextMatch) still advances from the
    // current match, unchanged.
    onSearchQueryChanged: {
        mainPanel.resetSearchMatch()
        mainPanel.locateMatch(0)
    }

    // Item 7: the mode toggles now feed the live search as well as replace
    // (searchQuery is the pattern for both), so flipping one re-runs the
    // search from the top the same way retyping the query does — otherwise
    // the highlight and the "a/b" count would keep describing the OLD mode.
    onFindRegexEnabledChanged: {
        mainPanel.resetSearchMatch()
        mainPanel.locateMatch(0)
    }

    onFindCaseInsensitiveChanged: {
        mainPanel.resetSearchMatch()
        mainPanel.locateMatch(0)
    }

    // O1: on first open of an already-transcribed file, transcriptArea can
    // transiently compute a wrong (oversized) width for one frame right as
    // `visible` flips true and `text` is set in the same tick, leaving the
    // text invisible (while the highlight overlay, a separate Rectangle,
    // still renders) until something forces a repaint — empirically,
    // selecting text in the TextArea is known to make it reappear, so
    // selectAll()+deselect() (invokable regardless of focus, no visible
    // side effect since nothing stays selected) reproduces that same nudge
    // programmatically. Timer delay (rather than Qt.callLater, which only
    // defers one event-loop turn) gives the width self-correction observed
    // ~100ms after the flip time to actually happen first.
    property bool transcriptWasEmpty: true
    Timer {
        id: transcriptRepaintNudgeTimer
        interval: 120
        onTriggered: {
            transcriptArea.selectAll()
            transcriptArea.deselect()
        }
    }

    // Item 8 (Wave D): follow the transcript as it grows during an active
    // transcription of the currently-open file — only while that job is
    // actually running, so browsing a finished transcript never gets
    // yanked to the bottom. Qt.callLater so the TextArea/ScrollView have
    // re-laid-out with the new text before contentHeight is read.
    //
    // O6: only if the user was ALREADY scrolled at/near the bottom before
    // this change — otherwise a user who scrolled up to re-read earlier
    // text would get yanked back down on every new segment. Checked here,
    // BEFORE Qt.callLater/new layout, against the current (pre-growth)
    // contentY/contentHeight.
    onTranscriptBodyChanged: {
        if (mainPanel.transcriptWasEmpty && mainPanel.transcriptBody !== "")
            transcriptRepaintNudgeTimer.restart()
        mainPanel.transcriptWasEmpty = mainPanel.transcriptBody === ""
        // Scroll-follow no longer lives here — reacting to transcriptBody meant
        // measuring against a contentHeight that had already grown (see the
        // notePage.bodyScrollView.contentItem Connections below, which reacts to
        // contentHeight itself and so never races the layout).
    }

    // Race-free follow-to-bottom while a file is being transcribed. Fires on the
    // flickable's OWN contentHeight — which, by the time this runs, has already
    // settled to the post-growth value, so there's no Qt.callLater guesswork
    // about whether layout caught up. "Was the user parked at the bottom before
    // this growth?" is tested against lastFollowContentHeight (the PREVIOUS
    // contentHeight), never the live one: the live value is already grown, so
    // testing against it would classify any jump bigger than the threshold as
    // "the user scrolled up" and stop following — the exact defect that left a
    // multi-segment whisper tick or a whole-window Parakeet burst stranded above
    // the fold. Gated on followingTail so ordinary browsing, a file open, or a
    // window resize never yanks the view to the bottom.
    Connections {
        target: notePage.bodyScrollView.contentItem
        function onContentHeightChanged() {
            const flick = notePage.bodyScrollView.contentItem
            const nearBottomThreshold = 40
            const prevBottom = Math.max(0, mainPanel.lastFollowContentHeight - flick.height)
            const wasAtBottom = flick.contentY >= prevBottom - nearBottomThreshold
            mainPanel.lastFollowContentHeight = flick.contentHeight
            if (mainPanel.followingTail && wasAtBottom)
                flick.contentY = Math.max(0, flick.contentHeight - flick.height)
        }
    }

    Shortcut {
        sequence: "Ctrl+F"
        enabled: mainPanel.activeFocus && mainPanel.currentMdUrl !== ""
        onActivated: {
            mainPanel.searchExpanded = true
            searchReplaceBar.forceSearchFocus()
        }
    }

    // Item P16: undoes the last label/prelude/tag/transcript/speaker-rename
    // edit made through this panel — see undoStack's own doc comment above.
    Shortcut {
        sequence: "Ctrl+Z"
        enabled: mainPanel.activeFocus && undoStack.canUndo
        onActivated: mainPanel.undoLast()
    }

    // Item 46 / Item 20: redo — re-applies the most recently undone edit.
    // Bound to BOTH conventional redo chords (Ctrl+Y and Ctrl+Shift+Z), each
    // as its own Shortcut so both work regardless of platform habit. Gated
    // the same way Ctrl+Z is (panel focus + a non-empty redoStack).
    Shortcut {
        sequences: ["Ctrl+Y", "Ctrl+Shift+Z"]
        enabled: mainPanel.activeFocus && undoStack.canRedo
        onActivated: mainPanel.redoLast()
    }

    // Item 7: Ctrl+H opens the SAME bar Ctrl+F does — there is only one now —
    // but arms the replace row on the way in, so the old "jump straight to
    // replace" muscle memory still lands somewhere useful. Same enable-gate
    // as Ctrl+F. Not a toggle: it's "get me to replace", so pressing it twice
    // shouldn't shut the bar you just opened.
    Shortcut {
        sequence: "Ctrl+H"
        enabled: mainPanel.activeFocus && mainPanel.currentMdUrl !== ""
        onActivated: {
            mainPanel.searchExpanded = true
            mainPanel.replaceEnabled = true
            searchReplaceBar.forceSearchFocus()
        }
    }

    // Item 51: a plain left-click landing on a non-interactive region (the
    // panel's empty background, a gap between rows) clears any active text
    // selection. Attached to the root so it only fires when nothing deeper
    // under the pointer consumes the press — a deeper handler always wins
    // Qt Quick's pointer hit-testing, so clicking INTO a field (its own
    // press grab), a timestamp/seek TapHandler, a button, or middle-drag
    // panning (ScrollPage's built-in MiddleDragScroll TapHandler) all still
    // work untouched. Deliberately a TapHandler, NOT a MouseArea: an
    // Item-attached handler carries no implicit cursor, so unlike a
    // MouseArea it can span the whole panel without poisoning the
    // I-beam/pointer cursor resolution of everything above it (see the
    // project's MouseArea cursor-bleed notes). The default gesturePolicy
    // (DragThreshold) means a click-drag that selects text is NOT treated as
    // a tap, so dragging out a selection never immediately self-clears.
    TapHandler {
        acceptedButtons: Qt.LeftButton
        onTapped: mainPanel.deselectTextSelections()
    }

    color: Qt.rgba(AppSettings.mainBackgroundColor.r, AppSettings.mainBackgroundColor.g,
                    AppSettings.mainBackgroundColor.b, AppSettings.mainOpacity)
    // Only the corners touching the SplitView divider are rounded; the
    // window-edge corners are left square so the outer window mask (which
    // already rounds the true window corners) isn't duplicated here.
    topLeftRadius: AppSettings.borderRadius
    bottomLeftRadius: AppSettings.borderRadius

    Item {
        anchors.fill: parent
        visible: mainPanel.vaultPath.toString() === "" && !mainPanel.topLevelScreenOpen

        Button {
            id: openVaultButton
            anchors.centerIn: parent
            text: qsTr("Open Vault")
            hoverEnabled: true
            onClicked: mainPanel.openVaultRequested()

            background: Rectangle {
                radius: AppSettings.borderRadius
                color: (openVaultButton.hovered && openVaultButton.enabled) ? Theme.surfaceHover : Theme.surface
                border.width: 1
                border.color: Theme.border
            }
            HoverHandler {
                cursorShape: Qt.PointingHandCursor
            }
            HoverBorder {
                anchors.fill: parent
                active: openVaultButton.hovered
            }
        }
    }

    ScrollPage {
        id: notePage
        anchors.fill: parent
        visible: mainPanel.vaultPath.toString() !== "" && !mainPanel.topLevelScreenOpen
        // Preserve the "transcriptScrollView" objectName the QQuickView/offscreen
        // harnesses look the inner ScrollView up by — ScrollPage owns the
        // ScrollView now, so pass the name through its bodyObjectName alias.
        bodyObjectName: "transcriptScrollView"

        // Sticky header — the three fixed rows above the scrolling transcript
        // (playback progress, title/regen/search, find/replace). They live in
        // ScrollPage's header slot so they stay put while the transcript scrolls
        // under them; ScrollPage's built-in middle-drag + edge-gutter scrollbar
        // replace the old hand-rolled transcriptViewportContainer chrome.
        header: [

        // Item 13: playback progress bar relocated to the very top, full
        // width, with a play/pause icon instead of a separate Play button.
        // Hidden entirely for the orphaned-md text-only view (nothing to
        // play).
        SeekBar {
            panel: mainPanel
        },

        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            visible: mainPanel.currentMdUrl !== ""

            Item {
                id: titleContainer
                Layout.fillWidth: true
                // Item 048: without a floor, this is the only fillWidth item
                // in the sticky title RowLayout, so once the search/replace
                // cluster expands (~350px of fixed-width controls) the
                // RowLayout squeezes titleContainer toward 0 instead of
                // giving it graceful room — that reads as the title getting
                // clipped. A minimumWidth stops the collapse; searchField
                // below gives back width in turn so the row still fits.
                Layout.minimumWidth: 80
                implicitHeight: titleColumn.implicitHeight

                ColumnLayout {
                    id: titleColumn
                    anchors.left: parent.left
                    anchors.right: parent.right
                    spacing: 1

                    Item {
                        id: titleRow
                        Layout.fillWidth: true
                        implicitHeight: titleField.implicitHeight

                        // Always-editable: a single TextField is present at all
                        // times (no display-Label/edit-TextField swap), styled by
                        // UnderlineField to look like plain text until focused.
                        // This is what fixes the right-click-copy bug — since
                        // there is no visibility state tied to focus, the native
                        // TextEditingContextMenu stealing focus on right-click can
                        // no longer hide the field out from under the user.
                        TextField {
                            id: titleField
                            objectName: "titleField" // located by tst_mainpanel_renameundo
                            anchors.fill: parent
                            color: "white"
                            font.pixelSize: 14
                            text: mainPanel.displayTitle()
                            // Blocked while a transcribe of THIS file is
                            // queued/active — in non-labels mode this field
                            // renames the real file (commitRename → renameFile),
                            // which must not race a running transcription (see
                            // the delete trio's identical busyOrQueuedUrls gate,
                            // and serialActiveConflictsWith). In labels mode it
                            // only edits the AI label (setLabel, a live inline
                            // edit that's fine mid-job), so stays enabled — the
                            // file rename there lives in filenameSubtitle, which
                            // is already guarded. player.source (the AUDIO url)
                            // is what busyOrQueuedUrls holds, not currentMdUrl.
                            enabled: AppSettings.useLabelsMainPanel
                                || mainPanel.busyOrQueuedUrls.indexOf(mainPanel.player.source.toString()) === -1
                            // QQC2's Basic-style TextField has its own
                            // padding (6px, +4 more on the left) and
                            // UnderlineField's background otherwise imposes
                            // a 24px min-height — both invisible while this
                            // was a transient edit-mode field, but a
                            // permanent, jarring size/spacing change now
                            // that it's always present. Zeroed so this reads
                            // at exactly the old Label's tight size.
                            padding: 0
                            leftPadding: 0
                            rightPadding: 0
                            topPadding: 0
                            bottomPadding: 0

                            onAccepted: mainPanel.commitRename(text)
                            onActiveFocusChanged: if (!activeFocus) mainPanel.commitRename(text)

                            background: UnderlineField { implicitHeight: 0; plainUntilHovered: true; focused: titleField.activeFocus }

                            Keys.onEscapePressed: titleField.text = mainPanel.displayTitle()

                            // Item 13: QQC2 TextField has no built-in I-beam
                            // — add one so it reads as editable text, same
                            // idiom as transcriptArea/searchField.
                            HoverHandler { cursorShape: Qt.IBeamCursor }
                        }

                        // Confirm-changes checkmark: appears at the field's
                        // right edge only while the title is focused AND edited
                        // (text differs from the committed displayTitle()).
                        // Tapping it blurs the field, which commits via
                        // titleField.onActiveFocusChanged → commitRename().
                        // Clicking anywhere else commits the same way (see
                        // deselectTextSelections()).
                        Item {
                            id: titleConfirmButton
                            objectName: "titleConfirmButton" // located by tst_mainpanel_renameundo
                            width: 18
                            height: 18
                            anchors.right: parent.right
                            anchors.verticalCenter: parent.verticalCenter
                            visible: titleField.activeFocus && titleField.text !== mainPanel.displayTitle()

                            HoverHandler {
                                id: titleConfirmHover
                                cursorShape: Qt.PointingHandCursor
                            }
                            TapHandler {
                                onTapped: titleField.focus = false
                            }
                            HoverBorder {
                                anchors.fill: parent
                                active: titleConfirmHover.hovered
                            }
                            AppIcon {
                                anchors.centerIn: parent
                                name: "check"
                                iconSize: 12
                                color: Theme.textPrimary
                            }
                        }
                    }

                    // Item P5: when "labels" mode is showing an AI-generated
                    // label instead of the real filename (see
                    // AppSettings.useLabelsMainPanel/displayTitle()), the
                    // actual on-disk file is no longer visible anywhere —
                    // this small secondary line restores that, reusing
                    // displayTitle()'s own relativeToVault() helper rather
                    // than re-deriving the path (currentFileStem() works for an
                    // orphaned .md and an audio file alike, via currentMdUrl),
                    // and only once this file actually HAS a label (an
                    // untranscribed file with labels-mode on still just
                    // shows its filename as the title itself, so a
                    // filename-under-filename subtitle would be redundant).
                    // Item: editable rename control for the REAL file (shown
                    // only in labels mode, where the title field edits the AI
                    // label instead — so without this the actual file couldn't
                    // be renamed from the panel). Always-editable per
                    // StyleGuide (one TextField, UnderlineField
                    // plainUntilHovered so it reads as plain text until
                    // hovered/focused; commit on Return + blur; Esc reverts).
                    // Shows the base name WITH extension (currentFileBase) so it
                    // names the real on-disk file; commitFilenameRename strips
                    // the extension back off since renameFile takes a stem and
                    // preserves the real extension. Disabled while a job is
                    // running so a rename can't race an operation.
                    TextField {
                        id: filenameSubtitle
                        objectName: "filenameSubtitle" // located by tst_mainpanel_renameundo
                        Layout.fillWidth: true
                        visible: AppSettings.useLabelsMainPanel && !!mainPanel.metadata.label
                        enabled: !(mainPanel.transcriptionController && mainPanel.transcriptionController.busy)
                        color: "#777777"
                        font.pixelSize: 10
                        text: mainPanel.currentFileBase()
                        padding: 0
                        leftPadding: 0
                        rightPadding: 0
                        topPadding: 0
                        bottomPadding: 0

                        onAccepted: mainPanel.commitFilenameRename(text)
                        onActiveFocusChanged: if (!activeFocus) mainPanel.commitFilenameRename(text)
                        Keys.onEscapePressed: filenameSubtitle.text = mainPanel.currentFileBase()

                        background: UnderlineField { implicitHeight: 0; plainUntilHovered: true; focused: filenameSubtitle.activeFocus }

                        HoverHandler { cursorShape: Qt.IBeamCursor }
                    }
                }
            }

            // Item 28: transcribe as an icon button (same 22x22 box as the
            // regenerate/search icons it now sits beside), replacing the old
            // full-width text Button that lived down in the scrollable
            // content below the model combo. Same enable/tooltip logic the
            // old Button had (Wave F: a manual click always re-runs,
            // regardless of stored-metadata match) — modelCombo is declared
            // lower in this file but still resolves fine since ids are scoped
            // to the whole document.
            AppIconButton {
                id: transcribeIconButton
                visible: mainPanel.hasAudio

                readonly property bool isActive: mainPanel.transcriptionController && mainPanel.player
                    && mainPanel.transcriptionController.busy
                    && mainPanel.transcriptionController.activeOccupiesFile
                    && mainPanel.transcriptionController.currentFile.toString() === mainPanel.player.source.toString()
                readonly property bool isQueued: mainPanel.transcriptionController && mainPanel.player
                    && mainPanel.player.source.toString() !== ""
                    && mainPanel.transcriptionController.queuedFileUrls.some(
                           u => u.toString() === mainPanel.player.source.toString())
                enabled: visible && mainPanel.transcriptionController
                         && !transcribeIconButton.isActive && !transcribeIconButton.isQueued

                iconName: "transcribe"
                iconSize: 13
                tooltip: transcribeIconButton.isActive ? qsTr("Transcribing…")
                         : transcribeIconButton.isQueued ? qsTr("Queued…")
                         : (mainPanel.transcriptionController && mainPanel.transcriptionController.busy
                            ? qsTr("Add to queue")
                            : (mainPanel.hasMetadata ? qsTr("Re-transcribe") : qsTr("Transcribe")))

                onTapped: mainPanel.transcriptionController.transcribe(mainPanel.player.source, mainPanel.vaultPath,
                                                                         modelCombo.currentValue,
                                                                         AppSettings.autoLabelEnabled,
                                                                         AppSettings.restrictTagsToExisting,
                                                                         AppSettings.tagReuseBias,
                                                                         AppSettings.wordiness)
            }

            // Items 24/53: "Run all jobs" — queues the full transcribe->label
            // ->chapters pipeline for the open file. Per the user's spec, shown
            // ONLY when there's no transcription yet (an untranscribed audio
            // file): once transcribed, the individual regenerate-label /
            // generate-chapters icons beside it cover the derived passes. Not
            // shown for an orphaned .md (nothing to transcribe — the
            // relabel/chapters icons already handle that case).
            AppIconButton {
                id: runAllIconButton
                visible: mainPanel.transcriptionController && mainPanel.hasAudio && !mainPanel.hasMetadata
                readonly property bool isQueued: mainPanel.transcriptionController && mainPanel.player
                    && mainPanel.transcriptionController.queuedFileUrls.some(
                           u => u.toString() === mainPanel.player.source.toString())
                enabled: visible && !runAllIconButton.isQueued
                         && !(mainPanel.transcriptionController.busy
                              && mainPanel.transcriptionController.activeOccupiesFile
                              && mainPanel.transcriptionController.currentFile.toString()
                                 === mainPanel.player.source.toString())

                iconName: "run-all"
                tooltip: qsTr("Run all jobs (transcribe, label, chapters)")

                onTapped: mainPanel.transcriptionController.runAllJobs(mainPanel.player.source, mainPanel.vaultPath,
                                                                        modelCombo.currentValue,
                                                                        AppSettings.restrictTagsToExisting,
                                                                        AppSettings.tagReuseBias,
                                                                        AppSettings.wordiness,
                                                                        AppSettings.chapterDensity,
                                                                        AppSettings.diarizationSegmentationModel,
                                                                        AppSettings.diarizationEmbeddingModel,
                                                                        AppSettings.diarizationSensitivity)
            }

            // Item 9: regenerate auto-title — re-runs just the AI labeling
            // pass on the already-transcribed text (no re-transcription).
            // Visible whenever there's a transcript to label, independent of
            // AppSettings.autoLabelEnabled (that setting is about running
            // automatically after each transcription, not gating manual
            // regeneration).
            AppIconButton {
                id: regenerateLabelButton
                visible: mainPanel.transcriptionController && (mainPanel.hasTranscript || mainPanel.transcribeInFlight)
                readonly property url targetUrl: mainPanel.currentMdUrl
                enabled: visible && mainPanel.busyOrQueuedKeys.indexOf(regenerateLabelButton.targetUrl.toString() + " " + "relabel") === -1

                iconName: "relabel"
                iconSize: 14
                tooltip: qsTr("Regenerate label, prelude, and tags")

                onTapped: mainPanel.transcriptionController.relabel(regenerateLabelButton.targetUrl,
                                                                     mainPanel.vaultPath,
                                                                     AppSettings.restrictTagsToExisting,
                                                                     AppSettings.tagReuseBias,
                                                                     AppSettings.wordiness)
            }

            // Feature 3c: "Generate chapters" — mirrors regenerateLabelButton
            // above exactly, including orphaned-.md support: chapter
            // generation (TranscriptionController::generateChaptersJob())
            // only ever reads the .md's existing frontmatter/body, never the
            // audio file, so it works identically whether the row is
            // player-backed or an orphaned .md — placed here in the
            // icon-button cluster rather than inside the chapter list's own
            // header row so it's still reachable to generate a FIRST set of
            // chapters when mainPanel.chapters is still empty (the list
            // itself only renders once chapters.length > 0).
            AppIconButton {
                id: generateChaptersButton
                visible: mainPanel.transcriptionController && (mainPanel.hasTranscript || mainPanel.transcribeInFlight)
                readonly property url targetUrl: mainPanel.currentMdUrl
                enabled: visible && mainPanel.busyOrQueuedKeys.indexOf(generateChaptersButton.targetUrl.toString() + " " + "chapters") === -1

                iconName: "chapters"
                tooltip: qsTr("Generate chapters")

                onTapped: mainPanel.transcriptionController.generateChapters(generateChaptersButton.targetUrl, mainPanel.vaultPath, AppSettings.chapterDensity)
            }

            // Item 52: "Detect speakers" — runs speaker diarization
            // (TranscriptionController::diarizeFile()) as its own standalone job
            // on the already-transcribed OPEN file, WITHOUT re-transcribing.
            // Lets the user add speaker detection after the fact, or redo it.
            // Unlike regenerateLabel/generateChapters (which read only the .md),
            // diarization decodes the AUDIO, so this is offered only for a
            // real audio-backed transcribed file — not an orphaned .md (no
            // audio to diarize). See DESIGN.md "Speaker diarization".
            //
            // Parity (CLAUDE.md): the bulk counterparts exist on the Sidebar
            // ribbon (detectSpeakersAllButton) and its search-scoped mirror
            // (searchDetectSpeakersButton); the old per-row hover cluster was
            // intentionally removed. Enforced by tst_styleguide's
            // sidebar_detectSpeakersActionParityAcrossAllSites.
            AppIconButton {
                id: detectSpeakersButton
                visible: mainPanel.transcriptionController && mainPanel.hasAudio && (mainPanel.hasMetadata || mainPanel.transcribeInFlight)
                readonly property url targetUrl: mainPanel.player.source
                enabled: visible && mainPanel.busyOrQueuedKeys.indexOf(detectSpeakersButton.targetUrl.toString() + " " + "diarize") === -1

                iconName: "speakers"
                iconSize: 14
                tooltip: qsTr("Detect speakers")

                onTapped: mainPanel.transcriptionController.diarizeFile(detectSpeakersButton.targetUrl,
                                                                        mainPanel.vaultPath,
                                                                        AppSettings.diarizationSegmentationModel,
                                                                        AppSettings.diarizationEmbeddingModel,
                                                                        AppSettings.diarizationSensitivity)
            }

            // Item 2: "Delete transcript" — removes the open transcribed audio
            // file's sibling .md (VaultTreeModel::trashTranscript()), reverting
            // it to untranscribed while KEEPING the audio. Only for a
            // transcribed AUDIO file (an orphaned .md has no audio to keep — it
            // gets "Delete file" instead). Disabled while a job is
            // queued/active for this file (deleting its sidecar mid-write would
            // fight the worker) — same isQueued gate the regenerate icons use.
            AppIconButton {
                id: deleteTranscriptButton
                visible: mainPanel.transcriptionController && mainPanel.vaultTreeModel
                         && mainPanel.hasAudio && mainPanel.hasMetadata
                readonly property url targetUrl: mainPanel.player.source
                enabled: visible && mainPanel.busyOrQueuedUrls.indexOf(deleteTranscriptButton.targetUrl.toString()) === -1

                iconName: "delete-transcript"
                iconSize: 14
                iconColor: deleteTranscriptButton.enabled ? Theme.danger : Theme.textDisabled
                tooltip: qsTr("Delete transcript (keep audio)")

                onTapped: mainPanel.requestDeleteTranscript()
            }

            // "Delete audio only" — the complement of "Delete transcript", and
            // gated identically: a transcribed AUDIO file (orphanedMdPath == ""
            // + hasMetadata), never an orphaned .md (no audio to drop)
            // and never an untranscribed one (nothing to keep, so it would just
            // duplicate "Delete file"). Same isQueued gate — dropping the audio
            // out from under a running job would fight the worker.
            AppIconButton {
                id: deleteAudioOnlyButton
                objectName: "deleteAudioOnlyButton"
                visible: mainPanel.transcriptionController && mainPanel.vaultTreeModel
                         && mainPanel.hasAudio && mainPanel.hasMetadata
                readonly property url targetUrl: mainPanel.player.source
                enabled: visible && mainPanel.busyOrQueuedUrls.indexOf(deleteAudioOnlyButton.targetUrl.toString()) === -1

                iconName: "delete-audio"
                iconSize: 14
                iconColor: deleteAudioOnlyButton.enabled ? Theme.danger : Theme.textDisabled
                tooltip: qsTr("Delete audio (keep transcript)")

                onTapped: mainPanel.requestDeleteAudioOnly()
            }

            // Item 2: "Delete file" — deletes the whole open file and its
            // sidecars (VaultTreeModel::trashFiles()): audio + .md + partial,
            // or an orphaned .md alone. Shown for any open file (audio OR
            // orphaned .md). Same isQueued gate as above.
            //
            // `iconObjectName` names the inner AppIcon "deleteFileIcon" —
            // tst_mainpanel_deleteundo looks that up to assert the glyph's
            // painted color flips danger->disabled with the queue state, which
            // the button's own objectName can't answer.
            AppIconButton {
                id: deleteFileButton
                objectName: "deleteFileButton"
                iconObjectName: "deleteFileIcon"
                visible: mainPanel.vaultTreeModel && mainPanel.currentMdUrl !== ""
                readonly property url targetUrl: mainPanel.currentFileUrl()
                enabled: visible && mainPanel.busyOrQueuedUrls.indexOf(deleteFileButton.targetUrl.toString()) === -1

                iconName: "delete-file"
                iconSize: 14
                iconColor: deleteFileButton.enabled ? Theme.danger : Theme.textDisabled
                tooltip: qsTr("Delete file")

                onTapped: mainPanel.requestDeleteFile()
            }

            // Item 18: search icon → expands to a text field, Ctrl+F while
            // the main panel has focus. Searches title/tags (via
            // metadata, already loaded) + the transcript body
            // (jumpToNextMatch() selects/scrolls the next match below).
            // Item 7: this field is ALSO the find pattern for replace — the
            // .*/Aa toggles and the Replace checkbox sit right after it, and
            // the replace row unfolds below the whole title row.
            // Extracted to SearchReplaceBar.qml. Layout.fillWidth is gated on
            // searchExpanded so the collapsed bar (just the search icon) claims
            // no row space — expanded, it competes with titleContainer for
            // width exactly as the individual controls used to.
            SearchReplaceBar {
                id: searchReplaceBar
                panel: mainPanel
                Layout.fillWidth: mainPanel.searchExpanded
            }
        },

        // Item 7: the replace row — sticky, right under the title/search row.
        // The find half of this bar is the search row above (searchField IS
        // the pattern, .*/Aa are its modes), so all that lives here is the
        // replacement string, the two actions, and the shared error banner.
        // The row appears only once the Replace checkbox arms it; the banner
        // does not, since an invalid regex is worth reporting while you're
        // merely searching. Regex mode uses JS RegExp (see buildFindRegex);
        // an invalid pattern shows the banner rather than throwing. Replace
        // routes through commitTranscriptEdits() via replaceNext()/
        // replaceAll(), so it inherits that path's per-sentence line-count
        // safety check.
        ReplaceRow {
            panel: mainPanel
        }
        ] // end header slot

        // Item: only the playback progress bar and title/regen/search row
        // above are "sticky" (fixed) — they live in ScrollPage's header slot.
        // Everything else (tags, speaker pills, detection/model controls, the
        // transcribe button, and the transcript itself) is ScrollPage's single
        // scrolling content child below, so it scrolls away together with the
        // transcript on a long file instead of permanently eating vertical
        // space above it. ScrollPage owns the inner ScrollView (reached via
        // notePage.bodyScrollView — its .contentItem is the same Flickable the
        // live-transcription auto-scroll-to-bottom and the built-in middle-drag
        // reference) and the edge-gutter scrollbar geometry, so the old wrapping
        // transcriptViewportContainer Item and its reparented MiddleDragScroll
        // are gone.
            ColumnLayout {
                id: scrollableContent
                // Item 55: while a (re)transcription for THIS file has started
                // but no new segment has streamed in yet, dim + hard-disable the
                // stale transcript so it can't be selected/hovered/edited (the
                // transcribingOverlay in the bodyOverlay slot shows a spinner on
                // top and covers it, blocking pointer + wheel). The moment live
                // output begins (awaitingFirstSegment clears on the first live
                // tick) this reverts and the user watches it stream in. Applied
                // to the content column because ScrollPage owns the ScrollView
                // now. A true gaussian blur was deliberately avoided here: it
                // needs a new QtQuick.Effects/Qt5Compat.GraphicalEffects import,
                // and per project memory a fresh QML import can crash-on-load (or
                // silently freeze) under AOT — uncheckable without a live GUI run
                // — so dim is the AOT-safe half of "blur/dim".
                enabled: !mainPanel.showTranscribingOverlay
                opacity: mainPanel.showTranscribingOverlay ? 0.25 : 1
                // Bind to ScrollPage's availableContentWidth: the transcript
                // keeps its true 16px right inset and the overlay scrollbar
                // floats in the reclaimed gutter (see ScrollPage.qml).
                width: notePage.availableContentWidth
                spacing: 12

                // Item 13: there was previously no way to see generated tags at all
                // (auto-labeling wrote them to frontmatter but nothing displayed
                // them). Small rounded pills, one per comma-separated tag.
                Flow {
                    id: tagsRow
                    Layout.fillWidth: true
                    spacing: 6
                    // Item P15/P20: the add-tag affordance needs to be
                    // reachable even before this file has any tags at all —
                    // visible whenever there's an actual transcript to attach
                    // tags to (mirrors preludeContainer/chapterListColumn's own
                    // gating), not just once currentTags() is non-empty.
                    visible: mainPanel.hasTranscript

                    Repeater {
                        model: mainPanel.currentTags()

                        Rectangle {
                            id: tagPill
                            required property string modelData
                            radius: AppSettings.borderRadius
                            color: tagPillHover.hovered ? "#3f3f3f" : "#333333"
                            border.width: 1
                            border.color: Theme.borderStrong
                            implicitWidth: tagPillRow.implicitWidth + 12
                            implicitHeight: tagPillRow.implicitHeight + 6

                            // tagField below is directly editable (plain
                            // click places the cursor, same idiom as
                            // titleField/preludeArea) — filtering the
                            // sidebar tree by this tag now goes through
                            // tagSortIcon's dedicated trigger instead of the
                            // pill body, so the two can never both fire off
                            // one tap.
                            HoverHandler {
                                id: tagPillHover
                                cursorShape: Qt.PointingHandCursor
                            }

                            // Reverts to the true on-disk tag text if the
                            // field was left empty, otherwise hands off to
                            // renameTag() — mirrors speakerPillItem's own
                            // commitOrRevert (see its doc comment for why
                            // this can't live inside renameTag() itself).
                            function commitOrRevert(value) {
                                if (value.trim() === "") {
                                    tagField.text = tagPill.modelData
                                    return
                                }
                                mainPanel.renameTag(tagPill.modelData, value)
                            }

                            RowLayout {
                                id: tagPillRow
                                anchors.centerIn: parent
                                spacing: 4

                                TextField {
                                    id: tagField
                                    Layout.alignment: Qt.AlignVCenter
                                    color: "#cccccc"
                                    font.pixelSize: 11
                                    text: tagPill.modelData
                                    selectByMouse: true
                                    // See titleField's doc comment — zeroed
                                    // to match the old Label's tight sizing
                                    // inside this pill.
                                    padding: 0
                                    leftPadding: 0
                                    rightPadding: 0
                                    topPadding: 0
                                    bottomPadding: 0

                                    onAccepted: tagPill.commitOrRevert(text)
                                    onActiveFocusChanged: if (!activeFocus) tagPill.commitOrRevert(text)

                                    background: UnderlineField { implicitHeight: 0; plainUntilHovered: true; focused: tagField.activeFocus }

                                    Keys.onEscapePressed: tagField.text = tagPill.modelData

                                    // Item 13: editable text needs an I-beam.
                                    HoverHandler { cursorShape: Qt.IBeamCursor }
                                }

                                // Filters the sidebar tree to files with this
                                // tag (see mainPanel.tagActivated's doc
                                // comment) — a dedicated button rather than
                                // the pencil-icon-doubling-as-edit-trigger
                                // this used to be, since that made a single
                                // tap on the icon fire BOTH the sidebar
                                // filter and (via tagField's own click-to-
                                // edit) a text edit at once.
                                AppIcon {
                                    id: tagSortIcon
                                    Layout.alignment: Qt.AlignVCenter
                                    name: "filter"
                                    iconSize: 10
                                    color: tagSortIconHover.hovered ? Theme.textPrimary : Theme.textSecondary

                                    HoverHandler {
                                        id: tagSortIconHover
                                        cursorShape: Qt.PointingHandCursor
                                    }
                                    TapHandler {
                                        onTapped: mainPanel.tagActivated(tagPill.modelData)
                                    }
                                }

                                // Item P20: per-pill delete — always visible
                                // (not hover-only) so its hit target is
                                // predictable/discoverable, same treatment
                                // as searchCloseButton's "x" elsewhere in
                                // this file.
                                AppIcon {
                                    id: tagRemoveIcon
                                    name: "close"
                                    iconSize: 12
                                    color: tagRemoveHover.hovered ? Theme.danger : Theme.textSecondary

                                    HoverHandler {
                                        id: tagRemoveHover
                                        cursorShape: Qt.PointingHandCursor
                                    }
                                    TapHandler {
                                        onTapped: mainPanel.removeTag(tagPill.modelData)
                                    }
                                }
                            }
                        }
                    }

                    // Always-editable "+ tag" entry field (see titleField's
                    // doc comment for the general pattern) — a single
                    // TextField is present at all times, no icon/TextField
                    // swap. Committing (Enter) adds the tag and clears back
                    // to the placeholder; losing focus without committing
                    // discards whatever partial text was typed (this field
                    // has no persisted value of its own to revert to, unlike
                    // title/prelude/speaker/tag-rename above).
                    TextField {
                        id: tagAddField
                        implicitWidth: 70
                        font.pixelSize: 11
                        placeholderText: "+ " + qsTr("tag")
                        // See titleField's doc comment — zeroed to match the
                        // tag pills' own tight sizing.
                        padding: 0
                        leftPadding: 0
                        rightPadding: 0
                        topPadding: 0
                        bottomPadding: 0

                        background: UnderlineField { implicitHeight: 0; plainUntilHovered: true; focused: tagAddField.activeFocus }

                        onAccepted: {
                            mainPanel.addTag(text)
                            text = ""
                        }
                        onActiveFocusChanged: if (!activeFocus) text = ""
                        Keys.onEscapePressed: text = ""

                        // Item 13: editable text needs an I-beam.
                        HoverHandler { cursorShape: Qt.IBeamCursor }
                    }
                }

                // Item 11 (Wave E): one pill per distinct speaker id detected in
                // THIS transcript, showing its current display name (default:
                // "Speaker N"). Click-to-rename mirrors the title's own
                // Label<->TextField swap pattern; renaming rewrites every one of
                // that speaker's "[name]" tags directly in the transcript BODY
                // itself (see TranscriptionController::renameSpeaker()'s
                // 2026-07-07 rewrite) — this pill's label updates optimistically
                // via pendingSpeakerRenames the instant you commit, ahead of the
                // background file rewrite actually finishing. Drag one pill onto
                // another to MERGE them (see mainPanel.handleSpeakerDrop() and
                // TranscriptionController::mergeSpeakers()) — for over-split
                // diarization, where two pills are really the same person.
                Flow {
                    id: speakersRow
                    Layout.fillWidth: true
                    spacing: 6
                    visible: mainPanel.distinctSpeakerIds.length > 0

                    Repeater {
                        model: mainPanel.distinctSpeakerIds

                        Item {
                            id: speakerPillItem
                            required property string modelData
                            readonly property bool confirmingMerge: mainPanel.mergeConfirmSourceId === modelData
                            readonly property bool pendingMerge: modelData in mainPanel.pendingSpeakerMerges
                            readonly property bool isDropTarget: mainPanel.dragHoverSpeakerId === modelData
                            implicitWidth: (confirmingMerge ? mergeConfirmRow.implicitWidth
                                            : speakerPillRow.implicitWidth) + 12
                            implicitHeight: 22
                            opacity: pendingMerge ? 0.5 : 1.0

                            // Reverts to the true on-disk name if the field was
                            // left empty, otherwise hands off to
                            // commitSpeakerRename() — factored out since both
                            // onAccepted and onActiveFocusChanged below need it,
                            // and commitSpeakerRename() itself has no way to
                            // reach back into this specific delegate's field.
                            function commitOrRevert(value) {
                                if (value.trim() === "") {
                                    speakerNameField.text = mainPanel.effectiveSpeakerName(speakerPillItem.modelData)
                                    return
                                }
                                mainPanel.commitSpeakerRename(speakerPillItem.modelData, value)
                            }

                            Rectangle {
                                anchors.fill: parent
                                radius: AppSettings.borderRadius
                                color: speakerPillItem.isDropTarget ? "#3f7f5f"
                                       : speakerPillHover.hovered ? "#3f5f7f" : "#2f4f5f"
                                border.width: speakerPillItem.isDropTarget ? 2 : 1
                                border.color: speakerPillItem.isDropTarget ? "#6fdf9f" : "#4a6a7a"
                            }
                            HoverHandler {
                                id: speakerPillHover
                                cursorShape: Qt.PointingHandCursor
                                enabled: !speakerPillItem.confirmingMerge
                            }
                            // Filtering the sidebar file tree by this speaker
                            // goes through speakerSortIcon's dedicated trigger
                            // (mirrors tagPill's tagSortIcon); the merge-drag
                            // gesture goes through the dedicated speakerDragHandle
                            // grip below. Both are their own icons rather than
                            // overloading the pill body / the name field's click
                            // — a plain click on the pill just places the text
                            // cursor in speakerNameField (StyleGuide "Always-
                            // editable text": give a secondary action its own
                            // trigger, never the field's own click). This is what
                            // resolves the old text-selection-vs-merge-drag
                            // conflict: the name field is freely click-dragged to
                            // select text, and only the grip starts a merge.

                            // Always-editable (see titleField's doc comment
                            // for the general pattern): a single TextField is
                            // present at all times, no Label/TextField swap,
                            // and a plain click places the cursor directly
                            // (activeFocusOnPress/selectByMouse both true) —
                            // speakerSortIcon is the dedicated sidebar-filter
                            // trigger instead. Hidden only while this pill is
                            // showing its merge-confirmation row.
                            Row {
                                id: speakerPillRow
                                anchors.centerIn: parent
                                spacing: 3
                                visible: !speakerPillItem.confirmingMerge

                                // Item: dedicated drag grip — makes the
                                // merge-by-drag gesture discoverable (a plain
                                // click on the pill edits the name, so the drag
                                // needs its own handle) and, being its own
                                // element, it never collides with the name
                                // field's click-drag text selection. Dragging
                                // one speaker's grip onto another pill merges
                                // them (see mainPanel.handleSpeakerDrop /
                                // speakerIdAtScenePos). target:null so the pill
                                // itself doesn't move — only the ghost follows.
                                AppIcon {
                                    id: speakerDragHandle
                                    anchors.verticalCenter: parent.verticalCenter
                                    name: "drag-handle"
                                    iconSize: 10
                                    color: speakerDragHandleHover.hovered || speakerPillDrag.active
                                           ? Theme.textPrimary : Theme.textSecondary

                                    HoverHandler {
                                        id: speakerDragHandleHover
                                        cursorShape: Qt.SizeAllCursor
                                        enabled: !speakerPillItem.confirmingMerge && !speakerPillItem.pendingMerge
                                    }
                                    DragHandler {
                                        id: speakerPillDrag
                                        enabled: !speakerPillItem.confirmingMerge && !speakerPillItem.pendingMerge
                                        target: null
                                        onActiveChanged: {
                                            if (active) {
                                                mainPanel.draggingSpeakerId = speakerPillItem.modelData
                                                return
                                            }
                                            if (mainPanel.draggingSpeakerId !== speakerPillItem.modelData)
                                                return
                                            if (mainPanel.dragHoverSpeakerId !== "")
                                                mainPanel.handleSpeakerDrop(speakerPillItem.modelData, mainPanel.dragHoverSpeakerId)
                                            mainPanel.draggingSpeakerId = ""
                                            mainPanel.dragHoverSpeakerId = ""
                                        }
                                        onCentroidChanged: {
                                            if (!active)
                                                return
                                            const scenePos = speakerDragHandle.mapToItem(mainPanel, centroid.position.x,
                                                                                          centroid.position.y)
                                            mainPanel.dragGhostX = scenePos.x
                                            mainPanel.dragGhostY = scenePos.y
                                            mainPanel.dragHoverSpeakerId =
                                                mainPanel.speakerIdAtScenePos(scenePos.x, scenePos.y, speakerPillItem.modelData)
                                        }
                                    }
                                }

                                TextField {
                                    id: speakerNameField
                                    anchors.verticalCenter: parent.verticalCenter
                                    color: Theme.textPrimary
                                    font.pixelSize: 11
                                    text: mainPanel.effectiveSpeakerName(speakerPillItem.modelData)
                                    selectByMouse: true
                                    // See titleField's doc comment — zeroed
                                    // so this fits the pill's fixed 22px
                                    // implicitHeight the same as the old
                                    // Label did, instead of overflowing it.
                                    padding: 0
                                    leftPadding: 0
                                    rightPadding: 0
                                    topPadding: 0
                                    bottomPadding: 0

                                    onAccepted: speakerPillItem.commitOrRevert(text)
                                    onActiveFocusChanged: if (!activeFocus) speakerPillItem.commitOrRevert(text)

                                    background: UnderlineField { implicitHeight: 0; plainUntilHovered: true; focused: speakerNameField.activeFocus }

                                    // Item 13: editable text needs an I-beam.
                                    HoverHandler { cursorShape: Qt.IBeamCursor }

                                    Keys.onEscapePressed: speakerNameField.text = mainPanel.effectiveSpeakerName(speakerPillItem.modelData)

                                    // Resyncs after a FAILED rename specifically
                                    // — a successful one instead reparses via
                                    // sentencesRevision, which recreates this
                                    // whole delegate fresh (see
                                    // distinctSpeakerIds), so only the failure
                                    // path needs an explicit reset back to the
                                    // true on-disk name. text's binding above is
                                    // a one-shot, severed the moment the user
                                    // types (same caveat as titleField/
                                    // preludeArea).
                                    Connections {
                                        target: mainPanel.transcriptionController
                                        function onSpeakerRenamed(fileUrl, oldName, ok) {
                                            if (!ok && oldName === speakerPillItem.modelData)
                                                speakerNameField.text = mainPanel.effectiveSpeakerName(speakerPillItem.modelData)
                                        }
                                    }
                                }

                                // Filters the sidebar tree to files with this
                                // speaker (see mainPanel.speakerActivated's
                                // doc comment) — mirrors tagSortIcon above,
                                // and for the same reason: this used to be a
                                // pencil icon doubling as the rename trigger,
                                // which fired both the rename focus AND (via
                                // the pill-body tap) the sidebar filter off a
                                // single tap.
                                AppIcon {
                                    id: speakerSortIcon
                                    anchors.verticalCenter: parent.verticalCenter
                                    name: "sort"
                                    iconSize: 10
                                    color: speakerSortIconHover.hovered ? Theme.textPrimary : Theme.textSecondary

                                    HoverHandler {
                                        id: speakerSortIconHover
                                        cursorShape: Qt.PointingHandCursor
                                    }
                                    TapHandler {
                                        enabled: !speakerPillItem.confirmingMerge && !speakerPillItem.pendingMerge
                                        onTapped: mainPanel.speakerActivated(mainPanel.effectiveSpeakerName(speakerPillItem.modelData))
                                    }
                                }
                            }

                            // Inline "Merge into X?" confirmation — see
                            // mainPanel.resolveSpeakerMerge()'s doc comment for
                            // when this shows instead of committing immediately.
                            // Inline rather than a popup, per StyleGuide.md
                            // ("Warning banners: inline, not a popup/toast").
                            RowLayout {
                                id: mergeConfirmRow
                                anchors.centerIn: parent
                                visible: speakerPillItem.confirmingMerge
                                spacing: 4
                                Label {
                                    text: qsTr("Merge into %1?").arg(mainPanel.effectiveSpeakerName(mainPanel.mergeConfirmTargetId))
                                    color: Theme.textPrimary
                                    font.pixelSize: 11
                                }
                                AppIcon {
                                    name: "check"
                                    iconSize: 12
                                    color: "#8fdf9f"
                                    HoverHandler { cursorShape: Qt.PointingHandCursor }
                                    TapHandler { onTapped: mainPanel.confirmSpeakerMerge(true) }
                                }
                                AppIcon {
                                    name: "cancel"
                                    iconSize: 12
                                    color: Theme.danger
                                    HoverHandler { cursorShape: Qt.PointingHandCursor }
                                    TapHandler { onTapped: mainPanel.confirmSpeakerMerge(false) }
                                }
                            }
                        }
                    }
                }

                Label {
                    Layout.fillWidth: true
                    visible: mainPanel.currentMdUrl === ""
                    text: qsTr("No file selected")
                    color: "white"
                }

                RowLayout {
                    spacing: 12
                    visible: mainPanel.hasAudio && mainPanel.hasMetadata

                    TextEdit {
                        objectName: "previouslyTranscribedInfo"
                        Layout.fillWidth: true
                        wrapMode: TextEdit.WordWrap
                        readOnly: true
                        selectByMouse: true
                        selectByKeyboard: true
                        // Focusable so a keyboard Copy (Ctrl+C) reaches this
                        // read-only TextEdit — selectByMouse alone highlights
                        // without focus, and StandardKey.Copy is a key event
                        // that only lands on the focused item. Matches every
                        // other selectable field in this panel.
                        activeFocusOnPress: true
                        text: {
                            const base = qsTr("Previously transcribed — model: %1")
                                             .arg(mainPanel.metadata.model || "?")
                            if (!mainPanel.metadata.speakerDetection)
                                return base + qsTr(", speaker detection: off")
                            const seg = mainPanel.metadata.segmentationModel || ""
                            const emb = mainPanel.metadata.embeddingModel || ""
                            // Transcripts diarized before the models were recorded
                            // fall back to the old "on".
                            if (seg === "" && emb === "")
                                return base + qsTr(", speaker detection: on")
                            return base + qsTr(", speaker detection: %1 + %2").arg(seg).arg(emb)
                        }
                        color: Theme.textMuted
                        font.pixelSize: 11

                        HoverHandler { cursorShape: Qt.IBeamCursor }
                    }
                }

                // Feature 2 (upgrades O26's old read-only single-line
                // summary): a click-to-edit, multi-paragraph prelude —
                // same muted secondary-text treatment as the "Previously
                // transcribed" label above (color/pixelSize) when not being
                // edited, just larger and unbolded so it reads as
                // descriptive prose rather than a status line. Same
                // click-to-edit idiom as titleLabel/titleField above, but a
                // TextArea (multi-line) that autosaves on blur instead of
                // commit-on-Enter — plain Enter must insert a newline here.
                // Shown whenever the open file has a transcript to describe,
                // orphaned .md and audio-backed alike (the prelude lives in the
                // .md body and reads/writes through currentMdUrl either way).
                PreludeEditor {
                    id: preludeEditor
                    panel: mainPanel
                    Layout.fillWidth: true
                    visible: mainPanel.hasMetadata
                }

                // Feature 3c: collapsible chapter list — same tap-to-seek
                // idiom as the seek-bar tick marks (3b) and
                // timestampClickArea further below. Only ever visible once
                // mainPanel.chapters is non-empty — unlike the prelude editor
                // above, this IS shown for an orphaned .md (chapters only
                // ever read the .md body, never audio — see
                // refreshPreludeAndChapters()'s doc comment); tapping a
                // timestamp is just inert there since mainPanel.player has no
                // source to seek. Collapse toggle uses the same "+"/"−"
                // disclosure glyph Sidebar.qml already uses for its tree
                // rows, rather than introducing a new ▶/▼ convention;
                // defaults EXPANDED since most files will only have a
                // handful of chapters and hiding useful navigation by
                // default would be the wrong default — the toggle exists as
                // an escape hatch for the rare long list, not a resting
                // state.
                ChapterTocList {
                    id: chapterTocList
                    panel: mainPanel
                    Layout.fillWidth: true
                    visible: mainPanel.chapters.length > 0
                }

                // Speaker detection was requested but couldn't actually run (models
                // unavailable/failed to load) — the transcription still completed,
                // ASR-only, rather than being blocked (see DESIGN.md "Speaker
                // diarization" graceful-degradation decision).
                //
                // Stays an INLINE banner (StyleGuide.md "Warning banners") rather
                // than moving to a snackbar with the transient errors: this is a
                // durable property of the OPEN FILE's stored metadata, not a report
                // that an action just failed. It must persist for as long as the
                // file is open and reappear whenever it's reopened — a snackbar
                // auto-expires, which would lose it.
                Label {
                    Layout.fillWidth: true
                    visible: mainPanel.hasAudio && mainPanel.hasMetadata
                             && !!mainPanel.metadata.speakerDetectionError
                    text: qsTr("Speaker detection was requested but did not run: %1")
                          .arg(mainPanel.metadata.speakerDetectionError || "")
                    color: "#e0a25a"
                    font.pixelSize: 11
                    wrapMode: Text.WordWrap
                }

                // Silence-skipping VAD (anti-repetition-hallucination gating +
                // app-level windowing) was expected but couldn't run — the ~2 MB
                // Silero model failed to download or VAD detection failed. The
                // transcript still completed with weaker hallucination hardening
                // (beam search / entropy_thold / suppress_nst remain). Same INLINE
                // durable-metadata banner as speakerDetectionError above (a
                // property of the OPEN FILE's stored metadata, not a transient
                // action failure), NOT a snackbar. Empty vadError (subprocess
                // engines never attempt VAD; legacy pre-vad transcripts) → no
                // banner.
                Label {
                    Layout.fillWidth: true
                    visible: mainPanel.hasAudio && mainPanel.hasMetadata
                             && !!mainPanel.metadata.vadError
                    text: qsTr("Silence detection did not run, so repetition artifacts are more likely: %1")
                          .arg(mainPanel.metadata.vadError || "")
                    color: "#e0a25a"
                    font.pixelSize: 11
                    wrapMode: Text.WordWrap
                }

                RowLayout {
                    id: modelRow
                    spacing: 12
                    Layout.fillWidth: true
                    visible: mainPanel.hasAudio

                    // Speaker detection is no longer a transcribe-time toggle —
                    // it's the standalone "Detect speakers" action (the
                    // detectSpeakersButton icon in the cluster above) / the
                    // runAllJobs pipeline step. Transcribe here is pure ASR.

                    Label {
                        text: qsTr("Model")
                        color: Theme.textMuted
                    }

                    // Transcription model. Shares the row evenly with the two
                    // diarization pickers below via matching Layout.fillWidth
                    // + preferredWidth; ModelComboBox elides the closed label
                    // (full text in tooltip/popup) so nothing overflows.
                    ModelComboBox {
                        id: modelCombo
                        Layout.fillWidth: true
                        Layout.preferredWidth: 1
                        model: mainPanel.transcriptionModelList
                        selectedId: AppSettings.defaultModel
                        onPicked: (id) => AppSettings.defaultModel = id
                    }

                    // VAD status dot. Green when the selected model runs through
                    // the app's Silero VAD path (silence-gated windows — fewer
                    // repetition hallucinations); red in two cases: the engine
                    // self-segments and bypasses VAD, OR the open file's stored
                    // transcript recorded a VAD failure (`vadError` — the same
                    // signal the fallback banner above reads), i.e. VAD was
                    // expected but the ~2 MB model / detection didn't run for
                    // THAT file. A status dot (Rectangle circle), not an icon —
                    // StyleGuide "Not every symbol is an icon". Hover-for-info
                    // only, so no pointing-hand cursor / HoverBorder.
                    Rectangle {
                        id: vadIndicator
                        readonly property bool usesVad: mainPanel.transcriptionController
                            ? mainPanel.transcriptionController.modelUsesVad(modelCombo.currentValue) : false
                        // Whisper-only per-window silence GATING (a subset of
                        // usesVad — parakeet gets windowing but not this), so
                        // the green tooltip can scope its silence-skipping claim.
                        readonly property bool usesGating: mainPanel.transcriptionController
                            ? mainPanel.transcriptionController.modelUsesVadGating(modelCombo.currentValue) : false
                        // Ground truth from the OPEN file's stored transcript:
                        // VAD was attempted but failed (guarded exactly like the
                        // banner above — metadata is a valid map once hasMetadata).
                        readonly property bool runtimeVadFailed: mainPanel.hasMetadata
                            && !!mainPanel.metadata.vadError
                        readonly property bool vadOk: usesVad && !runtimeVadFailed
                        implicitWidth: 10
                        implicitHeight: 10
                        radius: 5
                        color: vadOk ? Theme.success : Theme.danger
                        Layout.alignment: Qt.AlignVCenter

                        HoverHandler {
                            id: vadHover
                        }
                        ToolTip {
                            visible: vadHover.hovered
                            delay: 400
                            // Width-capped so the explanation wraps instead of
                            // stretching into one long line off the window.
                            width: 320
                            // Four states: engine bypasses VAD → OFF; VAD
                            // expected but failed on the open file → FAILED
                            // (with the recorded reason); ON with whisper's
                            // per-window silence gating → full; ON via windowing
                            // only (parakeet) → pause-splitting, gating scoped
                            // out honestly.
                            text: !vadIndicator.usesVad
                                ? qsTr("Voice activity detection is OFF for this engine.\n\nThis engine segments audio itself, so the whole file is sent without VAD silence-gating — repetition hallucinations over silence are more likely.")
                                : vadIndicator.runtimeVadFailed
                                    ? qsTr("Voice activity detection FAILED on the open file: %1\n\nSilence detection didn't run for this transcript, so repetition artifacts are more likely. Re-transcribe once the VAD model is available.")
                                        .arg(mainPanel.metadata.vadError || "")
                                    : vadIndicator.usesGating
                                        ? qsTr("Voice activity detection is ON.\n\nSilero VAD finds the spoken regions and skips silent / non-speech audio, so the model is never fed empty stretches. This suppresses repetition hallucinations and splits long files at natural pauses.")
                                        : qsTr("Voice activity detection is ON (windowing only).\n\nSilero VAD splits this long audio into chunks at natural pauses, which reduces drift. Per-window silence gating — dropping silent frames so the model isn't fed empty stretches — is a whisper-only feature this engine doesn't get.")
                        }
                    }

                    // The two speaker-diarization models (segmentation +
                    // embedding). They tune the standalone "Detect speakers"
                    // action / the runAllJobs pipeline step — both of which
                    // already read these same AppSettings keys — surfaced here
                    // for quick access alongside the fuller controls in
                    // Settings › Speaker detection. Sensitivity is on its own
                    // row below.
                    Label {
                        text: qsTr("Speakers")
                        color: Theme.textMuted
                    }

                    ModelComboBox {
                        id: segmentationCombo
                        Layout.fillWidth: true
                        Layout.preferredWidth: 1
                        model: mainPanel.segmentationModelList
                        selectedId: AppSettings.diarizationSegmentationModel
                        onPicked: (id) => AppSettings.diarizationSegmentationModel = id
                    }

                    ModelComboBox {
                        id: embeddingCombo
                        Layout.fillWidth: true
                        Layout.preferredWidth: 1
                        model: mainPanel.embeddingModelList
                        selectedId: AppSettings.diarizationEmbeddingModel
                        onPicked: (id) => AppSettings.diarizationEmbeddingModel = id
                    }

                    // O35 / Item 25: the coarse "gpu"/"cpu" kind of the
                    // backend that loaded the active model (activeBackendKind,
                    // not the raw Vulkan/CUDA/CPU name) — matching the bottom
                    // bar's own indicator. Empty (and hidden) until the first
                    // model load completes in this session.
                    Label {
                        text: mainPanel.transcriptionController ? mainPanel.transcriptionController.activeBackendKind : ""
                        visible: text !== ""
                        color: Theme.textSecondary
                        font.pixelSize: 11
                    }
                }

                // Diarization clustering-distance cutoff for the "Detect
                // speakers" action / runAllJobs step (both read
                // AppSettings.diarizationSensitivity). Continuous knob (like
                // SettingsPopup's copy) clamped away from the 0/1 extremes,
                // which degenerate — lower = stricter = MORE speakers, higher
                // = more merging = FEWER.
                RowLayout {
                    id: sensitivityRow
                    spacing: 12
                    Layout.fillWidth: true
                    visible: mainPanel.hasAudio

                    Label {
                        text: qsTr("Sensitivity")
                        color: Theme.textMuted
                    }

                    Slider {
                        id: diarizationSensitivitySlider
                        Layout.fillWidth: true
                        from: 0.1
                        to: 0.9
                        value: AppSettings.diarizationSensitivity
                        hoverEnabled: true
                        onMoved: AppSettings.diarizationSensitivity = value

                        HoverHandler {
                            cursorShape: Qt.PointingHandCursor
                        }
                        HoverBorder {
                            anchors.fill: parent
                            active: diarizationSensitivitySlider.hovered
                        }
                    }

                    Label {
                        text: diarizationSensitivitySlider.value.toFixed(2)
                        color: Theme.textSecondary
                        font.pixelSize: 11
                    }
                }

            TextArea {
                id: transcriptArea
                objectName: "transcriptArea"
                Layout.fillWidth: true
                visible: mainPanel.transcriptBody !== ""
                // Always-editable (see titleField's doc comment for the
                // general pattern) — kept read-only only while there's
                // nothing transcribed yet to edit, or while THIS file's
                // transcription job is actively appending live segments
                // (mainPanel.transcribingThisFile) — editing concurrently
                // with that would fight the same text. Return is blocked
                // from inserting a newline (see Keys.onReturnPressed below)
                // to preserve the one-line-per-sentence invariant
                // displaySentences' char offsets depend on; a multi-line
                // paste slipping past that is instead caught defensively at
                // commit time (see the Keys.onReturnPressed comment below).
                // Commit happens on blur (onActiveFocusChanged), diffed
                // against displaySentences by
                // mainPanel.commitTranscriptEdits().
                readOnly: !mainPanel.hasMetadata || mainPanel.transcribingThisFile
                wrapMode: TextArea.Wrap
                text: mainPanel.displayText
                color: Theme.textPrimary
                // See titleField's doc comment — zeroed (except leftPadding,
                // which is intentionally set below to reserve the gutter
                // columns) to match the old read-only TextArea's sizing.
                padding: 0
                rightPadding: 0
                topPadding: 0
                bottomPadding: 0
                background: UnderlineField { implicitHeight: 0; plainUntilHovered: true; focused: transcriptArea.activeFocus }
                selectByMouse: true
                selectByKeyboard: true

                // Return is the primary defense against inserting a new
                // line (see the doc comment above) — a multi-line PASTE
                // slipping past this is instead caught defensively at
                // commit time: commitTranscriptEdits() rejects (and
                // reverts) the whole edit if the resulting line count
                // doesn't match displaySentences.length, rather than trying
                // to sanitize text live on every keystroke here (which would
                // fight cursor position for a rare edge case already
                // defended at the one point it actually matters — commit).
                Keys.onReturnPressed: (event) => event.accepted = true
                Keys.onEnterPressed: (event) => event.accepted = true
                // Copy has to go through copySelectionWithChapters(), which puts
                // the chapter titles the selection swept over back into the
                // copied text — the native copy sees only this document, where
                // each title is a blank reserved line (see displayData). Falls
                // through to the native copy when nothing is selected.
                Keys.onPressed: (event) => {
                    if (event.matches(StandardKey.Copy))
                        event.accepted = mainPanel.copySelectionWithChapters()
                }
                onActiveFocusChanged: if (!activeFocus) mainPanel.commitTranscriptEdits(transcriptArea.text)
                // Reserves space for the timestamp + speaker gutters below
                // — neither is part of this control's actual text (see
                // displayText's doc comment), so a click-drag selection can
                // never touch them; this is just the visual margin they're
                // drawn in.
                leftPadding: (mainPanel.timestampGutterWidth > 0 ? mainPanel.timestampGutterWidth + mainPanel.gutterSpacing : 0)
                             + mainPanel.speakerGutterWidth
                             + (mainPanel.speakerGutterWidth > 0 ? mainPanel.gutterSpacing : 0)
                // Whole-control default; the timestamp gutter's own
                // MouseArea (below) overrides this to a pointer while
                // hovering directly over a clickable timestamp. TextArea
                // (a Control/Item) has no plain "cursorShape" property —
                // only MouseArea/PointerHandler expose one — hence the
                // HoverHandler here rather than a direct property set.
                HoverHandler {
                    cursorShape: Qt.IBeamCursor
                }
                // Regression fix: Item 7 made ALL native selection invisible
                // (transparent) to avoid double-highlighting jumpToNextMatch's
                // select() call against searchMatchOverlay below — but that
                // also hid genuine user click-drag selection entirely, which
                // read as "selection stopped working". Only hide the native
                // highlight when the CURRENT selection is exactly the active
                // search match (a purely reactive check — the moment the user
                // drags a different selection, this is false again and the
                // native highlight reappears on its own).
                readonly property bool selectionIsSearchMatch: mainPanel.searchMatchIndex >= 0
                    && transcriptArea.selectionStart === mainPanel.searchMatchStart
                    && transcriptArea.selectionEnd === mainPanel.searchMatchStart + mainPanel.searchMatchLength
                selectionColor: transcriptArea.selectionIsSearchMatch ? "transparent" : Qt.rgba(0.35, 0.55, 0.85, 0.5)
                selectedTextColor: transcriptArea.selectionIsSearchMatch ? transcriptArea.color : "#ffffff"

                // Real bug, found via an offscreen QML reproduction (not
                // guessed) — see .task/plan.md for the repro transcript.
                // Querying positionToRectangle() from EACH gutter delegate's
                // own binding can run BEFORE this control's internal text
                // layout has caught up to a newly-changed `text` value (Qt
                // logs "QTextCursor::setPosition: position N out of range"
                // when this happens); every position past the stale length
                // silently returns a zeroed rect, collapsing every later
                // gutter entry onto row 0 — exactly "an end gutter takes the
                // same position as the start gutter". Computed ONCE here
                // (shared by both gutter Repeaters below, instead of two
                // independent — and independently stale — queries per
                // sentence) with an EXPLICIT dependency on
                // transcriptArea.length (the actual current document
                // length), so this genuinely re-evaluates once layout
                // catches up rather than only reacting to displaySentences
                // itself changing; the bounds guard means an in-between
                // stale read never warns/misbehaves, it just recomputes
                // correctly moments later.
                // The LIVE document's line-start offsets, one per line.
                // displaySentences' own `start` values index displayText — the
                // MODEL's string — and the `text: mainPanel.displayText`
                // binding is severed the moment the user types (see
                // onDisplayTextChanged), so every offset past the caret goes
                // stale by the edit delta mid-edit. Feeding those to
                // positionToRectangle() resolves each overlay to the tail of
                // the PREVIOUS line, which is how a chapter heading ended up
                // painted on top of a sentence with its own reserved blank
                // line left empty. Line INDEX survives what the char offset
                // doesn't: displayData appends exactly one line per entry and
                // Return is blocked, so entry idx N is always document line N.
                readonly property var liveLineStarts: {
                    void transcriptArea.length
                    const t = transcriptArea.text
                    const starts = [0]
                    let pos = t.indexOf("\n")
                    while (pos !== -1) {
                        starts.push(pos + 1)
                        pos = t.indexOf("\n", pos + 1)
                    }
                    return starts
                }

                // Live offset of the line `entry` owns, falling back to the
                // model offset when the two have desynced (see `aligned`).
                function liveStartOf(entry) {
                    const starts = transcriptArea.liveLineStarts
                    return starts.length === mainPanel.displaySentences.length
                        ? starts[entry.idx] : entry.start
                }

                readonly property var gutterLineRects: {
                    const len = transcriptArea.length
                    const starts = transcriptArea.liveLineStarts
                    // Backspace at a line start MERGES two lines, breaking the
                    // idx-to-line correspondence. commitTranscriptEdits()
                    // rejects that edit wholesale on blur anyway, so for that
                    // transient fall back to the model offsets rather than
                    // indexing off the end of a short array — no worse than the
                    // behaviour this replaces, and no one-frame vanish of the
                    // headings on a state that's about to be reverted.
                    const aligned = starts.length === mainPanel.displaySentences.length
                    // Same width/padding/font dependency gap as
                    // safePositionToRectangle above — positionToRectangle()
                    // is called directly here (not through that helper), so
                    // it needs its own explicit reads.
                    void transcriptArea.width
                    void transcriptArea.leftPadding
                    void transcriptArea.topPadding
                    void transcriptArea.font
                    return mainPanel.displaySentences.map((s, i) => {
                        const p = aligned ? starts[i] : s.start
                        return p <= len ? transcriptArea.positionToRectangle(p) : Qt.rect(0, 0, 0, 0)
                    })
                }

                // Two SEPARATE gutter columns (Item: independently stylable
                // areas, not one combined label) — each aligned to its
                // sentence's first visual line via gutterLineRects above.
                // The TIMESTAMP gutter shows one timestamp on EVERY sentence
                // line (gutterTimestampModel = every non-heading sentence),
                // so the elapsed time is always readable per segment. The
                // SPEAKER gutter still uses showGutter (see displayData) to
                // collapse a run of same-speaker sentences down to ONE name
                // at the start of that turn, rather than repeating the same
                // name on every line — speakers read as turn boundaries,
                // timestamps as a continuous per-line ruler.
                Repeater {
                    id: timestampGutterRepeater
                    model: mainPanel.gutterTimestampModel

                    Text {
                        required property var modelData
                        readonly property rect lineRect:
                            transcriptArea.gutterLineRects[modelData.idx] || Qt.rect(0, 0, 0, 0)
                        x: 0
                        y: lineRect.y
                        // Match the corresponding line's own height and
                        // center within it — the gutter's small font
                        // otherwise sits near the TOP of the (taller) main
                        // text line instead of level with its glyphs.
                        height: lineRect.height
                        width: mainPanel.timestampGutterWidth
                        horizontalAlignment: Text.AlignLeft
                        verticalAlignment: Text.AlignVCenter
                        text: mainPanel.formatGutterTimestamp(modelData.t0)
                        color: "#777777"
                        font.pixelSize: 10
                        font.underline: timestampClickArea.containsMouse && mainPanel.player

                        MouseArea {
                            id: timestampClickArea
                            anchors.fill: parent
                            enabled: !!mainPanel.player
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: mainPanel.player.position = modelData.t0 * 1000
                        }
                    }
                }

                // Item P27: TextEdit (not plain Text) so a speaker name in
                // the gutter is independently selectable/copyable with its
                // own I-beam cursor — this is a distinct Item from
                // transcriptArea (a sibling child positioned on top of it,
                // same as before), so its selection is naturally scoped to
                // just this one name and can never merge into the
                // transcript's own selection. TextEdit has no `elide`
                // (unlike the plain Text this replaces), but
                // speakerGutterWidth is already sized to the widest name
                // actually shown, so truncation was never really exercised
                // in practice.
                Repeater {
                    id: speakerGutterRepeater
                    model: mainPanel.gutterSpeakerModel

                    TextEdit {
                        required property var modelData
                        readonly property rect lineRect:
                            transcriptArea.gutterLineRects[modelData.idx] || Qt.rect(0, 0, 0, 0)
                        x: mainPanel.timestampGutterWidth + mainPanel.gutterSpacing
                        y: lineRect.y
                        height: lineRect.height
                        width: mainPanel.speakerGutterWidth
                        readOnly: true
                        selectByMouse: true
                        selectByKeyboard: true
                        activeFocusOnPress: false
                        // Right-aligned so the name sits right up against
                        // the transcript text (only gutterSpacing away)
                        // instead of leaving a ragged gap whenever the name
                        // is shorter than the column.
                        horizontalAlignment: TextEdit.AlignRight
                        verticalAlignment: TextEdit.AlignVCenter
                        text: modelData.speaker
                        color: "#8fa8bf"
                        font.pixelSize: 10
                        font.bold: true

                        HoverHandler {
                            cursorShape: Qt.IBeamCursor
                        }
                        // Toggles gutterSpeakerFilter — coexists with
                        // selectByMouse's native click-drag select/copy the
                        // same way speakerNameField's rename-vs-filter split
                        // coexists with speakerPillDrag: a plain tap (no
                        // movement) reaches this handler, a drag is claimed
                        // by the TextEdit's own native selection.
                        TapHandler {
                            onTapped: mainPanel.gutterSpeakerFilter =
                                (mainPanel.gutterSpeakerFilter === modelData.speaker) ? "" : modelData.speaker
                        }
                    }
                }

                // Dims every sentence NOT matching gutterSpeakerFilter above
                // — deliberately an overlay, not a change to
                // transcriptArea's actual text/selection (see
                // gutterSpeakerFilter's own doc comment for why). Reuses
                // gutterLineRects the same way the gutter labels do; a long
                // sentence that wraps across multiple visual lines only gets
                // its first visual line dimmed (cosmetic-only gap, the
                // underlying sentence data is unaffected either way).
                Repeater {
                    id: speakerDimOverlayRepeater
                    model: mainPanel.speakerDimModel

                    Rectangle {
                        required property var modelData
                        readonly property rect lineRect:
                            transcriptArea.gutterLineRects[modelData.idx] || Qt.rect(0, 0, 0, 0)
                        x: transcriptArea.leftPadding
                        y: lineRect.y
                        width: transcriptArea.width - transcriptArea.leftPadding
                        height: lineRect.height
                        color: Qt.rgba(0, 0, 0, 0.55)
                    }
                }

                // Item P32: one heading row per chapter marker, interleaved
                // into displaySentences by displayData above (see its own
                // doc comment) — each heading pseudo-entry reserved a real
                // blank line in transcriptArea's own text, so
                // gutterLineRects[modelData.idx] (same lookup the two gutter
                // Repeaters above use) gives it a genuine row of its own to
                // render into, rather than overlapping an adjacent
                // sentence's line. Spans the full transcript width
                // (including the gutter columns) since a chapter break
                // applies to the whole turn, not one speaker's column.
                Repeater {
                    id: chapterHeadingRepeater
                    model: mainPanel.chapterHeadingModel

                    ChapterHeadingRow {
                        // LHS names differ from the outer ids on purpose: a
                        // delegate binding `panel: panel` would resolve the RHS
                        // against the delegate's OWN property and self-bind to
                        // null. Distinct names let the RHS reach the outer ids.
                        panel: mainPanel
                        txArea: transcriptArea
                    }
                }

                // Silence phase 3 (queue item 085, locked decision): one
                // centered divider row per qualifying stored `silences:`
                // span (mainPanel.silenceMarkerModel), interleaved into
                // displayData/pushSilenceMarker exactly like a chapter
                // heading — same reserved-blank-line technique, same
                // gutterLineRects[idx] lookup for its row's geometry (see
                // that Repeater's own doc comment just above). Never
                // interactive (no TapHandler/MouseArea, no
                // HoverBorder) — it is pure decoration, not an editable or
                // clickable affordance; StyleGuide.md "Transcript silence
                // divider" is the source of truth for the glyphs/color/size
                // this renders (mainPanel.formatSilenceMarkerText).
                Repeater {
                    id: silenceMarkerRepeater
                    model: mainPanel.silenceMarkerModel

                    Text {
                        required property var modelData
                        readonly property rect lineRect:
                            transcriptArea.gutterLineRects[modelData.idx] || Qt.rect(0, 0, 0, 0)
                        x: 0
                        y: lineRect.y
                        width: transcriptArea.width
                        height: lineRect.height
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                        text: mainPanel.formatSilenceMarkerText(modelData.silenceSeconds)
                        color: "#5a5a5a"
                        font.italic: true
                        font.pixelSize: 11
                    }
                }

                Rectangle {
                    id: sentenceHighlightOverlay
                    visible: mainPanel.activeSentenceIndex >= 0
                    z: -1
                    radius: AppSettings.highlightRadius
                    color: AppSettings.highlightColor
                    x: mainPanel.activeSentenceRect.x
                    y: mainPanel.activeSentenceRect.y
                    width: mainPanel.activeSentenceRect.width
                    height: mainPanel.activeSentenceRect.height
                }

                // Item 7 (Wave E): Ctrl+F search match is now a real rounded
                // overlay too, not just the TextArea's native (unrounded,
                // system-styled) selection rectangle — transcriptArea's own
                // selectionColor is only made transparent WHILE the current
                // selection is exactly this match (see selectionIsSearchMatch
                // above), so this is the only visible highlight for it;
                // select() is still called (see jumpToNextMatch) purely for
                // its scroll-into-view behavior.
                Rectangle {
                    id: searchMatchOverlay
                    visible: mainPanel.searchMatchIndex >= 0
                    z: -1
                    radius: AppSettings.highlightRadius
                    color: AppSettings.highlightColor
                    x: mainPanel.searchMatchRect.x
                    y: mainPanel.searchMatchRect.y
                    width: mainPanel.searchMatchRect.width
                    height: mainPanel.searchMatchRect.height
                }
            }

            } // end scrollableContent ColumnLayout (ScrollPage's content child)

        // Item 55: loading overlay shown from the instant THIS file's
        // (re)transcription starts until the first new segment lands (see
        // showTranscribingOverlay / awaitingFirstSegment). A dim scrim over
        // the (already dimmed + disabled) stale transcript plus a centered
        // BusyIndicator + a task label (transcribingOverlayLabel, keyed off
        // currentPhase so it names the actual task). It also swallows pointer
        // input (TapHandler for clicks/drags, HoverHandler for the wait
        // cursor); wheel over the frozen transcript is blocked too, since a
        // covering overlay item doesn't let wheel fall through to the
        // ScrollView sibling beneath it. Lives in ScrollPage's bodyOverlay
        // slot, so it's scoped to exactly the scrolling body region (not the
        // sticky header). Reuses the built-in QQC2 BusyIndicator (the app had
        // no existing spinner component) so it inherits the app's control style.
        bodyOverlay: [
        Rectangle {
            id: transcribingOverlay
            anchors.fill: parent
            z: 2
            visible: mainPanel.showTranscribingOverlay
            color: Qt.rgba(AppSettings.mainBackgroundColor.r, AppSettings.mainBackgroundColor.g,
                           AppSettings.mainBackgroundColor.b, 0.55)

            // Block interaction with the frozen transcript beneath while shown.
            HoverHandler { cursorShape: Qt.BusyCursor }
            TapHandler { acceptedButtons: Qt.AllButtons }

            ColumnLayout {
                anchors.centerIn: parent
                spacing: 10

                BusyIndicator {
                    Layout.alignment: Qt.AlignHCenter
                    running: transcribingOverlay.visible
                }
                Label {
                    Layout.alignment: Qt.AlignHCenter
                    text: mainPanel.transcribingOverlayLabel
                    color: Theme.textPrimary
                    font.pixelSize: 13
                }
            }
        }
        ] // end bodyOverlay slot

        // ScrollPage bakes in the Chrome/Firefox-style middle-click autoscroll
        // for this page: its built-in MiddleDragScroll fills the whole ScrollPage
        // and gates on notePage.visible, so a middle-click anywhere on the note
        // page (tag pills / prelude / chapter list / transcript) arms the
        // transcript's scroll — the same reach the old reparented
        // middleDragScrollArea had, now owned by the shared base. See
        // MiddleDragScroll.qml / ScrollPage.qml.
    } // end notePage ScrollPage

    // Floating ghost label that visually represents an in-progress speaker
    // pill drag (see mainPanel.draggingSpeakerId/dragGhostX/Y) — a direct,
    // high-z sibling of the whole content layout above rather than a child
    // of speakersRow itself, since the real pill is deliberately never
    // moved (see draggingSpeakerId's doc comment).
    Label {
        id: speakerDragGhost
        visible: mainPanel.draggingSpeakerId !== ""
        z: 1000
        x: mainPanel.dragGhostX + 12
        y: mainPanel.dragGhostY + 12
        text: mainPanel.effectiveSpeakerName(mainPanel.draggingSpeakerId)
        color: Theme.textPrimary
        font.pixelSize: 11
        padding: 4
        background: Rectangle {
            radius: AppSettings.borderRadius
            color: "#2f4f5f"
            border.width: 1
            border.color: "#6fdf9f"
        }
    }

    // Recorder page: sibling of the two content blocks above (and of the
    // UsbGrabberPage below), gated on mainPanel.recording (toggled by
    // TitleBar's record button, see Main.qml's onRecordRequested). Exactly
    // one of these four top-level blocks is ever visible at once. Extracted
    // into its own RecorderPage.qml component (2026-07-19 decomposition); the
    // host wires the recorder + vault info and forwards openVaultRequested.
    RecorderPage {
        anchors.fill: parent
        visible: mainPanel.recording
        audioRecorder: mainPanel.audioRecorder
        vaultLocalPath: mainPanel.vaultLocalPath
        vaultPath: mainPanel.vaultPath
        vaultTreeModel: mainPanel.vaultTreeModel
        onOpenVaultRequested: mainPanel.openVaultRequested()
    }

    // Benchmark page: another sibling top-level block. Promoted out of Settings
    // to its own TitleBar button (see Main.qml's onBenchmarkRequested). Mutually
    // exclusive with the recorder/USB pages by construction — setTopLevelScreen()
    // guarantees at most one flag is true, so this needs no precedence negation.
    ScrollPage {
        id: benchmarkScroll
        anchors.fill: parent
        visible: mainPanel.benchmarkOpen

        BenchmarkPage {
            width: benchmarkScroll.availableContentWidth
            active: benchmarkScroll.visible
            transcriptionController: mainPanel.transcriptionController
            vaultTreeModel: mainPanel.vaultTreeModel
            // B2: default the benchmark to whatever file is open/highlighted in
            // the left panel (the same "current file" expression Main.qml feeds
            // the sidebar); empty ⇒ BenchmarkPage falls back to its picker.
            currentFilePath: mainPanel.currentMdUrl.replace("file://", "")
        }
    }

    // USB grabber page: sibling of the three content blocks above, gated on
    // mainPanel.usbGrabberOpen (toggled by TitleBar's USB grabber button,
    // see Main.qml's onUsbGrabberRequested). Mutually exclusive with the other
    // top-level blocks by construction — setTopLevelScreen() keeps at most one
    // flag true, so no precedence negation is needed here.
    UsbGrabberPage {
        anchors.fill: parent
        visible: mainPanel.usbGrabberOpen
        usbGrabberController: mainPanel.usbGrabberController
        vaultLocalPath: mainPanel.vaultLocalPath
        vaultTreeModel: mainPanel.vaultTreeModel
    }

    // Item 2 (delete): reached ONLY when the trash refused the open file — a
    // successful trash never opens this, it posts a snackbar with Undo (see
    // requestDeleteFile()/requestDeleteTranscript()). So the wording promises no
    // trash: it used to say "Moved to the trash" on every delete, which is
    // exactly wrong here — this is the one case where nothing is. Message +
    // confirm label switch on mainPanel.pendingDeleteMode.
    ConfirmPopup {
        id: deleteFileConfirmPopup
        message: mainPanel.pendingDeleteMode === "transcript"
                 ? qsTr("This transcript can't be moved to the trash. Delete it " +
                        "permanently? The audio file is kept. This can't be undone.")
                 : mainPanel.pendingDeleteMode === "audioOnly"
                 ? qsTr("This audio file can't be moved to the trash. Delete it " +
                        "permanently? The transcript is kept. This can't be undone.")
                 : qsTr("This file can't be moved to the trash. Delete it and its " +
                        "transcript permanently? This can't be undone.")
        confirmText: mainPanel.pendingDeleteMode === "transcript"
                     ? qsTr("Delete transcript")
                     : mainPanel.pendingDeleteMode === "audioOnly"
                     ? qsTr("Delete audio")
                     : qsTr("Delete file")
        onConfirmed: mainPanel.confirmFileDelete()
    }
}
