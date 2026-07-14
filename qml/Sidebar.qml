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
    // clicking the row directly. No separate "selected note" state exists.
    function activateNote(path, isOrphanedMd) {
        if (isOrphanedMd)
            sidebar.orphanedMdActivated(path)
        else
            sidebar.fileActivated(path)
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
        tree.positionViewAtRow(notes[idx].row, TableView.Contain)
    }
    onSearchQueryChanged: sidebar.refreshSearchResults()
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

        const sortKeys = {}
        for (const path of audioPaths) {
            const key = clusterKeyOf[path]
            sortKeys[path] = [key !== undefined ? groupRankOf[key] : ownRank(path),
                               key !== undefined ? memberOrderRank(path) : ownRank(path)]
        }

        // Adjacency (combineTopMerged/combineBottomMerged) has to be
        // computed from the SAME order the tree actually ends up displaying
        // — see this property's doc comment above for the bug that bit when
        // this used allAudioPaths()'s raw order instead.
        const displayOrder = audioPaths.slice().sort((a, b) => {
            const ka = sortKeys[a], kb = sortKeys[b]
            if (ka[0] !== kb[0])
                return ka[0] < kb[0] ? -1 : 1
            if (ka[1] !== kb[1])
                return ka[1] < kb[1] ? -1 : 1
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
                        const paths = Object.keys(sidebar.untranscribedPaths)
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

            // Wave D item 7: bulk "regenerate auto-title" mirroring
            // transcribeAllButton above, over already-transcribed files
            // instead of untranscribed ones.
            Item {
                id: relabelAllButton
                Layout.preferredWidth: 22
                Layout.preferredHeight: 22
                enabled: sidebar.transcriptionController && Object.keys(sidebar.transcribedPaths).length > 0

                HoverHandler {
                    id: relabelAllHover
                    cursorShape: Qt.PointingHandCursor
                    enabled: relabelAllButton.enabled
                }
                TapHandler {
                    enabled: relabelAllButton.enabled
                    onTapped: {
                        const paths = Object.keys(sidebar.transcribedPaths)
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
                Text {
                    anchors.centerIn: parent
                    text: "↻"
                    color: relabelAllButton.enabled ? "#dddddd" : "#666666"
                    font.pixelSize: 15
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
                enabled: sidebar.transcriptionController && Object.keys(sidebar.chaptersPaths).length > 0

                HoverHandler {
                    id: chaptersAllHover
                    cursorShape: Qt.PointingHandCursor
                    enabled: chaptersAllButton.enabled
                }
                TapHandler {
                    enabled: chaptersAllButton.enabled
                    onTapped: {
                        const paths = Object.keys(sidebar.chaptersPaths)
                        const urls = paths.map((p) => "file://" + p)
                        sidebar.transcriptionController.generateChaptersAll(urls, sidebar.vaultPath)
                    }
                }
                HoverBorder {
                    anchors.fill: parent
                    active: chaptersAllButton.enabled && chaptersAllHover.hovered
                }
                Text {
                    anchors.centerIn: parent
                    text: "≡"
                    color: chaptersAllButton.enabled ? "#dddddd" : "#666666"
                    font.pixelSize: 15
                }

                ToolTip.visible: chaptersAllHover.hovered
                ToolTip.text: qsTr("Regenerate all chapters")
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
                Text {
                    anchors.centerIn: parent
                    text: "⛓"
                    color: combineButton.enabled ? "#dddddd" : "#666666"
                    font.pixelSize: 15
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
                Text {
                    anchors.centerIn: parent
                    text: "⇅"
                    color: "#dddddd"
                    font.pixelSize: 15
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
                Text {
                    anchors.centerIn: parent
                    text: "×"
                    color: "#dddddd"
                    font.pixelSize: 15
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
                            Text {
                                anchors.centerIn: parent
                                text: "×"
                                color: tagChipRemoveHover.hovered ? "white" : "#999999"
                                font.pixelSize: 12
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
                Text {
                    anchors.centerIn: parent
                    text: "×"
                    color: "#dddddd"
                    font.pixelSize: 15
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
                            Text {
                                anchors.centerIn: parent
                                text: "×"
                                color: speakerChipRemoveHover.hovered ? "white" : "#999999"
                                font.pixelSize: 12
                            }
                        }
                    }
                }
            }
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
                    // P13(a): expandAll() re-inserts every previously-expanded
                    // directory's children synchronously, but the ALREADY
                    // pending Qt.callLater(forceLayout) from
                    // relayoutForSearchStateChange() (triggered by the same
                    // model reset) can fire before this point, caching stale/
                    // partial row positions with nothing to re-trigger a
                    // layout afterward — force one now, after expansion is done.
                    tree.forceLayout()
                    stop()
                }
            }
        }

        Connections {
            target: searchProxy
            function onModelReset() {
                expandRetryTimer.start()
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
            // TreeView happens to have realized). Non-adjacent members of
            // the same group (rare — e.g. files split across folders) simply
            // get their own separate merged run; still correct, just not a
            // single perimeter in that case.
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
                }
            }

            TapHandler {
                onTapped: {
                    // Grabs keyboard focus so Up/Down (see the Shortcut
                    // items near Ctrl+F's) can move between notes right
                    // after a click — never touches sidebar.currentPath, so
                    // the persistent "currently open" outline (O7) is
                    // unaffected; clicking into MainPanel afterward moves
                    // activeFocus away exactly as it always has.
                    treeDelegate.treeView.forceActiveFocus()
                    if (treeDelegate.isDir) {
                        treeDelegate.treeView.toggleExpanded(treeDelegate.row)
                    } else if (treeDelegate.isAudio) {
                        sidebar.fileActivated(treeDelegate.path)
                    } else if (treeDelegate.isOrphanedMd) {
                        sidebar.orphanedMdActivated(treeDelegate.path)
                    }
                }
            }

            // Note: no anchors.rightMargin here — tree.columnWidthProvider
            // already subtracts the FULL AppSettings.sidebarRightMargin from
            // this row's own width, so this Item's right edge already sits
            // at that inset; adding it again here would double it.
            HoverBorder {
                anchors.fill: parent
                anchors.topMargin: 1
                anchors.bottomMargin: 1
                anchors.leftMargin: AppSettings.sidebarEdgeLeftMargin
                active: rowHover.hovered
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

                    Text {
                        // Collapses to 0 width for the common case (a leaf
                        // row, no expand glyph to show) instead of always
                        // reserving a fixed column — that reservation was
                        // the dominant contributor to a left margin much
                        // bigger than the row's own vertical centering gap
                        // (user report). Expandable dir rows still get their
                        // glyph; only ITS OWN name shifts right relative to
                        // sibling file rows now, which reads fine since a
                        // dir's children are already indented a further
                        // level below it anyway.
                        Layout.alignment: Qt.AlignVCenter
                        Layout.preferredWidth: (treeDelegate.isDir && treeDelegate.hasChildren) ? 10 : 0
                        horizontalAlignment: Text.AlignHCenter
                        text: (treeDelegate.isDir && treeDelegate.hasChildren)
                              ? (treeDelegate.expanded ? "−" : "+")
                              : ""
                        color: "#aaaaaa"
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
                        Layout.alignment: Qt.AlignVCenter
                        // Item 1 (Wave D): sidebar previously always showed the
                        // raw filename — AppSettings.useLabelsSidebar (a
                        // display-only toggle; Sidebar has no rename UI, unlike
                        // MainPanel's useLabelsMainPanel) now lets it show the
                        // stored label instead, when one exists.
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
                        elide: Text.ElideRight
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
                    // Always available for any audio row, transcribed or
                    // not — a manual, explicit re-run should never be
                    // blocked (only "transcribe all"/bulkEligible above
                    // skips already-current files; this per-row icon is a
                    // deliberate single-file request, so it always fires).
                    visible: treeDelegate.isAudio
                    width: 20
                    height: 20
                    opacity: rowHover.hovered ? 1 : 0
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
                        active: rowTranscribeButton.enabled && rowTranscribeHover.hovered
                    }
                    Text {
                        anchors.centerIn: parent
                        text: "⤒"
                        color: "#dddddd"
                        font.pixelSize: 13
                    }
                }

                // Wave D item 7: per-row "regenerate auto-title", same
                // fade-in pattern as rowTranscribeButton but for
                // already-transcribed rows.
                Item {
                    id: rowRelabelButton
                    visible: treeDelegate.relabelEligible
                    width: 20
                    height: 20
                    opacity: rowHover.hovered ? 1 : 0
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
                        active: rowRelabelButton.enabled && rowRelabelHover.hovered
                    }
                    Text {
                        anchors.centerIn: parent
                        text: "↻"
                        color: "#dddddd"
                        font.pixelSize: 13
                    }
                }

                // Per-row "regenerate chapters", same fade-in shape as
                // rowRelabelButton but gated on chaptersEligible instead of
                // relabelEligible — orphaned .md rows qualify too, since
                // chapter generation never touches audio (see
                // chaptersEligible's doc comment above).
                Item {
                    id: rowChaptersButton
                    visible: treeDelegate.chaptersEligible
                    width: 20
                    height: 20
                    opacity: rowHover.hovered ? 1 : 0
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
                        active: rowChaptersButton.enabled && rowChaptersHover.hovered
                    }
                    Text {
                        anchors.centerIn: parent
                        text: "≡"
                        color: "#dddddd"
                        font.pixelSize: 13
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
                Text {
                    anchors.centerIn: parent
                    text: "⊕"
                    color: treeDelegate.combineGroupColor
                    font.pixelSize: 13
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
                onTapped: {
                    // Mirrors treeDelegate's TapHandler exactly (minus the
                    // isDir/toggleExpanded branch — results are always
                    // leaves, never directories), including the
                    // forceActiveFocus() so Up/Down works here too.
                    searchResultsView.forceActiveFocus()
                    if (resultDelegate.resultIsAudio)
                        sidebar.fileActivated(resultDelegate.resultPath)
                    else if (resultDelegate.resultIsOrphanedMd)
                        sidebar.orphanedMdActivated(resultDelegate.resultPath)
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
}
