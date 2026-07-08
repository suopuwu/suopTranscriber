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
        // Speaker detection defaults ON for an untranscribed file (see
        // DESIGN.md "Speaker diarization") — an already-transcribed file
        // instead honors whatever was actually used last time, same as
        // every other stored setting here.
        speakerDetectionToggle.checked =
            mainPanel.hasStoredMetadata ? storedMetadata.speakerDetection === true : true
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

    // Which speaker pill (if any) is currently showing its rename TextField
    // — mirrors renamingTitle's click-to-rename pattern, just keyed by id
    // since there can be several pills.
    property string renamingSpeakerId: ""

    // Surfaced briefly next to the Speakers row (see speakerOpErrorTimer) —
    // this app has no toast/notification system, so a small inline red
    // label is the simplest consistent way to explain a rejected rename OR
    // a failed merge (see commitSpeakerRename()/onSpeakersMerged() below) —
    // both are speaker-tag rewrites that can fail the same way, so they
    // share this one banner slot rather than each getting their own.
    property string speakerOpError: ""

    Timer {
        id: speakerOpErrorTimer
        interval: 4000
        onTriggered: mainPanel.speakerOpError = ""
    }

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

    // oldName must be the speaker's TRUE on-disk name (i.e. an entry from
    // distinctSpeakerIds) — see pendingSpeakerRenames' doc comment above for
    // why the pill Repeater passes that instead of the currently-DISPLAYED
    // (possibly already-overridden) name.
    function commitSpeakerRename(oldName, newValue) {
        const trimmed = newValue.trim()
        mainPanel.renamingSpeakerId = ""
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
                mainPanel.speakerOpError = qsTr("A speaker named \"%1\" already exists.").arg(trimmed)
                speakerOpErrorTimer.restart()
                return
            }
        }
        mainPanel.speakerOpError = ""

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
                mainPanel.speakerOpError = error
                speakerOpErrorTimer.restart()
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
                mainPanel.speakerOpError = error
                speakerOpErrorTimer.restart()
            }
            const updated = Object.assign({}, mainPanel.pendingSpeakerMerges)
            delete updated[sourceName]
            mainPanel.pendingSpeakerMerges = updated
        }
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
    // transcription of the currently-open file — reading .liveUpdateTick
    // here (even though the value itself isn't used) makes this binding
    // re-evaluate each time a real new segment line lands in the .md file,
    // not on every one of whisper's internal progress ticks (those fire per
    // decode-loop iteration, including retries, far more often than real
    // new content appears — see TranscriptionController::liveUpdateTick's
    // doc comment). Gated on currentFile matching so unrelated jobs' ticks
    // don't cause pointless re-reads.
    readonly property string transcriptBody: {
        if (mainPanel.transcriptionController && mainPanel.player
                && mainPanel.transcriptionController.currentFile.toString() === mainPanel.player.source.toString()) {
            void mainPanel.transcriptionController.liveUpdateTick
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
    // Same live-update hook as transcriptBody above (read .liveUpdateTick so
    // this re-evaluates each time a real new segment lands, not just at
    // completion) — this was missing here, which is why displayText/the
    // transcript view (both now built FROM sentences, not from
    // transcriptBody directly) didn't show new segments live during an
    // in-progress transcription: nothing else reliably re-triggers this
    // binding mid-job (storedMetadata's reference changes, but
    // hasStoredMetadata's VALUE stays true the whole time once frontmatter
    // exists, so its own change notify doesn't fire on every tick).
    readonly property var sentences: {
        void mainPanel.sentencesRevision
        if (mainPanel.transcriptionController && mainPanel.player
                && mainPanel.transcriptionController.currentFile.toString() === mainPanel.player.source.toString()) {
            void mainPanel.transcriptionController.liveUpdateTick
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
    // with no speaker detection (speaker is always "") have no "turns" to
    // group by, so every sentence counts as its own section there.
    //
    // speaker here is the EFFECTIVE (pending-rename-applied) display name —
    // distinctSpeakerIds/commitSpeakerRename still work off the raw on-disk
    // sentences[i].speaker directly (see effectiveSpeakerName()'s doc
    // comment for why the override is deliberately NOT baked in there).
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
            list.push({t0: s.t0, speaker: speaker, text: displayText, start: start, length: s.text.length,
                       showGutter: showGutter})
            previousSpeaker = speaker
            text += displayText + "\n"
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

            // Item 28: transcribe as an icon button (same 22x22 box as the
            // regenerate/search icons it now sits beside), replacing the old
            // full-width text Button that lived down in the scrollable
            // content below the model combo. Same enable/tooltip logic the
            // old Button had (Wave F: a manual click always re-runs,
            // regardless of stored-metadata match) — modelCombo/
            // speakerDetectionToggle are declared lower in this file but
            // still resolve fine since ids are scoped to the whole document.
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
                                                                             speakerDetectionToggle.checked,
                                                                             AppSettings.autoLabelEnabled,
                                                                             AppSettings.restrictTagsToExisting,
                                                                             AppSettings.tagReuseBias)
                }
                HoverBorder {
                    anchors.fill: parent
                    active: transcribeIconButton.enabled && transcribeIconHover.hovered
                }
                Text {
                    anchors.centerIn: parent
                    text: "⤒"
                    color: transcribeIconButton.enabled ? "#dddddd" : "#666666"
                    font.pixelSize: 13
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

        ScrollView {
            id: transcriptScrollView
            anchors.fill: parent
            clip: true
            ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

            ColumnLayout {
                id: scrollableContent
                width: transcriptScrollView.availableWidth
                spacing: 12

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
                            readonly property bool renaming: mainPanel.renamingSpeakerId === modelData
                            readonly property bool confirmingMerge: mainPanel.mergeConfirmSourceId === modelData
                            readonly property bool pendingMerge: modelData in mainPanel.pendingSpeakerMerges
                            readonly property bool isDropTarget: mainPanel.dragHoverSpeakerId === modelData
                            implicitWidth: (renaming ? speakerNameField.implicitWidth
                                            : confirmingMerge ? mergeConfirmRow.implicitWidth
                                            : speakerPillLabel.implicitWidth) + 12
                            implicitHeight: 22
                            opacity: pendingMerge ? 0.5 : 1.0

                            Rectangle {
                                anchors.fill: parent
                                radius: AppSettings.borderRadius
                                color: speakerPillItem.isDropTarget ? "#3f7f5f"
                                       : speakerPillHover.hovered ? "#3f5f7f" : "#2f4f5f"
                                border.width: speakerPillItem.isDropTarget ? 2 : 1
                                border.color: speakerPillItem.isDropTarget ? "#6fdf9f" : "#4a6a7a"
                                visible: !speakerPillItem.renaming
                            }
                            HoverHandler {
                                id: speakerPillHover
                                cursorShape: Qt.PointingHandCursor
                                enabled: !speakerPillItem.renaming && !speakerPillItem.confirmingMerge
                            }
                            TapHandler {
                                enabled: !speakerPillItem.renaming && !speakerPillItem.confirmingMerge
                                         && !speakerPillItem.pendingMerge
                                onTapped: {
                                    speakerNameField.text = mainPanel.effectiveSpeakerName(speakerPillItem.modelData)
                                    mainPanel.renamingSpeakerId = speakerPillItem.modelData
                                    speakerNameField.forceActiveFocus()
                                    speakerNameField.selectAll()
                                }
                            }
                            // Drag source AND (via mainPanel.speakerIdAtScenePos())
                            // the mechanism that finds the drop target — coexists
                            // with the TapHandler above; Qt's pointer handlers
                            // already disambiguate a plain tap (rename) from a
                            // drag past the movement threshold (merge).
                            DragHandler {
                                id: speakerPillDrag
                                enabled: !speakerPillItem.renaming && !speakerPillItem.confirmingMerge
                                         && !speakerPillItem.pendingMerge
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
                                    const scenePos = speakerPillItem.mapToItem(mainPanel, centroid.position.x,
                                                                                centroid.position.y)
                                    mainPanel.dragGhostX = scenePos.x
                                    mainPanel.dragGhostY = scenePos.y
                                    mainPanel.dragHoverSpeakerId =
                                        mainPanel.speakerIdAtScenePos(scenePos.x, scenePos.y, speakerPillItem.modelData)
                                }
                            }
                            Label {
                                id: speakerPillLabel
                                anchors.centerIn: parent
                                visible: !speakerPillItem.renaming && !speakerPillItem.confirmingMerge
                                text: mainPanel.effectiveSpeakerName(speakerPillItem.modelData)
                                color: "#dddddd"
                                font.pixelSize: 11
                            }

                            // Inline "Merge into X?" confirmation — see
                            // mainPanel.resolveSpeakerMerge()'s doc comment for
                            // when this shows instead of committing immediately.
                            // Inline rather than a popup, per StyleGuide.md
                            // ("Warning banners: inline, not a popup/toast") and
                            // matching the rename Label<->TextField swap above.
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
                                Label {
                                    text: "✓"
                                    color: "#8fdf9f"
                                    font.pixelSize: 12
                                    font.bold: true
                                    HoverHandler { cursorShape: Qt.PointingHandCursor }
                                    TapHandler { onTapped: mainPanel.confirmSpeakerMerge(true) }
                                }
                                Label {
                                    text: "✕"
                                    color: "#e07a7a"
                                    font.pixelSize: 12
                                    font.bold: true
                                    HoverHandler { cursorShape: Qt.PointingHandCursor }
                                    TapHandler { onTapped: mainPanel.confirmSpeakerMerge(false) }
                                }
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

                // Rejected/failed speaker rename OR merge feedback (duplicate
                // name, or a background rewrite failure) — see
                // commitSpeakerRename()/onSpeakerRenamed/onSpeakersMerged. No
                // existing toast system in this app, so a small auto-clearing
                // inline label is the simplest consistent way to surface it.
                Label {
                    Layout.fillWidth: true
                    visible: mainPanel.speakerOpError !== ""
                    text: mainPanel.speakerOpError
                    color: "#e07a7a"
                    font.pixelSize: 11
                    wrapMode: Text.WordWrap
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

                // Speaker detection was requested but couldn't actually run (models
                // unavailable/failed to load) — the transcription still completed,
                // ASR-only, rather than being blocked (see DESIGN.md "Speaker
                // diarization" graceful-degradation decision). Same inline-warning
                // shape as speakerOpError above.
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
                        hoverEnabled: true

                        Component.onCompleted: currentIndex = mainPanel.indexForModelId(AppSettings.lastModel)
                        onActivated: AppSettings.lastModel = currentValue

                        ToolTip.visible: modelCombo.hovered
                        ToolTip.text: modelCombo.currentText
                        ToolTip.delay: 500

                        HoverHandler {
                            cursorShape: Qt.PointingHandCursor
                        }
                        HoverBorder {
                            anchors.fill: parent
                            active: modelCombo.hovered
                        }
                    }
                }

            TextArea {
                id: transcriptArea
                Layout.fillWidth: true
                visible: mainPanel.transcriptBody !== ""
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

                Repeater {
                    id: speakerGutterRepeater
                    model: mainPanel.displaySentences

                    Text {
                        required property var modelData
                        required property int index
                        visible: modelData.showGutter && modelData.speaker !== ""
                        readonly property rect lineRect: transcriptArea.gutterLineRects[index] || Qt.rect(0, 0, 0, 0)
                        x: mainPanel.timestampGutterWidth + mainPanel.gutterSpacing
                        y: lineRect.y
                        height: lineRect.height
                        width: mainPanel.speakerGutterWidth
                        elide: Text.ElideRight
                        // Right-aligned so the name sits right up against
                        // the transcript text (only gutterSpacing away)
                        // instead of leaving a ragged gap whenever the name
                        // is shorter than the column.
                        horizontalAlignment: Text.AlignRight
                        verticalAlignment: Text.AlignVCenter
                        text: modelData.speaker
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

            } // end scrollableContent ColumnLayout
        } // end transcriptScrollView

        // Item 12 (Wave E): Chrome/PDF-viewer-style middle-click-hold-
        // drag scrolling — content follows the cursor 1:1 while the
        // middle button is held (grab-and-pan), not a toggled/velocity-
        // based autoscroll. acceptedButtons is scoped to MiddleButton
        // only, so left-click text selection and the scrollbar's own
        // drag are completely unaffected (a non-matching button simply
        // isn't accepted, falling through to whatever's beneath). Lives as
        // an overlay sibling of transcriptScrollView (not inside it, see
        // transcriptViewportContainer's doc comment) so it covers the same
        // full viewport without being counted as the ScrollView's content.
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
}
