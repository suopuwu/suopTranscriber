import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Window
import SuopTranscriber
import "TextFormat.js" as TextFormat

// Extracted verbatim from Sidebar.qml's TreeView `delegate: Item { ... }`.
// Pure relocation: `sidebarRoot` and `treeRoot` (the Sidebar root and its TreeView)
// are threaded in as properties below and bound at the instantiation site in
// Sidebar.qml (`delegate: TreeRowDelegate { sidebarRoot: sidebarRoot; treeRoot: treeRoot; ... }`),
// so every `sidebarRoot.foo` / `treeRoot.foo` reference in the body resolves unchanged.
Item {
    id: treeDelegate

    // The Sidebar root and its TreeView, threaded from the instantiation site
    // so the moved-verbatim body's `sidebarRoot.*` / `treeRoot.*` references resolve.
    required property Item sidebarRoot
    required property TreeView treeRoot
    // Column width (and so this delegate's own width) comes from
    // treeRoot.columnWidthProvider above, not implicitWidth here — see
    // that property's doc comment for why (the Basic style's
    // vertical ScrollBar is an OVERLAY needing a dedicated lane, and
    // AppSettings.sidebarRightMargin is the ONE setting for that
    // whole right-side gap, live-reactive via columnWidthProvider).
    // snippetText now wraps instead of truncating,
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
    // VaultTreeModel::LabelRole — read live from the sidecar .md's
    // frontmatter per data() call, so a relabel's finished() ->
    // model.refreshFile() -> dataChanged updates it reactively with no
    // revision-counter push (see the onFinished handler above).
    required property string label
    // The synthetic in-progress row standing in for a combine that
    // hasn't produced its file yet — see VaultTreeModel::
    // setPendingCombines(). It is not a file: isDir/isAudio/
    // isOrphanedMd are all false, which already makes every eligibility
    // binding below false and keeps it out of sidebarRoot.visibleNotePaths()
    // (so it can't be selected, arrow-navigated, or bulk-acted on)
    // without any of them naming it. Only the things that would
    // otherwise treat "not a dir" as "openable file" need the explicit
    // guards below: the two TapHandlers and the drag.
    required property bool isCombinePlaceholder
    required property int combineSourceCount

    // Is the combine this placeholder stands for the job actually
    // RUNNING right now (vs. still queued behind other work)? A combine
    // job's fileUrl is its output path, which is exactly this row's
    // path — that's what lets the row find its own job without a
    // lookup. Bound to currentFile/busy/progress, all of which are real
    // Q_PROPERTYs with notify signals, so this stays live (an
    // isQueued()-style Q_INVOKABLE would not — see MainPanel's
    // busyOrQueuedUrls comment for that exact trap).
    readonly property bool combineActive: treeDelegate.isCombinePlaceholder
        && sidebarRoot.transcriptionController
        && sidebarRoot.transcriptionController.busy
        && String(sidebarRoot.transcriptionController.currentFile).replace("file://", "")
               === treeDelegate.path
    readonly property real combineProgress: treeDelegate.combineActive
        ? sidebarRoot.transcriptionController.progress : 0
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
    readonly property bool bulkEligible: treeDelegate.isAudio && sidebarRoot.transcriptionController
        && !sidebarRoot.transcriptionController.matchesExistingTranscription(
               "file://" + treeDelegate.path, AppSettings.defaultModel)

    // Inverse of bulkEligible — already-transcribed
    // audio files, for the per-row + bulk "regenerate auto-title".
    readonly property bool relabelEligible: treeDelegate.isAudio && treeDelegate.status === 2

    // Chapters, unlike relabel, only ever read the .md's existing
    // transcript body (TranscriptionController::generateChaptersJob()
    // never touches audio) — so an orphaned .md is just as eligible
    // as an already-transcribed audio row.
    readonly property bool chaptersEligible: treeDelegate.relabelEligible || treeDelegate.isOrphanedMd

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
    // it, so it would light the entire treeRoot; a drop targeting the root
    // is marked by rootDropTarget's own border around the whole view
    // instead.
    readonly property string folderHighlightRoot: {
        const candidate = sidebarRoot.rowDragActive ? sidebarRoot.rowDragDest : sidebarRoot.hoveredFolderPath
        const vaultRoot = sidebarRoot.model ? sidebarRoot.model.vaultLocalPath : ""
        return (candidate === "" || candidate === vaultRoot) ? "" : candidate
    }
    readonly property bool inFolderHighlight:
            TextFormat.isUnderFolder(treeDelegate.path, treeDelegate.folderHighlightRoot)

    // The run's shape at this row. A folder's descendants are
    // contiguous with it in display order (the treeRoot IS the directory
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
            && TextFormat.isUnderFolder(sidebarRoot.pathAtTreeRow(treeDelegate.row + 1, treeRoot.rows),
                                      treeDelegate.folderHighlightRoot)

    // Nesting-guide inputs (see the guide Rectangles below): the next
    // display row's depth decides whether this row is the LAST row of
    // its parent's expanded block (nextRowDepth < depth — anything
    // still inside the block, including this row's own children, is
    // deeper). -1 out of range, so the treeRoot's final row counts as a
    // block end too.
    readonly property int nextRowDepth: sidebarRoot.depthAtTreeRow(treeDelegate.row + 1, treeRoot.rows)
    // Dim on purpose — the guides are containment texture behind the
    // rows' real content, not content themselves (the expand glyphs'
    // #aaaaaa at ~20% alpha).
    readonly property color guideColor: "#33aaaaaa"

    // True while THIS row is the one being renamed inline — see
    // sidebarRoot.renamingPath for why the gate is that explicit property
    // and never the editor's activeFocus. Every hover-revealed overlay
    // on the row keys off this to stay hidden for the duration of the
    // edit (see groupMergeButton, combineOrderBadge), since they all paint ON TOP of the row's
    // content rather than reserving layout space: a rename is the one
    // time the row's full width is the user's to read and type in, and
    // the icons were covering the tail of a long name (user report).
    readonly property bool renamingThisRow: sidebarRoot.renamingPath === treeDelegate.path

    // True iff this row is a member of a "ready" (would-
    // combine-if-run) split-recording group per
    // sidebarRoot.combineReadyPaths — a genuinely reactive binding (both
    // combineReadyPaths and path have real NOTIFY signals), unlike
    // jobStatus above which needs the explicit refresh idiom because
    // jobStatusForPath() itself has none. Drives the row highlight
    // and groupMergeButton below.
    readonly property bool combineEligible: treeDelegate.isAudio
            && sidebarRoot.combineReadyPaths[treeDelegate.path] !== undefined
    readonly property string combineOutputPath: treeDelegate.combineEligible
            ? sidebarRoot.combineReadyPaths[treeDelegate.path] : ""
    readonly property color combineGroupColor: treeDelegate.combineEligible
            ? TextFormat.combineGroupColor(treeDelegate.combineOutputPath) : "transparent"
    // 1-based position in the group's combine order (group.files
    // order from findCombineGroups() — the order files are actually
    // concatenated in, not necessarily this treeRoot's display order).
    // Drives combineOrderBadge below.
    readonly property int combineOrder: treeDelegate.combineEligible
            ? (sidebarRoot.combineOrderPaths[treeDelegate.path] || 0) : 0

    // Merges this row's outline with a vertically-adjacent row's
    // outline when they're in the SAME combine group, so a
    // contiguous run of group members reads as one unified outline
    // (only the outer perimeter drawn) instead of N stacked boxes —
    // see CombineGroupOutline below. Plain reactive bindings against
    // sidebarRoot.combineTopMergedPaths/combineBottomMergedPaths (see
    // TreeOrdering::compute()'s doc comment for why those are computed
    // from the treeRoot's actual display order, not from which rows
    // TreeView happens to have realized). A group's members are
    // always siblings in one folder (findCombineGroups() keys group
    // identity on the containing folder too, so a cross-folder group
    // can't exist), and the sortKeys triple gives cluster identity
    // its own tier — so members are contiguous both in that display
    // order and on screen, and a group draws as one perimeter.
    readonly property bool combineTopMerged: treeDelegate.combineEligible
            && sidebarRoot.combineTopMergedPaths[treeDelegate.path] === true
    readonly property bool combineBottomMerged: treeDelegate.combineEligible
            && sidebarRoot.combineBottomMergedPaths[treeDelegate.path] === true

    // JobStatus int for this row's file (None/Queued/Active/
    // Paused/Done/Failed, see TranscriptionController::JobStatus) —
    // unlike bulkEligible/relabelEligible above this can't be a
    // readonly binding (jobStatusForPath() is a plain Q_INVOKABLE
    // call, not a bound property), so it's explicitly refreshed on
    // path reassignment and whenever sidebarRoot.jobStatusRevision bumps.
    property int jobStatus: -1
    function refreshJobStatus() {
        treeDelegate.jobStatus = (treeDelegate.isAudio && sidebarRoot.transcriptionController)
            ? sidebarRoot.transcriptionController.jobStatusForPath(treeDelegate.path) : -1
    }
    Connections {
        target: sidebarRoot
        function onJobStatusRevisionChanged() { treeDelegate.refreshJobStatus() }
    }

    // Bug: TreeView recycles a fixed pool of delegate Items as rows
    // scroll in/out of view (reuseItems defaults to true) — required
    // properties like path/isAudio/isOrphanedMd (and now label) get
    // reassigned to the recycled Item for its new row, but jobStatus
    // above is a plain imperative property that was only ever pushed
    // once, at Component.onCompleted (i.e. whichever row this Item
    // instance FIRST rendered). Every recycled reuse of that same
    // instance kept showing that original row's jobStatus forever
    // after, regardless of which file it now represents. Refresh on
    // every path reassignment, not just first creation. (metadata/label
    // no longer needs this — it's a model role that auto-updates on
    // recycle like the other required properties.)
    onPathChanged: {
        treeDelegate.refreshJobStatus()
    }

    Component.onCompleted: {
        treeDelegate.refreshJobStatus()
    }
    Component.onDestruction: {
        // Balance the hover count if this row is destroyed (e.g.
        // TreeView recycling) WHILE still counted as hovering —
        // otherwise that increment would never be decremented and
        // the group's button could get stuck visible forever.
        if (treeDelegate.activeHoverGroupPath !== "") {
            sidebarRoot.leaveCombineGroupHover(treeDelegate.activeHoverGroupPath)
            treeDelegate.activeHoverGroupPath = ""
        }
    }

    // Ellipsis-bounded transcript snippet around the first
    // match, shown only when there's an active query, transcript
    // search is enabled, and this row's body actually contains it —
    // rows that only matched by name/label/tags show no snippet.
    readonly property string cleanedBody: (sidebarRoot.searchIncludeTranscriptions && treeDelegate.transcriptBody)
        ? TextFormat.cleanTranscript(treeDelegate.transcriptBody) : ""
    readonly property string snippetText: (sidebarRoot.searchQuery !== "" && treeDelegate.cleanedBody !== "")
        ? TextFormat.transcriptSnippet(treeDelegate.cleanedBody, sidebarRoot.searchQuery, 40,
                               AppSettings.highlightColor) : ""
    readonly property bool showSnippet: treeDelegate.snippetText !== ""
    // showSnippet can also flip because transcriptBody
    // changed independently of sidebarRoot.searchQuery/
    // searchIncludeTranscriptions (e.g. a transcript streaming in
    // live while search is already active) — neither of treeRoot's two
    // Connections handlers above fire for that, leaving this row's
    // cached TableView Y stale. Trigger the same relayout directly.
    onShowSnippetChanged: treeRoot.relayoutForSearchStateChange()

    opacity: treeDelegate.matchesSearch ? 1 : 0.3

    // The specific group path THIS row is currently counted as
    // hovering, per sidebarRoot.enterCombineGroupHover()/
    // leaveCombineGroupHover() — captured at enter time and reused
    // at leave time rather than re-reading treeDelegate.
    // combineOutputPath, which can change out from under a hovered
    // row if TreeView recycles this delegate Item to a different
    // file mid-hover (see onPathChanged's own doc comment for why
    // that recycling happens at all).
    property string activeHoverGroupPath: ""

    HoverHandler {
        id: rowHover
        // A placeholder isn't clickable (both TapHandlers are disabled
        // for it), so it must not offer a pointing hand — the cursor is
        // the only thing that would still claim it is.
        cursorShape: treeDelegate.isCombinePlaceholder ? Qt.ArrowCursor : Qt.PointingHandCursor
        // Pushes this row's group into
        // sidebarRoot.hoveredCombineGroup so the ONE per-group merge
        // button (see groupMergeButton) reveals on hovering ANY
        // member row, not just this exact one. Reference-counted
        // (see sidebarRoot.hoveredCombineGroupCounts' doc comment) so
        // moving the pointer directly from one member row to an
        // adjacent one doesn't flicker the button off.
        onHoveredChanged: {
            if (rowHover.hovered && treeDelegate.combineEligible) {
                treeDelegate.activeHoverGroupPath = treeDelegate.combineOutputPath
                sidebarRoot.enterCombineGroupHover(treeDelegate.activeHoverGroupPath)
            } else if (!rowHover.hovered && treeDelegate.activeHoverGroupPath !== "") {
                sidebarRoot.leaveCombineGroupHover(treeDelegate.activeHoverGroupPath)
                treeDelegate.activeHoverGroupPath = ""
            }
            // Hovering a folder lights the folder AND its contents —
            // publish which folder that is, since the descendant rows
            // lighting up can't see this row's hover state themselves
            // (see sidebarRoot.hoveredFolderPath).
            if (treeDelegate.isDir) {
                if (rowHover.hovered)
                    sidebarRoot.enterFolderHover(treeDelegate.path)
                else
                    sidebarRoot.leaveFolderHover(treeDelegate.path)
            }
        }
    }

    // Right-click (or press-and-hold) opens the folder-
    // management context menu for this row — separate TapHandler so
    // it doesn't disturb the left-click open/expand handler below.
    TapHandler {
        id: treeRowContextTap
        acceptedButtons: Qt.RightButton
        // Every entry in that menu (open/rename/move/delete/combine)
        // acts on a file, and a placeholder has none yet.
        enabled: !treeDelegate.isCombinePlaceholder
        onTapped: {
            const p = treeDelegate.mapToItem(sidebarRoot, treeRowContextTap.point.position.x,
                                               treeRowContextTap.point.position.y)
            // "New folder" target: a directory row creates inside
            // itself; a note row creates inside its own parent dir.
            const createIn = treeDelegate.isDir
                ? treeDelegate.path
                : treeDelegate.path.substring(0, treeDelegate.path.lastIndexOf("/"))
            sidebarRoot.openContextMenu(treeDelegate.path, treeDelegate.isDir,
                                     treeDelegate.isAudio || treeDelegate.isOrphanedMd,
                                     treeDelegate.isAudio, treeDelegate.status,
                                     createIn,
                                     treeDelegate.label,
                                     p.x, p.y)
        }
    }

    // A plain click on the name — or anywhere in the empty space to its
    // right, the row's hitbox being far wider than a short name (see
    // tapIsOnRowName) — starts an inline rename: on a note that's
    // already the sole selection (file-manager convention), or on a
    // FOLDER outright. Second way in, alongside the context menu's
    // "Rename".
    //
    // A folder needs no already-selected first click because it has no
    // selected state to reach: directories never enter
    // sidebarRoot.selectedPaths (they aren't openable files), so the
    // note gesture's "click once to select, again to rename" has no
    // first click to offer. Its name-click is unambiguous instead —
    // expand/collapse moved off the row and onto the glyph alone (see
    // tapIsOnExpandGlyph), so a folder's name has no competing job.
    //
    // Decided inside treeRowTap rather than by a TapHandler of its own
    // on rowNameText: overlapping TapHandlers aren't mutually exclusive
    // in Qt Quick, so a tap on the name would fire BOTH and race
    // rename against handleNoteClick (StyleGuide.md "Always-editable
    // text" documents that exact bug). One handler, one decision.
    //
    // Bounded by the name's LAID-OUT width (it Layout.fillWidths to the
    // row's right edge), NOT its painted implicitWidth — the row's
    // hitbox is much wider than a short name's glyphs, and a click in
    // the empty space right of the name must begin the rename too, not
    // just a click on the letters themselves (user report). Still left-
    // bounded at the name's start (local.x >= 0) so the row's left-side
    // status glyphs keep their own space.
    function tapIsOnRowName(pos) {
        if (!rowNameText.visible)
            return false
        const local = rowNameText.mapFromItem(treeDelegate, pos.x, pos.y)
        return local.x >= 0 && local.x <= rowNameText.width
                && local.y >= 0 && local.y <= rowNameText.height
    }

    // Expand/collapse is the GLYPH's gesture, not the row's — that's
    // what frees the rest of the folder row to mean "rename" instead
    // (see tapIsOnRowName, which now claims the name AND the empty space
    // to its right). Putting expand back on the row's dead space is what
    // made the name unclickable in the first place; keeping it on the
    // glyph alone is what lets that dead space rename now.
    //
    // The painted glyph is only 10x11px — too small to ask a real
    // cursor to land inside. So the hit box isn't the glyph's bounds:
    // horizontally it's padded by 4px (which stops exactly at the
    // name's left edge, the RowLayout's own spacing, so the two regions
    // still can't overlap), and vertically it's the WHOLE row. Hence
    // the two coordinate spaces below — x is tested against the glyph,
    // y against the row.
    function tapIsOnExpandGlyph(pos) {
        if (!treeDelegate.isDir)
            return false
        const pad = 4
        const localX = expandGlyph.mapFromItem(treeDelegate, pos.x, pos.y).x
        return localX >= -pad && localX <= expandGlyph.width + pad
                && pos.y >= 0 && pos.y <= treeDelegate.height
    }

    // Toggles this dir row, via whichever of the two expanded-states is
    // actually authoritative for it. An empty folder must NOT go
    // through toggleExpanded(): TreeView won't expand a childless node,
    // but the attempt still latches treeDelegate.expanded to true
    // permanently. See sidebarRoot.collapsedEmptyDirs.
    function toggleThisDir() {
        if (treeDelegate.hasChildren)
            treeDelegate.treeView.toggleExpanded(treeDelegate.row)
        else
            sidebarRoot.toggleEmptyDirCollapsed(treeDelegate.path)
    }

    TapHandler {
        id: treeRowTap
        // A placeholder has nothing to open, expand, or rename — see
        // isCombinePlaceholder. Inert rather than click-then-fail.
        enabled: !treeDelegate.isCombinePlaceholder
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
            if (sidebarRoot.renamingPath !== "" && sidebarRoot.renamingPath !== treeDelegate.path)
                sidebarRoot.commitActiveRename()
            // Grabs keyboard focus so Up/Down (see the Shortcut
            // items near Ctrl+F's) can move between notes right
            // after a click — never touches sidebarRoot.currentPath, so
            // the persistent "currently open" outline is
            // unaffected; clicking into MainPanel afterward moves
            // activeFocus away exactly as it always has.
            treeDelegate.treeView.forceActiveFocus()
            const plainTap = (treeRowTap.point.modifiers
                              & (Qt.ShiftModifier | Qt.ControlModifier)) === 0
            if (treeDelegate.isDir) {
                // Glyph expands, name renames, everything else on the
                // row does nothing — see tapIsOnExpandGlyph.
                if (treeDelegate.tapIsOnExpandGlyph(treeRowTap.point.position))
                    treeDelegate.toggleThisDir()
                else if (plainTap
                         && treeDelegate.tapIsOnRowName(treeRowTap.point.position)) {
                    sidebarRoot.beginRename(treeDelegate.path, true, "")
                    sidebarRoot.renamingCaretScenePos = treeRowTap.point.scenePosition
                }
            } else if (treeDelegate.isAudio || treeDelegate.isOrphanedMd) {
                // Click the name of a row that's ALREADY the sole
                // selection -> rename it, rather than re-opening the
                // file it already opened on the first click. Requires
                // no modifiers and a selection of exactly this row, so
                // it can never fire while a Shift/Ctrl multi-selection
                // is being built.
                const soleSelection = sidebarRoot.selectedPaths.length === 1
                        && sidebarRoot.selectedPaths[0] === treeDelegate.path
                // Renaming the OPEN file could need an extra
                // click. A background STRUCTURE-change rebuild (an import,
                // USB grab, combine, or delete landing another file while
                // you work — VaultTreeModel::rebuild only resets on a
                // changed file set, not on a transcript rewrite) runs
                // onModelReset, which clears selectedPaths — but NOT
                // currentPath. If that reset lands between the
                // select-click and the name-click, the soleSelection
                // precondition is gone and the next click only re-selects
                // instead of renaming. currentPath survives the reset, so
                // honor it: the open file stays renameable in one
                // name-click. Gated on an EMPTY selection (exactly the
                // post-reset state) so this changes nothing when any file
                // is actually selected.
                const openFileNoSelection = sidebarRoot.selectedPaths.length === 0
                        && treeDelegate.path === sidebarRoot.currentPath
                if (plainTap && (soleSelection || openFileNoSelection)
                        && treeDelegate.tapIsOnRowName(treeRowTap.point.position)) {
                    sidebarRoot.beginRename(treeDelegate.path, false, treeDelegate.label)
                    sidebarRoot.renamingCaretScenePos = treeRowTap.point.scenePosition
                    return
                }
                // Shift/Ctrl-click multi-select — see
                // sidebarRoot.handleNoteClick's doc comment. modifiers
                // comes from this TapHandler's own point (Qt
                // doesn't pass them as an onTapped argument).
                sidebarRoot.handleNoteClick(treeDelegate.path, treeDelegate.isOrphanedMd,
                                         treeRowTap.point.modifiers)
            }
        }
    }

    // Drag-to-move: press-and-drag this row to move it into a
    // folder. All the state (including WHICH paths are moving) lives on
    // the sidebarRoot — see sidebarRoot.beginRowDrag's doc comment.
    DragHandler {
        id: rowDragHandler
        // The row itself never moves — rowDragGhost follows the cursor
        // instead, so the treeRoot's own layout stays untouched and there's
        // no torn-out row to put back if the drag is abandoned.
        target: null
        acceptedButtons: Qt.LeftButton
        // Disarmed while THIS row is being inline-renamed, so a press-drag
        // inside the rename field selects text instead of starting a
        // file-move drag (the handler's default grabPermissions would
        // otherwise steal the TextInput's selection grab). Mirrors the
        // !renamingThisRow gate already used on the row's name element.
        enabled: (treeDelegate.isDir || treeDelegate.isAudio || treeDelegate.isOrphanedMd)
                 && !treeDelegate.renamingThisRow
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
        // the viewport, and the treeRoot can't scroll while this handler
        // has the pointer. (Plain clicks are unaffected — treeRowTap's
        // default DragThreshold gesturePolicy gives ITS grab up once
        // the pointer moves, so click-to-open still wins for a click.)
        onActiveChanged: {
            if (rowDragHandler.active) {
                sidebarRoot.beginRowDrag(treeDelegate.path, treeDelegate.isDir,
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
            sidebarRoot.endRowDrag(rowDragHandler.centroid.pressedButtons === Qt.NoButton)
        }
        onCentroidChanged: {
            if (rowDragHandler.active)
                sidebarRoot.updateRowDrag(rowDragHandler.centroid.scenePosition)
        }
    }

    // Drag-to-move: every row is a drop target. A directory
    // row takes the drop itself; a note row forwards it to the note's
    // own parent directory, so dropping onto a file means "into the
    // folder that file lives in" (standard file-manager behavior)
    // rather than being a dead zone the user has to aim around.
    DropArea {
        id: rowDropTarget
        anchors.fill: parent
        // Matches rowDragGhost's Drag.keys — scopes this target to the
        // sidebarRoot's own row drags, so Main.qml's whole-window
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
            if (!sidebarRoot.canDropRowsOn(rowDropTarget.destDir)) {
                drag.accepted = false
                return
            }
            sidebarRoot.rowDragDest = rowDropTarget.destDir
        }
        onExited: {
            // Guarded because sibling rows in the same folder share a
            // destDir: Qt delivers the old target's exit before the new
            // target's enter, so an unguarded clear here would blank the
            // destination the row we just moved onto had already set.
            if (sidebarRoot.rowDragDest === rowDropTarget.destDir)
                sidebarRoot.rowDragDest = ""
        }
        onDropped: {
            const paths = sidebarRoot.rowDragPaths.slice()
            const isDir = sidebarRoot.rowDragIsDir
            const dest = rowDropTarget.destDir
            // Deferred — see performRowMove's doc comment (the move
            // rebuilds the model, destroying this very delegate).
            Qt.callLater(() => sidebarRoot.performRowMove(paths, isDir, dest))
        }
    }

    // Drag-to-move: the fill for the folder a drop would land
    // in — covering the destination folder AND every row inside it (see
    // treeDelegate.inFolderHighlight), so the drop's whole landing zone
    // is what lights up. Keyed off sidebarRoot.rowDragDest rather than this
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
        visible: sidebarRoot.rowDragActive && treeDelegate.inFolderHighlight
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
        strokeColor: sidebarRoot.rowDragActive ? AppSettings.highlightColor : AppSettings.accentColor
        // Matches the sibling combine outline's weight, and for the
        // same reason — see its own strokeWidth comment below.
        strokeWidth: 1.5
        cornerRadius: AppSettings.borderRadius
        topRounded: !treeDelegate.folderHighlightTopMerged
        bottomRounded: !treeDelegate.folderHighlightBottomMerged
    }

    // Note: no anchors.rightMargin here — treeRoot.columnWidthProvider
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
        active: rowHover.hovered && !sidebarRoot.rowDragActive
                && !treeDelegate.inFolderHighlight
    }

    // Persistent highlight for whatever MainPanel currently has
    // open, distinct from HoverBorder above (which is hover-only and
    // white) so both can be visible at once without being confused.
    Rectangle {
        anchors.fill: parent
        anchors.topMargin: 1
        anchors.bottomMargin: 1
        anchors.leftMargin: AppSettings.sidebarEdgeLeftMargin
        visible: treeDelegate.path === sidebarRoot.currentPath
        color: AppSettings.sidebarSelectionColor
        radius: AppSettings.borderRadius
        border.width: 1
        border.color: AppSettings.sidebarSelectionBorderColor
    }

    // Multi-select highlight — border-only (not a fill, so
    // it composes with the "currently open" rectangle above
    // instead of competing with it) and only shown once a genuine
    // multi-selection exists (2+ rows), since a lone selected row is
    // already the row the open-file highlight marks. Reuses
    // AppSettings.highlightColor (the same color already used for
    // search-match spans) rather than adding a new configurable
    // color for what's a secondary affordance.
    Rectangle {
        anchors.fill: parent
        anchors.topMargin: 1
        anchors.bottomMargin: 1
        anchors.leftMargin: AppSettings.sidebarEdgeLeftMargin
        visible: sidebarRoot.selectedPaths.length > 1 && sidebarRoot.selectedPaths.indexOf(treeDelegate.path) >= 0
        color: "transparent"
        radius: AppSettings.borderRadius
        border.width: 1
        border.color: AppSettings.highlightColor
    }

    // Border-only (not a fill, so it composes with the
    // selection highlight above instead of covering it) highlight for
    // rows that are members of a "ready" — would-combine-if-run —
    // split-recording group, colored per-group (TextFormat.combineGroupColor),
    // same stroke weight and corner radius as HoverBorder. A
    // contiguous run of same-group rows suppresses the edges they
    // SHARE with a same-group neighbor (combineTopMerged/
    // combineBottomMerged — see CombineGroupOutline's doc comment for
    // why a plain bordered Rectangle can't do this) so the run reads
    // as one continuous rounded outline instead of N stacked boxes.
    CombineGroupOutline {
        anchors.fill: parent
        // Horizontal inset matches HoverBorder's (breathing room from
        // the sidebarRoot's own left/right edges), but the vertical
        // inset is much smaller — unlike HoverBorder (one row at a
        // time), this draws the OUTER top/bottom edge of a whole
        // multi-row merged group, and a 3px inset there read as
        // unwanted padding separating the group from the unrelated
        // rows directly above/below it (user report).
        // No anchors.rightMargin — see the sibling HoverBorder's
        // comment above; treeRoot.columnWidthProvider already accounts
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
                // An empty folder's state comes from the sidebarRoot, NOT
                // from treeDelegate.expanded, which is a one-way latch
                // for childless nodes — see sidebarRoot.collapsedEmptyDirs.
                id: expandGlyph
                objectName: "expandGlyph"
                Layout.alignment: Qt.AlignVCenter
                Layout.preferredWidth: treeDelegate.isDir ? 10 : 0
                name: !treeDelegate.isDir
                      ? ""
                      : (treeDelegate.hasChildren
                         ? (treeDelegate.expanded ? "collapse" : "expand")
                         : (sidebarRoot.isEmptyDirCollapsed(treeDelegate.path) ? "expand" : "collapse"))
                color: Theme.textMuted
                iconSize: 11
            }

            Rectangle {
                Layout.alignment: Qt.AlignVCenter
                visible: treeDelegate.isAudio || treeDelegate.isOrphanedMd
                Layout.preferredWidth: 8
                Layout.preferredHeight: 8
                radius: 4
                color: treeDelegate.status === 2 ? Theme.success
                     : treeDelegate.status === 4 ? "#e67e22"
                     : (treeDelegate.isOrphanedMd ? "#3498db" : "#f1c40f")
            }

            // Separate small dot for an in-progress transcription
            // job (Queued/Active/Paused) — deliberately distinct from
            // the transcribed/untranscribed dot above rather than
            // overloading it, since a Done/Failed job alongside an
            // already-transcribed file would be redundant to show and
            // this dot only ever appears for the 3 "in progress"
            // statuses (Queued/Active/Paused — see the visible: binding
            // below, which gates on 0 <= jobStatus <= 2).
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

            // The in-progress combine row (see
            // treeDelegate.isCombinePlaceholder). Deliberately names no
            // file: the merged file does not exist yet, so there is
            // nothing honest to call it — it says what is happening and
            // to how many parts, and tracks the job's progress.
            RowLayout {
                id: combinePlaceholderRow
                objectName: "combinePlaceholderRow"
                visible: treeDelegate.isCombinePlaceholder
                Layout.fillWidth: true
                Layout.alignment: Qt.AlignVCenter
                spacing: 6

                AppIcon {
                    Layout.alignment: Qt.AlignVCenter
                    name: "combine"
                    // The same blue jobStatusDot uses for an Active job
                    // when it's running; the grey it uses for a waiting
                    // one when it isn't.
                    color: treeDelegate.combineActive ? "#3498db" : "#95a5a6"
                    iconSize: 12
                }

                Text {
                    objectName: "combinePlaceholderLabel"
                    Layout.alignment: Qt.AlignVCenter
                    Layout.fillWidth: true
                    elide: Text.ElideRight
                    color: Theme.textMuted
                    font.pixelSize: 12
                    font.italic: true
                    text: treeDelegate.combineActive
                          ? qsTr("Combining %n recording(s)… %1%", "", treeDelegate.combineSourceCount)
                                .arg(Math.round(treeDelegate.combineProgress * 100))
                          : qsTr("Combining %n recording(s)… (queued)", "",
                                  treeDelegate.combineSourceCount)
                }
            }

            Text {
                id: rowNameText
                objectName: "rowNameText"
                Layout.alignment: Qt.AlignVCenter
                // Hidden only while THIS row is being renamed inline (see
                // treeDelegate.renamingThisRow) —
                // rowRenameField below takes its place in the layout.
                // Gated on sidebarRoot.renamingPath, never on the field's
                // activeFocus (see sidebarRoot.renamingPath's doc comment).
                // A combine placeholder has no name to show and
                // combinePlaceholderRow above stands in its place.
                visible: !treeDelegate.renamingThisRow && !treeDelegate.isCombinePlaceholder
                // sidebarRoot previously always showed the
                // raw filename — AppSettings.useLabelsSidebar lets it
                // show the stored label instead, when one exists. That
                // choice is load-bearing beyond display now: inline
                // rename edits whatever this shows, so with the toggle
                // on, a labelled row renames its LABEL, not its file
                // (see TextFormat.renameInitialText/performRename).
                // When the query matches this text directly,
                // the match is highlighted via rich text.
                //
                // Bug: this Text had no width bound at all, so a long
                // filename (routine for split-recording candidates —
                // exactly the "acme mic 3"-style names combine rules
                // target) ran under the sidebarRoot's scrollbar/edge
                // instead of stopping short of it. Fills whatever
                // width the RowLayout above has left after its other,
                // actually-visible children (see RowLayout's own doc
                // comment) — not shrunk to dodge the hover-only
                // groupMergeButton overlay (declared after this Column,
                // so it already paints on top) rather than reserved
                // layout space, so there's nothing to truncate for
                // even while they're showing (user report: reserving
                // space for them made names truncate the instant the
                // icons faded in, for icons that weren't even always
                // present).
                Layout.fillWidth: true
                // Elide + Text.StyledText is an
                // unsupported/unpredictable combination in Qt
                // Quick's Text (see snippetText's identical fix
                // above) — this Text switches to StyledText
                // whenever a search query is active (for the
                // highlight <span> markup below), so eliding must be
                // OFF exactly then. clip: true is the backstop that
                // still keeps overflow from painting over the
                // per-row icon cluster while elide is off.
                elide: textFormat === Text.PlainText ? Text.ElideRight : Text.ElideNone
                clip: true
                textFormat: sidebarRoot.searchQuery !== "" ? Text.StyledText : Text.PlainText
                text: {
                    const base = (function () {
                        // Anti-flash: while a just-committed rename's
                        // write is still deferred (see
                        // sidebarRoot.optimisticRenamePath), show the new
                        // name instead of the stale model value.
                        if (sidebarRoot.optimisticRenamePath !== ""
                                && sidebarRoot.optimisticRenamePath === treeDelegate.path)
                            return sidebarRoot.optimisticRenameName
                        if (!treeDelegate.isDir && AppSettings.useLabelsSidebar
                                && treeDelegate.label)
                            return treeDelegate.label
                        // Optimistic cross-panel rename: a file rename in
                        // flight for this row (from EITHER panel), still
                        // deferred behind a same-file transcription. Read
                        // pendingRenamesRevision so this re-evaluates when
                        // the store changes (bare invokable ⇒ no dep).
                        // After the label branch ⇒ never disturbs a
                        // label-shown row (a file rename doesn't change it).
                        if (sidebarRoot.transcriptionController
                                && sidebarRoot.transcriptionController.pendingRenamesRevision >= 0) {
                            const stem = sidebarRoot.transcriptionController
                                .pendingRenameStem("file://" + treeDelegate.path)
                            if (stem !== "")
                                return TextFormat.optimisticDisplayName(treeDelegate.path, false, "", stem,
                                                                        AppSettings.useLabelsSidebar,
                                                                        AppSettings.hideExtensionsSidebar)
                        }
                        if (treeDelegate.isDir || !AppSettings.hideExtensionsSidebar)
                            return treeDelegate.name
                        return TextFormat.stripExtension(treeDelegate.name)
                    })()
                    return sidebarRoot.searchQuery !== ""
                            ? TextFormat.highlightMatch(base, sidebarRoot.searchQuery, AppSettings.highlightColor)
                            : base
                }
                color: "white"
            }

            // Inline rename editor — present only for the one row being
            // renamed (sidebarRoot.renamingPath). See that property's doc
            // comment for why this swap is a deliberate, documented
            // exception to StyleGuide.md's "Always-editable text" rule,
            // and why the gate is renamingPath rather than activeFocus.
            TextField {
                id: rowRenameField
                objectName: "rowRenameField"
                Layout.alignment: Qt.AlignVCenter
                Layout.fillWidth: true
                visible: treeDelegate.renamingThisRow
                color: "white"
                font.pixelSize: rowNameText.font.pixelSize
                // padding:0 alone leaves QQC2's 4px leftPadding in place, which
                // shifts the editor's text right of the rowNameText it replaces.
                // Zero the horizontal paddings explicitly so the two align.
                padding: 0
                leftPadding: 0
                rightPadding: 0
                // StyleGuide.md "Text fields": underline background, not
                // the default boxed one.
                background: UnderlineField { focused: rowRenameField.activeFocus }

                // A QQC2 TextField gets no I-beam on its own.
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
                    // Seeded by beginRename() — the sidebarRoot decides what
                    // is being edited (see TextFormat.renameInitialText), this just
                    // shows it.
                    rowRenameField.text = sidebarRoot.renamingText
                    rowRenameField.forceActiveFocus()
                    // A name-CLICK rename drops the caret where the pointer landed
                    // (no selection); a menu/programmatic rename (no click point)
                    // selects the whole name so typing replaces it. positionAt
                    // needs the field laid out with its seeded text, which isn't
                    // true on the tick `visible` flips — so defer a frame.
                    Qt.callLater(rowRenameField.placeCaret)
                }

                function placeCaret() {
                    if (!rowRenameField.visible)
                        return
                    const sp = sidebarRoot.renamingCaretScenePos
                    if (!sp) {
                        rowRenameField.selectAll()
                        return
                    }
                    const local = rowRenameField.mapFromItem(null, sp.x, sp.y)
                    // positionAt clamps to [0, length], so a click past the name's
                    // end lands the caret at the end — the desired behavior for the
                    // row's wide empty right-hand space.
                    rowRenameField.deselect()
                    rowRenameField.cursorPosition = rowRenameField.positionAt(local.x, local.y)
                }

                // Keeps the sidebarRoot's authoritative copy live. onTextEdited
                // (user typing), not onTextChanged — the latter also fires
                // for the programmatic seed above, which would race it.
                onTextEdited: sidebarRoot.renamingText = rowRenameField.text

                onActiveFocusChanged: {
                    if (rowRenameField.activeFocus) {
                        rowRenameField.everFocused = true
                        return
                    }
                    // StyleGuide.md "Always-editable text" commit
                    // convention for a single-line field: commit on blur
                    // as well as on Return. Only reached for a REAL blur
                    // (clicking into MainPanel, say) — a click on another
                    // treeRoot row leaves this field focused, and is committed
                    // by treeRowTap instead.
                    if (rowRenameField.everFocused && sidebarRoot.renamingPath === treeDelegate.path)
                        sidebarRoot.commitActiveRename()
                }

                onAccepted: sidebarRoot.commitActiveRename()
                Keys.onEscapePressed: sidebarRoot.cancelRename()
            }
        }

        // Fixes 4 issues with this row — (1) no
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
                onTapped: sidebarRoot.transcriptMatchActivated(treeDelegate.path, treeDelegate.isOrphanedMd,
                                                            sidebarRoot.searchQuery)
            }
            HoverBorder {
                anchors.fill: parent
                anchors.margins: -2
                active: snippetHover.hovered
            }
        }
    }

    // The combine placeholder's progress track (see
    // treeDelegate.isCombinePlaceholder). Declared here, after
    // treeColumn, so it paints over the row's background layers rather
    // than under them — same reason groupMergeButton below is.
    //
    // A thin bar pinned to the row's bottom edge, not an inline
    // ProgressBar in combinePlaceholderRow: the row is 28px and already
    // carries an icon and a label, and a full-width edge bar reads as
    // "this row is filling up" at a glance without competing for that
    // space. Only ever shown while the job is actually running —
    // combineProgress is 0 while queued, and a permanently-empty track
    // would just read as a stalled one.
    Rectangle {
        objectName: "combineProgressTrack"
        visible: treeDelegate.combineActive
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.leftMargin: AppSettings.sidebarEdgeLeftMargin
        anchors.rightMargin: AppSettings.sidebarRightMargin
        height: 2
        radius: 1
        color: Theme.surface

        Rectangle {
            objectName: "combineProgressFill"
            anchors.left: parent.left
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            width: parent.width * Math.max(0, Math.min(1, treeDelegate.combineProgress))
            // No radius: a 2px-tall sliver anchored inside the already-
            // rounded track — its own corner rounding is imperceptible,
            // and a hardcoded one trips the corner-radius guard.
            color: "#3498db"
        }
    }

    // ONE "combine this group now" button per
    // group, not one per row — it used to live in this row's
    // already-busy per-file action cluster (transcribe/relabel/
    // chapters icons), competing with the row's own hover highlight
    // for attention and only ever appearing while hovering this
    // EXACT row. Now it's decoupled from that cluster entirely:
    // rendered only on the group's TOP row (!combineTopMerged, i.e.
    // the top of the merged outline — user report, 2026-07-13: was
    // the bottom row, moved to the top so it reads next to the
    // group's "first" file) and revealed by hovering ANY member row,
    // via sidebarRoot.hoveredCombineGroup (see rowHover's
    // onHoveredChanged above) — so it reads as "the whole outlined
    // group has one action," not another per-row icon.
    Item {
        id: groupMergeButton
        // Fixed to the row's top-left corner — this button only ever
        // renders on the group's TOP row (see visible below), but the hover that
        // reveals it (sidebarRoot.hoveredCombineGroup) can be triggered
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
        opacity: (!treeDelegate.renamingThisRow
                  && sidebarRoot.hoveredCombineGroup === treeDelegate.combineOutputPath) ? 1 : 0
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
            onTapped: sidebarRoot.transcriptionController.combineGroup(
                          AppSettings.combineRules, sidebarRoot.model.allAudioPaths(),
                          treeDelegate.combineOutputPath, sidebarRoot.vaultPath,
                          AppSettings.deleteOriginalsAfterCombine)
        }
        // Opaque backdrop so the icon stays readable painted over the
        // row's own content (name/status) rather than blending into it.
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
    // not necessarily this treeRoot's display order). Unlike
    // groupMergeButton above, this renders on EVERY member row (the
    // merge action only makes sense once per group, but each file
    // needs its own ordinal) — positioned as the leftmost item of
    // the overlay cluster (groupMergeButton anchors off THIS item's
    // width, to its right) since the ordinal reads more naturally
    // before the action, and gated by the same
    // sidebarRoot.hoveredCombineGroup hover so they appear/disappear
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
        opacity: (!treeDelegate.renamingThisRow
                  && sidebarRoot.hoveredCombineGroup === treeDelegate.combineOutputPath) ? 1 : 0

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
