import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import SuopTranscriber

Rectangle {
    id: mainPanel

    property url vaultPath
    property string vaultLocalPath: ""
    property AudioPlayer player
    property AudioRecorder audioRecorder
    property TranscriptionController transcriptionController
    property VaultTreeModel vaultTreeModel
    property UsbGrabberController usbGrabberController
    // Set true/false only by TitleBar's record toggle (see Main.qml's
    // onRecordRequested) — gates the recorder page below and hides the
    // normal per-file content while it's showing (see the two `visible:`
    // bindings just above the recorder page block).
    property bool recording: false
    // Set true/false only by TitleBar's USB grabber toggle (see Main.qml's
    // onUsbGrabberRequested) — gates the USB grabber page below, mirroring
    // `recording` above. `recording` wins if both happen to be true (see
    // usbGrabberPage's own `visible:` binding).
    property bool usbGrabberOpen: false
    // Set true/false only by TitleBar's benchmark toggle (see Main.qml's
    // onBenchmarkRequested) — gates the benchmark page below, mirroring
    // usbGrabberOpen. recording/usbGrabber win if several are true.
    property bool benchmarkOpen: false
    property var storedMetadata: ({})
    readonly property bool hasStoredMetadata: Object.keys(mainPanel.storedMetadata).length > 0
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
    // runAllJobs pipeline on one file). Mirrors BottomBar's phaseLabel wording
    // but in the spinner's title-case gerund style; falls back to
    // "Transcribing…" for an empty/unknown phase.
    readonly property string transcribingOverlayLabel: {
        const phase = mainPanel.transcriptionController ? mainPanel.transcriptionController.currentPhase : ""
        switch (phase) {
        case "detecting-speakers":
        case "detecting-speakers-segmenting":
        case "detecting-speakers-identifying": return qsTr("Detecting speakers…")
        case "chaptering": return qsTr("Generating chapters…")
        case "combining": return qsTr("Combining…")
        case "importing": return qsTr("Importing…")
        case "loading": return qsTr("Loading model…")
        case "decoding": return qsTr("Decoding audio…")
        case "detecting-speech": return qsTr("Detecting speech…")
        case "downloading": return qsTr("Downloading…")
        case "transcribing": return qsTr("Transcribing…")
        default: return qsTr("Transcribing…")
        }
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
    // the transcriptScrollView.contentItem Connections below) — NOT a
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
        if (c.busy && c.currentFile.toString() !== "")
            urls.push(c.currentFile.toString())
        return urls
    }

    signal openVaultRequested()
    // Item 16 (Wave E): a tag pill was clicked — Main.qml routes this to
    // sidebar.applyTagFilter(tag).
    signal tagActivated(string tag)
    // A speaker pill was clicked — Main.qml routes this to
    // sidebar.applySpeakerFilter(name), mirroring tagActivated above.
    signal speakerActivated(string name)

    // Text-only view for an orphaned .md (no matching audio) — mutually
    // exclusive with player.source; showAudioFile()/showOrphanedMd() below
    // are the only places that should touch either.
    property string orphanedMdPath: ""
    property var orphanedMdMetadata: ({})

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
    // storedMetadata/frontmatter (see TranscriptionController::readPrelude()),
    // so it needs its own property, populated alongside storedMetadata in
    // refreshMetadata() below.
    property string prelude: ""

    // Feature 3b/3c: chapter markers (see
    // TranscriptionController::readChapters()) — shared by the seek-bar tick
    // marks (3b) and the chapter list (3c), populated once alongside
    // prelude/storedMetadata in refreshMetadata() below rather than each
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
        storedMetadata = (player && player.source.toString() !== "" && transcriptionController)
            ? transcriptionController.readMetadata(player.source) : ({})
        // titleField is always-editable (no display/edit swap — see its doc
        // comment), so its `text: displayTitle()` binding is a one-shot: it's
        // severed the first time the user types. Resync explicitly on every
        // metadata refresh, skipped while the user is actively editing so
        // this never fights their in-progress typing.
        if (!titleField.activeFocus)
            titleField.text = mainPanel.displayTitle()
        // The filename subtitle is the same always-editable one-shot binding
        // (text: currentFileStem()) — resync it here too, skipping while the
        // user is typing into it, so a rename/file-switch refreshes it.
        if (!filenameSubtitle.activeFocus)
            filenameSubtitle.text = mainPanel.currentFileStem()
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
    // storedMetadata above, which is a cheap frontmatter-only read either
    // way) — see beginTranscriptLoad()'s doc comment for why that path
    // defers to the async completion instead.
    function refreshPreludeAndChapters() {
        mainPanel.prelude = (player && player.source.toString() !== "" && transcriptionController)
            ? transcriptionController.readPrelude(player.source) : ""
        // Chapters, unlike prelude, are also readable for an orphaned .md —
        // TranscriptionController::readChapters() only ever reads the .md's
        // existing body, never audio, same reasoning as generateChaptersButton
        // above.
        const chaptersUrl = mainPanel.orphanedMdPath !== "" ? ("file://" + mainPanel.orphanedMdPath)
            : (player ? player.source.toString() : "")
        mainPanel.chapters = (chaptersUrl !== "" && transcriptionController)
            ? transcriptionController.readChapters(chaptersUrl) : []
        // preludeArea is always-editable (see its doc comment) — same
        // one-shot-binding caveat as titleField, resynced here.
        if (!preludeArea.activeFocus)
            preludeArea.text = mainPanel.prelude
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
        mainPanel.pushUndo({type: "chapterEdit", op: "add", fileUrl: url,
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
        mainPanel.pushUndo({type: "chapterEdit", op: "rename", fileUrl: url,
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
        mainPanel.pushUndo({type: "chapterEdit", op: "delete", fileUrl: url,
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

    // The .md url backing the currently-shown chapters (player file or the
    // orphaned .md), or "" if none — chapters/collapse are readable/persistable
    // for both.
    function chaptersFileUrl() {
        if (mainPanel.orphanedMdPath !== "")
            return "file://" + mainPanel.orphanedMdPath
        return (mainPanel.player && mainPanel.player.source.toString() !== "")
            ? mainPanel.player.source.toString() : ""
    }

    // Item: persist which chapters are collapsed into the .md frontmatter, so
    // it survives reopening the file. Writes the collapse map's live keys via
    // setCollapsedChapters (which does NOT emit finished(), so this can't
    // trigger a reload cycle).
    function persistCollapsedChapters() {
        if (!mainPanel.transcriptionController)
            return
        const url = mainPanel.chaptersFileUrl()
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
        const meta = mainPanel.orphanedMdPath !== "" ? mainPanel.orphanedMdMetadata : mainPanel.storedMetadata
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

    function refreshOrphanedMetadata() {
        orphanedMdMetadata = (mainPanel.orphanedMdPath !== "" && mainPanel.transcriptionController)
            ? mainPanel.transcriptionController.readMetadata("file://" + mainPanel.orphanedMdPath) : ({})
        if (!titleField.activeFocus)
            titleField.text = mainPanel.displayTitle()
    }

    function showAudioFile(path) {
        mainPanel.orphanedMdPath = ""
        mainPanel.player.source = "file://" + path
    }

    function showOrphanedMd(path) {
        mainPanel.player.source = ""
        mainPanel.orphanedMdPath = path
        mainPanel.refreshOrphanedMetadata()
    }

    // Item 13: tags are comma-separated (see TranscriptionController's
    // frontmatter format — not a YAML list), so split+trim+drop-empties here.
    function currentTags() {
        const metadata = mainPanel.orphanedMdPath !== "" ? mainPanel.orphanedMdMetadata : mainPanel.storedMetadata
        const raw = metadata && metadata.tags ? String(metadata.tags) : ""
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
    // storedMetadata/orphanedMdMetadata reference churn alone doesn't
    // reliably re-trigger it) — a successful speaker rename is exactly that
    // case, since it rewrites the transcript body sentences reads from.
    property int sentencesRevision: 0

    // A rejected speaker rename OR a failed merge (see commitSpeakerRename()/
    // onSpeakersMerged() below) and commitTranscriptEdits()'s rejection paths
    // all post to the Notifications singleton now (StyleGuide.md "Snackbars")
    // rather than each carrying its own auto-clearing inline label — they're
    // transient reports about an action that just failed, not descriptions of
    // a control's current state, so they belong in the one floating queue.

    // The file a speaker rename/merge should be applied to — the orphaned-md
    // view and the normal audio-player view are mutually exclusive (see
    // showAudioFile()/showOrphanedMd()), so exactly one of these is ever
    // live at a time. Player-less/no-file case included so callers can
    // safely .toString()-compare against a signal's fileUrl without a null
    // check of their own.
    function currentFileUrl() {
        return mainPanel.orphanedMdPath !== "" ? ("file://" + mainPanel.orphanedMdPath)
                                                : (mainPanel.player ? mainPanel.player.source : "")
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

    function requestDeleteTranscript() {
        if (!mainPanel.vaultTreeModel)
            return
        const url = mainPanel.currentFileUrl()
        if (String(url) === "")
            return
        // A 1-element list: these calls are batch-shaped (one rebuild per user
        // action — see trashFiles()' doc comment), and MainPanel's cluster is
        // by definition the single-open-file half of the CLAUDE.md action
        // parity, so its batch is always exactly one.
        mainPanel.handleTrashResult(
            mainPanel.vaultTreeModel.trashTranscript([url]), "transcript",
            qsTr("Moved the transcript for %1 to the trash.").arg(mainPanel.currentFileName()))
    }

    // The complement of requestDeleteTranscript(): drops the audio, keeps the
    // .md. fileDeleted (not transcriptDeleted) closes the open file — see
    // trashAudioOnly()'s comment for why the surviving .md is a different row.
    function requestDeleteAudioOnly() {
        if (!mainPanel.vaultTreeModel)
            return
        const url = mainPanel.currentFileUrl()
        if (String(url) === "")
            return
        mainPanel.handleTrashResult(
            mainPanel.vaultTreeModel.trashAudioOnly([url]), "audioOnly",
            qsTr("Moved the audio for %1 to the trash — the transcript is still here.")
                .arg(mainPanel.currentFileName()))
    }

    function requestDeleteFile() {
        if (!mainPanel.vaultTreeModel)
            return
        const url = mainPanel.currentFileUrl()
        if (String(url) === "")
            return
        mainPanel.handleTrashResult(
            mainPanel.vaultTreeModel.trashFiles([url]), "file",
            qsTr("Moved %1 to the trash.").arg(mainPanel.currentFileName()))
    }

    // Reached ONLY when the trash refused — this really is the irreversible one.
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
        mainPanel.pushUndo({type: "speakerRename", fileUrl: mainPanel.currentFileUrl(),
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

    // A speaker's default, never-renamed display name is always exactly
    // "Speaker N" (see DiarizationEngine/labelForSegment) — used to decide
    // which side of a merge "wins" the resulting name below.
    function isDefaultSpeakerName(name) {
        return /^Speaker \d+$/.test(name)
    }

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
        const sourceNamed = !mainPanel.isDefaultSpeakerName(mainPanel.effectiveSpeakerName(sourceId))
        const targetNamed = !mainPanel.isDefaultSpeakerName(mainPanel.effectiveSpeakerName(targetId))
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
    // rename) — a plain JS stack of {type, ...} entries, each recording
    // BOTH its pre-edit (oldValue) and post-edit (newValue) state so the
    // SAME entry object can be replayed in either direction: undoLast()
    // applies the "undo" side and moves it to redoStack; redoLast() applies
    // the "redo" side and moves it back. Entries are pushed BEFORE the
    // mutation they guard. Both stacks reset whenever the open file changes
    // (see onSourceChanged/onOrphanedMdPathChanged below) so Ctrl+Z/Ctrl+Y
    // never reach across a file switch into a different note's history.
    // Speaker MERGE is deliberately NOT tracked here — unlike a rename, a
    // merge collapses two distinct speaker identities into one (which
    // sentences originally belonged to the losing speaker is no longer
    // recoverable from the rewritten transcript alone), so there is no
    // simple reverse operation to record.
    property var undoStack: []
    property var redoStack: []

    // Reassigns (rather than push()/pop() in place) so undoStackChanged()
    // actually fires — a plain JS array mutated in place doesn't notify any
    // binding that reads mainPanel.undoStack.length (e.g. the Ctrl+Z
    // Shortcut's `enabled` below), since QML's dependency tracking only
    // hooks the property's own notify signal, not the array contents.
    function pushUndo(entry) {
        mainPanel.undoStack = mainPanel.undoStack.concat([entry])
        // A fresh user edit invalidates any pending redo history — standard
        // undo/redo semantics: once the user diverges from the undone
        // branch, that branch is no longer reachable. (undoLast()/redoLast()
        // push to the stacks directly, NOT through here, so replaying
        // history never triggers this clear.)
        mainPanel.redoStack = []
    }

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
        if (mainPanel.orphanedMdPath !== "") {
            mainPanel.refreshOrphanedMetadata()
        } else {
            mainPanel.refreshMetadata()
            mainPanel.refreshPreludeAndChapters()
        }
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
        if (mainPanel.undoStack.length === 0 || !mainPanel.transcriptionController)
            return
        const entry = mainPanel.undoStack[mainPanel.undoStack.length - 1]
        mainPanel.undoStack = mainPanel.undoStack.slice(0, -1)
        mainPanel.applyHistoryEntry(entry, "undo")
        mainPanel.redoStack = mainPanel.redoStack.concat([entry])
    }

    function redoLast() {
        if (mainPanel.redoStack.length === 0 || !mainPanel.transcriptionController)
            return
        const entry = mainPanel.redoStack[mainPanel.redoStack.length - 1]
        mainPanel.redoStack = mainPanel.redoStack.slice(0, -1)
        mainPanel.applyHistoryEntry(entry, "redo")
        mainPanel.undoStack = mainPanel.undoStack.concat([entry])
    }

    // Item 51: clears the selection in the panel's main selectable text
    // bodies (transcript + prelude). deselect() is a no-op when there is no
    // selection, so this is always safe — it never disturbs cursor position
    // or focus. Called by the root deselect TapHandler below when a click
    // lands on a non-interactive region.
    function deselectTextSelections() {
        transcriptArea.deselect()
        preludeArea.deselect()
        // Item 34: clicking a non-interactive region also commits+exits any
        // in-progress chapter-title edit — the active title's onEditingChanged
        // handler forces its own blur (→ commit) when this clears.
        mainPanel.editingChapterTs = -1
    }

    function displayTitle() {
        if (mainPanel.orphanedMdPath !== "") {
            const label = mainPanel.orphanedMdMetadata.label
            if (AppSettings.useLabelsMainPanel && label)
                return label
            const base = mainPanel.orphanedMdPath.substring(mainPanel.orphanedMdPath.lastIndexOf("/") + 1)
            return AppSettings.hideExtensionsMainPanel ? mainPanel.stripExtension(base) : base
        }
        if (!mainPanel.player || mainPanel.player.source.toString() === "")
            return qsTr("No file selected")
        if (AppSettings.useLabelsMainPanel && mainPanel.storedMetadata.label)
            return mainPanel.storedMetadata.label
        const rel = mainPanel.relativeToVault(mainPanel.player.source)
        return AppSettings.hideExtensionsMainPanel ? mainPanel.stripExtension(rel) : rel
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

        if (mainPanel.orphanedMdPath !== "") {
            if (AppSettings.useLabelsMainPanel) {
                const oldLabel = mainPanel.orphanedMdMetadata.label || ""
                if (trimmed !== oldLabel)
                    mainPanel.pushUndo({type: "label", fileUrl: "file://" + mainPanel.orphanedMdPath, oldValue: oldLabel, newValue: trimmed})
                mainPanel.transcriptionController.setLabel("file://" + mainPanel.orphanedMdPath, trimmed)
                mainPanel.refreshOrphanedMetadata()
            } else if (mainPanel.vaultTreeModel
                       && mainPanel.vaultTreeModel.renameFile("file://" + mainPanel.orphanedMdPath, trimmed)) {
                const dir = mainPanel.orphanedMdPath.substring(0, mainPanel.orphanedMdPath.lastIndexOf("/"))
                mainPanel.orphanedMdPath = dir + "/" + trimmed + ".md"
                mainPanel.refreshOrphanedMetadata()
            }
            return
        }

        if (mainPanel.player && mainPanel.player.source.toString() !== "") {
            if (AppSettings.useLabelsMainPanel) {
                const oldLabel = mainPanel.storedMetadata.label || ""
                if (trimmed !== oldLabel)
                    mainPanel.pushUndo({type: "label", fileUrl: mainPanel.player.source, oldValue: oldLabel, newValue: trimmed})
                mainPanel.transcriptionController.setLabel(mainPanel.player.source, trimmed)
                mainPanel.refreshMetadata()
                mainPanel.refreshPreludeAndChapters()
            } else if (mainPanel.vaultTreeModel) {
                const oldPath = mainPanel.player.source.toString().replace("file://", "")
                if (mainPanel.vaultTreeModel.renameFile(mainPanel.player.source, trimmed)) {
                    const dir = oldPath.substring(0, oldPath.lastIndexOf("/"))
                    const dot = oldPath.lastIndexOf(".")
                    const ext = dot > 0 ? oldPath.substring(dot) : ""
                    mainPanel.player.source = "file://" + dir + "/" + trimmed + ext
                }
            }
        }
    }

    // The base filename stem (no directory, no extension) of the open file —
    // what renameFile() takes as its new-name argument and what the editable
    // filename subtitle shows/edits.
    function currentFileStem() {
        if (!mainPanel.player || mainPanel.player.source.toString() === "")
            return ""
        const rel = mainPanel.relativeToVault(mainPanel.player.source)
        const base = rel.substring(rel.lastIndexOf("/") + 1)
        return mainPanel.stripExtension(base)
    }

    // Item: the filename subtitle (shown when the title is an AI label) is an
    // editable rename control for the REAL file. renameFile() renames the
    // audio file AND its sibling .md together (so this "also edits the md
    // file"), then we repoint player.source, which fires onSourceChanged →
    // refreshMetadata → resyncs both the title and this subtitle. The subtitle
    // is disabled while busy (see its `enabled`), so a rename can't race a
    // running job; on any no-op/failure we revert the field to the real stem.
    // No undo entry — file renames aren't on the undo stack (matches
    // commitRename's file-rename branch).
    function commitFilenameRename(newValue) {
        const trimmed = newValue.trim()
        const oldStem = mainPanel.currentFileStem()
        if (!mainPanel.transcriptionController || !mainPanel.vaultTreeModel
                || !mainPanel.player || mainPanel.player.source.toString() === ""
                || trimmed === "" || trimmed === oldStem) {
            filenameSubtitle.text = oldStem
            return
        }
        const oldPath = mainPanel.player.source.toString().replace("file://", "")
        if (mainPanel.vaultTreeModel.renameFile(mainPanel.player.source, trimmed)) {
            const dir = oldPath.substring(0, oldPath.lastIndexOf("/"))
            const dot = oldPath.lastIndexOf(".")
            const ext = dot > 0 ? oldPath.substring(dot) : ""
            mainPanel.player.source = "file://" + dir + "/" + trimmed + ext
        } else {
            filenameSubtitle.text = oldStem
        }
    }

    // Autosaves the prelude editor below on blur (see preludeArea's
    // onActiveFocusChanged) — no explicit Save/Cancel; plain Enter just
    // inserts a newline since the prelude is multi-line, same as
    // transcriptArea's own blur-commit below. Orphaned-md files never show
    // the prelude editor (see its visible: condition below), so unlike
    // commitRename() this only needs the player.source path. setLabel()'s
    // failures are already silently ignored by commitRename() above;
    // matched here for consistency.
    function commitPrelude(newValue) {
        if (!mainPanel.transcriptionController || !mainPanel.player || mainPanel.player.source.toString() === "") {
            preludeArea.text = mainPanel.prelude
            return
        }
        if (newValue === mainPanel.prelude)
            return
        mainPanel.pushUndo({type: "prelude", fileUrl: mainPanel.player.source, oldValue: mainPanel.prelude, newValue: newValue})
        mainPanel.transcriptionController.setPrelude(mainPanel.player.source, newValue)
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
        if (!mainPanel.transcriptionController || !mainPanel.player || mainPanel.player.source.toString() === "")
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
            if (entry.isChapterHeading)
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

        mainPanel.pushUndo({type: "transcriptText", fileUrl: mainPanel.player.source, oldValue: oldValues, newValue: edits})
        const ok = mainPanel.transcriptionController.setSentenceTexts(mainPanel.player.source, edits)
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
        mainPanel.pushUndo({type: "tags", fileUrl: mainPanel.currentFileUrl(), oldValue: current, newValue: current.concat([trimmed])})
        mainPanel.transcriptionController.setTags(mainPanel.currentFileUrl(), current.concat([trimmed]))
    }

    function removeTag(tag) {
        if (!mainPanel.transcriptionController)
            return
        const current = mainPanel.currentTags()
        mainPanel.pushUndo({type: "tags", fileUrl: mainPanel.currentFileUrl(), oldValue: current, newValue: current.filter(t => t !== tag)})
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
        mainPanel.pushUndo({type: "tags", fileUrl: mainPanel.currentFileUrl(), oldValue: current, newValue: updated})
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
    //      on sentencesRevision bumps, and on hasStoredMetadata flipping
    //      true (frontmatter now exists — matches the moment the original
    //      binding's dependency on hasStoredMetadata would have started
    //      returning real content instead of ""/[]). All three are either
    //      small edits to an already-open file or an early/small live
    //      transcript — cheap synchronous calls, exactly as before.
    property string transcriptBody: ""
    property var sentences: []
    // Which fileUrl (string form) the most recent beginTranscriptLoad() is
    // waiting on — "" when nothing is in flight (untranscribed file, no
    // transcriptionController, etc.).
    property string transcriptLoadingFor: ""

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
        if (!preludeArea.activeFocus)
            preludeArea.text = ""
        if (!mainPanel.transcriptionController)
            return
        if (mainPanel.orphanedMdPath !== "") {
            const key = "file://" + mainPanel.orphanedMdPath
            mainPanel.transcriptLoadingFor = key
            mainPanel.transcriptionController.loadTranscriptAsync(key)
        } else if (mainPanel.player && mainPanel.player.source.toString() !== "" && mainPanel.hasStoredMetadata) {
            const key = mainPanel.player.source.toString()
            mainPanel.transcriptLoadingFor = key
            mainPanel.transcriptionController.loadTranscriptAsync(key)
        }
        // else: no file, or an untranscribed file with nothing to load yet
        // — stays cleared until hasStoredMetadata flips true (see
        // onHasStoredMetadataChanged below).
    }

    function refreshTranscriptSync() {
        if (!mainPanel.transcriptionController) {
            mainPanel.transcriptBody = ""
            mainPanel.sentences = []
            return
        }
        let key = ""
        if (mainPanel.orphanedMdPath !== "")
            key = "file://" + mainPanel.orphanedMdPath
        else if (mainPanel.player && mainPanel.player.source.toString() !== "" && mainPanel.hasStoredMetadata)
            key = mainPanel.player.source.toString()
        if (key === "") {
            mainPanel.transcriptBody = ""
            mainPanel.sentences = []
            return
        }
        mainPanel.transcriptBody = mainPanel.transcriptionController.readTranscriptBody(key)
        mainPanel.sentences = mainPanel.transcriptionController.parseSentences(key)
    }

    onSentencesRevisionChanged: mainPanel.refreshTranscriptSync()
    onHasStoredMetadataChanged: mainPanel.refreshTranscriptSync()

    Connections {
        target: mainPanel.transcriptionController
        function onTranscriptLoaded(fileUrl, body, loadedSentences) {
            if (fileUrl.toString() !== mainPanel.transcriptLoadingFor)
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
                // at job START — so hasStoredMetadata is guaranteed readable
                // by now. onProgressChanged (the other metadata-refresh site)
                // can fire during model-load, BEFORE the frontmatter flush,
                // so on a file's very FIRST transcription its regenerate
                // label/prelude/chapters buttons (gated on hasStoredMetadata)
                // could stay hidden until the next progress tick or a full
                // reopen. Refreshing metadata here — on the definitely-
                // post-frontmatter content edge — closes that hole and also
                // guarantees refreshTranscriptSync() below sees hasStoredMetadata
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
    readonly property var displayData: {
        let text = ""
        const list = []
        let previousSpeaker = null
        const chapterList = mainPanel.chapters
        const collapsed = mainPanel.collapsedChapters
        let chapterIdx = 0
        // Real t0 of the chapter the CURRENT sentence falls under (-1 = before
        // the first chapter marker / no chapters). Item 33 keys collapse off
        // this; the displayT0 the heading itself renders at is separately
        // clamped (see below), so the two are tracked independently.
        let currentChapterT0 = -1

        // displayT0 is the (possibly clamped) time the heading is positioned/
        // highlighted at; chapter.t0 is the chapter's REAL timestamp, carried
        // through as chapterT0 so the heading's edit/collapse/delete controls
        // address the actual chapter marker (setChapterTitle/deleteChapter/
        // collapse all key off the real t0, never the clamped one).
        function pushHeading(chapter, displayT0) {
            const headingStart = text.length
            list.push({idx: list.length, t0: displayT0, chapterT0: chapter.t0, speaker: "", text: "", start: headingStart, length: 0,
                       showGutter: false, isChapterHeading: true, chapterTitle: chapter.title,
                       chapterManual: !!chapter.manual})
            text += "\n"
            previousSpeaker = null // next real sentence's gutter shows fresh after a section break
        }

        for (const s of mainPanel.sentences) {
            // Skip blank/whitespace-only segments entirely — whisper can
            // occasionally emit one (e.g. a near-silent stretch), and giving
            // it its own empty line + gutter timestamp is exactly the "one
            // timestamp with nothing under it" pattern that reads as a
            // gutter glitch. A blank segment has no content to show, so it
            // shouldn't get a line OR a timestamp.
            if (s.text.trim() === "")
                continue
            while (chapterIdx < chapterList.length && chapterList[chapterIdx].t0 <= s.t0) {
                currentChapterT0 = chapterList[chapterIdx].t0
                pushHeading(chapterList[chapterIdx], Math.min(chapterList[chapterIdx].t0, s.t0))
                chapterIdx++
            }
            // Item 33: a collapsed chapter hides its body entirely — the
            // sentence is dropped from BOTH `text` and `list` (its heading was
            // already pushed above and stays visible), keeping every
            // downstream offset consumer (transcriptArea's string, the overlay
            // Repeaters, searchCorpus()) consistent.
            if (currentChapterT0 !== -1 && collapsed[String(currentChapterT0)])
                continue
            const start = text.length
            const speaker = s.speaker === "" ? "" : mainPanel.effectiveSpeakerName(s.speaker)
            const showGutter = (speaker === "") || (previousSpeaker === null) || (speaker !== previousSpeaker)
            // repeatCount > 1 (SegmentLineWriter merged consecutive identical
            // whisper segments into one on-disk line, see its doc comment) —
            // append a "(Nx)" marker so that's still visible here, not just
            // in the raw .md file. Appended AFTER length is captured from
            // s.text alone, so the marker itself sits outside the
            // highlightable/selectable "spoken words" range (same treatment
            // as gutter timestamps/speaker names — decoration, not content).
            const displayText = s.repeatCount > 1 ? s.text + " (" + s.repeatCount + "x)" : s.text
            list.push({idx: list.length, t0: s.t0, speaker: speaker, text: displayText, start: start, length: s.text.length,
                       showGutter: showGutter, isChapterHeading: false, chapterTitle: "",
                       chapterT0: currentChapterT0, chapterManual: false})
            previousSpeaker = speaker
            text += displayText + "\n"
        }
        // Any chapter at/after the last real sentence (e.g. a trailing
        // marker with no further speech) still gets a heading rather than
        // being silently dropped.
        while (chapterIdx < chapterList.length) {
            pushHeading(chapterList[chapterIdx], chapterList[chapterIdx].t0)
            chapterIdx++
        }
        if (text.length > 0)
            text = text.slice(0, -1) // drop the final trailing newline
        return {text: text, sentences: list}
    }
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
    // timestamp gutter takes every non-heading sentence; the speaker gutter
    // keeps the showGutter (turn-boundary) gate.
    readonly property var gutterTimestampModel:
        mainPanel.displaySentences.filter(s => !s.isChapterHeading)
    readonly property var gutterSpeakerModel:
        mainPanel.displaySentences.filter(s => s.showGutter && s.speaker !== "")
    // Recomputes when the user toggles a speaker filter (empty model while no
    // filter is active, which is the state every file open starts in).
    readonly property var speakerDimModel:
        mainPanel.gutterSpeakerFilter === "" ? []
            : mainPanel.displaySentences.filter(
                s => !s.isChapterHeading && s.speaker !== mainPanel.gutterSpeakerFilter)
    readonly property var chapterHeadingModel:
        mainPanel.displaySentences.filter(s => s.isChapterHeading)
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

    // Compact gutter timestamp: only the start time (no range), leading
    // zero-valued units (hours, then minutes) dropped entirely rather than
    // shown as "0:"/"00:", and fractional-second precision governed by
    // AppSettings.timestampDecimalPlaces (0 by default — display-only, the
    // stored data always keeps full precision, see formatTimestamp() in
    // TranscriptionController.cpp).
    function formatGutterTimestamp(t0) {
        const decimals = AppSettings.timestampDecimalPlaces
        const scale = Math.pow(10, decimals)
        const totalUnits = Math.round(t0 * scale)
        const wholeSeconds = Math.floor(totalUnits / scale)
        const frac = totalUnits % scale
        const fracPart = decimals > 0 ? "." + String(frac).padStart(decimals, "0") : ""
        const h = Math.floor(wholeSeconds / 3600)
        const m = Math.floor((wholeSeconds % 3600) / 60)
        const s = wholeSeconds % 60
        if (h > 0)
            return h + ":" + String(m).padStart(2, "0") + ":" + String(s).padStart(2, "0") + fracPart
        if (m > 0)
            return m + ":" + String(s).padStart(2, "0") + fracPart
        return s + fracPart
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
        const positionSeconds = (mainPanel.player ? mainPanel.player.position : 0) / 1000.0
        return mainPanel.transcriptionController.activeSentenceIndex(mainPanel.displaySentences, positionSeconds)
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
    // into transcriptScrollView's Flickable frame (same contentItem used by
    // the live-transcription auto-scroll in onTranscriptBodyChanged, and by
    // middleDragScrollArea) gives its on-screen position relative to the
    // current viewport, which is then minimally clamped into view — no
    // centering, just enough scroll to reveal it, same as a browser's
    // "scroll into view".
    function scrollMatchIntoView() {
        const rect = mainPanel.searchMatchRect
        if (rect.width === 0 && rect.height === 0)
            return
        const flick = transcriptScrollView.contentItem
        const viewportPos = transcriptArea.mapToItem(flick, rect.x, rect.y)
        const maxContentY = Math.max(0, flick.contentHeight - flick.height)
        if (viewportPos.y < 0)
            flick.contentY = Math.max(0, flick.contentY + viewportPos.y)
        else if (viewportPos.y + rect.height > flick.height)
            flick.contentY = Math.min(maxContentY, flick.contentY + (viewportPos.y + rect.height - flick.height))
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

    Connections {
        target: mainPanel.player
        // Item P8: restart the repaint nudge on EVERY file open, not just
        // the empty->populated transition (see transcriptRepaintNudgeTimer's
        // own doc comment below) — switching directly between two
        // already-transcribed files never passes through an empty state, so
        // onTranscriptBodyChanged's own trigger alone would miss it.
        // Item P16: also reset per-file undo history so Ctrl+Z can never
        // reach across a file switch into a different note's edits.
        function onSourceChanged() {
            mainPanel.refreshMetadata()
            // Item P23: NOT refreshPreludeAndChapters() here — this is the
            // cold-open case, see beginTranscriptLoad()'s doc comment.
            mainPanel.beginTranscriptLoad()
            transcriptRepaintNudgeTimer.restart()
            mainPanel.undoStack = []
            mainPanel.redoStack = []
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
    }
    // Item P8/P16: showOrphanedMd() switches files by setting orphanedMdPath
    // directly (player.source is cleared to "" first, which fires
    // onSourceChanged above for the "no file" state, not the orphaned file
    // itself) — this is the other half of "a different file was just
    // opened", covering the orphaned-md view.
    onOrphanedMdPathChanged: {
        mainPanel.beginTranscriptLoad()
        transcriptRepaintNudgeTimer.restart()
        mainPanel.undoStack = []
        mainPanel.redoStack = []
        mainPanel.gutterSpeakerFilter = ""
        mainPanel.collapsedChapters = ({})
        mainPanel.editingChapterTs = -1
        mainPanel.closedByDeleteUrl = "" // see onSourceChanged's identical line
        mainPanel.followingTail = false // see onSourceChanged's identical line
    }
    Connections {
        target: mainPanel.transcriptionController
        function onFinished(fileUrl, ok, error) {
            mainPanel.refreshMetadata()
            mainPanel.refreshPreludeAndChapters()
            mainPanel.refreshOrphanedMetadata()
            // None of the three refreshes above touch the transcript BODY
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
            if (ok && mainPanel.player
                    && fileUrl.toString() === mainPanel.player.source.toString())
                mainPanel.refreshTranscriptSync()
        }
        // Item 10: frontmatter is written at job START (durability, see
        // TranscriptionController), before any segment lines exist — so
        // hasStoredMetadata can flip true well before onFinished fires.
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
        // transcriptScrollView.contentItem Connections below, which reacts to
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
        target: transcriptScrollView.contentItem
        function onContentHeightChanged() {
            const flick = transcriptScrollView.contentItem
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
        enabled: mainPanel.activeFocus && (mainPanel.orphanedMdPath !== ""
                     || (mainPanel.player && mainPanel.player.source.toString() !== ""))
        onActivated: {
            mainPanel.searchExpanded = true
            searchField.forceActiveFocus()
        }
    }

    // Item P16: undoes the last label/prelude/tag/transcript/speaker-rename
    // edit made through this panel — see undoStack's own doc comment above.
    Shortcut {
        sequence: "Ctrl+Z"
        enabled: mainPanel.activeFocus && mainPanel.undoStack.length > 0
        onActivated: mainPanel.undoLast()
    }

    // Item 46 / Item 20: redo — re-applies the most recently undone edit.
    // Bound to BOTH conventional redo chords (Ctrl+Y and Ctrl+Shift+Z), each
    // as its own Shortcut so both work regardless of platform habit. Gated
    // the same way Ctrl+Z is (panel focus + a non-empty redoStack).
    Shortcut {
        sequences: ["Ctrl+Y", "Ctrl+Shift+Z"]
        enabled: mainPanel.activeFocus && mainPanel.redoStack.length > 0
        onActivated: mainPanel.redoLast()
    }

    // Item 7: Ctrl+H opens the SAME bar Ctrl+F does — there is only one now —
    // but arms the replace row on the way in, so the old "jump straight to
    // replace" muscle memory still lands somewhere useful. Same enable-gate
    // as Ctrl+F. Not a toggle: it's "get me to replace", so pressing it twice
    // shouldn't shut the bar you just opened.
    Shortcut {
        sequence: "Ctrl+H"
        enabled: mainPanel.activeFocus && (mainPanel.orphanedMdPath !== ""
                     || (mainPanel.player && mainPanel.player.source.toString() !== ""))
        onActivated: {
            mainPanel.searchExpanded = true
            mainPanel.replaceEnabled = true
            searchField.forceActiveFocus()
        }
    }

    // Item 51: a plain left-click landing on a non-interactive region (the
    // panel's empty background, a gap between rows) clears any active text
    // selection. Attached to the root so it only fires when nothing deeper
    // under the pointer consumes the press — a deeper handler always wins
    // Qt Quick's pointer hit-testing, so clicking INTO a field (its own
    // press grab), a timestamp/seek TapHandler, a button, or middle-drag
    // panning (middleDragScrollArea's MiddleButton TapHandler) all still
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
        visible: mainPanel.vaultPath.toString() === "" && !mainPanel.recording && !mainPanel.usbGrabberOpen
                 && !mainPanel.benchmarkOpen

        Button {
            id: openVaultButton
            anchors.centerIn: parent
            text: qsTr("Open Vault")
            hoverEnabled: true
            onClicked: mainPanel.openVaultRequested()

            background: Rectangle {
                radius: AppSettings.borderRadius
                color: (openVaultButton.hovered && openVaultButton.enabled) ? "#3a3a3a" : "#2a2a2a"
                border.width: 1
                border.color: "#3a3a3a"
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

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 16
        spacing: 12
        visible: mainPanel.vaultPath.toString() !== "" && !mainPanel.recording && !mainPanel.usbGrabberOpen
                 && !mainPanel.benchmarkOpen

        // Item 13: playback progress bar relocated to the very top, full
        // width, with a play/pause icon instead of a separate Play button.
        // Hidden entirely for the orphaned-md text-only view (nothing to
        // play).
        RowLayout {
            Layout.fillWidth: true
            spacing: 12
            visible: mainPanel.orphanedMdPath === ""

            Item {
                id: playPauseButton
                Layout.preferredWidth: 28
                Layout.preferredHeight: 28
                enabled: mainPanel.player && mainPanel.player.source.toString() !== ""

                HoverHandler {
                    id: playPauseHover
                    cursorShape: Qt.PointingHandCursor
                    enabled: playPauseButton.enabled
                }
                TapHandler {
                    enabled: playPauseButton.enabled
                    onTapped: mainPanel.player.togglePlay()
                }
                // Distinct button background (not just the bare panel) —
                // fixed dark shades so it reads as a button regardless of
                // the user's configurable panel background color.
                Rectangle {
                    anchors.fill: parent
                    radius: AppSettings.borderRadius
                    color: (playPauseHover.hovered && playPauseButton.enabled) ? "#3a3a3a" : "#2a2a2a"
                }
                HoverBorder {
                    anchors.fill: parent
                    active: playPauseButton.enabled && playPauseHover.hovered
                }
                // The play/pause icon. Under the classic pack these resolve
                // to the hand-drawn PlayGlyph/PauseGlyph shapes (a font "▶"
                // can't be reliably centered on its triangle — see
                // PlayGlyph.qml); under material, to the play_arrow/pause SVGs.
                AppIcon {
                    id: playPauseIcon
                    anchors.centerIn: parent
                    name: (mainPanel.player && mainPanel.player.playing) ? "pause" : "play"
                    iconSize: 14
                    color: playPauseButton.enabled ? "white" : "#666666"
                }
            }

            // Playback position/total duration — own display on the
            // playback bar itself, deliberately separate from BottomBar's
            // task progress text (which only ever shows task elapsed/ETA,
            // never anything playback-related).
            Label {
                text: mainPanel.player
                    ? (mainPanel.formatDuration(mainPanel.player.position) + " / " + mainPanel.formatDuration(mainPanel.player.duration))
                    : "0:00 / 0:00"
                color: "#aaaaaa"
                font.pixelSize: 11
            }

            // Item 10: the hover highlight rings only the handle/puck, not
            // the whole track — custom `handle:` so we own that geometry
            // instead of the default QQC2 style's whole-control HoverBorder.
            Slider {
                id: seekSlider
                Layout.fillWidth: true
                hoverEnabled: true
                from: 0
                to: mainPanel.player ? Math.max(mainPanel.player.duration, 1) : 1
                value: mainPanel.player ? mainPanel.player.position : 0
                onMoved: mainPanel.player.position = value

                // Item 5 (Wave E): undo Wave D item 4's border-on-hover puck
                // (reported as not clear enough) — back to a separate
                // expanding ring around the puck (Wave C item 5's tightened
                // +2px/1px-gap version), which reads more clearly as a hover
                // state at a glance.
                handle: Item {
                    x: seekSlider.leftPadding + seekSlider.visualPosition * (seekSlider.availableWidth - width)
                    y: seekSlider.topPadding + seekSlider.availableHeight / 2 - height / 2
                    implicitWidth: 16
                    implicitHeight: 16

                    Rectangle {
                        anchors.centerIn: parent
                        width: parent.width + 2
                        height: parent.height + 2
                        radius: width / 2
                        color: "transparent"
                        border.width: 1
                        border.color: AppSettings.accentColor
                        opacity: seekSlider.hovered ? 1 : 0
                        Behavior on opacity {
                            NumberAnimation { duration: AppSettings.animationsEnabled ? 100 : 0 }
                        }
                    }
                    Rectangle {
                        anchors.fill: parent
                        radius: width / 2
                        color: seekSlider.pressed ? "#dddddd" : "#ffffff"
                        border.width: 1
                        border.color: "#888888"
                    }
                }

                // Feature 3b: one tick mark per chapter (see
                // mainPanel.chapters/readChapters()), positioned with the
                // EXACT SAME leftPadding/availableWidth math as the handle
                // above (:1071) — visualPosition there is
                // (value - from) / (to - from) with value/to in player
                // position milliseconds, so a chapter's t0 (seconds) is
                // scaled to milliseconds here to land in the same space.
                // Gated on a real duration so a not-yet-loaded player (to
                // falls back to 1, see seekSlider.to above) never produces
                // broken 0/0-style math.
                Repeater {
                    model: (mainPanel.player && mainPanel.player.duration > 0) ? mainPanel.chapters : []

                    Rectangle {
                        id: chapterTick
                        required property var modelData
                        x: seekSlider.leftPadding
                           + (chapterTick.modelData.t0 * 1000 / mainPanel.player.duration) * (seekSlider.availableWidth - width)
                        y: seekSlider.topPadding + seekSlider.availableHeight / 2 - height / 2
                        width: 2
                        height: 10
                        radius: 1
                        color: "#ffffff"
                        opacity: chapterTickArea.containsMouse ? 0.9 : 0.55

                        ToolTip.visible: chapterTickArea.containsMouse
                        ToolTip.text: chapterTick.modelData.title
                        ToolTip.delay: 500

                        MouseArea {
                            id: chapterTickArea
                            anchors.fill: parent
                            anchors.margins: -3
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: mainPanel.player.position = chapterTick.modelData.t0 * 1000
                        }
                    }
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            visible: mainPanel.orphanedMdPath !== "" || (mainPanel.player && mainPanel.player.source.toString() !== "")

            Item {
                id: titleContainer
                Layout.fillWidth: true
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
                            anchors.fill: parent
                            color: "white"
                            font.pixelSize: 14
                            text: mainPanel.displayTitle()
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
                    }

                    // Item P5: when "labels" mode is showing an AI-generated
                    // label instead of the real filename (see
                    // AppSettings.useLabelsMainPanel/displayTitle()), the
                    // actual on-disk file is no longer visible anywhere —
                    // this small secondary line restores that, reusing
                    // displayTitle()'s own relativeToVault() helper rather
                    // than re-deriving the path. Only for the player-backed
                    // view (orphaned .md has no player.source to relativize)
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
                    // Shows the base stem (no dir/ext) since renameFile takes a
                    // stem and preserves the extension. Disabled while a job is
                    // running so a rename can't race an operation.
                    TextField {
                        id: filenameSubtitle
                        Layout.fillWidth: true
                        visible: AppSettings.useLabelsMainPanel
                                 && mainPanel.orphanedMdPath === "" && !!mainPanel.storedMetadata.label
                        enabled: !(mainPanel.transcriptionController && mainPanel.transcriptionController.busy)
                        color: "#777777"
                        font.pixelSize: 10
                        text: mainPanel.currentFileStem()
                        padding: 0
                        leftPadding: 0
                        rightPadding: 0
                        topPadding: 0
                        bottomPadding: 0

                        onAccepted: mainPanel.commitFilenameRename(text)
                        onActiveFocusChanged: if (!activeFocus) mainPanel.commitFilenameRename(text)
                        Keys.onEscapePressed: filenameSubtitle.text = mainPanel.currentFileStem()

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
            Item {
                id: transcribeIconButton
                Layout.preferredWidth: 22
                Layout.preferredHeight: 22
                visible: mainPanel.orphanedMdPath === "" && mainPanel.player
                         && mainPanel.player.source.toString() !== ""

                readonly property bool isActive: mainPanel.transcriptionController && mainPanel.player
                    && mainPanel.transcriptionController.busy
                    && mainPanel.transcriptionController.currentFile.toString() === mainPanel.player.source.toString()
                readonly property bool isQueued: mainPanel.transcriptionController && mainPanel.player
                    && mainPanel.player.source.toString() !== ""
                    && mainPanel.transcriptionController.queuedFileUrls.some(
                           u => u.toString() === mainPanel.player.source.toString())
                enabled: visible && mainPanel.transcriptionController
                         && !transcribeIconButton.isActive && !transcribeIconButton.isQueued

                ToolTip.visible: transcribeIconHover.hovered
                ToolTip.text: transcribeIconButton.isActive ? qsTr("Transcribing…")
                              : transcribeIconButton.isQueued ? qsTr("Queued…")
                              : (mainPanel.transcriptionController && mainPanel.transcriptionController.busy
                                 ? qsTr("Add to queue")
                                 : (mainPanel.hasStoredMetadata ? qsTr("Re-transcribe") : qsTr("Transcribe")))
                ToolTip.delay: 500

                HoverHandler {
                    id: transcribeIconHover
                    cursorShape: Qt.PointingHandCursor
                    enabled: transcribeIconButton.enabled
                }
                TapHandler {
                    enabled: transcribeIconButton.enabled
                    onTapped: mainPanel.transcriptionController.transcribe(mainPanel.player.source, mainPanel.vaultPath,
                                                                             modelCombo.currentValue,
                                                                             AppSettings.autoLabelEnabled,
                                                                             AppSettings.restrictTagsToExisting,
                                                                             AppSettings.tagReuseBias,
                                                                             AppSettings.wordiness)
                }
                HoverBorder {
                    anchors.fill: parent
                    active: transcribeIconButton.enabled && transcribeIconHover.hovered
                }
                AppIcon {
                    anchors.centerIn: parent
                    name: "transcribe"
                    iconSize: 13
                    color: transcribeIconButton.enabled ? "#dddddd" : "#666666"
                }
            }

            // Items 24/53: "Run all jobs" — queues the full transcribe->label
            // ->chapters pipeline for the open file. Per the user's spec, shown
            // ONLY when there's no transcription yet (an untranscribed audio
            // file): once transcribed, the individual regenerate-label /
            // generate-chapters icons beside it cover the derived passes. Not
            // shown for an orphaned .md (nothing to transcribe — the
            // relabel/chapters icons already handle that case).
            Item {
                id: runAllIconButton
                Layout.preferredWidth: 22
                Layout.preferredHeight: 22
                visible: mainPanel.transcriptionController && mainPanel.player
                         && mainPanel.player.source.toString() !== ""
                         && mainPanel.orphanedMdPath === "" && !mainPanel.hasStoredMetadata
                readonly property bool isQueued: mainPanel.transcriptionController && mainPanel.player
                    && mainPanel.transcriptionController.queuedFileUrls.some(
                           u => u.toString() === mainPanel.player.source.toString())
                enabled: visible && !runAllIconButton.isQueued
                         && !(mainPanel.transcriptionController.busy
                              && mainPanel.transcriptionController.currentFile.toString()
                                 === mainPanel.player.source.toString())

                ToolTip.visible: runAllIconHover.hovered
                ToolTip.text: qsTr("Run all jobs (transcribe, label, chapters)")
                ToolTip.delay: 500

                HoverHandler {
                    id: runAllIconHover
                    cursorShape: Qt.PointingHandCursor
                    enabled: runAllIconButton.enabled
                }
                TapHandler {
                    enabled: runAllIconButton.enabled
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
                HoverBorder {
                    anchors.fill: parent
                    active: runAllIconButton.enabled && runAllIconHover.hovered
                }
                AppIcon {
                    anchors.centerIn: parent
                    name: "run-all"
                    iconSize: 15
                    color: runAllIconButton.enabled ? "#dddddd" : "#666666"
                }
            }

            // Item 9: regenerate auto-title — re-runs just the AI labeling
            // pass on the already-transcribed text (no re-transcription).
            // Visible whenever there's a transcript to label, independent of
            // AppSettings.autoLabelEnabled (that setting is about running
            // automatically after each transcription, not gating manual
            // regeneration).
            Item {
                id: regenerateLabelButton
                Layout.preferredWidth: 22
                Layout.preferredHeight: 22
                visible: mainPanel.transcriptionController
                         && (mainPanel.orphanedMdPath !== "" || mainPanel.hasStoredMetadata)
                readonly property url targetUrl: mainPanel.orphanedMdPath !== ""
                    ? ("file://" + mainPanel.orphanedMdPath) : mainPanel.player.source
                enabled: visible && mainPanel.busyOrQueuedUrls.indexOf(regenerateLabelButton.targetUrl.toString()) === -1

                ToolTip.visible: regenerateLabelHover.hovered
                ToolTip.text: qsTr("Regenerate label and prelude")
                ToolTip.delay: 500

                HoverHandler {
                    id: regenerateLabelHover
                    cursorShape: Qt.PointingHandCursor
                    enabled: regenerateLabelButton.enabled
                }
                TapHandler {
                    enabled: regenerateLabelButton.enabled
                    onTapped: mainPanel.transcriptionController.relabel(regenerateLabelButton.targetUrl,
                                                                         mainPanel.vaultPath,
                                                                         AppSettings.restrictTagsToExisting,
                                                                         AppSettings.tagReuseBias,
                                                                         AppSettings.wordiness)
                }
                HoverBorder {
                    anchors.fill: parent
                    active: regenerateLabelButton.enabled && regenerateLabelHover.hovered
                }
                AppIcon {
                    anchors.centerIn: parent
                    name: "relabel"
                    iconSize: 14
                    color: regenerateLabelButton.enabled ? "#dddddd" : "#666666"
                }
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
            Item {
                id: generateChaptersButton
                Layout.preferredWidth: 22
                Layout.preferredHeight: 22
                visible: mainPanel.transcriptionController
                         && (mainPanel.orphanedMdPath !== "" || mainPanel.hasStoredMetadata)
                readonly property url targetUrl: mainPanel.orphanedMdPath !== ""
                    ? ("file://" + mainPanel.orphanedMdPath) : mainPanel.player.source
                enabled: visible && mainPanel.busyOrQueuedUrls.indexOf(generateChaptersButton.targetUrl.toString()) === -1

                ToolTip.visible: generateChaptersHover.hovered
                ToolTip.text: qsTr("Generate chapters")
                ToolTip.delay: 500

                HoverHandler {
                    id: generateChaptersHover
                    cursorShape: Qt.PointingHandCursor
                    enabled: generateChaptersButton.enabled
                }
                TapHandler {
                    enabled: generateChaptersButton.enabled
                    onTapped: mainPanel.transcriptionController.generateChapters(generateChaptersButton.targetUrl, mainPanel.vaultPath, AppSettings.chapterDensity)
                }
                HoverBorder {
                    anchors.fill: parent
                    active: generateChaptersButton.enabled && generateChaptersHover.hovered
                }
                AppIcon {
                    anchors.centerIn: parent
                    name: "chapters"
                    iconSize: 15
                    color: generateChaptersButton.enabled ? "#dddddd" : "#666666"
                }
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
            Item {
                id: detectSpeakersButton
                Layout.preferredWidth: 22
                Layout.preferredHeight: 22
                visible: mainPanel.transcriptionController
                         && mainPanel.orphanedMdPath === "" && mainPanel.hasStoredMetadata
                readonly property url targetUrl: mainPanel.player.source
                enabled: visible && mainPanel.busyOrQueuedUrls.indexOf(detectSpeakersButton.targetUrl.toString()) === -1

                ToolTip.visible: detectSpeakersHover.hovered
                ToolTip.text: qsTr("Detect speakers")
                ToolTip.delay: 500

                HoverHandler {
                    id: detectSpeakersHover
                    cursorShape: Qt.PointingHandCursor
                    enabled: detectSpeakersButton.enabled
                }
                TapHandler {
                    enabled: detectSpeakersButton.enabled
                    onTapped: mainPanel.transcriptionController.diarizeFile(detectSpeakersButton.targetUrl,
                                                                            mainPanel.vaultPath,
                                                                            AppSettings.diarizationSegmentationModel,
                                                                            AppSettings.diarizationEmbeddingModel,
                                                                            AppSettings.diarizationSensitivity)
                }
                HoverBorder {
                    anchors.fill: parent
                    active: detectSpeakersButton.enabled && detectSpeakersHover.hovered
                }
                AppIcon {
                    anchors.centerIn: parent
                    name: "speakers"
                    iconSize: 14
                    color: detectSpeakersButton.enabled ? "#dddddd" : "#666666"
                }
            }

            // Item 2: "Delete transcript" — removes the open transcribed audio
            // file's sibling .md (VaultTreeModel::trashTranscript()), reverting
            // it to untranscribed while KEEPING the audio. Only for a
            // transcribed AUDIO file (an orphaned .md has no audio to keep — it
            // gets "Delete file" instead). Disabled while a job is
            // queued/active for this file (deleting its sidecar mid-write would
            // fight the worker) — same isQueued gate the regenerate icons use.
            Item {
                id: deleteTranscriptButton
                Layout.preferredWidth: 22
                Layout.preferredHeight: 22
                visible: mainPanel.transcriptionController && mainPanel.vaultTreeModel && mainPanel.player
                         && mainPanel.orphanedMdPath === "" && mainPanel.hasStoredMetadata
                readonly property url targetUrl: mainPanel.player.source
                enabled: visible && mainPanel.busyOrQueuedUrls.indexOf(deleteTranscriptButton.targetUrl.toString()) === -1

                ToolTip.visible: deleteTranscriptHover.hovered
                ToolTip.text: qsTr("Delete transcript (keep audio)")
                ToolTip.delay: 500

                HoverHandler {
                    id: deleteTranscriptHover
                    cursorShape: Qt.PointingHandCursor
                    enabled: deleteTranscriptButton.enabled
                }
                TapHandler {
                    enabled: deleteTranscriptButton.enabled
                    onTapped: mainPanel.requestDeleteTranscript()
                }
                HoverBorder {
                    anchors.fill: parent
                    active: deleteTranscriptButton.enabled && deleteTranscriptHover.hovered
                }
                AppIcon {
                    anchors.centerIn: parent
                    name: "delete-transcript"
                    iconSize: 14
                    color: deleteTranscriptButton.enabled ? "#e07a7a" : "#666666"
                }
            }

            // "Delete audio only" — the complement of "Delete transcript", and
            // gated identically: a transcribed AUDIO file (orphanedMdPath == ""
            // + hasStoredMetadata), never an orphaned .md (no audio to drop)
            // and never an untranscribed one (nothing to keep, so it would just
            // duplicate "Delete file"). Same isQueued gate — dropping the audio
            // out from under a running job would fight the worker.
            Item {
                id: deleteAudioOnlyButton
                objectName: "deleteAudioOnlyButton"
                Layout.preferredWidth: 22
                Layout.preferredHeight: 22
                visible: mainPanel.transcriptionController && mainPanel.vaultTreeModel && mainPanel.player
                         && mainPanel.orphanedMdPath === "" && mainPanel.hasStoredMetadata
                readonly property url targetUrl: mainPanel.player.source
                enabled: visible && mainPanel.busyOrQueuedUrls.indexOf(deleteAudioOnlyButton.targetUrl.toString()) === -1

                ToolTip.visible: deleteAudioOnlyHover.hovered
                ToolTip.text: qsTr("Delete audio (keep transcript)")
                ToolTip.delay: 500

                HoverHandler {
                    id: deleteAudioOnlyHover
                    cursorShape: Qt.PointingHandCursor
                    enabled: deleteAudioOnlyButton.enabled
                }
                TapHandler {
                    enabled: deleteAudioOnlyButton.enabled
                    onTapped: mainPanel.requestDeleteAudioOnly()
                }
                HoverBorder {
                    anchors.fill: parent
                    active: deleteAudioOnlyButton.enabled && deleteAudioOnlyHover.hovered
                }
                AppIcon {
                    anchors.centerIn: parent
                    name: "delete-audio"
                    iconSize: 14
                    color: deleteAudioOnlyButton.enabled ? "#e07a7a" : "#666666"
                }
            }

            // Item 2: "Delete file" — deletes the whole open file and its
            // sidecars (VaultTreeModel::trashFiles()): audio + .md + partial,
            // or an orphaned .md alone. Shown for any open file (audio OR
            // orphaned .md). Same isQueued gate as above.
            Item {
                id: deleteFileButton
                Layout.preferredWidth: 22
                Layout.preferredHeight: 22
                visible: mainPanel.vaultTreeModel
                         && (mainPanel.orphanedMdPath !== ""
                             || (mainPanel.player && mainPanel.player.source.toString() !== ""))
                readonly property url targetUrl: mainPanel.currentFileUrl()
                enabled: visible && mainPanel.busyOrQueuedUrls.indexOf(deleteFileButton.targetUrl.toString()) === -1

                ToolTip.visible: deleteFileHover.hovered
                ToolTip.text: qsTr("Delete file")
                ToolTip.delay: 500

                HoverHandler {
                    id: deleteFileHover
                    cursorShape: Qt.PointingHandCursor
                    enabled: deleteFileButton.enabled
                }
                TapHandler {
                    enabled: deleteFileButton.enabled
                    onTapped: mainPanel.requestDeleteFile()
                }
                HoverBorder {
                    anchors.fill: parent
                    active: deleteFileButton.enabled && deleteFileHover.hovered
                }
                AppIcon {
                    anchors.centerIn: parent
                    name: "delete-file"
                    iconSize: 14
                    color: deleteFileButton.enabled ? "#e07a7a" : "#666666"
                }
            }

            // Item 18: search icon → expands to a text field, Ctrl+F while
            // the main panel has focus. Searches title/tags (via
            // storedMetadata, already loaded) + the transcript body
            // (jumpToNextMatch() selects/scrolls the next match below).
            // Item 7: this field is ALSO the find pattern for replace — the
            // .*/Aa toggles and the Replace checkbox sit right after it, and
            // the replace row unfolds below the whole title row.
            Item {
                id: searchToggleButton
                visible: !mainPanel.searchExpanded
                Layout.preferredWidth: 22
                Layout.preferredHeight: 22

                HoverHandler {
                    id: searchToggleHover
                    cursorShape: Qt.PointingHandCursor
                }
                TapHandler {
                    onTapped: {
                        mainPanel.searchExpanded = true
                        searchField.forceActiveFocus()
                    }
                }
                HoverBorder {
                    anchors.fill: parent
                    active: searchToggleHover.hovered
                }
                AppIcon {
                    anchors.centerIn: parent
                    name: "search"
                    iconSize: 15
                    color: "#dddddd"
                }
            }

            TextField {
                id: searchField
                visible: mainPanel.searchExpanded
                Layout.preferredWidth: 200
                // Item 7: the placeholder tracks the mode, since the same
                // field means a literal string or a regex depending on ".*".
                placeholderText: mainPanel.findRegexEnabled
                                 ? qsTr("Pattern in transcript, title, tags…")
                                 : qsTr("Search transcript, title, tags…")
                text: mainPanel.searchQuery
                onTextChanged: mainPanel.searchQuery = text
                onAccepted: mainPanel.jumpToNextMatch()
                // Item 7: an invalid regex reddens the text it's invalid in
                // (the banner below spells out why).
                color: mainPanel.findReplaceError !== "" ? "#e07a7a" : "#dddddd"

                background: UnderlineField { focused: searchField.activeFocus }

                HoverHandler {
                    cursorShape: Qt.IBeamCursor
                }
            }

            // Item 7: regex mode toggle (.*) — active state uses the app
            // accent so it reads as a mode, not a one-shot button. Governs
            // the live search as well as replace (see findRegexEnabled).
            Item {
                id: regexToggle
                visible: mainPanel.searchExpanded
                Layout.preferredWidth: 26
                Layout.preferredHeight: 22
                ToolTip.visible: regexToggleHover.hovered
                ToolTip.text: qsTr("Regular expression")
                ToolTip.delay: 500
                HoverHandler { id: regexToggleHover; cursorShape: Qt.PointingHandCursor }
                TapHandler { onTapped: mainPanel.findRegexEnabled = !mainPanel.findRegexEnabled }
                HoverBorder { anchors.fill: parent; active: regexToggleHover.hovered }
                Text {
                    anchors.centerIn: parent
                    text: ".*"
                    color: mainPanel.findRegexEnabled ? AppSettings.accentColor : "#888888"
                    font.pixelSize: 13
                    font.bold: mainPanel.findRegexEnabled
                }
            }

            // Item 7: case-sensitivity toggle (Aa) — on means case-INsensitive.
            Item {
                id: caseToggle
                visible: mainPanel.searchExpanded
                Layout.preferredWidth: 26
                Layout.preferredHeight: 22
                ToolTip.visible: caseToggleHover.hovered
                ToolTip.text: qsTr("Ignore case")
                ToolTip.delay: 500
                HoverHandler { id: caseToggleHover; cursorShape: Qt.PointingHandCursor }
                TapHandler { onTapped: mainPanel.findCaseInsensitive = !mainPanel.findCaseInsensitive }
                HoverBorder { anchors.fill: parent; active: caseToggleHover.hovered }
                Text {
                    anchors.centerIn: parent
                    text: "Aa"
                    color: mainPanel.findCaseInsensitive ? AppSettings.accentColor : "#888888"
                    font.pixelSize: 12
                    font.bold: mainPanel.findCaseInsensitive
                }
            }

            // Item 13: "a/b" match position, updating as Enter cycles
            // through matches (and wraps back to 1 after the last one).
            Label {
                visible: mainPanel.searchExpanded && mainPanel.searchQuery !== ""
                text: mainPanel.searchMatchCount > 0
                      ? qsTr("%1/%2").arg(mainPanel.searchMatchOrdinal).arg(mainPanel.searchMatchCount)
                      : qsTr("0/0")
                color: "#888888"
                font.pixelSize: 11
            }

            // Item 7: the checkbox that arms replace. Minimal pill toggle
            // (same idiom as Sidebar.qml's search-scope toggle) rather than a
            // QQC2 CheckBox — which would
            // also steal focus from searchField on press, per the
            // focusable-control-beside-a-TextField hazard this file already
            // avoids everywhere else.
            Item {
                id: replaceToggle
                visible: mainPanel.searchExpanded
                Layout.preferredWidth: replaceToggleLabel.implicitWidth + 16
                Layout.preferredHeight: 22

                HoverHandler { id: replaceToggleHover; cursorShape: Qt.PointingHandCursor }
                TapHandler { onTapped: mainPanel.replaceEnabled = !mainPanel.replaceEnabled }
                Rectangle {
                    anchors.fill: parent
                    radius: AppSettings.borderRadius
                    color: mainPanel.replaceEnabled
                           ? (replaceToggleHover.hovered ? "#2ecc71" : "#25a25a")
                           : (replaceToggleHover.hovered ? "#3a3a3a" : "#2a2a2a")
                    border.width: 1
                    border.color: mainPanel.replaceEnabled ? "#2ecc71" : "#4a4a4a"

                    Behavior on color { ColorAnimation { duration: AppSettings.animationsEnabled ? 100 : 0 } }
                }
                Label {
                    id: replaceToggleLabel
                    anchors.centerIn: parent
                    text: qsTr("Replace")
                    color: mainPanel.replaceEnabled ? "white" : "#aaaaaa"
                    font.pixelSize: 11
                }
            }

            Item {
                id: searchCloseButton
                visible: mainPanel.searchExpanded
                Layout.preferredWidth: 20
                Layout.preferredHeight: 20

                HoverHandler {
                    id: searchCloseHover
                    cursorShape: Qt.PointingHandCursor
                }
                TapHandler {
                    onTapped: {
                        searchField.text = ""
                        mainPanel.searchQuery = ""
                        mainPanel.searchExpanded = false
                        // Item 7: closing disarms replace and drops the
                        // banner, so the bar can't reopen mid-edit already
                        // armed with a stale pattern.
                        mainPanel.replaceEnabled = false
                        mainPanel.findReplaceError = ""
                    }
                }
                HoverBorder {
                    anchors.fill: parent
                    active: searchCloseHover.hovered
                }
                AppIcon {
                    anchors.centerIn: parent
                    name: "close"
                    iconSize: 15
                    color: "#dddddd"
                }
            }
        }

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
        ColumnLayout {
            Layout.fillWidth: true
            visible: mainPanel.searchExpanded
            spacing: 4

            RowLayout {
                Layout.fillWidth: true
                visible: mainPanel.replaceEnabled
                spacing: 8

                Label {
                    text: qsTr("Replace with")
                    color: "#888888"
                    font.pixelSize: 11
                }

                TextField {
                    id: replaceField
                    Layout.preferredWidth: 180
                    placeholderText: qsTr("replacement…")
                    text: mainPanel.replaceText
                    onTextChanged: mainPanel.replaceText = text
                    color: "#dddddd"
                    background: UnderlineField { focused: replaceField.activeFocus }
                    HoverHandler { cursorShape: Qt.IBeamCursor }
                }

                // Replace / Replace-all: Item+TapHandler buttons (NOT
                // focusable QQC2 Buttons) so they never steal focus from the
                // find/replace fields on press — project memory: a focusable
                // button next to a focused TextField grabs focus on press.
                Item {
                    id: replaceNextButton
                    Layout.preferredHeight: 22
                    Layout.preferredWidth: replaceNextText.implicitWidth + 16
                    HoverHandler { id: replaceNextHover; cursorShape: Qt.PointingHandCursor }
                    TapHandler { onTapped: mainPanel.replaceNext() }
                    Rectangle {
                        anchors.fill: parent
                        radius: AppSettings.borderRadius
                        color: "transparent"
                        border.width: 1
                        border.color: "#3a3a3a"
                    }
                    HoverBorder { anchors.fill: parent; active: replaceNextHover.hovered }
                    Text {
                        id: replaceNextText
                        anchors.centerIn: parent
                        text: qsTr("Replace")
                        color: "#dddddd"
                        font.pixelSize: 11
                    }
                }

                Item {
                    id: replaceAllButton
                    Layout.preferredHeight: 22
                    Layout.preferredWidth: replaceAllText.implicitWidth + 16
                    HoverHandler { id: replaceAllHover; cursorShape: Qt.PointingHandCursor }
                    TapHandler { onTapped: mainPanel.replaceAll() }
                    Rectangle {
                        anchors.fill: parent
                        radius: AppSettings.borderRadius
                        color: "transparent"
                        border.width: 1
                        border.color: "#3a3a3a"
                    }
                    HoverBorder { anchors.fill: parent; active: replaceAllHover.hovered }
                    Text {
                        id: replaceAllText
                        anchors.centerIn: parent
                        text: qsTr("Replace all")
                        color: "#dddddd"
                        font.pixelSize: 11
                    }
                }

                Item { Layout.fillWidth: true }

                // Item 7: disarms replace, folding this row away and leaving
                // the find bar (and your pattern) intact — the "×" on the
                // search row above is what closes the whole thing.
                Item {
                    id: replaceCollapseButton
                    Layout.preferredWidth: 20
                    Layout.preferredHeight: 20
                    ToolTip.visible: replaceCollapseHover.hovered
                    ToolTip.text: qsTr("Hide replace")
                    ToolTip.delay: 500
                    HoverHandler { id: replaceCollapseHover; cursorShape: Qt.PointingHandCursor }
                    TapHandler { onTapped: mainPanel.replaceEnabled = false }
                    HoverBorder { anchors.fill: parent; active: replaceCollapseHover.hovered }
                    AppIcon {
                        anchors.centerIn: parent
                        name: "close"
                        iconSize: 15
                        color: "#dddddd"
                    }
                }
            }

            // Invalid-regex inline error (StyleGuide.md warning-banner
            // convention: backing string, #e07a7a, 11px, wrapping — no
            // separate visible flag beyond the string being non-empty).
            Label {
                Layout.fillWidth: true
                visible: mainPanel.findReplaceError !== ""
                text: mainPanel.findReplaceError
                color: "#e07a7a"
                font.pixelSize: 11
                wrapMode: Text.WordWrap
            }
        }

        // Item: only the playback progress bar and title/regen/search row
        // above are "sticky" (fixed, always visible) — everything else
        // (tags, speaker pills, detection/model controls, the transcribe
        // button, and the transcript itself) now lives INSIDE
        // transcriptScrollView's own scrollable content, so it scrolls away
        // together with the transcript on a long file instead of
        // permanently eating vertical space above it. This is the SAME
        // ScrollView/contentItem that middleDragScrollArea and the
        // live-transcription auto-scroll-to-bottom logic already reference
        // (see onTranscriptBodyChanged) — moving these rows inside its
        // single content child doesn't change either of those, since both
        // already operate on transcriptScrollView.contentItem generically.
        // transcriptScrollView must have exactly ONE content child for QQC2's
        // ScrollView to correctly auto-track content size (contentWidth/
        // contentHeight) — a second child (middleDragScrollArea used to live
        // here too) leaves that tracking ill-defined and silently breaks
        // scrolling entirely. So the MouseArea now lives as a sibling
        // OVERLAY on top of the ScrollView instead, inside this wrapping
        // Item — anchors.fill still gives it the same full-viewport
        // coverage for middle-drag panning, it just no longer counts as
        // flickable content itself.
        Item {
            id: transcriptViewportContainer
            Layout.fillWidth: true
            Layout.fillHeight: true

            // The Basic style's vertical ScrollBar is an OVERLAY — `parent:
            // control; x: control.width - width` (see SettingsPopup.qml's
            // contentColumn for the same trap) — so it reserves no layout
            // space and floats flush against the ScrollView's right edge, on
            // top of a transcript that fills the full width. SettingsPopup
            // solves this by narrowing its content; here the transcript is
            // worth keeping as wide as it is, and the root ColumnLayout's
            // 16px right margin is dead space, so the bar moves THERE
            // instead: cancel part of that margin so the ScrollView reaches
            // into the gutter, then subtract the same amount back off
            // scrollableContent's width (below) so the transcript keeps
            // exactly the bounds it had. 13 leaves the Basic bar's 10px box
            // (6px groove + 2px padding a side) centered in the 16px gutter,
            // 3px clear of both the transcript and the panel edge.
            readonly property int scrollBarGutter: 13
            Layout.rightMargin: -scrollBarGutter

        ScrollView {
            id: transcriptScrollView
            objectName: "transcriptScrollView"
            anchors.fill: parent
            clip: true
            // Item 55: while a (re)transcription for THIS file has started but
            // no new segment has streamed in yet, dim + hard-disable the stale
            // transcript so it can't be selected/hovered/scrolled/edited (the
            // transcribingOverlay sibling below shows a spinner on top). The
            // moment live output begins (awaitingFirstSegment clears on the
            // first live tick) this reverts and the user watches it stream in.
            // A true gaussian blur was deliberately avoided here: it needs a
            // new QtQuick.Effects/Qt5Compat.GraphicalEffects import, and per
            // project memory a fresh QML import can crash-on-load (or silently
            // freeze) under AOT — uncheckable without a live GUI run — so dim
            // is the AOT-safe half of "blur/dim".
            enabled: !mainPanel.showTranscribingOverlay
            opacity: mainPanel.showTranscribingOverlay ? 0.25 : 1
            ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

            ColumnLayout {
                id: scrollableContent
                // Give back the margin transcriptViewportContainer bled into
                // for the overlay scrollbar (see its doc comment) — the
                // transcript keeps its original width and right edge, and the
                // bar now lives entirely in the reclaimed gutter.
                width: transcriptScrollView.availableWidth - transcriptViewportContainer.scrollBarGutter
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
                    // visible whenever there's an actual note to attach tags
                    // to (mirrors preludeContainer/chapterListColumn's own
                    // gating), not just once currentTags() is non-empty.
                    visible: mainPanel.currentTags().length > 0 || mainPanel.orphanedMdPath !== ""
                             || mainPanel.hasStoredMetadata

                    Repeater {
                        model: mainPanel.currentTags()

                        Rectangle {
                            id: tagPill
                            required property string modelData
                            radius: AppSettings.borderRadius
                            color: tagPillHover.hovered ? "#3f3f3f" : "#333333"
                            border.width: 1
                            border.color: "#4a4a4a"
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
                                    color: tagSortIconHover.hovered ? "#dddddd" : "#888888"

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
                                    color: tagRemoveHover.hovered ? "#e07a7a" : "#888888"

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
                                           ? "#dddddd" : "#888888"

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
                                    color: "#dddddd"
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
                                    color: speakerSortIconHover.hovered ? "#dddddd" : "#888888"

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
                                    color: "#dddddd"
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
                                    color: "#e07a7a"
                                    HoverHandler { cursorShape: Qt.PointingHandCursor }
                                    TapHandler { onTapped: mainPanel.confirmSpeakerMerge(false) }
                                }
                            }
                        }
                    }
                }

                Label {
                    Layout.fillWidth: true
                    visible: mainPanel.orphanedMdPath === "" && (!mainPanel.player || mainPanel.player.source.toString() === "")
                    text: qsTr("No file selected")
                    color: "white"
                }

                RowLayout {
                    spacing: 12
                    visible: mainPanel.orphanedMdPath === "" && mainPanel.hasStoredMetadata

                    TextEdit {
                        Layout.fillWidth: true
                        wrapMode: TextEdit.WordWrap
                        readOnly: true
                        selectByMouse: true
                        selectByKeyboard: true
                        activeFocusOnPress: false
                        text: qsTr("Previously transcribed — model: %1, speaker detection: %2")
                              .arg(mainPanel.storedMetadata.model || "?")
                              .arg(mainPanel.storedMetadata.speakerDetection ? qsTr("on") : qsTr("off"))
                        color: "#aaaaaa"
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
                // Visibility mirrors O26's own gating exactly (never shown
                // for an orphaned .md, only once the file has been
                // transcribed at all).
                Item {
                    id: preludeContainer
                    Layout.fillWidth: true
                    visible: mainPanel.orphanedMdPath === "" && mainPanel.hasStoredMetadata
                    implicitHeight: preludeArea.implicitHeight

                    // Always-editable: a single TextArea is present at all
                    // times (no display-TextEdit/edit-TextArea swap, mirrors
                    // titleField's pattern above) — clicking directly into
                    // the text starts editing, no separate pencil-icon
                    // trigger needed any more. Autosaves via commitPrelude()
                    // the moment focus leaves the field (see
                    // onActiveFocusChanged below), no Save/Cancel chrome.
                    TextArea {
                        id: preludeArea
                        anchors.left: parent.left
                        anchors.right: parent.right
                        wrapMode: TextArea.Wrap
                        color: "#dddddd"
                        font.pixelSize: 12
                        selectByMouse: true
                        selectByKeyboard: true
                        text: mainPanel.prelude
                        placeholderText: qsTr("Click to add a prelude…")
                        // See titleField's doc comment — zeroed to match
                        // the old read-only TextEdit's tight sizing.
                        padding: 0
                        leftPadding: 0
                        rightPadding: 0
                        topPadding: 0
                        bottomPadding: 0
                        background: UnderlineField { implicitHeight: 0; plainUntilHovered: true; focused: preludeArea.activeFocus }
                        Keys.onEscapePressed: preludeArea.text = mainPanel.prelude
                        onActiveFocusChanged: if (!activeFocus) mainPanel.commitPrelude(text)

                        // Item 13: this LOST its I-beam when b72b365 turned the
                        // old read-only TextEdit into an editable TextArea —
                        // QQC2 TextArea has no built-in one, restore it here.
                        HoverHandler { cursorShape: Qt.IBeamCursor }
                    }
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
                ColumnLayout {
                    id: chapterListColumn
                    Layout.fillWidth: true
                    visible: mainPanel.chapters.length > 0
                    spacing: 4

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 6

                        AppIcon {
                            name: mainPanel.chaptersExpanded ? "collapse" : "expand"
                            iconSize: 11
                            color: "#aaaaaa"

                            HoverHandler { cursorShape: Qt.PointingHandCursor }
                            TapHandler { onTapped: mainPanel.chaptersExpanded = !mainPanel.chaptersExpanded }
                        }

                        Label {
                            Layout.fillWidth: true
                            text: qsTr("Chapters (%1)").arg(mainPanel.chapters.length)
                            color: "#aaaaaa"
                            font.pixelSize: 11
                            font.bold: true

                            HoverHandler { cursorShape: Qt.PointingHandCursor }
                            TapHandler { onTapped: mainPanel.chaptersExpanded = !mainPanel.chaptersExpanded }
                        }

                        // Item 17: add a MANUAL chapter at the current
                        // playhead. Player-backed only (an orphaned .md has no
                        // audio to position against). Item+TapHandler rather
                        // than a focusable QQC2 Button so tapping it can't
                        // steal focus from a chapter title being renamed
                        // (project-memory focus-grab warning) — it just adds +
                        // drops into editing the new one.
                        Item {
                            id: addChapterButton
                            Layout.preferredWidth: 18
                            Layout.preferredHeight: 18
                            visible: !!mainPanel.player && mainPanel.player.source.toString() !== ""

                            ToolTip.visible: addChapterHover.hovered
                            ToolTip.text: qsTr("Add chapter at playhead")
                            ToolTip.delay: 500

                            HoverHandler {
                                id: addChapterHover
                                cursorShape: Qt.PointingHandCursor
                            }
                            TapHandler { onTapped: mainPanel.addChapterAtPlayhead() }
                            HoverBorder {
                                anchors.fill: parent
                                active: addChapterHover.hovered
                            }
                            AppIcon {
                                anchors.centerIn: parent
                                name: "add"
                                iconSize: 14
                                color: "#8fdf9f"
                            }
                        }
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        Layout.leftMargin: 20
                        spacing: 2
                        visible: mainPanel.chaptersExpanded

                        Repeater {
                            model: mainPanel.chapters

                            RowLayout {
                                id: chapterRow
                                required property var modelData
                                Layout.fillWidth: true
                                spacing: 8

                                // Item P33: pointer cursor + click-to-seek
                                // scoped to JUST the timestamp now (used to
                                // span the whole row, covering the title
                                // too) — the title below gets its own
                                // independent I-beam/selectable treatment
                                // instead of inheriting this one.
                                Label {
                                    id: chapterTimestampLabel
                                    text: mainPanel.formatGutterTimestamp(chapterRow.modelData.t0)
                                    color: "#777777"
                                    font.pixelSize: 10

                                    HoverHandler { cursorShape: Qt.PointingHandCursor }
                                    TapHandler { onTapped: mainPanel.player.position = chapterRow.modelData.t0 * 1000 }
                                }

                                // Item 17: manual chapters (user-created or
                                // -edited) get a subtle pencil marker,
                                // distinguishing them from auto-generated ones
                                // via readChapters()' `manual` bool.
                                AppIcon {
                                    visible: !!chapterRow.modelData.manual
                                    name: "edit"
                                    iconSize: 10
                                    color: "#8fdf9f"
                                }

                                // Item 34/35: click-to-edit chapter title. NOT
                                // free-selectable at rest (readOnly + no
                                // selectByMouse) — that's what kills the
                                // multiple-simultaneous-highlights bug (item
                                // 34): only when THIS chapter becomes the
                                // single active edit (editingChapterTs === its
                                // real t0) does it turn into a focused,
                                // selectable field. Tapping another title (or
                                // clicking away → deselectTextSelections)
                                // reassigns editingChapterTs, whose change
                                // blurs+commits this one. Commit routes through
                                // setChapterTitle via commitChapterTitle().
                                TextEdit {
                                    id: tocChapterTitle
                                    readonly property bool editing: mainPanel.editingChapterTs === chapterRow.modelData.t0
                                    Layout.fillWidth: true
                                    clip: true
                                    readOnly: !tocChapterTitle.editing
                                    selectByMouse: tocChapterTitle.editing
                                    selectByKeyboard: tocChapterTitle.editing
                                    activeFocusOnPress: false
                                    text: chapterRow.modelData.title
                                    color: "#cccccc"
                                    font.pixelSize: 11

                                    HoverHandler { cursorShape: Qt.IBeamCursor }
                                    // Only the tapped instance focuses itself
                                    // (a chapter has BOTH a TOC row and a
                                    // transcript heading bound to the same
                                    // editingChapterTs — focusing from
                                    // onEditingChanged would make them fight
                                    // over which grabs focus).
                                    TapHandler {
                                        onTapped: (eventPoint) => {
                                            mainPanel.editingChapterTs = chapterRow.modelData.t0
                                            if (!tocChapterTitle.activeFocus) {
                                                tocChapterTitle.forceActiveFocus()
                                                // Land the caret where the click
                                                // was. The press itself can't do
                                                // it (selectByMouse is still false
                                                // at rest), and selecting the whole
                                                // title instead would make the next
                                                // keystroke wipe it.
                                                tocChapterTitle.cursorPosition =
                                                    tocChapterTitle.positionAt(eventPoint.position.x,
                                                                               eventPoint.position.y)
                                            }
                                        }
                                    }
                                    // Exit side only: when this stops being the
                                    // active edit while still focused, blur
                                    // (→ commit). The unfocused twin skips this.
                                    onEditingChanged: {
                                        if (!tocChapterTitle.editing && tocChapterTitle.activeFocus)
                                            tocChapterTitle.focus = false
                                    }
                                    onActiveFocusChanged: {
                                        if (!tocChapterTitle.activeFocus) {
                                            mainPanel.commitChapterTitle(chapterRow.modelData.t0,
                                                                         chapterRow.modelData.title,
                                                                         tocChapterTitle.text)
                                            // Only clear if we're still the active
                                            // one — a tap on another title already
                                            // moved editingChapterTs, don't clobber it.
                                            if (mainPanel.editingChapterTs === chapterRow.modelData.t0)
                                                mainPanel.editingChapterTs = -1
                                        }
                                    }
                                    // Single-line commit convention (StyleGuide):
                                    // Return commits (blocks the newline), Escape
                                    // reverts to the last-known-good title.
                                    Keys.onReturnPressed: (event) => {
                                        event.accepted = true
                                        mainPanel.editingChapterTs = -1
                                    }
                                    Keys.onEnterPressed: (event) => {
                                        event.accepted = true
                                        mainPanel.editingChapterTs = -1
                                    }
                                    Keys.onEscapePressed: (event) => {
                                        event.accepted = true
                                        tocChapterTitle.text = chapterRow.modelData.title
                                        mainPanel.editingChapterTs = -1
                                    }
                                }

                                // Item 17: delete this chapter marker.
                                // Item+TapHandler (not a focusable Button) for
                                // the same focus-grab reason as addChapterButton.
                                Item {
                                    id: deleteChapterButton
                                    Layout.preferredWidth: 16
                                    Layout.preferredHeight: 16

                                    ToolTip.visible: deleteChapterHover.hovered
                                    ToolTip.text: qsTr("Delete chapter")
                                    ToolTip.delay: 500

                                    HoverHandler {
                                        id: deleteChapterHover
                                        cursorShape: Qt.PointingHandCursor
                                    }
                                    TapHandler {
                                        onTapped: mainPanel.deleteChapterAt(chapterRow.modelData.t0,
                                                                            chapterRow.modelData.title,
                                                                            !!chapterRow.modelData.manual)
                                    }
                                    HoverBorder {
                                        anchors.fill: parent
                                        active: deleteChapterHover.hovered
                                    }
                                    AppIcon {
                                        anchors.centerIn: parent
                                        name: "close"
                                        iconSize: 13
                                        color: "#c07a7a"
                                    }
                                }
                            }
                        }
                    }
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
                    visible: mainPanel.orphanedMdPath === "" && mainPanel.hasStoredMetadata
                             && !!mainPanel.storedMetadata.speakerDetectionError
                    text: qsTr("Speaker detection was requested but did not run: %1")
                          .arg(mainPanel.storedMetadata.speakerDetectionError || "")
                    color: "#e0a25a"
                    font.pixelSize: 11
                    wrapMode: Text.WordWrap
                }

                RowLayout {
                    id: modelRow
                    spacing: 12
                    Layout.fillWidth: true
                    visible: mainPanel.orphanedMdPath === ""

                    // Speaker detection is no longer a transcribe-time toggle —
                    // it's the standalone "Detect speakers" action (the
                    // detectSpeakersButton icon in the cluster above) / the
                    // runAllJobs pipeline step. Transcribe here is pure ASR.

                    Label {
                        text: qsTr("Model")
                        color: "#aaaaaa"
                    }

                    // Transcription model. Shares the row evenly with the two
                    // diarization pickers below via matching Layout.fillWidth
                    // + preferredWidth; ModelComboBox elides the closed label
                    // (full text in tooltip/popup) so nothing overflows.
                    ModelComboBox {
                        id: modelCombo
                        Layout.fillWidth: true
                        Layout.preferredWidth: 1
                        model: mainPanel.transcriptionController ? mainPanel.transcriptionController.availableModels() : []
                        selectedId: AppSettings.defaultModel
                        onPicked: (id) => AppSettings.defaultModel = id
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
                        color: "#aaaaaa"
                    }

                    ModelComboBox {
                        id: segmentationCombo
                        Layout.fillWidth: true
                        Layout.preferredWidth: 1
                        model: mainPanel.transcriptionController ? mainPanel.transcriptionController.availableSegmentationModels() : []
                        selectedId: AppSettings.diarizationSegmentationModel
                        onPicked: (id) => AppSettings.diarizationSegmentationModel = id
                    }

                    ModelComboBox {
                        id: embeddingCombo
                        Layout.fillWidth: true
                        Layout.preferredWidth: 1
                        model: mainPanel.transcriptionController ? mainPanel.transcriptionController.availableEmbeddingModels() : []
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
                        color: "#888888"
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
                    visible: mainPanel.orphanedMdPath === ""

                    Label {
                        text: qsTr("Sensitivity")
                        color: "#aaaaaa"
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
                        color: "#888888"
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
                readOnly: !mainPanel.hasStoredMetadata || mainPanel.transcribingThisFile
                wrapMode: TextArea.Wrap
                text: mainPanel.displayText
                color: "#dddddd"
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
                readonly property var gutterLineRects: {
                    const len = transcriptArea.length
                    // Same width/padding/font dependency gap as
                    // safePositionToRectangle above — positionToRectangle()
                    // is called directly here (not through that helper), so
                    // it needs its own explicit reads.
                    void transcriptArea.width
                    void transcriptArea.leftPadding
                    void transcriptArea.topPadding
                    void transcriptArea.font
                    return mainPanel.displaySentences.map(
                        s => s.start <= len ? transcriptArea.positionToRectangle(s.start) : Qt.rect(0, 0, 0, 0))
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

                    RowLayout {
                        id: chapterHeadingRow
                        objectName: "chapterHeadingRow"
                        required property var modelData
                        readonly property bool collapsed:
                            !!mainPanel.collapsedChapters[String(chapterHeadingRow.modelData.chapterT0)]
                        readonly property rect lineRect:
                            transcriptArea.gutterLineRects[chapterHeadingRow.modelData.idx] || Qt.rect(0, 0, 0, 0)
                        x: 0
                        y: lineRect.y
                        width: transcriptArea.width
                        height: lineRect.height
                        spacing: 8

                        // Opens the title for editing from an at-rest tap, with
                        // the caret at the click (clickX is in headingTitle's
                        // coordinates). The press can't place it itself —
                        // selectByMouse is false until this title IS the active
                        // edit — and selecting the whole title instead would
                        // make the next keystroke wipe it.
                        //
                        // ONLY the at-rest tap routes through here. Once
                        // editing, the field's own selectByMouse places the
                        // caret and drives drag-select natively; stepping in
                        // would clobber both. That's why headingTitle has to
                        // span the row (see its Layout note) rather than sit
                        // beside a tap-target strip: a strip can begin an edit,
                        // but it can't extend a selection.
                        function beginTitleEdit(clickX) {
                            if (headingTitle.editing)
                                return
                            mainPanel.editingChapterTs = chapterHeadingRow.modelData.chapterT0
                            headingTitle.forceActiveFocus()
                            headingTitle.cursorPosition =
                                headingTitle.positionAt(clickX, headingTitle.height / 2)
                        }

                        // Item 33: a dedicated disclosure triangle collapses/
                        // expands this chapter's body in the transcript
                        // (displayData drops the collapsed sentences). Kept
                        // SEPARATE from the title (which is click-to-edit, item
                        // 35) so the two gestures never collide — a triangle is
                        // the least ambiguous collapse affordance (task note).
                        AppIcon {
                            Layout.alignment: Qt.AlignVCenter
                            name: chapterHeadingRow.collapsed ? "chapter-expand" : "chapter-collapse"
                            iconSize: 10
                            color: "#8fdf9f"

                            HoverHandler { cursorShape: Qt.PointingHandCursor }
                            // ReleaseWithinBounds (not the default
                            // DragThreshold) on every tap target in this row:
                            // the row is an OVERLAY sitting on top of
                            // transcriptArea, and DragThreshold takes only a
                            // PASSIVE grab — the press falls straight through
                            // to the TextArea underneath, which focuses itself
                            // and drops a cursor on the heading's blank
                            // reserved line. An exclusive grab from press is
                            // what keeps a click on this row from also being a
                            // click in the transcript body.
                            TapHandler {
                                gesturePolicy: TapHandler.ReleaseWithinBounds
                                onTapped: mainPanel.toggleChapterCollapse(chapterHeadingRow.modelData.chapterT0)
                            }
                        }
                        // Item 35: the transcript heading is now click-to-edit
                        // too, sharing the SAME single-active-edit state
                        // (editingChapterTs, keyed by the real chapterT0) as
                        // the TOC row, so editing one exits the other. Commits
                        // through setChapterTitle via commitChapterTitle().
                        TextEdit {
                            id: headingTitle
                            objectName: "chapterHeadingTitle"
                            readonly property bool editing:
                                mainPanel.editingChapterTs === chapterHeadingRow.modelData.chapterT0
                            // Spans the whole row right of the triangle, not
                            // just its text: everything to the right of the
                            // title still LOOKS like the heading line, and a
                            // click or drag out there has to be a real click in
                            // THIS field (caret to the end of the title, native
                            // drag-select back through it). A sibling strip
                            // holding the rule can't do that — its tap grabs the
                            // press, so a click past the last character while
                            // editing did nothing and a drag from out there
                            // selected nothing. Filling the width also keeps the
                            // press off transcriptArea underneath, which is what
                            // the strip was there for.
                            Layout.fillWidth: true
                            Layout.alignment: Qt.AlignVCenter
                            readOnly: !headingTitle.editing
                            selectByMouse: headingTitle.editing
                            selectByKeyboard: headingTitle.editing
                            activeFocusOnPress: false
                            text: chapterHeadingRow.modelData.chapterTitle
                            color: "#8fdf9f"
                            font.pixelSize: 12
                            font.bold: true
                            // The SAME selection colour transcriptArea uses (see
                            // its selectionColor) — a title swept by a transcript
                            // selection (sweptBySelection below) has to read as
                            // part of that one selection, not a separate widget
                            // lighting up in the Basic palette's opaque navy.
                            selectionColor: Qt.rgba(0.35, 0.55, 0.85, 0.5)

                            // A drag through the transcript can't natively
                            // select this title (it's a separate document — see
                            // selectionSweepsHeading), so when the transcript's
                            // selection sweeps across this heading's line, mirror
                            // it here: the title highlights along with the body
                            // around it, and copySelectionWithChapters() splices
                            // the same titles into the clipboard, keyed off the
                            // same predicate so what you copy is what you saw
                            // highlighted. All-or-nothing per title — the line it
                            // reserves is zero-length, so there's no sub-title
                            // offset a transcript selection could even land on.
                            //
                            // Never while editing: this title's own selection is
                            // the user's then, and stomping it would undo their
                            // drag-select mid-gesture.
                            readonly property bool sweptBySelection:
                                !headingTitle.editing
                                && mainPanel.selectionSweepsHeading(chapterHeadingRow.modelData.start)
                            onSweptBySelectionChanged: {
                                if (headingTitle.sweptBySelection)
                                    headingTitle.select(0, headingTitle.length)
                                else
                                    headingTitle.deselect()
                            }

                            HoverHandler { cursorShape: Qt.IBeamCursor }
                            // Only the tapped instance focuses itself (see the
                            // TOC title's twin handler for why).
                            // Default (DragThreshold/passive) gesturePolicy
                            // here, unlike this row's other tap targets: a
                            // TextEdit accepts the press ITSELF, so nothing
                            // falls through to transcriptArea anyway, and an
                            // exclusive grab would swallow the presses that
                            // place the cursor / drag-select once this title
                            // IS being edited.
                            TapHandler {
                                onTapped: (eventPoint) => chapterHeadingRow.beginTitleEdit(eventPoint.position.x)
                            }
                            onEditingChanged: {
                                if (!headingTitle.editing && headingTitle.activeFocus)
                                    headingTitle.focus = false
                            }
                            onActiveFocusChanged: {
                                if (!headingTitle.activeFocus) {
                                    mainPanel.commitChapterTitle(chapterHeadingRow.modelData.chapterT0,
                                                                 chapterHeadingRow.modelData.chapterTitle,
                                                                 headingTitle.text)
                                    if (mainPanel.editingChapterTs === chapterHeadingRow.modelData.chapterT0)
                                        mainPanel.editingChapterTs = -1
                                }
                            }
                            Keys.onReturnPressed: (event) => {
                                event.accepted = true
                                mainPanel.editingChapterTs = -1
                            }
                            Keys.onEnterPressed: (event) => {
                                event.accepted = true
                                mainPanel.editingChapterTs = -1
                            }
                            Keys.onEscapePressed: (event) => {
                                event.accepted = true
                                headingTitle.text = chapterHeadingRow.modelData.chapterTitle
                                mainPanel.editingChapterTs = -1
                            }

                            // The rule filling the heading line to the right of
                            // the title. A CHILD of the field, offset by
                            // contentWidth (the text's real width), rather than
                            // a sibling laid out beside it — the field has to
                            // own that space for a click/drag past the last
                            // character to reach the text, so the rule has to
                            // live inside it. It's a bare Rectangle: it accepts
                            // no mouse of its own, and presses land on the
                            // TextEdit behind it, which is exactly what we want.
                            Rectangle {
                                x: headingTitle.contentWidth + 8
                                width: Math.max(0, headingTitle.width - x)
                                anchors.verticalCenter: parent.verticalCenter
                                height: 1
                                color: "#3a3a3a"
                            }
                        }
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

            } // end scrollableContent ColumnLayout
        } // end transcriptScrollView

        // Item 55: loading overlay shown from the instant THIS file's
        // (re)transcription starts until the first new segment lands (see
        // showTranscribingOverlay / awaitingFirstSegment). A dim scrim over
        // the (already dimmed + disabled) stale transcript plus a centered
        // BusyIndicator + a task label (transcribingOverlayLabel, keyed off
        // currentPhase so it names the actual task). It also swallows pointer
        // input (TapHandler for clicks/drags, HoverHandler for the wait
        // cursor) so nothing underneath reacts while the old transcript is
        // frozen. Sibling of transcriptScrollView inside
        // transcriptViewportContainer, so it's scoped to exactly the
        // transcript region (not the sticky header). Reuses the built-in
        // QQC2 BusyIndicator (the app had no existing spinner component) so
        // it inherits the app's control style.
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
                    color: "#dddddd"
                    font.pixelSize: 13
                }
            }
        }

        // Chrome/Firefox-style middle-click autoscroll for the transcript
        // view — see MiddleDragScroll.qml for the whole design (why it's an
        // Item and not a MouseArea, why arming is panel-scoped but the armed
        // phase is window-wide, which gesturePolicy and why).
        //
        // Declared as an overlay sibling of transcriptScrollView (not inside
        // it, see transcriptViewportContainer's doc comment) so the ScrollView
        // keeps exactly one flickable content child — but VISUALLY reparented
        // to mainPanel so a middle-click anywhere on the note page arms it,
        // not just one inside the transcript box. That reparent moves it out
        // of the note page's ColumnLayout, so it also needs its own `visible`
        // gate (it no longer inherits the note page's) — inert on the
        // recorder/USB/empty pages, where there's no transcript to pan.
        MiddleDragScroll {
            id: middleDragScrollArea
            parent: mainPanel
            anchors.fill: parent
            visible: mainPanel.vaultPath.toString() !== "" && !mainPanel.recording
                     && !mainPanel.usbGrabberOpen && !mainPanel.benchmarkOpen
            flickable: transcriptScrollView.contentItem as Flickable
        }

        } // end transcriptViewportContainer
    }

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
        color: "#dddddd"
        font.pixelSize: 11
        padding: 4
        background: Rectangle {
            radius: AppSettings.borderRadius
            color: "#2f4f5f"
            border.width: 1
            border.color: "#6fdf9f"
        }
    }

    // Recorder page: sibling of the two content blocks above (and of
    // usbGrabberPage below), gated on mainPanel.recording (toggled by
    // TitleBar's record button, see Main.qml's onRecordRequested). Exactly
    // one of these four top-level blocks is ever visible at once.
    Item {
        id: recorderPage
        anchors.fill: parent
        visible: mainPanel.recording

        property bool useMic: true
        property bool useSystemAudio: false
        property string selectedMicId: ""
        property var micDevices: []
        // Backing string for the shared warning banner below — cleared
        // whenever the folder picker succeeds/reopens, or whenever
        // audioRecorder.lastError clears (see the Connections block below).
        property string recordingFolderError: ""
        // Recent RMS samples for the scrolling waveform Canvas, oldest
        // first, capped at ~100 (see waveformCanvas below). AppSettings.
        // animationsEnabled off collapses this to a single-element "static
        // level" array instead of a scrolling history (CLAUDE.md/this
        // app's convention: that flag kills continuous-update CPU/GPU work).
        property var levelSamples: []

        function refreshMicDevices() {
            recorderPage.micDevices = mainPanel.audioRecorder ? mainPanel.audioRecorder.availableMicDevices() : []
            if (recorderPage.selectedMicId === "" && recorderPage.micDevices.length > 0)
                recorderPage.selectedMicId = recorderPage.micDevices[0].id
        }

        onVisibleChanged: if (recorderPage.visible) recorderPage.refreshMicDevices()
        Component.onCompleted: recorderPage.refreshMicDevices()

        // audioRecorder.lastError clearing (a fresh startRecording() call
        // clears it server-side) should also clear the shared banner if
        // it was the error currently showing.
        Connections {
            target: mainPanel.audioRecorder
            function onLevelUpdated() {
                if (AppSettings.animationsEnabled) {
                    const samples = recorderPage.levelSamples.slice()
                    samples.push(mainPanel.audioRecorder.currentLevel)
                    if (samples.length > 100)
                        samples.shift()
                    recorderPage.levelSamples = samples
                } else {
                    recorderPage.levelSamples = [mainPanel.audioRecorder.currentLevel]
                }
                waveformCanvas.requestPaint()
            }
        }

        // No vault open: don't let the user configure mic/folder pickers
        // with nothing to record into. Same "Open Vault" button precedent
        // as the empty-vault block above.
        Item {
            anchors.fill: parent
            visible: mainPanel.vaultPath.toString() === ""

            Button {
                id: recorderOpenVaultButton
                anchors.centerIn: parent
                text: qsTr("Open Vault")
                hoverEnabled: true
                onClicked: mainPanel.openVaultRequested()

                background: Rectangle {
                    radius: AppSettings.borderRadius
                    color: (recorderOpenVaultButton.hovered && recorderOpenVaultButton.enabled) ? "#3a3a3a" : "#2a2a2a"
                    border.width: 1
                    border.color: "#3a3a3a"
                }
                HoverHandler {
                    cursorShape: Qt.PointingHandCursor
                }
                HoverBorder {
                    anchors.fill: parent
                    active: recorderOpenVaultButton.hovered
                }
            }
        }

        ColumnLayout {
            id: recorderContent
            anchors.fill: parent
            anchors.margins: 16
            spacing: 12
            visible: mainPanel.vaultPath.toString() !== ""

            // Microphone row: toggle + device combo + gain slider.
            RowLayout {
                Layout.fillWidth: true
                spacing: 12

                Item {
                    id: micToggle
                    Layout.preferredWidth: micToggleLabel.implicitWidth + 16
                    Layout.preferredHeight: 24

                    HoverHandler {
                        id: micToggleHover
                        cursorShape: Qt.PointingHandCursor
                    }
                    TapHandler {
                        onTapped: recorderPage.useMic = !recorderPage.useMic
                    }
                    Rectangle {
                        anchors.fill: parent
                        radius: AppSettings.borderRadius
                        color: recorderPage.useMic
                               ? (micToggleHover.hovered ? "#2ecc71" : "#25a25a")
                               : (micToggleHover.hovered ? "#3a3a3a" : "#2a2a2a")
                        border.width: 1
                        border.color: recorderPage.useMic ? "#2ecc71" : "#4a4a4a"

                        Behavior on color { ColorAnimation { duration: AppSettings.animationsEnabled ? 100 : 0 } }
                    }
                    Label {
                        id: micToggleLabel
                        anchors.centerIn: parent
                        text: qsTr("Microphone")
                        color: recorderPage.useMic ? "white" : "#aaaaaa"
                        font.pixelSize: 11
                    }
                }

                ComboBox {
                    id: micCombo
                    Layout.preferredWidth: 220
                    enabled: recorderPage.useMic
                    hoverEnabled: true
                    textRole: "name"
                    valueRole: "id"
                    model: recorderPage.micDevices
                    onActivated: recorderPage.selectedMicId = micCombo.currentValue

                    HoverHandler {
                        cursorShape: Qt.PointingHandCursor
                        enabled: micCombo.enabled
                    }
                    HoverBorder {
                        anchors.fill: parent
                        active: micCombo.enabled && micCombo.hovered
                    }
                }

                Slider {
                    id: micGainSlider
                    Layout.fillWidth: true
                    enabled: recorderPage.useMic
                    hoverEnabled: true
                    from: 0.0
                    to: 2.0
                    value: AppSettings.recordingMicGain
                    onMoved: AppSettings.recordingMicGain = value

                    HoverHandler {
                        cursorShape: Qt.PointingHandCursor
                        enabled: micGainSlider.enabled
                    }
                    HoverBorder {
                        anchors.fill: parent
                        active: micGainSlider.enabled && micGainSlider.hovered
                    }
                }
            }

            // System audio row: toggle (parec's default-sink monitor, "all
            // windows" mix — see AudioRecorder.h) + its own gain slider.
            RowLayout {
                Layout.fillWidth: true
                spacing: 12

                Item {
                    id: systemAudioToggle
                    Layout.preferredWidth: systemAudioToggleLabel.implicitWidth + 16
                    Layout.preferredHeight: 24

                    HoverHandler {
                        id: systemAudioToggleHover
                        cursorShape: Qt.PointingHandCursor
                    }
                    TapHandler {
                        onTapped: recorderPage.useSystemAudio = !recorderPage.useSystemAudio
                    }
                    Rectangle {
                        anchors.fill: parent
                        radius: AppSettings.borderRadius
                        color: recorderPage.useSystemAudio
                               ? (systemAudioToggleHover.hovered ? "#2ecc71" : "#25a25a")
                               : (systemAudioToggleHover.hovered ? "#3a3a3a" : "#2a2a2a")
                        border.width: 1
                        border.color: recorderPage.useSystemAudio ? "#2ecc71" : "#4a4a4a"

                        Behavior on color { ColorAnimation { duration: AppSettings.animationsEnabled ? 100 : 0 } }
                    }
                    Label {
                        id: systemAudioToggleLabel
                        anchors.centerIn: parent
                        text: qsTr("System Audio")
                        color: recorderPage.useSystemAudio ? "white" : "#aaaaaa"
                        font.pixelSize: 11
                    }
                }

                Slider {
                    id: systemGainSlider
                    Layout.fillWidth: true
                    enabled: recorderPage.useSystemAudio
                    hoverEnabled: true
                    from: 0.0
                    to: 2.0
                    value: AppSettings.recordingSystemGain
                    onMoved: AppSettings.recordingSystemGain = value

                    HoverHandler {
                        cursorShape: Qt.PointingHandCursor
                        enabled: systemGainSlider.enabled
                    }
                    HoverBorder {
                        anchors.fill: parent
                        active: systemGainSlider.enabled && systemGainSlider.hovered
                    }
                }
            }

            // Start/Stop + disable-animations + duration counter row.
            RowLayout {
                Layout.fillWidth: true
                spacing: 12

                Item {
                    id: recordToggleButton
                    Layout.preferredWidth: 32
                    Layout.preferredHeight: 32
                    enabled: recorderPage.useMic || recorderPage.useSystemAudio || mainPanel.audioRecorder.recording

                    HoverHandler {
                        id: recordToggleHover
                        cursorShape: Qt.PointingHandCursor
                        enabled: recordToggleButton.enabled
                    }
                    TapHandler {
                        enabled: recordToggleButton.enabled
                        onTapped: {
                            if (mainPanel.audioRecorder.recording) {
                                mainPanel.audioRecorder.stopRecording()
                            } else {
                                recorderPage.recordingFolderError = ""
                                mainPanel.audioRecorder.startRecording(
                                    mainPanel.vaultPath, AppSettings.recordingOutputFolder,
                                    recorderPage.useMic, recorderPage.selectedMicId,
                                    recorderPage.useSystemAudio, AppSettings.recordingMicGain,
                                    AppSettings.recordingSystemGain)
                            }
                        }
                    }
                    Rectangle {
                        anchors.fill: parent
                        radius: AppSettings.borderRadius
                        color: (recordToggleHover.hovered && recordToggleButton.enabled) ? "#3a3a3a" : "#2a2a2a"
                    }
                    HoverBorder {
                        anchors.fill: parent
                        active: recordToggleButton.enabled && recordToggleHover.hovered
                    }
                    ToolTip.visible: recordToggleHover.hovered
                    ToolTip.text: mainPanel.audioRecorder.recording ? qsTr("Stop recording") : qsTr("Start recording")
                    ToolTip.delay: 500
                    AppIcon {
                        anchors.centerIn: parent
                        name: mainPanel.audioRecorder.recording ? "stop" : "record"
                        iconSize: 16
                        color: mainPanel.audioRecorder.recording ? "#e07a7a" : "#6fdf9f"
                    }
                }

                // Global animations/blur kill switch — placed here per the
                // user's explicit placement decision, but affects the whole
                // app (every existing Behavior/compositor blur already gates
                // on AppSettings.animationsEnabled), not just this page.
                Item {
                    id: animationsToggleButton
                    Layout.preferredWidth: 32
                    Layout.preferredHeight: 32

                    HoverHandler {
                        id: animationsToggleHover
                        cursorShape: Qt.PointingHandCursor
                    }
                    TapHandler {
                        onTapped: AppSettings.animationsEnabled = !AppSettings.animationsEnabled
                    }
                    Rectangle {
                        anchors.fill: parent
                        radius: AppSettings.borderRadius
                        color: animationsToggleHover.hovered ? "#3a3a3a" : "#2a2a2a"
                    }
                    HoverBorder {
                        anchors.fill: parent
                        active: animationsToggleHover.hovered
                    }
                    ToolTip.visible: animationsToggleHover.hovered
                    ToolTip.text: AppSettings.animationsEnabled ? qsTr("Disable animations") : qsTr("Enable animations")
                    ToolTip.delay: 500
                    AppIcon {
                        anchors.centerIn: parent
                        name: "freeze"
                        iconSize: 15
                        color: AppSettings.animationsEnabled ? "#dddddd" : "#6fdf9f"
                    }
                }

                Label {
                    id: recordingDurationLabel
                    text: mainPanel.formatDuration(mainPanel.audioRecorder ? mainPanel.audioRecorder.elapsedMs : 0)
                    color: "white"
                    font.pixelSize: 16
                }

                Item { Layout.fillWidth: true }

                // Output folder picker.
                Item {
                    id: changeFolderButton
                    Layout.preferredWidth: 32
                    Layout.preferredHeight: 32

                    HoverHandler {
                        id: changeFolderHover
                        cursorShape: Qt.PointingHandCursor
                    }
                    TapHandler {
                        onTapped: {
                            const sub = AppSettings.recordingOutputFolder
                            recordingFolderDialog.currentFolder =
                                "file://" + mainPanel.vaultLocalPath + (sub ? "/" + sub : "")
                            recordingFolderDialog.open()
                        }
                    }
                    Rectangle {
                        anchors.fill: parent
                        radius: AppSettings.borderRadius
                        color: changeFolderHover.hovered ? "#3a3a3a" : "#2a2a2a"
                    }
                    HoverBorder {
                        anchors.fill: parent
                        active: changeFolderHover.hovered
                    }
                    ToolTip.visible: changeFolderHover.hovered
                    ToolTip.text: qsTr("Change output folder")
                    ToolTip.delay: 500
                    AppIcon {
                        anchors.centerIn: parent
                        name: "folder"
                        iconSize: 15
                        color: "#dddddd"
                    }
                }
            }

            // Shared warning banner (StyleGuide.md "Warning banners"): shows
            // whichever of the folder-picker rejection / audioRecorder's own
            // lastError is currently non-empty. No dismiss button — clears
            // itself the next time the folder picker succeeds/reopens, or
            // the next time lastErrorChanged reports an empty string.
            Label {
                Layout.fillWidth: true
                visible: text !== ""
                text: recorderPage.recordingFolderError !== "" ? recorderPage.recordingFolderError
                      : (mainPanel.audioRecorder ? mainPanel.audioRecorder.lastError : "")
                color: "#e07a7a"
                font.pixelSize: 11
                wrapMode: Text.WordWrap
            }

            // Scrolling waveform/level indicator — mirrors playPauseIcon's
            // Canvas idiom above (getContext("2d")/onPaint), fed from
            // recorderPage.levelSamples (pushed on each throttled
            // levelUpdated tick, see the Connections block above).
            Canvas {
                id: waveformCanvas
                Layout.fillWidth: true
                Layout.preferredHeight: 80

                onWidthChanged: requestPaint()
                onHeightChanged: requestPaint()

                onPaint: {
                    const ctx = getContext("2d")
                    ctx.reset()
                    ctx.fillStyle = "#1a1a1a"
                    ctx.fillRect(0, 0, width, height)

                    const samples = recorderPage.levelSamples
                    if (samples.length === 0)
                        return

                    ctx.strokeStyle = "#6fdf9f"
                    ctx.lineWidth = 2

                    if (!AppSettings.animationsEnabled) {
                        // Static flat line at the last-known level — no
                        // scrolling history while animations are off.
                        const level = samples[samples.length - 1]
                        const y = height - level * height
                        ctx.beginPath()
                        ctx.moveTo(0, y)
                        ctx.lineTo(width, y)
                        ctx.stroke()
                        return
                    }

                    const maxSamples = 100
                    const stepX = width / Math.max(1, maxSamples - 1)
                    ctx.beginPath()
                    for (let i = 0; i < samples.length; i++) {
                        const x = width - (samples.length - i) * stepX
                        const y = height - samples[i] * height
                        if (i === 0)
                            ctx.moveTo(x, y)
                        else
                            ctx.lineTo(x, y)
                    }
                    ctx.stroke()
                }
            }
        }
    }

    FolderDialog {
        id: recordingFolderDialog
        onAccepted: {
            const localPath = recordingFolderDialog.selectedFolder.toString().replace("file://", "")
            if (!mainPanel.vaultTreeModel || !mainPanel.vaultTreeModel.isPathInsideVault(localPath)) {
                recorderPage.recordingFolderError = qsTr("That folder is outside the vault — pick a location inside it.")
                return
            }
            recorderPage.recordingFolderError = ""
            const prefix = mainPanel.vaultLocalPath + "/"
            AppSettings.recordingOutputFolder = localPath === mainPanel.vaultLocalPath ? ""
                : (localPath.startsWith(prefix) ? localPath.slice(prefix.length) : localPath)
        }
    }

    // Benchmark page: another sibling top-level block (see the mutual-exclusion
    // negations on the content/recorder/usb blocks). Promoted out of Settings to
    // its own TitleBar button (see Main.qml's onBenchmarkRequested). Given lower
    // precedence than recording/usbGrabber, same as those negate each other.
    ScrollView {
        id: benchmarkScroll
        anchors.fill: parent
        anchors.margins: 16
        visible: mainPanel.benchmarkOpen && !mainPanel.recording && !mainPanel.usbGrabberOpen
        contentWidth: availableWidth
        clip: true

        BenchmarkPage {
            width: benchmarkScroll.availableWidth
            active: benchmarkScroll.visible
            transcriptionController: mainPanel.transcriptionController
            vaultTreeModel: mainPanel.vaultTreeModel
            // B2: default the benchmark to whatever file is open/highlighted in
            // the left panel (the same "current file" expression Main.qml feeds
            // the sidebar); empty ⇒ BenchmarkPage falls back to its picker.
            currentFilePath: (mainPanel.player && mainPanel.player.source.toString() !== "")
                             ? mainPanel.player.source.toString().replace("file://", "")
                             : (mainPanel.orphanedMdPath !== "" ? mainPanel.orphanedMdPath : "")
        }
    }

    // USB grabber page: sibling of the three content blocks above, gated on
    // mainPanel.usbGrabberOpen (toggled by TitleBar's USB grabber button,
    // see Main.qml's onUsbGrabberRequested) — also hidden while recording,
    // for full mutual exclusivity across all four top-level blocks.
    Item {
        id: usbGrabberPage
        anchors.fill: parent
        visible: mainPanel.usbGrabberOpen && !mainPanel.recording

        // Raw snapshots from UsbGrabberController — re-pulled by
        // refreshDevices()/refreshHistory() rather than bound live, since the
        // controller exposes plain Q_INVOKABLE getters (no per-device
        // changed signals to bind against). deviceRows below derives the
        // actual per-row display model from these two plus a fresh
        // ruleForDevice() read per device, so it stays current every time
        // either snapshot is refreshed.
        property var mountedDevices: []
        property var savedRules: []
        property var catchAllRule: ({enabled: false, extensionFilter: [], filenameRegex: "", preserveStructure: true, startFolder: "", deviceLabel: "", deviceSerial: "*", destinationFolder: ""})
        property var historyEntries: []

        function refreshDevices() {
            if (!mainPanel.usbGrabberController) {
                usbGrabberPage.mountedDevices = []
                usbGrabberPage.savedRules = []
                usbGrabberPage.catchAllRule = ({enabled: false, extensionFilter: [], filenameRegex: "", preserveStructure: true, startFolder: "", deviceLabel: "", deviceSerial: "*", destinationFolder: ""})
                return
            }
            usbGrabberPage.mountedDevices = mainPanel.usbGrabberController.mountedDevices()
            usbGrabberPage.savedRules = mainPanel.usbGrabberController.allRules()
            usbGrabberPage.catchAllRule = mainPanel.usbGrabberController.ruleForDevice("*")
        }

        // Newest first — the controller returns history in file-append
        // (oldest-first) order.
        function refreshHistory() {
            usbGrabberPage.historyEntries = mainPanel.usbGrabberController
                ? mainPanel.usbGrabberController.history().slice().reverse() : []
        }

        function formatTimestamp(iso) {
            const d = new Date(iso)
            return isNaN(d.getTime()) ? iso : Qt.formatDateTime(d, "yyyy-MM-dd hh:mm")
        }

        // Grab preview: which device's preview is currently shown (only one
        // at a time, rendered inline under that device's own card — see the
        // Repeater delegate below), its raw {path,status} entries once
        // requestGrabPreview()'s async scan completes, and a running/error
        // flag for the interim "Scanning…" state. Cleared/reset on every new
        // Preview click, not on every refreshDevices() poll tick, so an
        // in-flight or just-finished preview survives the 2.5s device poll.
        property string previewDeviceSerial: ""
        property var previewEntries: []
        property bool previewRunning: false
        property string previewError: ""

        // Destination-folder picker rejection (see usbDestinationFolderDialog
        // below) — StyleGuide.md's warning-banner convention (inline Label,
        // driven by a non-empty backing string, no dismiss button).
        property string destinationFolderError: ""

        function requestPreview(serial) {
            if (!mainPanel.usbGrabberController)
                return
            usbGrabberPage.previewDeviceSerial = serial
            usbGrabberPage.previewEntries = []
            usbGrabberPage.previewError = ""
            usbGrabberPage.previewRunning = true
            mainPanel.usbGrabberController.requestGrabPreview(serial)
        }

        // Put the preview panel away. Clearing previewDeviceSerial is all it
        // takes even mid-scan: onGrabPreviewReady discards any result whose
        // serial no longer matches, so an in-flight walk simply lands nowhere
        // (it copies nothing and writes nothing — there is no side effect to
        // undo, only a panel to close). Nothing else re-shows the panel; only
        // another Preview click does.
        function dismissPreview() {
            usbGrabberPage.previewDeviceSerial = ""
            usbGrabberPage.previewEntries = []
            usbGrabberPage.previewError = ""
            usbGrabberPage.previewRunning = false
        }

        // Same three-state palette across color/label/summary — "will grab"
        // reuses the same green the "connected" status label already uses
        // (see deviceCard's mounted Label below), so the preview's meaning
        // ("this device is eligible and this file will move") reads
        // consistently with the rest of the page.
        function previewStatusColor(status) {
            switch (status) {
            case "grab": return "#7ac07a"
            case "duplicate": return "#888888"
            case "filtered": return "#666666"
            default: return "#888888"
            }
        }

        function previewStatusLabel(status) {
            switch (status) {
            case "grab": return qsTr("will grab")
            case "duplicate": return qsTr("skip — already grabbed")
            case "filtered": return qsTr("skip — filtered out")
            default: return status
            }
        }

        function previewSummary(entries) {
            let grabCount = 0, dupCount = 0, filteredCount = 0
            for (const e of entries) {
                if (e.status === "grab") ++grabCount
                else if (e.status === "duplicate") ++dupCount
                else if (e.status === "filtered") ++filteredCount
            }
            return qsTr("%1 will be grabbed, %2 already grabbed, %3 filtered out")
                    .arg(grabCount).arg(dupCount).arg(filteredCount)
        }

        function escapePreviewHtml(s) {
            return String(s).replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;")
        }

        function previewHtml(entries) {
            return entries.map(function (e) {
                return "<span style=\"color:" + usbGrabberPage.previewStatusColor(e.status) + ";\">"
                        + usbGrabberPage.previewStatusLabel(e.status) + " — "
                        + usbGrabberPage.escapePreviewHtml(e.path) + "</span>"
            }).join("<br>")
        }

        // Resolves a FolderDialog's selectedFolder into a vault-relative
        // string suitable for a rule's destinationFolder field ("" = vault
        // root), or null if the chosen folder is outside the vault — same
        // shape as recordingFolderDialog's onAccepted below.
        function relativeToVault(localPath) {
            if (!mainPanel.vaultTreeModel || !mainPanel.vaultTreeModel.isPathInsideVault(localPath))
                return null
            const prefix = mainPanel.vaultLocalPath + "/"
            return localPath === mainPanel.vaultLocalPath ? ""
                : (localPath.startsWith(prefix) ? localPath.slice(prefix.length) : localPath)
        }

        // Shared by every per-field commit below: re-reads the device's
        // current rule (so fields this call doesn't touch are preserved),
        // applies one field, saves, then re-derives deviceRows via
        // refreshDevices() so the row reflects what was actually persisted.
        //
        // If this device's preview is currently on screen, it is now describing
        // a rule that no longer exists — re-run it so the filename regex (and
        // every other field: a destination change re-decides what counts as a
        // duplicate, an extension change re-decides what's filtered) is
        // previewed as edited, the way the combine rules' preview follows their
        // pattern field. Deliberately NOT per keystroke, unlike the validity
        // indicator beside the field: a preview is an async walk that stats
        // every file on a physical device (DESIGN.md "USB import"), so it's
        // driven by an actual commit and skipped entirely when the value didn't
        // change — onEditingFinished fires on every focus-out, edited or not.
        // requestPreview() re-reads the SAVED rule, so this must run after
        // saveRule() rather than against the in-hand value.
        function commitRuleField(serial, label, field, value) {
            if (!mainPanel.usbGrabberController)
                return
            const rule = mainPanel.usbGrabberController.ruleForDevice(serial)
            const changed = JSON.stringify(rule[field]) !== JSON.stringify(value)
            rule[field] = value
            if (label)
                rule.deviceLabel = label
            mainPanel.usbGrabberController.saveRule(rule)
            usbGrabberPage.refreshDevices()
            // A "*" edit also re-runs it: an on-screen preview may be resolving
            // through the catch-all rule right now (a device whose own rule is
            // disabled falls back to it — see resolveEffectiveRule()), and from
            // here there's no way to tell whether it is, so re-run rather than
            // leave a stale preview standing. Re-previewing a device that
            // wasn't using the catch-all after all just reproduces what's
            // already shown.
            if (changed && usbGrabberPage.previewDeviceSerial !== ""
                    && (usbGrabberPage.previewDeviceSerial === serial || serial === "*"))
                usbGrabberPage.requestPreview(usbGrabberPage.previewDeviceSerial)
        }

        // Union of currently-mounted devices and previously-configured ones
        // (from savedRules, excluding the "*" catch-all — that has its own
        // dedicated card below), GROUPED by serial, each row carrying a
        // fresh ruleForDevice() read so toggling/editing one field doesn't
        // require a full round-trip before the row reflects it.
        //
        // One row per physical device, not per volume: rules are keyed by
        // serial, so a device's volumes necessarily share one rule and showing
        // them as separate rows would render one rule as several independently-
        // toggleable-looking switches. mountedDevices is per-VOLUME though (see
        // UsbDeviceMonitor::currentlyMountedDevices()), so volumes sharing a
        // serial are collected into mountPaths rather than overwriting each
        // other — the row's volume count comes from that, and "Grab now" grabs
        // all of them via triggerManualGrab().
        property var deviceRows: {
            if (!mainPanel.usbGrabberController)
                return []
            const bySerial = {}
            for (const d of usbGrabberPage.mountedDevices) {
                const existing = bySerial[d.serial]
                if (existing) {
                    existing.mountPaths.push(d.mountPath)
                    continue
                }
                const label = ((d.vendor || "") + " " + (d.model || "")).trim()
                bySerial[d.serial] = {serial: d.serial, label: label.length > 0 ? label : d.serial,
                                       mounted: true, mountPaths: [d.mountPath]}
            }
            for (const r of usbGrabberPage.savedRules) {
                if (r.deviceSerial === "*" || bySerial[r.deviceSerial])
                    continue
                bySerial[r.deviceSerial] = {serial: r.deviceSerial, label: r.deviceLabel || r.deviceSerial,
                                             mounted: false, mountPaths: []}
            }
            const rows = []
            for (const serial in bySerial) {
                const row = bySerial[serial]
                const rule = mainPanel.usbGrabberController.ruleForDevice(serial)
                row.enabled = rule.enabled
                row.autoGrab = rule.autoGrab
                row.extensionFilterText = (rule.extensionFilter || []).join(", ")
                row.filenameRegex = rule.filenameRegex
                row.preserveStructure = rule.preserveStructure
                row.startFolder = rule.startFolder
                row.destinationFolder = rule.destinationFolder
                rows.push(row)
            }
            return rows
        }

        onVisibleChanged: if (usbGrabberPage.visible) { usbGrabberPage.refreshDevices(); usbGrabberPage.refreshHistory() }
        Component.onCompleted: { usbGrabberPage.refreshDevices(); usbGrabberPage.refreshHistory() }

        // No live deviceAdded/deviceRemoved signal is exposed on
        // UsbGrabberController itself (only the underlying UsbDeviceMonitor
        // has those) — polling is simplest for a low-frequency, non-
        // performance-sensitive utility page like this one, and only runs
        // while the page is actually visible.
        Timer {
            interval: 2500
            running: usbGrabberPage.visible
            repeat: true
            onTriggered: usbGrabberPage.refreshDevices()
        }

        // A grab (manual or hotplug-triggered) just finished — refresh both
        // the history list and the device rows (a hotplug grab may have
        // introduced a device this page hasn't seen before).
        Connections {
            target: mainPanel.usbGrabberController
            function onGrabFinished() { usbGrabberPage.refreshHistory(); usbGrabberPage.refreshDevices() }
            function onGrabPreviewReady(serial, entries, error) {
                if (serial !== usbGrabberPage.previewDeviceSerial)
                    return
                usbGrabberPage.previewRunning = false
                usbGrabberPage.previewEntries = entries
                usbGrabberPage.previewError = error
            }
        }

        ScrollView {
            id: usbScrollArea
            anchors.fill: parent
            anchors.margins: 16
            clip: true
            ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

            ColumnLayout {
                // See SettingsPopup.qml's contentColumn doc comment for why
                // this is bound to the ScrollView's own width (minus the
                // overlay scrollbar's clearance) rather than parent.width.
                width: usbScrollArea.width - 14
                spacing: 16

                Label {
                    text: qsTr("USB Grabber")
                    color: "white"
                    font.bold: true
                    font.pixelSize: 16
                }

                Label {
                    Layout.fillWidth: true
                    visible: mainPanel.usbGrabberController && mainPanel.usbGrabberController.lastError !== ""
                    text: mainPanel.usbGrabberController ? mainPanel.usbGrabberController.lastError : ""
                    color: "#e07a7a"
                    font.pixelSize: 11
                    wrapMode: Text.WordWrap
                }

                Label {
                    Layout.fillWidth: true
                    visible: usbGrabberPage.destinationFolderError !== ""
                    text: usbGrabberPage.destinationFolderError
                    color: "#e07a7a"
                    font.pixelSize: 11
                    wrapMode: Text.WordWrap
                }

                // "Any device" catch-all rule card — same shape as a
                // per-device card below, minus the mounted/grab-now concept
                // (it isn't tied to one physical device).
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 4

                    Rectangle {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 1
                        color: "#3a3a3a"
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        Label {
                            Layout.fillWidth: true
                            text: qsTr("Any USB device")
                            color: "white"
                        }
                        // Auto-grab is strictly narrower than enabled (see
                        // UsbGrabRule::autoGrab) — meaningless on a device that
                        // can't be grabbed at all, so it's disabled until the
                        // rule is on rather than sitting there as a live switch
                        // that quietly does nothing.
                        Switch {
                            id: catchAllAutoGrabSwitch
                            text: qsTr("Auto-grab")
                            enabled: usbGrabberPage.catchAllRule.enabled
                            checked: usbGrabberPage.catchAllRule.autoGrab || false
                            hoverEnabled: true
                            onToggled: usbGrabberPage.commitRuleField("*", "", "autoGrab", checked)
                            HoverHandler { cursorShape: Qt.PointingHandCursor; enabled: catchAllAutoGrabSwitch.enabled }
                            HoverBorder {
                                anchors.fill: parent
                                active: catchAllAutoGrabSwitch.enabled && catchAllAutoGrabSwitch.hovered
                            }
                            ToolTip.visible: hovered
                            ToolTip.delay: 500
                            ToolTip.text: qsTr("Grab from any matching device as soon as it's plugged in, without asking")
                        }
                        Switch {
                            id: catchAllEnabledSwitch
                            checked: usbGrabberPage.catchAllRule.enabled
                            hoverEnabled: true
                            onToggled: usbGrabberPage.commitRuleField("*", "", "enabled", checked)
                            HoverHandler { cursorShape: Qt.PointingHandCursor }
                            HoverBorder { anchors.fill: parent; active: catchAllEnabledSwitch.hovered }
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        TextField {
                            id: catchAllExtensionField
                            Layout.fillWidth: true
                            placeholderText: qsTr("Extensions (comma-separated, blank = all files)")
                            text: (usbGrabberPage.catchAllRule.extensionFilter || []).join(", ")
                            background: UnderlineField { focused: catchAllExtensionField.activeFocus }
                            onEditingFinished: usbGrabberPage.commitRuleField("*", "", "extensionFilter",
                                text.split(",").map(s => s.trim()).filter(s => s.length > 0))
                            // Item 13: editable text needs an I-beam.
                            HoverHandler { cursorShape: Qt.IBeamCursor }
                        }
                        Switch {
                            id: catchAllPreserveSwitch
                            text: qsTr("Preserve folders")
                            checked: usbGrabberPage.catchAllRule.preserveStructure
                            hoverEnabled: true
                            onToggled: usbGrabberPage.commitRuleField("*", "", "preserveStructure", checked)
                            HoverHandler { cursorShape: Qt.PointingHandCursor }
                            HoverBorder { anchors.fill: parent; active: catchAllPreserveSwitch.hovered }
                        }
                    }

                    UsbRegexField {
                        Layout.fillWidth: true
                        pattern: usbGrabberPage.catchAllRule.filenameRegex || ""
                        controller: mainPanel.usbGrabberController
                        onCommitted: (newPattern) => usbGrabberPage.commitRuleField("*", "", "filenameRegex", newPattern)
                    }

                    TextField {
                        id: catchAllStartFolderField
                        Layout.fillWidth: true
                        placeholderText: qsTr("Start folder (relative to device root, blank = root)")
                        text: usbGrabberPage.catchAllRule.startFolder || ""
                        background: UnderlineField { focused: catchAllStartFolderField.activeFocus }
                        onEditingFinished: usbGrabberPage.commitRuleField("*", "", "startFolder", text)
                        // Item 13: editable text needs an I-beam.
                        HoverHandler { cursorShape: Qt.IBeamCursor }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        Label {
                            Layout.fillWidth: true
                            text: qsTr("Destination: %1").arg(
                                usbGrabberPage.catchAllRule.destinationFolder || qsTr("vault root"))
                            color: "#888888"
                            font.pixelSize: 11
                            elide: Text.ElideMiddle
                        }
                        Item {
                            id: catchAllDestinationButton
                            Layout.preferredWidth: 28
                            Layout.preferredHeight: 28
                            HoverHandler { id: catchAllDestinationHover; cursorShape: Qt.PointingHandCursor }
                            TapHandler {
                                onTapped: {
                                    const sub = usbGrabberPage.catchAllRule.destinationFolder
                                    usbDestinationFolderDialog.targetSerial = "*"
                                    usbDestinationFolderDialog.targetLabel = ""
                                    usbDestinationFolderDialog.currentFolder =
                                        "file://" + mainPanel.vaultLocalPath + (sub ? "/" + sub : "")
                                    usbDestinationFolderDialog.open()
                                }
                            }
                            Rectangle {
                                anchors.fill: parent
                                radius: AppSettings.borderRadius
                                color: catchAllDestinationHover.hovered ? "#3a3a3a" : "#2a2a2a"
                            }
                            HoverBorder { anchors.fill: parent; active: catchAllDestinationHover.hovered }
                            ToolTip.visible: catchAllDestinationHover.hovered
                            ToolTip.text: qsTr("Choose destination folder")
                            ToolTip.delay: 500
                            AppIcon { anchors.centerIn: parent; name: "folder"; iconSize: 13; color: "#dddddd" }
                        }
                    }
                }

                Label {
                    text: qsTr("Devices")
                    color: "#cccccc"
                    font.pixelSize: 13
                }

                Label {
                    Layout.fillWidth: true
                    visible: usbGrabberPage.deviceRows.length === 0
                    text: qsTr("No USB devices seen yet — plug one in, or rely on \"Any USB device\" above.")
                    color: "#888888"
                    font.pixelSize: 11
                    wrapMode: Text.WordWrap
                }

                Repeater {
                    model: usbGrabberPage.deviceRows

                    delegate: ColumnLayout {
                        id: deviceCard
                        required property var modelData

                        Layout.fillWidth: true
                        spacing: 4

                        Rectangle {
                            Layout.fillWidth: true
                            Layout.preferredHeight: 1
                            color: "#3a3a3a"
                        }

                        RowLayout {
                            Layout.fillWidth: true
                            Label {
                                Layout.fillWidth: true
                                text: deviceCard.modelData.label
                                color: "white"
                                elide: Text.ElideRight
                            }
                            Label {
                                // A multi-LUN device (e.g. a recorder exposing internal
                                // memory and an SD card) mounts several volumes under one
                                // serial and one rule — say so, since "connected" alone
                                // would hide that "Grab now" is about to walk more than
                                // one of them.
                                text: !deviceCard.modelData.mounted ? qsTr("not connected")
                                    : deviceCard.modelData.mountPaths.length > 1
                                        ? qsTr("%1 volumes connected").arg(deviceCard.modelData.mountPaths.length)
                                        : qsTr("connected")
                                color: deviceCard.modelData.mounted ? "#7ac07a" : "#888888"
                                font.pixelSize: 11
                            }
                            // Strictly narrower than the enable switch beside it
                            // (see UsbGrabRule::autoGrab) — meaningless on a
                            // device that can't be grabbed at all, so it's
                            // disabled until the rule is on rather than sitting
                            // there as a live switch that quietly does nothing.
                            Switch {
                                id: deviceAutoGrabSwitch
                                text: qsTr("Auto-grab")
                                enabled: deviceCard.modelData.enabled
                                checked: deviceCard.modelData.autoGrab || false
                                hoverEnabled: true
                                onToggled: usbGrabberPage.commitRuleField(deviceCard.modelData.serial,
                                    deviceCard.modelData.label, "autoGrab", checked)
                                HoverHandler { cursorShape: Qt.PointingHandCursor; enabled: deviceAutoGrabSwitch.enabled }
                                HoverBorder {
                                    anchors.fill: parent
                                    active: deviceAutoGrabSwitch.enabled && deviceAutoGrabSwitch.hovered
                                }
                                ToolTip.visible: hovered
                                ToolTip.delay: 500
                                ToolTip.text: qsTr("Grab from this device as soon as it's plugged in, without asking")
                            }
                            Switch {
                                id: deviceEnabledSwitch
                                checked: deviceCard.modelData.enabled
                                hoverEnabled: true
                                onToggled: usbGrabberPage.commitRuleField(deviceCard.modelData.serial,
                                    deviceCard.modelData.label, "enabled", checked)
                                HoverHandler { cursorShape: Qt.PointingHandCursor }
                                HoverBorder { anchors.fill: parent; active: deviceEnabledSwitch.hovered }
                            }
                            Button {
                                id: grabNowButton
                                text: qsTr("Grab now")
                                implicitHeight: 22
                                enabled: deviceCard.modelData.mounted
                                opacity: enabled ? 1 : 0.35
                                hoverEnabled: enabled
                                onClicked: mainPanel.usbGrabberController.triggerManualGrab(deviceCard.modelData.serial)
                                background: Rectangle {
                                    radius: AppSettings.borderRadius
                                    color: "transparent"
                                    border.width: 1
                                    border.color: "#3a3a3a"
                                }
                                HoverHandler { cursorShape: Qt.PointingHandCursor; enabled: grabNowButton.enabled }
                                HoverBorder { anchors.fill: parent; active: grabNowButton.hovered }
                            }
                            Button {
                                id: previewGrabButton
                                text: qsTr("Preview")
                                implicitHeight: 22
                                enabled: deviceCard.modelData.mounted
                                opacity: enabled ? 1 : 0.35
                                hoverEnabled: enabled
                                onClicked: usbGrabberPage.requestPreview(deviceCard.modelData.serial)
                                background: Rectangle {
                                    radius: AppSettings.borderRadius
                                    color: "transparent"
                                    border.width: 1
                                    border.color: "#3a3a3a"
                                }
                                HoverHandler { cursorShape: Qt.PointingHandCursor; enabled: previewGrabButton.enabled }
                                HoverBorder { anchors.fill: parent; active: previewGrabButton.hovered }
                            }
                            Button {
                                id: forgetDeviceButton
                                text: qsTr("Forget")
                                implicitHeight: 22
                                hoverEnabled: true
                                onClicked: {
                                    mainPanel.usbGrabberController.removeRule(deviceCard.modelData.serial)
                                    usbGrabberPage.refreshDevices()
                                }
                                background: Rectangle {
                                    radius: AppSettings.borderRadius
                                    color: "transparent"
                                    border.width: 1
                                    border.color: "#3a3a3a"
                                }
                                HoverHandler { cursorShape: Qt.PointingHandCursor }
                                HoverBorder { anchors.fill: parent; active: forgetDeviceButton.hovered }
                            }
                        }

                        RowLayout {
                            Layout.fillWidth: true
                            TextField {
                                id: deviceExtensionField
                                Layout.fillWidth: true
                                placeholderText: qsTr("Extensions (comma-separated, blank = all files)")
                                text: deviceCard.modelData.extensionFilterText
                                background: UnderlineField { focused: deviceExtensionField.activeFocus }
                                onEditingFinished: usbGrabberPage.commitRuleField(deviceCard.modelData.serial,
                                    deviceCard.modelData.label, "extensionFilter",
                                    text.split(",").map(s => s.trim()).filter(s => s.length > 0))
                                // Item 13: editable text needs an I-beam.
                                HoverHandler { cursorShape: Qt.IBeamCursor }
                            }
                            Switch {
                                id: devicePreserveSwitch
                                text: qsTr("Preserve folders")
                                checked: deviceCard.modelData.preserveStructure
                                hoverEnabled: true
                                onToggled: usbGrabberPage.commitRuleField(deviceCard.modelData.serial,
                                    deviceCard.modelData.label, "preserveStructure", checked)
                                HoverHandler { cursorShape: Qt.PointingHandCursor }
                                HoverBorder { anchors.fill: parent; active: devicePreserveSwitch.hovered }
                            }
                        }

                        UsbRegexField {
                            Layout.fillWidth: true
                            pattern: deviceCard.modelData.filenameRegex || ""
                            controller: mainPanel.usbGrabberController
                            onCommitted: (newPattern) => usbGrabberPage.commitRuleField(
                                deviceCard.modelData.serial, deviceCard.modelData.label,
                                "filenameRegex", newPattern)
                        }

                        TextField {
                            id: deviceStartFolderField
                            Layout.fillWidth: true
                            placeholderText: qsTr("Start folder (relative to device root, blank = root)")
                            text: deviceCard.modelData.startFolder || ""
                            background: UnderlineField { focused: deviceStartFolderField.activeFocus }
                            onEditingFinished: usbGrabberPage.commitRuleField(deviceCard.modelData.serial,
                                deviceCard.modelData.label, "startFolder", text)
                            // Item 13: editable text needs an I-beam.
                            HoverHandler { cursorShape: Qt.IBeamCursor }
                        }

                        RowLayout {
                            Layout.fillWidth: true
                            Label {
                                Layout.fillWidth: true
                                text: qsTr("Destination: %1").arg(
                                    deviceCard.modelData.destinationFolder || qsTr("vault root"))
                                color: "#888888"
                                font.pixelSize: 11
                                elide: Text.ElideMiddle
                            }
                            Item {
                                id: deviceDestinationButton
                                Layout.preferredWidth: 28
                                Layout.preferredHeight: 28
                                HoverHandler { id: deviceDestinationHover; cursorShape: Qt.PointingHandCursor }
                                TapHandler {
                                    onTapped: {
                                        const sub = deviceCard.modelData.destinationFolder
                                        usbDestinationFolderDialog.targetSerial = deviceCard.modelData.serial
                                        usbDestinationFolderDialog.targetLabel = deviceCard.modelData.label
                                        usbDestinationFolderDialog.currentFolder =
                                            "file://" + mainPanel.vaultLocalPath + (sub ? "/" + sub : "")
                                        usbDestinationFolderDialog.open()
                                    }
                                }
                                Rectangle {
                                    anchors.fill: parent
                                    radius: AppSettings.borderRadius
                                    color: deviceDestinationHover.hovered ? "#3a3a3a" : "#2a2a2a"
                                }
                                HoverBorder { anchors.fill: parent; active: deviceDestinationHover.hovered }
                                ToolTip.visible: deviceDestinationHover.hovered
                                ToolTip.text: qsTr("Choose destination folder")
                                ToolTip.delay: 500
                                AppIcon { anchors.centerIn: parent; name: "folder"; iconSize: 13; color: "#dddddd" }
                            }
                        }

                        // Grab preview — rendered right under whichever device's
                        // "Preview" button was last clicked (see
                        // usbGrabberPage.requestPreview()/previewDeviceSerial),
                        // styled like SettingsPopup.qml's combine-rule preview
                        // cards (dark card, rich-text lines, per-status color).
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 4
                            visible: deviceCard.modelData.serial === usbGrabberPage.previewDeviceSerial

                            // Preview header. The dismiss control lives HERE
                            // rather than inside the results card below so it's
                            // reachable in every preview state — scanning,
                            // refused, empty, and results — not only the one
                            // that happened to find files. Nothing else ever
                            // takes the panel down (deliberately: it survives
                            // the 2.5s device poll, see previewDeviceSerial),
                            // so without this the only way out was previewing a
                            // different device or leaving the page.
                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 4

                                Label {
                                    Layout.fillWidth: true
                                    text: qsTr("Preview")
                                    color: "#888888"
                                    font.pixelSize: 11
                                }
                                Item {
                                    id: dismissPreviewButton
                                    Layout.preferredWidth: 20
                                    Layout.preferredHeight: 20

                                    HoverHandler {
                                        id: dismissPreviewHover
                                        cursorShape: Qt.PointingHandCursor
                                    }
                                    TapHandler { onTapped: usbGrabberPage.dismissPreview() }
                                    HoverBorder { anchors.fill: parent; active: dismissPreviewHover.hovered }
                                    AppIcon {
                                        anchors.centerIn: parent
                                        name: "close"
                                        iconSize: 13
                                        color: "#dddddd"
                                    }
                                    ToolTip.visible: dismissPreviewHover.hovered
                                    ToolTip.delay: 500
                                    ToolTip.text: qsTr("Dismiss preview")
                                }
                            }

                            Label {
                                visible: usbGrabberPage.previewRunning
                                text: qsTr("Scanning…")
                                color: "#888888"
                                font.pixelSize: 11
                            }
                            Label {
                                Layout.fillWidth: true
                                visible: !usbGrabberPage.previewRunning && usbGrabberPage.previewError !== ""
                                text: usbGrabberPage.previewError
                                color: "#e07a7a"
                                font.pixelSize: 11
                                wrapMode: Text.WordWrap
                            }
                            Label {
                                visible: !usbGrabberPage.previewRunning && usbGrabberPage.previewError === ""
                                        && usbGrabberPage.previewEntries.length === 0
                                text: qsTr("No files found.")
                                color: "#888888"
                                font.pixelSize: 11
                            }
                            Rectangle {
                                Layout.fillWidth: true
                                Layout.preferredHeight: previewCardColumn.implicitHeight + 12
                                visible: !usbGrabberPage.previewRunning && usbGrabberPage.previewEntries.length > 0
                                radius: AppSettings.borderRadius
                                color: "#1e1e1e"
                                border.width: 1
                                border.color: "#3a3a3a"

                                ColumnLayout {
                                    id: previewCardColumn
                                    anchors.fill: parent
                                    anchors.margins: 6
                                    spacing: 2

                                    Label {
                                        Layout.fillWidth: true
                                        text: usbGrabberPage.previewSummary(usbGrabberPage.previewEntries)
                                        color: "#dddddd"
                                        font.pixelSize: 11
                                        wrapMode: Text.WordWrap
                                    }
                                    TextEdit {
                                        Layout.fillWidth: true
                                        wrapMode: Text.WordWrap
                                        readOnly: true
                                        selectByMouse: true
                                        selectionColor: Qt.rgba(0.35, 0.55, 0.85, 0.5)
                                        color: "#dddddd"
                                        font.pixelSize: 10
                                        textFormat: Text.RichText
                                        text: usbGrabberPage.previewHtml(usbGrabberPage.previewEntries)
                                        // Item 13: selectable text needs an I-beam.
                                        HoverHandler { cursorShape: Qt.IBeamCursor }
                                    }
                                }
                            }
                        }
                    }
                }

                Label {
                    text: qsTr("History")
                    color: "#cccccc"
                    font.pixelSize: 13
                }

                Label {
                    Layout.fillWidth: true
                    visible: usbGrabberPage.historyEntries.length === 0
                    text: qsTr("No grabs yet.")
                    color: "#888888"
                    font.pixelSize: 11
                }

                Repeater {
                    model: usbGrabberPage.historyEntries

                    delegate: ColumnLayout {
                        id: historyCard
                        required property var modelData

                        Layout.fillWidth: true
                        spacing: 2

                        Rectangle {
                            Layout.fillWidth: true
                            Layout.preferredHeight: 1
                            color: "#2a2a2a"
                        }

                        RowLayout {
                            Layout.fillWidth: true
                            Label {
                                text: usbGrabberPage.formatTimestamp(historyCard.modelData.timestamp)
                                color: "#888888"
                                font.pixelSize: 11
                            }
                            Label {
                                Layout.fillWidth: true
                                text: historyCard.modelData.deviceLabel || historyCard.modelData.deviceSerial
                                color: "white"
                                font.pixelSize: 11
                                elide: Text.ElideRight
                            }
                            Label {
                                text: qsTr("%1 copied, %2 skipped").arg(historyCard.modelData.filesCopied)
                                    .arg(historyCard.modelData.filesSkipped)
                                color: "#cccccc"
                                font.pixelSize: 11
                            }
                        }

                        Label {
                            Layout.fillWidth: true
                            visible: (historyCard.modelData.errors || []).length > 0
                            text: (historyCard.modelData.errors || []).join("; ")
                            color: "#e07a7a"
                            font.pixelSize: 11
                            wrapMode: Text.WordWrap
                        }
                    }
                }
            }
        }

        // Shared by both the catch-all card and every per-device card's
        // destination-folder button above — targetSerial/targetLabel record
        // which rule to commit into on accept (mirrors recordingFolderDialog's
        // single-shared-dialog shape further below).
        FolderDialog {
            id: usbDestinationFolderDialog
            property string targetSerial: ""
            property string targetLabel: ""
            onAccepted: {
                const localPath = usbDestinationFolderDialog.selectedFolder.toString().replace("file://", "")
                const relative = usbGrabberPage.relativeToVault(localPath)
                if (relative === null) {
                    usbGrabberPage.destinationFolderError =
                        qsTr("That folder is outside the vault — pick a location inside it.")
                    return
                }
                usbGrabberPage.destinationFolderError = ""
                usbGrabberPage.commitRuleField(usbDestinationFolderDialog.targetSerial,
                    usbDestinationFolderDialog.targetLabel, "destinationFolder", relative)
            }
        }
    }

    // Item 2 (delete): reached ONLY when the trash refused the open file — a
    // successful trash never opens this, it posts a snackbar with Undo (see
    // requestDeleteFile()/requestDeleteTranscript()). So the wording promises no
    // trash: it used to say "Moved to the trash" on every delete, which is
    // exactly wrong here — this is the one case where nothing is. Message +
    // confirm label switch on mainPanel.pendingDeleteMode.
    Popup {
        id: deleteFileConfirmPopup
        anchors.centerIn: Overlay.overlay
        width: 320
        padding: 12
        modal: true
        dim: true
        focus: true
        closePolicy: Popup.CloseOnPressOutside | Popup.CloseOnEscape

        background: Rectangle {
            color: "#242424"
            radius: AppSettings.borderRadius
            border.width: 1
            border.color: "#3a3a3a"
        }

        ColumnLayout {
            width: deleteFileConfirmPopup.width - deleteFileConfirmPopup.padding * 2
            spacing: 10

            Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                text: mainPanel.pendingDeleteMode === "transcript"
                      ? qsTr("This transcript can't be moved to the trash. Delete it " +
                             "permanently? The audio file is kept. This can't be undone.")
                      : mainPanel.pendingDeleteMode === "audioOnly"
                      ? qsTr("This audio file can't be moved to the trash. Delete it " +
                             "permanently? The transcript is kept. This can't be undone.")
                      : qsTr("This file can't be moved to the trash. Delete it and its " +
                             "transcript permanently? This can't be undone.")
                color: "white"
                font.pixelSize: 12
            }

            RowLayout {
                Layout.alignment: Qt.AlignRight
                spacing: 6

                Item {
                    id: deleteFileCancelButton
                    Layout.preferredWidth: deleteFileCancelLabel.implicitWidth + 16
                    Layout.preferredHeight: 24
                    HoverHandler {
                        id: deleteFileCancelHover
                        cursorShape: Qt.PointingHandCursor
                    }
                    TapHandler {
                        onTapped: deleteFileConfirmPopup.close()
                    }
                    HoverBorder {
                        anchors.fill: parent
                        active: deleteFileCancelHover.hovered
                    }
                    Text {
                        id: deleteFileCancelLabel
                        anchors.centerIn: parent
                        text: qsTr("Cancel")
                        color: "#aaaaaa"
                        font.pixelSize: 12
                    }
                }

                Item {
                    id: deleteFileConfirmButton
                    Layout.preferredWidth: deleteFileConfirmLabel.implicitWidth + 16
                    Layout.preferredHeight: 24
                    HoverHandler {
                        id: deleteFileConfirmHover
                        cursorShape: Qt.PointingHandCursor
                    }
                    TapHandler {
                        onTapped: mainPanel.confirmFileDelete()
                    }
                    HoverBorder {
                        anchors.fill: parent
                        active: deleteFileConfirmHover.hovered
                    }
                    Text {
                        id: deleteFileConfirmLabel
                        anchors.centerIn: parent
                        text: mainPanel.pendingDeleteMode === "transcript"
                              ? qsTr("Delete transcript")
                              : mainPanel.pendingDeleteMode === "audioOnly"
                              ? qsTr("Delete audio")
                              : qsTr("Delete file")
                        color: "#e07a7a"
                        font.pixelSize: 12
                    }
                }
            }
        }
    }
}
