import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SuopTranscriber

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

    // Item 17: suggestions for the tag fragment currently being typed (text
    // after the last comma), excluding tags already present earlier in the
    // field. Vault vocabulary via TranscriptionController.existingVaultTags
    // (Q_INVOKABLE, scans every .md's tags: frontmatter line fresh).
    function tagSuggestions() {
        if (!sidebar.transcriptionController || sidebar.vaultPath.toString() === "")
            return []
        const parts = sidebar.tagFilterText.split(",")
        const fragment = parts[parts.length - 1].trim().toLowerCase()
        if (fragment === "")
            return []
        const already = parts.slice(0, -1).map(t => t.trim().toLowerCase())
        const vocabulary = sidebar.transcriptionController.existingVaultTags(sidebar.vaultPath)
        return vocabulary.filter(tag =>
            tag.toLowerCase().startsWith(fragment) && !already.includes(tag.toLowerCase()))
    }

    // Replaces the in-progress (last, possibly partial) tag fragment with
    // the chosen suggestion, leaving a trailing ", " ready for the next tag.
    function acceptTagSuggestion(tag) {
        const parts = sidebar.tagFilterText.split(",")
        parts[parts.length - 1] = " " + tag
        sidebar.tagFilterText = parts.map(t => t.trim()).join(", ") + ", "
        tagFilterField.forceActiveFocus()
        tagFilterField.cursorPosition = tagFilterField.text.length
    }

    // --- Item 9 helpers: match highlighting + transcript snippet ------------
    // Rich-text (StyledText) escaping for plain segments around a highlight;
    // Qt rich text supports inline background-color spans fine (only
    // border-radius is unsupported, per the item 11 investigation).
    function escapeHtml(s) {
        return String(s).replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;")
    }

    // Wraps the first case-insensitive occurrence of query in text with a
    // highlight span; returns escaped-but-unwrapped text if there's no match.
    function highlightMatch(text, query) {
        const q = String(query)
        const t = String(text)
        if (q === "")
            return sidebar.escapeHtml(t)
        const idx = t.toLowerCase().indexOf(q.toLowerCase())
        if (idx === -1)
            return sidebar.escapeHtml(t)
        const before = sidebar.escapeHtml(t.substring(0, idx))
        const match = sidebar.escapeHtml(t.substring(idx, idx + q.length))
        const after = sidebar.escapeHtml(t.substring(idx + q.length))
        return before + "<span style=\"background-color:" + AppSettings.highlightColor + "\">" + match + "</span>" + after
    }

    // Collapses a raw transcript body (with "[start - end] text [SPEAKER_TURN]"
    // segment lines) into a single normalized-whitespace string for snippet
    // display — the sidebar has no need for segment boundaries, unlike
    // MainPanel's cross-segment search (item 12).
    function cleanTranscript(body) {
        return String(body).split("\n").map(function (line) {
            return line.replace(/^\[\d{2}:\d{2}:\d{2}\.\d{3} - \d{2}:\d{2}:\d{2}\.\d{3}\]\s*/, "")
                        .replace(/\s*\[SPEAKER_TURN\]\s*$/, "")
        }).join(" ").replace(/\s+/g, " ").trim()
    }

    // Ellipsis-bounded, highlighted snippet of cleaned transcript text around
    // the first match. Returns "" if query doesn't occur (caller should not
    // show a snippet row in that case).
    function transcriptSnippet(cleanedBody, query, contextChars) {
        const q = String(query)
        const body = String(cleanedBody)
        if (q === "" || body === "")
            return ""
        const idx = body.toLowerCase().indexOf(q.toLowerCase())
        if (idx === -1)
            return ""
        const start = Math.max(0, idx - contextChars)
        const end = Math.min(body.length, idx + q.length + contextChars)
        const before = (start > 0 ? "… " : "") + sidebar.escapeHtml(body.substring(start, idx))
        const match = sidebar.escapeHtml(body.substring(idx, idx + q.length))
        const after = sidebar.escapeHtml(body.substring(idx + q.length, end)) + (end < body.length ? " …" : "")
        return before + "<span style=\"background-color:" + AppSettings.highlightColor + "\">" + match + "</span>" + after
    }

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
    // reads metadataRevision/etc. as a dependency trick — this codebase has
    // already been bitten once by qmlcachegen (AOT) silently dropping an
    // unused dummy-property read meant only to force binding
    // re-evaluation (see treeDelegate.metadata's own comment above), so
    // every other "recompute on X" case in this file uses an explicit push
    // instead, and this follows the same idiom.
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
    // Item 41: which of the current row's per-row action buttons currently
    // holds keyboard focus — "" means focus is on the tree row itself (no
    // button focused), otherwise one of "transcribe"/"relabel"/"chapters"
    // (the actionButtonRow's declaration order). Left/Right move this in/out
    // and along the cluster; Enter/Return/Space activate the named button for
    // sidebar.currentPath. Kept as sidebar-level state (not real activeFocus
    // on a delegate child) because TreeView recycles delegates, so a focused
    // button would be lost the moment its row scrolls out of the pool.
    property string actionButtonFocus: ""
    // Reset button focus whenever the keyboard-current row changes (plain
    // arrow, group-jump, or a click that opens a different file) — leaving
    // the row must drop out of its action cluster back onto the row itself.
    onCurrentPathChanged: sidebar.actionButtonFocus = ""

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

    // transcribeAllButton/relabelAllButton/chaptersAllButton (see the ribbon
    // below) each pass their own eligibility map (untranscribedPaths/
    // transcribedPaths/chaptersPaths) here — with 2+ rows selected, bulk
    // narrows to that selection (intersected with eligibility); with 0 or 1
    // selected, bulk keeps its original "everything eligible" behavior so
    // single-file workflows (no selection ever made) are unaffected.
    function bulkTargetPaths(eligibleMap) {
        const eligible = Object.keys(eligibleMap)
        if (sidebar.selectedPaths.length < 2)
            return eligible
        return eligible.filter((p) => sidebar.selectedPaths.indexOf(p) >= 0)
    }

    // Item 44: the current multi-selection narrowed to AUDIO notes only.
    // combineSelected() concatenates audio, so an orphaned .md that got
    // selected must be dropped. Reads isAudio from searchResultNotes() (the
    // same visible-note metadata the search run-all eligibility uses, valid in
    // both text-search and tree modes), intersected with selectedPaths.
    // Returns [] for a selection of fewer than two audio files — the caller's
    // and the UI's >= 2 gate.
    function selectedAudioPaths() {
        const selected = sidebar.selectedPaths
        if (selected.length < 2)
            return []
        return sidebar.searchResultNotes()
            .filter((n) => n.isAudio && selected.indexOf(n.path) >= 0)
            .map((n) => n.path)
    }

    // Item 44: manual "combine selected" — hand the explicit, user-selected
    // audio files to the controller's combineSelected(), which reuses the same
    // combine job/worker the rule-driven auto-combine uses. Ordering (mtime
    // ascending) and the merged output name are decided controller-side; the
    // sidebar just supplies the file list as file:// URLs. No-op unless 2+
    // audio files are selected (the UI gates on the same condition).
    function combineSelectedFiles() {
        if (!sidebar.transcriptionController)
            return
        const paths = sidebar.selectedAudioPaths()
        if (paths.length < 2)
            return
        sidebar.transcriptionController.combineSelected(paths.map((p) => "file://" + p), sidebar.vaultPath)
    }

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
                           "file://" + n.path, AppSettings.defaultModel, false))
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
    // Items 24/53: search-scoped "run all jobs" — same eligibility as
    // searchTranscribePaths() (untranscribed audio in the filtered set), since
    // run-all's whole point is the full pipeline on files that need it; an
    // already-transcribed result's label/chapters are the other two search
    // buttons' job (runAllJobs() itself skips a redundant re-transcribe anyway).
    function searchRunAllPaths() {
        if (!sidebar.transcriptionController)
            return []
        return sidebar.searchResultNotes()
            .filter((n) => n.isAudio
                    && !sidebar.transcriptionController.matchesExistingTranscription(
                           "file://" + n.path, AppSettings.defaultModel, false))
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

    // Item 42: focus the in-app file explorer (this tree) and reveal `path` in
    // it — the target of the bottom bar's current-file label click, which used
    // to shell out to the OS file manager. `path` is normally already
    // sidebar.currentPath (BottomBar mirrors MainPanel's open file), so the row
    // already carries currentPath's persistent outline; this just gives the
    // tree keyboard focus and scrolls the row into view, syncing the
    // selection/cursor state so subsequent arrow-nav starts from it. Under an
    // active text search the TreeView is hidden, so focus the results list and
    // scroll to the row if it's among the matches (best-effort — no forced
    // search clear).
    function focusAndRevealNote(path) {
        if (path === "")
            return
        if (sidebar.searchQuery !== "") {
            searchResultsView.forceActiveFocus()
            sidebar.scrollNoteIntoView(path)
            return
        }
        tree.forceActiveFocus()
        sidebar.selectedPaths = [path]
        sidebar.lastClickedPath = path
        sidebar.selectionCursorPath = path
        sidebar.scrollNoteIntoView(path)
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

    // Shift+Up/Down: grow/shrink the selection between the fixed anchor
    // (lastClickedPath) and a moving cursor (selectionCursorPath), mirroring
    // Shift-click's contiguous range. Does NOT open a file (matches
    // handleNoteClick's shift branch, which only changes selectedPaths).
    function extendNoteSelection(direction) {
        sidebar.actionButtonFocus = ""
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

    // Right/Left move keyboard focus into and along the current row's action-
    // button cluster; Left off the first button (direction -1 from index 0)
    // returns to the row. currentRowActionButtons() lists exactly the buttons
    // the treeDelegate renders for currentPath, in VISUAL left-to-right order
    // — actionButtonRow is layoutDirection: Qt.RightToLeft, so its declaration
    // order (transcribe first) runs RIGHT-to-left on screen and this function
    // reverses it at the end. That reversal is what makes Right actually move
    // right: the cluster is anchored to the row's right edge, so walking
    // rightward from the label enters at the cluster's LEFTMOST button and
    // ends on transcribe (rightmost), and Left off the leftmost drops back to
    // the row — one continuous left-to-right axis from the filename outward.
    function currentRowActionButtons() {
        if (sidebar.currentPath === "" || sidebar.searchQuery !== "")
            return []
        for (let row = 0; row < tree.rows; row++) {
            const ri = tree.index(row, 0)
            if (searchProxy.data(ri, VaultTreeModel.PathRole) !== sidebar.currentPath)
                continue
            const isAudio = searchProxy.data(ri, VaultTreeModel.IsAudioRole)
            const isOrphanedMd = searchProxy.data(ri, VaultTreeModel.IsOrphanedMdRole)
            const relabelEligible = isAudio && searchProxy.data(ri, VaultTreeModel.StatusRole) === 2
            const chaptersEligible = relabelEligible || isOrphanedMd
            const list = []
            if (isAudio)
                list.push("transcribe")
            if (relabelEligible)
                list.push("relabel")
            if (chaptersEligible)
                list.push("chapters")
            // Item 52: detect-speakers matches rowDetectSpeakersButton's own
            // visibility (transcribed audio only — diarization decodes the
            // audio, so no orphaned .md, same rule as relabelEligible).
            if (relabelEligible)
                list.push("detectSpeakers")
            // Items 24/53: run-all matches rowRunAllButton's own visibility
            // (any note — audio or orphaned .md).
            if (isAudio || isOrphanedMd)
                list.push("runAll")
            // Built above in actionButtonRow's declaration order (so the
            // eligibility rules read alongside the buttons they mirror);
            // reversed here into the on-screen left-to-right order Left/Right
            // navigate along — see this function's doc comment.
            return list.reverse()
        }
        return []
    }
    function focusActionButton(direction) {
        const order = sidebar.currentRowActionButtons()
        if (order.length === 0)
            return
        const next = order.indexOf(sidebar.actionButtonFocus) + direction
        if (next < 0) {
            sidebar.actionButtonFocus = ""    // Left out of the cluster, back to the row
            return
        }
        sidebar.actionButtonFocus = order[Math.min(next, order.length - 1)]
    }
    // Enter/Return/Space on a keyboard-focused action button — replays the
    // exact controller call that button's own TapHandler makes, for
    // currentPath.
    function activateFocusedActionButton() {
        if (sidebar.actionButtonFocus === "" || sidebar.currentPath === ""
                || !sidebar.transcriptionController)
            return
        const url = "file://" + sidebar.currentPath
        if (sidebar.actionButtonFocus === "transcribe")
            sidebar.transcriptionController.transcribe(
                url, sidebar.vaultPath, AppSettings.defaultModel, false,
                AppSettings.autoLabelEnabled, AppSettings.restrictTagsToExisting,
                AppSettings.tagReuseBias, AppSettings.wordiness)
        else if (sidebar.actionButtonFocus === "relabel")
            sidebar.transcriptionController.relabel(
                url, sidebar.vaultPath, AppSettings.restrictTagsToExisting,
                AppSettings.tagReuseBias, AppSettings.wordiness)
        else if (sidebar.actionButtonFocus === "chapters")
            sidebar.transcriptionController.generateChapters(url, sidebar.vaultPath, AppSettings.chapterDensity)
        else if (sidebar.actionButtonFocus === "detectSpeakers")
            sidebar.transcriptionController.diarizeFile(url, sidebar.vaultPath)
        else if (sidebar.actionButtonFocus === "runAll")
            sidebar.transcriptionController.runAllJobs(
                url, sidebar.vaultPath, AppSettings.defaultModel, false,
                AppSettings.restrictTagsToExisting, AppSettings.tagReuseBias,
                AppSettings.wordiness, AppSettings.chapterDensity)
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
        // Item 3: entering/leaving text search swaps which flattened order
        // visibleNotePaths() walks (searchResultsList vs. the TreeView row
        // walk) — a Shift-range anchor or selection built in one context
        // doesn't map cleanly onto the other, so start fresh rather than
        // risk a stale/confusing carry-over.
        sidebar.selectedPaths = []
        sidebar.lastClickedPath = ""
        sidebar.selectionCursorPath = ""
        sidebar.actionButtonFocus = ""
        // The tree swaps out for the flat results list (and back) on this
        // change, destroying the row being renamed along with it.
        sidebar.cancelRename()
    }
    onSearchIncludeTranscriptionsChanged: sidebar.refreshSearchResults()
    Connections {
        target: sidebar
        function onMetadataRevisionChanged() { sidebar.refreshSearchResults() }
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
            sidebar.refreshTreeOrdering()
            // Item 3: vault contents changed (file added/removed/renamed) —
            // a stale selectedPaths entry could point at a file that no
            // longer exists, so drop the selection defensively rather than
            // risk bulk actions silently no-op'ing on a vanished path.
            sidebar.selectedPaths = []
            sidebar.lastClickedPath = ""
            sidebar.selectionCursorPath = ""
            sidebar.actionButtonFocus = ""
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
    }

    // O9: bumped on every transcribe/relabel completion so each realized
    // delegate's metadata (label/tags/etc., read fresh off disk) can push
    // itself up to date instead of staying frozen at delegate-creation time
    // — see treeDelegate.refreshMetadata()/metadata below for why this is a
    // push (explicit assignment) rather than a binding-dependency trick.
    property int metadataRevision: 0
    // P6: same push idiom as metadataRevision above, but for job-status
    // (queued/active/paused/etc.) — jobStatusForPath() is a plain
    // Q_INVOKABLE lookup, not a reactive property, so delegates need an
    // explicit nudge (via jobStatusRevision below) whenever the controller
    // says some path's resolved status may have changed.
    property int jobStatusRevision: 0
    Connections {
        target: sidebar.transcriptionController
        function onFinished(fileUrl, ok, error) {
            sidebar.metadataRevision += 1
            sidebar.refreshTreeOrdering()
            // Item 6: labels/tags/chapters are written in-place into an
            // already-existing .md, which QFileSystemWatcher::directoryChanged
            // never fires for — so the tree row would stay stale until some
            // unrelated directory event (e.g. the NEXT file's .md being
            // created) triggered a rebuild ("delayed by one"). Nudge the model
            // to re-query just this row now that its file finished writing.
            if (ok && sidebar.model && sidebar.model.refreshFile)
                sidebar.model.refreshFile(fileUrl)
        }
        function onJobStatusesChanged() {
            sidebar.jobStatusRevision += 1
            sidebar.refreshTreeOrdering()
        }
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
    // refreshTreeOrdering() — the badge itself is still only ever shown for
    // "ready" groups via treeDelegate.combineEligible, but reusing the one
    // all-statuses map here is what keeps this in lockstep with the tree's
    // own intra-group display order, which also needs an order for
    // non-ready groups to sort consistently).
    property var combineOrderPaths: ({})

    // Sort mode / combine-candidate clustering (see AppSettings.sortMode's
    // doc comment and CombineGroupOutline usage below): computed once here,
    // covering three concerns that all boil down to "what order do files
    // appear in" and so have to be computed together to stay consistent —
    // see refreshTreeOrdering()'s body for how each is derived.
    function refreshTreeOrdering() {
        const newMap = {}
        const audioPaths = (sidebar.transcriptionController && sidebar.model) ? sidebar.model.allAudioPaths() : []
        const groups = (audioPaths.length > 0 && AppSettings.combineRules.length > 0)
                ? sidebar.transcriptionController.detectCombineGroups(AppSettings.combineRules, audioPaths) : []
        for (const g of groups) {
            if (g.status === "ready") {
                for (const f of g.files)
                    newMap[f] = g.outputPath
            }
        }

        // clusterKeyOf: path -> stable group id, for every group with 2+
        // files regardless of ready/pending/alreadyTranscribed/
        // alreadyCombined status — clustering ("stick together in the file
        // tree") is a broader concern than the ready-only outline highlight
        // above, since even a not-currently-combinable group still benefits
        // from its members sitting next to each other.
        //
        // groupFileOrder: path -> 1-based index within g.files, same
        // all-statuses sweep. g.files is already in whatever order
        // findCombineGroups() actually uses to combine (the rule's
        // configured sort differentiator if it has one and every member
        // captured a value for it, else mtime ascending — see that
        // function's doc comment in TranscriptionController.cpp) — reusing
        // that order here, instead of independently re-deriving an
        // mtime-only rank in QML, is what guarantees the tree's display
        // order and the per-row combine-order badge's numbering (both driven
        // from this one map — see memberOrderRank below and
        // sidebar.combineOrderPaths) can never disagree, even for a rule
        // whose combine order isn't mtime-based at all.
        const clusterKeyOf = {}
        const groupFileOrder = {}
        for (const g of groups) {
            if (g.files.length >= 2) {
                for (let i = 0; i < g.files.length; i++) {
                    const f = g.files[i]
                    clusterKeyOf[f] = g.outputPath
                    groupFileOrder[f] = i + 1
                }
            }
        }

        // ownRank: this file's own comparable value for the active sort
        // mode, as a plain string so the C++ proxy only ever does a QString
        // compare (see VaultSearchProxyModel::fileSortKeys' doc comment).
        // mtimes is only fetched when actually needed — audioLastModified()
        // walks the whole tree, no need to pay for that on the (default)
        // "name" mode.
        const mtimes = (AppSettings.sortMode !== "name" && sidebar.model) ? sidebar.model.audioLastModified() : {}
        function ownRank(path) {
            if (AppSettings.sortMode === "name") {
                const slash = path.lastIndexOf("/")
                return (slash >= 0 ? path.slice(slash + 1) : path).toLowerCase()
            }
            const ms = mtimes[path] || 0
            // Number.MAX_SAFE_INTEGER, not some larger power of ten — mtimes
            // are milliseconds-since-epoch (~1.7e12 today), safely inside
            // JS's exact-integer range, so this subtraction (for "newest
            // first") stays exact. A bigger constant like 1e20 would lose
            // enough float64 precision at this magnitude to collide distinct
            // timestamps into the same padded string.
            const value = AppSettings.sortMode === "dateAsc" ? ms : (Number.MAX_SAFE_INTEGER - ms)
            return String(value).padStart(16, "0")
        }

        // memberOrderRank: a group member's position AMONG ITS OWN
        // groupmates, always following g.files' own combine order (via
        // groupFileOrder above) regardless of AppSettings.sortMode — so the
        // first-to-be-combined file is always at the top. That order is
        // mtime ascending BY DEFAULT, but a rule with a configured sort
        // differentiator orders members by that instead (see
        // groupFileOrder's doc comment) — deriving this from the same map
        // the combine-order badge uses, rather than independently
        // recomputing an mtime rank in QML, keeps display order and badge
        // numbering from ever disagreeing. Only ever used as sortKeys'
        // second element for grouped files below — ungrouped files still
        // use plain ownRank, so vaults without combine rules see no change.
        function memberOrderRank(path) {
            return String(groupFileOrder[path] || 0).padStart(16, "0")
        }

        // groupRank: the MINIMUM ownRank among a group's members, for every
        // file in a 2+-member group, else just the file's own ownRank (a
        // lone file is its own one-member "group"). Since ownRank's
        // encoding above already flips for "dateDesc" (newest maps to the
        // smallest padded string), taking the min here naturally picks
        // whichever member is "first" under the CURRENT sort direction as
        // the group's representative — its earliest member for "name"/
        // "dateAsc", its most recently modified member for "dateDesc" — so
        // a group sorts alongside other items near its own most-relevant
        // edge instead of always anchoring to one fixed member regardless
        // of direction. With sortMode "name" and no active groups,
        // groupRank === ownRank for every file, so ordering is
        // byte-identical to plain alphabetical — no behavior change for
        // vaults without combine rules.
        const groupRankOf = {}
        for (const path of audioPaths) {
            const key = clusterKeyOf[path]
            if (key === undefined)
                continue
            const rank = ownRank(path)
            if (groupRankOf[key] === undefined || rank < groupRankOf[key])
                groupRankOf[key] = rank
        }

        // Three elements, compared in order: groupRank (where the whole
        // cluster sits among everything else), then the cluster's own
        // IDENTITY, then the member's position within it.
        //
        // Bug: this used to be just [groupRank, memberOrderRank], which
        // silently stopped clustering whenever two DIFFERENT groups computed
        // the same groupRank — not a contrived case, it's the norm for this
        // app's own "acme mic 3" rule. That rule keys on date+edit/orig, and
        // the recorder writes each pair's edit and orig files at the same
        // instant, so both groups have byte-identical mtimes and therefore
        // an identical min-mtime groupRank. With element 0 tied, element 1
        // tied too (each pair's edit and orig are both member #1 of their
        // own group), so every comparison fell through to the proxy's
        // row()-order default — i.e. plain alphabetical, which interleaves
        // edit/orig one row apart. Nothing was adjacent, so every member
        // also drew its own isolated CombineGroupOutline box instead of one
        // merged run (that outline reads adjacency off displayOrder below,
        // so it was consistently wrong in the same way, not separately
        // broken). Making cluster identity its own tier means a tie on
        // groupRank orders the two clusters arbitrarily-but-consistently
        // rather than shuffling their members together.
        //
        // Ungrouped files use their own path as the identity — unique per
        // file, so a lone file whose ownRank happens to tie a group's
        // groupRank lands cleanly before or after that whole group instead
        // of inside it.
        const sortKeys = {}
        for (const path of audioPaths) {
            const key = clusterKeyOf[path]
            sortKeys[path] = [key !== undefined ? groupRankOf[key] : ownRank(path),
                               key !== undefined ? key : path,
                               key !== undefined ? memberOrderRank(path) : ownRank(path)]
        }

        // Adjacency (combineTopMerged/combineBottomMerged) has to be
        // computed from the SAME order the tree actually ends up displaying
        // — see this property's doc comment above for the bug that bit when
        // this used allAudioPaths()'s raw order instead. Must compare every
        // element VaultSearchProxyModel::lessThan() does, in the same order,
        // for exactly that reason.
        const displayOrder = audioPaths.slice().sort((a, b) => {
            const ka = sortKeys[a], kb = sortKeys[b]
            for (let i = 0; i < 3; i++) {
                if (ka[i] !== kb[i])
                    return ka[i] < kb[i] ? -1 : 1
            }
            return 0
        })
        const topMerged = {}
        const bottomMerged = {}
        for (let i = 0; i < displayOrder.length; i++) {
            const path = displayOrder[i]
            if (newMap[path] === undefined)
                continue
            if (i > 0 && newMap[displayOrder[i - 1]] === newMap[path])
                topMerged[path] = true
            if (i < displayOrder.length - 1 && newMap[displayOrder[i + 1]] === newMap[path])
                bottomMerged[path] = true
        }

        sidebar.combineReadyPaths = newMap
        sidebar.combineOrderPaths = groupFileOrder
        sidebar.combineTopMergedPaths = topMerged
        sidebar.combineBottomMergedPaths = bottomMerged
        searchProxy.fileSortKeys = sortKeys
    }
    Component.onCompleted: sidebar.refreshTreeOrdering()
    Connections {
        target: AppSettings
        function onCombineRulesChanged() { sidebar.refreshTreeOrdering() }
        function onSortModeChanged() { sidebar.refreshTreeOrdering() }
    }

    // Item 51 refinement: stable, distinct color per group, derived from the
    // group's outputPath (unique per group, stable across rescans) via a
    // simple string hash -> hue — no sequential group-index bookkeeping
    // needed, so it stays correct even as groups appear/disappear/reorder.
    // Fixed saturation/lightness chosen to stay readable against this app's
    // dark sidebar background regardless of hue.
    function combineGroupColor(outputPath) {
        let hash = 0
        for (let i = 0; i < outputPath.length; i++)
            hash = (hash * 31 + outputPath.charCodeAt(i)) | 0
        const hue = (Math.abs(hash) % 360) / 360
        return Qt.hsla(hue, 0.55, 0.6, 1.0)
    }

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

    // Is `path` the folder `folderPath` itself, or anything inside it? The
    // vault tree IS the directory structure, so containment is a pure path
    // prefix test — no model walk needed. The trailing "/" matters: without
    // it "Interviews2/x.mp3" would count as being inside "Interviews".
    function isUnderFolder(path, folderPath) {
        return folderPath !== "" && path !== ""
                && (path === folderPath || path.startsWith(folderPath + "/"))
    }

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

    // Same tracking pattern, inverted: already-transcribed audio files, for
    // "regenerate all labels" (Wave D item 7).
    property var transcribedPaths: ({})
    function setRelabelEligible(path, eligible) {
        if (eligible)
            sidebar.transcribedPaths[path] = true
        else
            delete sidebar.transcribedPaths[path]
    }

    // Same tracking pattern again, for "regenerate all chapters"
    // (chaptersAllButton) — a superset of transcribedPaths above, since
    // chapter generation (TranscriptionController::generateChaptersJob())
    // only ever reads the .md's existing transcript body and never touches
    // audio, so an orphaned .md (audio deleted, text still present) is just
    // as eligible as a transcribed audio row. See treeDelegate.chaptersEligible.
    property var chaptersPaths: ({})
    function setChaptersEligible(path, eligible) {
        if (eligible)
            sidebar.chaptersPaths[path] = true
        else
            delete sidebar.chaptersPaths[path]
    }

    // P6: path -> JobStatus int (Queued/Active/Paused only — see setJobStatus),
    // same keyed-hash idiom as untranscribedPaths/transcribedPaths above,
    // populated by each realized delegate's own jobStatus property.
    property var jobStatuses: ({})
    function setJobStatus(path, status) {
        // Only Queued(0)/Active(1)/Paused(2) are "in progress" — None(-1),
        // Done(3) and Failed(4) aren't interesting for this indicator (see
        // treeDelegate's existing transcribed/untranscribed status dot).
        if (status === 0 || status === 1 || status === 2)
            sidebar.jobStatuses[path] = status
        else
            delete sidebar.jobStatuses[path]
    }

    // TitleBar's "⋯" menu action: unlike transcribeAllButton (which only
    // targets untranscribedPaths/bulkEligible files realized in the current
    // TreeView viewport), this queues every audio file in the vault,
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
        sidebar.transcriptionController.transcribeAll(urls, sidebar.vaultPath, AppSettings.defaultModel, false,
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

    function beginRename(path, isDir, label) {
        if (path === "")
            return
        // Starting a rename elsewhere while one is open commits the open one,
        // matching the blur convention.
        if (sidebar.renamingPath !== "" && sidebar.renamingPath !== path)
            sidebar.commitActiveRename()
        sidebar.renamingIsDir = isDir === true
        sidebar.renamingLabel = label ? label : ""
        sidebar.renamingText = sidebar.renameInitialText(path, sidebar.renamingIsDir, sidebar.renamingLabel)
        sidebar.renamingPath = path
    }

    function cancelRename() {
        sidebar.renamingPath = ""
        sidebar.renamingText = ""
    }

    // The text the editor opens with — always what renameFile()/renameFolder()
    // actually consume, which is NOT always what the row displays:
    //
    // - A folder edits its own name.
    // - A note showing a stored label (AppSettings.useLabelsSidebar) edits that
    //   LABEL, not the filename — you rename what you can see, matching
    //   MainPanel's titleField convention for the same toggle.
    // - Any other note edits its BASE name, extension always stripped, even
    //   when AppSettings.hideExtensionsSidebar is off and the row therefore
    //   shows "talk.m4a". renameFile() takes a base name and re-appends the
    //   real extension itself, so seeding "talk.m4a" here would rename the file
    //   to "talk.m4a.m4a". The editor showing one fewer token than the row did
    //   is the honest representation of what's being edited.
    function renameInitialText(path, isDir, label) {
        const base = path.substring(path.lastIndexOf("/") + 1)
        if (isDir)
            return base
        if (AppSettings.useLabelsSidebar && label)
            return label
        const dot = base.lastIndexOf(".")
        return dot > 0 ? base.substring(0, dot) : base
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
        sidebar.renamingPath = ""
        sidebar.renamingText = ""
        Qt.callLater(() => sidebar.performRename(path, isDir, label, newValue))
    }

    function performRename(path, isDir, label, newValue) {
        const trimmed = newValue.trim()
        if (trimmed === "" || trimmed === sidebar.renameInitialText(path, isDir, label))
            return // empty or unchanged — silently keep the old name

        if (isDir) {
            if (sidebar.model && !sidebar.model.renameFolder("file://" + path, trimmed)) {
                sidebar.rowActionError = qsTr("Couldn't rename folder — name taken or invalid.")
                rowActionErrorClearTimer.restart()
            }
            return
        }

        // Editing a displayed label rewrites the .md's frontmatter, not the
        // filename. setLabel() emits finished(), which already bumps
        // metadataRevision and refreshes the row (see the Connections above) —
        // no rebuild, so nothing extra to nudge here.
        if (AppSettings.useLabelsSidebar && label) {
            if (sidebar.transcriptionController)
                sidebar.transcriptionController.setLabel("file://" + path, trimmed)
            return
        }

        if (sidebar.model && !sidebar.model.renameFile("file://" + path, trimmed)) {
            sidebar.rowActionError = qsTr("Couldn't rename — name taken or invalid.")
            rowActionErrorClearTimer.restart()
        }
    }

    function openMoveTo() {
        const all = sidebar.model ? sidebar.model.vaultFolders() : []
        const ownDir = sidebar.ctxPath.substring(0, sidebar.ctxPath.lastIndexOf("/"))
        sidebar.moveFolderList = all.filter((f) => f !== ownDir)
        moveToPopup.open()
    }

    function moveNoteTo(destDir) {
        if (sidebar.model)
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
    // authority (vault containment, collisions, sidecar integrity) — this only
    // covers the two rejections a user causes by AIMING, so the drop target
    // simply refuses to light up instead of taking the drop and failing: a
    // no-op move into the row's own current directory, and a folder into itself
    // or one of its own descendants.
    function canDropRowsOn(destDir) {
        if (!sidebar.rowDragActive || destDir === "")
            return false
        for (let i = 0; i < sidebar.rowDragPaths.length; i++) {
            const p = sidebar.rowDragPaths[i]
            if (p.substring(0, p.lastIndexOf("/")) === destDir)
                return false
            if (sidebar.rowDragIsDir && (destDir === p || destDir.startsWith(p + "/")))
                return false
        }
        return true
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
        if (!sidebar.model || destDir === "")
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
        // Same transient-banner treatment as Main.qml's drag-and-drop import
        // failures, which this gesture is the in-vault sibling of.
        if (failed.length > 0) {
            sidebar.rowActionError = qsTr("Couldn't move: %1").arg(failed.join(", "))
            rowActionErrorClearTimer.restart()
        }
    }

    // Transient failure feedback for any row-targeted action the model refuses
    // — a move (drag/"Move to…"), an inline rename, or a delete. All of them
    // are otherwise completely silent: the row just stays put / keeps its old
    // name, with nothing to tell the user why. Shared rather than one banner
    // per action: the placement and lifetime are identical, and only one of
    // these gestures can be in flight at a time.
    property string rowActionError: ""

    Timer {
        id: rowActionErrorClearTimer
        interval: 4000
        onTriggered: sidebar.rowActionError = ""
    }

    function confirmDeleteFolder() {
        if (sidebar.model && sidebar.ctxPath !== "")
            sidebar.model.deleteFolder("file://" + sidebar.ctxPath)
        deleteConfirmPopup.close()
    }

    // Item 2 (delete transcript / delete file): both destructive file actions
    // route through the shared fileDeleteConfirmPopup below, keyed by
    // pendingDeleteMode. VaultTreeModel::deleteTranscript()/deleteFile()
    // move files to the trash (recoverable), but a confirm is still required
    // for a destructive user action.
    //
    // An ARRAY, not a single path: "Delete file" acts on the whole
    // multi-selection when the right-clicked row is part of one (see
    // selectionPathsFor, shared with drag-to-move). "Delete transcript" stays
    // deliberately single-file — it's offered only for the one right-clicked
    // transcribed audio row, so it always has exactly one target.
    property var pendingDeletePaths: []
    property string pendingDeleteMode: "" // "transcript" | "file"

    // The single filename a confirm message names, for the 1-target case.
    function pendingDeleteName() {
        if (sidebar.pendingDeletePaths.length !== 1)
            return ""
        const p = sidebar.pendingDeletePaths[0]
        return p.substring(p.lastIndexOf("/") + 1)
    }

    function beginDeleteTranscript(path) {
        sidebar.pendingDeletePaths = [path]
        sidebar.pendingDeleteMode = "transcript"
        fileDeleteConfirmPopup.open()
    }

    function beginDeleteFile(path) {
        sidebar.pendingDeletePaths = sidebar.selectionPathsFor(path, false)
        sidebar.pendingDeleteMode = "file"
        fileDeleteConfirmPopup.open()
    }

    function confirmFileDelete() {
        const paths = sidebar.pendingDeletePaths
        if (sidebar.model && paths.length > 0) {
            if (sidebar.pendingDeleteMode === "transcript") {
                // Always exactly one target — see pendingDeletePaths.
                sidebar.model.deleteTranscript("file://" + paths[0])
            } else {
                // One C++ call for the whole batch — deleteFiles() rebuilds the
                // model once rather than once per file (see its doc comment).
                const urls = paths.map((p) => "file://" + p)
                const deleted = sidebar.model.deleteFiles(urls)
                if (deleted < paths.length) {
                    sidebar.rowActionError = qsTr("Couldn't delete %1 of %2 items.")
                        .arg(paths.length - deleted).arg(paths.length)
                    rowActionErrorClearTimer.restart()
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

    // Vertical arrow-key navigation between notes, gated on the tree/
    // search-results view actually having keyboard focus (grabbed by their
    // TapHandlers above on click) — inert while a TextField elsewhere has
    // focus, since only one item can have activeFocus at a time.
    Shortcut {
        sequence: "Up"
        enabled: tree.activeFocus || searchResultsView.activeFocus
        onActivated: sidebar.moveNoteSelection(-1)
    }
    Shortcut {
        sequence: "Down"
        enabled: tree.activeFocus || searchResultsView.activeFocus
        onActivated: sidebar.moveNoteSelection(1)
    }

    // Item 41: Shift+Up/Down extend the selection range from the anchor
    // (mirrors Shift-click), same focus gating as plain Up/Down.
    Shortcut {
        sequence: "Shift+Up"
        enabled: tree.activeFocus || searchResultsView.activeFocus
        onActivated: sidebar.extendNoteSelection(-1)
    }
    Shortcut {
        sequence: "Shift+Down"
        enabled: tree.activeFocus || searchResultsView.activeFocus
        onActivated: sidebar.extendNoteSelection(1)
    }

    // Item 41: Ctrl+Up/Down jump to the current group's (parent-folder's)
    // first/last note, then hop group-by-group on repeat.
    Shortcut {
        sequence: "Ctrl+Up"
        enabled: tree.activeFocus || searchResultsView.activeFocus
        onActivated: sidebar.jumpToGroupBoundary(-1)
    }
    Shortcut {
        sequence: "Ctrl+Down"
        enabled: tree.activeFocus || searchResultsView.activeFocus
        onActivated: sidebar.jumpToGroupBoundary(1)
    }

    // Item 41: Left/Right move keyboard focus into/along the current row's
    // per-row action-button cluster (transcribe/relabel/chapters). Tree-only:
    // the flat search-results view has no such cluster. Right enters the
    // first button; Left off the first button returns to the row.
    Shortcut {
        sequence: "Right"
        enabled: tree.activeFocus
        onActivated: sidebar.focusActionButton(1)
    }
    Shortcut {
        sequence: "Left"
        enabled: tree.activeFocus
        onActivated: sidebar.focusActionButton(-1)
    }
    // Enter/Return/Space activate the keyboard-focused action button; inert
    // (falls through to any other handler) unless a button is actually
    // focused, so they don't shadow unrelated Space/Return uses otherwise.
    Shortcut {
        sequences: ["Return", "Enter", "Space"]
        enabled: tree.activeFocus && sidebar.actionButtonFocus !== ""
        onActivated: sidebar.activateFocusedActionButton()
    }
    // Escape drops out of the action cluster back onto the row (only while a
    // button is focused, so it still reaches search-close etc. otherwise).
    Shortcut {
        sequence: "Escape"
        enabled: tree.activeFocus && sidebar.actionButtonFocus !== ""
        onActivated: sidebar.actionButtonFocus = ""
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
        height: sidebar.searchExpanded ? expandedColumn.implicitHeight : 32

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
                        // Item 3: with 2+ rows selected, narrow to the
                        // selection (intersected with eligibility) —
                        // otherwise (0 or 1 selected) keep acting on every
                        // untranscribed file, same as before this item.
                        const paths = sidebar.bulkTargetPaths(sidebar.untranscribedPaths)
                        if (paths.length === 0)
                            return
                        // Model paths are raw local paths, not file:// URLs
                        // (see VaultTreeModel's PathRole) — Q_INVOKABLE
                        // QUrl params need the scheme or QUrl::toLocalFile()
                        // silently returns empty.
                        const urls = paths.map((p) => "file://" + p)
                        sidebar.transcriptionController.transcribeAll(urls, sidebar.vaultPath, AppSettings.defaultModel,
                                                                        false, AppSettings.autoLabelEnabled,
                                                                        AppSettings.restrictTagsToExisting,
                                                                        AppSettings.tagReuseBias, AppSettings.wordiness)
                    }
                }
                HoverBorder {
                    anchors.fill: parent
                    active: transcribeAllHover.hovered
                }
                AppIcon {
                    anchors.centerIn: parent
                    name: "transcribe"
                    color: "#dddddd"
                    iconSize: 15
                }

                ToolTip.visible: transcribeAllHover.hovered
                ToolTip.text: qsTr("Transcribe all untranscribed audio")
                ToolTip.delay: 500
            }

            // Wave D item 7: bulk "regenerate auto-title" mirroring
            // transcribeAllButton above, over already-transcribed files
            // instead of untranscribed ones.
            Item {
                id: relabelAllButton
                Layout.preferredWidth: 22
                Layout.preferredHeight: 22
                // Item 3: same selected-vs-all narrowing as transcribeAllButton.
                enabled: sidebar.transcriptionController && sidebar.bulkTargetPaths(sidebar.transcribedPaths).length > 0

                HoverHandler {
                    id: relabelAllHover
                    cursorShape: Qt.PointingHandCursor
                    enabled: relabelAllButton.enabled
                }
                TapHandler {
                    enabled: relabelAllButton.enabled
                    onTapped: {
                        const paths = sidebar.bulkTargetPaths(sidebar.transcribedPaths)
                        const urls = paths.map((p) => "file://" + p)
                        sidebar.transcriptionController.relabelAll(urls, sidebar.vaultPath,
                                                                     AppSettings.restrictTagsToExisting,
                                                                     AppSettings.tagReuseBias, AppSettings.wordiness)
                    }
                }
                HoverBorder {
                    anchors.fill: parent
                    active: relabelAllButton.enabled && relabelAllHover.hovered
                }
                AppIcon {
                    anchors.centerIn: parent
                    name: "relabel"
                    color: relabelAllButton.enabled ? "#dddddd" : "#666666"
                    iconSize: 15
                }

                ToolTip.visible: relabelAllHover.hovered
                ToolTip.text: qsTr("Regenerate all auto-titles")
                ToolTip.delay: 500
            }

            // Bulk "regenerate chapters", same shape as relabelAllButton
            // above but over chaptersPaths (a superset of transcribedPaths —
            // see treeDelegate.chaptersEligible for why orphaned .md rows
            // qualify too) and calls the chapters-only bulk entry point,
            // generateChaptersAll().
            Item {
                id: chaptersAllButton
                Layout.preferredWidth: 22
                Layout.preferredHeight: 22
                // Item 3: same selected-vs-all narrowing as transcribeAllButton.
                enabled: sidebar.transcriptionController && sidebar.bulkTargetPaths(sidebar.chaptersPaths).length > 0

                HoverHandler {
                    id: chaptersAllHover
                    cursorShape: Qt.PointingHandCursor
                    enabled: chaptersAllButton.enabled
                }
                TapHandler {
                    enabled: chaptersAllButton.enabled
                    onTapped: {
                        const paths = sidebar.bulkTargetPaths(sidebar.chaptersPaths)
                        const urls = paths.map((p) => "file://" + p)
                        sidebar.transcriptionController.generateChaptersAll(urls, sidebar.vaultPath,
                                                                              AppSettings.chapterDensity)
                    }
                }
                HoverBorder {
                    anchors.fill: parent
                    active: chaptersAllButton.enabled && chaptersAllHover.hovered
                }
                AppIcon {
                    anchors.centerIn: parent
                    name: "chapters"
                    color: chaptersAllButton.enabled ? "#dddddd" : "#666666"
                    iconSize: 15
                }

                ToolTip.visible: chaptersAllHover.hovered
                ToolTip.text: qsTr("Regenerate all chapters")
                ToolTip.delay: 500
            }

            // Item 52 (CLAUDE.md per-file-action parity): bulk "detect speakers"
            // — runs standalone diarization over the transcribed audio files
            // (same transcribedPaths eligibility relabelAllButton uses, since
            // diarization needs an existing transcript), calling
            // diarizeFilesForPaths(). Mirrors MainPanel's detectSpeakersButton
            // and the per-row rowDetectSpeakersButton below.
            Item {
                id: detectSpeakersAllButton
                Layout.preferredWidth: 22
                Layout.preferredHeight: 22
                // Same selected-vs-all narrowing as the other ribbon buttons.
                enabled: sidebar.transcriptionController && sidebar.bulkTargetPaths(sidebar.transcribedPaths).length > 0

                HoverHandler {
                    id: detectSpeakersAllHover
                    cursorShape: Qt.PointingHandCursor
                    enabled: detectSpeakersAllButton.enabled
                }
                TapHandler {
                    enabled: detectSpeakersAllButton.enabled
                    onTapped: {
                        const paths = sidebar.bulkTargetPaths(sidebar.transcribedPaths)
                        const urls = paths.map((p) => "file://" + p)
                        sidebar.transcriptionController.diarizeFilesForPaths(urls, sidebar.vaultPath)
                    }
                }
                HoverBorder {
                    anchors.fill: parent
                    active: detectSpeakersAllButton.enabled && detectSpeakersAllHover.hovered
                }
                AppIcon {
                    anchors.centerIn: parent
                    name: "speakers"
                    color: detectSpeakersAllButton.enabled ? "#dddddd" : "#666666"
                    iconSize: 14
                }

                ToolTip.visible: detectSpeakersAllHover.hovered
                ToolTip.text: qsTr("Detect speakers for all transcribed audio")
                ToolTip.delay: 500
            }

            // Items 24/53: bulk "run all jobs" — the full transcribe->label->
            // chapters pipeline for every untranscribed file (same eligibility
            // as transcribeAllButton above: an already-transcribed file's
            // label/chapters are the relabelAll/chaptersAll buttons' job).
            // runAllJobs() itself skips the transcribe step for anything that
            // slips through already-transcribed, so it's safe regardless.
            Item {
                id: runAllButton
                Layout.preferredWidth: 22
                Layout.preferredHeight: 22
                enabled: sidebar.transcriptionController
                         && sidebar.bulkTargetPaths(sidebar.untranscribedPaths).length > 0

                HoverHandler {
                    id: runAllHover
                    cursorShape: Qt.PointingHandCursor
                    enabled: runAllButton.enabled
                }
                TapHandler {
                    enabled: runAllButton.enabled
                    onTapped: {
                        const paths = sidebar.bulkTargetPaths(sidebar.untranscribedPaths)
                        if (paths.length === 0)
                            return
                        const urls = paths.map((p) => "file://" + p)
                        sidebar.transcriptionController.runAllJobsForPaths(urls, sidebar.vaultPath,
                                                                            AppSettings.defaultModel, false,
                                                                            AppSettings.restrictTagsToExisting,
                                                                            AppSettings.tagReuseBias,
                                                                            AppSettings.wordiness,
                                                                            AppSettings.chapterDensity)
                    }
                }
                HoverBorder {
                    anchors.fill: parent
                    active: runAllButton.enabled && runAllHover.hovered
                }
                AppIcon {
                    anchors.centerIn: parent
                    name: "run-all"
                    color: runAllButton.enabled ? "#dddddd" : "#666666"
                    iconSize: 15
                }

                ToolTip.visible: runAllHover.hovered
                ToolTip.text: qsTr("Run all jobs (transcribe, label, chapters)")
                ToolTip.delay: 500
            }

            // Split-recording combine, run on demand rather than
            // automatically: it used to fire on every vault-tree rebuild
            // (open + every filesystem change, via VaultTreeModel's
            // modelReset), silently merging files the moment a recorder
            // dropped in a new part. That surprised users watching a vault
            // fill up mid-recording, so it's now an explicit action here —
            // see TranscriptionController::runCombineScan()'s doc comment.
            Item {
                id: combineButton
                Layout.preferredWidth: 22
                Layout.preferredHeight: 22
                enabled: sidebar.transcriptionController && AppSettings.combineRules.length > 0

                HoverHandler {
                    id: combineHover
                    cursorShape: Qt.PointingHandCursor
                    enabled: combineButton.enabled
                }
                TapHandler {
                    enabled: combineButton.enabled
                    onTapped: {
                        sidebar.transcriptionController.runCombineScan(
                                    AppSettings.combineRules, sidebar.model.allAudioPaths(),
                                    sidebar.vaultPath, AppSettings.deleteOriginalsAfterCombine)
                    }
                }
                HoverBorder {
                    anchors.fill: parent
                    active: combineButton.enabled && combineHover.hovered
                }
                AppIcon {
                    anchors.centerIn: parent
                    name: "combine"
                    color: combineButton.enabled ? "#dddddd" : "#666666"
                    iconSize: 15
                }

                ToolTip.visible: combineHover.hovered
                ToolTip.text: qsTr("Combine split recordings")
                ToolTip.delay: 500
            }

            // File-tree sort mode (Name / Date Modified newest-first /
            // Date Modified oldest-first) — see AppSettings.sortMode's doc
            // comment. Combine-candidate clustering (refreshTreeOrdering())
            // is automatic whenever combine rules exist and composes with
            // whichever of these is active, so there's no separate toggle
            // for it here.
            Item {
                id: sortModeButton
                Layout.preferredWidth: 22
                Layout.preferredHeight: 22

                HoverHandler {
                    id: sortModeHover
                    cursorShape: Qt.PointingHandCursor
                }
                TapHandler {
                    onTapped: sortModeMenu.visible ? sortModeMenu.close() : sortModeMenu.open()
                }
                HoverBorder {
                    anchors.fill: parent
                    active: sortModeHover.hovered
                }
                AppIcon {
                    anchors.centerIn: parent
                    name: "sort"
                    color: "#dddddd"
                    iconSize: 15
                }

                ToolTip.visible: sortModeHover.hovered
                ToolTip.text: qsTr("Sort files")
                ToolTip.delay: 500

                // Same custom-Popup shape as TitleBar.qml's "⋯" overflow
                // menu (moreActionsMenu) — this codebase has no QQC2
                // Menu/MenuItem usage anywhere, just a Popup anchored off its
                // trigger button with plain Item rows inside.
                Popup {
                    id: sortModeMenu
                    x: 0
                    y: sortModeButton.height + 4
                    width: 220
                    padding: 4
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

                    Column {
                        width: sortModeMenu.width - sortModeMenu.padding * 2
                        spacing: 2

                        Repeater {
                            model: [
                                { value: "name", label: qsTr("Name") },
                                { value: "dateDesc", label: qsTr("Date modified (newest first)") },
                                { value: "dateAsc", label: qsTr("Date modified (oldest first)") }
                            ]
                            delegate: Item {
                                id: sortOptionRow
                                required property var modelData
                                width: parent.width
                                height: 28

                                HoverHandler {
                                    id: sortOptionHover
                                    cursorShape: Qt.PointingHandCursor
                                }
                                TapHandler {
                                    onTapped: {
                                        AppSettings.sortMode = sortOptionRow.modelData.value
                                        sortModeMenu.close()
                                    }
                                }
                                Rectangle {
                                    anchors.fill: parent
                                    radius: AppSettings.borderRadius
                                    color: sortOptionHover.hovered ? "#3a3a3a" : "transparent"
                                }
                                Text {
                                    anchors.left: parent.left
                                    anchors.leftMargin: 8
                                    anchors.verticalCenter: parent.verticalCenter
                                    text: (AppSettings.sortMode === sortOptionRow.modelData.value ? "✓ " : "")
                                          + sortOptionRow.modelData.label
                                    color: "white"
                                    font.pixelSize: 12
                                }
                            }
                        }
                    }
                }
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
                AppIcon {
                    anchors.centerIn: parent
                    name: "search"
                    color: "#dddddd"
                    iconSize: 15
                }
            }

            Item {
                Layout.fillWidth: true
            }

            // Item 3: "N selected" indicator, only shown once a genuine
            // multi-selection exists (2+ rows) — a single selected row is
            // already communicated by the existing O7 "currently open"
            // outline in the tree below, so showing "1 selected" here too
            // would be redundant. Same typography as BottomBar.qml's
            // tasksLabel (the closest existing "small status readout next
            // to a toolbar" precedent in this codebase).
            Label {
                id: multiSelectCountLabel
                visible: sidebar.selectedPaths.length > 1
                text: qsTr("%1 selected").arg(sidebar.selectedPaths.length)
                color: "#aaaaaa"
                font.pixelSize: 11
                Layout.alignment: Qt.AlignVCenter
            }

            // Item 44: discoverable "Combine" button next to the "N selected"
            // indicator — the nice-to-have companion to the context-menu entry.
            // Shown only when 2+ AUDIO files are selected (same gate as the
            // context entry); calls the same combineSelectedFiles() path.
            Item {
                id: combineSelectedButton
                visible: sidebar.selectedAudioPaths().length >= 2
                Layout.preferredHeight: 22
                Layout.preferredWidth: combineSelectedButtonText.implicitWidth + 12
                Layout.alignment: Qt.AlignVCenter

                HoverHandler {
                    id: combineSelectedButtonHover
                    cursorShape: Qt.PointingHandCursor
                }
                TapHandler {
                    onTapped: sidebar.combineSelectedFiles()
                }
                HoverBorder {
                    anchors.fill: parent
                    active: combineSelectedButtonHover.hovered
                }
                Text {
                    id: combineSelectedButtonText
                    anchors.centerIn: parent
                    text: qsTr("Combine")
                    color: "#dddddd"
                    font.pixelSize: 11
                }
            }
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
                           ? (includeTranscriptionsHover.hovered ? "#2ecc71" : "#25a25a")
                           : (includeTranscriptionsHover.hovered ? "#3a3a3a" : "#2a2a2a")
                    border.width: 1
                    border.color: sidebar.searchIncludeTranscriptions ? "#2ecc71" : "#4a4a4a"

                    Behavior on color { ColorAnimation { duration: AppSettings.animationsEnabled ? 100 : 0 } }
                }
                Label {
                    id: includeTranscriptionsLabel
                    anchors.centerIn: parent
                    text: qsTr("Transcriptions")
                    color: sidebar.searchIncludeTranscriptions ? "white" : "#aaaaaa"
                    font.pixelSize: 11
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
                        // Item 49: closing search used to leave the tag
                        // filter (a separate, orthogonal feature — see
                        // activeTagFilters' doc comment) active, so rows
                        // stayed hidden even after the search UI itself was
                        // gone. Exiting search now fully clears filtering.
                        tagFilterField.text = ""
                        sidebar.tagFilterText = ""
                    }
                }
                HoverBorder {
                    anchors.fill: parent
                    active: searchCloseHover.hovered
                }
                AppIcon {
                    anchors.centerIn: parent
                    name: "close"
                    color: "#dddddd"
                    iconSize: 15
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
                Layout.fillWidth: true
                Layout.preferredHeight: 24
                placeholderText: qsTr("Filter by tags (comma-separated)…")
                text: sidebar.tagFilterText
                onTextChanged: sidebar.tagFilterText = text

                background: UnderlineField { focused: tagFilterField.activeFocus }

                HoverHandler {
                    cursorShape: Qt.IBeamCursor
                }
            }

            Item {
                id: tagFilterClearButton
                visible: sidebar.tagFilterText !== ""
                Layout.preferredWidth: 20
                Layout.preferredHeight: 20

                HoverHandler {
                    id: tagFilterClearHover
                    cursorShape: Qt.PointingHandCursor
                }
                TapHandler {
                    onTapped: sidebar.tagFilterText = ""
                }
                HoverBorder {
                    anchors.fill: parent
                    active: tagFilterClearHover.hovered
                }
                AppIcon {
                    anchors.centerIn: parent
                    name: "close"
                    color: "#dddddd"
                    iconSize: 15
                }
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
                    border.color: "#4a4a4a"
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
        // SpeakersRole instead of tags. No autocomplete-suggestions popup
        // (unlike tagFilterField's tagSuggestions()) — not requested, and
        // speaker vocabulary isn't vault-wide indexed the way tags are.
        RowLayout {
            width: parent.width
            height: 24
            spacing: 6

            TextField {
                id: speakerFilterField
                Layout.fillWidth: true
                Layout.preferredHeight: 24
                placeholderText: qsTr("Filter by speakers (comma-separated)…")
                text: sidebar.speakerFilterText
                onTextChanged: sidebar.speakerFilterText = text

                background: UnderlineField { focused: speakerFilterField.activeFocus }

                HoverHandler {
                    cursorShape: Qt.IBeamCursor
                }
            }

            Item {
                id: speakerFilterClearButton
                visible: sidebar.speakerFilterText !== ""
                Layout.preferredWidth: 20
                Layout.preferredHeight: 20

                HoverHandler {
                    id: speakerFilterClearHover
                    cursorShape: Qt.PointingHandCursor
                }
                TapHandler {
                    onTapped: sidebar.speakerFilterText = ""
                }
                HoverBorder {
                    anchors.fill: parent
                    active: speakerFilterClearHover.hovered
                }
                AppIcon {
                    anchors.centerIn: parent
                    name: "close"
                    color: "#dddddd"
                    iconSize: 15
                }
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
                    border.color: "#4a4a4a"
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
        // eligible subset of the filtered set instead of bulkTargetPaths(...).
        // Disabled unless a filter is actually active AND that action's
        // eligible subset is non-empty (see searchFilterActive + the per-action
        // helpers above), so they never silently run over the whole vault.
        RowLayout {
            width: parent.width
            height: 24
            spacing: 4

            Label {
                text: qsTr("Run on results:")
                color: "#aaaaaa"
                font.pixelSize: 11
                Layout.alignment: Qt.AlignVCenter
            }

            Item {
                id: searchTranscribeButton
                Layout.preferredWidth: 22
                Layout.preferredHeight: 22
                enabled: sidebar.searchFilterActive && sidebar.searchTranscribePaths().length > 0

                HoverHandler {
                    id: searchTranscribeHover
                    cursorShape: Qt.PointingHandCursor
                    enabled: searchTranscribeButton.enabled
                }
                TapHandler {
                    enabled: searchTranscribeButton.enabled
                    onTapped: {
                        const paths = sidebar.searchTranscribePaths()
                        if (paths.length === 0)
                            return
                        const urls = paths.map((p) => "file://" + p)
                        sidebar.transcriptionController.transcribeAll(urls, sidebar.vaultPath, AppSettings.defaultModel,
                                                                        false, AppSettings.autoLabelEnabled,
                                                                        AppSettings.restrictTagsToExisting,
                                                                        AppSettings.tagReuseBias, AppSettings.wordiness)
                    }
                }
                HoverBorder {
                    anchors.fill: parent
                    active: searchTranscribeButton.enabled && searchTranscribeHover.hovered
                }
                AppIcon {
                    anchors.centerIn: parent
                    name: "transcribe"
                    color: searchTranscribeButton.enabled ? "#dddddd" : "#666666"
                    iconSize: 15
                }

                ToolTip.visible: searchTranscribeHover.hovered
                ToolTip.text: qsTr("Transcribe untranscribed results")
                ToolTip.delay: 500
            }

            Item {
                id: searchRelabelButton
                Layout.preferredWidth: 22
                Layout.preferredHeight: 22
                enabled: sidebar.transcriptionController && sidebar.searchFilterActive
                         && sidebar.searchRelabelPaths().length > 0

                HoverHandler {
                    id: searchRelabelHover
                    cursorShape: Qt.PointingHandCursor
                    enabled: searchRelabelButton.enabled
                }
                TapHandler {
                    enabled: searchRelabelButton.enabled
                    onTapped: {
                        const paths = sidebar.searchRelabelPaths()
                        const urls = paths.map((p) => "file://" + p)
                        sidebar.transcriptionController.relabelAll(urls, sidebar.vaultPath,
                                                                     AppSettings.restrictTagsToExisting,
                                                                     AppSettings.tagReuseBias, AppSettings.wordiness)
                    }
                }
                HoverBorder {
                    anchors.fill: parent
                    active: searchRelabelButton.enabled && searchRelabelHover.hovered
                }
                AppIcon {
                    anchors.centerIn: parent
                    name: "relabel"
                    color: searchRelabelButton.enabled ? "#dddddd" : "#666666"
                    iconSize: 15
                }

                ToolTip.visible: searchRelabelHover.hovered
                ToolTip.text: qsTr("Regenerate auto-titles for results")
                ToolTip.delay: 500
            }

            Item {
                id: searchChaptersButton
                Layout.preferredWidth: 22
                Layout.preferredHeight: 22
                enabled: sidebar.transcriptionController && sidebar.searchFilterActive
                         && sidebar.searchChaptersPaths().length > 0

                HoverHandler {
                    id: searchChaptersHover
                    cursorShape: Qt.PointingHandCursor
                    enabled: searchChaptersButton.enabled
                }
                TapHandler {
                    enabled: searchChaptersButton.enabled
                    onTapped: {
                        const paths = sidebar.searchChaptersPaths()
                        const urls = paths.map((p) => "file://" + p)
                        sidebar.transcriptionController.generateChaptersAll(urls, sidebar.vaultPath,
                                                                              AppSettings.chapterDensity)
                    }
                }
                HoverBorder {
                    anchors.fill: parent
                    active: searchChaptersButton.enabled && searchChaptersHover.hovered
                }
                AppIcon {
                    anchors.centerIn: parent
                    name: "chapters"
                    color: searchChaptersButton.enabled ? "#dddddd" : "#666666"
                    iconSize: 15
                }

                ToolTip.visible: searchChaptersHover.hovered
                ToolTip.text: qsTr("Regenerate chapters for results")
                ToolTip.delay: 500
            }

            // Item 52 (CLAUDE.md per-file-action parity): search-scoped
            // "detect speakers", parity with the ribbon's detectSpeakersAllButton
            // and treeDelegate's rowDetectSpeakersButton — over the transcribed
            // audio in the filtered result set.
            Item {
                id: searchDetectSpeakersButton
                Layout.preferredWidth: 22
                Layout.preferredHeight: 22
                enabled: sidebar.transcriptionController && sidebar.searchFilterActive
                         && sidebar.searchDetectSpeakersPaths().length > 0

                HoverHandler {
                    id: searchDetectSpeakersHover
                    cursorShape: Qt.PointingHandCursor
                    enabled: searchDetectSpeakersButton.enabled
                }
                TapHandler {
                    enabled: searchDetectSpeakersButton.enabled
                    onTapped: {
                        const paths = sidebar.searchDetectSpeakersPaths()
                        const urls = paths.map((p) => "file://" + p)
                        sidebar.transcriptionController.diarizeFilesForPaths(urls, sidebar.vaultPath)
                    }
                }
                HoverBorder {
                    anchors.fill: parent
                    active: searchDetectSpeakersButton.enabled && searchDetectSpeakersHover.hovered
                }
                AppIcon {
                    anchors.centerIn: parent
                    name: "speakers"
                    color: searchDetectSpeakersButton.enabled ? "#dddddd" : "#666666"
                    iconSize: 14
                }

                ToolTip.visible: searchDetectSpeakersHover.hovered
                ToolTip.text: qsTr("Detect speakers for results")
                ToolTip.delay: 500
            }

            // Items 24/53: search-scoped "run all jobs", parity with the
            // ribbon's runAllButton and treeDelegate's rowRunAllButton — over
            // the untranscribed audio in the filtered result set.
            Item {
                id: searchRunAllButton
                Layout.preferredWidth: 22
                Layout.preferredHeight: 22
                enabled: sidebar.transcriptionController && sidebar.searchFilterActive
                         && sidebar.searchRunAllPaths().length > 0

                HoverHandler {
                    id: searchRunAllHover
                    cursorShape: Qt.PointingHandCursor
                    enabled: searchRunAllButton.enabled
                }
                TapHandler {
                    enabled: searchRunAllButton.enabled
                    onTapped: {
                        const paths = sidebar.searchRunAllPaths()
                        if (paths.length === 0)
                            return
                        const urls = paths.map((p) => "file://" + p)
                        sidebar.transcriptionController.runAllJobsForPaths(urls, sidebar.vaultPath,
                                                                            AppSettings.defaultModel, false,
                                                                            AppSettings.restrictTagsToExisting,
                                                                            AppSettings.tagReuseBias,
                                                                            AppSettings.wordiness,
                                                                            AppSettings.chapterDensity)
                    }
                }
                HoverBorder {
                    anchors.fill: parent
                    active: searchRunAllButton.enabled && searchRunAllHover.hovered
                }
                AppIcon {
                    anchors.centerIn: parent
                    name: "run-all"
                    color: searchRunAllButton.enabled ? "#dddddd" : "#666666"
                    iconSize: 15
                }

                ToolTip.visible: searchRunAllHover.hovered
                ToolTip.text: qsTr("Run all jobs for results")
                ToolTip.delay: 500
            }

            Item { Layout.fillWidth: true }
        }
        }

        // Item 17: autofill suggestions for the tag currently being typed
        // (the fragment after the last comma) — vault tag vocabulary
        // filtered to those starting with that fragment, excluding tags
        // already present in the field.
        Popup {
            id: tagSuggestionsPopup
            y: ribbon.height
            x: 6
            width: 220
            padding: 4
            visible: sidebar.searchExpanded && tagFilterField.activeFocus && tagSuggestionsList.model.length > 0
            focus: false
            modal: true
            closePolicy: Popup.NoAutoClose

            background: Rectangle {
                color: "#242424"
                radius: AppSettings.borderRadius
                border.width: 1
                border.color: "#3a3a3a"
            }

            ListView {
                id: tagSuggestionsList
                implicitHeight: Math.min(contentHeight, 140)
                width: parent.width
                model: sidebar.tagSuggestions()

                delegate: Item {
                    required property string modelData
                    width: tagSuggestionsList.width
                    height: 22

                    HoverHandler {
                        id: suggestionHover
                        cursorShape: Qt.PointingHandCursor
                    }
                    TapHandler {
                        onTapped: sidebar.acceptTagSuggestion(modelData)
                    }
                    Rectangle {
                        anchors.fill: parent
                        color: suggestionHover.hovered ? "#3a3a3a" : "transparent"
                        radius: AppSettings.borderRadius
                    }
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.left: parent.left
                        anchors.leftMargin: 6
                        text: parent.modelData
                        color: "#dddddd"
                        font.pixelSize: 11
                    }
                }
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
            color: "#242424"
            radius: AppSettings.borderRadius
            border.width: 1
            // Doubles as the "will this drop do anything" readout, so the
            // answer travels with the cursor instead of only living in the
            // (possibly far-away) highlighted target row.
            border.color: sidebar.canDropRowsOn(sidebar.rowDragDest)
                ? AppSettings.highlightColor : "#3a3a3a"
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

    // Failure feedback for a row action VaultTreeModel refuses — a move (most
    // likely a same-named file already in the destination), an inline rename
    // (name taken / invalid), or a delete. Same transient red banner as
    // Main.qml's drag-and-drop import errors, which these gestures are the
    // in-vault siblings of; a SUCCESSFUL action needs no banner, the row simply
    // reappears renamed / in its new folder / gone.
    Label {
        z: 101
        anchors.top: parent.top
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.topMargin: 12
        visible: sidebar.rowActionError !== ""
        text: sidebar.rowActionError
        color: "white"
        padding: 8
        background: Rectangle {
            color: "#c0392b"
            radius: AppSettings.borderRadius
        }
    }

    TreeView {
        id: tree
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
        // can't have. cellAtPosition() wants contentItem coordinates (the view
        // scrolls under them), while a TapHandler's point.position is relative
        // to its parent item — hence the map.
        TapHandler {
            id: treeEmptyContextTap
            acceptedButtons: Qt.RightButton
            onTapped: {
                const local = treeEmptyContextTap.point.position
                const inContent = tree.mapToItem(tree.contentItem, local.x, local.y)
                const cell = tree.cellAtPosition(inContent.x, inContent.y, true)
                if (cell.y >= 0)
                    return // a row — its own treeRowContextTap owns this click
                const p = tree.mapToItem(sidebar, local.x, local.y)
                sidebar.openContextMenu("", false, false, false, 0,
                                         sidebar.model ? sidebar.model.vaultLocalPath : "", "", p.x, p.y)
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
                tree.expandPending = true
                expandRetryTimer.start()
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

        delegate: Item {
            id: treeDelegate
            // Column width (and so this delegate's own width) comes from
            // tree.columnWidthProvider above, not implicitWidth here — see
            // that property's doc comment for why (the Basic style's
            // vertical ScrollBar is an OVERLAY needing a dedicated lane, and
            // AppSettings.sidebarRightMargin is the ONE setting for that
            // whole right-side gap, live-reactive via columnWidthProvider).
            // Item 2 (Wave E): snippetText now wraps instead of truncating,
            // so its height is variable — grow to fit instead of a fixed 44.
            implicitHeight: treeDelegate.showSnippet ? (28 + snippetText.implicitHeight + 4) : 28

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
            // Proxy-provided (VaultSearchProxyModel): true for a direct match
            // or, for a directory, any matching descendant; always true with
            // no active query. Single source of truth for both the sort
            // order (proxy's lessThan()) and this delegate's opacity.
            required property bool matchesSearch
            // Proxy-provided passthrough of VaultTreeModel::TranscriptBodyRole
            // (empty unless this is a TranscribedStatus audio row) — used to
            // build the search-result snippet below.
            required property string transcriptBody

            // "transcribe all" (bulk) skips files whose stored settings
            // already match what bulk would use (untranscribed OR the
            // stored model/speakerDetection differs) — but this does NOT
            // gate the per-row manual Transcribe icon below, which is always
            // available so a single file can always be explicitly re-run.
            // Bulk always requests AppSettings.defaultModel with
            // speakerDetection=false (see transcribeAllButton's TapHandler).
            readonly property bool bulkEligible: treeDelegate.isAudio && sidebar.transcriptionController
                && !sidebar.transcriptionController.matchesExistingTranscription(
                       "file://" + treeDelegate.path, AppSettings.defaultModel, false)
            onBulkEligibleChanged: sidebar.setBulkEligible(treeDelegate.path, treeDelegate.bulkEligible)

            // Wave D item 7: inverse of bulkEligible — already-transcribed
            // audio files, for the per-row + bulk "regenerate auto-title".
            readonly property bool relabelEligible: treeDelegate.isAudio && treeDelegate.status === 2
            onRelabelEligibleChanged: sidebar.setRelabelEligible(treeDelegate.path, treeDelegate.relabelEligible)

            // Chapters, unlike relabel, only ever read the .md's existing
            // transcript body (TranscriptionController::generateChaptersJob()
            // never touches audio) — so an orphaned .md is just as eligible
            // as an already-transcribed audio row.
            readonly property bool chaptersEligible: treeDelegate.relabelEligible || treeDelegate.isOrphanedMd
            onChaptersEligibleChanged: sidebar.setChaptersEligible(treeDelegate.path, treeDelegate.chaptersEligible)

            // A folder highlight covers the folder AND everything inside it,
            // so what's about to be acted on reads as one whole container
            // rather than a single lit row whose contents are left ambiguous.
            // This is the root of the run this row belongs to, or "".
            //
            // A drag's destination folder wins over a plain mouse hover: mid-
            // drag the pointer is necessarily over SOME row, and the drop is
            // the thing the user is deciding about (the sibling
            // rowHoverBorder suppresses itself mid-drag for the same reason).
            //
            // The vault ROOT is deliberately excluded — every row is "under"
            // it, so it would light the entire tree; a drop targeting the root
            // is marked by rootDropTarget's own border around the whole view
            // instead.
            readonly property string folderHighlightRoot: {
                const candidate = sidebar.rowDragActive ? sidebar.rowDragDest : sidebar.hoveredFolderPath
                const vaultRoot = sidebar.model ? sidebar.model.vaultLocalPath : ""
                return (candidate === "" || candidate === vaultRoot) ? "" : candidate
            }
            readonly property bool inFolderHighlight:
                    sidebar.isUnderFolder(treeDelegate.path, treeDelegate.folderHighlightRoot)

            // The run's shape at this row. A folder's descendants are
            // contiguous with it in display order (the tree IS the directory
            // structure, and children render directly beneath their parent),
            // so only two facts are needed: every row in the run except the
            // folder itself has an in-run row directly above it, and a row's
            // bottom is an internal seam iff the NEXT display row is in the
            // run too. Both feed CombineGroupOutline's per-edge suppression
            // below, so the whole subtree draws as ONE perimeter rather than a
            // stack of separate boxes.
            readonly property bool folderHighlightTopMerged: treeDelegate.inFolderHighlight
                    && treeDelegate.path !== treeDelegate.folderHighlightRoot
            readonly property bool folderHighlightBottomMerged: treeDelegate.inFolderHighlight
                    && sidebar.isUnderFolder(sidebar.pathAtTreeRow(treeDelegate.row + 1, tree.rows),
                                              treeDelegate.folderHighlightRoot)

            // Nesting-guide inputs (see the guide Rectangles below): the next
            // display row's depth decides whether this row is the LAST row of
            // its parent's expanded block (nextRowDepth < depth — anything
            // still inside the block, including this row's own children, is
            // deeper). -1 out of range, so the tree's final row counts as a
            // block end too.
            readonly property int nextRowDepth: sidebar.depthAtTreeRow(treeDelegate.row + 1, tree.rows)
            // Dim on purpose — the guides are containment texture behind the
            // rows' real content, not content themselves (the expand glyphs'
            // #aaaaaa at ~20% alpha).
            readonly property color guideColor: "#33aaaaaa"

            // Item 41: the per-row action cluster is hover-only, EXCEPT while
            // the keyboard is actually inside this row's cluster (currentPath
            // + a focused button, which only Left/Right can establish) — then
            // it stays revealed without a mouse hover so the focused button
            // has a visible target. Merely selecting/opening a row does NOT
            // reveal its icons: they'd then sit permanently on the open file
            // (user report), so entering the cluster is an explicit Right
            // press. tree.activeFocus mirrors the nav Shortcuts' own gating,
            // so focus leaving the tree re-hides them rather than stranding
            // the icons on a row the arrows no longer drive.
            // (rowHover is declared further down the delegate; QML resolves id
            // references regardless of textual order.)
            readonly property bool rowActionsRevealed: rowHover.hovered
                    || (tree.activeFocus && treeDelegate.path === sidebar.currentPath
                        && sidebar.actionButtonFocus !== "")

            // Item 51: true iff this row is a member of a "ready" (would-
            // combine-if-run) split-recording group per
            // sidebar.combineReadyPaths — a genuinely reactive binding (both
            // combineReadyPaths and path have real NOTIFY signals), unlike
            // jobStatus above which needs the explicit refresh idiom because
            // jobStatusForPath() itself has none. Drives the row highlight
            // and groupMergeButton below.
            readonly property bool combineEligible: treeDelegate.isAudio
                    && sidebar.combineReadyPaths[treeDelegate.path] !== undefined
            readonly property string combineOutputPath: treeDelegate.combineEligible
                    ? sidebar.combineReadyPaths[treeDelegate.path] : ""
            readonly property color combineGroupColor: treeDelegate.combineEligible
                    ? sidebar.combineGroupColor(treeDelegate.combineOutputPath) : "transparent"
            // 1-based position in the group's combine order (group.files
            // order from findCombineGroups() — the order files are actually
            // concatenated in, not necessarily this tree's display order).
            // Drives combineOrderBadge below.
            readonly property int combineOrder: treeDelegate.combineEligible
                    ? (sidebar.combineOrderPaths[treeDelegate.path] || 0) : 0

            // Merges this row's outline with a vertically-adjacent row's
            // outline when they're in the SAME combine group, so a
            // contiguous run of group members reads as one unified outline
            // (only the outer perimeter drawn) instead of N stacked boxes —
            // see CombineGroupOutline below. Plain reactive bindings against
            // sidebar.combineTopMergedPaths/combineBottomMergedPaths (see
            // refreshTreeOrdering()'s doc comment for why those are computed
            // from the tree's actual display order, not from which rows
            // TreeView happens to have realized). A group's members are
            // always siblings in one folder (findCombineGroups() keys group
            // identity on the containing folder too, so a cross-folder group
            // can't exist), and the sortKeys triple gives cluster identity
            // its own tier — so members are contiguous both in that display
            // order and on screen, and a group draws as one perimeter.
            readonly property bool combineTopMerged: treeDelegate.combineEligible
                    && sidebar.combineTopMergedPaths[treeDelegate.path] === true
            readonly property bool combineBottomMerged: treeDelegate.combineEligible
                    && sidebar.combineBottomMergedPaths[treeDelegate.path] === true

            // P6: JobStatus int for this row's file (None/Queued/Active/
            // Paused/Done/Failed, see TranscriptionController::JobStatus) —
            // unlike bulkEligible/relabelEligible above this can't be a
            // readonly binding (jobStatusForPath() is a plain Q_INVOKABLE
            // call, not a bound property), so it's explicitly refreshed —
            // same push idiom as refreshMetadata() below — on completion and
            // whenever sidebar.jobStatusRevision bumps.
            property int jobStatus: -1
            function refreshJobStatus() {
                treeDelegate.jobStatus = (treeDelegate.isAudio && sidebar.transcriptionController)
                    ? sidebar.transcriptionController.jobStatusForPath(treeDelegate.path) : -1
            }
            onJobStatusChanged: sidebar.setJobStatus(treeDelegate.path, treeDelegate.jobStatus)
            Connections {
                target: sidebar
                function onJobStatusRevisionChanged() { treeDelegate.refreshJobStatus() }
            }

            // Bug: TreeView recycles a fixed pool of delegate Items as rows
            // scroll in/out of view (reuseItems defaults to true) — required
            // properties like path/isAudio/isOrphanedMd get reassigned to the
            // recycled Item for its new row, but metadata/jobStatus above are
            // plain imperative properties that were only ever pushed once, at
            // Component.onCompleted (i.e. whichever row this Item instance
            // FIRST rendered). Every recycled reuse of that same instance kept
            // showing that original row's label/jobStatus forever after,
            // regardless of which file it now represents — this is why one
            // label appeared to "repeat" at a fixed interval (the pool size)
            // throughout the whole tree, not just in search. Refresh on every
            // reassignment, not just first creation.
            onPathChanged: {
                treeDelegate.refreshMetadata()
                treeDelegate.refreshJobStatus()
            }

            Component.onCompleted: {
                sidebar.setBulkEligible(treeDelegate.path, treeDelegate.bulkEligible)
                sidebar.setRelabelEligible(treeDelegate.path, treeDelegate.relabelEligible)
                sidebar.setChaptersEligible(treeDelegate.path, treeDelegate.chaptersEligible)
                treeDelegate.refreshMetadata()
                treeDelegate.refreshJobStatus()
            }
            Component.onDestruction: {
                sidebar.setBulkEligible(treeDelegate.path, false)
                sidebar.setRelabelEligible(treeDelegate.path, false)
                sidebar.setChaptersEligible(treeDelegate.path, false)
                sidebar.setJobStatus(treeDelegate.path, -1)
                // Balance the hover count if this row is destroyed (e.g.
                // TreeView recycling) WHILE still counted as hovering —
                // otherwise that increment would never be decremented and
                // the group's button could get stuck visible forever.
                if (treeDelegate.activeHoverGroupPath !== "") {
                    sidebar.leaveCombineGroupHover(treeDelegate.activeHoverGroupPath)
                    treeDelegate.activeHoverGroupPath = ""
                }
            }

            // O9: was a pure readonly binding (isAudio/isOrphanedMd/
            // transcriptionController only) — none of those change on
            // relabel/transcribe completion, so it froze at delegate
            // creation and only "caught up" whenever an unrelated vault
            // event forced VaultTreeModel::rebuild() (recreating every
            // delegate from scratch). A dummy-property read inside this
            // binding to force re-evaluation on completion would risk being
            // silently dropped by AOT (qmlcachegen) if the read result is
            // otherwise unused — see a prior bug in this exact codebase.
            // Instead this is now a plain property, explicitly (re)assigned
            // by refreshMetadata() — a genuine push, not a binding trick.
            // readMetadata() resolves the sidecar .md regardless of whether
            // the given path is the audio file or the .md itself
            // (sidecarPath() is self-referential when the path already ends
            // in .md), so this covers both audio and orphaned rows.
            property var metadata: ({})
            function refreshMetadata() {
                treeDelegate.metadata = ((treeDelegate.isAudio || treeDelegate.isOrphanedMd)
                                          && sidebar.transcriptionController)
                    ? sidebar.transcriptionController.readMetadata("file://" + treeDelegate.path) : ({})
            }
            Connections {
                target: sidebar
                function onMetadataRevisionChanged() { treeDelegate.refreshMetadata() }
            }

            // Item 9: ellipsis-bounded transcript snippet around the first
            // match, shown only when there's an active query, transcript
            // search is enabled, and this row's body actually contains it —
            // rows that only matched by name/label/tags show no snippet.
            readonly property string cleanedBody: (sidebar.searchIncludeTranscriptions && treeDelegate.transcriptBody)
                ? sidebar.cleanTranscript(treeDelegate.transcriptBody) : ""
            readonly property string snippetText: (sidebar.searchQuery !== "" && treeDelegate.cleanedBody !== "")
                ? sidebar.transcriptSnippet(treeDelegate.cleanedBody, sidebar.searchQuery, 40) : ""
            readonly property bool showSnippet: treeDelegate.snippetText !== ""
            // Item 8 fix: showSnippet can also flip because transcriptBody
            // changed independently of sidebar.searchQuery/
            // searchIncludeTranscriptions (e.g. a transcript streaming in
            // live while search is already active) — neither of tree's two
            // Connections handlers above fire for that, leaving this row's
            // cached TableView Y stale. Trigger the same relayout directly.
            onShowSnippetChanged: tree.relayoutForSearchStateChange()

            opacity: treeDelegate.matchesSearch ? 1 : 0.3

            // The specific group path THIS row is currently counted as
            // hovering, per sidebar.enterCombineGroupHover()/
            // leaveCombineGroupHover() — captured at enter time and reused
            // at leave time rather than re-reading treeDelegate.
            // combineOutputPath, which can change out from under a hovered
            // row if TreeView recycles this delegate Item to a different
            // file mid-hover (see onPathChanged's own doc comment for why
            // that recycling happens at all).
            property string activeHoverGroupPath: ""

            HoverHandler {
                id: rowHover
                cursorShape: Qt.PointingHandCursor
                // Item 51 refinement: pushes this row's group into
                // sidebar.hoveredCombineGroup so the ONE per-group merge
                // button (see groupMergeButton) reveals on hovering ANY
                // member row, not just this exact one. Reference-counted
                // (see sidebar.hoveredCombineGroupCounts' doc comment) so
                // moving the pointer directly from one member row to an
                // adjacent one doesn't flicker the button off.
                onHoveredChanged: {
                    if (rowHover.hovered && treeDelegate.combineEligible) {
                        treeDelegate.activeHoverGroupPath = treeDelegate.combineOutputPath
                        sidebar.enterCombineGroupHover(treeDelegate.activeHoverGroupPath)
                    } else if (!rowHover.hovered && treeDelegate.activeHoverGroupPath !== "") {
                        sidebar.leaveCombineGroupHover(treeDelegate.activeHoverGroupPath)
                        treeDelegate.activeHoverGroupPath = ""
                    }
                    // Hovering a folder lights the folder AND its contents —
                    // publish which folder that is, since the descendant rows
                    // lighting up can't see this row's hover state themselves
                    // (see sidebar.hoveredFolderPath).
                    if (treeDelegate.isDir) {
                        if (rowHover.hovered)
                            sidebar.enterFolderHover(treeDelegate.path)
                        else
                            sidebar.leaveFolderHover(treeDelegate.path)
                    }
                }
            }

            // Item 40: right-click (or press-and-hold) opens the folder-
            // management context menu for this row — separate TapHandler so
            // it doesn't disturb the left-click open/expand handler below.
            TapHandler {
                id: treeRowContextTap
                acceptedButtons: Qt.RightButton
                onTapped: {
                    const p = treeDelegate.mapToItem(sidebar, treeRowContextTap.point.position.x,
                                                       treeRowContextTap.point.position.y)
                    // "New folder" target: a directory row creates inside
                    // itself; a note row creates inside its own parent dir.
                    const createIn = treeDelegate.isDir
                        ? treeDelegate.path
                        : treeDelegate.path.substring(0, treeDelegate.path.lastIndexOf("/"))
                    sidebar.openContextMenu(treeDelegate.path, treeDelegate.isDir,
                                             treeDelegate.isAudio || treeDelegate.isOrphanedMd,
                                             treeDelegate.isAudio, treeDelegate.status,
                                             createIn,
                                             treeDelegate.metadata ? treeDelegate.metadata.label : "",
                                             p.x, p.y)
                }
            }

            // A plain click on the NAME of an already-solely-selected note
            // starts an inline rename (file-manager convention) — the second
            // way in, alongside the context menu's "Rename".
            //
            // Decided inside treeRowTap rather than by a TapHandler of its own
            // on rowNameText: overlapping TapHandlers aren't mutually exclusive
            // in Qt Quick, so a tap on the name would fire BOTH and race
            // rename against handleNoteClick (StyleGuide.md "Always-editable
            // text" documents that exact bug). One handler, one decision.
            //
            // Bounded by the name's PAINTED width (implicitWidth), not the
            // Text's laid-out width — it Layout.fillWidths the whole row, so
            // the plain width would make empty space far right of a short name
            // count as "on the name".
            function tapIsOnRowName(pos) {
                if (!rowNameText.visible)
                    return false
                const local = rowNameText.mapFromItem(treeDelegate, pos.x, pos.y)
                const textWidth = Math.min(rowNameText.width, rowNameText.implicitWidth)
                return local.x >= 0 && local.x <= textWidth
                        && local.y >= 0 && local.y <= rowNameText.height
            }

            TapHandler {
                id: treeRowTap
                onTapped: {
                    // Clicking a DIFFERENT row while an inline rename is open
                    // must commit it (blur convention) — and nothing else will,
                    // because this click never actually blurs the editor: the
                    // TreeView is a FocusScope, so the forceActiveFocus() below
                    // hands activeFocus straight back to its focused child (the
                    // editor) rather than taking it away. The editor's own
                    // blur-commit therefore never fires, and without this the
                    // edit is silently dropped while the editor sits open on a
                    // row that is no longer even selected.
                    if (sidebar.renamingPath !== "" && sidebar.renamingPath !== treeDelegate.path)
                        sidebar.commitActiveRename()
                    // Grabs keyboard focus so Up/Down (see the Shortcut
                    // items near Ctrl+F's) can move between notes right
                    // after a click — never touches sidebar.currentPath, so
                    // the persistent "currently open" outline (O7) is
                    // unaffected; clicking into MainPanel afterward moves
                    // activeFocus away exactly as it always has.
                    treeDelegate.treeView.forceActiveFocus()
                    if (treeDelegate.isDir) {
                        // An empty folder must NOT go through toggleExpanded():
                        // TreeView won't expand a childless node, but the
                        // attempt still latches treeDelegate.expanded to true
                        // permanently. See sidebar.collapsedEmptyDirs.
                        if (treeDelegate.hasChildren)
                            treeDelegate.treeView.toggleExpanded(treeDelegate.row)
                        else
                            sidebar.toggleEmptyDirCollapsed(treeDelegate.path)
                    } else if (treeDelegate.isAudio || treeDelegate.isOrphanedMd) {
                        // Click the name of a row that's ALREADY the sole
                        // selection -> rename it, rather than re-opening the
                        // file it already opened on the first click. Requires
                        // no modifiers and a selection of exactly this row, so
                        // it can never fire while a Shift/Ctrl multi-selection
                        // is being built.
                        const plain = (treeRowTap.point.modifiers
                                       & (Qt.ShiftModifier | Qt.ControlModifier)) === 0
                        const soleSelection = sidebar.selectedPaths.length === 1
                                && sidebar.selectedPaths[0] === treeDelegate.path
                        if (plain && soleSelection
                                && treeDelegate.tapIsOnRowName(treeRowTap.point.position)) {
                            sidebar.beginRename(treeDelegate.path, false,
                                                treeDelegate.metadata ? treeDelegate.metadata.label : "")
                            return
                        }
                        // Item 3: Shift/Ctrl-click multi-select — see
                        // sidebar.handleNoteClick's doc comment. modifiers
                        // comes from this TapHandler's own point (Qt
                        // doesn't pass them as an onTapped argument).
                        sidebar.handleNoteClick(treeDelegate.path, treeDelegate.isOrphanedMd,
                                                 treeRowTap.point.modifiers)
                    }
                }
            }

            // Item 40 (drag-to-move): press-and-drag this row to move it into a
            // folder. All the state (including WHICH paths are moving) lives on
            // the sidebar — see sidebar.beginRowDrag's doc comment.
            DragHandler {
                id: rowDragHandler
                // The row itself never moves — rowDragGhost follows the cursor
                // instead, so the tree's own layout stays untouched and there's
                // no torn-out row to put back if the drag is abandoned.
                target: null
                acceptedButtons: Qt.LeftButton
                enabled: treeDelegate.isDir || treeDelegate.isAudio || treeDelegate.isOrphanedMd
                // grabPermissions is deliberately left at the DEFAULT. The
                // obvious worry is that the enclosing TreeView (a Flickable)
                // steals the grab once the stroke passes its own flick
                // threshold, turning the drag back into the "merely drags the
                // whole thing" scroll this replaces — but that doesn't happen:
                // this handler's exclusive grab alone is enough, verified in
                // tst_sidebar_dragmove's dragDoesNotFlickTheTree (which DOES
                // reproduce the flick when this handler is disabled, so it's
                // genuinely testing for it). Tightening this to
                // CanTakeOverFromAnything would fix nothing and cost something:
                // it drops ApprovesCancellation, and this drag depends on being
                // cancellable when the delegate is destroyed under it (see
                // onActiveChanged below).
                //
                // Holding the grab for the whole stroke also keeps the source
                // delegate alive: TreeView only recycles delegates that leave
                // the viewport, and the tree can't scroll while this handler
                // has the pointer. (Plain clicks are unaffected — treeRowTap's
                // default DragThreshold gesturePolicy gives ITS grab up once
                // the pointer moves, so click-to-open still wins for a click.)
                onActiveChanged: {
                    if (rowDragHandler.active) {
                        sidebar.beginRowDrag(treeDelegate.path, treeDelegate.isDir,
                                             rowDragHandler.centroid.scenePosition)
                        return
                    }
                    // Deactivating means either the button came up (drop it) or
                    // the grab was taken/cancelled — e.g. this delegate being
                    // destroyed under an in-flight drag by a VaultTreeModel
                    // rebuild. A real release has already cleared the button
                    // from the centroid; a cancellation still shows it held, and
                    // must abandon the drag rather than silently move files the
                    // user never dropped.
                    sidebar.endRowDrag(rowDragHandler.centroid.pressedButtons === Qt.NoButton)
                }
                onCentroidChanged: {
                    if (rowDragHandler.active)
                        sidebar.updateRowDrag(rowDragHandler.centroid.scenePosition)
                }
            }

            // Item 40 (drag-to-move): every row is a drop target. A directory
            // row takes the drop itself; a note row forwards it to the note's
            // own parent directory, so dropping onto a file means "into the
            // folder that file lives in" (standard file-manager behavior)
            // rather than being a dead zone the user has to aim around.
            DropArea {
                id: rowDropTarget
                anchors.fill: parent
                // Matches rowDragGhost's Drag.keys — scopes this target to the
                // sidebar's own row drags, so Main.qml's whole-window
                // audio-file import DropArea and this one can never both claim
                // the same drop.
                keys: ["suopVaultRow"]
                readonly property string destDir: treeDelegate.isDir
                    ? treeDelegate.path
                    : treeDelegate.path.substring(0, treeDelegate.path.lastIndexOf("/"))
                onEntered: (drag) => {
                    // Refusing here (rather than taking the drop and letting
                    // the C++ guard fail it) is what makes an impossible target
                    // stay dark and drop nothing: a rejected DropArea gets no
                    // onDropped.
                    if (!sidebar.canDropRowsOn(rowDropTarget.destDir)) {
                        drag.accepted = false
                        return
                    }
                    sidebar.rowDragDest = rowDropTarget.destDir
                }
                onExited: {
                    // Guarded because sibling rows in the same folder share a
                    // destDir: Qt delivers the old target's exit before the new
                    // target's enter, so an unguarded clear here would blank the
                    // destination the row we just moved onto had already set.
                    if (sidebar.rowDragDest === rowDropTarget.destDir)
                        sidebar.rowDragDest = ""
                }
                onDropped: {
                    const paths = sidebar.rowDragPaths.slice()
                    const isDir = sidebar.rowDragIsDir
                    const dest = rowDropTarget.destDir
                    // Deferred — see performRowMove's doc comment (the move
                    // rebuilds the model, destroying this very delegate).
                    Qt.callLater(() => sidebar.performRowMove(paths, isDir, dest))
                }
            }

            // Item 40 (drag-to-move): the fill for the folder a drop would land
            // in — covering the destination folder AND every row inside it (see
            // treeDelegate.inFolderHighlight), so the drop's whole landing zone
            // is what lights up. Keyed off sidebar.rowDragDest rather than this
            // row's own rowDropTarget.containsDrag so the highlight always
            // marks the destination FOLDER's region: dragging over a note
            // inside "Interviews" lights the whole "Interviews" subtree, not
            // the single note under the cursor (dropping ONTO a file isn't a
            // containment the vault has any concept of).
            Rectangle {
                objectName: "rowDropHighlight"
                anchors.fill: parent
                // An internal seam (a boundary shared with the next/previous
                // row of the same run) collapses its inset to 0 so the fill is
                // continuous down the run, instead of showing a 2px gutter
                // across every row boundary. The run's real outer edges keep
                // the 1px inset the rest of the row's outlines use.
                anchors.topMargin: treeDelegate.folderHighlightTopMerged ? 0 : 1
                anchors.bottomMargin: treeDelegate.folderHighlightBottomMerged ? 0 : 1
                anchors.leftMargin: AppSettings.sidebarEdgeLeftMargin
                visible: sidebar.rowDragActive && treeDelegate.inFolderHighlight
                color: AppSettings.sidebarSelectionColor
                // Per-corner, so only the run's genuine outer corners round and
                // the seams stay square. No border property here: a Rectangle's
                // border draws all four sides, which would put a line straight
                // across every internal seam — the run's perimeter is drawn as
                // one shape by the CombineGroupOutline below instead.
                topLeftRadius: treeDelegate.folderHighlightTopMerged ? 0 : AppSettings.borderRadius
                topRightRadius: treeDelegate.folderHighlightTopMerged ? 0 : AppSettings.borderRadius
                bottomLeftRadius: treeDelegate.folderHighlightBottomMerged ? 0 : AppSettings.borderRadius
                bottomRightRadius: treeDelegate.folderHighlightBottomMerged ? 0 : AppSettings.borderRadius
            }

            // The perimeter around that same folder run — one unbroken outline
            // around the folder + its contents, drawn by the same component the
            // combine-group outline uses (its whole purpose is suppressing the
            // seams between vertically adjacent rows of one run; a plain
            // Rectangle border can't, see CombineGroupOutline.qml).
            //
            // Covers BOTH folder-highlight sources, since folderHighlightRoot
            // already resolves drag-destination vs plain hover — only the color
            // differs: the drag destination reuses the drop fill's highlight
            // color, while a plain hover matches what a single row's own
            // HoverBorder would have drawn (that HoverBorder suppresses itself
            // for rows in this run — see its `active` below — so the two never
            // double up).
            CombineGroupOutline {
                objectName: "folderHighlightOutline"
                anchors.fill: parent
                anchors.leftMargin: AppSettings.sidebarEdgeLeftMargin
                anchors.topMargin: 1
                anchors.bottomMargin: 1
                // Must match anchors.topMargin/bottomMargin above — see this
                // property's doc comment in CombineGroupOutline.qml.
                edgeMargin: 1
                visible: treeDelegate.inFolderHighlight
                strokeColor: sidebar.rowDragActive ? AppSettings.highlightColor : AppSettings.accentColor
                // Matches the sibling combine outline's weight, and for the
                // same reason — see its own strokeWidth comment below.
                strokeWidth: 1.5
                cornerRadius: AppSettings.borderRadius
                topRounded: !treeDelegate.folderHighlightTopMerged
                bottomRounded: !treeDelegate.folderHighlightBottomMerged
            }

            // Note: no anchors.rightMargin here — tree.columnWidthProvider
            // already subtracts the FULL AppSettings.sidebarRightMargin from
            // this row's own width, so this Item's right edge already sits
            // at that inset; adding it again here would double it.
            //
            // Suppressed for the duration of a row drag: an outline tracking the
            // pointer from row to row while a ghost is in flight reads as "drop
            // this file ONTO that one" — a containment the vault has no concept
            // of. Mid-drag, the drop highlight above is the only row feedback,
            // so the one thing lit is the folder the drop actually lands in.
            //
            // Also suppressed for any row inside a folder highlight run: that
            // run's own perimeter (folderHighlightOutline above) already
            // outlines this row as part of the folder, and hovering a FOLDER
            // would otherwise draw this single-row box on top of the run's
            // outline — two outlines over the same row.
            HoverBorder {
                objectName: "rowHoverBorder"
                anchors.fill: parent
                anchors.topMargin: 1
                anchors.bottomMargin: 1
                anchors.leftMargin: AppSettings.sidebarEdgeLeftMargin
                active: rowHover.hovered && !sidebar.rowDragActive
                        && !treeDelegate.inFolderHighlight
            }

            // O7: persistent highlight for whatever MainPanel currently has
            // open, distinct from HoverBorder above (which is hover-only and
            // white) so both can be visible at once without being confused.
            Rectangle {
                anchors.fill: parent
                anchors.topMargin: 1
                anchors.bottomMargin: 1
                anchors.leftMargin: AppSettings.sidebarEdgeLeftMargin
                visible: treeDelegate.path === sidebar.currentPath
                color: AppSettings.sidebarSelectionColor
                radius: AppSettings.borderRadius
                border.width: 1
                border.color: AppSettings.sidebarSelectionBorderColor
            }

            // Item 3: multi-select highlight — border-only (not a fill, so
            // it composes with the O7 "currently open" rectangle above
            // instead of competing with it) and only shown once a genuine
            // multi-selection exists (2+ rows), since a lone selected row is
            // already the same row O7's highlight marks as open. Reuses
            // AppSettings.highlightColor (the same color already used for
            // search-match spans) rather than adding a new configurable
            // color for what's a secondary affordance.
            Rectangle {
                anchors.fill: parent
                anchors.topMargin: 1
                anchors.bottomMargin: 1
                anchors.leftMargin: AppSettings.sidebarEdgeLeftMargin
                visible: sidebar.selectedPaths.length > 1 && sidebar.selectedPaths.indexOf(treeDelegate.path) >= 0
                color: "transparent"
                radius: AppSettings.borderRadius
                border.width: 1
                border.color: AppSettings.highlightColor
            }

            // Item 51: border-only (not a fill, so it composes with the
            // selection highlight above instead of covering it) highlight for
            // rows that are members of a "ready" — would-combine-if-run —
            // split-recording group, colored per-group (combineGroupColor),
            // same stroke weight and corner radius as HoverBorder. A
            // contiguous run of same-group rows suppresses the edges they
            // SHARE with a same-group neighbor (combineTopMerged/
            // combineBottomMerged — see CombineGroupOutline's doc comment for
            // why a plain bordered Rectangle can't do this) so the run reads
            // as one continuous rounded outline instead of N stacked boxes.
            CombineGroupOutline {
                anchors.fill: parent
                // Horizontal inset matches HoverBorder's (breathing room from
                // the sidebar's own left/right edges), but the vertical
                // inset is much smaller — unlike HoverBorder (one row at a
                // time), this draws the OUTER top/bottom edge of a whole
                // multi-row merged group, and a 3px inset there read as
                // unwanted padding separating the group from the unrelated
                // rows directly above/below it (user report).
                // No anchors.rightMargin — see the sibling HoverBorder's
                // comment above; tree.columnWidthProvider already accounts
                // for the full right-side gap.
                anchors.leftMargin: AppSettings.sidebarEdgeLeftMargin
                anchors.topMargin: 1
                anchors.bottomMargin: 1
                // Must match anchors.topMargin/bottomMargin above — see this
                // property's doc comment in CombineGroupOutline.qml.
                edgeMargin: 1
                visible: treeDelegate.combineEligible
                strokeColor: treeDelegate.combineGroupColor
                // Shape's anti-aliased curve stroke reads visibly thinner
                // than a Rectangle border of the same nominal width (the
                // latter renders as a crisp, fully-opaque line; the former
                // spreads coverage across the AA edge) — bumped up from 1 to
                // match HoverBorder's actual perceived weight instead of its
                // literal border.width value.
                strokeWidth: 1.5
                cornerRadius: AppSettings.borderRadius
                topRounded: !treeDelegate.combineTopMerged
                bottomRounded: !treeDelegate.combineBottomMerged
            }

            // Nesting guides: indentation alone was the only thing separating
            // a folder's contents from the rows around it. One 1px vertical
            // line per ancestor level, at that level's expand-glyph center;
            // each row draws its own full-height segment, so consecutive
            // rows' segments join into one continuous line down the folder's
            // whole expanded block (VSCode-style: the line runs through
            // nested sub-blocks too, rather than stopping at the folder's
            // last DIRECT child — the classic Explorer style needs an
            // "is my level-N ancestor a last sibling" model walk per row for
            // a distinction that reads no clearer).
            Repeater {
                model: treeDelegate.depth
                Rectangle {
                    id: nestingGuide
                    required property int index
                    objectName: "nestingGuide"
                    // Center of the depth-`index` ancestor's 10px-wide
                    // expand-glyph column (treeColumn.x below is where a
                    // row's content starts; a dir's glyph is its first 10px).
                    x: nestingGuide.index * 16 + AppSettings.sidebarEdgeLeftMargin
                       + AppSettings.sidebarContentLeftMargin + 5
                    width: 1
                    // The deepest line — this row's own parent's — closes
                    // into a "└" at the connector tick when this is the last
                    // row of the parent's block, instead of overshooting half
                    // a row past its last child.
                    height: (nestingGuide.index === treeDelegate.depth - 1
                             && treeDelegate.nextRowDepth < treeDelegate.depth)
                            ? guideTick.y + 1 : treeDelegate.height
                    color: treeDelegate.guideColor
                }
            }
            // The horizontal connector: visibly hangs each child off its
            // parent's vertical guide, stopping 3px short of where this row's
            // own content begins so it points at the row without touching it.
            Rectangle {
                id: guideTick
                objectName: "guideTick"
                visible: treeDelegate.depth > 0
                x: (treeDelegate.depth - 1) * 16 + AppSettings.sidebarEdgeLeftMargin
                   + AppSettings.sidebarContentLeftMargin + 5
                // The title line's vertical center: the row header is 28px
                // tall in both row shapes (see implicitHeight above), with
                // the expand glyph centered in it.
                y: 14
                // From the parent guide across the remaining indent step
                // (16px), minus the 5px to the glyph center behind it and the
                // 3px stand-off before the content.
                width: 16 - 5 - 3
                height: 1
                color: treeDelegate.guideColor
            }

            Column {
                id: treeColumn
                // AppSettings.sidebarEdgeLeftMargin matches the outlines'
                // own left inset (HoverBorder/selection/CombineGroupOutline
                // above); sidebarContentLeftMargin is the FURTHER configurable
                // gap from that outline's left edge to where the row's own
                // content (status dot/label) begins — see both properties'
                // doc comments in AppSettings.qml.
                x: treeDelegate.depth * 16 + AppSettings.sidebarEdgeLeftMargin + AppSettings.sidebarContentLeftMargin
                // See the identical fix + explanation on searchResultsView's
                // resultColumn in this same file — conditionally toggling
                // between anchors.top and anchors.verticalCenter via
                // ternary+undefined is fragile and was observed live to get
                // stuck at the wrong position; a plain "y" binding avoids
                // the whole class of bug.
                y: treeDelegate.showSnippet ? 4 : (treeDelegate.height - treeColumn.implicitHeight) / 2
                spacing: 2

                // RowLayout, not Row — lets the title Text below
                // Layout.fillWidth to consume exactly whatever space its
                // ACTUALLY-VISIBLE siblings leave. A plain Row can't express
                // that: the title's width had to subtract a fixed worst-case
                // buffer (36px = expand-glyph 10 + status dot 8 + job dot 6 +
                // 3 spacings) for siblings that usually aren't all present
                // (a typical leaf row with no in-progress job only spends
                // ~16px), so the buffer overshot by ~20px and the name
                // elided noticeably before it needed to (user report — not a
                // Text/elide quirk, just this fixed constant). Layout
                // children can't use `anchors` (unsupported on direct Layout
                // children, unlike Row/Column) — Layout.alignment:
                // Qt.AlignVCenter below replaces each item's old
                // anchors.verticalCenter.
                RowLayout {
                    width: treeDelegate.width - treeColumn.x
                    spacing: 4

                    AppIcon {
                        // Collapses to 0 width for the common case (a leaf
                        // row, no expand glyph to show) instead of always
                        // reserving a fixed column — that reservation was
                        // the dominant contributor to a left margin much
                        // bigger than the row's own vertical centering gap
                        // (user report). Dir rows still get their glyph; only
                        // ITS OWN name shifts right relative to sibling file
                        // rows now, which reads fine since a dir's children
                        // are already indented a further level below it
                        // anyway.
                        //
                        // Gated on isDir ALONE, not isDir && hasChildren: an
                        // empty folder is a real folder (VaultTreeModel no
                        // longer prunes them) and reads as one only if it
                        // carries the same glyph column as its siblings —
                        // without it, it renders name-aligned with the FILES
                        // around it.
                        //
                        // An empty folder's state comes from the sidebar, NOT
                        // from treeDelegate.expanded, which is a one-way latch
                        // for childless nodes — see sidebar.collapsedEmptyDirs.
                        objectName: "expandGlyph"
                        Layout.alignment: Qt.AlignVCenter
                        Layout.preferredWidth: treeDelegate.isDir ? 10 : 0
                        name: !treeDelegate.isDir
                              ? ""
                              : (treeDelegate.hasChildren
                                 ? (treeDelegate.expanded ? "collapse" : "expand")
                                 : (sidebar.isEmptyDirCollapsed(treeDelegate.path) ? "expand" : "collapse"))
                        color: "#aaaaaa"
                        iconSize: 11
                    }

                    Rectangle {
                        Layout.alignment: Qt.AlignVCenter
                        visible: treeDelegate.isAudio || treeDelegate.isOrphanedMd
                        Layout.preferredWidth: 8
                        Layout.preferredHeight: 8
                        radius: 4
                        color: treeDelegate.status === 2 ? "#2ecc71" : (treeDelegate.isOrphanedMd ? "#3498db" : "#f1c40f")
                    }

                    // P6: separate small dot for an in-progress transcription
                    // job (Queued/Active/Paused) — deliberately distinct from
                    // the transcribed/untranscribed dot above rather than
                    // overloading it, since a Done/Failed job alongside an
                    // already-transcribed file would be redundant to show and
                    // this dot only ever appears for the 3 "in progress"
                    // statuses (see sidebar.setJobStatus's gating).
                    Rectangle {
                        id: jobStatusDot
                        Layout.alignment: Qt.AlignVCenter
                        visible: treeDelegate.isAudio && treeDelegate.jobStatus >= 0 && treeDelegate.jobStatus <= 2
                        Layout.preferredWidth: 6
                        Layout.preferredHeight: 6
                        radius: 3
                        color: treeDelegate.jobStatus === 1 ? "#3498db"
                               : (treeDelegate.jobStatus === 0 ? "#f39c12" : "#95a5a6")

                        HoverHandler {
                            id: jobStatusDotHover
                        }
                        ToolTip.visible: jobStatusDotHover.hovered
                        ToolTip.text: treeDelegate.jobStatus === 1 ? qsTr("Transcribing…")
                                      : (treeDelegate.jobStatus === 0 ? qsTr("Queued") : qsTr("Paused"))
                        ToolTip.delay: 500
                    }

                    Text {
                        id: rowNameText
                        objectName: "rowNameText"
                        Layout.alignment: Qt.AlignVCenter
                        // Hidden only while THIS row is being renamed inline —
                        // rowRenameField below takes its place in the layout.
                        // Gated on sidebar.renamingPath, never on the field's
                        // activeFocus (see sidebar.renamingPath's doc comment).
                        visible: sidebar.renamingPath !== treeDelegate.path
                        // Item 1 (Wave D): sidebar previously always showed the
                        // raw filename — AppSettings.useLabelsSidebar lets it
                        // show the stored label instead, when one exists. That
                        // choice is load-bearing beyond display now: inline
                        // rename edits whatever this shows, so with the toggle
                        // on, a labelled row renames its LABEL, not its file
                        // (see sidebar.renameInitialText/performRename).
                        // Item 9: when the query matches this text directly,
                        // the match is highlighted via rich text.
                        //
                        // Bug: this Text had no width bound at all, so a long
                        // filename (routine for split-recording candidates —
                        // exactly the "acme mic 3"-style names combine rules
                        // target) ran under the sidebar's scrollbar/edge
                        // instead of stopping short of it. Fills whatever
                        // width the RowLayout above has left after its other,
                        // actually-visible children (see RowLayout's own doc
                        // comment) — not shrunk to dodge the per-row
                        // action-icon cluster (rowTranscribeButton/
                        // rowRelabelButton/rowChaptersButton/groupMergeButton)
                        // — those icons are hover-only overlays (declared
                        // after this Column, so they already paint on top —
                        // see actionButtonRow below) rather than reserved
                        // layout space, so there's nothing to truncate for
                        // even while they're showing (user report: reserving
                        // space for them made names truncate the instant the
                        // icons faded in, for icons that weren't even always
                        // present).
                        Layout.fillWidth: true
                        // Item 8 bug fix: elide + Text.StyledText is an
                        // unsupported/unpredictable combination in Qt
                        // Quick's Text (see snippetText's identical fix
                        // above, item 2/Wave E, which this file already
                        // documents) — this Text switches to StyledText
                        // whenever a search query is active (for the
                        // highlight <span> markup below), so eliding must be
                        // OFF exactly then. clip: true is the backstop that
                        // still keeps overflow from painting over the
                        // per-row icon cluster while elide is off.
                        elide: textFormat === Text.PlainText ? Text.ElideRight : Text.ElideNone
                        clip: true
                        textFormat: sidebar.searchQuery !== "" ? Text.StyledText : Text.PlainText
                        text: {
                            const base = (function () {
                                if (!treeDelegate.isDir && AppSettings.useLabelsSidebar
                                        && treeDelegate.metadata && treeDelegate.metadata.label)
                                    return treeDelegate.metadata.label
                                if (treeDelegate.isDir || !AppSettings.hideExtensionsSidebar)
                                    return treeDelegate.name
                                const dot = treeDelegate.name.lastIndexOf(".")
                                return dot > 0 ? treeDelegate.name.substring(0, dot) : treeDelegate.name
                            })()
                            return sidebar.searchQuery !== "" ? sidebar.highlightMatch(base, sidebar.searchQuery) : base
                        }
                        color: "white"
                    }

                    // Inline rename editor — present only for the one row being
                    // renamed (sidebar.renamingPath). See that property's doc
                    // comment for why this swap is a deliberate, documented
                    // exception to StyleGuide.md's "Always-editable text" rule,
                    // and why the gate is renamingPath rather than activeFocus.
                    TextField {
                        id: rowRenameField
                        objectName: "rowRenameField"
                        Layout.alignment: Qt.AlignVCenter
                        Layout.fillWidth: true
                        visible: sidebar.renamingPath === treeDelegate.path
                        color: "white"
                        font.pixelSize: rowNameText.font.pixelSize
                        padding: 0
                        // StyleGuide.md "Text fields": underline background, not
                        // the default boxed one.
                        background: UnderlineField { focused: rowRenameField.activeFocus }

                        // Item 13: a QQC2 TextField gets no I-beam on its own.
                        // Declared on the field itself so it wins over the whole
                        // row's PointingHandCursor (rowHover) while editing —
                        // the row still reads as clickable, this text as typable.
                        HoverHandler {
                            cursorShape: Qt.IBeamCursor
                        }

                        // Guards the blur-commit below. Entering edit mode
                        // (visible -> true) happens a frame before focus lands,
                        // and the context-menu path arrives while the modal
                        // popup is still releasing focus — without this, that
                        // initial focus-less state reads as a blur and commits
                        // instantly, closing the editor the moment it opened.
                        property bool everFocused: false

                        onVisibleChanged: {
                            if (!rowRenameField.visible)
                                return
                            rowRenameField.everFocused = false
                            // Seeded by beginRename() — the sidebar decides what
                            // is being edited (see renameInitialText), this just
                            // shows it.
                            rowRenameField.text = sidebar.renamingText
                            rowRenameField.forceActiveFocus()
                            rowRenameField.selectAll()
                        }

                        // Keeps the sidebar's authoritative copy live. onTextEdited
                        // (user typing), not onTextChanged — the latter also fires
                        // for the programmatic seed above, which would race it.
                        onTextEdited: sidebar.renamingText = rowRenameField.text

                        onActiveFocusChanged: {
                            if (rowRenameField.activeFocus) {
                                rowRenameField.everFocused = true
                                return
                            }
                            // StyleGuide.md "Always-editable text" commit
                            // convention for a single-line field: commit on blur
                            // as well as on Return. Only reached for a REAL blur
                            // (clicking into MainPanel, say) — a click on another
                            // tree row leaves this field focused, and is committed
                            // by treeRowTap instead.
                            if (rowRenameField.everFocused && sidebar.renamingPath === treeDelegate.path)
                                sidebar.commitActiveRename()
                        }

                        onAccepted: sidebar.commitActiveRename()
                        Keys.onEscapePressed: sidebar.cancelRename()
                    }
                }

                // Item 2 (Wave E): fixes 4 issues with this row — (1) no
                // longer truncates: elide is documented as unsupported/
                // unpredictable together with StyledText in Qt Quick's Text
                // (the likely cause of (2) below too), so this now WRAPS
                // instead of eliding; (2) the highlight span now actually
                // renders, for the same reason; (3) clicking it opens the
                // file and jumps to the searched phrase; (4) explicit `x`
                // matches the title Text's own left edge (glyph column is
                // always 0-width here — showSnippet only ever applies to
                // audio/orphaned rows, never directories — so just spacing +
                // status dot + spacing = 16px), instead of starting flush at
                // the row's true left edge like before.
                Text {
                    id: snippetText
                    visible: treeDelegate.showSnippet
                    x: 16
                    width: Math.max(10, treeDelegate.width - x - 16)
                    textFormat: Text.StyledText
                    text: treeDelegate.snippetText
                    color: "#999999"
                    font.pixelSize: 11
                    wrapMode: Text.WordWrap

                    HoverHandler {
                        id: snippetHover
                        cursorShape: Qt.PointingHandCursor
                    }
                    TapHandler {
                        onTapped: sidebar.transcriptMatchActivated(treeDelegate.path, treeDelegate.isOrphanedMd,
                                                                    sidebar.searchQuery)
                    }
                    HoverBorder {
                        anchors.fill: parent
                        anchors.margins: -2
                        active: snippetHover.hovered
                    }
                }
            }

            // Per-row action-icon cluster (transcribe/relabel/chapters) — a
            // Row rather than each Item individually anchored to its
            // neighbor's .left, so the cluster's actual width tracks
            // whichever icons THIS row is actually eligible for. An
            // untranscribed row is only ever eligible for transcribe, not
            // relabel/chapters — with the old fixed anchor chain, the
            // ineligible ones still occupied their 20px+4px slot (just
            // invisible via opacity 0), so groupMergeButton (anchored off
            // rowChaptersButton.left) always sat 3 icons' worth of gap away
            // regardless of how many icons this row actually had (user
            // report). `visible` here is the row's PERMANENT eligibility
            // (isAudio/relabelEligible/chaptersEligible) — Row only reserves
            // space for visible children — while each icon's own `opacity`
            // stays the hover-only fade, so the cluster's width doesn't
            // itself animate/jump as rowHover toggles, only the icons within
            // it fade. layoutDirection: RightToLeft keeps declaration order
            // matching visual order (transcribe rightmost, same as before).
            Row {
                id: actionButtonRow
                anchors.right: parent.right
                anchors.rightMargin: 8
                anchors.verticalCenter: parent.verticalCenter
                layoutDirection: Qt.RightToLeft
                spacing: 4

                Item {
                    id: rowTranscribeButton
                    // objectName (here and on the four buttons below): lets
                    // tst_sidebar_keynav find these by name and read their
                    // real x, so it can check Left/Right against actual
                    // on-screen order instead of trusting
                    // currentRowActionButtons()'s own list order.
                    objectName: "rowTranscribeButton"
                    // Always available for any audio row, transcribed or
                    // not — a manual, explicit re-run should never be
                    // blocked (only "transcribe all"/bulkEligible above
                    // skips already-current files; this per-row icon is a
                    // deliberate single-file request, so it always fires).
                    visible: treeDelegate.isAudio
                    width: 20
                    height: 20
                    opacity: treeDelegate.rowActionsRevealed ? 1 : 0
                    enabled: opacity > 0

                    Behavior on opacity {
                        NumberAnimation { duration: AppSettings.animationsEnabled ? 120 : 0 }
                    }

                    ToolTip.visible: rowTranscribeHover.hovered
                    ToolTip.text: treeDelegate.status === 2 ? qsTr("Re-transcribe") : qsTr("Transcribe")
                    ToolTip.delay: 500

                    HoverHandler {
                        id: rowTranscribeHover
                        cursorShape: Qt.PointingHandCursor
                        enabled: rowTranscribeButton.enabled
                    }
                    TapHandler {
                        enabled: rowTranscribeButton.enabled
                        onTapped: sidebar.transcriptionController.transcribe(
                                      "file://" + treeDelegate.path, sidebar.vaultPath, AppSettings.defaultModel,
                                      false, AppSettings.autoLabelEnabled, AppSettings.restrictTagsToExisting,
                                      AppSettings.tagReuseBias, AppSettings.wordiness)
                    }
                    // Opaque backdrop so the glyph reads clearly against
                    // whatever filename text it's overlapping (these icons
                    // sit ON TOP of the label Text now, not beside it in
                    // reserved space — see that Text's own doc comment) —
                    // fades in/out together with this Item since Qt Quick
                    // multiplies a child's opacity by its parent's, so no
                    // separate Behavior needed here.
                    Rectangle {
                        anchors.fill: parent
                        radius: AppSettings.borderRadius
                        color: AppSettings.sidebarBackgroundColor
                    }
                    HoverBorder {
                        anchors.fill: parent
                        // Item 41: also outline when this button holds
                        // keyboard focus (Left/Right cluster nav) on the
                        // current row.
                        active: (rowTranscribeButton.enabled && rowTranscribeHover.hovered)
                                || (tree.activeFocus && treeDelegate.path === sidebar.currentPath
                                    && sidebar.actionButtonFocus === "transcribe")
                    }
                    AppIcon {
                        anchors.centerIn: parent
                        name: "transcribe"
                        color: "#dddddd"
                        iconSize: 13
                    }
                }

                // Wave D item 7: per-row "regenerate auto-title", same
                // fade-in pattern as rowTranscribeButton but for
                // already-transcribed rows.
                Item {
                    id: rowRelabelButton
                    objectName: "rowRelabelButton"
                    visible: treeDelegate.relabelEligible
                    width: 20
                    height: 20
                    opacity: treeDelegate.rowActionsRevealed ? 1 : 0
                    enabled: opacity > 0

                    Behavior on opacity {
                        NumberAnimation { duration: AppSettings.animationsEnabled ? 120 : 0 }
                    }

                    ToolTip.visible: rowRelabelHover.hovered
                    ToolTip.text: qsTr("Regenerate auto-title")
                    ToolTip.delay: 500

                    HoverHandler {
                        id: rowRelabelHover
                        cursorShape: Qt.PointingHandCursor
                        enabled: rowRelabelButton.enabled
                    }
                    TapHandler {
                        enabled: rowRelabelButton.enabled
                        onTapped: sidebar.transcriptionController.relabel(
                                      "file://" + treeDelegate.path, sidebar.vaultPath,
                                      AppSettings.restrictTagsToExisting, AppSettings.tagReuseBias,
                                      AppSettings.wordiness)
                    }
                    // See rowTranscribeButton's identical backdrop above.
                    Rectangle {
                        anchors.fill: parent
                        radius: AppSettings.borderRadius
                        color: AppSettings.sidebarBackgroundColor
                    }
                    HoverBorder {
                        anchors.fill: parent
                        active: (rowRelabelButton.enabled && rowRelabelHover.hovered)
                                || (tree.activeFocus && treeDelegate.path === sidebar.currentPath
                                    && sidebar.actionButtonFocus === "relabel")
                    }
                    AppIcon {
                        anchors.centerIn: parent
                        name: "relabel"
                        color: "#dddddd"
                        iconSize: 13
                    }
                }

                // Per-row "regenerate chapters", same fade-in shape as
                // rowRelabelButton but gated on chaptersEligible instead of
                // relabelEligible — orphaned .md rows qualify too, since
                // chapter generation never touches audio (see
                // chaptersEligible's doc comment above).
                Item {
                    id: rowChaptersButton
                    objectName: "rowChaptersButton"
                    visible: treeDelegate.chaptersEligible
                    width: 20
                    height: 20
                    opacity: treeDelegate.rowActionsRevealed ? 1 : 0
                    enabled: opacity > 0

                    Behavior on opacity {
                        NumberAnimation { duration: AppSettings.animationsEnabled ? 120 : 0 }
                    }

                    ToolTip.visible: rowChaptersHover.hovered
                    ToolTip.text: qsTr("Regenerate chapters")
                    ToolTip.delay: 500

                    HoverHandler {
                        id: rowChaptersHover
                        cursorShape: Qt.PointingHandCursor
                        enabled: rowChaptersButton.enabled
                    }
                    TapHandler {
                        enabled: rowChaptersButton.enabled
                        onTapped: sidebar.transcriptionController.generateChapters(
                                      "file://" + treeDelegate.path, sidebar.vaultPath, AppSettings.chapterDensity)
                    }
                    // See rowTranscribeButton's identical backdrop above.
                    Rectangle {
                        anchors.fill: parent
                        radius: AppSettings.borderRadius
                        color: AppSettings.sidebarBackgroundColor
                    }
                    HoverBorder {
                        anchors.fill: parent
                        active: (rowChaptersButton.enabled && rowChaptersHover.hovered)
                                || (tree.activeFocus && treeDelegate.path === sidebar.currentPath
                                    && sidebar.actionButtonFocus === "chapters")
                    }
                    AppIcon {
                        anchors.centerIn: parent
                        name: "chapters"
                        color: "#dddddd"
                        iconSize: 13
                    }
                }

                // Item 52 (CLAUDE.md per-file-action parity): per-row "detect
                // speakers", same fade-in shape as rowChaptersButton but gated
                // on relabelEligible (transcribed audio only — diarization
                // decodes the audio, so no orphaned .md). Calls diarizeFile()
                // like MainPanel's detectSpeakersButton.
                Item {
                    id: rowDetectSpeakersButton
                    objectName: "rowDetectSpeakersButton"
                    visible: treeDelegate.relabelEligible
                    width: 20
                    height: 20
                    opacity: treeDelegate.rowActionsRevealed ? 1 : 0
                    enabled: opacity > 0

                    Behavior on opacity {
                        NumberAnimation { duration: AppSettings.animationsEnabled ? 120 : 0 }
                    }

                    ToolTip.visible: rowDetectSpeakersHover.hovered
                    ToolTip.text: qsTr("Detect speakers")
                    ToolTip.delay: 500

                    HoverHandler {
                        id: rowDetectSpeakersHover
                        cursorShape: Qt.PointingHandCursor
                        enabled: rowDetectSpeakersButton.enabled
                    }
                    TapHandler {
                        enabled: rowDetectSpeakersButton.enabled
                        onTapped: sidebar.transcriptionController.diarizeFile(
                                      "file://" + treeDelegate.path, sidebar.vaultPath)
                    }
                    // See rowTranscribeButton's identical backdrop above.
                    Rectangle {
                        anchors.fill: parent
                        radius: AppSettings.borderRadius
                        color: AppSettings.sidebarBackgroundColor
                    }
                    HoverBorder {
                        anchors.fill: parent
                        active: (rowDetectSpeakersButton.enabled && rowDetectSpeakersHover.hovered)
                                || (tree.activeFocus && treeDelegate.path === sidebar.currentPath
                                    && sidebar.actionButtonFocus === "detectSpeakers")
                    }
                    AppIcon {
                        anchors.centerIn: parent
                        name: "speakers"
                        color: "#dddddd"
                        iconSize: 13
                    }
                }

                // Items 24/53: per-row "run all jobs" — full transcribe->
                // label->chapters pipeline for THIS one file. Visible for any
                // note (audio or orphaned .md); runAllJobs() skips the
                // transcribe step for anything already transcribed (incl. an
                // orphaned .md), so it's the right action regardless of status.
                Item {
                    id: rowRunAllButton
                    objectName: "rowRunAllButton"
                    visible: treeDelegate.isAudio || treeDelegate.isOrphanedMd
                    width: 20
                    height: 20
                    opacity: treeDelegate.rowActionsRevealed ? 1 : 0
                    enabled: opacity > 0

                    Behavior on opacity {
                        NumberAnimation { duration: AppSettings.animationsEnabled ? 120 : 0 }
                    }

                    ToolTip.visible: rowRunAllHover.hovered
                    ToolTip.text: qsTr("Run all jobs (transcribe, label, chapters)")
                    ToolTip.delay: 500

                    HoverHandler {
                        id: rowRunAllHover
                        cursorShape: Qt.PointingHandCursor
                        enabled: rowRunAllButton.enabled
                    }
                    TapHandler {
                        enabled: rowRunAllButton.enabled
                        onTapped: sidebar.transcriptionController.runAllJobs(
                                      "file://" + treeDelegate.path, sidebar.vaultPath, AppSettings.defaultModel,
                                      false, AppSettings.restrictTagsToExisting, AppSettings.tagReuseBias,
                                      AppSettings.wordiness, AppSettings.chapterDensity)
                    }
                    // See rowTranscribeButton's identical backdrop above.
                    Rectangle {
                        anchors.fill: parent
                        radius: AppSettings.borderRadius
                        color: AppSettings.sidebarBackgroundColor
                    }
                    HoverBorder {
                        anchors.fill: parent
                        active: (rowRunAllButton.enabled && rowRunAllHover.hovered)
                                || (tree.activeFocus && treeDelegate.path === sidebar.currentPath
                                    && sidebar.actionButtonFocus === "runAll")
                    }
                    AppIcon {
                        anchors.centerIn: parent
                        name: "run-all"
                        color: "#dddddd"
                        iconSize: 13
                    }
                }
            }

            // Item 51 refinement: ONE "combine this group now" button per
            // group, not one per row — it used to live in this row's
            // already-busy per-file action cluster (transcribe/relabel/
            // chapters icons), competing with the row's own hover highlight
            // for attention and only ever appearing while hovering this
            // EXACT row. Now it's decoupled from that cluster entirely:
            // rendered only on the group's TOP row (!combineTopMerged, i.e.
            // the top of the merged outline — user report, 2026-07-13: was
            // the bottom row, moved to the top so it reads next to the
            // group's "first" file) and revealed by hovering ANY member row,
            // via sidebar.hoveredCombineGroup (see rowHover's
            // onHoveredChanged above) — so it reads as "the whole outlined
            // group has one action," not another per-row icon.
            Item {
                id: groupMergeButton
                // Fixed to the row's top-left corner instead of anchored
                // off actionButtonRow (rowTranscribeButton/rowRelabelButton/
                // rowChaptersButton) — this button only ever renders on the
                // group's TOP row (see visible below), but the hover that
                // reveals it (sidebar.hoveredCombineGroup) can be triggered
                // by ANY row in the group, not just this one. Anchoring
                // relative to a sibling cluster whose own layout can shift
                // independently of THIS hover state is fragile and hard to
                // exercise with a real hover test — a fixed corner sidesteps
                // that entirely (user report, 2026-07-13).
                anchors.left: parent.left
                anchors.leftMargin: AppSettings.sidebarEdgeLeftMargin + 2 + combineOrderBadge.width + 2
                anchors.top: parent.top
                anchors.topMargin: 2
                width: 20
                height: 20
                visible: treeDelegate.combineEligible && !treeDelegate.combineTopMerged
                opacity: sidebar.hoveredCombineGroup === treeDelegate.combineOutputPath ? 1 : 0
                enabled: opacity > 0

                Behavior on opacity {
                    NumberAnimation { duration: AppSettings.animationsEnabled ? 120 : 0 }
                }

                ToolTip.visible: groupMergeHover.hovered
                ToolTip.text: qsTr("Combine this group")
                ToolTip.delay: 500

                HoverHandler {
                    id: groupMergeHover
                    cursorShape: Qt.PointingHandCursor
                    enabled: groupMergeButton.enabled
                }
                TapHandler {
                    enabled: groupMergeButton.enabled
                    onTapped: sidebar.transcriptionController.combineGroup(
                                  AppSettings.combineRules, sidebar.model.allAudioPaths(),
                                  treeDelegate.combineOutputPath, sidebar.vaultPath,
                                  AppSettings.deleteOriginalsAfterCombine)
                }
                // See rowTranscribeButton's identical backdrop doc comment.
                Rectangle {
                    anchors.fill: parent
                    radius: AppSettings.borderRadius
                    color: AppSettings.sidebarBackgroundColor
                }
                Rectangle {
                    anchors.fill: parent
                    color: "transparent"
                    radius: AppSettings.borderRadius
                    border.width: 1
                    border.color: treeDelegate.combineGroupColor
                    opacity: groupMergeButton.enabled && groupMergeHover.hovered ? 1 : 0
                    Behavior on opacity {
                        NumberAnimation { duration: AppSettings.animationsEnabled ? 100 : 0 }
                    }
                }
                AppIcon {
                    anchors.centerIn: parent
                    name: "group-merge"
                    color: treeDelegate.combineGroupColor
                    iconSize: 13
                }
            }

            // Per-row combine-order badge: this file's 1-based position in
            // its group's combine order (treeDelegate.combineOrder, from
            // group.files — the order files actually get concatenated in,
            // not necessarily this tree's display order). Unlike
            // groupMergeButton above, this renders on EVERY member row (the
            // merge action only makes sense once per group, but each file
            // needs its own ordinal) — positioned as the leftmost item of
            // the overlay cluster (groupMergeButton anchors off THIS item's
            // width, to its right) since the ordinal reads more naturally
            // before the action, and gated by the same
            // sidebar.hoveredCombineGroup hover so they appear/disappear
            // together.
            Item {
                id: combineOrderBadge
                anchors.left: parent.left
                anchors.leftMargin: AppSettings.sidebarEdgeLeftMargin + 2
                anchors.top: parent.top
                anchors.topMargin: 2
                width: 20
                height: 20
                visible: treeDelegate.combineEligible
                opacity: sidebar.hoveredCombineGroup === treeDelegate.combineOutputPath ? 1 : 0

                Behavior on opacity {
                    NumberAnimation { duration: AppSettings.animationsEnabled ? 120 : 0 }
                }

                Rectangle {
                    anchors.fill: parent
                    radius: AppSettings.borderRadius
                    color: AppSettings.sidebarBackgroundColor
                }
                Text {
                    anchors.centerIn: parent
                    text: treeDelegate.combineOrder
                    color: treeDelegate.combineGroupColor
                    font.pixelSize: 13
                }
            }
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

        delegate: Item {
            id: resultDelegate
            required property var modelData
            // Same unified right-side gap as tree.columnWidthProvider above
            // (see that property's doc comment) — but this is a plain
            // ListView delegate, not a TableView column, so this ordinary
            // width binding stays live-reactive on its own; no
            // columnWidthProvider/forceLayout() equivalent needed here.
            width: searchResultsView.width - AppSettings.sidebarRightMargin

            readonly property string resultPath: resultDelegate.modelData.path
            readonly property string resultName: resultDelegate.modelData.name
            readonly property string resultLabel: resultDelegate.modelData.label
            readonly property int resultStatus: resultDelegate.modelData.status
            readonly property bool resultIsAudio: resultDelegate.modelData.isAudio
            readonly property bool resultIsOrphanedMd: resultDelegate.modelData.isOrphanedMd
            readonly property string resultTranscriptBody: resultDelegate.modelData.transcriptBody

            // Same snippet-building idiom as treeDelegate above (item 9),
            // just fed from this result's own transcriptBody instead of a
            // tree row's TranscriptBodyRole — reuses the exact same
            // sidebar.cleanTranscript()/transcriptSnippet() helpers so the
            // two never drift.
            readonly property string cleanedBody: (sidebar.searchIncludeTranscriptions
                                                     && resultDelegate.resultTranscriptBody)
                ? sidebar.cleanTranscript(resultDelegate.resultTranscriptBody) : ""
            readonly property string snippetText: (sidebar.searchQuery !== "" && resultDelegate.cleanedBody !== "")
                ? sidebar.transcriptSnippet(resultDelegate.cleanedBody, sidebar.searchQuery, 40) : ""
            readonly property bool showSnippet: resultDelegate.snippetText !== ""

            readonly property string displayName: {
                if (AppSettings.useLabelsSidebar && resultDelegate.resultLabel)
                    return resultDelegate.resultLabel
                if (!AppSettings.hideExtensionsSidebar)
                    return resultDelegate.resultName
                const dot = resultDelegate.resultName.lastIndexOf(".")
                return dot > 0 ? resultDelegate.resultName.substring(0, dot) : resultDelegate.resultName
            }

            implicitHeight: resultDelegate.showSnippet ? (28 + resultSnippetText.implicitHeight + 4) : 28

            HoverHandler {
                id: resultHover
                cursorShape: Qt.PointingHandCursor
            }
            TapHandler {
                id: resultRowTap
                onTapped: {
                    // Mirrors treeDelegate's TapHandler exactly (minus the
                    // isDir/toggleExpanded branch — results are always
                    // leaves, never directories), including the
                    // forceActiveFocus() so Up/Down works here too, and
                    // (item 3) routing through the same handleNoteClick for
                    // Shift/Ctrl multi-select.
                    searchResultsView.forceActiveFocus()
                    if (resultDelegate.resultIsAudio || resultDelegate.resultIsOrphanedMd)
                        sidebar.handleNoteClick(resultDelegate.resultPath, resultDelegate.resultIsOrphanedMd,
                                                 resultRowTap.point.modifiers)
                }
            }
            // Note: no anchors.rightMargin — resultDelegate.width above
            // already subtracts the full AppSettings.sidebarRightMargin.
            HoverBorder {
                anchors.fill: parent
                anchors.topMargin: 1
                anchors.bottomMargin: 1
                anchors.leftMargin: AppSettings.sidebarEdgeLeftMargin
                active: resultHover.hovered
            }

            // O7: same persistent "currently open" highlight as treeDelegate.
            Rectangle {
                anchors.fill: parent
                anchors.topMargin: 1
                anchors.bottomMargin: 1
                anchors.leftMargin: AppSettings.sidebarEdgeLeftMargin
                visible: resultDelegate.resultPath === sidebar.currentPath
                color: AppSettings.sidebarSelectionColor
                radius: AppSettings.borderRadius
                border.width: 1
                border.color: AppSettings.sidebarSelectionBorderColor
            }

            // Item 3: same multi-select highlight as treeDelegate's above.
            Rectangle {
                anchors.fill: parent
                anchors.topMargin: 1
                anchors.bottomMargin: 1
                anchors.leftMargin: AppSettings.sidebarEdgeLeftMargin
                visible: sidebar.selectedPaths.length > 1
                         && sidebar.selectedPaths.indexOf(resultDelegate.resultPath) >= 0
                color: "transparent"
                radius: AppSettings.borderRadius
                border.width: 1
                border.color: AppSettings.highlightColor
            }

            Column {
                id: resultColumn
                // Same shape as treeDelegate.treeColumn's x above.
                x: AppSettings.sidebarEdgeLeftMargin + AppSettings.sidebarContentLeftMargin
                // Bug fix: conditionally toggling BETWEEN anchors.top and
                // anchors.verticalCenter via ternary+undefined is fragile —
                // Qt Quick's anchoring engine doesn't reliably clear/re-apply
                // anchor lines when both are driven by the same changing
                // condition (observed live: refining a search from a
                // body-match to a title-only match, which flips showSnippet
                // true->false, left this Column visually stuck at its old
                // top-anchored position instead of re-centering, overlapping
                // the row's top border). A plain "y" binding sidesteps the
                // anchor system entirely — this is an ordinary property
                // re-evaluation, not an anchor reconfiguration, so there's no
                // conflicting-anchor-lines class of bug possible.
                y: resultDelegate.showSnippet ? 4 : (resultDelegate.height - resultColumn.implicitHeight) / 2
                spacing: 2

                Row {
                    spacing: 6

                    Rectangle {
                        anchors.verticalCenter: parent.verticalCenter
                        width: 8
                        height: 8
                        radius: 4
                        // Same color logic as treeDelegate's status dot
                        // (TranscribedStatus green, orphaned-.md blue,
                        // otherwise untranscribed yellow).
                        color: resultDelegate.resultStatus === 2 ? "#2ecc71"
                               : (resultDelegate.resultIsOrphanedMd ? "#3498db" : "#f1c40f")
                    }

                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        textFormat: Text.StyledText
                        text: sidebar.highlightMatch(resultDelegate.displayName, sidebar.searchQuery)
                        color: "white"
                    }
                }

                Text {
                    id: resultSnippetText
                    visible: resultDelegate.showSnippet
                    x: 14
                    width: Math.max(10, resultDelegate.width - x - 16)
                    textFormat: Text.StyledText
                    text: resultDelegate.snippetText
                    color: "#999999"
                    font.pixelSize: 11
                    wrapMode: Text.WordWrap

                    HoverHandler {
                        id: resultSnippetHover
                        cursorShape: Qt.PointingHandCursor
                    }
                    TapHandler {
                        onTapped: sidebar.transcriptMatchActivated(resultDelegate.resultPath,
                                                                    resultDelegate.resultIsOrphanedMd,
                                                                    sidebar.searchQuery)
                    }
                    HoverBorder {
                        anchors.fill: parent
                        anchors.margins: -2
                        active: resultSnippetHover.hovered
                    }
                }
            }
        }
    }

    // Chrome/Firefox-style middle-click autoscroll for the file tree —
    // same two-gesture pattern (quick click arms free-scroll; press-drag-
    // release disarms on release) as MainPanel's transcript-view
    // middleDragScrollArea, adapted to TreeView (itself the Flickable, so
    // no .contentItem indirection is needed the way ScrollView requires).
    //
    // z is the whole story here, confirmed empirically (an offscreen
    // QTest::mouseMove repro against Qt 6.11's actual cursor resolution,
    // since this was wrong twice from reasoning alone): Qt Quick's cursor
    // resolution is a pure top-down PAINT-ORDER scan for "whichever item is
    // topmost at this point" — NOT gated by hoverEnabled, acceptedButtons,
    // or even whether that topmost item has an explicit cursorShape at all.
    // An idle item sitting above a row, even fully inert (hoverEnabled:
    // false, no cursorShape bound), still wins and shows the plain arrow
    // instead of falling through to the row's HoverHandler cursor — and
    // separately, an idle item with hoverEnabled: true also blocks hover
    // EVENT delivery itself to the row below (a different mechanism; that
    // part broke this area's HoverBorder highlight too, until an earlier
    // fix made hoverEnabled conditional).
    //
    // So this item now stays BELOW tree (z negative) while idle — proven by
    // the same repro that a below-tree MouseArea still receives the
    // qualifying middle-button press (TreeView's delegates only claim
    // LeftButton via TapHandler, so MiddleButton falls through to whatever
    // is underneath, no special fallthrough trick needed) — and only rises
    // above tree (z positive) once actually armed, which is exactly when it
    // SHOULD own both the cursor (SizeAll/SizeVer) and click-to-cancel
    // priority over row clicks, per Chrome's own convention.
    MouseArea {
        id: middleDragScrollArea
        // Search UI rearchitecture: this autoscroll is wired specifically
        // to tree's own Flickable geometry (contentY/contentHeight below) —
        // while search results are showing, tree is invisible and this would
        // just scroll a hidden view for no visible effect, so it's disabled
        // rather than adapted to also drive searchResultsView (which already
        // gets ordinary wheel/flick scrolling for free from ListView).
        enabled: sidebar.searchQuery === ""
        anchors.top: ribbon.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        z: middleDragScrollArea.armed ? 1 : -1
        acceptedButtons: middleDragScrollArea.armed
                         ? (Qt.LeftButton | Qt.MiddleButton | Qt.RightButton)
                         : Qt.MiddleButton
        // hoverEnabled only while armed: idle doesn't need it (arming only
        // needs onPressed, which fires regardless of hoverEnabled), and
        // leaving it true while idle would still shadow the row's own hover
        // EVENT delivery (see comment above) even though z now keeps the
        // CURSOR correct either way. Gesture 2's held-button drag still
        // delivers onPositionChanged with hoverEnabled false (an active
        // press grab guarantees that); gesture 1's free-scroll-without-
        // holding is the one case that needs hover tracking, and that only
        // ever runs once armed, when this binding has already flipped true.
        hoverEnabled: middleDragScrollArea.armed
        cursorShape: {
            if (!middleDragScrollArea.armed)
                return Qt.ArrowCursor
            const dy = Math.abs(middleDragScrollArea.currentY - middleDragScrollArea.originY)
            return dy <= middleDragScrollArea.deadzone ? Qt.SizeAllCursor : Qt.SizeVerCursor
        }

        property bool armed: false
        property real originY: 0
        property real currentY: 0
        property bool buttonHeld: false
        property bool draggedWhileHeld: false
        readonly property int deadzone: 8

        onPressed: (mouse) => {
            if (middleDragScrollArea.armed) {
                middleDragScrollArea.armed = false
                return
            }
            middleDragScrollArea.buttonHeld = true
            middleDragScrollArea.draggedWhileHeld = false
            middleDragScrollArea.armed = true
            const p = middleDragScrollArea.mapToItem(tree, mouse.x, mouse.y)
            middleDragScrollArea.originY = p.y
            middleDragScrollArea.currentY = p.y
        }
        onPositionChanged: (mouse) => {
            const p = middleDragScrollArea.mapToItem(tree, mouse.x, mouse.y)
            middleDragScrollArea.currentY = p.y
            if (middleDragScrollArea.buttonHeld && !middleDragScrollArea.draggedWhileHeld
                    && Math.abs(middleDragScrollArea.currentY - middleDragScrollArea.originY) > middleDragScrollArea.deadzone) {
                middleDragScrollArea.draggedWhileHeld = true
            }
        }
        onReleased: (mouse) => {
            if (mouse.button !== Qt.MiddleButton)
                return
            middleDragScrollArea.buttonHeld = false
            if (middleDragScrollArea.draggedWhileHeld)
                middleDragScrollArea.armed = false
            // else: real click, no drag — gesture 1 keeps running.
        }

        Timer {
            interval: 16
            repeat: true
            running: middleDragScrollArea.armed
            onTriggered: {
                const dy = middleDragScrollArea.currentY - middleDragScrollArea.originY
                const deadzone = middleDragScrollArea.deadzone
                if (Math.abs(dy) <= deadzone)
                    return
                const beyond = dy - Math.sign(dy) * deadzone
                const speed = Math.sign(beyond) * beyond * beyond * 0.002
                const maxY = Math.max(0, tree.contentHeight - tree.height)
                tree.contentY = Math.max(0, Math.min(maxY, tree.contentY + speed))
            }
        }
    }

    Shortcut {
        sequence: "Escape"
        enabled: middleDragScrollArea.armed
        onActivated: middleDragScrollArea.armed = false
    }

    // Item 40: folder-management context menu. Same custom-Popup shape as the
    // sortModeMenu above (this codebase has no QQC2 Menu) — plain Item rows,
    // each with a HoverHandler (pointer cursor) + TapHandler.
    Popup {
        id: contextMenu
        x: sidebar.ctxMenuX
        y: sidebar.ctxMenuY
        width: 200
        padding: 4
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
                    color: ctxNewFolderHover.hovered ? "#3a3a3a" : "transparent"
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

            // Item 44: "Combine selected (N)" — manual combine of the current
            // multi-selection. Visible only when 2+ AUDIO files are selected
            // (selectedAudioPaths() drops any orphaned .md), independent of
            // which specific row was right-clicked; the cleanest home for the
            // action alongside the other context entries. Calls
            // combineSelectedFiles() -> controller.combineSelected().
            Item {
                id: ctxCombineSelectedRow
                visible: sidebar.selectedAudioPaths().length >= 2
                width: parent.width
                height: 28

                HoverHandler {
                    id: ctxCombineSelectedHover
                    cursorShape: Qt.PointingHandCursor
                }
                TapHandler {
                    onTapped: {
                        contextMenu.close()
                        sidebar.combineSelectedFiles()
                    }
                }
                Rectangle {
                    anchors.fill: parent
                    radius: AppSettings.borderRadius
                    color: ctxCombineSelectedHover.hovered ? "#3a3a3a" : "transparent"
                }
                Text {
                    anchors.left: parent.left
                    anchors.leftMargin: 8
                    anchors.verticalCenter: parent.verticalCenter
                    text: qsTr("Combine selected (%1)").arg(sidebar.selectedAudioPaths().length)
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
                    color: ctxRenameHover.hovered ? "#3a3a3a" : "transparent"
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
                    color: ctxMoveHover.hovered ? "#3a3a3a" : "transparent"
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
                        contextMenu.close()
                        deleteConfirmPopup.open()
                    }
                }
                Rectangle {
                    anchors.fill: parent
                    radius: AppSettings.borderRadius
                    color: ctxDeleteHover.hovered ? "#3a3a3a" : "transparent"
                }
                Text {
                    anchors.left: parent.left
                    anchors.leftMargin: 8
                    anchors.verticalCenter: parent.verticalCenter
                    text: qsTr("Delete folder")
                    color: "#e07a7a"
                    font.pixelSize: 12
                }
            }

            // Item 2: "Delete transcript" — only for a transcribed AUDIO row
            // (status 2 == TranscribedStatus). Removes the sibling .md,
            // reverting the audio to untranscribed; the audio itself is kept.
            // (An orphaned .md has no audio to keep, so it gets "Delete file"
            // below instead, not this.)
            Item {
                id: ctxDeleteTranscriptRow
                visible: sidebar.ctxIsAudio && sidebar.ctxStatus === 2
                width: parent.width
                height: 28

                HoverHandler {
                    id: ctxDeleteTranscriptHover
                    cursorShape: Qt.PointingHandCursor
                }
                TapHandler {
                    onTapped: {
                        contextMenu.close()
                        sidebar.beginDeleteTranscript(sidebar.ctxPath)
                    }
                }
                Rectangle {
                    anchors.fill: parent
                    radius: AppSettings.borderRadius
                    color: ctxDeleteTranscriptHover.hovered ? "#3a3a3a" : "transparent"
                }
                Text {
                    anchors.left: parent.left
                    anchors.leftMargin: 8
                    anchors.verticalCenter: parent.verticalCenter
                    text: qsTr("Delete transcript")
                    color: "#e07a7a"
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
                        sidebar.beginDeleteFile(sidebar.ctxPath)
                    }
                }
                Rectangle {
                    anchors.fill: parent
                    radius: AppSettings.borderRadius
                    color: ctxDeleteFileHover.hovered ? "#3a3a3a" : "transparent"
                }
                Text {
                    objectName: "ctxDeleteFileLabel"
                    anchors.left: parent.left
                    anchors.leftMargin: 8
                    anchors.verticalCenter: parent.verticalCenter
                    text: ctxDeleteFileRow.targetCount > 1
                          ? qsTr("Delete %1 items").arg(ctxDeleteFileRow.targetCount)
                          : qsTr("Delete file")
                    color: "#e07a7a"
                    font.pixelSize: 12
                }
            }
        }
    }

    // Item 40: "New folder" name entry.
    Popup {
        id: newFolderPopup
        x: sidebar.ctxMenuX
        y: sidebar.ctxMenuY
        width: 240
        padding: 8
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
                color: "#e07a7a"
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
                        color: "#aaaaaa"
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
        x: sidebar.ctxMenuX
        y: sidebar.ctxMenuY
        width: 260
        padding: 4
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

        Column {
            width: moveToPopup.width - moveToPopup.padding * 2
            spacing: 2

            Text {
                width: parent.width
                leftPadding: 8
                topPadding: 4
                bottomPadding: 2
                text: qsTr("Move to…")
                color: "#aaaaaa"
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
                        color: moveFolderHover.hovered ? "#3a3a3a" : "transparent"
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

    // Item 40: delete-folder confirmation (recursive delete is destructive).
    Popup {
        id: deleteConfirmPopup
        x: sidebar.ctxMenuX
        y: sidebar.ctxMenuY
        width: 260
        padding: 8
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

        Column {
            width: deleteConfirmPopup.width - deleteConfirmPopup.padding * 2
            spacing: 8

            Label {
                width: parent.width
                wrapMode: Text.WordWrap
                text: qsTr("Delete “%1” and everything inside it? This can't be undone.")
                      .arg(sidebar.ctxPath.substring(sidebar.ctxPath.lastIndexOf("/") + 1))
                color: "white"
                font.pixelSize: 12
            }

            Row {
                anchors.right: parent.right
                spacing: 6

                Item {
                    id: deleteCancelButton
                    width: deleteCancelLabel.implicitWidth + 16
                    height: 24
                    HoverHandler {
                        id: deleteCancelHover
                        cursorShape: Qt.PointingHandCursor
                    }
                    TapHandler {
                        onTapped: deleteConfirmPopup.close()
                    }
                    HoverBorder {
                        anchors.fill: parent
                        active: deleteCancelHover.hovered
                    }
                    Text {
                        id: deleteCancelLabel
                        anchors.centerIn: parent
                        text: qsTr("Cancel")
                        color: "#aaaaaa"
                        font.pixelSize: 12
                    }
                }

                Item {
                    id: deleteConfirmButton
                    width: deleteConfirmLabel.implicitWidth + 16
                    height: 24
                    HoverHandler {
                        id: deleteConfirmHover
                        cursorShape: Qt.PointingHandCursor
                    }
                    TapHandler {
                        onTapped: sidebar.confirmDeleteFolder()
                    }
                    HoverBorder {
                        anchors.fill: parent
                        active: deleteConfirmHover.hovered
                    }
                    Text {
                        id: deleteConfirmLabel
                        anchors.centerIn: parent
                        text: qsTr("Delete")
                        color: "#e07a7a"
                        font.pixelSize: 12
                    }
                }
            }
        }
    }

    // Item 2: shared confirmation for "Delete transcript" / "Delete file"
    // (destructive, though the files go to the trash — recoverable). Mirrors
    // deleteConfirmPopup's style; the message + confirm action switch on
    // sidebar.pendingDeleteMode.
    Popup {
        id: fileDeleteConfirmPopup
        x: sidebar.ctxMenuX
        y: sidebar.ctxMenuY
        width: 280
        padding: 8
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

        Column {
            width: fileDeleteConfirmPopup.width - fileDeleteConfirmPopup.padding * 2
            spacing: 8

            Label {
                width: parent.width
                wrapMode: Text.WordWrap
                text: {
                    if (sidebar.pendingDeleteMode === "transcript")
                        return qsTr("Delete the transcript for “%1”? The audio file is kept. Moved to the trash.")
                            .arg(sidebar.pendingDeleteName())
                    return sidebar.pendingDeletePaths.length > 1
                        ? qsTr("Delete %1 items and their transcripts? Moved to the trash.")
                            .arg(sidebar.pendingDeletePaths.length)
                        : qsTr("Delete “%1” and its transcript? Moved to the trash.")
                            .arg(sidebar.pendingDeleteName())
                }
                color: "white"
                font.pixelSize: 12
            }

            Row {
                anchors.right: parent.right
                spacing: 6

                Item {
                    id: fileDeleteCancelButton
                    width: fileDeleteCancelLabel.implicitWidth + 16
                    height: 24
                    HoverHandler {
                        id: fileDeleteCancelHover
                        cursorShape: Qt.PointingHandCursor
                    }
                    TapHandler {
                        onTapped: fileDeleteConfirmPopup.close()
                    }
                    HoverBorder {
                        anchors.fill: parent
                        active: fileDeleteCancelHover.hovered
                    }
                    Text {
                        id: fileDeleteCancelLabel
                        anchors.centerIn: parent
                        text: qsTr("Cancel")
                        color: "#aaaaaa"
                        font.pixelSize: 12
                    }
                }

                Item {
                    id: fileDeleteConfirmButton
                    width: fileDeleteConfirmLabel.implicitWidth + 16
                    height: 24
                    HoverHandler {
                        id: fileDeleteConfirmHover
                        cursorShape: Qt.PointingHandCursor
                    }
                    TapHandler {
                        onTapped: sidebar.confirmFileDelete()
                    }
                    HoverBorder {
                        anchors.fill: parent
                        active: fileDeleteConfirmHover.hovered
                    }
                    Text {
                        id: fileDeleteConfirmLabel
                        anchors.centerIn: parent
                        text: sidebar.pendingDeleteMode === "transcript"
                              ? qsTr("Delete transcript")
                              : (sidebar.pendingDeletePaths.length > 1
                                 ? qsTr("Delete %1 items").arg(sidebar.pendingDeletePaths.length)
                                 : qsTr("Delete file"))
                        color: "#e07a7a"
                        font.pixelSize: 12
                    }
                }
            }
        }
    }
}
