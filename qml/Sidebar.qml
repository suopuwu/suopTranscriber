import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Window
import SuopTranscriber
import "TextFormat.js" as TextFormat

Rectangle {
    id: sidebar

    // Item 9: a VaultSearchProxyModel now sits between the externally-bound
    // model and the TreeView, so sidebar.model still means "the vault to
    // show" from the outside while the proxy handles matches-first sorting.
    property alias model: searchProxy.sourceModel
    property TranscriptionController transcriptionController
    property url vaultPath
    // O7: whatever MainPanel currently has open (audio file or orphaned .md,
    // raw local path either way) — fed back down from Main.qml so the tree
    // delegate below can render a persistent "currently open" highlight.
    property string currentPath: ""
    signal fileActivated(string path)
    signal orphanedMdActivated(string path)
    // Item 2 (Wave E): clicking a search-result transcript snippet opens
    // that file AND jumps to/highlights the searched phrase in it.
    signal transcriptMatchActivated(string path, bool isOrphanedMd, string query)

    property string searchQuery: ""
    // Transcriptions are searched by default (labels/tags always are); the
    // checkbox lets the user EXCLUDE them, e.g. for a quick label-only scan.
    property bool searchIncludeTranscriptions: true
    property bool searchExpanded: false

    // Item 16/17 (Wave E): comma-separated tag filter, genuinely hides
    // non-matching rows (unlike searchQuery's dim+sort) via
    // VaultSearchProxyModel.tagFilter.
    property string tagFilterText: ""
    readonly property var activeTagFilters: sidebar.tagFilterText.split(",")
        .map(t => t.trim()).filter(t => t.length > 0)

    // Mirrors tagFilterText/activeTagFilters/applyTagFilter above, against
    // VaultSearchProxyModel.speakerFilter instead — see MainPanel's
    // speakerActivated signal (fired by clicking a speaker pill).
    property string speakerFilterText: ""
    readonly property var activeSpeakerFilters: sidebar.speakerFilterText.split(",")
        .map(s => s.trim()).filter(s => s.length > 0)

    // Item 16: MainPanel's tag pill onClicked calls this (Main.qml wires
    // sidebar as a direct id, no signal roundtrip needed) — appends this tag
    // to any existing filter (skipping a case-insensitive duplicate) and
    // opens the search area so the active filter is visible.
    function applyTagFilter(tag) {
        sidebar.searchExpanded = true
        const existing = sidebar.tagFilterText.split(",").map(t => t.trim()).filter(t => t.length > 0)
        if (!existing.some(t => t.toLowerCase() === tag.toLowerCase()))
            existing.push(tag)
        sidebar.tagFilterText = existing.join(", ")
    }

    function applySpeakerFilter(name) {
        sidebar.searchExpanded = true
        const existing = sidebar.speakerFilterText.split(",").map(s => s.trim()).filter(s => s.length > 0)
        if (!existing.some(s => s.toLowerCase() === name.toLowerCase()))
            existing.push(name)
        sidebar.speakerFilterText = existing.join(", ")
    }

    // Item 17 / filter-suggestions rework: the comma-separated-fragment logic
    // behind BOTH filter autocompletes (tags and speakers). Generic over the
    // field text and its vocabulary so the two call sites below share one
    // implementation — see FilterSuggestions.qml for the shared UI half.
    //
    // "Fragment" = the text after the last comma, i.e. the entry currently
    // being typed. Entries already committed earlier in the field are
    // excluded from the candidates so a tag can't be suggested twice.
    function fragmentSuggestions(fieldText, vocabulary) {
        const parts = fieldText.split(",")
        const fragment = parts[parts.length - 1].trim().toLowerCase()
        if (fragment === "")
            return []
        const already = parts.slice(0, -1).map(t => t.trim().toLowerCase())
        return vocabulary.filter(entry =>
            entry.toLowerCase().startsWith(fragment) && !already.includes(entry.toLowerCase()))
    }

    // Replaces the in-progress (last, possibly partial) fragment with the
    // chosen value, leaving a trailing ", " ready for the next entry.
    // Returns the new field text; the caller assigns it and restores focus.
    function acceptFragment(fieldText, value) {
        const parts = fieldText.split(",")
        parts[parts.length - 1] = " " + value
        return parts.map(t => t.trim()).join(", ") + ", "
    }

    function tagSuggestions() {
        if (!sidebar.transcriptionController || sidebar.vaultPath.toString() === "")
            return []
        return sidebar.fragmentSuggestions(
            sidebar.tagFilterText,
            sidebar.transcriptionController.existingVaultTags(sidebar.vaultPath))
    }

    // The speaker mirror — vocabulary from existingVaultSpeakers() (see
    // queue item 045), which scans the same `speakers:` frontmatter the
    // speaker filter itself matches against.
    function speakerSuggestions() {
        if (!sidebar.transcriptionController || sidebar.vaultPath.toString() === "")
            return []
        return sidebar.fragmentSuggestions(
            sidebar.speakerFilterText,
            sidebar.transcriptionController.existingVaultSpeakers(sidebar.vaultPath))
    }

    // --- Item 9 helpers: match highlighting + transcript snippet ------------
    // escapeHtml/highlightMatch/cleanTranscript/transcriptSnippet moved to
    // TextFormat.js (pure, shared with TreeRowDelegate.qml). The two
    // highlighters took AppSettings.highlightColor as a hidden mid-string-build
    // read; it's an explicit argument at every call site now.

    VaultSearchProxyModel {
        id: searchProxy
        // Bug fix: searchQuery/includeTranscriptions are deliberately NOT
        // bound to sidebar.searchQuery/searchIncludeTranscriptions anymore.
        // Since the search-UI rearchitecture, text search is rendered
        // entirely by searchResultsView below (via
        // vaultTreeModel.searchResults()) — the TreeView bound to this proxy
        // is fully hidden whenever a text search is active
        // (treeView.visible: sidebar.searchQuery === ""). Feeding a live
        // search query into this proxy while the TreeView was hidden still
        // triggered its sort/dim re-evaluation and the model-reset/
        // expandAll relayout machinery below — invisibly, off-screen — and
        // left the TreeView's cached row positions in a stale/inconsistent
        // state by the time it became visible again after the search
        // cleared (observed live: large gaps and overlapping rows,
        // hover-highlight geometry correct because it's computed fresh on
        // hover, static text left at stale positions). searchQuery staying
        // permanently "" here is exactly equivalent to the old
        // bound-but-only-relevant-when-""-anyway behavior from the
        // TreeView's own point of view, since the TreeView is never visible
        // while sidebar.searchQuery is non-"" in the first place.
        tagFilter: sidebar.activeTagFilters
        speakerFilter: sidebar.activeSpeakerFilters
    }

    // Search UI rearchitecture: while a text search query is active, the
    // normal TreeView is hidden and this flat, pre-computed results list
    // (from VaultTreeModel::searchResults(), NOT the in-tree
    // VaultSearchProxyModel dim+sort above — that stays exactly as-is,
    // still driving tag-filter row-hiding) is shown instead. A plain
    // property + explicit refresh function, not a reactive binding that
    // reads jobStatusRevision/etc. as a dependency trick — this codebase has
    // already been bitten once by qmlcachegen (AOT) silently dropping an
    // unused dummy-property read meant only to force binding
    // re-evaluation, so every other "recompute on X" case in this file uses
    // an explicit push instead, and this follows the same idiom.
    //
    // Edge case (tag filter + text search both active at once): the two are
    // separate, orthogonal features, and this flat list intentionally does
    // NOT also apply activeTagFilters — text search simply takes precedence
    // for what's visible while its query is non-empty. Simplest coherent
    // behavior per the plan; revisit only if that combination turns out to
    // matter in practice.
    property var searchResultsList: []
    function refreshSearchResults() {
        sidebar.searchResultsList = (sidebar.searchQuery !== "" && sidebar.model)
            ? sidebar.model.searchResults(sidebar.searchQuery, sidebar.searchIncludeTranscriptions)
            : []
    }
    // Burst-collapsing twin, for callers driven by a PER-TASK signal (onFinished)
    // rather than a user gesture — same idiom as refreshSelectionEligibility().
    // Direct callers (a query edit, a toggle) keep the synchronous function:
    // there is exactly one of them per gesture, and the result must be on screen
    // for the same frame.
    property bool searchResultsRefreshPending: false
    function refreshSearchResultsCoalesced() {
        if (sidebar.searchResultsRefreshPending)
            return
        sidebar.searchResultsRefreshPending = true
        Qt.callLater(sidebar.doRefreshSearchResultsCoalesced)
    }
    function doRefreshSearchResultsCoalesced() {
        sidebar.searchResultsRefreshPending = false
        sidebar.refreshSearchResults()
    }

    // Reuses the exact fileActivated/orphanedMdActivated signal path a
    // click emits — currentPath's persistent outline (Main.qml) and
    // MainPanel's open file update as a side effect of that binding, same as
    // clicking the row directly.
    function activateNote(path, isOrphanedMd) {
        if (isOrphanedMd)
            sidebar.orphanedMdActivated(path)
        else
            sidebar.fileActivated(path)
    }

    // Item 3: multi-select, layered on top of currentPath above rather than
    // replacing it — currentPath stays MainPanel's single "what's open"
    // pointer (O7), selectedPaths is a separate, purely-sidebar-local flat
    // array of note (audio/orphaned-.md) paths. Directories are never
    // selectable — clicking one only ever toggles expand (see treeDelegate's
    // TapHandler), same as before this item.
    property var selectedPaths: []
    // The last note path clicked WITHOUT Shift — the anchor a Shift-click
    // range is measured from (repeated Shift-clicks re-extend/shrink from
    // this same anchor, matching common file-manager convention, rather than
    // sliding the anchor to wherever the previous Shift-click landed).
    property string lastClickedPath: ""

    // Expand/collapse state for EMPTY directories only — populated ones use
    // TreeView's own expanded state, which is the single source of truth for
    // them. This exists because QQuickTreeView flatly refuses to expand a
    // childless node: toggleExpanded()/expand() leave isExpanded(row) false
    // forever (verified against the live view in tst_sidebar_expand.cpp), and
    // yet the delegate's `expanded` property still latches to true on the
    // attempt and never clears — so reading `expanded` for an empty folder
    // gives a glyph that flips to "−" once and sticks there. The delegate
    // therefore never calls toggleExpanded() on a childless dir, and reads
    // this instead.
    //
    // Nothing is revealed either way (an empty folder has nothing to reveal),
    // so this is purely the glyph's state — kept because a folder that can't
    // be toggled at all reads as broken next to siblings that can.
    //
    // Tracks the COLLAPSED ones, not the expanded ones, because the default is
    // expanded: tree.expandAll() runs on every model reset, so every populated
    // folder shows "−" by default and an empty one defaulting to "+" would look
    // arbitrary. expandAll() clears this for the same reason — it already blows
    // away a user's collapse of a POPULATED folder, so empty folders forgetting
    // theirs at the same moment is parity, not data loss.
    property var collapsedEmptyDirs: ({})

    function isEmptyDirCollapsed(path) {
        return sidebar.collapsedEmptyDirs[path] === true
    }

    // Reassigns rather than mutates: a `var` property holding an object emits
    // no change signal when you poke a key into it, so the delegates' glyph
    // bindings would never re-evaluate.
    function toggleEmptyDirCollapsed(path) {
        const next = {}
        for (const k in sidebar.collapsedEmptyDirs)
            next[k] = true
        if (next[path])
            delete next[path]
        else
            next[path] = true
        sidebar.collapsedEmptyDirs = next
    }

    // Item 41: the moving end of a keyboard range gesture (Shift+Up/Down) —
    // distinct from lastClickedPath, which is the fixed anchor the range is
    // measured FROM. A plain click/plain arrow/group-jump collapses both to
    // the same row (single selection); Shift+arrow walks this cursor while
    // the anchor stays put, exactly like Shift+click's moving end.
    property string selectionCursorPath: ""

    // Notes (audio/orphaned .md — never directories) in on-screen order,
    // exactly as a user could click them. Same shape as moveNoteSelection's
    // own two branches below (flat searchResultsList while a text search is
    // active, else a TreeView row walk skipping collapsed children) but
    // paths-only — Shift-range only ever needs "where does X sit relative to
    // Y in what's visible," not moveNoteSelection's row-for-scrolling detail.
    function visibleNotePaths() {
        if (sidebar.searchQuery !== "")
            return sidebar.searchResultsList.map((r) => r.path)
        const paths = []
        for (let r = 0; r < tree.rows; r++) {
            const rowIndex = tree.index(r, 0)
            if (searchProxy.data(rowIndex, VaultTreeModel.IsAudioRole)
                    || searchProxy.data(rowIndex, VaultTreeModel.IsOrphanedMdRole))
                paths.push(searchProxy.data(rowIndex, VaultTreeModel.PathRole))
        }
        return paths
    }

    // Ribbon's prepended selectAllButton, and its Ctrl+A shortcut mirror
    // below: select every note currently visible (whichever mode —
    // tree or search results — is showing), anchored top-to-bottom like a
    // top-to-bottom Shift-click so a following Shift+Up shrinks from the
    // bottom instead of the top.
    function selectAllVisible() {
        const order = sidebar.visibleNotePaths()
        if (order.length === 0)
            return
        sidebar.selectedPaths = order
        sidebar.lastClickedPath = order[0]
        sidebar.selectionCursorPath = order[order.length - 1]
    }

    // Collapse any multi-selection down to just the currently-open file.
    // Reached three ways: the Escape key (see the Shortcut below), a click on
    // any non-file surface in the sidebar (empty tree/search space — see
    // treeEmptyLeftTap/searchEmptyLeftTap), and the explicit deselect button
    // beside the "N selected" indicator. "Deselect everything but the current
    // file" — so the open file stays: it keeps its currentPath outline either
    // way (that's driven by currentPath, not selectedPaths), and here we also
    // make it the sole real selection so a following Shift-click still has a
    // sane anchor. If the open file isn't a visible note (nothing open, or it's
    // hidden by a filter/collapse), fall back to a fully empty selection.
    //
    // Deliberately NOT wired to file-action buttons (transcribe/delete/…) or
    // file rows: those must keep the selection they act on, and overlapping
    // TapHandlers aren't mutually exclusive in Qt Quick (see the treeRowTap /
    // StyleGuide "Always-editable text" notes), so this is added only on the
    // genuinely-empty surfaces, never as a blanket catcher over the ribbon.
    // The ribbon's search toggle lives in SidebarRibbon.qml, which can't reach
    // searchField (a sibling inside expandedColumn) by id across the component
    // boundary — it calls this instead.
    function focusSearchField() { searchField.forceActiveFocus() }

    function deselectExceptCurrent() {
        if (sidebar.currentPath !== ""
                && sidebar.visibleNotePaths().indexOf(sidebar.currentPath) >= 0) {
            sidebar.selectedPaths = [sidebar.currentPath]
            sidebar.lastClickedPath = sidebar.currentPath
            sidebar.selectionCursorPath = sidebar.currentPath
        } else {
            sidebar.selectedPaths = []
            sidebar.lastClickedPath = ""
            sidebar.selectionCursorPath = ""
        }
    }

    // Single entry point for both treeDelegate's and searchResultsView's
    // TapHandlers (see each's onTapped below) — modifiers come from
    // TapHandler.point.modifiers (QQuickHandlerPoint's own property, not
    // something the tapped signal itself carries). Plain click keeps the
    // exact pre-item-3 behavior (select just this row AND open it — the two
    // were always the same action here). Shift/Ctrl-click only change
    // selectedPaths, deliberately WITHOUT calling activateNote — same as
    // Nautilus/most file managers, where building a multi-selection
    // shouldn't also keep swapping MainPanel's open file out from under the
    // user on every click.
    function handleNoteClick(path, isOrphanedMd, modifiers) {
        const shift = (modifiers & Qt.ShiftModifier) !== 0
        const ctrl = (modifiers & Qt.ControlModifier) !== 0
        // Item 41: the clicked row is the new moving end for any subsequent
        // keyboard Shift+arrow extension (Shift-click's moving end is exactly
        // where the pointer landed).
        sidebar.selectionCursorPath = path
        if (shift && sidebar.lastClickedPath !== "") {
            const order = sidebar.visibleNotePaths()
            const fromIdx = order.indexOf(sidebar.lastClickedPath)
            const toIdx = order.indexOf(path)
            if (fromIdx >= 0 && toIdx >= 0) {
                const lo = Math.min(fromIdx, toIdx)
                const hi = Math.max(fromIdx, toIdx)
                sidebar.selectedPaths = order.slice(lo, hi + 1)
            } else {
                // Anchor no longer visible (e.g. scrolled out from under a
                // collapsed dir) — fall back to a plain single-select rather
                // than silently doing nothing.
                sidebar.selectedPaths = [path]
                sidebar.lastClickedPath = path
            }
        } else if (ctrl) {
            sidebar.selectedPaths = sidebar.selectedPaths.indexOf(path) >= 0
                ? sidebar.selectedPaths.filter((p) => p !== path)
                : sidebar.selectedPaths.concat([path])
            sidebar.lastClickedPath = path
        } else {
            sidebar.selectedPaths = [path]
            sidebar.lastClickedPath = path
            sidebar.activateNote(path, isOrphanedMd)
        }
    }

    // transcribeAllButton/relabelAllButton/chaptersAllButton/
    // detectSpeakersAllButton/runAllButton (see the ribbon below) read the
    // selection*Paths properties, which refreshSelectionEligibility() derives
    // from the MODEL (searchResultNotes()) narrowed to the CURRENT SELECTION —
    // never "everything eligible". An empty selection means an empty target
    // set, so the buttons dim; that pairs with the ribbon's prepended
    // selectAllButton (and its Ctrl+A mirror) for the old "act on everything"
    // workflow.

    // Item 39: run-all over the CURRENT SEARCH/FILTER RESULTS (the third,
    // search-menu home for the transcribe/relabel/chapters actions, alongside
    // the ribbon's bulk buttons and treeDelegate's per-row cluster). The
    // filtered/visible note set in whichever mode is active — the flat
    // searchResultsList while a text query is present (VaultTreeModel::
    // searchResults(), already the matched set), else the tag/speaker-filtered
    // tree rows (searchProxy hides non-matching rows via filterAcceptsRow).
    // Same dual-mode shape as visibleNotePaths() above, but carries each note's
    // eligibility fields (isAudio/isOrphanedMd/status) so the three search
    // buttons can intersect with the SAME eligibility rules the ribbon's
    // bulk/relabel/chapters buttons use (treeDelegate.bulkEligible/
    // relabelEligible/chaptersEligible). Self-contained (reads searchResultsList
    // + searchProxy directly) rather than the realized-delegate eligibility
    // maps, since in text-search mode the tree isn't realized at all.
    function searchResultNotes() {
        if (sidebar.searchQuery !== "")
            return sidebar.searchResultsList.map((r) => ({
                path: r.path, isAudio: r.isAudio,
                isOrphanedMd: r.isOrphanedMd, status: r.status }))
        const notes = []
        for (let r = 0; r < tree.rows; r++) {
            const ri = tree.index(r, 0)
            const isAudio = searchProxy.data(ri, VaultTreeModel.IsAudioRole)
            const isOrphanedMd = searchProxy.data(ri, VaultTreeModel.IsOrphanedMdRole)
            if (isAudio || isOrphanedMd)
                notes.push({
                    path: searchProxy.data(ri, VaultTreeModel.PathRole),
                    isAudio: isAudio, isOrphanedMd: isOrphanedMd,
                    status: searchProxy.data(ri, VaultTreeModel.StatusRole) })
        }
        return notes
    }

    // The filtered/visible note paths — the task's searchResultPaths(); same
    // set visibleNotePaths() returns, kept as its own name because the search
    // run-all buttons and their eligibility filters below are all phrased in
    // terms of "the search results".
    function searchResultPaths() {
        return sidebar.searchResultNotes().map((n) => n.path)
    }

    // Per-action eligible subsets of the filtered set, each mirroring the exact
    // rule its ribbon counterpart uses (status 2 == TranscribedStatus, matching
    // the numeric literal treeDelegate/resultDelegate already use):
    //  - transcribe: audio not already matching an existing transcription for
    //    the default model (re-transcribe with a different model still counts),
    //    exactly treeDelegate.bulkEligible.
    //  - relabel:    already-transcribed audio (treeDelegate.relabelEligible).
    //  - chapters:   transcribed audio OR an orphaned .md, since chapter
    //    generation only reads the .md body (treeDelegate.chaptersEligible).
    function searchTranscribePaths() {
        if (!sidebar.transcriptionController)
            return []
        return sidebar.searchResultNotes()
            .filter((n) => n.isAudio
                    && !sidebar.transcriptionController.matchesExistingTranscription(
                           "file://" + n.path, AppSettings.defaultModel))
            .map((n) => n.path)
    }
    function searchRelabelPaths() {
        return sidebar.searchResultNotes()
            .filter((n) => n.isAudio && n.status === 2)
            .map((n) => n.path)
    }
    function searchChaptersPaths() {
        return sidebar.searchResultNotes()
            .filter((n) => (n.isAudio && n.status === 2) || n.isOrphanedMd)
            .map((n) => n.path)
    }
    // Item 52: search-scoped "detect speakers" — transcribed audio in the
    // filtered set (same rule as searchRelabelPaths(); diarization needs an
    // existing transcript and decodes the audio, so no orphaned .md).
    function searchDetectSpeakersPaths() {
        return sidebar.searchResultNotes()
            .filter((n) => n.isAudio && n.status === 2)
            .map((n) => n.path)
    }
    // Search-scoped "run all available jobs" — EVERY audio result, transcribed
    // or not: runAllJobs() queues only the steps a given file still needs and
    // no-ops when it needs none, so narrowing to untranscribed files here would
    // just hide the action from the files it now has work for. Audio only (no
    // orphaned .md — the diarize step needs audio to decode).
    function searchRunAllPaths() {
        return sidebar.searchResultNotes()
            .filter((n) => n.isAudio)
            .map((n) => n.path)
    }

    // Whether any filter/search is actually narrowing the visible set — the
    // search buttons stay disabled otherwise so they can't silently run over
    // the ENTIRE vault (that's the ribbon's job); they only ever act on a
    // genuinely filtered subset. activeTagFilters/activeSpeakerFilters always
    // split to length >= 1 (a bare "" yields [""]), so gate on the raw text.
    readonly property bool searchFilterActive: sidebar.searchQuery !== ""
        || sidebar.tagFilterText.trim() !== "" || sidebar.speakerFilterText.trim() !== ""

    // Item 41 helpers ------------------------------------------------------
    // Parent directory of a note path (everything before the last "/"), used
    // to define a "group" (contiguous run of same-parent notes) for the
    // Ctrl+Up/Down group-boundary jump below.
    function parentDir(path) {
        const i = path.lastIndexOf("/")
        return i >= 0 ? path.substring(0, i) : ""
    }

    // Emit the right activation signal for a bare path — visibleNotePaths()
    // is paths-only, but activateNote() needs isOrphanedMd to pick
    // orphanedMdActivated vs fileActivated, so look it up from whichever
    // view's model backs the current mode.
    function activateNotePath(path) {
        if (sidebar.searchQuery !== "") {
            const r = sidebar.searchResultsList.find((x) => x.path === path)
            if (r)
                sidebar.activateNote(path, r.isOrphanedMd)
            return
        }
        for (let row = 0; row < tree.rows; row++) {
            const ri = tree.index(row, 0)
            if (searchProxy.data(ri, VaultTreeModel.PathRole) === path) {
                sidebar.activateNote(path, searchProxy.data(ri, VaultTreeModel.IsOrphanedMdRole))
                return
            }
        }
    }

    // Scroll a note (by path) fully into view in whichever view is showing —
    // the by-row/by-index variants moveNoteSelection uses inline, but reached
    // from a path when the caller (extend/group-jump) only has the path.
    function scrollNoteIntoView(path) {
        if (sidebar.searchQuery !== "") {
            const i = sidebar.searchResultsList.findIndex((r) => r.path === path)
            if (i >= 0)
                searchResultsView.positionViewAtIndex(i, ListView.Contain)
            return
        }
        for (let row = 0; row < tree.rows; row++) {
            if (searchProxy.data(tree.index(row, 0), VaultTreeModel.PathRole) === path) {
                tree.positionViewAtRow(row, TableView.Contain)
                return
            }
        }
    }

    // Single-select + open + reveal one note, collapsing anchor and cursor
    // onto it — the shared tail of a plain arrow move and a Ctrl+arrow group
    // jump (both behave like a plain click).
    function selectAndOpenNote(path) {
        sidebar.selectedPaths = [path]
        sidebar.lastClickedPath = path
        sidebar.selectionCursorPath = path
        sidebar.activateNotePath(path)
        sidebar.scrollNoteIntoView(path)
    }

    // Re-establish a note as a TRUE selection after an undo-of-delete restored
    // it. The restore rebuild() fires modelReset, whose handler wipes
    // selectedPaths/lastClickedPath/selectionCursorPath (see the target:
    // sidebar.model Connections) — and MainPanel reopening the file only gives
    // it the currentPath "open file" OUTLINE, which LOOKS selected but is not a
    // selectedPaths member and, crucially, is not a Shift-click anchor. Without
    // this, a restored file couldn't start a Shift-range (the reported bug):
    // handleNoteClick's shift branch needs a non-empty lastClickedPath.
    //
    // APPENDS (rather than replacing) so a batch undo that restores N files
    // leaves all N selected — each restore signal fires after the single
    // modelReset that cleared the set, so the first append starts from empty.
    // The anchor/cursor collapse onto the most-recently-restored path, matching
    // how a fresh single-select leaves them.
    function selectRestoredNote(path) {
        if (path === "")
            return
        if (sidebar.selectedPaths.indexOf(path) < 0)
            sidebar.selectedPaths = sidebar.selectedPaths.concat([path])
        sidebar.lastClickedPath = path
        sidebar.selectionCursorPath = path
    }

    // Shift+Up/Down: grow/shrink the selection between the fixed anchor
    // (lastClickedPath) and a moving cursor (selectionCursorPath), mirroring
    // Shift-click's contiguous range. Does NOT open a file (matches
    // handleNoteClick's shift branch, which only changes selectedPaths).
    function extendNoteSelection(direction) {
        const order = sidebar.visibleNotePaths()
        if (order.length === 0)
            return
        let anchorIdx = order.indexOf(sidebar.lastClickedPath !== "" ? sidebar.lastClickedPath
                                                                      : sidebar.currentPath)
        let cursorIdx = order.indexOf(sidebar.selectionCursorPath !== "" ? sidebar.selectionCursorPath
                                                                         : sidebar.currentPath)
        if (anchorIdx < 0)
            anchorIdx = cursorIdx >= 0 ? cursorIdx : 0
        if (cursorIdx < 0)
            cursorIdx = anchorIdx
        cursorIdx = Math.max(0, Math.min(order.length - 1, cursorIdx + direction))
        sidebar.lastClickedPath = order[anchorIdx]
        sidebar.selectionCursorPath = order[cursorIdx]
        const lo = Math.min(anchorIdx, cursorIdx)
        const hi = Math.max(anchorIdx, cursorIdx)
        sidebar.selectedPaths = order.slice(lo, hi + 1)
        sidebar.scrollNoteIntoView(order[cursorIdx])
    }

    // Ctrl+Up/Down: jump to the boundary of the current group — the first/
    // last note sharing the current row's parent folder. Already at that
    // boundary? Step one note into the adjacent group (its near boundary), so
    // repeated presses walk group-by-group instead of dead-ending. Opens the
    // landed note like a plain move.
    function jumpToGroupBoundary(direction) {
        const order = sidebar.visibleNotePaths()
        if (order.length === 0)
            return
        let idx = order.indexOf(sidebar.currentPath)
        if (idx < 0) {
            sidebar.selectAndOpenNote(order[direction > 0 ? 0 : order.length - 1])
            return
        }
        const parent = sidebar.parentDir(order[idx])
        let j = idx
        while (true) {
            const k = j + direction
            if (k < 0 || k > order.length - 1)
                break
            if (sidebar.parentDir(order[k]) !== parent)
                break
            j = k
        }
        if (j === idx) {
            // Already at this group's edge — hop into the neighbouring group.
            const k = idx + direction
            if (k >= 0 && k <= order.length - 1)
                j = k
        }
        sidebar.selectAndOpenNote(order[j])
    }

    // Ctrl+Shift+Up/Down: the Shift analog of jumpToGroupBoundary — extend the
    // selection to the current group's boundary instead of collapsing onto it.
    // The anchor (lastClickedPath) stays fixed while the moving cursor
    // (selectionCursorPath) jumps to the first/last note sharing the CURSOR's
    // parent folder (then hops into the adjacent group on a repeat, exactly like
    // the plain group jump), selecting everything swept in between. Walks from
    // the moving cursor, not currentPath, so it composes with a Shift+arrow
    // range already in progress. Does NOT open a file (matches
    // extendNoteSelection / Shift-click).
    function extendNoteSelectionToGroupBoundary(direction) {
        const order = sidebar.visibleNotePaths()
        if (order.length === 0)
            return
        let anchorIdx = order.indexOf(sidebar.lastClickedPath !== "" ? sidebar.lastClickedPath
                                                                      : sidebar.currentPath)
        let cursorIdx = order.indexOf(sidebar.selectionCursorPath !== "" ? sidebar.selectionCursorPath
                                                                         : sidebar.currentPath)
        if (anchorIdx < 0)
            anchorIdx = cursorIdx >= 0 ? cursorIdx : 0
        if (cursorIdx < 0)
            cursorIdx = anchorIdx
        // Same boundary walk as jumpToGroupBoundary, but from the moving cursor.
        const parent = sidebar.parentDir(order[cursorIdx])
        let j = cursorIdx
        while (true) {
            const k = j + direction
            if (k < 0 || k > order.length - 1)
                break
            if (sidebar.parentDir(order[k]) !== parent)
                break
            j = k
        }
        if (j === cursorIdx) {
            // Already at this group's edge — hop into the neighbouring group.
            const k = cursorIdx + direction
            if (k >= 0 && k <= order.length - 1)
                j = k
        }
        sidebar.lastClickedPath = order[anchorIdx]
        sidebar.selectionCursorPath = order[j]
        const lo = Math.min(anchorIdx, j)
        const hi = Math.max(anchorIdx, j)
        sidebar.selectedPaths = order.slice(lo, hi + 1)
        sidebar.scrollNoteIntoView(order[j])
    }

    // Backs the Up/Down Shortcut items near Ctrl+F's: moves to the
    // previous/next NOTE (audio file or orphaned .md; directories are
    // skipped) relative to sidebar.currentPath, in on-screen order — and
    // scrolls the newly-selected row into view if it was offscreen (a
    // keyboard-driven selection change with no visible feedback would be
    // indistinguishable from doing nothing).
    function moveNoteSelection(direction) {
        if (sidebar.searchQuery !== "") {
            // Flat mode (searchResultsView) — already leaf-only, in display
            // order, no tree walk needed.
            const results = sidebar.searchResultsList
            if (!results || results.length === 0)
                return
            let idx = results.findIndex(r => r.path === sidebar.currentPath)
            idx = idx < 0 ? (direction > 0 ? 0 : results.length - 1)
                          : Math.max(0, Math.min(results.length - 1, idx + direction))
            sidebar.activateNote(results[idx].path, results[idx].isOrphanedMd)
            // Item 41: a plain arrow drives selection exactly like a plain
            // click — collapse the selection (and both range endpoints) onto
            // the single landed note.
            sidebar.selectedPaths = [results[idx].path]
            sidebar.lastClickedPath = results[idx].path
            sidebar.selectionCursorPath = results[idx].path
            // Contain (not Visible): scrolls minimally so the row is FULLY
            // in view, not just barely clipped-in — matches the row the
            // outline just jumped to rather than leaving it half-hidden.
            searchResultsView.positionViewAtIndex(idx, ListView.Contain)
            return
        }

        // Walks TreeView's own visible-row order (tree.index(row, 0) — NOT
        // modelIndex(row, column), which is deprecated in favor of this
        // exact overload as of this Qt version), not a raw tree traversal —
        // a collapsed directory's children must be skipped exactly as they
        // are onscreen, same as what a click could actually reach. Keeps
        // each note's TreeView row alongside its path so the row is
        // available below for positionViewAtRow, without a second scan to
        // re-find it.
        const notes = []
        for (let r = 0; r < tree.rows; r++) {
            const rowIndex = tree.index(r, 0)
            const isAudio = searchProxy.data(rowIndex, VaultTreeModel.IsAudioRole)
            const isOrphanedMd = searchProxy.data(rowIndex, VaultTreeModel.IsOrphanedMdRole)
            if (isAudio || isOrphanedMd) {
                notes.push({
                    path: searchProxy.data(rowIndex, VaultTreeModel.PathRole),
                    isOrphanedMd: isOrphanedMd,
                    row: r
                })
            }
        }
        if (notes.length === 0)
            return
        let idx = notes.findIndex(n => n.path === sidebar.currentPath)
        idx = idx < 0 ? (direction > 0 ? 0 : notes.length - 1)
                      : Math.max(0, Math.min(notes.length - 1, idx + direction))
        sidebar.activateNote(notes[idx].path, notes[idx].isOrphanedMd)
        // Item 41: plain arrow == plain click for selection — single-select
        // the landed note and collapse both range endpoints onto it.
        sidebar.selectedPaths = [notes[idx].path]
        sidebar.lastClickedPath = notes[idx].path
        sidebar.selectionCursorPath = notes[idx].path
        tree.positionViewAtRow(notes[idx].row, TableView.Contain)
    }
    onSearchQueryChanged: {
        sidebar.refreshSearchResults()
        // The visible note set is an input to ribbon eligibility (see
        // refreshSelectionEligibility); re-derive when it changes.
        sidebar.refreshSelectionEligibility()
        // Item 3: entering/leaving text search swaps which flattened order
        // visibleNotePaths() walks (searchResultsList vs. the TreeView row
        // walk) — a Shift-range anchor or selection built in one context
        // doesn't map cleanly onto the other, so start fresh rather than
        // risk a stale/confusing carry-over.
        sidebar.selectedPaths = []
        sidebar.lastClickedPath = ""
        sidebar.selectionCursorPath = ""
        // The tree swaps out for the flat results list (and back) on this
        // change, destroying the row being renamed along with it.
        sidebar.cancelRename()
    }
    onSearchIncludeTranscriptionsChanged: {
        sidebar.refreshSearchResults()
        sidebar.refreshSelectionEligibility()
    }
    Connections {
        // Vault contents changed (file added/removed/renamed) — re-run the
        // search against the fresh tree. sidebar.model is itself the
        // VaultTreeModel (searchProxy.sourceModel via the alias), and
        // QAbstractItemModel::modelReset is a plain Qt signal any
        // Connections can bind to, same as searchProxy's own onModelReset
        // above.
        target: sidebar.model
        function onModelReset() {
            sidebar.refreshSearchResults()
            sidebar.refreshSelectionEligibility()
            sidebar.refreshTreeOrdering()
            // Item 3: vault contents changed (file added/removed/renamed) —
            // a stale selectedPaths entry could point at a file that no
            // longer exists, so drop the selection defensively rather than
            // risk bulk actions silently no-op'ing on a vanished path.
            sidebar.selectedPaths = []
            sidebar.lastClickedPath = ""
            sidebar.selectionCursorPath = ""
            // Item 40 (drag-to-move): a reset recreates every delegate,
            // including the one whose DragHandler is driving an in-flight
            // drag. The vault watcher fires on any vault change (a
            // transcription finishing mid-drag, say), so this isn't only the
            // drag's own move landing — abandon the gesture rather than leave
            // rowDragGhost stranded on screen with no handler left to end it.
            if (sidebar.rowDragActive)
                sidebar.endRowDrag(false)
            // Inline rename: a reset destroys the delegate holding the editor
            // (and the half-typed text in it), so the gesture is over whether
            // we like it or not — drop the state rather than leave renamingPath
            // pointing at a row that may not even exist anymore. This is also
            // the normal exit for a rename that SUCCEEDED: renameFile() /
            // renameFolder() rebuild, and commitActiveRename() has already
            // cleared the state by then, so this is a no-op in that case.
            sidebar.cancelRename()
        }
        // Undo-of-delete restore signals (emitted AFTER the rebuild's modelReset
        // — see VaultTreeModel::applyRestoreOutcome, which rebuilds THEN emits).
        // onModelReset above just cleared the selection; these put it back onto
        // the restored file so it's a real, Shift-anchorable selection and not
        // just the currentPath outline. `fileUrl` follows each signal's
        // convention that the user-visible note is the AUDIO (or the orphaned
        // .md), which is exactly what visibleNotePaths()/selectedPaths key on.
        function onFileRestored(fileUrl, isOrphanedMd) {
            sidebar.selectRestoredNote(fileUrl.toString().replace("file://", ""))
        }
        function onTranscriptRestored(fileUrl) {
            sidebar.selectRestoredNote(fileUrl.toString().replace("file://", ""))
        }
        function onAudioRestoredForTranscript(fileUrl, transcriptUrl) {
            sidebar.selectRestoredNote(fileUrl.toString().replace("file://", ""))
        }
    }

    // P6: push idiom for job-status (queued/active/paused/etc.) —
    // jobStatusForPath() is a plain Q_INVOKABLE lookup, not a reactive
    // property, so delegates need an explicit nudge (via jobStatusRevision
    // below) whenever the controller says some path's resolved status may have
    // changed. (Label/tags/etc. used to have a matching metadataRevision, now
    // gone: the delegate binds VaultTreeModel::LabelRole directly, refreshed
    // reactively by the model.refreshFile() call below.)
    property int jobStatusRevision: 0
    Connections {
        target: sidebar.transcriptionController
        // The queue is the only thing that can change which combines are
        // pending — a combine being enqueued, starting, or finishing all land
        // here. setPendingCombines() no-ops unless the set genuinely changed,
        // which is what makes it safe to call on a signal this chatty.
        function onQueueChanged() { sidebar.pushPendingCombines() }
        // File rename is an instant-lane task (see performRename): clear the
        // optimistic override once the rename has actually landed on disk — by
        // now the file-watcher rebuild is showing the real new name, so
        // releasing the override can't reintroduce the flash. On failure,
        // surface the error and drop the override so the row reverts to the
        // (unchanged) on-disk name.
        function onFileRenamed(oldUrl, newUrl, ok, error) {
            if (!ok)
                Notifications.error(error === "" ? qsTr("Couldn't rename — name taken or invalid.") : error)
            sidebar.clearOptimisticRename()
            // In-place rename (VaultTreeModel::applyRenameOutcome) updates the
            // tree WITHOUT a modelReset, so onModelReset never re-runs
            // refreshTreeOrdering — and fileSortKeys is keyed by PATH, so the
            // renamed file's NEW path has no key and drops to the proxy's
            // keyless tier (sorts after every audio file — the row visibly
            // slides down under an mtime sort though its mtime never changed).
            // Recompute the keys from the fresh tree. Success only: a refused
            // rename left every path unchanged.
            if (ok)
                sidebar.refreshTreeOrdering()
        }
        // File move is an instant-lane task like rename (see performRowMove).
        // On failure, surface the error; success needs nothing here — the
        // file-watcher rebuild shows the file in its new folder, and the undo
        // snackbar (auto-wired from undoAvailable) offers a one-click move-back.
        function onFileMoved(oldUrl, newUrl, ok, error) {
            if (!ok)
                Notifications.error(error === "" ? qsTr("Couldn't move — name taken or invalid.") : error)
        }
        // Async trash completed (controller path): whatever went to the trash
        // already offered its Undo via the generic undo snackbar (undoAvailable);
        // here we only prompt for the entries the filesystem REFUSED (still on
        // disk, untouched), exactly as the synchronous handleTrashResult did.
        // scope ("files"/"transcript"/"audioOnly"/"folder") carries through to
        // confirmFileDelete so the right permanent-delete runs.
        function onTrashResult(scope, untrashable) {
            sidebar.promptPermanentDelete(untrashable, scope)
        }
        // Folder rename/move are instant-lane tasks (FolderRenameTask/
        // FolderMoveTask). Clear the optimistic rename override once the rename
        // lands (deferred to avoid the flash — see performRename's isDir branch);
        // surface an error on failure.
        function onFolderRenamed(oldUrl, newUrl, ok, error) {
            if (!ok)
                Notifications.error(error === "" ? qsTr("Couldn't rename folder — name taken or invalid.") : error)
            sidebar.clearOptimisticRename()
        }
        function onFolderMoved(oldUrl, newUrl, ok, error) {
            if (!ok)
                Notifications.error(error === "" ? qsTr("Couldn't move folder — name taken or invalid.") : error)
        }
        function onFinished(fileUrl, ok, error, structural) {
            // A STRUCTURAL task (rename/move/import/combine/grab — see
            // Task::isStructuralFileOp) moved files, so the vault watcher's
            // rebuild is about to emit a full modelReset, and onModelReset below
            // already re-runs BOTH of these plus expandAll(). Doing them here
            // too is what reflowed the panel twice for one rename: a re-sort now,
            // then the authoritative reset ~50ms later. Stand down and let the
            // reset own it. In-place edits (label/tags/chapters/speaker) never
            // produce a reset, so for them this stays their only reflow trigger.
            if (!structural) {
                // COALESCED, not called directly: finished() arrives one per
                // task, and a bulk queue operation emits a whole burst of them
                // in a single synchronous pass (cancelling a 240-task queue
                // synthesizes 240 terminal reports). Reflowing per report ran
                // the vault walk + proxy re-sort 240 times for one user action
                // and froze the GUI thread. The burst collapses to one reflow.
                sidebar.refreshTreeOrderingCoalesced()
                // A label/tag/transcript change can change what the active search
                // matches, so re-run it (was previously driven by metadataRevision).
                sidebar.refreshSearchResultsCoalesced()
                sidebar.refreshSelectionEligibility() // already coalesced
            }
            // Item 6: labels/tags/chapters are written in-place into an
            // already-existing .md, which QFileSystemWatcher::directoryChanged
            // never fires for — so the tree row would stay stale until some
            // unrelated directory event (e.g. the NEXT file's .md being
            // created) triggered a rebuild ("delayed by one"). Nudge the model
            // to re-query just this row now that its file finished writing —
            // this is also what refreshes the delegate's bound LabelRole.
            if (ok && sidebar.model && sidebar.model.refreshFile)
                sidebar.model.refreshFile(fileUrl)
        }
        function onJobStatusesChanged() {
            sidebar.jobStatusRevision += 1
            // Eligibility is a function of on-disk transcription state, which a
            // finishing job changes under a selection that never moved — the
            // ribbon must re-derive rather than wait for the next click.
            sidebar.refreshSelectionEligibility()
            // Deliberately does NOT call refreshTreeOrdering() (it used to).
            // Job status is queue state (queued/active/paused/...); tree
            // ORDER is a pure function of the filesystem + settings —
            // allAudioPaths(), detectCombineGroups() (whose g.status is a
            // combine-group status read off disk, NOT a queue status),
            // audioLastModified() and AppSettings.sortMode. No queue
            // transition can change any of those, so the call could only ever
            // recompute an identical answer.
            //
            // It was not cheap: a full vault walk + a QFileInfo stat per rule
            // per audio file inside findCombineGroups(), + audioLastModified()
            // re-statting every file, + a JS sort, and then fileSortKeys =
            // ... invalidates the proxy, re-running its whole O(n log n) sort.
            // All of that fired on EVERY status tick, i.e. repeatedly
            // throughout any transcription queue run — the one time the app is
            // already busy.
            //
            // The genuine triggers remain: onFinished above (a job's write
            // really can change ordering — e.g. a group becoming
            // alreadyTranscribed) and onModelReset (the watcher saw the
            // directory change).
        }
    }

    // Combine placeholders (see VaultTreeModel::setPendingCombines()): the
    // combines that are queued or running but haven't landed on disk yet, as
    // [{outputPath, sources}]. Derived from the controller's `jobs` list rather
    // than from a bespoke C++ property — a combine job already carries exactly
    // this (its fileUrl IS the output path, and combineSourcePaths is the
    // ordered part list), so there's nothing for the controller to add.
    //
    // Queued AND active, not just active: the whole point is that the tree
    // reacts the moment you press Combine, and a job sitting behind other work
    // is precisely the case where the wait is longest. Finished jobs (done/
    // failed) drop out, which is what retires the placeholder — see
    // setPendingCombines().
    function pendingCombines() {
        // `jobs` is a real Q_PROPERTY on a live controller, but a test harness
        // (or the window between wiring and first read) can hand us a
        // controller whose jobs isn't populated yet — guard the list, not just
        // the controller, so .filter is never called on undefined.
        const jobs = sidebar.transcriptionController ? sidebar.transcriptionController.jobs : null
        if (!jobs)
            return []
        return jobs
            .filter((j) => j.kind === "combine"
                    && (j.status === "queued" || j.status === "active"))
            .map((j) => ({
                outputPath: String(j.fileUrl).replace("file://", ""),
                sources: j.combineSourcePaths
            }))
    }

    function pushPendingCombines() {
        if (sidebar.model)
            sidebar.model.setPendingCombines(sidebar.pendingCombines())
    }

    // Item 51: path -> outputPath, for every file currently a member of a
    // "ready" (would-combine-if-run) group per detectCombineGroups() — drives
    // both the row highlight and the per-group merge button below. Unlike
    // untranscribedPaths/transcribedPaths/chaptersPaths above (populated
    // bottom-up by each realized delegate reporting its OWN eligibility),
    // group membership can't be known from a single file in isolation — this
    // is a GLOBAL computation over the whole file list, same shape as
    // SettingsPopup.qml's combineOverlapWarnings, refreshed imperatively on
    // the same class of triggers (rules changed, vault contents changed, a
    // job finished) rather than a declarative binding (detectCombineGroups()
    // is a plain Q_INVOKABLE with no reactive dependency tracking of its
    // own). combineReadyPaths itself IS a genuine QML property with a real
    // NOTIFY signal, though, so treeDelegate.combineEligible below can bind
    // to it directly — no revision-counter push idiom needed for that part.
    property var combineReadyPaths: ({})
    // Bug: combineTopMerged/combineBottomMerged used to be derived from
    // sidebar.rowPaths, a row-number -> path map pushed by each realized
    // TreeView delegate purely so a row could ask its VISUAL neighbors'
    // paths. That only ever reflected whichever rows TreeView happened to
    // have realized (reuseItems recycles a small fixed pool) — for any group
    // bigger than the realized pool, an interior row's neighbor lookup could
    // come back "unknown" and get conservatively treated as a group
    // boundary, drawing a full 4-sided box mid-run instead of a suppressed
    // seam.
    //
    // Fixed that, but a real vault then exposed a SECOND bug from the same
    // root cause: adjacency was computed from allAudioPaths()'s raw
    // (dirs-first, alphabetical) order, which is only ONE possible tree
    // order now that searchProxy can reorder files by sortMode/combine-group
    // clustering (see refreshTreeOrdering() below) — a combine rule whose
    // key doesn't happen to match the alphabetical grouping (e.g. this app's
    // own "acme mic 3" rule: grouped by date+edit/orig, NOT by MIC number, so
    // alphabetical order alternates edit/orig every row) left same-group
    // files almost never adjacent, so nearly every member rendered its own
    // isolated box instead of one merged run. Both combineTopMergedPaths/
    // combineBottomMergedPaths AND the tree's own visual order now come from
    // the SAME sorted array (see refreshTreeOrdering()), so they can't drift
    // apart again.
    property var combineTopMergedPaths: ({})
    property var combineBottomMergedPaths: ({})
    // path -> 1-based index in its group's combine order (group.files order,
    // i.e. the order files are actually concatenated in — see
    // combineOrderBadge below). Populated for every 2+-member group
    // regardless of status (same sweep as clusterKeyOf/groupFileOrder in
    // TreeOrdering::compute() — the badge itself is still only ever shown for
    // "ready" groups via treeDelegate.combineEligible, but reusing the one
    // all-statuses map here is what keeps this in lockstep with the tree's
    // own intra-group display order, which also needs an order for
    // non-ready groups to sort consistently).
    property var combineOrderPaths: ({})

    // Burst-collapsing twin of refreshTreeOrdering() below, for callers driven
    // by a PER-TASK signal (onFinished) rather than a user gesture or a model
    // reset — same idiom as refreshSelectionEligibility(). refreshTreeOrdering()
    // is the most expensive routine in the panel (vault walk + a stat sweep + a
    // JS sort + a fileSortKeys write that re-runs the proxy's whole O(n log n)
    // sort), so running it once per finished task is what turns a bulk queue
    // operation into a multi-second freeze.
    property bool treeOrderingRefreshPending: false
    function refreshTreeOrderingCoalesced() {
        if (sidebar.treeOrderingRefreshPending)
            return
        sidebar.treeOrderingRefreshPending = true
        Qt.callLater(sidebar.doRefreshTreeOrderingCoalesced)
    }
    function doRefreshTreeOrderingCoalesced() {
        sidebar.treeOrderingRefreshPending = false
        sidebar.refreshTreeOrdering()
    }

    // Sort mode / combine-candidate clustering (see AppSettings.sortMode's
    // doc comment and CombineGroupOutline usage below): computed once here,
    // covering three concerns that all boil down to "what order do files
    // appear in" and so have to be computed together to stay consistent —
    // see refreshTreeOrdering()'s body for how each is derived.
    //
    // The ~150 lines of derivation this used to hold now live in
    // TreeOrdering::compute() (src/core/TreeOrdering.cpp) — read that file for
    // how each output is built and why. It moved because sortKeys' encoding
    // has to mirror VaultSearchProxyModel::lessThan() exactly, and that
    // contract is far easier to keep honest with producer and consumer in the
    // same directory and the same language.
    //
    // What stayed here is deliberate: the INPUT gathering (each source is
    // guarded/short-circuited exactly as before) and the five property
    // assignments, in the same order, at the same call sites. Assigning
    // searchProxy.fileSortKeys re-enters VaultSearchProxyModel's invalidate,
    // which has a use-after-free history when it lands mid source-model reset
    // — so the number and timing of these writes must not change.
    function refreshTreeOrdering() {
        const audioPaths = (sidebar.transcriptionController && sidebar.model) ? sidebar.model.allAudioPaths() : []
        const groups = (audioPaths.length > 0 && AppSettings.combineRules.length > 0)
                ? sidebar.transcriptionController.detectCombineGroups(AppSettings.combineRules, audioPaths) : []
        // mtimes is only fetched when actually needed — audioLastModified()
        // walks the whole tree, no need to pay for that on the (default)
        // "name" mode. TreeOrdering::compute() never reads it in that mode,
        // so an empty map is the correct thing to hand it.
        const mtimes = (AppSettings.sortMode !== "name" && sidebar.model) ? sidebar.model.audioLastModified() : {}

        const ordering = TreeOrdering.compute(audioPaths, mtimes, groups,
                                              sidebar.pendingCombines(), AppSettings.sortMode)

        sidebar.combineReadyPaths = ordering.readyPaths
        sidebar.combineOrderPaths = ordering.orderPaths
        sidebar.combineTopMergedPaths = ordering.topMerged
        sidebar.combineBottomMergedPaths = ordering.bottomMerged
        searchProxy.fileSortKeys = ordering.sortKeys

        // A group whose newest member is still inside the combine stability
        // window reads as "pending", not "ready", so it's absent from
        // combineReadyPaths and shows no candidate outline. "pending" is the
        // only status that resolves by the passage of time alone, and nothing
        // else re-runs this scan once the file stops changing — so without
        // this, a just-arrived group stays a non-candidate until some
        // unrelated event (a job finishing, a rule edit, a restart) happens to
        // recompute ordering, i.e. the outline lags one filesystem event
        // behind. Re-run ourselves once the window elapses so it settles on
        // its own; the timer self-cancels once no group is pending.
        const anyPending = groups.some((g) => g.status === "pending")
        if (anyPending)
            combinePendingRescanTimer.restart()
    }
    // pushPendingCombines() too, not just on the queueChanged above: a combine
    // job survives a restart (the queue is persisted), so the tree can open
    // with one already pending and no queue transition coming to announce it.
    Component.onCompleted: {
        sidebar.pushPendingCombines()
        sidebar.refreshTreeOrdering()
    }
    Connections {
        target: AppSettings
        function onCombineRulesChanged() { sidebar.refreshTreeOrdering() }
        function onSortModeChanged() { sidebar.refreshTreeOrdering() }
    }

    // Passive combine re-scan: fires once, ~just past the stability window, to
    // re-evaluate a group that refreshTreeOrdering() last saw as "pending"
    // (newest member too freshly written). Interval is the C++ scan's own
    // window plus a 1s margin so the file's mtime is safely past it when we
    // re-scan. See refreshTreeOrdering()'s pending check.
    Timer {
        id: combinePendingRescanTimer
        repeat: false
        interval: (sidebar.transcriptionController
                   ? sidebar.transcriptionController.combineStabilityWindowSecs() : 5) * 1000 + 1000
        onTriggered: sidebar.refreshTreeOrdering()
    }

    // Item 51 refinement: the per-group color (combineGroupColor) moved to
    // TextFormat.js — a pure outputPath -> hue hash with no component state.

    // Item 51 refinement: path of the combine group currently under the
    // pointer (empty when none), pushed by whichever treeDelegate's rowHover
    // enters/leaves a combineEligible row — same push idiom as
    // untranscribedPaths/etc. below. Drives the per-group merge button
    // (rendered once per group, not once per row — see groupMergeButton)
    // so hovering ANY member reveals it, instead of the button living in
    // each row's already-busy per-file action cluster.
    property string hoveredCombineGroup: ""
    // Bug (user report, 2026-07-13): moving the pointer directly from one
    // group member row to an ADJACENT one made the button disappear instead
    // of staying up — hoveredCombineGroup was a plain last-write-wins string,
    // so whichever row's onHoveredChanged(hovered: false) fired LAST always
    // cleared it, even when a same-group sibling's onHoveredChanged(hovered:
    // true) had already fired first (Qt Quick doesn't guarantee the old
    // row's leave fires before the new row's enter). A per-group reference
    // count fixes this: entering increments this group's count and (re)sets
    // hoveredCombineGroup; leaving decrements it and only clears
    // hoveredCombineGroup once the count actually reaches 0, so a leave that
    // follows a same-group sibling's enter no longer wins.
    property var hoveredCombineGroupCounts: ({})
    function enterCombineGroupHover(outputPath) {
        sidebar.hoveredCombineGroupCounts[outputPath] = (sidebar.hoveredCombineGroupCounts[outputPath] || 0) + 1
        sidebar.hoveredCombineGroup = outputPath
    }
    function leaveCombineGroupHover(outputPath) {
        const next = (sidebar.hoveredCombineGroupCounts[outputPath] || 0) - 1
        if (next > 0) {
            sidebar.hoveredCombineGroupCounts[outputPath] = next
        } else {
            delete sidebar.hoveredCombineGroupCounts[outputPath]
            if (sidebar.hoveredCombineGroup === outputPath)
                sidebar.hoveredCombineGroup = ""
        }
    }

    // Which directory row the pointer is inside right now ("" = none). Pushed
    // by each directory row's own HoverHandler, because a folder highlight
    // covers the folder AND its contents (see treeDelegate.inFolderHighlight)
    // and a descendant row has no way to see an ANCESTOR's hover state from
    // its own handlers.
    //
    // Guarded clear rather than an unconditional one: moving the pointer
    // straight from folder A to folder B can deliver B's enter before A's
    // leave, and clearing unconditionally on A's leave would then wipe B's
    // highlight — the same last-write-wins trap hoveredCombineGroup documents
    // above. Only one directory row can be hovered at a time (rows don't
    // overlap), so unlike hoveredCombineGroup this needs no reference count.
    property string hoveredFolderPath: ""
    function enterFolderHover(path) {
        sidebar.hoveredFolderPath = path
    }
    function leaveFolderHover(path) {
        if (sidebar.hoveredFolderPath === path)
            sidebar.hoveredFolderPath = ""
    }

    // Folder containment (isUnderFolder) is a pure path-prefix test — moved to
    // TextFormat.js, called directly by TreeRowDelegate.qml.

    // The path at a given TreeView row, or "" out of range. rowCount is passed
    // in rather than read off tree.rows in here so that a BINDING calling this
    // takes a real dependency on the row count: tree.index()/searchProxy.data()
    // are plain function calls that create no dependency of their own, so a
    // binding that didn't mention rows would go stale the moment an
    // expand/collapse inserted or removed rows underneath it (rowCount is used
    // for the bounds check, so it's a genuine argument, not a dummy dependency
    // that a QML compiler could optimize away).
    function pathAtTreeRow(row, rowCount) {
        if (row < 0 || row >= rowCount)
            return ""
        return searchProxy.data(tree.index(row, 0), VaultTreeModel.PathRole) || ""
    }

    // The nesting depth at a given TreeView row, or -1 out of range. Same
    // rowCount-as-genuine-argument idiom as pathAtTreeRow above — tree.depth()
    // is a plain invokable with no dependency of its own, so a binding calling
    // this must take its row-count dependency through the argument.
    function depthAtTreeRow(row, rowCount) {
        return (row < 0 || row >= rowCount) ? -1 : tree.depth(row)
    }

    // The ribbon's bulk targets: the CURRENT SELECTION intersected with each
    // action's eligibility rule.
    //
    // Bug (same class as combineReadyPaths' rowPaths bug below): these used to
    // be untranscribedPaths/transcribedPaths/chaptersPaths, path -> true maps
    // pushed bottom-up by each realized TreeView delegate. That only ever
    // described whichever rows TreeView happened to have realized, and it was
    // wrong in BOTH directions at once: reuseItems recycles a fixed delegate
    // pool, so a pooled Item reassigned from an eligible row to another
    // eligible row saw NO change in its eligibility binding, never fired
    // onXEligibleChanged, and so never registered the new path (while the old
    // path's key leaked, since Component.onDestruction doesn't run for a
    // pooled reuse). Net effect: select a perfectly eligible file and the
    // ribbon button sat there dim.
    //
    // Derived from the MODEL instead, via the same searchResultNotes() walk
    // (all proxy rows, or the flat searchResultsList in text-search mode) that
    // already backs the search-scoped mirrors below — one source of truth for
    // each rule, and realization-independent by construction.
    //
    // Plain properties + an explicit refresh, NOT a binding that reads a
    // revision counter as a dependency trick — same idiom (and same
    // qmlcachegen reason) as searchResultsList/refreshSearchResults() above.
    property var selectionTranscribePaths: []
    property var selectionRelabelPaths: []
    property var selectionChaptersPaths: []
    property var selectionRunAllPaths: []
    property bool selectionEligibilityRefreshPending: false
    // DEFERRED + coalesced, deliberately: the search*Paths() helpers walk
    // searchProxy.data(...) over every proxy row, and several callers fire
    // during a model reset (the vault-reset handler below, a job finishing, a
    // selection cleared by a rebuild). Reading the proxy WHILE its source reset
    // is in flight walks freed Nodes (the proxy-invalidate-during-reset UAF
    // class — see VaultSearchProxyModel). Qt.callLater pushes the walk to the
    // next event-loop turn, after any in-flight reset has fully settled, and
    // the pending flag collapses a burst of triggers into one walk.
    function refreshSelectionEligibility() {
        if (sidebar.selectionEligibilityRefreshPending)
            return
        sidebar.selectionEligibilityRefreshPending = true
        Qt.callLater(sidebar.doRefreshSelectionEligibility)
    }
    function doRefreshSelectionEligibility() {
        sidebar.selectionEligibilityRefreshPending = false
        const selected = sidebar.selectedPaths
        if (selected.length === 0) {
            // Nothing selected → every narrowed set is empty; skip the proxy
            // walk entirely (also the common case right after a reset clears
            // the selection).
            sidebar.selectionTranscribePaths = []
            sidebar.selectionRelabelPaths = []
            sidebar.selectionChaptersPaths = []
            sidebar.selectionRunAllPaths = []
            return
        }
        const narrow = (paths) => paths.filter((p) => selected.indexOf(p) >= 0)
        sidebar.selectionTranscribePaths = narrow(sidebar.searchTranscribePaths())
        sidebar.selectionRelabelPaths = narrow(sidebar.searchRelabelPaths())
        sidebar.selectionChaptersPaths = narrow(sidebar.searchChaptersPaths())
        sidebar.selectionRunAllPaths = narrow(sidebar.searchRunAllPaths())
    }
    onSelectedPathsChanged: sidebar.refreshSelectionEligibility()

    // TitleBar's "⋯" menu action: unlike transcribeAllButton (which targets
    // the current SELECTION narrowed to untranscribed audio, see
    // selectionTranscribePaths), this queues every audio file in the vault,
    // including already-transcribed ones — VaultTreeModel.allAudioPaths()
    // walks the full tree directly rather than relying on realized
    // delegates. transcribe()'s only in-flight gate is isQueued(), so this
    // unconditionally re-runs each file (see TranscriptionController.h).
    function forceRetranscribeAll() {
        if (!sidebar.transcriptionController || sidebar.vaultPath.toString() === "")
            return
        const paths = sidebar.model.allAudioPaths()
        if (paths.length === 0)
            return
        const urls = paths.map((p) => "file://" + p)
        sidebar.transcriptionController.transcribeAll(urls, sidebar.vaultPath, AppSettings.defaultModel,
                                                        AppSettings.autoLabelEnabled, AppSettings.restrictTagsToExisting,
                                                        AppSettings.tagReuseBias, AppSettings.wordiness)
    }

    // Item 40 (folder management): right-click context menu over tree rows /
    // empty tree area to create + delete folders and move a file into another
    // folder. Custom Popups in the same style as the sortModeMenu above and
    // TitleBar.qml's moreActionsMenu — this codebase uses no QQC2 Menu, just a
    // Popup with plain Item rows. State is captured at right-click time; the
    // shared menus below read it.
    property string ctxPath: ""       // the right-clicked row's own path ("" for empty area)
    property bool ctxIsDir: false
    property bool ctxIsNote: false    // audio file or orphaned .md
    property bool ctxIsAudio: false   // Item 2: audio file specifically (gates "Delete transcript")
    property int ctxStatus: 0         // Item 2: VaultTreeModel::Status of the row (2 == Transcribed)
    // Directory a "New folder" gets created inside: the row itself when a
    // directory is clicked, else the clicked note's parent dir, else the
    // vault root (empty-area right-click).
    property string ctxCreateInDir: ""
    // The label the right-clicked row is displaying ("" if none / not showing
    // one) — "Rename" edits what the row shows, so it has to know whether that
    // was a label or a filename. Captured at right-click time like the rest of
    // this state; the menu can't read the delegate's metadata itself.
    property string ctxLabel: ""
    property real ctxMenuX: 0
    property real ctxMenuY: 0
    // The click-anchored popups (contextMenu / newFolderPopup / moveToPopup /
    // fileDeleteConfirmPopup) all spawn at the cursor via ctxMenuX/ctxMenuY.
    // These are IN-SCENE popups, so Qt won't flip/clamp them to stay on screen
    // — a right-click low or far-right would push the popup past the sidebar
    // edge and clip it under the window frame. Clamp the anchor by the popup's
    // own size so its full extent stays inside the sidebar. Passing the size in
    // (rather than reading popup.width/height globally) keeps each popup's
    // clamp reactive to ITS size, and makes width/height/ctxMenu* all binding
    // dependencies so it re-clamps as the content lays out.
    function clampedMenuX(w) {
        return Math.max(0, Math.min(sidebar.ctxMenuX, sidebar.width - w))
    }
    function clampedMenuY(h) {
        return Math.max(0, Math.min(sidebar.ctxMenuY, sidebar.height - h))
    }
    // Destinations for the "Move to…" picker (from VaultTreeModel.vaultFolders(),
    // minus the file's own current directory).
    property var moveFolderList: []


    function openContextMenu(path, isDir, isNote, isAudio, status, createInDir, label, x, y) {
        sidebar.ctxPath = path
        sidebar.ctxIsDir = isDir
        sidebar.ctxIsNote = isNote
        sidebar.ctxIsAudio = isAudio
        sidebar.ctxStatus = status
        sidebar.ctxCreateInDir = createInDir
        sidebar.ctxLabel = label ? label : ""
        sidebar.ctxMenuX = x
        sidebar.ctxMenuY = y
        contextMenu.open()
    }

    function beginCreateFolder(dirPath) {
        sidebar.ctxCreateInDir = dirPath
        newFolderField.text = ""
        newFolderError.text = ""
        newFolderPopup.open()
        newFolderField.forceActiveFocus()
    }

    function confirmNewFolder() {
        if (!sidebar.model)
            return
        const name = newFolderField.text.trim()
        if (name === "") {
            newFolderError.text = qsTr("Enter a folder name.")
            return
        }
        // Model paths are raw local paths; a Q_INVOKABLE QUrl param needs the
        // file:// scheme or QUrl::toLocalFile() comes back empty (same as the
        // transcribe/relabel calls elsewhere in this file).
        if (sidebar.model.createFolder("file://" + sidebar.ctxCreateInDir, name))
            newFolderPopup.close()
        else
            newFolderError.text = qsTr("Couldn't create folder — invalid name, or it already exists.")
    }

    // Inline rename: which row is currently in edit mode ("" = none). Exactly
    // one row can be, so this is a single path rather than per-delegate state
    // — and it MUST live here rather than on the delegate, because TreeView
    // recycles delegate Items as rows scroll (see treeDelegate.onPathChanged);
    // an `editing` bool on the Item would silently retarget itself onto a
    // different file. Keyed by path, the editor follows the file.
    //
    // StyleGuide.md's "Always-editable text" rule (one always-present editable
    // control, never a display-vs-edit swap) deliberately does NOT apply here
    // — see that doc's "Inline rename in a list/tree row" carve-out. A tree row
    // can't be always-editable: a plain click on the name must open the file,
    // not place a text cursor. What the rule actually guards against is the
    // field vanishing under a focus steal, so the load-bearing half is kept:
    // this is gated on an explicit renamingPath, NEVER on activeFocus, so the
    // native right-click "Copy" menu can't dismiss the editor.
    // The whole rename lives here, not just which row: the editor's live text
    // and the two facts performRename() needs (is it a folder, was the row
    // showing a label). Hoisted for the same reason rowDrag* above is —
    // delegate-local state can't be reached from anywhere else, and a click on
    // a DIFFERENT row has to be able to commit this one. That's not
    // hypothetical: the editor keeps activeFocus through such a click (see
    // treeRowTap), so the blur-commit never fires and the edit would be lost.
    // The field is a view onto this state; this state is the authority.
    property string renamingPath: ""
    property bool renamingIsDir: false
    property string renamingLabel: "" // the label the row displays, "" if none
    property string renamingText: ""  // the editor's current text
    // The scene-coordinate point the rename was launched from, when it began
    // with a click ON the name (treeRowTap) — the editor maps this to a caret
    // position so the cursor lands where the user clicked. null when the rename
    // came from a non-positional entry (context-menu "Rename"), in which case
    // the editor selects the whole name instead (the menu-rename convention).
    property var renamingCaretScenePos: null

    // Item: anti-flash for the inline rename. commitActiveRename() leaves edit
    // mode (clears renamingPath) SYNCHRONOUSLY but defers the actual write to
    // the next tick (renameFile()→rebuild() would destroy the running
    // delegate). In that one-frame gap the row falls back to rowNameText, whose
    // text is still the OLD model name — a visible flash to the old name before
    // the rebuild swaps in the new one. So during that window rowNameText shows
    // this optimistic name for optimisticRenamePath instead of the stale model
    // value; performRename() clears it once the write (and its rebuild/refresh)
    // has landed. Mirrors MainPanel's pendingSpeakerRenames optimistic pattern.
    property string optimisticRenamePath: ""
    property string optimisticRenameName: ""

    // renameInitialText/renameBaseValue/optimisticDisplayName moved to
    // TextFormat.js, where they share ONE extension-stripping primitive
    // (stripExtension/extensionOf) instead of open-coding the `dot > 0` dotfile
    // rule once each. They were gated on AppSettings.useLabelsSidebar and
    // AppSettings.hideExtensionsSidebar, which a shared script can't read — both
    // are explicit trailing booleans now, passed at each call site below.

    function beginRename(path, isDir, label) {
        if (path === "")
            return
        // Starting a rename elsewhere while one is open commits the open one,
        // matching the blur convention.
        if (sidebar.renamingPath !== "" && sidebar.renamingPath !== path)
            sidebar.commitActiveRename()
        // Default to no click point (a menu/programmatic rename selects the whole
        // name). A name-CLICK entry overrides this right after, with the click's
        // scene position, so the caret lands where the pointer did — see
        // treeRowTap. Kept OFF beginRename's parameter list so the C++
        // QMetaObject::invokeMethod call sites stay 3-arg.
        sidebar.renamingCaretScenePos = null
        sidebar.renamingIsDir = isDir === true
        sidebar.renamingLabel = label ? label : ""
        sidebar.renamingText = TextFormat.renameInitialText(path, sidebar.renamingIsDir, sidebar.renamingLabel,
                                                           AppSettings.useLabelsSidebar,
                                                           AppSettings.hideExtensionsSidebar)
        sidebar.renamingPath = path
    }

    function cancelRename() {
        sidebar.renamingPath = ""
        sidebar.renamingText = ""
    }

    // Commits whatever rename is currently open, from the sidebar's own copy of
    // the editor's text — so it works from the editor's Return/blur AND from a
    // click on another row, which never blurs the editor at all.
    //
    // Leaves edit mode IMMEDIATELY (so the blur that follows can't re-enter
    // this) and defers the actual write: renameFile()/renameFolder() call
    // rebuild(), which destroys the very delegate whose handler is running
    // right now — the same hazard the drag-drop handler defers for (see
    // rowDropTarget's Qt.callLater).
    function commitActiveRename() {
        if (sidebar.renamingPath === "")
            return // nothing open, or already committed/cancelled
        const path = sidebar.renamingPath
        const isDir = sidebar.renamingIsDir
        const label = sidebar.renamingLabel
        const newValue = sidebar.renamingText
        // Set the optimistic display BEFORE clearing renamingPath, so the frame
        // where this row falls back to rowNameText already shows the new name
        // (see optimisticRenamePath's doc comment). performRename() clears it.
        const useLabels = AppSettings.useLabelsSidebar
        const hideExt = AppSettings.hideExtensionsSidebar
        const baseValue = TextFormat.renameBaseValue(path, isDir, label, newValue.trim(), useLabels, hideExt)
        const originalBase = TextFormat.renameBaseValue(path, isDir, label,
                                                        TextFormat.renameInitialText(path, isDir, label,
                                                                                     useLabels, hideExt),
                                                        useLabels, hideExt)
        if (baseValue !== "" && baseValue !== originalBase) {
            sidebar.optimisticRenamePath = path
            sidebar.optimisticRenameName = TextFormat.optimisticDisplayName(path, isDir, label, baseValue,
                                                                            useLabels, hideExt)
        }
        sidebar.renamingPath = ""
        sidebar.renamingText = ""
        Qt.callLater(() => sidebar.performRename(path, isDir, label, newValue))
    }

    function clearOptimisticRename() {
        sidebar.optimisticRenamePath = ""
        sidebar.optimisticRenameName = ""
    }

    function performRename(path, isDir, label, newValue) {
        const useLabels = AppSettings.useLabelsSidebar
        const hideExt = AppSettings.hideExtensionsSidebar
        const baseValue = TextFormat.renameBaseValue(path, isDir, label, newValue.trim(), useLabels, hideExt)
        const originalBase = TextFormat.renameBaseValue(path, isDir, label,
                                                        TextFormat.renameInitialText(path, isDir, label,
                                                                                     useLabels, hideExt),
                                                        useLabels, hideExt)
        if (baseValue === "" || baseValue === originalBase) {
            sidebar.clearOptimisticRename()
            return // empty or unchanged — silently keep the old name
        }

        if (isDir) {
            // Folder rename is an instant-lane FolderRenameTask with a controller
            // (async, undoable) — the optimistic override is cleared by
            // onFolderRenamed once the rename lands (deferring it avoids the
            // flash, same as the file-rename path). The synchronous
            // model.renameFolder() is the no-controller fallback (rebuild is
            // immediate, so clear now).
            if (sidebar.transcriptionController) {
                sidebar.transcriptionController.renameFolder("file://" + path, baseValue, sidebar.vaultPath)
                return
            }
            if (sidebar.model && !sidebar.model.renameFolder("file://" + path, baseValue))
                Notifications.error(qsTr("Couldn't rename folder — name taken or invalid."))
            sidebar.clearOptimisticRename()
            return
        }

        // Editing a displayed label rewrites the .md's frontmatter, not the
        // filename. setLabel() emits finished(), which already refreshes the
        // row's LabelRole via model.refreshFile() (see the Connections above) —
        // no rebuild, so nothing extra to nudge here.
        if (AppSettings.useLabelsSidebar && label) {
            if (sidebar.transcriptionController)
                sidebar.transcriptionController.setLabel("file://" + path, baseValue)
            sidebar.clearOptimisticRename()
            return
        }

        // File rename goes through the instant-lane task when a controller is
        // wired (the real app) — visible in the Tasks list, deferred behind a
        // same-file transcription. The optimistic override (set in
        // commitActiveRename) is deliberately NOT cleared here on that path: it
        // must hold the new name until the rename lands on disk and the model
        // rebuilds (async via the file watcher), or the row flashes back to its
        // old model name; onFileRenamed above clears it once the task reports.
        //
        // Sidebar also runs WITHOUT a controller (plain filename mode — see
        // m_controller in tst_sidebar_rename): there, fall back to the
        // synchronous VaultTreeModel.renameFile() (which rebuilds inline, so
        // the override is safe to clear immediately).
        if (sidebar.transcriptionController) {
            sidebar.transcriptionController.renameFile("file://" + path, baseValue)
        } else {
            if (sidebar.model && !sidebar.model.renameFile("file://" + path, baseValue))
                Notifications.error(qsTr("Couldn't rename — name taken or invalid."))
            sidebar.clearOptimisticRename()
        }
    }

    function openMoveTo() {
        const all = sidebar.model ? sidebar.model.vaultFolders() : []
        const ownDir = sidebar.ctxPath.substring(0, sidebar.ctxPath.lastIndexOf("/"))
        sidebar.moveFolderList = all.filter((f) => f !== ownDir)
        moveToPopup.open()
    }

    function moveNoteTo(destDir) {
        // Instant-lane task when a controller is present (async, undoable —
        // see moveFiles()/onFileMoved); the synchronous model call is the
        // no-controller fallback (plain-model harnesses), mirroring
        // performRename()/performRowMove().
        if (sidebar.transcriptionController)
            sidebar.transcriptionController.moveFiles(["file://" + sidebar.ctxPath], "file://" + destDir,
                                                       sidebar.vaultPath)
        else if (sidebar.model)
            sidebar.model.moveFile("file://" + sidebar.ctxPath, "file://" + destDir)
        moveToPopup.close()
    }

    // Item 40 (drag-to-move): left-press-and-drag a tree row onto a folder to
    // move it — the same VaultTreeModel::moveFile()/moveFolder() the "Move to…"
    // context menu calls, reachable as a direct gesture. Rows had no drag
    // gesture at all before, so a left-drag fell through to the TreeView's own
    // flick-to-scroll (user report: "dragging files in the side panel merely
    // drags the whole thing, not allowing you to do any operations"). Drag also
    // reaches moveFolder(), which no UI called until now — "Move to…" is
    // note-only (ctxMoveRow's `visible: sidebar.ctxIsNote`).
    //
    // Drag state lives on the sidebar, not the delegate: TreeView RECYCLES
    // delegates as the view scrolls (see treeDelegate.onPathChanged), so state
    // anchored to the pressed delegate would silently retarget itself onto a
    // different file mid-gesture if the source row scrolled out of the
    // viewport — which the drag autoscroll below makes routine, not a corner
    // case.
    property bool rowDragActive: false
    property var rowDragPaths: []      // one path, or the whole multi-selection
    property bool rowDragIsDir: false
    // Destination directory under the cursor right now ("" = none). Pushed by
    // whichever row's DropArea the ghost is over: a directory row means itself,
    // a note row means its PARENT dir (dropping onto a file drops into that
    // file's folder — standard file-manager behavior), and the tree's empty
    // space below the last row means the vault root (rootDropArea).
    property string rowDragDest: ""
    // Ghost position, in sidebar coordinates.
    property real rowDragX: 0
    property real rowDragY: 0

    // Which paths a row-targeted action applies to: a row that's part of a 2+
    // multi-selection carries the WHOLE selection; anything else carries just
    // that one row and leaves the existing selection alone. Directories are
    // never part of selectedPaths (only notes are selectable — see
    // handleNoteClick), so a folder always acts on itself alone.
    //
    // Shared by the two row actions that can act on many rows at once —
    // drag-to-move (beginRowDrag) and delete (beginDeleteFile) — so both read
    // a multi-selection the same way rather than each inventing its own rule.
    function selectionPathsFor(path, isDir) {
        if (!isDir && sidebar.selectedPaths.length > 1
                && sidebar.selectedPaths.indexOf(path) >= 0)
            return sidebar.selectedPaths.slice()
        return [path]
    }

    // Cheap synchronous pre-check deciding whether a destination highlights and
    // accepts at all. The C++ guards in moveFile()/moveFolder() remain the real
    // authority (vault containment, collisions, sidecar integrity). A folder
    // into itself or a descendant is impossible, so it stays dark and refuses.
    // A drop back onto the row's OWN current directory is offered (it lights up
    // and accepts) so a mid-drag user can change their mind — performRowMove()
    // then makes that drop do nothing. A mixed selection where only SOME rows
    // already live in destDir has no single clean meaning and is refused.
    function canDropRowsOn(destDir) {
        if (!sidebar.rowDragActive || destDir === "")
            return false
        let inDest = 0
        for (let i = 0; i < sidebar.rowDragPaths.length; i++) {
            const p = sidebar.rowDragPaths[i]
            // A folder onto itself or one of its own descendants is impossible —
            // no move could ever satisfy it, so it stays dark and refuses.
            if (sidebar.rowDragIsDir && (destDir === p || destDir.startsWith(p + "/")))
                return false
            if (p.substring(0, p.lastIndexOf("/")) === destDir)
                inDest++
        }
        // Two clean cases light up and accept: a real move (nothing already
        // lives in destDir) and the pure no-op of releasing every row back on
        // its origin folder (everything does). The no-op is short-circuited in
        // performRowMove(), so a user who changed their mind has a safe place to
        // drop — the change-your-mind affordance a same-folder refusal removed.
        // A MIXED selection (only SOME rows already in destDir) has no single
        // clean meaning and stays refused, exactly as before.
        return inDest === 0 || inDest === sidebar.rowDragPaths.length
    }

    function beginRowDrag(path, isDir, scenePoint) {
        sidebar.rowDragPaths = sidebar.selectionPathsFor(path, isDir)
        sidebar.rowDragIsDir = isDir
        sidebar.rowDragDest = ""
        sidebar.rowDragActive = true
        sidebar.updateRowDrag(scenePoint)
        // Imperative Drag.start()/drop()/cancel() rather than binding
        // Drag.active to rowDragActive: the drop has to be DELIVERED before the
        // state clears (dropping active first cancels the drag instead), and
        // Drag.drop() assigns active internally, which would break such a
        // binding permanently on the first drag anyway.
        rowDragGhost.Drag.start()
    }

    function updateRowDrag(scenePoint) {
        // mapFromItem(null, …) maps from SCENE coordinates — the drag handler's
        // centroid is scene-relative, while the ghost is a child of sidebar.
        const p = sidebar.mapFromItem(null, scenePoint.x, scenePoint.y)
        sidebar.rowDragX = p.x
        sidebar.rowDragY = p.y
    }

    function endRowDrag(dropIt) {
        if (!sidebar.rowDragActive)
            return
        if (dropIt && sidebar.canDropRowsOn(sidebar.rowDragDest))
            rowDragGhost.Drag.drop()   // → the accepting DropArea's onDropped
        else
            rowDragGhost.Drag.cancel()
        sidebar.rowDragActive = false
        sidebar.rowDragPaths = []
        sidebar.rowDragDest = ""
    }

    // Runs the moves a drop asked for. paths/isDir/destDir are passed in rather
    // than read off the sidebar's own drag state because endRowDrag() clears
    // that state in the same tick the drop is delivered.
    //
    // Deferred by its caller (see rowDropTarget's onDropped): a successful move
    // calls VaultTreeModel::rebuild(), which destroys and recreates every tree
    // delegate — including the very delegate whose DragHandler/DropArea callback
    // is mid-execution. This codebase has already been bitten by exactly that
    // (a delegate array rebuilt underneath an in-flight click), so the move is
    // pushed to the next event-loop turn, after the gesture has fully unwound.
    function performRowMove(paths, isDir, destDir) {
        if (destDir === "")
            return
        // Change-your-mind no-op: every row already lives directly in destDir
        // (the user released back on the origin folder, which canDropRowsOn now
        // lights up as a target). Nothing to move — and enqueuing a
        // same-directory move would only raise a spurious collision error — so
        // just return.
        let allInDest = true
        for (let i = 0; i < paths.length; i++)
            if (paths[i].substring(0, paths[i].lastIndexOf("/")) !== destDir) {
                allInDest = false
                break
            }
        if (allInDest)
            return
        // With a controller, moves are async instant-lane tasks (visible +
        // undoable): FILE moves collapse into one undo BATCH for the whole drag
        // (a single "Moved N files" snackbar — moveFiles); FOLDER moves are
        // individual FolderMoveTasks (multi-folder drag is rare, so no batch).
        // A refused move surfaces asynchronously via onFileMoved/onFolderMoved.
        // A no-controller harness (tst_sidebar_*) falls back to the synchronous
        // model calls below — same split as performRename().
        if (sidebar.transcriptionController) {
            if (isDir) {
                for (let i = 0; i < paths.length; i++)
                    sidebar.transcriptionController.moveFolder("file://" + paths[i], "file://" + destDir,
                                                               sidebar.vaultPath)
            } else {
                sidebar.transcriptionController.moveFiles(paths.map((p) => "file://" + p), "file://" + destDir,
                                                          sidebar.vaultPath)
            }
            return
        }
        if (!sidebar.model)
            return
        const failed = []
        for (let i = 0; i < paths.length; i++) {
            // Model paths are raw local paths; a Q_INVOKABLE QUrl param needs
            // the file:// scheme or QUrl::toLocalFile() comes back empty (same
            // as confirmNewFolder()/moveNoteTo() above).
            const ok = isDir
                ? sidebar.model.moveFolder("file://" + paths[i], "file://" + destDir)
                : sidebar.model.moveFile("file://" + paths[i], "file://" + destDir)
            if (!ok)
                failed.push(paths[i].split("/").pop())
        }
        // A refused move (most likely a same-named file already in the
        // destination) is otherwise completely silent — the row just stays put.
        // Same snackbar treatment as Main.qml's drag-and-drop import failures,
        // which this gesture is the in-vault sibling of.
        if (failed.length > 0)
            Notifications.error(qsTr("Couldn't move: %1").arg(failed.join(", ")))
    }


    // Item 2 (delete transcript / delete file / delete folder): every delete
    // TRIES THE TRASH FIRST and only asks for confirmation if the trash refuses.
    //
    // The old flow confirmed every delete, because the model's one delete call
    // silently did "trash, or permanently remove if trashing fails" and the UI
    // could never tell which it had gotten — so it had to assume the worst
    // every time, and the prompt cried wolf on the overwhelmingly common
    // recoverable case. Now:
    //
    //   trash succeeded  -> no prompt at all; a snackbar offers Undo instead.
    //                       Confirming a reversible action is just friction.
    //   trash refused    -> the entries are STILL ON DISK, untouched (a failed
    //                       moveToTrash is non-destructive — see trashOnly() in
    //                       VaultTreeModel.cpp), so we can safely prompt now,
    //                       naming exactly the ones that would be permanent.
    //
    // Nothing predicts trash-ability up front; attempting it IS the test, and
    // it's free. So the prompt now means what it says: "this cannot be undone".
    //
    // An ARRAY, not a single path: every file-kind delete acts on a whole set.
    // From the context menu that's the multi-selection when the right-clicked
    // row is part of one (see selectionPathsFor, shared with drag-to-move);
    // from the ribbon's delete buttons and the Delete key it's the selection
    // itself. Only "folder" is ever single-target — trashFolder() is a separate
    // C++ call precisely so a recursive folder delete can't ride along in a
    // file batch (see its doc comment).
    property var pendingDeletePaths: []
    property string pendingDeleteMode: "" // "transcript" | "audioOnly" | "file" | "folder"

    // The single filename a confirm message names, for the 1-target case.
    function pendingDeleteName() {
        if (sidebar.pendingDeletePaths.length !== 1)
            return ""
        const p = sidebar.pendingDeletePaths[0]
        return p.substring(p.lastIndexOf("/") + 1)
    }

    // Opens the permanent-delete confirm prompt for exactly the entries the
    // filesystem refused to trash — the ONLY ones a permanent delete is on the
    // table for (they were left untouched, so nothing has happened to them yet).
    // Shared by the async controller path (onTrashResult) and the synchronous
    // no-controller fallback (handleTrashResult).
    function promptPermanentDelete(untrashable, mode) {
        if (untrashable.length > 0) {
            sidebar.pendingDeletePaths = untrashable.map((u) => u.toString().replace("file://", ""))
            sidebar.pendingDeleteMode = mode
            fileDeleteConfirmPopup.open()
        }
    }

    // Synchronous no-controller FALLBACK tail (plain-model harnesses): report
    // what happened, offer Undo via the model's own restoreFromTrash, and prompt
    // for what couldn't be trashed. With a controller the trash is an async
    // TrashTask instead: the generic undo snackbar (Main.qml, from
    // undoAvailable) replaces this Undo, and onTrashResult below replaces this
    // prompt. `result` is a trashFiles()/trashFolder()/trashTranscript() map.
    function handleTrashResult(result, mode, trashedMessage) {
        const untrashable = result.untrashable || []
        const restorable = result.restorable || []

        if (result.trashed > 0) {
            Notifications.post({
                text: trashedMessage,
                kind: "info",
                actions: restorable.length > 0 ? [{
                    label: qsTr("Undo"),
                    onTriggered: function() {
                        const back = sidebar.model.restoreFromTrash(restorable)
                        if (back < restorable.length)
                            Notifications.warn(qsTr("Only part of that could be restored — " +
                                                    "some items are no longer in the trash."))
                    }
                }] : []
            })
        }

        sidebar.promptPermanentDelete(untrashable, mode)
    }

    // All three take a PATHS ARRAY, not a single path, so the three callers can
    // share one entry point each: the context menu passes
    // selectionPathsFor(ctxPath) (the right-clicked row, widened to the
    // selection when it's part of one), while the ribbon's delete buttons and
    // the Delete key pass a selection subset directly — they have no
    // right-clicked row for selectionPathsFor() to widen from.
    //
    // Each is ONE C++ call for the whole batch: the trash* calls rebuild the
    // model once per call rather than once per file, and a rebuild rescans the
    // vault recursively AND resets the model (see trashFiles()' doc comment).
    // Looping here would also eat its own selection — a model reset clears
    // selectedPaths (see onModelReset above).
    function baseNameOf(path) {
        return path.substring(path.lastIndexOf("/") + 1)
    }

    // Each begins a trash: with a controller it's an async, undoable TrashTask
    // (the generic undo snackbar + onTrashResult's confirm prompt take over);
    // without one (plain-model harnesses) it falls back to the synchronous
    // model.trash* + handleTrashResult, mirroring performRename/performRowMove.
    function beginDeleteTranscript(paths) {
        if (paths.length === 0)
            return
        const urls = paths.map((p) => "file://" + p)
        if (sidebar.transcriptionController) {
            sidebar.transcriptionController.trash("transcript", urls, sidebar.vaultPath)
            return
        }
        if (!sidebar.model)
            return
        const result = sidebar.model.trashTranscript(urls)
        // Keyed on the TARGET count, not on result.trashed: with several
        // targets and one success, paths[0] isn't necessarily the file that
        // succeeded, so naming it would name the wrong one.
        sidebar.handleTrashResult(
            result, "transcript",
            paths.length === 1
                ? qsTr("Moved the transcript for %1 to the trash.").arg(sidebar.baseNameOf(paths[0]))
                : qsTr("Moved %1 transcripts to the trash.").arg(result.trashed))
    }

    // The complement of beginDeleteTranscript(): drops the audio, keeps the .md
    // (which becomes an orphaned-.md row).
    function beginDeleteAudioOnly(paths) {
        if (paths.length === 0)
            return
        const urls = paths.map((p) => "file://" + p)
        if (sidebar.transcriptionController) {
            sidebar.transcriptionController.trash("audioOnly", urls, sidebar.vaultPath)
            return
        }
        if (!sidebar.model)
            return
        const result = sidebar.model.trashAudioOnly(urls)
        sidebar.handleTrashResult(
            result, "audioOnly",
            paths.length === 1
                ? qsTr("Moved the audio for %1 to the trash — the transcript is still here.")
                      .arg(sidebar.baseNameOf(paths[0]))
                : qsTr("Moved %1 audio files to the trash — their transcripts are still here.")
                      .arg(result.trashed))
    }

    function beginDeleteFile(paths) {
        if (paths.length === 0)
            return
        const urls = paths.map((p) => "file://" + p)
        if (sidebar.transcriptionController) {
            sidebar.transcriptionController.trash("files", urls, sidebar.vaultPath)
            return
        }
        if (!sidebar.model)
            return
        const result = sidebar.model.trashFiles(urls)
        sidebar.handleTrashResult(
            result, "files",
            paths.length === 1
                ? qsTr("Moved %1 to the trash.").arg(sidebar.baseNameOf(paths[0]))
                : qsTr("Moved %1 items to the trash.").arg(result.trashed))
    }

    function beginDeleteFolder(path) {
        if (path === "")
            return
        if (sidebar.transcriptionController) {
            sidebar.transcriptionController.trash("folder", ["file://" + path], sidebar.vaultPath)
            return
        }
        if (!sidebar.model)
            return
        const name = path.substring(path.lastIndexOf("/") + 1)
        sidebar.handleTrashResult(
            sidebar.model.trashFolder("file://" + path), "folder",
            qsTr("Moved the folder %1 to the trash.").arg(name))
    }

    // Reached ONLY for entries that couldn't be trashed — i.e. this really is
    // the irreversible one, which is why it's the only path that prompts.
    function confirmFileDelete() {
        const paths = sidebar.pendingDeletePaths
        if (paths.length > 0) {
            const urls = paths.map((p) => "file://" + p)
            // With a controller, the permanent delete is an async DeleteTask
            // (visible in the Tasks list) — one scope-tagged call for the whole
            // batch (a folder is a single-url batch). onModelReset / the model's
            // per-file signals still fire via the task's applyRemovalOutcome, so
            // the open-file view state reacts identically. Without a controller,
            // fall back to the synchronous model.delete* calls (plain-model
            // harnesses), mirroring the trash side.
            if (sidebar.transcriptionController) {
                sidebar.transcriptionController.deletePermanently(sidebar.pendingDeleteMode, urls, sidebar.vaultPath)
            } else if (sidebar.model) {
                // "folder" is the one single-target mode — see pendingDeletePaths'
                // comment. The other three are batches, and each reports how many
                // of the batch it actually removed.
                if (sidebar.pendingDeleteMode === "folder") {
                    if (!sidebar.model.deleteFolderPermanently(urls[0]))
                        Notifications.error(qsTr("Couldn't delete that folder."))
                } else {
                    const deleted = sidebar.pendingDeleteMode === "transcript"
                        ? sidebar.model.deleteTranscriptPermanently(urls)
                        : sidebar.pendingDeleteMode === "audioOnly"
                            ? sidebar.model.deleteAudioOnlyPermanently(urls)
                            : sidebar.model.deleteFilesPermanently(urls)
                    if (deleted < paths.length)
                        Notifications.error(qsTr("Couldn't delete %1 of %2 items.")
                            .arg(paths.length - deleted).arg(paths.length))
                }
            }
        }
        // Nothing clears the selection here: a delete that removed anything
        // rebuilds the model, and the modelReset handler above already drops
        // the selection defensively for exactly this reason (a stale entry
        // pointing at a deleted file). A delete that removed NOTHING leaves the
        // selection standing, which is correct — those files are still there.
        fileDeleteConfirmPopup.close()
    }

    // Display name for a move destination: relative to the vault root, or a
    // "(vault root)" label for the root itself.
    function folderDisplayName(path) {
        const root = sidebar.model ? sidebar.model.vaultLocalPath : ""
        if (path === root)
            return qsTr("(vault root)")
        if (root !== "" && path.startsWith(root + "/"))
            return path.substring(root.length + 1)
        return path
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

    // The gate every note-selection shortcut below shares: the tree or the
    // search-results view actually has keyboard focus (grabbed by their
    // TapHandlers on click), and no inline rename is in flight.
    //
    // The `renamingPath` half is load-bearing, not belt-and-braces. The other
    // conjunct's "a TextField elsewhere has focus, so the tree doesn't" logic
    // holds for the search/tag fields, which sit OUTSIDE the tree — but the
    // inline rename editor is a TextField INSIDE a tree delegate, and TreeView
    // is a FocusScope: when its focused child holds active focus, the scope
    // reports activeFocus too. So `tree.activeFocus` is true while you're
    // typing in the rename editor, and without this an ordinary Delete
    // keystroke mid-rename would trash the file being renamed instead of
    // deleting a character.
    readonly property bool noteShortcutsEnabled:
        (tree.activeFocus || searchResultsView.activeFocus) && sidebar.renamingPath === ""

    // True when keyboard focus currently sits in a text-editing field anywhere
    // in the window — the inline rename editor, the sidebar's search/tag
    // fields, or (window-wide) MainPanel's transcript/title/prelude editors.
    // Duck-typed on cursorPosition, a property only TextInput/TextEdit-based
    // items expose; a plain Item, Button, or the TreeView returns undefined.
    // Keeps deleteShortcutEnabled below from hijacking an in-progress edit's
    // own Delete keystroke.
    readonly property bool textFieldHasFocus: {
        const fi = Window.activeFocusItem
        return fi !== null && fi.cursorPosition !== undefined
    }

    // The Delete key deletes the current sidebar selection whenever one exists
    // and no text field is being edited — deliberately NOT gated on
    // tree.activeFocus (unlike the navigation shortcuts), because the selection
    // stays highlighted in the left panel after focus drifts to the main panel,
    // and a visible selection that Delete ignores is the bug this fixes. The
    // renamingPath guard is belt-and-braces over textFieldHasFocus (the rename
    // editor is itself a text field).
    readonly property bool deleteShortcutEnabled:
        sidebar.selectedPaths.length > 0 && sidebar.renamingPath === ""
        && !sidebar.textFieldHasFocus

    // Vertical arrow-key navigation between notes.
    Shortcut {
        sequence: "Up"
        enabled: sidebar.noteShortcutsEnabled
        onActivated: sidebar.moveNoteSelection(-1)
    }
    Shortcut {
        sequence: "Down"
        enabled: sidebar.noteShortcutsEnabled
        onActivated: sidebar.moveNoteSelection(1)
    }

    // Item 41: Shift+Up/Down extend the selection range from the anchor
    // (mirrors Shift-click), same focus gating as plain Up/Down.
    Shortcut {
        sequence: "Shift+Up"
        enabled: sidebar.noteShortcutsEnabled
        onActivated: sidebar.extendNoteSelection(-1)
    }
    Shortcut {
        sequence: "Shift+Down"
        enabled: sidebar.noteShortcutsEnabled
        onActivated: sidebar.extendNoteSelection(1)
    }

    // Item 41: Ctrl+Up/Down jump to the current group's (parent-folder's)
    // first/last note, then hop group-by-group on repeat.
    Shortcut {
        sequence: "Ctrl+Up"
        enabled: sidebar.noteShortcutsEnabled
        onActivated: sidebar.jumpToGroupBoundary(-1)
    }
    Shortcut {
        sequence: "Ctrl+Down"
        enabled: sidebar.noteShortcutsEnabled
        onActivated: sidebar.jumpToGroupBoundary(1)
    }

    // Item 41: Ctrl+Shift+Up/Down extend the selection to the current group's
    // boundary (group jump + Shift-range in one gesture), same focus gating.
    Shortcut {
        sequence: "Ctrl+Shift+Up"
        enabled: sidebar.noteShortcutsEnabled
        onActivated: sidebar.extendNoteSelectionToGroupBoundary(-1)
    }
    Shortcut {
        sequence: "Ctrl+Shift+Down"
        enabled: sidebar.noteShortcutsEnabled
        onActivated: sidebar.extendNoteSelectionToGroupBoundary(1)
    }

    // Mirrors the ribbon's prepended selectAllButton. Same focus gating as
    // the other note-selection shortcuts above — inert while a TextField
    // elsewhere has focus, so it never fights a text field's own native
    // Ctrl+A (select-all-text).
    Shortcut {
        sequence: "Ctrl+A"
        enabled: sidebar.noteShortcutsEnabled
        onActivated: sidebar.selectAllVisible()
    }

    // Delete the selection, mirroring the ribbon's deleteFileAllButton and the
    // context menu's ctxDeleteFileRow — the whole-file delete, since that's the
    // one a Delete key means everywhere else. Trash-first like every other
    // delete path here, so it's undoable from the snackbar and prompts only if
    // the filesystem refuses (see pendingDeletePaths' comment).
    //
    // Deliberately the plain-file delete and not a folder one: selectedPaths
    // only ever holds notes (a directory row's TapHandler toggles expand and
    // never selects — see treeDelegate), so this can't recursively take a
    // folder. Deleting a folder stays an explicit right-click action.
    //
    // Gated on deleteShortcutEnabled (selection + not editing text) rather than
    // noteShortcutsEnabled, so it fires even when focus has left the tree — the
    // selection stays highlighted in the left panel the whole time.
    Shortcut {
        sequence: StandardKey.Delete
        enabled: sidebar.deleteShortcutEnabled
        onActivated: sidebar.beginDeleteFile(sidebar.selectedPaths.slice())
    }

    // Escape collapses a multi-selection back to just the open file (see
    // deselectExceptCurrent). Deliberately NOT gated on tree/search focus
    // (unlike noteShortcutsEnabled above) — the selection stays highlighted in
    // the left panel after focus drifts to the main panel, and the cancel
    // button/empty-space taps that mirror this already clear it regardless of
    // focus, so Escape should too. Gated on an actual multi-selection existing
    // so it never swallows Escape from anything else when there's nothing to
    // clear; `renamingPath === ""` keeps it off while an inline rename editor
    // is open (Escape there reverts the edit — see rowRenameField's
    // Keys.onEscapePressed).
    Shortcut {
        sequence: "Escape"
        enabled: sidebar.renamingPath === "" && sidebar.selectedPaths.length > 1
        onActivated: sidebar.deselectExceptCurrent()
    }

    Item {
        id: ribbon
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        // Bug fix: this used to be a hand-maintained magic-number formula
        // (base rows + tagChipsFlow.height) that silently went stale when
        // the speaker filter row/chips were added later without updating
        // it, undersizing the ribbon and letting the tree/searchResultsView
        // (anchored to ribbon.bottom) overlap the bottom search rows.
        // Binding to expandedColumn.implicitHeight tracks whatever rows
        // actually exist, so future additions can't repeat this bug.
        //
        // The collapsed half follows the same rule for the same reason: it was
        // a literal 32 (one 22px button row + padding), which only held while
        // the buttons fit on one line. They no longer do — the destructive trio
        // (CLAUDE.md action parity with MainPanel's cluster) pushed the count
        // past what a narrow sidebar can show in a row, so ribbonFlow wraps and
        // this tracks however many lines it ends up needing.
        height: sidebar.searchExpanded ? expandedColumn.implicitHeight
                                        : ribbonFlow.implicitHeight + ribbonFlow.anchors.topMargin * 2

        // A Flow, not the RowLayout this used to be: with 13 items a fixed row
        // silently squeezed/clipped buttons at a narrow sidebar width. Flow
        // ignores Layout attached properties entirely (it positions by plain
        // width/height), which is why every child below sets width/height
        // directly rather than Layout.preferredWidth/Height.
        SidebarRibbon {
            id: ribbonFlow
            sidebarRoot: sidebar
            anchors.top: parent.top
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.topMargin: 5
            anchors.leftMargin: 6
            anchors.rightMargin: 6
            visible: !sidebar.searchExpanded
        }

        Column {
            id: expandedColumn
            anchors.fill: parent
            anchors.leftMargin: 6
            anchors.rightMargin: 6
            spacing: 4
            visible: sidebar.searchExpanded

        RowLayout {
            width: parent.width
            height: 24
            spacing: 6

            TextField {
                id: searchField
                Layout.fillWidth: true
                Layout.preferredHeight: 24
                placeholderText: qsTr("Search labels & tags…")
                text: sidebar.searchQuery
                onTextChanged: sidebar.searchQuery = text

                background: UnderlineField { focused: searchField.activeFocus }

                HoverHandler {
                    cursorShape: Qt.IBeamCursor
                }
            }

            // Item 1 (Wave E): minimal text toggle instead of a QQC2 CheckBox
            // — clearer on/off read via background+outline (green when on)
            // rather than a small tick glyph, and shares the app's rounded
            // corners like every other interactive surface.
            Item {
                id: includeTranscriptionsToggle
                Layout.preferredWidth: includeTranscriptionsLabel.implicitWidth + 16
                Layout.preferredHeight: 24

                HoverHandler {
                    id: includeTranscriptionsHover
                    cursorShape: Qt.PointingHandCursor
                }
                TapHandler {
                    onTapped: sidebar.searchIncludeTranscriptions = !sidebar.searchIncludeTranscriptions
                }
                Rectangle {
                    anchors.fill: parent
                    radius: AppSettings.borderRadius
                    color: sidebar.searchIncludeTranscriptions
                           ? (includeTranscriptionsHover.hovered ? Theme.success : Theme.successDim)
                           : (includeTranscriptionsHover.hovered ? Theme.surfaceHover : Theme.surface)
                    border.width: 1
                    border.color: sidebar.searchIncludeTranscriptions ? Theme.success : Theme.borderStrong

                    Behavior on color { ColorAnimation { duration: AppSettings.animationsEnabled ? 100 : 0 } }
                }
                Label {
                    id: includeTranscriptionsLabel
                    anchors.centerIn: parent
                    text: qsTr("Transcriptions")
                    color: sidebar.searchIncludeTranscriptions ? "white" : Theme.textMuted
                    font.pixelSize: 11
                }
            }

            AppIconButton {
                id: searchCloseButton
                iconName: "close"
                Layout.preferredWidth: 20
                Layout.preferredHeight: 20
                onTapped: {
                    searchField.text = ""
                    sidebar.searchQuery = ""
                    sidebar.searchExpanded = false
                    // Item 49: closing search used to leave the tag
                    // filter (a separate, orthogonal feature — see
                    // activeTagFilters' doc comment) active, so rows
                    // stayed hidden even after the search UI itself was
                    // gone. Exiting search now fully clears filtering.
                    tagFilterField.text = ""
                    sidebar.tagFilterText = ""
                }
            }
        }

        // Item 17 (Wave E): comma-separated tag filter with autofill
        // suggestions drawn from the vault's existing tag vocabulary.
        // Separate from searchQuery (item 9's soft/dim+sort search) —
        // tagFilter genuinely hides non-matching rows (see
        // VaultSearchProxyModel::filterAcceptsRow).
        RowLayout {
            width: parent.width
            height: 24
            spacing: 6

            TextField {
                id: tagFilterField
                objectName: "tagFilterField"
                Layout.fillWidth: true
                Layout.preferredHeight: 24
                placeholderText: qsTr("Filter by tags (comma-separated)…")
                text: sidebar.tagFilterText
                onTextChanged: {
                    sidebar.tagFilterText = text
                    // Typing revives a dismissed list — an Escape suppresses
                    // the CURRENT fragment's suggestions, not the field's
                    // autocomplete forever.
                    tagSuggestionsPopup.dismissed = false
                }

                background: UnderlineField { focused: tagFilterField.activeFocus }

                HoverHandler {
                    cursorShape: Qt.IBeamCursor
                }

                // Arrow/Enter/Escape reach the popup only while it is showing;
                // otherwise they must fall through so Escape still collapses
                // search and Enter still behaves normally in the field.
                Keys.onDownPressed: (event) => {
                    if (tagSuggestionsPopup.visible) tagSuggestionsPopup.moveDown()
                    else event.accepted = false
                }
                Keys.onUpPressed: (event) => {
                    if (tagSuggestionsPopup.visible) tagSuggestionsPopup.moveUp()
                    else event.accepted = false
                }
                Keys.onReturnPressed: (event) => {
                    if (tagSuggestionsPopup.visible) tagSuggestionsPopup.acceptCurrent()
                    else event.accepted = false
                }
                Keys.onEnterPressed: (event) => {
                    if (tagSuggestionsPopup.visible) tagSuggestionsPopup.acceptCurrent()
                    else event.accepted = false
                }
                // Tab accepts the highlighted suggestion like Enter (the
                // conventional autocomplete-commit key); when the popup is
                // hidden it falls through to normal focus navigation.
                Keys.onTabPressed: (event) => {
                    if (tagSuggestionsPopup.visible) tagSuggestionsPopup.acceptCurrent()
                    else event.accepted = false
                }
                Keys.onEscapePressed: (event) => {
                    if (tagSuggestionsPopup.visible) tagSuggestionsPopup.dismiss()
                    else event.accepted = false
                }

                FilterSuggestions {
                    id: tagSuggestionsPopup
                    objectName: "tagSuggestionsPopup"
                    field: tagFilterField
                    enabledWhen: sidebar.searchExpanded
                    suggestions: sidebar.tagSuggestions()
                    onAccepted: (value) => {
                        sidebar.tagFilterText = sidebar.acceptFragment(sidebar.tagFilterText, value)
                        tagFilterField.forceActiveFocus()
                        tagFilterField.cursorPosition = tagFilterField.text.length
                    }
                }
            }

            AppIconButton {
                id: tagFilterClearButton
                iconName: "close"
                visible: sidebar.tagFilterText !== ""
                Layout.preferredWidth: 20
                Layout.preferredHeight: 20
                onTapped: sidebar.tagFilterText = ""
            }
        }

        // O12: per-tag removable chips for the active tag filter, below the
        // raw comma-separated TextField (which stays as-is for typing/
        // autocompleting new tags, see tagSuggestionsPopup) — visual style
        // matches MainPanel's own tagPill (a note's OWN tags, a separate
        // feature) for consistency rather than inventing a new look.
        Flow {
            id: tagChipsFlow
            width: parent.width
            visible: sidebar.activeTagFilters.length > 0
            spacing: 4

            Repeater {
                model: sidebar.activeTagFilters

                Rectangle {
                    id: tagChip
                    required property string modelData
                    required property int index
                    radius: AppSettings.borderRadius
                    color: tagChipHover.hovered ? "#3f3f3f" : "#333333"
                    border.width: 1
                    border.color: Theme.borderStrong
                    implicitWidth: tagChipRow.implicitWidth + 10
                    implicitHeight: tagChipRow.implicitHeight + 6

                    HoverHandler {
                        id: tagChipHover
                        cursorShape: Qt.PointingHandCursor
                    }

                    Row {
                        id: tagChipRow
                        anchors.centerIn: parent
                        spacing: 4

                        Label {
                            anchors.verticalCenter: parent.verticalCenter
                            text: tagChip.modelData
                            color: "#cccccc"
                            font.pixelSize: 11
                        }

                        Item {
                            id: tagChipRemove
                            anchors.verticalCenter: parent.verticalCenter
                            width: 12
                            height: 12

                            HoverHandler {
                                id: tagChipRemoveHover
                                cursorShape: Qt.PointingHandCursor
                            }
                            TapHandler {
                                onTapped: {
                                    const remaining = sidebar.activeTagFilters.filter((t, i) => i !== tagChip.index)
                                    sidebar.tagFilterText = remaining.join(", ")
                                }
                            }
                            AppIcon {
                                anchors.centerIn: parent
                                name: "close"
                                color: tagChipRemoveHover.hovered ? "white" : "#999999"
                                iconSize: 12
                            }
                        }
                    }
                }
            }
        }

        // Speaker filter — mirrors the tag filter immediately above
        // (RowLayout TextField+clear, then a removable-chips Flow) but
        // against VaultSearchProxyModel.speakerFilter/VaultTreeModel's
        // SpeakersRole instead of tags — including the autocomplete popup,
        // which is literally the same FilterSuggestions component over
        // speakerSuggestions() (existingVaultSpeakers' vault-wide
        // `speakers:` vocabulary). Keep the two call sites structurally
        // identical; divergence here is the bug this replaced.
        RowLayout {
            width: parent.width
            height: 24
            spacing: 6

            TextField {
                id: speakerFilterField
                objectName: "speakerFilterField"
                Layout.fillWidth: true
                Layout.preferredHeight: 24
                placeholderText: qsTr("Filter by speakers (comma-separated)…")
                text: sidebar.speakerFilterText
                onTextChanged: {
                    sidebar.speakerFilterText = text
                    // Typing revives a dismissed list — an Escape suppresses
                    // the CURRENT fragment's suggestions, not the field's
                    // autocomplete forever.
                    speakerSuggestionsPopup.dismissed = false
                }

                background: UnderlineField { focused: speakerFilterField.activeFocus }

                HoverHandler {
                    cursorShape: Qt.IBeamCursor
                }

                // Arrow/Enter/Escape reach the popup only while it is showing;
                // otherwise they must fall through so Escape still collapses
                // search and Enter still behaves normally in the field.
                Keys.onDownPressed: (event) => {
                    if (speakerSuggestionsPopup.visible) speakerSuggestionsPopup.moveDown()
                    else event.accepted = false
                }
                Keys.onUpPressed: (event) => {
                    if (speakerSuggestionsPopup.visible) speakerSuggestionsPopup.moveUp()
                    else event.accepted = false
                }
                Keys.onReturnPressed: (event) => {
                    if (speakerSuggestionsPopup.visible) speakerSuggestionsPopup.acceptCurrent()
                    else event.accepted = false
                }
                Keys.onEnterPressed: (event) => {
                    if (speakerSuggestionsPopup.visible) speakerSuggestionsPopup.acceptCurrent()
                    else event.accepted = false
                }
                // Tab accepts the highlighted suggestion like Enter (the
                // conventional autocomplete-commit key); when the popup is
                // hidden it falls through to normal focus navigation.
                Keys.onTabPressed: (event) => {
                    if (speakerSuggestionsPopup.visible) speakerSuggestionsPopup.acceptCurrent()
                    else event.accepted = false
                }
                Keys.onEscapePressed: (event) => {
                    if (speakerSuggestionsPopup.visible) speakerSuggestionsPopup.dismiss()
                    else event.accepted = false
                }

                FilterSuggestions {
                    id: speakerSuggestionsPopup
                    objectName: "speakerSuggestionsPopup"
                    field: speakerFilterField
                    enabledWhen: sidebar.searchExpanded
                    suggestions: sidebar.speakerSuggestions()
                    onAccepted: (value) => {
                        sidebar.speakerFilterText = sidebar.acceptFragment(sidebar.speakerFilterText, value)
                        speakerFilterField.forceActiveFocus()
                        speakerFilterField.cursorPosition = speakerFilterField.text.length
                    }
                }
            }

            AppIconButton {
                id: speakerFilterClearButton
                iconName: "close"
                visible: sidebar.speakerFilterText !== ""
                Layout.preferredWidth: 20
                Layout.preferredHeight: 20
                onTapped: sidebar.speakerFilterText = ""
            }
        }

        Flow {
            id: speakerChipsFlow
            width: parent.width
            visible: sidebar.activeSpeakerFilters.length > 0
            spacing: 4

            Repeater {
                model: sidebar.activeSpeakerFilters

                Rectangle {
                    id: speakerChip
                    required property string modelData
                    required property int index
                    radius: AppSettings.borderRadius
                    color: speakerChipHover.hovered ? "#3f3f3f" : "#333333"
                    border.width: 1
                    border.color: Theme.borderStrong
                    implicitWidth: speakerChipRow.implicitWidth + 10
                    implicitHeight: speakerChipRow.implicitHeight + 6

                    HoverHandler {
                        id: speakerChipHover
                    }

                    Row {
                        id: speakerChipRow
                        anchors.centerIn: parent
                        spacing: 4

                        Label {
                            anchors.verticalCenter: parent.verticalCenter
                            text: speakerChip.modelData
                            color: "#cccccc"
                            font.pixelSize: 11
                        }

                        Item {
                            id: speakerChipRemove
                            anchors.verticalCenter: parent.verticalCenter
                            width: 12
                            height: 12

                            HoverHandler {
                                id: speakerChipRemoveHover
                                cursorShape: Qt.PointingHandCursor
                            }
                            TapHandler {
                                onTapped: {
                                    const remaining = sidebar.activeSpeakerFilters.filter((s, i) => i !== speakerChip.index)
                                    sidebar.speakerFilterText = remaining.join(", ")
                                }
                            }
                            AppIcon {
                                anchors.centerIn: parent
                                name: "close"
                                color: speakerChipRemoveHover.hovered ? "white" : "#999999"
                                iconSize: 12
                            }
                        }
                    }
                }
            }
        }

        // Item 39: run-all actions scoped to the current search/filter results
        // (searchResultPaths()). Third home for transcribe/relabel/chapters,
        // matching CLAUDE.md's per-file-action-parity rule (ribbon = bulk,
        // treeDelegate cluster = per-row, this = filtered set). Each button
        // reuses the EXACT controller-call idiom + args its ribbon counterpart
        // uses (transcribeAll/relabelAll/generateChaptersAll), just over its
        // eligible subset of the filtered set instead of the selection*Paths.
        // Disabled unless a filter is actually active AND that action's
        // eligible subset is non-empty (see searchFilterActive + the per-action
        // helpers above), so they never silently run over the whole vault.
        RowLayout {
            width: parent.width
            height: 24
            spacing: 4

            Label {
                text: qsTr("Run on results:")
                color: Theme.textMuted
                font.pixelSize: 11
                Layout.alignment: Qt.AlignVCenter
            }

            AppIconButton {
                id: searchTranscribeButton
                iconName: "transcribe"
                tooltip: qsTr("Transcribe untranscribed results")
                Layout.preferredWidth: 22
                Layout.preferredHeight: 22
                enabled: sidebar.searchFilterActive && sidebar.searchTranscribePaths().length > 0
                onTapped: {
                    const paths = sidebar.searchTranscribePaths()
                    if (paths.length === 0)
                        return
                    const urls = paths.map((p) => "file://" + p)
                    sidebar.transcriptionController.transcribeAll(urls, sidebar.vaultPath, AppSettings.defaultModel,
                                                                    AppSettings.autoLabelEnabled,
                                                                    AppSettings.restrictTagsToExisting,
                                                                    AppSettings.tagReuseBias, AppSettings.wordiness)
                }
            }

            AppIconButton {
                id: searchRelabelButton
                iconName: "relabel"
                tooltip: qsTr("Regenerate auto-titles for results")
                Layout.preferredWidth: 22
                Layout.preferredHeight: 22
                enabled: sidebar.transcriptionController && sidebar.searchFilterActive
                         && sidebar.searchRelabelPaths().length > 0
                onTapped: {
                    const paths = sidebar.searchRelabelPaths()
                    const urls = paths.map((p) => "file://" + p)
                    sidebar.transcriptionController.relabelAll(urls, sidebar.vaultPath,
                                                                 AppSettings.restrictTagsToExisting,
                                                                 AppSettings.tagReuseBias, AppSettings.wordiness)
                }
            }

            AppIconButton {
                id: searchChaptersButton
                iconName: "chapters"
                tooltip: qsTr("Regenerate chapters for results")
                Layout.preferredWidth: 22
                Layout.preferredHeight: 22
                enabled: sidebar.transcriptionController && sidebar.searchFilterActive
                         && sidebar.searchChaptersPaths().length > 0
                onTapped: {
                    const paths = sidebar.searchChaptersPaths()
                    const urls = paths.map((p) => "file://" + p)
                    sidebar.transcriptionController.generateChaptersAll(urls, sidebar.vaultPath,
                                                                          AppSettings.chapterDensity)
                }
            }

            // Item 52 (CLAUDE.md per-file-action parity): search-scoped
            // "detect speakers", parity with the ribbon's detectSpeakersAllButton
            // — over the transcribed audio in the filtered result set.
            AppIconButton {
                id: searchDetectSpeakersButton
                iconName: "speakers"
                iconSize: 14
                tooltip: qsTr("Detect speakers for results")
                Layout.preferredWidth: 22
                Layout.preferredHeight: 22
                enabled: sidebar.transcriptionController && sidebar.searchFilterActive
                         && sidebar.searchDetectSpeakersPaths().length > 0
                onTapped: {
                    const paths = sidebar.searchDetectSpeakersPaths()
                    const urls = paths.map((p) => "file://" + p)
                    sidebar.transcriptionController.diarizeFilesForPaths(urls, sidebar.vaultPath,
                                                                        AppSettings.diarizationSegmentationModel,
                                                                        AppSettings.diarizationEmbeddingModel,
                                                                        AppSettings.diarizationSensitivity)
                }
            }

            // Search-scoped "run all available jobs", parity with the
            // ribbon's runAllButton — over every audio file in the filtered
            // result set (see searchRunAllPaths()).
            AppIconButton {
                id: searchRunAllButton
                iconName: "run-all"
                tooltip: qsTr("Run all available jobs for results")
                Layout.preferredWidth: 22
                Layout.preferredHeight: 22
                enabled: sidebar.transcriptionController && sidebar.searchFilterActive
                         && sidebar.searchRunAllPaths().length > 0
                onTapped: {
                    const paths = sidebar.searchRunAllPaths()
                    if (paths.length === 0)
                        return
                    const urls = paths.map((p) => "file://" + p)
                    sidebar.transcriptionController.runAllJobsForPaths(urls, sidebar.vaultPath,
                                                                        AppSettings.defaultModel,
                                                                        AppSettings.restrictTagsToExisting,
                                                                        AppSettings.tagReuseBias,
                                                                        AppSettings.wordiness,
                                                                        AppSettings.chapterDensity,
                                                                        AppSettings.diarizationSegmentationModel,
                                                                        AppSettings.diarizationEmbeddingModel,
                                                                        AppSettings.diarizationSensitivity)
                }
            }

            Item { Layout.fillWidth: true }
        }
        }

    }

    // Item 40 (drag-to-move): the tree's empty space below the last row stands
    // in for the vault root as a drop target — the root isn't itself a row, so
    // without this there'd be no gesture for dragging a file back OUT of a
    // subfolder. Deliberately declared BEFORE the tree, so it stacks underneath
    // and every row's own DropArea sits on top of it: Qt delivers a drag to the
    // topmost accepting DropArea only, which leaves this one reached exactly
    // where no row is. (Same "empty tree space means the vault root" mapping
    // treeEmptyContextTap already uses for right-click → New folder.)
    DropArea {
        id: rootDropTarget
        anchors.fill: tree
        // A hidden DropArea is skipped by drag delivery, so this must track the
        // tree's own visibility — under an active text search the tree is
        // replaced by searchResultsView and there is nothing to drop onto.
        visible: tree.visible
        keys: ["suopVaultRow"]
        readonly property string destDir: sidebar.model ? sidebar.model.vaultLocalPath : ""

        onEntered: (drag) => {
            if (!sidebar.canDropRowsOn(rootDropTarget.destDir)) {
                drag.accepted = false
                return
            }
            sidebar.rowDragDest = rootDropTarget.destDir
        }
        onExited: {
            if (sidebar.rowDragDest === rootDropTarget.destDir)
                sidebar.rowDragDest = ""
        }
        onDropped: {
            const paths = sidebar.rowDragPaths.slice()
            const isDir = sidebar.rowDragIsDir
            const dest = rootDropTarget.destDir
            // Deferred — see performRowMove's doc comment.
            Qt.callLater(() => sidebar.performRowMove(paths, isDir, dest))
        }

        // The root's "row" is the whole tree, so that's what highlights. Same
        // stroke as the per-folder drop highlight in treeDelegate.
        Rectangle {
            anchors.fill: parent
            visible: rootDropTarget.destDir !== "" && sidebar.rowDragActive
                     && sidebar.rowDragDest === rootDropTarget.destDir
            color: "transparent"
            radius: AppSettings.borderRadius
            border.width: 1
            border.color: AppSettings.highlightColor
        }
    }

    // Item 40 (drag-to-move): the thing that follows the cursor during a row
    // drag and, through its Drag attached property, IS the drag source Qt
    // hit-tests against the DropAreas. Parented to the sidebar rather than to
    // the dragged row: the row itself must stay put (TreeView owns its layout),
    // and a source item hanging off a recyclable delegate could be destroyed
    // mid-gesture. Driven imperatively (Drag.start()/drop()/cancel()) from
    // sidebar.beginRowDrag()/endRowDrag() — see beginRowDrag for why it isn't a
    // Drag.active binding.
    Item {
        id: rowDragGhost
        z: 100
        visible: sidebar.rowDragActive
        width: rowDragGhostBg.width
        height: rowDragGhostBg.height
        // This item's top-left tracks the pointer and Drag.hotSpot is (0, 0),
        // so Qt hit-tests drop targets at exactly the cursor — the visible
        // ghost body is offset off the pointer separately (below) so it never
        // covers the row being aimed at.
        x: sidebar.rowDragX
        y: sidebar.rowDragY
        Drag.keys: ["suopVaultRow"]
        Drag.hotSpot.x: 0
        Drag.hotSpot.y: 0

        Rectangle {
            id: rowDragGhostBg
            x: 12
            y: 12
            width: rowDragGhostLabel.implicitWidth + 12
            height: rowDragGhostLabel.implicitHeight + 8
            color: Theme.surfaceDialog
            radius: AppSettings.borderRadius
            border.width: 1
            // Doubles as the "will this drop do anything" readout, so the
            // answer travels with the cursor instead of only living in the
            // (possibly far-away) highlighted target row.
            border.color: sidebar.canDropRowsOn(sidebar.rowDragDest)
                ? AppSettings.highlightColor : Theme.border
            opacity: 0.9

            Text {
                id: rowDragGhostLabel
                anchors.centerIn: parent
                color: "white"
                font.pixelSize: 11
                text: sidebar.rowDragPaths.length > 1
                    ? qsTr("%1 items").arg(sidebar.rowDragPaths.length)
                    : (sidebar.rowDragPaths.length === 1
                       ? sidebar.rowDragPaths[0].split("/").pop()
                       : "")
            }
        }
    }

    TreeView {
        id: tree
        objectName: "vaultTree"
        model: searchProxy
        // Search UI rearchitecture: a non-empty text search query now shows
        // searchResultsView (below) instead — the old in-tree dimming/
        // snippet code beneath is left in place (unreachable, not deleted;
        // see that code's own comments) rather than risk removing something
        // this app has no automated way to re-verify. Tag filtering is a
        // separate, orthogonal feature and keeps hiding rows through this
        // same TreeView regardless of search state — only text search hides
        // the tree itself.
        visible: sidebar.searchQuery === ""
        anchors.top: ribbon.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        clip: true

        // Bug: driving the single column's width purely from the
        // delegate's own implicitWidth (tree.width - sidebarRightMargin)
        // left TableView's column-width cache stale when
        // AppSettings.sidebarRightMargin changed LIVE (e.g. dragging the
        // Settings popup's slider) — forceLayout() alone (see the
        // Connections block below) wasn't enough to invalidate it, even
        // though the delegate's own implicitWidth property had genuinely
        // re-evaluated. columnWidthProvider is TableView's own documented,
        // officially-supported mechanism for a column whose width depends
        // on external reactive state; forceLayout() reliably re-invokes it.
        columnWidthProvider: function (column) {
            return Math.max(0, tree.width - AppSettings.sidebarRightMargin)
        }

        // AlwaysOn (not the Basic style's default AsNeeded-but-invisible-
        // until-active overlay): the ask was a genuinely visible scrollbar
        // affordance, not just scroll functionality — TreeView already
        // scrolls via wheel/drag without this. AlwaysOn keeps the Basic
        // style's ScrollBar in its "active" opacity state permanently
        // (see ScrollBar.qml's `state` condition), rather than only
        // flashing in while a scroll is in progress.
        ScrollBar.vertical: ScrollBar { policy: ScrollBar.AlwaysOn }

        // A non-empty text query hides this whole TreeView (visible:
        // sidebar.searchQuery === "") and shows searchResultsView instead. A
        // QQuickTableView releases its realized delegates while invisible, so
        // contentHeight collapses toward the viewport height — and the
        // forceLayout()s fired by the search Connections below run either while
        // still invisible or on the very tick `visible` flips back to true
        // (before the item is re-polished), so they rebuild against a
        // not-yet-laid-out view and contentHeight stays stale-small. Result:
        // after exiting search the scrollbar thumb fills the track and the tree
        // won't scroll to the files below the fold. Re-lay-out once the tree is
        // genuinely visible again, deferred a turn so it runs after visible:
        // true is applied and the view re-polished.
        onVisibleChanged: {
            if (tree.visible)
                Qt.callLater(tree.forceLayout)
        }

        // Item 40: right-click on empty tree space (below the rows) opens the
        // context menu targeting the vault root — so "New folder" at the root
        // is reachable even with no rows to right-click.
        //
        // BUG FIX: this used to assume "a row's own treeRowContextTap (deeper
        // in the hierarchy) takes the exclusive grab first, so this only fires
        // for genuinely empty area." That is not how nested TapHandlers behave
        // — they are not mutually exclusive (the same hazard StyleGuide.md's
        // "Always-editable text" documents), so BOTH fired on every row
        // right-click, and this one fired LAST. It therefore overwrote the
        // row's captured ctx* state with the empty-area's, and the menu came up
        // showing only "New folder": Move to…/Delete file/Delete transcript/
        // Combine selected/Rename are all gated on ctxIsNote/ctxIsDir, which
        // this had just reset to false. Every row right-click was affected.
        // Verified against a real QQuickView in tst_sidebar_rename's
        // rightClickRow_menuOffersRenameAndNamesTheSelectionCount.
        //
        // So: bail when the tap landed on a real cell, rather than assume it
        // can't have. cellAtPosition() takes the tap in the TableView's OWN
        // coordinate system (it accounts for the scroll offset internally) —
        // which is exactly what a TapHandler's point.position already is. An
        // earlier version mapped the point into contentItem coordinates first,
        // which double-applied contentY: harmless at the top (contentY 0) but
        // once the view was scrolled down every real row read as -1 (empty
        // space), so a row click wrongly fell through to the empty-space branch.
        TapHandler {
            id: treeEmptyContextTap
            acceptedButtons: Qt.RightButton
            onTapped: {
                const local = treeEmptyContextTap.point.position
                const cell = tree.cellAtPosition(local.x, local.y, true)
                if (cell.y >= 0)
                    return // a row — its own treeRowContextTap owns this click
                const p = tree.mapToItem(sidebar, local.x, local.y)
                sidebar.openContextMenu("", false, false, false, 0,
                                         sidebar.model ? sidebar.model.vaultLocalPath : "", "", p.x, p.y)
            }
        }

        // Left-click on empty tree space (below/around the rows) clears the
        // multi-selection back to the open file — the "click a non-file surface
        // to deselect" behavior. Same cellAtPosition() hit-test as the
        // right-click handler above: bail when the tap actually landed on a
        // row, since nested TapHandlers aren't mutually exclusive (a row's own
        // treeRowTap must own its click and keep the selection it just made).
        TapHandler {
            id: treeEmptyLeftTap
            acceptedButtons: Qt.LeftButton
            onTapped: {
                const local = treeEmptyLeftTap.point.position
                const cell = tree.cellAtPosition(local.x, local.y, true)
                if (cell.y >= 0)
                    return // a row — treeRowTap owns this click
                sidebar.deselectExceptCurrent()
            }
        }

        function expandAll() {
            // Descending order: expanding a row only inserts rows
            // after it, so earlier (lower) indices stay valid.
            for (let r = rows - 1; r >= 0; r--)
                expandRecursively(r, -1)
            // Empty folders aren't reachable by expandRecursively (TreeView
            // won't expand a childless node at all) — expand them the only way
            // they can be: by dropping their collapsed flags.
            sidebar.collapsedEmptyDirs = ({})
        }

        // A model reset invalidates every QModelIndex, so TreeView drops its
        // whole expansion state and falls back to root-level rows — every
        // folder's contents vanish and everything below them slides up. The
        // tree therefore has to be re-expanded after any reset, and WHEN that
        // happens is the whole story: this used to run off a 16ms polling
        // Timer, i.e. ~1 frame at 60Hz, so the collapsed tree was genuinely
        // rendered and the rows visibly jumped up and back. That was the
        // "flash" after a drag-to-move drop (user report) — measured at ~22
        // consecutive frames/samples of collapsed tree in tst_sidebar_dragmove.
        //
        // expandPending/expanding drive the recovery instead:
        property bool expandPending: false
        property bool expanding: false

        // Scroll-offset preservation across a model reset. A reset drops every
        // QModelIndex, so the view collapses to root rows, contentHeight
        // shrinks, and the Flickable CLAMPS contentY — by the time
        // expandAllNow() has put the rows back, the offset is already gone.
        // So remember where the user actually was and put it back afterwards.
        //
        // Sampled continuously rather than read inside onModelReset: that
        // would depend on this QML Connections being invoked before
        // TreeView's own internal reset handler, and if that ordering ever
        // flipped, the bug would come back silently (restoring 0 over 0).
        // Anything that moves contentY WHILE recovering is the collapse
        // itself, never the user — hence the gate.
        property real lastUserContentY: 0
        property real pendingContentY: -1
        onContentYChanged: {
            if (!tree.expandPending && !tree.expanding)
                tree.lastUserContentY = tree.contentY
        }

        // insideLayout: true when the caller is itself running inside
        // TableView's layout pass (onRowsChanged is), which changes only how
        // the trailing forceLayout() is scheduled — see below.
        function expandAllNow(insideLayout) {
            // Re-entrancy: expandAll() inserts rows, which re-fires
            // onRowsChanged below while this is still running.
            if (tree.expanding || tree.rows <= 0)
                return
            tree.expanding = true
            tree.expandAll()
            // P13(a): expandAll() re-inserts every previously-expanded
            // directory's children synchronously, but the ALREADY pending
            // Qt.callLater(forceLayout) from relayoutForSearchStateChange()
            // (triggered by the same model reset) can fire before this point,
            // caching stale/partial row positions with nothing to re-trigger a
            // layout afterward — so a layout has to follow this expansion.
            //
            // It just can't always be an IMMEDIATE one: forceLayout() refuses
            // to re-enter an ongoing layout ("Cannot do an immediate re-layout
            // during an ongoing layout!"), and that refusal is a warning plus a
            // silent no-op, not an exception — so calling it from
            // onRowsChanged would log on every single vault change AND leave
            // the very stale positions P13(a) is about. Defer to the next event
            // loop turn in that case. Nothing visual depends on the timing: the
            // row INSERTIONS above are what kill the flash and they've already
            // happened synchronously; this only refreshes cached positions.
            if (insideLayout)
                Qt.callLater(tree.forceLayout)
            else
                tree.forceLayout()
            tree.expanding = false
            tree.expandPending = false
            expandRetryTimer.stop()
            tree.restorePendingContentY()
        }

        // Deferred, always: in the insideLayout case forceLayout() itself is
        // only queued above, so contentHeight isn't final yet. Qt.callLater
        // callbacks run in queue order, so this lands after that layout.
        // Clamped rather than assigned raw — the tree may genuinely be shorter
        // now (rows were deleted), and an out-of-range contentY leaves the
        // Flickable showing blank space below the last row.
        function restorePendingContentY() {
            if (tree.pendingContentY < 0)
                return
            const target = tree.pendingContentY
            tree.pendingContentY = -1
            Qt.callLater(function() {
                const maxY = Math.max(0, tree.contentHeight - tree.height)
                tree.contentY = Math.max(0, Math.min(target, maxY))
                tree.lastUserContentY = tree.contentY
            })
        }

        // The view has NOT processed the reset when this runs — a QML
        // Connections on the proxy can be invoked before TreeView's own
        // internal handler, so rows/expansion state here are still the old
        // ones and expanding now would no-op against stale indexes and then be
        // thrown away by the reset that follows (tried; it left the tree
        // permanently collapsed, see tst_sidebar_dragmove). So only arm the
        // flag, and let onRowsChanged below do the work at the first moment
        // it's actually possible.
        Connections {
            target: searchProxy
            function onModelReset() {
                tree.pendingContentY = tree.lastUserContentY
                tree.expandPending = true
                expandRetryTimer.start()
            }
        }

        // Opening a DIFFERENT vault resets the model as well, but there is no
        // previous position to preserve — start at the top.
        Connections {
            target: sidebar.model
            ignoreUnknownSignals: true
            function onVaultPathChanged() {
                tree.pendingContentY = -1
                tree.lastUserContentY = 0
            }
        }

        // The moment the view publishes its post-reset (collapsed) row count,
        // which happens during its own layout pass — before the frame is
        // rendered. Re-expanding here means the collapsed tree is never
        // painted, so there is nothing to flash.
        onRowsChanged: {
            if (tree.expandPending)
                tree.expandAllNow(true)   // fired from inside TableView's layout
        }

        // Backstop only, for a reset whose row count happens to be unchanged
        // (so onRowsChanged never fires) or that lands before the view has any
        // rows at all — e.g. an empty vault, where there's nothing on screen to
        // flash anyway. This is the path that used to run every time.
        Timer {
            id: expandRetryTimer
            interval: 16
            repeat: true
            onTriggered: {
                if (tree.rows > 0)
                    tree.expandAllNow(false)   // plain timer callback, no layout in progress
            }
        }

        // Item 8 (Wave E): hover highlight/outline appeared vertically offset
        // from a row's own text in some cases — traced to TreeView (built on
        // TableView) caching each row's Y position from its delegate's
        // implicitHeight, which this file changes reactively per-row
        // (showSnippet flips a row between 28px and a taller wrapped
        // snippet, see the delegate below) as the search query/toggle
        // changes. TableView doesn't always reflow already-realized rows'
        // cached positions when a delegate's implicit size changes out from
        // under it — forceLayout() explicitly recomputes them. Triggered on
        // exactly the two properties that drive showSnippet.
        function relayoutForSearchStateChange() {
            Qt.callLater(tree.forceLayout)
        }
        Connections {
            target: sidebar
            function onSearchQueryChanged() { tree.relayoutForSearchStateChange() }
            function onSearchIncludeTranscriptionsChanged() { tree.relayoutForSearchStateChange() }
            // P13(b): searchCloseButton flips searchExpanded=false in the
            // SAME tick it clears searchQuery — that synchronously collapses
            // the ribbon (64->32) this TreeView is anchored directly below,
            // while the query-changed relayout above is deferred a full
            // event-loop turn via Qt.callLater. For at least one frame, rows
            // would lay out against the new header geometry with stale
            // cached heights. Call forceLayout() directly (not deferred) so
            // this specific transition lands in the same frame as the
            // header collapse/expand.
            function onSearchExpandedChanged() { tree.forceLayout() }
        }

        // columnWidthProvider above needs an explicit forceLayout() call
        // whenever one of ITS OWN external inputs changes — same
        // requirement Qt's docs call out for this exact case (a
        // dynamically-computed column width), and the same
        // stale-cached-geometry class of bug relayoutForSearchStateChange
        // above works around on HEIGHT.
        Connections {
            target: AppSettings
            function onSidebarRightMarginChanged() { tree.forceLayout() }
        }

        delegate: TreeRowDelegate {
            // Extracted to its own component (TreeRowDelegate.qml) — a pure
            // relocation. The Sidebar root and this TreeView are threaded in
            // so the delegate body's `sidebarRoot.*` / `treeRoot.*` references
            // resolve unchanged; model roles + TreeView attached props
            // (name/path/row/treeView/depth/…) are still auto-injected into its
            // required properties by the delegate mechanism, exactly as inline.
            //
            // The LHS names (sidebarRoot/treeRoot) deliberately differ from the
            // outer ids (sidebar/tree): a delegate binding `sidebar: sidebar`
            // resolves the RHS against the delegate's OWN property and self-binds
            // to null. Distinct names let the RHS reach the outer Sidebar ids.
            sidebarRoot: sidebar
            treeRoot: tree
        }
    }

    // Search UI rearchitecture: a flat, separate results list shown INSTEAD
    // of tree above while a text search query is active — genuinely its own
    // ListView/delegate rather than another dimming/restyling mode bolted
    // onto treeDelegate, which is what caused the row-height/relayout races
    // relayoutForSearchStateChange/expandRetryTimer above were fixed for.
    // Bound to VaultTreeModel::searchResults() via sidebar.searchResultsList
    // (see that property's own comment for why it's an explicit push, not a
    // reactive binding).
    ListView {
        id: searchResultsView
        visible: sidebar.searchQuery !== ""
        anchors.top: ribbon.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        clip: true
        model: sidebar.searchResultsList

        ScrollBar.vertical: ScrollBar { policy: ScrollBar.AlwaysOn }

        // Mirror of the tree's treeEmptyLeftTap: a left-click on empty results
        // space (below the last result) clears the multi-selection back to the
        // open file. indexAt() wants content coordinates (the list scrolls
        // under the handler's parent-relative point), and returns -1 off any
        // real row — so a genuine result row's own resultRowTap keeps its
        // click and the selection it makes.
        TapHandler {
            id: searchEmptyLeftTap
            acceptedButtons: Qt.LeftButton
            onTapped: {
                const local = searchEmptyLeftTap.point.position
                const idx = searchResultsView.indexAt(searchResultsView.contentX + local.x,
                                                        searchResultsView.contentY + local.y)
                if (idx >= 0)
                    return // a result row — resultRowTap owns this click
                sidebar.deselectExceptCurrent()
            }
        }

        delegate: SearchResultDelegate {
            // Pure relocation to SearchResultDelegate.qml — sidebarRoot and the
            // ListView are threaded in so the body's sidebarRoot.* / listView.*
            // references resolve unchanged (model roles auto-injected). LHS
            // names differ from the outer ids to dodge the self-bind trap; see
            // TreeRowDelegate above.
            sidebarRoot: sidebar
            listView: searchResultsView
        }
    }

    // Chrome/Firefox-style middle-click autoscroll for the file tree — see
    // MiddleDragScroll.qml for the whole design. The tree is itself the
    // Flickable, so no ScrollView .contentItem indirection is needed here.
    //
    // Search UI rearchitecture: this is wired to tree's own Flickable
    // geometry, and while search results are showing tree is invisible — so
    // arming would just scroll a hidden view for no visible effect. Disabled
    // rather than adapted to also drive searchResultsView, which already gets
    // ordinary wheel/flick scrolling for free from ListView.
    MiddleDragScroll {
        id: middleDragScrollArea
        enabled: sidebar.searchQuery === ""
        anchors.top: ribbon.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        flickable: tree
    }

    // Item 40: folder-management context menu. Same custom-Popup shape as the
    // sortModeMenu above (this codebase has no QQC2 Menu) — plain Item rows,
    // each with a HoverHandler (pointer cursor) + TapHandler.
    Popup {
        id: contextMenu
        objectName: "contextMenu"
        x: sidebar.clampedMenuX(width)
        y: sidebar.clampedMenuY(height)
        width: 200
        padding: 4
        modal: true
        dim: true
        focus: false
        closePolicy: Popup.CloseOnPressOutside | Popup.CloseOnEscape

        background: Rectangle {
            color: Theme.surfaceDialog
            radius: AppSettings.borderRadius
            border.width: 1
            border.color: Theme.border
        }

        Column {
            width: contextMenu.width - contextMenu.padding * 2
            spacing: 2

            // "New folder" — always available; creates inside the clicked
            // directory (or the clicked note's parent dir, or the vault root
            // for an empty-area click).
            Item {
                id: ctxNewFolderRow
                objectName: "ctxNewFolderRow"
                width: parent.width
                height: 28

                HoverHandler {
                    id: ctxNewFolderHover
                    cursorShape: Qt.PointingHandCursor
                }
                TapHandler {
                    onTapped: {
                        contextMenu.close()
                        sidebar.beginCreateFolder(sidebar.ctxCreateInDir)
                    }
                }
                Rectangle {
                    anchors.fill: parent
                    radius: AppSettings.borderRadius
                    color: ctxNewFolderHover.hovered ? Theme.surfaceHover : "transparent"
                }
                Text {
                    anchors.left: parent.left
                    anchors.leftMargin: 8
                    anchors.verticalCenter: parent.verticalCenter
                    text: qsTr("New folder")
                    color: "white"
                    font.pixelSize: 12
                }
            }

            // "Rename" — any real row (note or folder), never the empty-area
            // click (ctxPath ""). Drops the row into inline edit mode rather
            // than opening a dialog; the same edit a click on an
            // already-selected row's name starts (see treeRowTap).
            //
            // Deferred: contextMenu is modal, and closing it hands focus back
            // before the editor could take it — forcing focus into a field
            // while the popup is still tearing down loses it again immediately,
            // which reads as the editor flashing open and shutting.
            Item {
                id: ctxRenameRow
                objectName: "ctxRenameRow"
                visible: sidebar.ctxIsNote || sidebar.ctxIsDir
                width: parent.width
                height: 28

                HoverHandler {
                    id: ctxRenameHover
                    cursorShape: Qt.PointingHandCursor
                }
                TapHandler {
                    onTapped: {
                        // Captured before close(): the menu's ctx* state is
                        // shared, and the deferred call below runs after it may
                        // have been retargeted.
                        const target = sidebar.ctxPath
                        const isDir = sidebar.ctxIsDir
                        const label = sidebar.ctxLabel
                        contextMenu.close()
                        Qt.callLater(() => sidebar.beginRename(target, isDir, label))
                    }
                }
                Rectangle {
                    anchors.fill: parent
                    radius: AppSettings.borderRadius
                    color: ctxRenameHover.hovered ? Theme.surfaceHover : "transparent"
                }
                Text {
                    anchors.left: parent.left
                    anchors.leftMargin: 8
                    anchors.verticalCenter: parent.verticalCenter
                    text: qsTr("Rename")
                    color: "white"
                    font.pixelSize: 12
                }
            }

            // "Move to…" — only for a note (audio / orphaned .md); a note
            // carries its sidecars when moved (VaultTreeModel::moveFile).
            Item {
                id: ctxMoveRow
                visible: sidebar.ctxIsNote
                width: parent.width
                height: 28

                HoverHandler {
                    id: ctxMoveHover
                    cursorShape: Qt.PointingHandCursor
                }
                TapHandler {
                    onTapped: {
                        contextMenu.close()
                        sidebar.openMoveTo()
                    }
                }
                Rectangle {
                    anchors.fill: parent
                    radius: AppSettings.borderRadius
                    color: ctxMoveHover.hovered ? Theme.surfaceHover : "transparent"
                }
                Text {
                    anchors.left: parent.left
                    anchors.leftMargin: 8
                    anchors.verticalCenter: parent.verticalCenter
                    text: qsTr("Move to…")
                    color: "white"
                    font.pixelSize: 12
                }
            }

            // "Delete folder" — only for a directory row (recursive delete,
            // guarded against the vault root in VaultTreeModel::deleteFolder).
            Item {
                id: ctxDeleteRow
                visible: sidebar.ctxIsDir
                width: parent.width
                height: 28

                HoverHandler {
                    id: ctxDeleteHover
                    cursorShape: Qt.PointingHandCursor
                }
                TapHandler {
                    onTapped: {
                        // No confirm up front: beginDeleteFolder() trashes
                        // (recoverable, Undo offered in a snackbar) and only
                        // prompts if the trash refuses. The folder's own
                        // deleteConfirmPopup is gone — fileDeleteConfirmPopup
                        // handles the one case that still needs confirming, for
                        // all three delete kinds.
                        contextMenu.close()
                        sidebar.beginDeleteFolder(sidebar.ctxPath)
                    }
                }
                Rectangle {
                    anchors.fill: parent
                    radius: AppSettings.borderRadius
                    color: ctxDeleteHover.hovered ? Theme.surfaceHover : "transparent"
                }
                Text {
                    anchors.left: parent.left
                    anchors.leftMargin: 8
                    anchors.verticalCenter: parent.verticalCenter
                    text: qsTr("Delete folder")
                    color: Theme.danger
                    font.pixelSize: 12
                }
            }

            // Item 2: "Delete transcript" — only for a transcribed AUDIO row
            // (status 2 == TranscribedStatus). Removes the sibling .md,
            // reverting the audio to untranscribed; the audio itself is kept.
            // (An orphaned .md has no audio to keep, so it gets "Delete file"
            // below instead, not this.)
            //
            // Acts on the whole multi-selection when the right-clicked row is
            // part of one, and names the count — see ctxDeleteFileRow below for
            // why the label must say so.
            Item {
                id: ctxDeleteTranscriptRow
                objectName: "ctxDeleteTranscriptRow"
                visible: sidebar.ctxIsAudio && sidebar.ctxStatus === 2
                readonly property int targetCount: ctxDeleteTranscriptRow.visible
                    ? sidebar.selectionPathsFor(sidebar.ctxPath, false).length : 1
                width: parent.width
                height: 28

                HoverHandler {
                    id: ctxDeleteTranscriptHover
                    cursorShape: Qt.PointingHandCursor
                }
                TapHandler {
                    onTapped: {
                        contextMenu.close()
                        sidebar.beginDeleteTranscript(
                            sidebar.selectionPathsFor(sidebar.ctxPath, false))
                    }
                }
                Rectangle {
                    anchors.fill: parent
                    radius: AppSettings.borderRadius
                    color: ctxDeleteTranscriptHover.hovered ? Theme.surfaceHover : "transparent"
                }
                Text {
                    objectName: "ctxDeleteTranscriptLabel"
                    anchors.left: parent.left
                    anchors.leftMargin: 8
                    anchors.verticalCenter: parent.verticalCenter
                    text: ctxDeleteTranscriptRow.targetCount > 1
                          ? qsTr("Delete %1 transcripts").arg(ctxDeleteTranscriptRow.targetCount)
                          : qsTr("Delete transcript")
                    color: Theme.danger
                    font.pixelSize: 12
                }
            }

            // "Delete audio only" — the complement of "Delete transcript", and
            // gated identically (transcribed AUDIO row). Drops the audio and
            // keeps the .md, which stays in the tree as an orphaned-.md row.
            //
            // Deliberately NOT shown for an untranscribed audio row: with no
            // .md to keep, it would do exactly what "Delete file" does, and two
            // menu items with one behavior is a worse menu.
            Item {
                id: ctxDeleteAudioOnlyRow
                objectName: "ctxDeleteAudioOnlyRow"
                visible: sidebar.ctxIsAudio && sidebar.ctxStatus === 2
                readonly property int targetCount: ctxDeleteAudioOnlyRow.visible
                    ? sidebar.selectionPathsFor(sidebar.ctxPath, false).length : 1
                width: parent.width
                height: 28

                HoverHandler {
                    id: ctxDeleteAudioOnlyHover
                    cursorShape: Qt.PointingHandCursor
                }
                TapHandler {
                    onTapped: {
                        contextMenu.close()
                        sidebar.beginDeleteAudioOnly(
                            sidebar.selectionPathsFor(sidebar.ctxPath, false))
                    }
                }
                Rectangle {
                    anchors.fill: parent
                    radius: AppSettings.borderRadius
                    color: ctxDeleteAudioOnlyHover.hovered ? Theme.surfaceHover : "transparent"
                }
                Text {
                    objectName: "ctxDeleteAudioOnlyLabel"
                    anchors.left: parent.left
                    anchors.leftMargin: 8
                    anchors.verticalCenter: parent.verticalCenter
                    text: ctxDeleteAudioOnlyRow.targetCount > 1
                          ? qsTr("Delete audio only for %1 items").arg(ctxDeleteAudioOnlyRow.targetCount)
                          : qsTr("Delete audio only")
                    color: Theme.danger
                    font.pixelSize: 12
                }
            }

            // Item 2: "Delete file" — for any note row (audio or orphaned .md):
            // deletes the file and all its sidecars (audio + .md + partial), or
            // just the orphaned .md alone.
            //
            // Acts on the whole multi-selection when the right-clicked row is
            // part of one, and says so ("Delete 3 items") rather than silently
            // deleting more than the one row named in the menu — same
            // selectionPathsFor() rule drag-to-move uses.
            Item {
                id: ctxDeleteFileRow
                objectName: "ctxDeleteFileRow"
                visible: sidebar.ctxIsNote
                // Re-evaluates on selectedPaths/ctxPath change (both are
                // properties), so the count is always the live selection's.
                readonly property int targetCount: sidebar.ctxIsNote
                    ? sidebar.selectionPathsFor(sidebar.ctxPath, false).length : 1
                width: parent.width
                height: 28

                HoverHandler {
                    id: ctxDeleteFileHover
                    cursorShape: Qt.PointingHandCursor
                }
                TapHandler {
                    onTapped: {
                        contextMenu.close()
                        sidebar.beginDeleteFile(
                            sidebar.selectionPathsFor(sidebar.ctxPath, false))
                    }
                }
                Rectangle {
                    anchors.fill: parent
                    radius: AppSettings.borderRadius
                    color: ctxDeleteFileHover.hovered ? Theme.surfaceHover : "transparent"
                }
                Text {
                    objectName: "ctxDeleteFileLabel"
                    anchors.left: parent.left
                    anchors.leftMargin: 8
                    anchors.verticalCenter: parent.verticalCenter
                    text: ctxDeleteFileRow.targetCount > 1
                          ? qsTr("Delete %1 items").arg(ctxDeleteFileRow.targetCount)
                          : qsTr("Delete file")
                    color: Theme.danger
                    font.pixelSize: 12
                }
            }
        }
    }

    // Item 40: "New folder" name entry.
    Popup {
        id: newFolderPopup
        x: sidebar.clampedMenuX(width)
        y: sidebar.clampedMenuY(height)
        width: 240
        padding: 8
        modal: true
        dim: true
        focus: true
        closePolicy: Popup.CloseOnPressOutside | Popup.CloseOnEscape

        background: Rectangle {
            color: Theme.surfaceDialog
            radius: AppSettings.borderRadius
            border.width: 1
            border.color: Theme.border
        }

        Column {
            width: newFolderPopup.width - newFolderPopup.padding * 2
            spacing: 6

            Text {
                text: qsTr("New folder")
                color: "white"
                font.pixelSize: 12
            }

            TextField {
                id: newFolderField
                width: parent.width
                placeholderText: qsTr("Folder name")
                background: UnderlineField { focused: newFolderField.activeFocus }
                onAccepted: sidebar.confirmNewFolder()

                HoverHandler {
                    cursorShape: Qt.IBeamCursor
                }
            }

            // Inline warning banner (StyleGuide.md): plain Label, visible on a
            // non-empty backing string, error color #e07a7a.
            Label {
                id: newFolderError
                width: parent.width
                visible: text !== ""
                text: ""
                color: Theme.danger
                font.pixelSize: 11
                wrapMode: Text.WordWrap
            }

            Row {
                anchors.right: parent.right
                spacing: 6

                Item {
                    id: newFolderCancelButton
                    width: newFolderCancelLabel.implicitWidth + 16
                    height: 24
                    HoverHandler {
                        id: newFolderCancelHover
                        cursorShape: Qt.PointingHandCursor
                    }
                    TapHandler {
                        onTapped: newFolderPopup.close()
                    }
                    HoverBorder {
                        anchors.fill: parent
                        active: newFolderCancelHover.hovered
                    }
                    Text {
                        id: newFolderCancelLabel
                        anchors.centerIn: parent
                        text: qsTr("Cancel")
                        color: Theme.textMuted
                        font.pixelSize: 12
                    }
                }

                Item {
                    id: newFolderCreateButton
                    width: newFolderCreateLabel.implicitWidth + 16
                    height: 24
                    HoverHandler {
                        id: newFolderCreateHover
                        cursorShape: Qt.PointingHandCursor
                    }
                    TapHandler {
                        onTapped: sidebar.confirmNewFolder()
                    }
                    HoverBorder {
                        anchors.fill: parent
                        active: newFolderCreateHover.hovered
                    }
                    Text {
                        id: newFolderCreateLabel
                        anchors.centerIn: parent
                        text: qsTr("Create")
                        color: "white"
                        font.pixelSize: 12
                    }
                }
            }
        }
    }

    // Item 40: "Move to…" destination picker — one row per vault folder
    // (VaultTreeModel.vaultFolders(), minus the file's own current dir).
    Popup {
        id: moveToPopup
        x: sidebar.clampedMenuX(width)
        y: sidebar.clampedMenuY(height)
        width: 260
        padding: 4
        modal: true
        dim: true
        focus: false
        closePolicy: Popup.CloseOnPressOutside | Popup.CloseOnEscape

        background: Rectangle {
            color: Theme.surfaceDialog
            radius: AppSettings.borderRadius
            border.width: 1
            border.color: Theme.border
        }

        Column {
            width: moveToPopup.width - moveToPopup.padding * 2
            spacing: 2

            Text {
                width: parent.width
                leftPadding: 8
                topPadding: 4
                bottomPadding: 2
                text: qsTr("Move to…")
                color: Theme.textMuted
                font.pixelSize: 11
            }

            Repeater {
                model: sidebar.moveFolderList

                delegate: Item {
                    id: moveFolderRow
                    required property string modelData
                    width: parent.width
                    height: 28

                    HoverHandler {
                        id: moveFolderHover
                        cursorShape: Qt.PointingHandCursor
                    }
                    TapHandler {
                        onTapped: sidebar.moveNoteTo(moveFolderRow.modelData)
                    }
                    Rectangle {
                        anchors.fill: parent
                        radius: AppSettings.borderRadius
                        color: moveFolderHover.hovered ? Theme.surfaceHover : "transparent"
                    }
                    Text {
                        anchors.left: parent.left
                        anchors.leftMargin: 8
                        anchors.verticalCenter: parent.verticalCenter
                        width: parent.width - 16
                        elide: Text.ElideMiddle
                        text: sidebar.folderDisplayName(moveFolderRow.modelData)
                        color: "white"
                        font.pixelSize: 12
                    }
                }
            }
        }
    }

    // The ONLY delete confirmation left, and it now means what it says: it is
    // reached only for entries the trash REFUSED (see handleTrashResult), so
    // confirming really does delete them permanently. Everything that trashed
    // successfully never gets here — it's already gone, with Undo offered in a
    // snackbar instead.
    //
    // Hence the wording below promises no trash: it used to say "Moved to the
    // trash" on every delete, which was exactly wrong for this path — this is
    // the one case where nothing is going to the trash. Serves all three delete
    // kinds; the message + confirm action switch on sidebar.pendingDeleteMode.
    ConfirmPopup {
        id: fileDeleteConfirmPopup
        // Click-anchored, in-scene popup (see clampedMenuX/Y): it spawns at the
        // cursor within the sidebar, NOT centered in the overlay like the other
        // confirm dialogs — so clear ConfirmPopup's centerIn anchor and keep the
        // clamped x/y and the narrower 280/8 chrome.
        anchors.centerIn: undefined
        x: sidebar.clampedMenuX(width)
        y: sidebar.clampedMenuY(height)
        width: 280
        padding: 8
        message: {
            // Every branch says "can't be moved to the trash" +
            // "permanently" + "can't be undone", because that is
            // precisely and only when this popup appears.
            if (sidebar.pendingDeleteMode === "transcript")
                return qsTr("The transcript for “%1” can't be moved to the trash. " +
                            "Delete it permanently? The audio file is kept. " +
                            "This can't be undone.")
                    .arg(sidebar.pendingDeleteName())
            if (sidebar.pendingDeleteMode === "audioOnly")
                return qsTr("The audio for “%1” can't be moved to the trash. " +
                            "Delete it permanently? The transcript is kept. " +
                            "This can't be undone.")
                    .arg(sidebar.pendingDeleteName())
            if (sidebar.pendingDeleteMode === "folder")
                return qsTr("The folder “%1” can't be moved to the trash. Delete it " +
                            "and everything in it permanently? This can't be undone.")
                    .arg(sidebar.pendingDeleteName())
            return sidebar.pendingDeletePaths.length > 1
                ? qsTr("%1 items can't be moved to the trash. Delete them and their " +
                       "transcripts permanently? This can't be undone.")
                    .arg(sidebar.pendingDeletePaths.length)
                : qsTr("“%1” can't be moved to the trash. Delete it and its transcript " +
                       "permanently? This can't be undone.")
                    .arg(sidebar.pendingDeleteName())
        }
        confirmText: sidebar.pendingDeleteMode === "transcript"
                     ? qsTr("Delete transcript")
                     : sidebar.pendingDeleteMode === "audioOnly"
                     ? qsTr("Delete audio")
                     : (sidebar.pendingDeletePaths.length > 1
                        ? qsTr("Delete %1 items").arg(sidebar.pendingDeletePaths.length)
                        : qsTr("Delete file"))
        onConfirmed: sidebar.confirmFileDelete()
    }
}
