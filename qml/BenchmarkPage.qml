import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SuopTranscriber

// Benchmark page — a TOP-LEVEL page (its own TitleBar button, mounted by
// MainPanel inside a ScrollView; promoted out of Settings). Two sweeps over ONE
// file, both on the controller's worker thread so the UI stays responsive:
//   1. Model benchmark — every downloaded transcription model + available engine
//      over the file, transcripts side by side (TranscriptionController::
//      runBenchmark). runtimeMs / realtime factor / segment-word counts.
//   2. Diarization sweep — combinations of segmentation × embedding × sensitivity
//      (runDiarizationBenchmark), reporting how many speakers each finds; guarded
//      by a confirm above ~10 tests. This is how you settle "which combo actually
//      splits a hard far-field recording".
// Writes NO transcripts — the model benchmark's only on-disk footprint is the
// last-run cache (VaultTreeModel::writeBenchmarkLastRun), reloaded on show.
// Defaults to the file open in the left panel (currentFilePath); onShown() (fired
// via the `active` flag) refreshes lists since a gated parent hides it.
ColumnLayout {
    id: benchmarkPage

    property var transcriptionController: null
    property var vaultTreeModel: null

    // B2: the file open/highlighted in the left panel (fed from MainPanel's
    // "current file" expression). When the page opens, the benchmark defaults to
    // this file; empty ⇒ fall back to the vault-wide picker below.
    property string currentFilePath: ""

    // Set by the enclosing ScrollView's visibility (this page's own `visible`
    // stays true; onVisibleChanged wouldn't fire when a gated parent hides it).
    // Drives the on-show refresh below.
    property bool active: false
    onActiveChanged: if (active) benchmarkPage.onShown()

    // Item: the benchmark file follows the left panel. currentFilePath is fed
    // from MainPanel's current-file expression and changes whenever the user
    // clicks a different file in the tree — mirror it into selectedFilePath so
    // selecting a file there automatically switches what gets benchmarked, even
    // while this page is hidden. (onShown() also seeds the initial value, since
    // this only fires on CHANGE.)
    onCurrentFilePathChanged: benchmarkPage.selectedFilePath = benchmarkPage.currentFilePath

    function onShown() {
        // The vault is loaded by now — detect engines lazily, restore the last
        // run, and default to the open file.
        if (!benchmarkPage.enginesDetected)
            Qt.callLater(benchmarkPage.detectEngines)
        if (benchmarkPage.results.length === 0)
            benchmarkPage.loadLastRun()
        benchmarkPage.selectedFilePath = benchmarkPage.currentFilePath
        benchmarkPage.refreshModels()
        benchmarkPage.refreshDiarizationModels()
    }

    spacing: 10

    // Live-updated during a run (benchmarkModelDone appends); replaced wholesale
    // by the persisted run on load / by benchmarkFinished at the end.
    property var results: []
    property var lastRun: ({})
    property bool running: false
    property string statusText: ""
    property string selectedFilePath: ""

    // The selectable transcription models (whisper tiers + parakeet), with a
    // `downloaded` flag each. Refreshed IMPERATIVELY (availableModels() is a
    // Q_INVOKABLE with no NOTIFY, so a declarative binding would evaluate once
    // at page creation — before any vault/model state — and never again; the
    // same untracked-invokable trap SettingsPopup.qml documents). Re-fetched on
    // show AND on modelsChanged() so a just-finished download flips a row's
    // "(not downloaded)" state live.
    property var models: []
    // Which model/engine ids are checked to benchmark. Undownloaded ones ARE
    // selectable now (they're downloaded before the run — see startBenchmark);
    // seeded to everything downloaded + every available engine the first time.
    property var selectedModelIds: []
    property bool modelSelectionSeeded: false
    function refreshModels() {
        benchmarkPage.models = benchmarkPage.transcriptionController
                ? benchmarkPage.transcriptionController.availableModels() : []
        benchmarkPage.seedModelSelectionIfNeeded()
    }

    // Default the model selection to everything already runnable — downloaded
    // transcription models + every available engine — once, the first time
    // BOTH lists are known (engines are detected lazily, AFTER models, so
    // seeding must wait for them or the engines would never be pre-checked;
    // see detectEngines). Guarded so it never clobbers the user's own picks.
    function seedModelSelectionIfNeeded() {
        if (benchmarkPage.modelSelectionSeeded)
            return
        if (benchmarkPage.models.length === 0 || !benchmarkPage.enginesDetected)
            return
        const seed = []
        for (let i = 0; i < benchmarkPage.models.length; i++)
            if (benchmarkPage.models[i].downloaded)
                seed.push(benchmarkPage.models[i].id)
        for (let j = 0; j < benchmarkPage.engineList.length; j++)
            if (benchmarkPage.engineList[j].available)
                seed.push(benchmarkPage.engineList[j].modelId)
        benchmarkPage.selectedModelIds = seed
        benchmarkPage.modelSelectionSeeded = true
    }

    // Engine capability detection is comparatively slow (probes GPU + runs each
    // engine's dependency self-test), so it's done once, lazily, after the page
    // first shows — not on every settings open.
    property var engineList: []
    property bool enginesDetected: false

    function formatDuration(seconds) {
        if (!seconds || seconds < 0)
            return "0:00"
        const s = Math.round(seconds)
        const m = Math.floor(s / 60)
        const r = s % 60
        return m + ":" + (r < 10 ? "0" + r : r)
    }

    // Whether a model/engine id currently needs downloading before it can run.
    // Engines are availability-gated (weights live outside the app), never
    // "downloadable", so only transcription models can be pending.
    function isModelDownloaded(id) {
        for (let i = 0; i < benchmarkPage.models.length; i++)
            if (benchmarkPage.models[i].id === id)
                return benchmarkPage.models[i].downloaded === true
        return true // an engine id (or unknown) — treated as ready
    }

    // The model/engine ids to benchmark: exactly what the user checked.
    function modelIdsToRun() {
        return benchmarkPage.selectedModelIds.slice()
    }

    function detectEngines() {
        benchmarkPage.engineList = benchmarkPage.transcriptionController
                ? benchmarkPage.transcriptionController.availableEngines() : []
        benchmarkPage.enginesDetected = true
    }

    function loadLastRun() {
        if (!benchmarkPage.vaultTreeModel)
            return
        const run = benchmarkPage.vaultTreeModel.readBenchmarkLastRun()
        if (run && run.results) {
            benchmarkPage.lastRun = run
            benchmarkPage.results = run.results
            // NOTE: no longer restores selectedFilePath — the benchmark file
            // follows the left-panel selection (currentFilePath), not the last
            // run's file (item: removed the in-page picker).
        }
    }

    // A run waiting on model downloads before it can start (item: you can pick
    // undownloaded models and they're fetched first). {kind:"model"|"diar",
    // requiredIds:[...], ids?/combos?}. onModelsChanged launches it once every
    // requiredId is present; cancelling clears it.
    property var pendingRun: null

    // The result whose full transcript the click-to-expand popup is showing.
    // Set by clicking a result card; the popup (transcriptPreviewPopup) binds
    // its header + transcript to it. Empty object when nothing is open.
    property var previewResult: ({})
    function openTranscriptPreview(result) {
        benchmarkPage.previewResult = result
        transcriptPreviewPopup.open()
    }

    function missingModelIds(ids) {
        const missing = []
        for (let i = 0; i < ids.length; i++)
            if (!benchmarkPage.isModelDownloaded(ids[i]))
                missing.push(ids[i])
        return missing
    }
    function isDiarModelDownloaded(id) {
        for (let i = 0; i < benchmarkPage.segModels.length; i++)
            if (benchmarkPage.segModels[i].id === id)
                return benchmarkPage.segModels[i].downloaded === true
        for (let i = 0; i < benchmarkPage.embedModels.length; i++)
            if (benchmarkPage.embedModels[i].id === id)
                return benchmarkPage.embedModels[i].downloaded === true
        return true
    }
    function startDownloads(ids) {
        for (let i = 0; i < ids.length; i++)
            benchmarkPage.transcriptionController.downloadModel(ids[i])
    }

    function startBenchmark() {
        if (!benchmarkPage.transcriptionController || benchmarkPage.selectedFilePath === "")
            return
        if (benchmarkPage.running || benchmarkPage.diarizationRunning || benchmarkPage.pendingRun)
            return
        if (!benchmarkPage.enginesDetected)
            benchmarkPage.detectEngines()
        const ids = benchmarkPage.modelIdsToRun()
        if (ids.length === 0) {
            benchmarkPage.statusText = qsTr("Select at least one model to benchmark.")
            return
        }
        const missing = benchmarkPage.missingModelIds(ids)
        if (missing.length > 0) {
            // Download the picked-but-undownloaded models first, then run
            // automatically once they land (see onModelsChanged).
            benchmarkPage.pendingRun = {kind: "model", requiredIds: ids, ids: ids}
            benchmarkPage.statusText = qsTr("Downloading %1 model(s) before running…").arg(missing.length)
            benchmarkPage.startDownloads(missing)
            return
        }
        benchmarkPage.executeBenchmark(ids)
    }

    function executeBenchmark(ids) {
        benchmarkPage.results = []
        benchmarkPage.running = true
        benchmarkPage.statusText = qsTr("Starting…")
        benchmarkPage.transcriptionController.runBenchmark("file://" + benchmarkPage.selectedFilePath, ids)
    }

    // --- Diarization sweep (segmentation × embedding × sensitivity) -----------
    property var segModels: []
    property var embedModels: []
    property var selectedSegIds: []
    property var selectedEmbedIds: []
    // The candidate sensitivity values a sweep can include (B4 warning triggers
    // on the full combo count = segs × embeds × thresholds).
    readonly property var thresholdChoices: [0.3, 0.4, 0.5, 0.6, 0.7]
    property var selectedThresholds: [0.3, 0.5, 0.7]
    property var diarizationResults: []
    property bool diarizationRunning: false
    property string diarizationStatus: ""

    function refreshDiarizationModels() {
        if (!benchmarkPage.transcriptionController)
            return
        benchmarkPage.segModels = benchmarkPage.transcriptionController.availableSegmentationModels()
        benchmarkPage.embedModels = benchmarkPage.transcriptionController.availableEmbeddingModels()
        // Default the selection to the currently-configured pair the first time.
        if (benchmarkPage.selectedSegIds.length === 0)
            benchmarkPage.selectedSegIds = [AppSettings.diarizationSegmentationModel]
        if (benchmarkPage.selectedEmbedIds.length === 0)
            benchmarkPage.selectedEmbedIds = [AppSettings.diarizationEmbeddingModel]
    }

    function toggleIn(arr, val) {
        const copy = arr.slice()
        const i = copy.indexOf(val)
        if (i >= 0)
            copy.splice(i, 1)
        else
            copy.push(val)
        return copy
    }

    function labelForId(list, id) {
        for (let i = 0; i < list.length; i++)
            if (list[i].id === id)
                return list[i].label
        return id
    }

    function diarizationComboCount() {
        return benchmarkPage.selectedSegIds.length * benchmarkPage.selectedEmbedIds.length
                * benchmarkPage.selectedThresholds.length
    }

    function buildDiarizationCombos() {
        const combos = []
        const segs = benchmarkPage.selectedSegIds
        const embs = benchmarkPage.selectedEmbedIds
        const ths = benchmarkPage.selectedThresholds.slice().sort(function (a, b) { return a - b })
        for (let s = 0; s < segs.length; s++)
            for (let e = 0; e < embs.length; e++)
                for (let t = 0; t < ths.length; t++)
                    combos.push({
                        segmentationModelId: segs[s],
                        embeddingModelId: embs[e],
                        threshold: ths[t],
                        label: benchmarkPage.labelForId(benchmarkPage.segModels, segs[s]) + "  +  "
                               + benchmarkPage.labelForId(benchmarkPage.embedModels, embs[e]) + "  @ "
                               + ths[t].toFixed(2)
                    })
        return combos
    }

    // B4: guard a large sweep behind a confirm.
    function startDiarizationBenchmark() {
        if (!benchmarkPage.transcriptionController || benchmarkPage.selectedFilePath === "")
            return
        if (benchmarkPage.running || benchmarkPage.diarizationRunning || benchmarkPage.pendingRun)
            return
        const combos = benchmarkPage.buildDiarizationCombos()
        if (combos.length === 0) {
            benchmarkPage.diarizationStatus = qsTr("Pick at least one segmentation model, one embedding model, "
                                                   + "and one sensitivity.")
            return
        }
        if (combos.length > 10) {
            diarizationConfirmDialog.pendingCombos = combos
            diarizationConfirmDialog.open()
            return
        }
        benchmarkPage.runDiarizationSweep(combos)
    }

    function runDiarizationSweep(combos) {
        // Ensure the segmentation/embedding models the combos use are
        // downloaded first (undownloaded ones are now selectable), then run.
        const req = []
        for (let i = 0; i < combos.length; i++) {
            if (req.indexOf(combos[i].segmentationModelId) < 0)
                req.push(combos[i].segmentationModelId)
            if (req.indexOf(combos[i].embeddingModelId) < 0)
                req.push(combos[i].embeddingModelId)
        }
        const missing = []
        for (let j = 0; j < req.length; j++)
            if (!benchmarkPage.isDiarModelDownloaded(req[j]))
                missing.push(req[j])
        if (missing.length > 0) {
            benchmarkPage.pendingRun = {kind: "diar", requiredIds: req, combos: combos}
            benchmarkPage.diarizationStatus = qsTr("Downloading %1 model(s) before running…").arg(missing.length)
            benchmarkPage.startDownloads(missing)
            return
        }
        benchmarkPage.executeDiarizationSweep(combos)
    }

    function executeDiarizationSweep(combos) {
        benchmarkPage.diarizationResults = []
        benchmarkPage.diarizationRunning = true
        benchmarkPage.diarizationStatus = qsTr("Starting %1 tests…").arg(combos.length)
        benchmarkPage.transcriptionController.runDiarizationBenchmark(
                    "file://" + benchmarkPage.selectedFilePath, combos)
    }

    // Launches a download-pending run once every required model has landed.
    function tryLaunchPendingRun() {
        const pr = benchmarkPage.pendingRun
        if (!pr)
            return
        for (let i = 0; i < pr.requiredIds.length; i++) {
            const ready = pr.kind === "diar" ? benchmarkPage.isDiarModelDownloaded(pr.requiredIds[i])
                                             : benchmarkPage.isModelDownloaded(pr.requiredIds[i])
            if (!ready)
                return // still waiting on at least one download
        }
        benchmarkPage.pendingRun = null
        if (pr.kind === "diar")
            benchmarkPage.executeDiarizationSweep(pr.combos)
        else
            benchmarkPage.executeBenchmark(pr.ids)
    }

    Component.onCompleted: {
        benchmarkPage.refreshModels()
        benchmarkPage.loadLastRun()
    }

    Connections {
        target: benchmarkPage.transcriptionController
        enabled: benchmarkPage.transcriptionController !== null

        function onBenchmarkModelStarted(modelId, index, total) {
            benchmarkPage.statusText = qsTr("Running %1 of %2…").arg(index + 1).arg(total)
            // Placeholder card the live text grows into; finalized by
            // onBenchmarkModelDone. label falls back to the id until the result
            // lands. `running: true` selects the "transcribing…" card state.
            benchmarkPage.results = benchmarkPage.results.concat([{
                modelId: modelId, label: modelId, running: true, text: "", ok: undefined
            }])
        }
        // Grow the running card's text as segments stream in (index-keyed so a
        // sweep with duplicate ids still addresses the right card).
        function onBenchmarkModelProgressText(modelIndex, partialText) {
            const r = benchmarkPage.results.slice()
            if (modelIndex >= 0 && modelIndex < r.length) {
                r[modelIndex] = Object.assign({}, r[modelIndex], {text: partialText})
                benchmarkPage.results = r
            }
        }
        function onBenchmarkModelDone(result) {
            const r = benchmarkPage.results.slice()
            // Replace the trailing running placeholder for this model in place
            // (index-keyed) instead of appending a duplicate.
            let idx = -1
            for (let k = r.length - 1; k >= 0; k--)
                if (r[k].running && r[k].modelId === result.modelId) { idx = k; break }
            if (idx >= 0) r[idx] = result
            else r.push(result)
            benchmarkPage.results = r
        }
        function onBenchmarkFinished(run) {
            benchmarkPage.running = false
            benchmarkPage.lastRun = run
            benchmarkPage.results = run.results
            benchmarkPage.statusText = run.cancelled ? qsTr("Cancelled.") : qsTr("Done.")
            if (benchmarkPage.vaultTreeModel)
                benchmarkPage.vaultTreeModel.writeBenchmarkLastRun(run)
        }
        function onBenchmarkFailed(error) {
            benchmarkPage.running = false
            benchmarkPage.diarizationRunning = false
            benchmarkPage.statusText = qsTr("Failed: %1").arg(error)
        }

        function onDiarizationBenchmarkComboStarted(label, index, total) {
            benchmarkPage.diarizationStatus = qsTr("Running %1 of %2…").arg(index + 1).arg(total)
        }
        function onDiarizationBenchmarkComboDone(result) {
            benchmarkPage.diarizationResults = benchmarkPage.diarizationResults.concat([result])
        }
        function onDiarizationBenchmarkFinished(run) {
            benchmarkPage.diarizationRunning = false
            benchmarkPage.diarizationResults = run.results
            benchmarkPage.diarizationStatus = run.cancelled ? qsTr("Cancelled.") : qsTr("Done.")
        }

        // Item: refresh the model lists when a download finishes (or any
        // model op) so "(not downloaded)" states update live — and launch a
        // run that was waiting on those downloads.
        function onModelsChanged() {
            benchmarkPage.refreshModels()
            benchmarkPage.refreshDiarizationModels()
            benchmarkPage.tryLaunchPendingRun()
        }
    }

    // B4: confirmation before a large sweep.
    Dialog {
        id: diarizationConfirmDialog
        property var pendingCombos: []
        modal: true
        anchors.centerIn: Overlay.overlay
        title: qsTr("Run a large sweep?")
        standardButtons: Dialog.Ok | Dialog.Cancel
        onAccepted: benchmarkPage.runDiarizationSweep(diarizationConfirmDialog.pendingCombos)
        // Break the Dialog implicitWidth binding loop by giving the content a
        // CONSTANT implicitWidth (320) that has no dependency to feed back: a
        // bare WordWrap Label as contentItem reports its full unwrapped
        // implicitWidth, which the Dialog sizes to, which changes the wrap,
        // which changes the implicitWidth — the loop. The wrapper Item pins
        // implicitWidth to 320 (a literal, not derived from the Label), and the
        // Label just fills that fixed width and wraps within it. (contentWidth
        // and Layout.maximumWidth both only downgraded the loop to a warning,
        // since the Dialog still read the Label's own implicitWidth.)
        contentItem: Item {
            implicitWidth: 320
            implicitHeight: sweepConfirmLabel.implicitHeight
            Label {
                id: sweepConfirmLabel
                width: parent.width
                wrapMode: Text.WordWrap
                text: qsTr("This runs %1 diarization tests over the file — each loads models and runs a full "
                           + "pass, so it can take a while. Continue?").arg(diarizationConfirmDialog.pendingCombos.length)
                color: "white"
            }
        }
    }

    Label {
        text: qsTr("Model Benchmark")
        color: "white"
        font.bold: true
    }
    Label {
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        text: qsTr("Pick models to run on one file and compare. Undownloaded models are downloaded "
                   + "before the run. Nothing is saved to the vault — this is for comparison only.")
        color: Theme.textMuted
        font.pixelSize: 12
    }

    // --- Model selection -----------------------------------------------------
    // Item: transcription models (whisper tiers + parakeet) plus available
    // engines. Undownloaded models ARE selectable — startBenchmark downloads
    // any checked-but-missing one first, then runs. The list refreshes on
    // modelsChanged so a finished download drops the "(not downloaded)" note.
    Label { text: qsTr("Models"); color: Theme.textPrimary; font.pixelSize: 12 }
    Flow {
        Layout.fillWidth: true
        spacing: 12
        Repeater {
            model: benchmarkPage.models
            CheckBox {
                required property var modelData
                text: modelData.label + (modelData.downloaded ? "" : qsTr(" (not downloaded)"))
                checked: benchmarkPage.selectedModelIds.indexOf(modelData.id) >= 0
                contentItem: Label {
                    text: parent.text
                    color: "white"
                    leftPadding: parent.indicator.width + 6
                    verticalAlignment: Text.AlignVCenter
                }
                onToggled: benchmarkPage.selectedModelIds =
                           benchmarkPage.toggleIn(benchmarkPage.selectedModelIds, modelData.id)
                HoverHandler { cursorShape: Qt.PointingHandCursor }
            }
        }
        Repeater {
            model: benchmarkPage.engineList
            CheckBox {
                required property var modelData
                visible: modelData.available
                text: modelData.label
                checked: benchmarkPage.selectedModelIds.indexOf(modelData.modelId) >= 0
                contentItem: Label {
                    text: parent.text
                    color: "white"
                    leftPadding: parent.indicator.width + 6
                    verticalAlignment: Text.AlignVCenter
                }
                onToggled: benchmarkPage.selectedModelIds =
                           benchmarkPage.toggleIn(benchmarkPage.selectedModelIds, modelData.modelId)
                HoverHandler { cursorShape: Qt.PointingHandCursor }
            }
        }
    }

    // --- Selected file + run controls ----------------------------------------
    // Item: no in-page file picker — the benchmark file always follows the
    // left-panel selection (currentFilePath), which is the same file the rest
    // of the app is looking at. selectedFilePath tracks it (see
    // onCurrentFilePathChanged). This label just shows what will be benchmarked.
    RowLayout {
        Layout.fillWidth: true
        spacing: 8

        Label {
            Layout.fillWidth: true
            text: benchmarkPage.selectedFilePath === ""
                  ? qsTr("No file selected — pick one in the left panel")
                  : benchmarkPage.selectedFilePath.substring(
                        benchmarkPage.selectedFilePath.lastIndexOf("/") + 1)
            color: benchmarkPage.selectedFilePath === "" ? "#c98500" : Theme.textPrimary
            font.pixelSize: 12
            elide: Text.ElideRight
        }

        Button {
            id: runButton
            text: benchmarkPage.pendingRun && benchmarkPage.pendingRun.kind === "model"
                  ? qsTr("Downloading…")
                  : benchmarkPage.running ? qsTr("Running…") : qsTr("Run benchmark")
            // Item: only one benchmark at a time — both sweeps share the single
            // worker thread, so a run is blocked while EITHER is in progress or
            // waiting on model downloads (cancel to start a different one).
            enabled: !benchmarkPage.running && !benchmarkPage.diarizationRunning
                     && !benchmarkPage.pendingRun && benchmarkPage.selectedFilePath !== ""
            opacity: enabled ? 1 : 0.4
            onClicked: benchmarkPage.startBenchmark()
            background: Rectangle {
                radius: AppSettings.borderRadius
                color: (runButton.hovered && runButton.enabled) ? Theme.surfaceHover : Theme.surface
                border.width: 1
                border.color: Theme.border
            }
            HoverHandler { cursorShape: Qt.PointingHandCursor; enabled: runButton.enabled }
            HoverBorder { anchors.fill: parent; active: runButton.enabled && runButton.hovered }
        }

        Button {
            id: cancelButton
            visible: benchmarkPage.running
                     || (benchmarkPage.pendingRun && benchmarkPage.pendingRun.kind === "model")
            text: qsTr("Cancel")
            onClicked: {
                if (benchmarkPage.running && benchmarkPage.transcriptionController)
                    benchmarkPage.transcriptionController.cancelBenchmark()
                else if (benchmarkPage.pendingRun) {
                    // Stop waiting to auto-run; the queued downloads keep going
                    // (they're in the tasks list) but won't trigger a run now.
                    benchmarkPage.pendingRun = null
                    benchmarkPage.statusText = qsTr("Cancelled.")
                }
            }
            background: Rectangle {
                radius: AppSettings.borderRadius
                color: cancelButton.hovered ? Theme.surfaceHover : Theme.surface
                border.width: 1
                border.color: Theme.border
            }
            HoverHandler { cursorShape: Qt.PointingHandCursor }
            HoverBorder { anchors.fill: parent; active: cancelButton.hovered }
        }
    }

    Label {
        visible: benchmarkPage.statusText !== ""
        text: benchmarkPage.statusText
        color: "#cccccc"
        font.pixelSize: 12
    }

    // --- Engine capability detection (surfaces "Parakeet: unavailable — …") --
    ColumnLayout {
        Layout.fillWidth: true
        spacing: 3
        visible: benchmarkPage.engineList.length > 0
        Repeater {
            model: benchmarkPage.engineList
            RowLayout {
                required property var modelData
                Layout.fillWidth: true
                spacing: 6
                Rectangle {
                    implicitWidth: 8; implicitHeight: 8; radius: 4
                    color: modelData.available ? Theme.success : "#777777"
                }
                Label {
                    text: modelData.label
                    color: "white"
                    font.pixelSize: 12
                }
                Label {
                    Layout.fillWidth: true
                    text: modelData.available ? qsTr("engine available")
                                              : qsTr("unavailable — %1").arg(modelData.unavailableReason)
                    color: modelData.available ? "#8fce9e" : "#c98500"
                    font.pixelSize: 11
                    elide: Text.ElideRight
                }
            }
        }
    }

    // --- Heuristics header ---------------------------------------------------
    RowLayout {
        Layout.fillWidth: true
        visible: benchmarkPage.lastRun && benchmarkPage.lastRun.audioSeconds !== undefined
        spacing: 16
        Label {
            text: qsTr("Audio: %1").arg(benchmarkPage.formatDuration(
                      benchmarkPage.lastRun ? benchmarkPage.lastRun.audioSeconds : 0))
            color: Theme.textPrimary; font.pixelSize: 12
        }
        Label {
            text: qsTr("Models: %1").arg(benchmarkPage.results.length)
            color: Theme.textPrimary; font.pixelSize: 12
        }
        Label {
            visible: benchmarkPage.lastRun && benchmarkPage.lastRun.totalRuntimeMs !== undefined
            text: qsTr("Total: %1s").arg(((benchmarkPage.lastRun ? benchmarkPage.lastRun.totalRuntimeMs : 0)
                                          / 1000).toFixed(1))
            color: Theme.textPrimary; font.pixelSize: 12
        }
    }

    // --- Per-model result cards (square, wrapping grid) ----------------------
    // A Flow so the fixed-size square cards wrap to as many columns as the page
    // width allows — one column on a narrow window, several on a wide one (the
    // "flex layout for large screens"). The transcript itself is NOT on the
    // card: it's a short elided preview, and clicking a card with a transcript
    // opens the full text in transcriptPreviewPopup (scrollable, middle-drag).
    Flow {
        id: resultsFlow
        objectName: "resultsFlow"
        Layout.fillWidth: true
        Layout.topMargin: 4
        spacing: 12
        Repeater {
            model: benchmarkPage.results
            Rectangle {
                id: card
                objectName: "resultCard"
                required property var modelData
                // Only ok results with actual text are clickable/enlargeable.
                readonly property bool hasTranscript:
                    card.modelData.ok === true && (card.modelData.text || "") !== ""

                width: 240
                height: 240
                radius: AppSettings.borderRadius
                color: (card.hasTranscript && cardHover.hovered) ? "#333333" : Theme.surface
                border.width: 1
                border.color: Theme.border

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 8
                    spacing: 4

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8
                        Label {
                            Layout.fillWidth: true
                            text: card.modelData.label
                            color: "white"
                            font.bold: true
                            font.pixelSize: 13
                            elide: Text.ElideRight
                        }
                        Label {
                            objectName: "cardKindBadge"
                            text: card.modelData.kind === "engine" ? qsTr("engine")
                                  : (card.modelData.kind === "parakeet" ? qsTr("parakeet")
                                     : qsTr("whisper"))
                            color: Theme.textSecondary
                            font.pixelSize: 10
                        }
                    }

                    Label {
                        visible: card.modelData.ok === true
                        text: qsTr("%1s · %2× realtime")
                              .arg(((card.modelData.runtimeMs || 0) / 1000).toFixed(1))
                              .arg((card.modelData.realtimeFactor || 0).toFixed(1))
                        color: "#8fce9e"
                        font.pixelSize: 11
                    }
                    Label {
                        visible: card.modelData.ok === true
                        text: qsTr("%1 seg · %2 words · %3 chars")
                              .arg(card.modelData.segmentCount || 0)
                              .arg(card.modelData.wordCount || 0)
                              .arg(card.modelData.charCount || 0)
                        color: "#999999"
                        font.pixelSize: 10
                    }

                    // Still transcribing: muted affordance while live text grows
                    // in below (ok is still undefined for a running placeholder).
                    Label {
                        visible: card.modelData.running === true
                        text: qsTr("transcribing…")
                        color: Theme.textSecondary
                        font.pixelSize: 11
                    }

                    // Failed/unavailable model: show why, no transcript. Not a
                    // running placeholder (which also has ok !== true).
                    Label {
                        visible: card.modelData.ok !== true && card.modelData.running !== true
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        wrapMode: Text.WordWrap
                        elide: Text.ElideRight
                        text: qsTr("Skipped: %1").arg(card.modelData.error || qsTr("unavailable"))
                        color: "#c98500"
                        font.pixelSize: 12
                    }

                    // Transcript PREVIEW — short, elided, fills the rest of the
                    // square. Shows live text while running AND the final text
                    // when ok; the full text lives in the click-to-expand popup.
                    Text {
                        visible: (card.modelData.text || "") !== ""
                                 && (card.modelData.ok === true || card.modelData.running === true)
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        clip: true
                        wrapMode: Text.WordWrap
                        elide: Text.ElideRight
                        text: card.modelData.text || ""
                        color: "#bbbbbb"
                        font.pixelSize: 11
                    }

                    // Affordance that the card expands.
                    Label {
                        visible: card.hasTranscript
                        text: qsTr("Click to expand")
                        color: "#777777"
                        font.pixelSize: 10
                    }
                }

                // Clickable only when there's a transcript to enlarge; the
                // StyleGuide hover pair (pointing cursor + 1px HoverBorder).
                HoverHandler {
                    id: cardHover
                    enabled: card.hasTranscript
                    cursorShape: Qt.PointingHandCursor
                }
                HoverBorder { anchors.fill: parent; active: card.hasTranscript && cardHover.hovered }
                TapHandler {
                    enabled: card.hasTranscript
                    onTapped: benchmarkPage.openTranscriptPreview(card.modelData)
                }
            }
        }
    }

    // Click-to-expand transcript popup — the full model transcript, scrollable
    // by wheel AND by the app's Chrome-style middle-mouse autoscroll
    // (MiddleDragScroll, same as the main transcript view / settings / tree).
    // contentItem is a plain Item (NOT a Layout) so the ScrollView and the
    // MiddleDragScroll overlay can be siblings that anchor to each other — the
    // overlay must sit OVER the ScrollView, not be laid out as content of it
    // (see MiddleDragScroll.qml and SettingsPopup.qml's middleDragScrollArea).
    Popup {
        id: transcriptPreviewPopup
        objectName: "transcriptPreviewPopup"
        parent: Overlay.overlay
        modal: true
        anchors.centerIn: Overlay.overlay
        width: Overlay.overlay ? Math.min(760, Overlay.overlay.width - 80) : 600
        height: Overlay.overlay ? Math.min(620, Overlay.overlay.height - 80) : 500
        // Suspended while the autoscroll is armed: Escape / click-outside are
        // how Chrome-style autoscroll cancels itself, so let it disarm the
        // scroll instead of closing the popup (SettingsPopup does the same).
        closePolicy: middleDragPreview.armed
                     ? Popup.NoAutoClose
                     : (Popup.CloseOnEscape | Popup.CloseOnPressOutside)
        background: Rectangle {
            radius: AppSettings.borderRadius
            color: "#1e1e1e"
            border.width: 1
            border.color: Theme.border
        }

        contentItem: Item {
            RowLayout {
                id: previewHeader
                anchors.top: parent.top
                anchors.left: parent.left
                anchors.right: parent.right
                spacing: 8
                Label {
                    Layout.fillWidth: true
                    text: benchmarkPage.previewResult.label || ""
                    color: "white"
                    font.bold: true
                    font.pixelSize: 14
                    elide: Text.ElideRight
                }
                Label {
                    text: benchmarkPage.previewResult.ok === true
                          ? qsTr("%1s · %2× realtime")
                            .arg(((benchmarkPage.previewResult.runtimeMs || 0) / 1000).toFixed(1))
                            .arg((benchmarkPage.previewResult.realtimeFactor || 0).toFixed(1))
                          : ""
                    color: "#8fce9e"
                    font.pixelSize: 12
                }
            }

            ScrollView {
                id: previewScroll
                anchors.top: previewHeader.bottom
                anchors.topMargin: 8
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                clip: true
                TextArea {
                    id: previewTranscriptText
                    objectName: "previewTranscriptText"
                    readOnly: true
                    wrapMode: TextArea.WordWrap
                    selectByMouse: true
                    text: benchmarkPage.previewResult.text || ""
                    color: Theme.textPrimary
                    font.pixelSize: 13
                    background: null
                    // StyleGuide "editable/selectable text": I-beam cursor.
                    HoverHandler { cursorShape: Qt.IBeamCursor }
                }
            }

            // Chrome-style middle-click autoscroll over the transcript — a
            // sibling overlay of previewScroll (not its content), sized over it.
            MiddleDragScroll {
                id: middleDragPreview
                anchors.fill: previewScroll
                flickable: previewScroll.contentItem as Flickable
            }
        }
    }

    // ===== Diarization sweep =================================================
    Rectangle {
        Layout.fillWidth: true
        Layout.topMargin: 12
        Layout.preferredHeight: 1
        color: Theme.border
    }

    Label {
        text: qsTr("Diarization sweep")
        color: "white"
        font.bold: true
    }
    Label {
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        text: qsTr("Run combinations of segmentation × embedding × sensitivity over the same file and "
                   + "compare how many speakers each finds. Undownloaded models are downloaded before the "
                   + "run. Nothing is saved to the vault.")
        color: Theme.textMuted
        font.pixelSize: 12
    }

    // Segmentation model checkboxes.
    Label { text: qsTr("Segmentation models"); color: Theme.textPrimary; font.pixelSize: 12 }
    Flow {
        Layout.fillWidth: true
        spacing: 12
        Repeater {
            model: benchmarkPage.segModels
            CheckBox {
                required property var modelData
                // Selectable even when not downloaded — it's fetched before the
                // run (see runDiarizationSweep). The "(not downloaded)" note +
                // dimmer text is just an indicator now, not a disable.
                text: modelData.label + (modelData.downloaded ? "" : qsTr(" (not downloaded)"))
                checked: benchmarkPage.selectedSegIds.indexOf(modelData.id) >= 0
                contentItem: Label {
                    text: parent.text
                    color: modelData.downloaded ? "white" : Theme.textMuted
                    leftPadding: parent.indicator.width + 6
                    verticalAlignment: Text.AlignVCenter
                }
                onToggled: benchmarkPage.selectedSegIds = benchmarkPage.toggleIn(benchmarkPage.selectedSegIds,
                                                                                  modelData.id)
                HoverHandler { cursorShape: Qt.PointingHandCursor }
            }
        }
    }

    // Embedding model checkboxes.
    Label { text: qsTr("Embedding models"); color: Theme.textPrimary; font.pixelSize: 12 }
    Flow {
        Layout.fillWidth: true
        spacing: 12
        Repeater {
            model: benchmarkPage.embedModels
            CheckBox {
                required property var modelData
                // Selectable even when not downloaded (fetched before the run).
                text: modelData.label + (modelData.downloaded ? "" : qsTr(" (not downloaded)"))
                checked: benchmarkPage.selectedEmbedIds.indexOf(modelData.id) >= 0
                contentItem: Label {
                    text: parent.text
                    color: modelData.downloaded ? "white" : Theme.textMuted
                    leftPadding: parent.indicator.width + 6
                    verticalAlignment: Text.AlignVCenter
                }
                onToggled: benchmarkPage.selectedEmbedIds = benchmarkPage.toggleIn(benchmarkPage.selectedEmbedIds,
                                                                                    modelData.id)
                HoverHandler { cursorShape: Qt.PointingHandCursor }
            }
        }
    }

    // Sensitivity value chips.
    Label { text: qsTr("Sensitivity values (lower = more speakers)"); color: Theme.textPrimary; font.pixelSize: 12 }
    Flow {
        Layout.fillWidth: true
        spacing: 12
        Repeater {
            model: benchmarkPage.thresholdChoices
            CheckBox {
                required property var modelData
                text: Number(modelData).toFixed(2)
                checked: benchmarkPage.selectedThresholds.indexOf(modelData) >= 0
                contentItem: Label {
                    text: parent.text
                    color: "white"
                    leftPadding: parent.indicator.width + 6
                    verticalAlignment: Text.AlignVCenter
                }
                onToggled: benchmarkPage.selectedThresholds = benchmarkPage.toggleIn(benchmarkPage.selectedThresholds,
                                                                                      modelData)
                HoverHandler { cursorShape: Qt.PointingHandCursor }
            }
        }
    }

    RowLayout {
        Layout.fillWidth: true
        spacing: 8
        Label {
            Layout.fillWidth: true
            text: qsTr("%1 tests").arg(benchmarkPage.diarizationComboCount())
                  + (benchmarkPage.diarizationComboCount() > 10 ? qsTr("  ⚠ large sweep") : "")
            color: benchmarkPage.diarizationComboCount() > 10 ? "#c9a227" : Theme.textPrimary
            font.pixelSize: 12
        }
        Button {
            id: diarRunButton
            text: benchmarkPage.pendingRun && benchmarkPage.pendingRun.kind === "diar"
                  ? qsTr("Downloading…")
                  : benchmarkPage.diarizationRunning ? qsTr("Running…") : qsTr("Run diarization sweep")
            // Item: blocked while EITHER sweep runs or is waiting on downloads
            // (shared worker thread).
            enabled: !benchmarkPage.diarizationRunning && !benchmarkPage.running
                     && !benchmarkPage.pendingRun
                     && benchmarkPage.selectedFilePath !== ""
                     && benchmarkPage.diarizationComboCount() > 0
            opacity: enabled ? 1 : 0.4
            onClicked: benchmarkPage.startDiarizationBenchmark()
            background: Rectangle {
                radius: AppSettings.borderRadius
                color: (diarRunButton.hovered && diarRunButton.enabled) ? Theme.surfaceHover : Theme.surface
                border.width: 1
                border.color: Theme.border
            }
            HoverHandler { cursorShape: Qt.PointingHandCursor; enabled: diarRunButton.enabled }
            HoverBorder { anchors.fill: parent; active: diarRunButton.enabled && diarRunButton.hovered }
        }
        Button {
            id: diarCancelButton
            visible: benchmarkPage.diarizationRunning
                     || (benchmarkPage.pendingRun && benchmarkPage.pendingRun.kind === "diar")
            text: qsTr("Cancel")
            onClicked: {
                if (benchmarkPage.diarizationRunning && benchmarkPage.transcriptionController)
                    benchmarkPage.transcriptionController.cancelBenchmark()
                else if (benchmarkPage.pendingRun) {
                    benchmarkPage.pendingRun = null
                    benchmarkPage.diarizationStatus = qsTr("Cancelled.")
                }
            }
            background: Rectangle {
                radius: AppSettings.borderRadius
                color: diarCancelButton.hovered ? Theme.surfaceHover : Theme.surface
                border.width: 1
                border.color: Theme.border
            }
            HoverHandler { cursorShape: Qt.PointingHandCursor }
            HoverBorder { anchors.fill: parent; active: diarCancelButton.hovered }
        }
    }

    Label {
        visible: benchmarkPage.diarizationStatus !== ""
        text: benchmarkPage.diarizationStatus
        color: "#cccccc"
        font.pixelSize: 12
    }

    // Diarization result cards — speaker count is the money metric.
    Repeater {
        model: benchmarkPage.diarizationResults
        Rectangle {
            required property var modelData
            Layout.fillWidth: true
            Layout.preferredHeight: diarCardCol.implicitHeight + 16
            radius: AppSettings.borderRadius
            color: Theme.surface
            border.width: 1
            border.color: Theme.border

            ColumnLayout {
                id: diarCardCol
                anchors.top: parent.top
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.margins: 8
                spacing: 4

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8
                    Label {
                        Layout.fillWidth: true
                        text: modelData.label
                        color: "white"
                        font.bold: true
                        font.pixelSize: 13
                        // Item: don't truncate the configuration description —
                        // the seg+embed+sensitivity combo is the whole point of
                        // the row, so wrap it instead of eliding.
                        wrapMode: Text.WordWrap
                    }
                    Label {
                        visible: modelData.ok === true
                        text: modelData.speakerCount === 1
                              ? qsTr("1 speaker")
                              : qsTr("%1 speakers").arg(modelData.speakerCount || 0)
                        color: (modelData.speakerCount || 0) > 1 ? "#8fce9e" : "#c98500"
                        font.bold: true
                        font.pixelSize: 13
                    }
                }
                Label {
                    visible: modelData.ok === true
                    text: qsTr("%1 turns · %2s · %3× realtime")
                          .arg(modelData.turnCount || 0)
                          .arg(((modelData.runtimeMs || 0) / 1000).toFixed(1))
                          .arg((modelData.realtimeFactor || 0).toFixed(1))
                    color: "#999999"
                    font.pixelSize: 11
                }
                Label {
                    visible: modelData.ok !== true
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    text: qsTr("Skipped: %1").arg(modelData.error || qsTr("unavailable"))
                    color: "#c98500"
                    font.pixelSize: 12
                }
            }
        }
    }
}
