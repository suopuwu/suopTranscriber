import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SuopTranscriber

Rectangle {
    id: mainPanel

    property url vaultPath
    property string vaultLocalPath: ""
    property AudioPlayer player
    property TranscriptionController transcriptionController
    property VaultTreeModel vaultTreeModel
    property var storedMetadata: ({})
    readonly property bool hasStoredMetadata: Object.keys(mainPanel.storedMetadata).length > 0
    signal openVaultRequested()

    // Text-only view for an orphaned .md (no matching audio) — mutually
    // exclusive with player.source; showAudioFile()/showOrphanedMd() below
    // are the only places that should touch either.
    property string orphanedMdPath: ""
    property var orphanedMdMetadata: ({})

    property bool renamingTitle: false

    property string searchQuery: ""
    property bool searchExpanded: false
    property int searchMatchIndex: -1

    function relativeToVault(fileUrl) {
        const filePath = fileUrl.toString()
        const prefix = "file://" + mainPanel.vaultLocalPath + "/"
        return filePath.startsWith(prefix) ? filePath.slice(prefix.length) : filePath
    }

    function stripExtension(name) {
        const dot = name.lastIndexOf(".")
        return dot > 0 ? name.substring(0, dot) : name
    }

    function refreshMetadata() {
        storedMetadata = (player && player.source.toString() !== "" && transcriptionController)
            ? transcriptionController.readMetadata(player.source) : ({})
        speakerDetectionToggle.checked = mainPanel.hasStoredMetadata && storedMetadata.speakerDetection === true
    }

    function refreshOrphanedMetadata() {
        orphanedMdMetadata = (mainPanel.orphanedMdPath !== "" && mainPanel.transcriptionController)
            ? mainPanel.transcriptionController.readMetadata("file://" + mainPanel.orphanedMdPath) : ({})
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

    function indexForModelId(id) {
        if (!mainPanel.transcriptionController)
            return -1
        const models = mainPanel.transcriptionController.availableModels()
        for (let i = 0; i < models.length; i++) {
            if (models[i].id === id)
                return i
        }
        return -1
    }

    function displayTitle() {
        if (mainPanel.orphanedMdPath !== "") {
            const label = mainPanel.orphanedMdMetadata.label
            if (AppSettings.useLabels && label)
                return label
            const base = mainPanel.orphanedMdPath.substring(mainPanel.orphanedMdPath.lastIndexOf("/") + 1)
            return AppSettings.hideExtensionsMainPanel ? mainPanel.stripExtension(base) : base
        }
        if (!mainPanel.player || mainPanel.player.source.toString() === "")
            return qsTr("No file selected")
        if (AppSettings.useLabels && mainPanel.storedMetadata.label)
            return mainPanel.storedMetadata.label
        const rel = mainPanel.relativeToVault(mainPanel.player.source)
        return AppSettings.hideExtensionsMainPanel ? mainPanel.stripExtension(rel) : rel
    }

    function commitRename(newValue) {
        const trimmed = newValue.trim()
        mainPanel.renamingTitle = false
        if (trimmed === "" || !mainPanel.transcriptionController)
            return

        if (mainPanel.orphanedMdPath !== "") {
            if (AppSettings.useLabels) {
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
            if (AppSettings.useLabels) {
                mainPanel.transcriptionController.setLabel(mainPanel.player.source, trimmed)
                mainPanel.refreshMetadata()
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

    // Item 10: live-updates as segments are written during an in-progress
    // transcription of the currently-open file — reading .progress here
    // (even though the value itself isn't used) makes this binding
    // re-evaluate on every per-segment progress tick, not just at
    // completion. Gated on currentFile matching so unrelated jobs' progress
    // ticks don't cause pointless re-reads.
    readonly property string transcriptBody: {
        if (mainPanel.transcriptionController && mainPanel.player
                && mainPanel.transcriptionController.currentFile.toString() === mainPanel.player.source.toString()) {
            void mainPanel.transcriptionController.progress
        }
        if (mainPanel.orphanedMdPath !== "" && mainPanel.transcriptionController)
            return mainPanel.transcriptionController.readTranscriptBody("file://" + mainPanel.orphanedMdPath)
        if (mainPanel.player && mainPanel.player.source.toString() !== "" && mainPanel.hasStoredMetadata
                && mainPanel.transcriptionController)
            return mainPanel.transcriptionController.readTranscriptBody(mainPanel.player.source)
        return ""
    }

    // Sentence-level highlighting synced to playback (DESIGN.md / CLAUDE.md:
    // sentence-level only, binary-search the active sentence on
    // positionChanged). Orphaned-md files have no player/position, so
    // sentences parse fine but activeSentenceIndex just stays -1 — no
    // highlight, which is correct (nothing is "playing").
    readonly property var sentences: {
        if (!mainPanel.transcriptionController)
            return []
        if (mainPanel.orphanedMdPath !== "")
            return mainPanel.transcriptionController.parseSentences("file://" + mainPanel.orphanedMdPath)
        if (mainPanel.player && mainPanel.player.source.toString() !== "" && mainPanel.hasStoredMetadata)
            return mainPanel.transcriptionController.parseSentences(mainPanel.player.source)
        return []
    }

    // player.position is milliseconds; segment timestamps are seconds.
    // This binding re-evaluates automatically on player.positionChanged
    // (QML property binding dependency tracking) — binary search lives in
    // C++ (activeSentenceIndex), not linear-scanned here, per DESIGN.md.
    readonly property int activeSentenceIndex: {
        if (!mainPanel.transcriptionController || mainPanel.sentences.length === 0)
            return -1
        const positionSeconds = (mainPanel.player ? mainPanel.player.position : 0) / 1000.0
        return mainPanel.transcriptionController.activeSentenceIndex(mainPanel.sentences, positionSeconds)
    }

    function escapeHtml(text) {
        return text.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;")
    }

    // Wraps the active sentence's substring (by its exact start/length into
    // transcriptBody, from parseSentences) in a highlight span. Qt's
    // cursorPosition/select() operate on the rendered plain-text position
    // space regardless of textFormat, and HTML-escaping doesn't change that
    // position count (entities decode back to one character each) — so this
    // coexists with jumpToNextMatch()'s select()-based search untouched.
    function highlightedTranscriptHtml() {
        const body = mainPanel.transcriptBody
        const idx = mainPanel.activeSentenceIndex
        let inner
        if (idx < 0 || idx >= mainPanel.sentences.length) {
            inner = mainPanel.escapeHtml(body)
        } else {
            const active = mainPanel.sentences[idx]
            const before = body.substring(0, active.start)
            const highlighted = body.substring(active.start, active.start + active.length)
            const after = body.substring(active.start + active.length)
            inner = mainPanel.escapeHtml(before)
                + "<span style=\"background-color: #3a5a7a;\">" + mainPanel.escapeHtml(highlighted) + "</span>"
                + mainPanel.escapeHtml(after)
        }
        // white-space:pre-wrap so the transcript's literal newlines/spacing
        // still render as line breaks under RichText — plain HTML would
        // otherwise collapse them, unlike the plain-text mode this replaces.
        return "<div style=\"white-space: pre-wrap;\">" + inner + "</div>"
    }

    function jumpToNextMatch() {
        const body = mainPanel.transcriptBody
        if (mainPanel.searchQuery === "" || body === "")
            return
        const q = mainPanel.searchQuery.toLowerCase()
        const lower = body.toLowerCase()
        let start = mainPanel.searchMatchIndex < 0 ? 0 : mainPanel.searchMatchIndex + 1
        let idx = lower.indexOf(q, start)
        if (idx === -1)
            idx = lower.indexOf(q, 0) // wrap around
        mainPanel.searchMatchIndex = idx
        if (idx !== -1) {
            // select() alone still scrolls the match into view and shows
            // the selection — no need to steal focus from the search field,
            // which would stop repeated Enter presses from working (each
            // jumps to the next match without needing to re-click into it).
            transcriptArea.select(idx, idx + q.length)
        }
    }

    Connections {
        target: mainPanel.player
        function onSourceChanged() { mainPanel.refreshMetadata() }
    }
    Connections {
        target: mainPanel.transcriptionController
        function onFinished() {
            mainPanel.refreshMetadata()
            mainPanel.refreshOrphanedMetadata()
        }
        // Item 10: frontmatter is written at job START (durability, see
        // TranscriptionController), before any segment lines exist — so
        // hasStoredMetadata can flip true well before onFinished fires.
        // Re-checking on every progress tick (for the file currently open)
        // is what lets transcriptBody's binding start reading a live,
        // in-progress file instead of waiting for completion.
        function onProgressChanged() {
            if (mainPanel.player
                    && mainPanel.transcriptionController.currentFile.toString() === mainPanel.player.source.toString())
                mainPanel.refreshMetadata()
        }
    }
    onSearchQueryChanged: mainPanel.searchMatchIndex = -1

    Shortcut {
        sequence: "Ctrl+F"
        enabled: mainPanel.activeFocus && (mainPanel.orphanedMdPath !== ""
                     || (mainPanel.player && mainPanel.player.source.toString() !== ""))
        onActivated: {
            mainPanel.searchExpanded = true
            searchField.forceActiveFocus()
        }
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
        visible: mainPanel.vaultPath.toString() === ""

        Button {
            id: openVaultButton
            anchors.centerIn: parent
            text: qsTr("Open Vault")
            hoverEnabled: true
            onClicked: mainPanel.openVaultRequested()

            background: Rectangle {
                radius: AppSettings.borderRadius
                color: openVaultButton.hovered ? "#3a3a3a" : "#2a2a2a"
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
        visible: mainPanel.vaultPath.toString() !== ""

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
                    color: playPauseHover.hovered ? "#3a3a3a" : "#2a2a2a"
                }
                HoverBorder {
                    anchors.fill: parent
                    active: playPauseButton.enabled && playPauseHover.hovered
                }
                Text {
                    anchors.centerIn: parent
                    // The "▶" glyph's own em-box isn't visually centered on
                    // its triangle (most fonts pad it left) — nudge right so
                    // it looks centered in the button; "⏸" is symmetric and
                    // needs no offset.
                    anchors.horizontalCenterOffset: text === "▶" ? 1 : 0
                    text: mainPanel.player && mainPanel.player.playing ? "⏸" : "▶"
                    color: playPauseButton.enabled ? "white" : "#666666"
                    font.pixelSize: 16
                }
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

                handle: Rectangle {
                    x: seekSlider.leftPadding + seekSlider.visualPosition * (seekSlider.availableWidth - width)
                    y: seekSlider.topPadding + seekSlider.availableHeight / 2 - height / 2
                    implicitWidth: 16
                    implicitHeight: 16
                    radius: 8
                    color: seekSlider.pressed ? "#dddddd" : "#ffffff"
                    border.width: 1
                    border.color: "#888888"

                    Rectangle {
                        anchors.centerIn: parent
                        width: parent.width + 2
                        height: parent.height + 2
                        radius: width / 2
                        color: "transparent"
                        border.width: 1
                        border.color: "white"
                        opacity: seekSlider.hovered ? 1 : 0
                        Behavior on opacity {
                            NumberAnimation { duration: 100 }
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
                implicitHeight: Math.max(titleLabel.implicitHeight, titleField.implicitHeight)

                Label {
                    id: titleLabel
                    anchors.fill: parent
                    visible: !mainPanel.renamingTitle
                    elide: Text.ElideMiddle
                    text: mainPanel.displayTitle()
                    color: "white"
                    font.pixelSize: 14

                    HoverHandler {
                        id: titleHover
                        cursorShape: Qt.PointingHandCursor
                    }
                    TapHandler {
                        onTapped: {
                            titleField.text = mainPanel.displayTitle()
                            mainPanel.renamingTitle = true
                            titleField.forceActiveFocus()
                            titleField.selectAll()
                        }
                    }
                    HoverBorder {
                        anchors.fill: parent
                        active: titleHover.hovered
                    }
                }

                TextField {
                    id: titleField
                    anchors.fill: parent
                    visible: mainPanel.renamingTitle
                    onAccepted: mainPanel.commitRename(text)
                    onActiveFocusChanged: if (!activeFocus && mainPanel.renamingTitle) mainPanel.commitRename(text)

                    Keys.onEscapePressed: mainPanel.renamingTitle = false
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
                enabled: visible && !mainPanel.transcriptionController.isQueued(regenerateLabelButton.targetUrl)

                ToolTip.visible: regenerateLabelHover.hovered
                ToolTip.text: qsTr("Regenerate auto-title")
                ToolTip.delay: 500

                HoverHandler {
                    id: regenerateLabelHover
                    cursorShape: Qt.PointingHandCursor
                    enabled: regenerateLabelButton.enabled
                }
                TapHandler {
                    enabled: regenerateLabelButton.enabled
                    onTapped: mainPanel.transcriptionController.relabel(regenerateLabelButton.targetUrl,
                                                                         mainPanel.vaultPath)
                }
                HoverBorder {
                    anchors.fill: parent
                    active: regenerateLabelButton.enabled && regenerateLabelHover.hovered
                }
                Text {
                    anchors.centerIn: parent
                    text: "↻"
                    color: regenerateLabelButton.enabled ? "#dddddd" : "#666666"
                    font.pixelSize: 14
                }
            }

            // Item 18: search icon → expands to a text field, Ctrl+F while
            // the main panel has focus. Searches title/tags (via
            // storedMetadata, already loaded) + the transcript body
            // (jumpToNextMatch() selects/scrolls the next match below).
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
                Text {
                    anchors.centerIn: parent
                    text: "⌕"
                    color: "#dddddd"
                    font.pixelSize: 15
                }
            }

            TextField {
                id: searchField
                visible: mainPanel.searchExpanded
                Layout.preferredWidth: 200
                placeholderText: qsTr("Search transcript, title, tags…")
                text: mainPanel.searchQuery
                onTextChanged: mainPanel.searchQuery = text
                onAccepted: mainPanel.jumpToNextMatch()

                HoverHandler {
                    cursorShape: Qt.IBeamCursor
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

        Label {
            Layout.fillWidth: true
            visible: mainPanel.orphanedMdPath === "" && (!mainPanel.player || mainPanel.player.source.toString() === "")
            text: qsTr("No file selected")
            color: "white"
        }

        RowLayout {
            spacing: 12
            visible: mainPanel.orphanedMdPath === "" && mainPanel.hasStoredMetadata

            Label {
                text: qsTr("Previously transcribed — model: %1, speaker detection: %2")
                      .arg(mainPanel.storedMetadata.model || "?")
                      .arg(mainPanel.storedMetadata.speakerDetection ? qsTr("on") : qsTr("off"))
                color: "#aaaaaa"
                font.pixelSize: 11
            }
        }

        RowLayout {
            spacing: 12
            visible: mainPanel.orphanedMdPath === ""

            CheckBox {
                id: speakerDetectionToggle
                text: qsTr("Speaker detection")

                HoverHandler {
                    cursorShape: Qt.PointingHandCursor
                }
            }

            Label {
                text: qsTr("Model")
                color: "#aaaaaa"
            }

            ComboBox {
                id: modelCombo
                Layout.preferredWidth: 240
                model: mainPanel.transcriptionController ? mainPanel.transcriptionController.availableModels() : []
                textRole: "label"
                valueRole: "id"
                enabled: !speakerDetectionToggle.checked
                hoverEnabled: true

                Component.onCompleted: currentIndex = mainPanel.indexForModelId(AppSettings.lastModel)
                onActivated: AppSettings.lastModel = currentValue

                ToolTip.visible: modelCombo.hovered
                ToolTip.text: modelCombo.currentText
                ToolTip.delay: 500

                HoverHandler {
                    cursorShape: Qt.PointingHandCursor
                    enabled: modelCombo.enabled
                }
                HoverBorder {
                    anchors.fill: parent
                    active: modelCombo.hovered
                }
            }

            Label {
                visible: speakerDetectionToggle.checked
                text: qsTr("(speaker detection requires small.en-tdrz)")
                color: "#888888"
                font.pixelSize: 11
            }
        }

        RowLayout {
            spacing: 12
            visible: mainPanel.orphanedMdPath === ""

            Button {
                id: transcribeButton

                readonly property bool isActive: mainPanel.transcriptionController && mainPanel.player
                    && mainPanel.transcriptionController.busy
                    && mainPanel.transcriptionController.currentFile.toString() === mainPanel.player.source.toString()
                readonly property bool isQueued: mainPanel.transcriptionController && mainPanel.player
                    && mainPanel.player.source.toString() !== ""
                    && mainPanel.transcriptionController.queuedFileUrls.some(
                           u => u.toString() === mainPanel.player.source.toString())

                text: transcribeButton.isActive ? qsTr("Transcribing…")
                      : transcribeButton.isQueued ? qsTr("Queued…")
                      : (mainPanel.transcriptionController && mainPanel.transcriptionController.busy
                         ? qsTr("Add to queue") : qsTr("Transcribe"))
                enabled: mainPanel.player && mainPanel.player.source.toString() !== ""
                         && mainPanel.transcriptionController
                         && !transcribeButton.isActive && !transcribeButton.isQueued
                         && (!mainPanel.hasStoredMetadata
                             || speakerDetectionToggle.checked !== (mainPanel.storedMetadata.speakerDetection === true))
                hoverEnabled: true
                onClicked: mainPanel.transcriptionController.transcribe(mainPanel.player.source, mainPanel.vaultPath,
                                                                         modelCombo.currentValue,
                                                                         speakerDetectionToggle.checked,
                                                                         AppSettings.autoLabelEnabled)

                background: Rectangle {
                    radius: AppSettings.borderRadius
                    color: transcribeButton.hovered ? "#3a3a3a" : "#2a2a2a"
                    opacity: transcribeButton.enabled ? 1 : 0.5
                    border.width: 1
                    border.color: "#3a3a3a"
                }
                HoverHandler {
                    cursorShape: Qt.PointingHandCursor
                    enabled: transcribeButton.enabled
                }
                HoverBorder {
                    anchors.fill: parent
                    active: transcribeButton.hovered
                }
            }
        }

        ScrollView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            visible: mainPanel.transcriptBody !== ""
            ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

            TextArea {
                id: transcriptArea
                readOnly: true
                wrapMode: TextArea.Wrap
                textFormat: TextEdit.RichText
                text: mainPanel.highlightedTranscriptHtml()
                color: "#dddddd"
                background: null
            }
        }

        Item {
            Layout.fillHeight: true
            visible: mainPanel.transcriptBody === ""
        }
    }
}
