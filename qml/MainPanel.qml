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

    // Feature 2: multi-paragraph, editable prelude (upgrades O26's old
    // read-only single-line summary) — lives in the .md BODY, not
    // storedMetadata/frontmatter (see TranscriptionController::readPrelude()),
    // so it needs its own property, populated alongside storedMetadata in
    // refreshMetadata() below.
    property string prelude: ""
    property bool editingPrelude: false

    // Feature 3b/3c: chapter markers (see
    // TranscriptionController::readChapters()) — shared by the seek-bar tick
    // marks (3b) and the chapter list (3c), populated once alongside
    // prelude/storedMetadata in refreshMetadata() below rather than each
    // calling readChapters() separately.
    property var chapters: []
    property bool chaptersExpanded: true

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
        // The prelude lives in the BODY, not frontmatter, so it can't
        // piggyback on storedMetadata above (readMetadata()'s frontmatter
        // cache has no reason to invalidate when only the body changes) —
        // needs its own read call. Folded in here rather than a separate
        // refreshPrelude(), so every existing refreshMetadata() call site
        // (onSourceChanged/onFinished/onProgressChanged) picks it up for free.
        mainPanel.prelude = (player && player.source.toString() !== "" && transcriptionController)
            ? transcriptionController.readPrelude(player.source) : ""
        // Same reasoning as prelude above — chapters live in the BODY as
        // "## [timestamp] title" marker lines, not frontmatter, so they need
        // their own read call here too.
        mainPanel.chapters = (player && player.source.toString() !== "" && transcriptionController)
            ? transcriptionController.readChapters(player.source) : []
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

    // Item P16: minimal flat undo (no redo) for the four kinds of small
    // manual edits this panel can make directly (label, prelude, tags,
    // speaker rename) — a plain JS stack of {type, ...} entries, each
    // recording enough to reverse itself, pushed BEFORE the mutation it
    // guards. Reset whenever the open file changes (see onSourceChanged/
    // onOrphanedMdPathChanged below) so Ctrl+Z never reaches across a file
    // switch into a different note's history. Speaker MERGE is
    // deliberately NOT undoable here — unlike a rename, a merge collapses
    // two distinct speaker identities into one (which sentences originally
    // belonged to the losing speaker is no longer recoverable from the
    // rewritten transcript alone), so there is no simple reverse operation
    // to record.
    property var undoStack: []

    // Reassigns (rather than push()/pop() in place) so undoStackChanged()
    // actually fires — a plain JS array mutated in place doesn't notify any
    // binding that reads mainPanel.undoStack.length (e.g. the Ctrl+Z
    // Shortcut's `enabled` below), since QML's dependency tracking only
    // hooks the property's own notify signal, not the array contents.
    function pushUndo(entry) {
        mainPanel.undoStack = mainPanel.undoStack.concat([entry])
    }

    function undoLast() {
        if (mainPanel.undoStack.length === 0 || !mainPanel.transcriptionController)
            return
        const entry = mainPanel.undoStack[mainPanel.undoStack.length - 1]
        mainPanel.undoStack = mainPanel.undoStack.slice(0, -1)
        switch (entry.type) {
        case "label":
            mainPanel.transcriptionController.setLabel(entry.fileUrl, entry.oldValue)
            break
        case "prelude":
            mainPanel.transcriptionController.setPrelude(entry.fileUrl, entry.oldValue)
            break
        case "tags":
            mainPanel.transcriptionController.setTags(entry.fileUrl, entry.oldValue)
            break
        case "speakerRename": {
            // Mirrors commitSpeakerRename()'s own optimistic overlay so the
            // pill updates instantly here too, not just via the eventual
            // background rewrite + sentencesRevision bump.
            const updated = Object.assign({}, mainPanel.pendingSpeakerRenames)
            updated[entry.reverseOld] = entry.reverseNew
            mainPanel.pendingSpeakerRenames = updated
            mainPanel.transcriptionController.renameSpeaker(entry.fileUrl, entry.reverseOld, entry.reverseNew)
            break
        }
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
                const oldLabel = mainPanel.orphanedMdMetadata.label || ""
                if (trimmed !== oldLabel)
                    mainPanel.pushUndo({type: "label", fileUrl: "file://" + mainPanel.orphanedMdPath, oldValue: oldLabel})
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
                    mainPanel.pushUndo({type: "label", fileUrl: mainPanel.player.source, oldValue: oldLabel})
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

    // Feature 2: Save action for the prelude editor below. Explicit
    // Save/Cancel rather than commit-on-Enter (titleField's pattern) since
    // the prelude is multi-line — plain Enter has to insert a newline
    // instead. Orphaned-md files never show the prelude editor (see its
    // visible: condition below), so unlike commitRename() this only needs
    // the player.source path. setLabel()'s failures are already silently
    // ignored by commitRename() above; matched here for consistency.
    function commitPrelude(newValue) {
        mainPanel.editingPrelude = false
        if (!mainPanel.transcriptionController || !mainPanel.player || mainPanel.player.source.toString() === "")
            return
        if (newValue !== mainPanel.prelude)
            mainPanel.pushUndo({type: "prelude", fileUrl: mainPanel.player.source, oldValue: mainPanel.prelude})
        mainPanel.transcriptionController.setPrelude(mainPanel.player.source, newValue)
        mainPanel.refreshMetadata()
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
        mainPanel.pushUndo({type: "tags", fileUrl: mainPanel.currentFileUrl(), oldValue: current})
        mainPanel.transcriptionController.setTags(mainPanel.currentFileUrl(), current.concat([trimmed]))
    }

    function removeTag(tag) {
        if (!mainPanel.transcriptionController)
            return
        const current = mainPanel.currentTags()
        mainPanel.pushUndo({type: "tags", fileUrl: mainPanel.currentFileUrl(), oldValue: current})
        mainPanel.transcriptionController.setTags(mainPanel.currentFileUrl(), current.filter(t => t !== tag))
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
        let chapterIdx = 0

        function pushHeading(chapter) {
            const headingStart = text.length
            list.push({t0: chapter.t0, speaker: "", text: "", start: headingStart, length: 0,
                       showGutter: false, isChapterHeading: true, chapterTitle: chapter.title})
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
                pushHeading({t0: Math.min(chapterList[chapterIdx].t0, s.t0), title: chapterList[chapterIdx].title})
                chapterIdx++
            }
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
                       showGutter: showGutter, isChapterHeading: false, chapterTitle: ""})
            previousSpeaker = speaker
            text += displayText + "\n"
        }
        // Any chapter at/after the last real sentence (e.g. a trailing
        // marker with no further speech) still gets a heading rather than
        // being silently dropped.
        while (chapterIdx < chapterList.length) {
            pushHeading(chapterList[chapterIdx])
            chapterIdx++
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

    // Shared match-finding core for both "jump to next" (Enter, searches
    // from just after the current match) and "search as you type"
    // (searchQuery changing, searches from the very start) — locates the
    // first occurrence of searchQuery at or after fromIndex in the search
    // corpus, wrapping around to the start if none is found from there,
    // then updates all the match-state properties and select()s the raw
    // range.
    function locateMatch(fromIndex) {
        if (mainPanel.searchQuery === "")
            return
        const corpus = mainPanel.searchCorpus()
        if (corpus.clean === "")
            return
        const q = mainPanel.searchQuery.toLowerCase()
        const lower = corpus.clean.toLowerCase()
        let idx = lower.indexOf(q, Math.max(0, fromIndex))
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
            transcriptRepaintNudgeTimer.restart()
            mainPanel.undoStack = []
        }
    }
    // Item P8/P16: showOrphanedMd() switches files by setting orphanedMdPath
    // directly (player.source is cleared to "" first, which fires
    // onSourceChanged above for the "no file" state, not the orphaned file
    // itself) — this is the other half of "a different file was just
    // opened", covering the orphaned-md view.
    onOrphanedMdPathChanged: {
        transcriptRepaintNudgeTimer.restart()
        mainPanel.undoStack = []
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
    // Item O10: typing now searches immediately (not just on Enter) —
    // reset first (clears the stale match's highlight/count instantly) then
    // locate the FIRST match from the very start of the corpus. Enter
    // (searchField.onAccepted -> jumpToNextMatch) still advances from the
    // current match, unchanged.
    onSearchQueryChanged: {
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

        if (mainPanel.transcriptionController && mainPanel.player && mainPanel.transcriptionController.busy
                && mainPanel.transcriptionController.currentFile.toString() === mainPanel.player.source.toString()) {
            const flick = transcriptScrollView.contentItem
            const nearBottomThreshold = 40
            const wasAtBottom = flick.contentY >= flick.contentHeight - flick.height - nearBottomThreshold
            if (wasAtBottom) {
                Qt.callLater(function () {
                    flick.contentY = Math.max(0, flick.contentHeight - flick.height)
                })
            }
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

    // Item P16: undoes the last label/prelude/tag/speaker-rename edit made
    // through this panel — see undoStack's own doc comment above.
    Shortcut {
        sequence: "Ctrl+Z"
        enabled: mainPanel.activeFocus && mainPanel.undoStack.length > 0
        onActivated: mainPanel.undoLast()
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
                // elsewhere) — a Canvas-drawn shape lets us control centering
                // directly. Pause's two bars are bbox-symmetric so centerIn
                // alone centers them correctly; play's triangle is not — its
                // visual centroid (average of its 3 vertices) sits left of
                // its bounding-box center, so it's drawn shifted right by a
                // fixed offset to visually balance against the pause glyph.
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
                            // Item P24: the tip used to sit at width+xOffset,
                            // 2px past this 14x14 canvas's own raster — it
                            // got silently clipped, reading as a blunt/cut
                            // triangle. Capping the tip at `width` instead
                            // (the offset now only shifts the LEFT edge in,
                            // shrinking the triangle's span rather than
                            // pushing it past the canvas bounds) keeps the
                            // whole shape on-canvas.
                            const xOffset = 2
                            ctx.beginPath()
                            ctx.moveTo(xOffset, 0)
                            ctx.lineTo(width, height / 2)
                            ctx.lineTo(xOffset, height)
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
                    Label {
                        id: filenameSubtitle
                        Layout.fillWidth: true
                        visible: !mainPanel.renamingTitle && AppSettings.useLabelsMainPanel
                                 && mainPanel.orphanedMdPath === "" && !!mainPanel.storedMetadata.label
                        elide: Text.ElideMiddle
                        text: (mainPanel.player && mainPanel.player.source.toString() !== "")
                              ? mainPanel.relativeToVault(mainPanel.player.source) : ""
                        color: "#777777"
                        font.pixelSize: 10
                    }
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
                                                                             AppSettings.tagReuseBias,
                                                                             AppSettings.wordiness)
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
                Text {
                    anchors.centerIn: parent
                    text: "↻"
                    color: regenerateLabelButton.enabled ? "#dddddd" : "#666666"
                    font.pixelSize: 14
                }
            }

            // Feature 3c: "Generate chapters" — mirrors regenerateLabelButton
            // above exactly (same enable/tooltip/hover shape), but scoped to
            // player-backed files only (not orphaned .md), matching the
            // prelude editor's same restriction just below (chapters, like
            // the prelude, are only ever read via refreshMetadata(), not
            // refreshOrphanedMetadata()) — placed here in the icon-button
            // cluster rather than inside the chapter list's own header row
            // so it's still reachable to generate a FIRST set of chapters
            // when mainPanel.chapters is still empty (the list itself only
            // renders once chapters.length > 0).
            Item {
                id: generateChaptersButton
                Layout.preferredWidth: 22
                Layout.preferredHeight: 22
                visible: mainPanel.transcriptionController
                         && mainPanel.orphanedMdPath === "" && mainPanel.hasStoredMetadata
                enabled: visible && !mainPanel.transcriptionController.isQueued(mainPanel.player.source)

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
                    onTapped: mainPanel.transcriptionController.generateChapters(mainPanel.player.source, mainPanel.vaultPath)
                }
                HoverBorder {
                    anchors.fill: parent
                    active: generateChaptersButton.enabled && generateChaptersHover.hovered
                }
                Text {
                    anchors.centerIn: parent
                    text: "≡"
                    color: generateChaptersButton.enabled ? "#dddddd" : "#666666"
                    font.pixelSize: 15
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

                            // Item 16 (Wave E): clicking the pill body (NOT
                            // the "x" below) filters the sidebar tree to
                            // files with that tag — the "x" TapHandler grabs
                            // the point first when the tap lands inside it
                            // (same exclusive-grab pattern already used by
                            // speakerPillItem's rename tap vs. its merge-
                            // confirm ✓/✕ labels), so the two never both fire.
                            HoverHandler {
                                id: tagPillHover
                                cursorShape: Qt.PointingHandCursor
                            }
                            TapHandler {
                                onTapped: mainPanel.tagActivated(tagPill.modelData)
                            }

                            RowLayout {
                                id: tagPillRow
                                anchors.centerIn: parent
                                spacing: 4

                                Label {
                                    id: tagLabel
                                    text: tagPill.modelData
                                    color: "#cccccc"
                                    font.pixelSize: 11
                                }

                                // Item P20: per-pill delete — always visible
                                // (not hover-only) so its hit target is
                                // predictable/discoverable, same treatment
                                // as searchCloseButton's "x" elsewhere in
                                // this file.
                                Text {
                                    id: tagRemoveIcon
                                    text: "×"
                                    color: tagRemoveHover.hovered ? "#e07a7a" : "#888888"
                                    font.pixelSize: 12

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

                    // Item P15: inline "+ tag" affordance — a small icon
                    // that swaps to a compact TextField on click (mirrors
                    // the title/prelude/speaker-pill click-to-edit idiom
                    // already used throughout this file), committing on
                    // Enter or on losing focus.
                    Item {
                        id: tagAddButton
                        visible: !tagAddField.editing
                        implicitWidth: tagAddLabel.implicitWidth + 12
                        implicitHeight: tagAddLabel.implicitHeight + 6

                        HoverHandler {
                            id: tagAddHover
                            cursorShape: Qt.PointingHandCursor
                        }
                        TapHandler {
                            onTapped: {
                                tagAddField.editing = true
                                tagAddField.forceActiveFocus()
                            }
                        }
                        Rectangle {
                            anchors.fill: parent
                            radius: AppSettings.borderRadius
                            color: tagAddHover.hovered ? "#3f3f3f" : "transparent"
                            border.width: 1
                            border.color: "#4a4a4a"
                        }
                        Label {
                            id: tagAddLabel
                            anchors.centerIn: parent
                            text: "+ " + qsTr("tag")
                            color: "#999999"
                            font.pixelSize: 11
                        }
                    }

                    TextField {
                        id: tagAddField
                        property bool editing: false
                        visible: editing
                        implicitWidth: 100
                        font.pixelSize: 11
                        placeholderText: qsTr("New tag…")

                        background: UnderlineField { focused: tagAddField.activeFocus }

                        onAccepted: {
                            mainPanel.addTag(text)
                            text = ""
                            tagAddField.editing = false
                        }
                        onActiveFocusChanged: {
                            if (!activeFocus) {
                                tagAddField.editing = false
                                tagAddField.text = ""
                            }
                        }
                        Keys.onEscapePressed: {
                            tagAddField.editing = false
                            tagAddField.text = ""
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

                                // Item P27: cursor-shape-only treatment — the
                                // pill's TapHandler (rename) and DragHandler
                                // (merge, both declared on the parent
                                // speakerPillItem above) grab on press, so
                                // giving this label real click-drag text
                                // selection (a TextEdit/MouseArea, which
                                // would need its own grab) would compete
                                // with — and likely break — the existing
                                // drag-to-merge gesture. HoverHandler alone
                                // never grabs, so it can show the I-beam
                                // hint without taking over the pointer.
                                HoverHandler {
                                    cursorShape: Qt.IBeamCursor
                                }
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
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        text: qsTr("Previously transcribed — model: %1, speaker detection: %2")
                              .arg(mainPanel.storedMetadata.model || "?")
                              .arg(mainPanel.storedMetadata.speakerDetection ? qsTr("on") : qsTr("off"))
                        color: "#aaaaaa"
                        font.pixelSize: 11
                    }
                }

                // Feature 2 (upgrades O26's old read-only single-line
                // summary): a click-to-edit, multi-paragraph prelude —
                // same muted secondary-text treatment as the "Previously
                // transcribed" label above (color/pixelSize) when not being
                // edited, just larger and unbolded so it reads as
                // descriptive prose rather than a status line. Same
                // click-to-edit idiom as titleLabel/titleField above, but a
                // TextArea (multi-line) with explicit Save/Cancel instead of
                // commit-on-Enter — plain Enter must insert a newline here.
                // Visibility mirrors O26's own gating exactly (never shown
                // for an orphaned .md, only once the file has been
                // transcribed at all).
                Item {
                    id: preludeContainer
                    Layout.fillWidth: true
                    visible: mainPanel.orphanedMdPath === "" && mainPanel.hasStoredMetadata
                    implicitHeight: mainPanel.editingPrelude
                        ? preludeEditColumn.implicitHeight : preludeLabel.implicitHeight

                    // Item P27: read-mode prelude is now a real selectable
                    // TextEdit (I-beam cursor, click-drag select/copy) —
                    // it can no longer double as the click-to-edit target
                    // (a plain click there now positions the text cursor/
                    // starts a selection instead), so preludeEditIcon below
                    // is the new, narrower trigger into edit mode.
                    Item {
                        id: preludeLabel
                        anchors.left: parent.left
                        anchors.right: parent.right
                        visible: !mainPanel.editingPrelude
                        implicitHeight: preludeText.implicitHeight

                        TextEdit {
                            id: preludeText
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.rightMargin: preludeEditIcon.implicitWidth + 6
                            readOnly: true
                            selectByMouse: true
                            selectByKeyboard: true
                            persistentSelection: true
                            activeFocusOnPress: false
                            text: mainPanel.prelude || qsTr("Click to add a prelude…")
                            wrapMode: TextEdit.WordWrap
                            color: mainPanel.prelude ? "#aaaaaa" : "#666666"
                            font.pixelSize: 12
                            font.italic: !mainPanel.prelude

                            HoverHandler {
                                cursorShape: Qt.IBeamCursor
                            }
                        }

                        Text {
                            id: preludeEditIcon
                            anchors.right: parent.right
                            anchors.top: parent.top
                            text: "✎"
                            color: preludeIconHover.hovered ? "#dddddd" : "#888888"
                            font.pixelSize: 11

                            HoverHandler {
                                id: preludeIconHover
                                cursorShape: Qt.PointingHandCursor
                            }
                            TapHandler {
                                onTapped: {
                                    preludeArea.text = mainPanel.prelude
                                    mainPanel.editingPrelude = true
                                    preludeArea.forceActiveFocus()
                                }
                            }
                            HoverBorder {
                                anchors.fill: parent
                                anchors.margins: -3
                                active: preludeIconHover.hovered
                            }
                        }
                    }

                    ColumnLayout {
                        id: preludeEditColumn
                        anchors.left: parent.left
                        anchors.right: parent.right
                        visible: mainPanel.editingPrelude
                        spacing: 4

                        TextArea {
                            id: preludeArea
                            Layout.fillWidth: true
                            wrapMode: TextArea.Wrap
                            color: "#dddddd"
                            font.pixelSize: 12
                            selectByMouse: true
                            selectByKeyboard: true
                            background: Rectangle {
                                color: "transparent"
                                radius: AppSettings.borderRadius
                                border.width: 1
                                border.color: "#5a5a5a"
                            }
                            Keys.onEscapePressed: mainPanel.editingPrelude = false
                        }

                        RowLayout {
                            Layout.alignment: Qt.AlignRight
                            spacing: 12

                            Label {
                                text: qsTr("Cancel")
                                color: "#e07a7a"
                                font.pixelSize: 11
                                HoverHandler { cursorShape: Qt.PointingHandCursor }
                                TapHandler { onTapped: mainPanel.editingPrelude = false }
                            }
                            Label {
                                text: qsTr("Save")
                                color: "#8fdf9f"
                                font.pixelSize: 11
                                font.bold: true
                                HoverHandler { cursorShape: Qt.PointingHandCursor }
                                TapHandler { onTapped: mainPanel.commitPrelude(preludeArea.text) }
                            }
                        }
                    }
                }

                // Feature 3c: collapsible chapter list — same tap-to-seek
                // idiom as the seek-bar tick marks (3b) and
                // timestampClickArea further below. Only ever visible once
                // mainPanel.chapters is non-empty (never for an orphaned
                // .md, same restriction as the prelude editor above — see
                // its own comment). Collapse toggle uses the same "+"/"−"
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
                    visible: mainPanel.orphanedMdPath === "" && mainPanel.chapters.length > 0
                    spacing: 4

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 6

                        Text {
                            text: mainPanel.chaptersExpanded ? "−" : "+"
                            color: "#aaaaaa"
                            font.pixelSize: 11

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
                                TextEdit {
                                    Layout.fillWidth: true
                                    clip: true
                                    readOnly: true
                                    selectByMouse: true
                                    selectByKeyboard: true
                                    persistentSelection: true
                                    activeFocusOnPress: false
                                    text: chapterRow.modelData.title
                                    color: "#cccccc"
                                    font.pixelSize: 11

                                    HoverHandler { cursorShape: Qt.IBeamCursor }
                                }
                            }
                        }
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

                    // Minimal pill toggle, matching Sidebar.qml's
                    // "Transcriptions" search-scope toggle (item 1, Wave E)
                    // rather than a QQC2 CheckBox — same on/off read via
                    // background+outline (green when on) as every other
                    // toggle-shaped control in this app.
                    Item {
                        id: speakerDetectionToggle
                        property bool checked: false
                        Layout.preferredWidth: speakerDetectionLabel.implicitWidth + 16
                        Layout.preferredHeight: 24

                        HoverHandler {
                            id: speakerDetectionHover
                            cursorShape: Qt.PointingHandCursor
                        }
                        TapHandler {
                            onTapped: {
                                speakerDetectionToggle.checked = !speakerDetectionToggle.checked
                                // Mirrors modelCombo's onActivated below —
                                // a manual toggle is remembered as the new
                                // vault-wide default going forward.
                                AppSettings.defaultSpeakerDetection = speakerDetectionToggle.checked
                            }
                        }
                        Rectangle {
                            anchors.fill: parent
                            radius: AppSettings.borderRadius
                            color: speakerDetectionToggle.checked
                                   ? (speakerDetectionHover.hovered ? "#2ecc71" : "#25a25a")
                                   : (speakerDetectionHover.hovered ? "#3a3a3a" : "#2a2a2a")
                            border.width: 1
                            border.color: speakerDetectionToggle.checked ? "#2ecc71" : "#4a4a4a"

                            Behavior on color { ColorAnimation { duration: 100 } }
                        }
                        Label {
                            id: speakerDetectionLabel
                            anchors.centerIn: parent
                            text: qsTr("Speaker detection")
                            color: speakerDetectionToggle.checked ? "white" : "#aaaaaa"
                            font.pixelSize: 11
                        }
                    }

                    Label {
                        text: qsTr("Model")
                        color: "#aaaaaa"
                    }

                    // Item P2: measures every model's label once (not
                    // per-delegate — a single throwaway FontMetrics reused
                    // across the whole list) so a long label (e.g. a
                    // multilingual "large-v3" entry) gets a box wide enough
                    // to show it in full, both closed and in the dropdown,
                    // instead of relying on elide to silently hide the tail.
                    FontMetrics {
                        id: modelLabelFontMetrics
                    }

                    ComboBox {
                        id: modelCombo
                        // 240 stays the floor for short label sets; the +64
                        // padding covers the dropdown row's own dot+spacing
                        // (8+6) plus the control's indicator/content
                        // padding, so the widest label never gets clipped
                        // in either the closed box or a delegate row (both
                        // are sized to modelCombo.width).
                        property real cachedMaxLabelWidth: 0
                        Layout.preferredWidth: Math.max(240, modelCombo.cachedMaxLabelWidth + 64)
                        model: mainPanel.transcriptionController ? mainPanel.transcriptionController.availableModels() : []
                        textRole: "label"
                        valueRole: "id"
                        hoverEnabled: true

                        Component.onCompleted: {
                            currentIndex = mainPanel.indexForModelId(AppSettings.defaultModel)
                            let maxWidth = 0
                            const models = mainPanel.transcriptionController ? mainPanel.transcriptionController.availableModels() : []
                            for (const m of models)
                                maxWidth = Math.max(maxWidth, modelLabelFontMetrics.advanceWidth(m.label))
                            modelCombo.cachedMaxLabelWidth = maxWidth
                        }
                        onActivated: AppSettings.defaultModel = currentValue

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

                        // O29: small dot per entry — green if that model's
                        // weights are already on disk, gray if picking it
                        // will first queue a download job.
                        delegate: ItemDelegate {
                            id: modelDelegate
                            required property var modelData
                            required property int index
                            width: modelCombo.width
                            highlighted: modelCombo.highlightedIndex === index

                            contentItem: RowLayout {
                                spacing: 6
                                Label {
                                    text: modelDelegate.modelData.label
                                    color: "white"
                                    Layout.fillWidth: true
                                    elide: Text.ElideRight
                                }
                                Rectangle {
                                    width: 8
                                    height: 8
                                    radius: 4
                                    color: modelDelegate.modelData.downloaded ? "#2ecc71" : "#555555"
                                    Layout.alignment: Qt.AlignVCenter
                                }
                            }
                        }

                        // Item P2: custom popup so its background can use
                        // the app's own corner radius (matches
                        // SettingsPopup.qml's own Popup background exactly)
                        // instead of QQC2's default square-cornered popup.
                        // The ListView here still drives itself off
                        // modelCombo.delegateModel (built from the
                        // top-level `model:`/`delegate:` above) — this is
                        // exactly how QQC2's OWN default ComboBox popup
                        // composes delegate+background internally, just
                        // with our own background/padding instead of the
                        // style's.
                        popup: Popup {
                            y: modelCombo.height
                            width: modelCombo.width
                            implicitHeight: Math.min(contentItem.implicitHeight, 320)
                            padding: 1

                            contentItem: ListView {
                                clip: true
                                implicitHeight: contentHeight
                                model: modelCombo.popup.visible ? modelCombo.delegateModel : null
                                currentIndex: modelCombo.highlightedIndex
                                ScrollIndicator.vertical: ScrollIndicator {}
                            }

                            background: Rectangle {
                                color: "#242424"
                                radius: AppSettings.borderRadius
                                border.width: 1
                                border.color: "#3a3a3a"
                            }
                        }
                    }

                    // O35: which backend actually loaded the active model
                    // ("Vulkan"/"CUDA"/"CPU"/…) — empty (and hidden) until
                    // the first model load completes in this session.
                    Label {
                        text: mainPanel.transcriptionController ? mainPanel.transcriptionController.activeBackendName : ""
                        visible: text !== ""
                        color: "#888888"
                        font.pixelSize: 11
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
                    model: mainPanel.displaySentences

                    TextEdit {
                        required property var modelData
                        required property int index
                        visible: modelData.showGutter && modelData.speaker !== ""
                        readonly property rect lineRect: transcriptArea.gutterLineRects[index] || Qt.rect(0, 0, 0, 0)
                        x: mainPanel.timestampGutterWidth + mainPanel.gutterSpacing
                        y: lineRect.y
                        height: lineRect.height
                        width: mainPanel.speakerGutterWidth
                        readOnly: true
                        selectByMouse: true
                        selectByKeyboard: true
                        persistentSelection: true
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
                    }
                }

                // Item P32: one heading row per chapter marker, interleaved
                // into displaySentences by displayData above (see its own
                // doc comment) — each heading pseudo-entry reserved a real
                // blank line in transcriptArea's own text, so
                // gutterLineRects[index] (same lookup the two gutter
                // Repeaters above use) gives it a genuine row of its own to
                // render into, rather than overlapping an adjacent
                // sentence's line. Spans the full transcript width
                // (including the gutter columns) since a chapter break
                // applies to the whole turn, not one speaker's column.
                Repeater {
                    id: chapterHeadingRepeater
                    model: mainPanel.displaySentences

                    RowLayout {
                        id: chapterHeadingRow
                        required property var modelData
                        required property int index
                        visible: chapterHeadingRow.modelData.isChapterHeading
                        readonly property rect lineRect: transcriptArea.gutterLineRects[index] || Qt.rect(0, 0, 0, 0)
                        x: 0
                        y: lineRect.y
                        width: transcriptArea.width
                        height: lineRect.height
                        spacing: 8

                        Rectangle {
                            Layout.preferredWidth: 16
                            Layout.preferredHeight: 1
                            Layout.alignment: Qt.AlignVCenter
                            color: "#3a3a3a"
                        }
                        Label {
                            Layout.alignment: Qt.AlignVCenter
                            text: chapterHeadingRow.modelData.chapterTitle
                            color: "#8fdf9f"
                            font.pixelSize: 12
                            font.bold: true
                        }
                        Rectangle {
                            Layout.fillWidth: true
                            Layout.preferredHeight: 1
                            Layout.alignment: Qt.AlignVCenter
                            color: "#3a3a3a"
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

        // Item 12 (Wave E, rewritten): Chrome/Firefox-style middle-click
        // autoscroll — two gestures, both ending in the SAME free-scroll
        // state: (1) a quick click (press+release, no real drag) arms
        // autoscroll that then free-scrolls just from moving the mouse,
        // no button held, until another click (any button) or Escape
        // disarms it; (2) press-hold-drag past a small threshold then
        // release disarms immediately, no second click needed.
        // draggedWhileHeld tells the two apart: it only flips true once
        // real movement exceeds the threshold WHILE the button is down,
        // so a plain click that never moved leaves release a no-op
        // (armed stays true) — that's what preserves gesture 1.
        // acceptedButtons only widens to Left/Right while armed (so any
        // button click cancels, matching Chrome) — while idle only
        // MiddleButton is accepted, so left-click text selection and the
        // scrollbar's own drag pass straight through untouched. Lives as
        // an overlay sibling of transcriptScrollView (not inside it, see
        // transcriptViewportContainer's doc comment) so it covers the same
        // full viewport without being counted as the ScrollView's content.
        MouseArea {
            id: middleDragScrollArea
            anchors.fill: parent
            acceptedButtons: middleDragScrollArea.armed
                             ? (Qt.LeftButton | Qt.MiddleButton | Qt.RightButton)
                             : Qt.MiddleButton
            hoverEnabled: true

            property bool armed: false
            property real originY: 0
            property real currentY: 0
            property bool buttonHeld: false
            property bool draggedWhileHeld: false
            readonly property int deadzone: 8

            cursorShape: {
                if (!middleDragScrollArea.armed)
                    return Qt.ArrowCursor
                const dy = Math.abs(middleDragScrollArea.currentY - middleDragScrollArea.originY)
                return dy <= middleDragScrollArea.deadzone ? Qt.SizeAllCursor : Qt.SizeVerCursor
            }

            onPressed: (mouse) => {
                // Any click while already armed cancels it (Chrome
                // convention) instead of performing that button's normal
                // action — acceptedButtons above ensures this branch is
                // the only one reachable for non-middle buttons.
                if (middleDragScrollArea.armed) {
                    middleDragScrollArea.armed = false
                    return
                }
                middleDragScrollArea.buttonHeld = true
                middleDragScrollArea.draggedWhileHeld = false
                middleDragScrollArea.armed = true
                const p = middleDragScrollArea.mapToItem(transcriptScrollView, mouse.x, mouse.y)
                middleDragScrollArea.originY = p.y
                middleDragScrollArea.currentY = p.y
            }
            onPositionChanged: (mouse) => {
                const p = middleDragScrollArea.mapToItem(transcriptScrollView, mouse.x, mouse.y)
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

            // Repeating timer (never runs while idle — gated by `armed`,
            // the same category as a Flickable's own flick-deceleration
            // animation) drives the actual scroll from the live
            // originY/currentY gap. Quadratic and uncapped: dragging
            // farther from the origin keeps accelerating rather than
            // plateauing — the Flickable's own content-size clamp below
            // is what actually stops it.
            Timer {
                interval: 16
                repeat: true
                running: middleDragScrollArea.armed
                onTriggered: {
                    const flick = transcriptScrollView.contentItem
                    const dy = middleDragScrollArea.currentY - middleDragScrollArea.originY
                    const deadzone = middleDragScrollArea.deadzone
                    if (Math.abs(dy) <= deadzone)
                        return
                    const beyond = dy - Math.sign(dy) * deadzone
                    const speed = Math.sign(beyond) * beyond * beyond * 0.002
                    const maxY = Math.max(0, flick.contentHeight - flick.height)
                    flick.contentY = Math.max(0, Math.min(maxY, flick.contentY + speed))
                }
            }
        }

        Shortcut {
            sequence: "Escape"
            enabled: middleDragScrollArea.armed
            onActivated: middleDragScrollArea.armed = false
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
