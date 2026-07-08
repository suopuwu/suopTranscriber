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
    // Item 16 (Wave E): a tag pill was clicked — Main.qml routes this to
    // sidebar.applyTagFilter(tag).
    signal tagActivated(string tag)

    // Text-only view for an orphaned .md (no matching audio) — mutually
    // exclusive with player.source; showAudioFile()/showOrphanedMd() below
    // are the only places that should touch either.
    property string orphanedMdPath: ""
    property var orphanedMdMetadata: ({})

    property bool renamingTitle: false

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

    // Item 13: tags are comma-separated (see TranscriptionController's
    // frontmatter format — not a YAML list), so split+trim+drop-empties here.
    function currentTags() {
        const metadata = mainPanel.orphanedMdPath !== "" ? mainPanel.orphanedMdMetadata : mainPanel.storedMetadata
        const raw = metadata && metadata.tags ? String(metadata.tags) : ""
        if (raw === "")
            return []
        return raw.split(",").map(t => t.trim()).filter(t => t.length > 0)
    }

    // Item 11 (Wave E): speakerNames is a flat "id=name, id=name" frontmatter
    // string (same no-nesting convention as tags) — id is always the STABLE
    // "Speaker N" form (see TranscriptionController::parseSentences' "speaker"
    // field), name is the user-chosen display name.
    function currentSpeakerNames() {
        const metadata = mainPanel.orphanedMdPath !== "" ? mainPanel.orphanedMdMetadata : mainPanel.storedMetadata
        const raw = metadata && metadata.speakerNames ? String(metadata.speakerNames) : ""
        const map = ({})
        if (raw === "")
            return map
        for (const pair of raw.split(",")) {
            const eq = pair.indexOf("=")
            if (eq <= 0)
                continue
            const id = pair.substring(0, eq).trim()
            const name = pair.substring(eq + 1).trim()
            if (id !== "" && name !== "")
                map[id] = name
        }
        return map
    }

    function displaySpeakerName(id) {
        const map = mainPanel.currentSpeakerNames()
        return (id in map) ? map[id] : id
    }

    // Distinct "Speaker N" ids actually present in THIS transcript, in
    // first-appearance order — not every id that's ever appeared in the
    // vault, and not shown at all for a transcript with no speaker
    // detection data (sentences[i].speaker is 0 in that case).
    readonly property var distinctSpeakerIds: {
        const ids = []
        const seen = ({})
        for (const s of mainPanel.sentences) {
            if (s.speaker && s.speaker > 0) {
                const id = "Speaker " + s.speaker
                if (!seen[id]) {
                    seen[id] = true
                    ids.push(id)
                }
            }
        }
        return ids
    }

    // Which speaker pill (if any) is currently showing its rename TextField
    // — mirrors renamingTitle's click-to-rename pattern, just keyed by id
    // since there can be several pills.
    property string renamingSpeakerId: ""

    function commitSpeakerRename(speakerId, newValue) {
        const trimmed = newValue.trim()
        mainPanel.renamingSpeakerId = ""
        if (trimmed === "" || !mainPanel.transcriptionController)
            return
        const targetUrl = mainPanel.orphanedMdPath !== "" ? ("file://" + mainPanel.orphanedMdPath)
                                                           : mainPanel.player.source
        mainPanel.transcriptionController.renameSpeaker(targetUrl, speakerId, trimmed)
        if (mainPanel.orphanedMdPath !== "")
            mainPanel.refreshOrphanedMetadata()
        else
            mainPanel.refreshMetadata()
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
        mainPanel.renamingTitle = false
        if (trimmed === "" || !mainPanel.transcriptionController)
            return

        if (mainPanel.orphanedMdPath !== "") {
            if (AppSettings.useLabelsMainPanel) {
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
    //
    // Same live-update hook as transcriptBody above (read .progress so this
    // re-evaluates every per-segment tick, not just at completion) — this
    // was missing here, which is why displayText/the transcript view (both
    // now built FROM sentences, not from transcriptBody directly) didn't
    // show new segments live during an in-progress transcription: nothing
    // else reliably re-triggers this binding mid-job (storedMetadata's
    // reference changes, but hasStoredMetadata's VALUE stays true the whole
    // time once frontmatter exists, so its own change notify doesn't fire
    // on every tick).
    readonly property var sentences: {
        if (mainPanel.transcriptionController && mainPanel.player
                && mainPanel.transcriptionController.currentFile.toString() === mainPanel.player.source.toString()) {
            void mainPanel.transcriptionController.progress
        }
        if (!mainPanel.transcriptionController)
            return []
        if (mainPanel.orphanedMdPath !== "")
            return mainPanel.transcriptionController.parseSentences("file://" + mainPanel.orphanedMdPath)
        if (mainPanel.player && mainPanel.player.source.toString() !== "" && mainPanel.hasStoredMetadata)
            return mainPanel.transcriptionController.parseSentences(mainPanel.player.source)
        return []
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
    // with no speaker detection (speaker is always 0) have no "turns" to
    // group by, so every sentence counts as its own section there.
    readonly property var displayData: {
        let text = ""
        const list = []
        let previousSpeaker = null
        for (const s of mainPanel.sentences) {
            // Skip blank/whitespace-only segments entirely — whisper can
            // occasionally emit one (e.g. a near-silent stretch), and giving
            // it its own empty line + gutter timestamp is exactly the "one
            // timestamp with nothing under it" pattern that reads as a
            // gutter glitch. A blank segment has no content to show, so it
            // shouldn't get a line OR a timestamp.
            if (s.text.trim() === "")
                continue
            const start = text.length
            const showGutter = (s.speaker === 0) || (previousSpeaker === null) || (s.speaker !== previousSpeaker)
            list.push({t0: s.t0, speaker: s.speaker, text: s.text, start: start, length: s.text.length,
                       showGutter: showGutter})
            previousSpeaker = s.speaker
            text += s.text + "\n"
        }
        if (text.length > 0)
            text = text.slice(0, -1) // drop the final trailing newline
        return {text: text, sentences: list}
    }
    readonly property string displayText: mainPanel.displayData.text
    readonly property var displaySentences: mainPanel.displayData.sentences

    // Gutter column layout (Item: separate gutter areas for timestamp vs.
    // speaker name, kept as named constants so both are easy to re-style/
    // resize independently later).
    readonly property real timestampGutterWidth: 44
    readonly property real speakerGutterWidth: 70
    readonly property real gutterSpacing: 8

    // Compact gutter timestamp: 1 decimal place, only the start time (no
    // range), and leading zero-valued units (hours, then minutes) dropped
    // entirely rather than shown as "0:"/"00:".
    function formatGutterTimestamp(t0) {
        const totalTenths = Math.round(t0 * 10)
        const wholeSeconds = Math.floor(totalTenths / 10)
        const tenth = totalTenths % 10
        const h = Math.floor(wholeSeconds / 3600)
        const m = Math.floor((wholeSeconds % 3600) / 60)
        const s = wholeSeconds % 60
        if (h > 0)
            return h + ":" + String(m).padStart(2, "0") + ":" + String(s).padStart(2, "0") + "." + tenth
        if (m > 0)
            return m + ":" + String(s).padStart(2, "0") + "." + tenth
        return s + "." + tenth
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
        return pos <= transcriptArea.length ? transcriptArea.positionToRectangle(pos) : Qt.rect(0, 0, 0, 0)
    }

    readonly property rect activeSentenceRect: {
        void transcriptArea.length
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

    // Item 13: total non-overlapping occurrences of q in clean — feeds the
    // "a/b" display alongside jumpToNextMatch's own forward-scan logic.
    function countMatches(clean, q) {
        if (q === "")
            return 0
        const lower = clean.toLowerCase()
        let count = 0
        let pos = 0
        while (true) {
            const idx = lower.indexOf(q, pos)
            if (idx === -1)
                break
            count++
            pos = idx + q.length
        }
        return count
    }

    // 1-based ordinal of the match STARTING at matchIdx, among all matches
    // in left-to-right order (matches countMatches' same non-overlapping scan).
    function ordinalOfMatch(clean, q, matchIdx) {
        const lower = clean.toLowerCase()
        let count = 0
        let pos = 0
        while (true) {
            const idx = lower.indexOf(q, pos)
            if (idx === -1)
                break
            count++
            if (idx === matchIdx)
                break
            pos = idx + q.length
        }
        return count
    }

    function resetSearchMatch() {
        mainPanel.searchMatchIndex = -1
        mainPanel.searchMatchCount = 0
        mainPanel.searchMatchOrdinal = 0
        mainPanel.searchMatchStart = -1
        mainPanel.searchMatchLength = 0
    }

    function jumpToNextMatch() {
        if (mainPanel.searchQuery === "")
            return
        const corpus = mainPanel.searchCorpus()
        if (corpus.clean === "")
            return
        const q = mainPanel.searchQuery.toLowerCase()
        const lower = corpus.clean.toLowerCase()
        let start = mainPanel.searchMatchIndex < 0 ? 0 : mainPanel.searchMatchIndex + 1
        let idx = lower.indexOf(q, start)
        if (idx === -1)
            idx = lower.indexOf(q, 0) // wrap around
        mainPanel.searchMatchIndex = idx
        if (idx === -1) {
            mainPanel.resetSearchMatch()
            return
        }
        mainPanel.searchMatchCount = mainPanel.countMatches(corpus.clean, q)
        mainPanel.searchMatchOrdinal = mainPanel.ordinalOfMatch(corpus.clean, q, idx)

        // Translate [idx, matchEnd) in the clean corpus back to a raw
        // select() range (into displayText) — spanning every sentence the
        // match touches, so a match crossing a segment boundary still
        // highlights correctly (the intervening newline/"[Speaker N]" text
        // between two sentences ends up included in the raw selection too —
        // there's no single contiguous range that would exclude it).
        const matchEnd = idx + q.length
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
        const selStart = firstEntry.rawStart + Math.max(0, idx - firstEntry.cleanStart)
        const selEnd = lastEntry.rawStart + Math.min(lastEntry.rawLength, matchEnd - lastEntry.cleanStart)
        mainPanel.searchMatchStart = selStart
        mainPanel.searchMatchLength = selEnd - selStart
        // select() alone still scrolls the match into view — no need to
        // steal focus from the search field, which would stop repeated
        // Enter presses from working (each jumps to the next match without
        // needing to re-click into it). The visible highlight itself is
        // searchMatchOverlay (Item 7), not the native selection.
        transcriptArea.select(selStart, selEnd)
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
    onSearchQueryChanged: mainPanel.resetSearchMatch()

    // Item 8 (Wave D): follow the transcript as it grows during an active
    // transcription of the currently-open file — only while that job is
    // actually running, so browsing a finished transcript never gets
    // yanked to the bottom. Qt.callLater so the TextArea/ScrollView have
    // re-laid-out with the new text before contentHeight is read.
    onTranscriptBodyChanged: {
        if (mainPanel.transcriptionController && mainPanel.player && mainPanel.transcriptionController.busy
                && mainPanel.transcriptionController.currentFile.toString() === mainPanel.player.source.toString()) {
            Qt.callLater(function () {
                const flick = transcriptScrollView.contentItem
                flick.contentY = Math.max(0, flick.contentHeight - flick.height)
            })
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
                    color: (playPauseHover.hovered && playPauseButton.enabled) ? "#3a3a3a" : "#2a2a2a"
                }
                HoverBorder {
                    anchors.fill: parent
                    active: playPauseButton.enabled && playPauseHover.hovered
                }
                // Drawn, not a font glyph: the "▶" Unicode character's em-box
                // is never truly centered on its triangle (padding varies by
                // font/system, so any fixed offset is a guess that breaks
                // elsewhere) — a Canvas-drawn shape is geometrically centered
                // by construction, matching pause's two bars exactly the
                // same way.
                Canvas {
                    id: playPauseIcon
                    anchors.centerIn: parent
                    width: 14
                    height: 14

                    property bool playing: mainPanel.player && mainPanel.player.playing
                    property color iconColor: playPauseButton.enabled ? "white" : "#666666"
                    onPlayingChanged: requestPaint()
                    onIconColorChanged: requestPaint()

                    onPaint: {
                        const ctx = getContext("2d")
                        ctx.reset()
                        ctx.fillStyle = playPauseIcon.iconColor
                        if (playPauseIcon.playing) {
                            const barWidth = width * 0.3
                            ctx.fillRect(0, 0, barWidth, height)
                            ctx.fillRect(width - barWidth, 0, barWidth, height)
                        } else {
                            ctx.beginPath()
                            ctx.moveTo(0, 0)
                            ctx.lineTo(width, height / 2)
                            ctx.lineTo(0, height)
                            ctx.closePath()
                            ctx.fill()
                        }
                    }
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
                        border.color: "white"
                        opacity: seekSlider.hovered ? 1 : 0
                        Behavior on opacity {
                            NumberAnimation { duration: 100 }
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

                    background: UnderlineField { focused: titleField.activeFocus }

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
                                                                         mainPanel.vaultPath,
                                                                         AppSettings.restrictTagsToExisting,
                                                                         AppSettings.tagReuseBias)
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

                background: UnderlineField { focused: searchField.activeFocus }

                HoverHandler {
                    cursorShape: Qt.IBeamCursor
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

        // Item 13: there was previously no way to see generated tags at all
        // (auto-labeling wrote them to frontmatter but nothing displayed
        // them). Small rounded pills, one per comma-separated tag.
        Flow {
            id: tagsRow
            Layout.fillWidth: true
            spacing: 6
            visible: mainPanel.currentTags().length > 0

            Repeater {
                model: mainPanel.currentTags()

                Rectangle {
                    id: tagPill
                    required property string modelData
                    radius: AppSettings.borderRadius
                    color: tagPillHover.hovered ? "#3f3f3f" : "#333333"
                    border.width: 1
                    border.color: "#4a4a4a"
                    implicitWidth: tagLabel.implicitWidth + 12
                    implicitHeight: tagLabel.implicitHeight + 6

                    // Item 16 (Wave E): clicking a tag filters the sidebar
                    // tree to files with that tag.
                    HoverHandler {
                        id: tagPillHover
                        cursorShape: Qt.PointingHandCursor
                    }
                    TapHandler {
                        onTapped: mainPanel.tagActivated(tagPill.modelData)
                    }

                    Label {
                        id: tagLabel
                        anchors.centerIn: parent
                        text: parent.modelData
                        color: "#cccccc"
                        font.pixelSize: 11
                    }
                }
            }
        }

        // Item 11 (Wave E): one pill per distinct speaker id detected in
        // THIS transcript, showing its current display name (default:
        // "Speaker N"). Click-to-rename mirrors the title's own
        // Label<->TextField swap pattern; renaming updates the shared
        // speakerNames frontmatter map, so every future read of this
        // transcript (this pill row, and anywhere else that consults the
        // same map) shows the new name — the transcript TEXT ITSELF below
        // still shows the stable "[Speaker N]" tag verbatim (renaming is a
        // frontmatter-only edit, not a rewrite of the segment lines), since
        // substituting display names into that text would shift the
        // character offsets sentence-highlighting/search overlays rely on.
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
                    readonly property bool renaming: mainPanel.renamingSpeakerId === modelData
                    implicitWidth: renaming ? speakerNameField.implicitWidth + 12 : speakerPillLabel.implicitWidth + 12
                    implicitHeight: 22

                    Rectangle {
                        anchors.fill: parent
                        radius: AppSettings.borderRadius
                        color: speakerPillHover.hovered ? "#3f5f7f" : "#2f4f5f"
                        border.width: 1
                        border.color: "#4a6a7a"
                        visible: !speakerPillItem.renaming
                    }
                    HoverHandler {
                        id: speakerPillHover
                        cursorShape: Qt.PointingHandCursor
                        enabled: !speakerPillItem.renaming
                    }
                    TapHandler {
                        enabled: !speakerPillItem.renaming
                        onTapped: {
                            speakerNameField.text = mainPanel.displaySpeakerName(speakerPillItem.modelData)
                            mainPanel.renamingSpeakerId = speakerPillItem.modelData
                            speakerNameField.forceActiveFocus()
                            speakerNameField.selectAll()
                        }
                    }
                    Label {
                        id: speakerPillLabel
                        anchors.centerIn: parent
                        visible: !speakerPillItem.renaming
                        text: mainPanel.displaySpeakerName(speakerPillItem.modelData)
                        color: "#dddddd"
                        font.pixelSize: 11
                    }

                    TextField {
                        id: speakerNameField
                        anchors.centerIn: parent
                        visible: speakerPillItem.renaming
                        font.pixelSize: 11
                        onAccepted: mainPanel.commitSpeakerRename(speakerPillItem.modelData, text)
                        onActiveFocusChanged: if (!activeFocus && speakerPillItem.renaming)
                                                  mainPanel.commitSpeakerRename(speakerPillItem.modelData, text)

                        background: UnderlineField { focused: speakerNameField.activeFocus }

                        Keys.onEscapePressed: mainPanel.renamingSpeakerId = ""
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
                    active: modelCombo.enabled && modelCombo.hovered
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
                         ? qsTr("Add to queue")
                         : (mainPanel.hasStoredMetadata ? qsTr("Re-transcribe") : qsTr("Transcribe")))
                // A manual, explicit click always re-runs — regardless of
                // whether the current model/speaker-detection selection
                // matches what's already stored (only "transcribe all"
                // bulk-skips already-current files; a single deliberate
                // click here never should).
                enabled: mainPanel.player && mainPanel.player.source.toString() !== ""
                         && mainPanel.transcriptionController
                         && !transcribeButton.isActive && !transcribeButton.isQueued
                hoverEnabled: true
                onClicked: mainPanel.transcriptionController.transcribe(mainPanel.player.source, mainPanel.vaultPath,
                                                                         modelCombo.currentValue,
                                                                         speakerDetectionToggle.checked,
                                                                         AppSettings.autoLabelEnabled,
                                                                         AppSettings.restrictTagsToExisting,
                                                                         AppSettings.tagReuseBias)

                background: Rectangle {
                    radius: AppSettings.borderRadius
                    color: (transcribeButton.hovered && transcribeButton.enabled) ? "#3a3a3a" : "#2a2a2a"
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
                    active: transcribeButton.enabled && transcribeButton.hovered
                }
            }
        }

        ScrollView {
            id: transcriptScrollView
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            visible: mainPanel.transcriptBody !== ""
            ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

            TextArea {
                id: transcriptArea
                readOnly: true
                wrapMode: TextArea.Wrap
                text: mainPanel.displayText
                color: "#dddddd"
                background: null
                selectByMouse: true
                selectByKeyboard: true
                // Reserves space for the timestamp + speaker gutters below
                // — neither is part of this control's actual text (see
                // displayText's doc comment), so a click-drag selection can
                // never touch them; this is just the visual margin they're
                // drawn in.
                leftPadding: mainPanel.timestampGutterWidth + mainPanel.gutterSpacing
                             + mainPanel.speakerGutterWidth + mainPanel.gutterSpacing
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
                    return mainPanel.displaySentences.map(
                        s => s.start <= len ? transcriptArea.positionToRectangle(s.start) : Qt.rect(0, 0, 0, 0))
                }

                // Two SEPARATE gutter columns (Item: independently stylable
                // areas, not one combined label) — each aligned to its
                // sentence's first visual line via gutterLineRects above.
                // showGutter (see displayData) collapses a run of
                // same-speaker sentences down to ONE pair of labels at the
                // start of that turn, rather than repeating the same
                // timestamp+name on every line — this is what makes "one
                // timestamp per section" a real, deterministic rule instead
                // of a defensive guess.
                Repeater {
                    id: timestampGutterRepeater
                    model: mainPanel.displaySentences

                    Text {
                        required property var modelData
                        required property int index
                        visible: modelData.showGutter
                        readonly property rect lineRect: transcriptArea.gutterLineRects[index] || Qt.rect(0, 0, 0, 0)
                        x: 0
                        y: lineRect.y
                        // Match the corresponding line's own height and
                        // center within it — the gutter's small font
                        // otherwise sits near the TOP of the (taller) main
                        // text line instead of level with its glyphs.
                        height: lineRect.height
                        width: mainPanel.timestampGutterWidth
                        horizontalAlignment: Text.AlignRight
                        verticalAlignment: Text.AlignVCenter
                        text: mainPanel.formatGutterTimestamp(modelData.t0)
                        color: "#777777"
                        font.pixelSize: 10
                    }
                }

                Repeater {
                    id: speakerGutterRepeater
                    model: mainPanel.displaySentences

                    Text {
                        required property var modelData
                        required property int index
                        visible: modelData.showGutter && modelData.speaker > 0
                        readonly property rect lineRect: transcriptArea.gutterLineRects[index] || Qt.rect(0, 0, 0, 0)
                        x: mainPanel.timestampGutterWidth + mainPanel.gutterSpacing
                        y: lineRect.y
                        height: lineRect.height
                        width: mainPanel.speakerGutterWidth
                        elide: Text.ElideRight
                        horizontalAlignment: Text.AlignLeft
                        verticalAlignment: Text.AlignVCenter
                        text: modelData.speaker > 0 ? mainPanel.displaySpeakerName("Speaker " + modelData.speaker) : ""
                        color: "#8fa8bf"
                        font.pixelSize: 10
                        font.bold: true
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

            // Item 12 (Wave E): Chrome/PDF-viewer-style middle-click-hold-
            // drag scrolling — content follows the cursor 1:1 while the
            // middle button is held (grab-and-pan), not a toggled/velocity-
            // based autoscroll. acceptedButtons is scoped to MiddleButton
            // only, so left-click text selection and the scrollbar's own
            // drag are completely unaffected (a non-matching button simply
            // isn't accepted, falling through to whatever's beneath).
            MouseArea {
                id: middleDragScrollArea
                anchors.fill: parent
                acceptedButtons: Qt.MiddleButton
                cursorShape: pressed ? Qt.SizeAllCursor : Qt.ArrowCursor
                propagateComposedEvents: true

                property real dragStartY: 0
                property real startContentY: 0

                onPressed: (mouse) => {
                    middleDragScrollArea.dragStartY = mouse.y
                    middleDragScrollArea.startContentY = transcriptScrollView.contentItem.contentY
                }
                onPositionChanged: (mouse) => {
                    if (!pressed)
                        return
                    const flick = transcriptScrollView.contentItem
                    const delta = mouse.y - middleDragScrollArea.dragStartY
                    const maxY = Math.max(0, flick.contentHeight - flick.height)
                    flick.contentY = Math.max(0, Math.min(maxY, middleDragScrollArea.startContentY - delta))
                }
            }
        }

        Item {
            Layout.fillHeight: true
            visible: mainPanel.transcriptBody === ""
        }
    }
}
