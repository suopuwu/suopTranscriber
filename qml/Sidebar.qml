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
        function onModelReset() { sidebar.refreshSearchResults() }
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
        function onFinished(fileUrl, ok, error) { sidebar.metadataRevision += 1 }
        function onJobStatusesChanged() { sidebar.jobStatusRevision += 1 }
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

    Item {
        id: ribbon
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        // Item 17 (Wave E): a 2nd row (tag filter input) appears below the
        // search row while expanded, so this needs to grow to fit both.
        // O12: a 3rd, variable-height row (wrapping tag-filter chips) grows
        // this further still, only while at least one tag filter is active.
        height: sidebar.searchExpanded ? (64 + (tagChipsFlow.visible ? tagChipsFlow.height + 4 : 0)) : 32

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

                    Behavior on color { ColorAnimation { duration: 100 } }
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

        delegate: Item {
            id: treeDelegate
            implicitWidth: tree.width
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

            Component.onCompleted: {
                sidebar.setBulkEligible(treeDelegate.path, treeDelegate.bulkEligible)
                sidebar.setRelabelEligible(treeDelegate.path, treeDelegate.relabelEligible)
                treeDelegate.refreshMetadata()
                treeDelegate.refreshJobStatus()
            }
            Component.onDestruction: {
                sidebar.setBulkEligible(treeDelegate.path, false)
                sidebar.setRelabelEligible(treeDelegate.path, false)
                sidebar.setJobStatus(treeDelegate.path, -1)
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
            // readMetadata() resolves <completeBaseName>.md regardless of
            // whether the given path is the audio file or the .md itself
            // (self-referential for an orphaned .md), so this covers both.
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

            HoverHandler {
                id: rowHover
                cursorShape: Qt.PointingHandCursor
            }

            TapHandler {
                onTapped: {
                    if (treeDelegate.isDir) {
                        treeDelegate.treeView.toggleExpanded(treeDelegate.row)
                    } else if (treeDelegate.isAudio) {
                        sidebar.fileActivated(treeDelegate.path)
                    } else if (treeDelegate.isOrphanedMd) {
                        sidebar.orphanedMdActivated(treeDelegate.path)
                    }
                }
            }

            HoverBorder {
                anchors.fill: parent
                anchors.margins: 1
                active: rowHover.hovered
            }

            // O7: persistent highlight for whatever MainPanel currently has
            // open, distinct from HoverBorder above (which is hover-only and
            // white) so both can be visible at once without being confused.
            Rectangle {
                anchors.fill: parent
                anchors.margins: 1
                visible: treeDelegate.path === sidebar.currentPath
                color: AppSettings.sidebarSelectionColor
                radius: AppSettings.borderRadius
                border.width: 1
                border.color: AppSettings.sidebarSelectionBorderColor
            }

            Column {
                id: treeColumn
                x: treeDelegate.depth * 16 + 6
                // See the identical fix + explanation on searchResultsView's
                // resultColumn in this same file — conditionally toggling
                // between anchors.top and anchors.verticalCenter via
                // ternary+undefined is fragile and was observed live to get
                // stuck at the wrong position; a plain "y" binding avoids
                // the whole class of bug.
                y: treeDelegate.showSnippet ? 4 : (treeDelegate.height - treeColumn.implicitHeight) / 2
                spacing: 2

                Row {
                    spacing: 6

                    Text {
                        // Fixed-width wrapper: always reserves space so file
                        // rows and dir rows stay aligned, even though the
                        // glyph itself only renders for expandable dirs.
                        anchors.verticalCenter: parent.verticalCenter
                        width: 14
                        horizontalAlignment: Text.AlignHCenter
                        text: (treeDelegate.isDir && treeDelegate.hasChildren)
                              ? (treeDelegate.expanded ? "−" : "+")
                              : ""
                        color: "#aaaaaa"
                    }

                    Rectangle {
                        anchors.verticalCenter: parent.verticalCenter
                        visible: treeDelegate.isAudio || treeDelegate.isOrphanedMd
                        width: 8
                        height: 8
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
                        anchors.verticalCenter: parent.verticalCenter
                        visible: treeDelegate.isAudio && treeDelegate.jobStatus >= 0 && treeDelegate.jobStatus <= 2
                        width: 6
                        height: 6
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
                        anchors.verticalCenter: parent.verticalCenter
                        // Item 1 (Wave D): sidebar previously always showed the
                        // raw filename — AppSettings.useLabelsSidebar (a
                        // display-only toggle; Sidebar has no rename UI, unlike
                        // MainPanel's useLabelsMainPanel) now lets it show the
                        // stored label instead, when one exists.
                        // Item 9: when the query matches this text directly,
                        // the match is highlighted via rich text.
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
                // matches the title Text's own left edge (glyph column +
                // spacing + status dot + spacing = 34px), instead of
                // starting flush at the row's true left edge like before.
                Text {
                    id: snippetText
                    visible: treeDelegate.showSnippet
                    x: 34
                    width: Math.max(10, tree.width - x - 16)
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

            Item {
                id: rowTranscribeButton
                anchors.right: parent.right
                anchors.rightMargin: 8
                anchors.verticalCenter: parent.verticalCenter
                width: 20
                height: 20
                // Always available for any audio row, transcribed or not — a
                // manual, explicit re-run should never be blocked (only
                // "transcribe all"/bulkEligible above skips already-current
                // files; this per-row icon is a deliberate single-file
                // request, so it always fires).
                opacity: (rowHover.hovered && treeDelegate.isAudio) ? 1 : 0
                enabled: opacity > 0

                Behavior on opacity {
                    NumberAnimation { duration: 120 }
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
                                  "file://" + treeDelegate.path, sidebar.vaultPath, AppSettings.defaultModel, false,
                                  AppSettings.autoLabelEnabled, AppSettings.restrictTagsToExisting,
                                  AppSettings.tagReuseBias, AppSettings.wordiness)
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

            // Wave D item 7: per-row "regenerate auto-title", same fade-in
            // pattern as rowTranscribeButton but for already-transcribed rows.
            // The transcribe icon is now always shown for any audio row (see
            // above), so this always sits directly to its left — no more
            // "invisible transcribe button leaves a gap" case to special-case.
            Item {
                id: rowRelabelButton
                anchors.right: rowTranscribeButton.left
                anchors.rightMargin: 4
                anchors.verticalCenter: parent.verticalCenter
                width: 20
                height: 20
                opacity: (rowHover.hovered && treeDelegate.relabelEligible) ? 1 : 0
                enabled: opacity > 0

                Behavior on opacity {
                    NumberAnimation { duration: 120 }
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
                    onTapped: sidebar.transcriptionController.relabel("file://" + treeDelegate.path, sidebar.vaultPath,
                                                                       AppSettings.restrictTagsToExisting,
                                                                       AppSettings.tagReuseBias, AppSettings.wordiness)
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
            width: searchResultsView.width

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
                    // leaves, never directories).
                    if (resultDelegate.resultIsAudio)
                        sidebar.fileActivated(resultDelegate.resultPath)
                    else if (resultDelegate.resultIsOrphanedMd)
                        sidebar.orphanedMdActivated(resultDelegate.resultPath)
                }
            }
            HoverBorder {
                anchors.fill: parent
                anchors.margins: 1
                active: resultHover.hovered
            }

            // O7: same persistent "currently open" highlight as treeDelegate.
            Rectangle {
                anchors.fill: parent
                anchors.margins: 1
                visible: resultDelegate.resultPath === sidebar.currentPath
                color: AppSettings.sidebarSelectionColor
                radius: AppSettings.borderRadius
                border.width: 1
                border.color: AppSettings.sidebarSelectionBorderColor
            }

            Column {
                id: resultColumn
                x: 6
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
                    width: Math.max(10, searchResultsView.width - x - 16)
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
