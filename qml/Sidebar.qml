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
    // sidebar as a direct id, no signal roundtrip needed) — replaces any
    // existing filter with just this one tag and opens the search area so
    // the active filter is visible.
    function applyTagFilter(tag) {
        sidebar.searchExpanded = true
        sidebar.tagFilterText = tag
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
        searchQuery: sidebar.searchQuery
        includeTranscriptions: sidebar.searchIncludeTranscriptions
        tagFilter: sidebar.activeTagFilters
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
        height: sidebar.searchExpanded ? 64 : 32

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
                        sidebar.transcriptionController.transcribeAll(urls, sidebar.vaultPath, AppSettings.lastModel,
                                                                        false, AppSettings.autoLabelEnabled,
                                                                        AppSettings.restrictTagsToExisting,
                                                                        AppSettings.tagReuseBias)
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
                                                                     AppSettings.tagReuseBias)
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
        anchors.top: ribbon.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        clip: true

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
            // Bulk always requests AppSettings.lastModel with
            // speakerDetection=false (see transcribeAllButton's TapHandler).
            readonly property bool bulkEligible: treeDelegate.isAudio && sidebar.transcriptionController
                && !sidebar.transcriptionController.matchesExistingTranscription(
                       "file://" + treeDelegate.path, AppSettings.lastModel, false)
            onBulkEligibleChanged: sidebar.setBulkEligible(treeDelegate.path, treeDelegate.bulkEligible)

            // Wave D item 7: inverse of bulkEligible — already-transcribed
            // audio files, for the per-row + bulk "regenerate auto-title".
            readonly property bool relabelEligible: treeDelegate.isAudio && treeDelegate.status === 2
            onRelabelEligibleChanged: sidebar.setRelabelEligible(treeDelegate.path, treeDelegate.relabelEligible)

            Component.onCompleted: {
                sidebar.setBulkEligible(treeDelegate.path, treeDelegate.bulkEligible)
                sidebar.setRelabelEligible(treeDelegate.path, treeDelegate.relabelEligible)
            }
            Component.onDestruction: {
                sidebar.setBulkEligible(treeDelegate.path, false)
                sidebar.setRelabelEligible(treeDelegate.path, false)
            }

            // readMetadata() resolves <completeBaseName>.md regardless of
            // whether the given path is the audio file or the .md itself
            // (self-referential for an orphaned .md), so this covers both.
            readonly property var metadata: ((treeDelegate.isAudio || treeDelegate.isOrphanedMd)
                                              && sidebar.transcriptionController)
                ? sidebar.transcriptionController.readMetadata("file://" + treeDelegate.path) : ({})

            // Item 9: ellipsis-bounded transcript snippet around the first
            // match, shown only when there's an active query, transcript
            // search is enabled, and this row's body actually contains it —
            // rows that only matched by name/label/tags show no snippet.
            readonly property string cleanedBody: (sidebar.searchIncludeTranscriptions && treeDelegate.transcriptBody)
                ? sidebar.cleanTranscript(treeDelegate.transcriptBody) : ""
            readonly property string snippetText: (sidebar.searchQuery !== "" && treeDelegate.cleanedBody !== "")
                ? sidebar.transcriptSnippet(treeDelegate.cleanedBody, sidebar.searchQuery, 40) : ""
            readonly property bool showSnippet: treeDelegate.snippetText !== ""

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

            Column {
                x: treeDelegate.depth * 16 + 6
                anchors.verticalCenter: treeDelegate.showSnippet ? undefined : parent.verticalCenter
                anchors.top: treeDelegate.showSnippet ? parent.top : undefined
                anchors.topMargin: treeDelegate.showSnippet ? 4 : 0
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
                                  "file://" + treeDelegate.path, sidebar.vaultPath, AppSettings.lastModel, false,
                                  AppSettings.autoLabelEnabled, AppSettings.restrictTagsToExisting,
                                  AppSettings.tagReuseBias)
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
                                                                       AppSettings.tagReuseBias)
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
}
