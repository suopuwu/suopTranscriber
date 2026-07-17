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

    function onShown() {
        // The vault is loaded by now — gather the file list, detect engines
        // lazily, restore the last run, and default to the open file.
        benchmarkPage.refreshAudioFiles()
        if (!benchmarkPage.enginesDetected)
            Qt.callLater(benchmarkPage.detectEngines)
        if (benchmarkPage.results.length === 0)
            benchmarkPage.loadLastRun()
        if (benchmarkPage.currentFilePath !== "")
            benchmarkPage.selectedFilePath = benchmarkPage.currentFilePath
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

    // The vault's audio files, for the file picker. Refreshed IMPERATIVELY (see
    // refreshAudioFiles()) rather than bound to vaultTreeModel.allAudioPaths()
    // directly: that's a Q_INVOKABLE with no NOTIFY signal, so a declarative
    // binding evaluates exactly once — at this page's creation, which happens
    // when SettingsPopup is built (app startup, before any vault is loaded) —
    // against an empty file list, and then never again. The combo would be
    // permanently empty. Same untracked-invokable trap SettingsPopup.qml's
    // combineOverlapWarnings documents and dodges the same way.
    property var audioFiles: []
    function refreshAudioFiles() {
        benchmarkPage.audioFiles = benchmarkPage.vaultTreeModel
                ? benchmarkPage.vaultTreeModel.allAudioPaths() : []
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

    // The model ids to benchmark: every downloaded whisper tier plus every
    // available engine. This is where the "only downloaded models" policy lives
    // (the controller just runs what it's handed).
    function modelIdsToRun() {
        const ids = []
        const models = benchmarkPage.transcriptionController
                       ? benchmarkPage.transcriptionController.availableModels() : []
        for (let i = 0; i < models.length; i++) {
            if (models[i].downloaded)
                ids.push(models[i].id)
        }
        for (let j = 0; j < benchmarkPage.engineList.length; j++) {
            if (benchmarkPage.engineList[j].available)
                ids.push(benchmarkPage.engineList[j].modelId)
        }
        return ids
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
            if (run.file)
                benchmarkPage.selectedFilePath = run.file
        }
    }

    function startBenchmark() {
        if (!benchmarkPage.transcriptionController || benchmarkPage.selectedFilePath === "")
            return
        if (!benchmarkPage.enginesDetected)
            benchmarkPage.detectEngines()
        const ids = benchmarkPage.modelIdsToRun()
        if (ids.length === 0) {
            benchmarkPage.statusText = qsTr("No downloaded models to benchmark — download a model first.")
            return
        }
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
        benchmarkPage.diarizationResults = []
        benchmarkPage.diarizationRunning = true
        benchmarkPage.diarizationStatus = qsTr("Starting %1 tests…").arg(combos.length)
        benchmarkPage.transcriptionController.runDiarizationBenchmark(
                    "file://" + benchmarkPage.selectedFilePath, combos)
    }

    Component.onCompleted: {
        benchmarkPage.refreshAudioFiles()
        benchmarkPage.loadLastRun()
    }

    Connections {
        target: benchmarkPage.transcriptionController
        enabled: benchmarkPage.transcriptionController !== null

        function onBenchmarkModelStarted(modelId, index, total) {
            benchmarkPage.statusText = qsTr("Running %1 of %2…").arg(index + 1).arg(total)
        }
        function onBenchmarkModelDone(result) {
            benchmarkPage.results = benchmarkPage.results.concat([result])
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
        contentItem: Label {
            wrapMode: Text.WordWrap
            text: qsTr("This runs %1 diarization tests over the file — each loads models and runs a full "
                       + "pass, so it can take a while. Continue?").arg(diarizationConfirmDialog.pendingCombos.length)
            color: "white"
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
        text: qsTr("Run every downloaded model on one file and compare. Nothing is saved to the vault — "
                   + "this is for comparison only.")
        color: "#aaaaaa"
        font.pixelSize: 12
    }

    // --- File picker + run controls ------------------------------------------
    RowLayout {
        Layout.fillWidth: true
        spacing: 8

        ComboBox {
            id: fileCombo
            Layout.fillWidth: true
            enabled: !benchmarkPage.running
            model: benchmarkPage.audioFiles
            textRole: ""
            displayText: benchmarkPage.selectedFilePath === ""
                         ? qsTr("Select an audio file…")
                         : benchmarkPage.selectedFilePath.substring(
                               benchmarkPage.selectedFilePath.lastIndexOf("/") + 1)
            onActivated: benchmarkPage.selectedFilePath = model[currentIndex]

            delegate: ItemDelegate {
                required property int index
                required property string modelData
                width: fileCombo.width
                text: modelData.substring(modelData.lastIndexOf("/") + 1)
                highlighted: fileCombo.highlightedIndex === index
            }
            HoverHandler { cursorShape: Qt.PointingHandCursor }
        }

        Button {
            id: runButton
            text: benchmarkPage.running ? qsTr("Running…") : qsTr("Run benchmark")
            enabled: !benchmarkPage.running && benchmarkPage.selectedFilePath !== ""
            opacity: enabled ? 1 : 0.4
            onClicked: benchmarkPage.startBenchmark()
            background: Rectangle {
                radius: AppSettings.borderRadius
                color: (runButton.hovered && runButton.enabled) ? "#3a3a3a" : "#2a2a2a"
                border.width: 1
                border.color: "#3a3a3a"
            }
            HoverHandler { cursorShape: Qt.PointingHandCursor; enabled: runButton.enabled }
            HoverBorder { anchors.fill: parent; active: runButton.enabled && runButton.hovered }
        }

        Button {
            id: cancelButton
            visible: benchmarkPage.running
            text: qsTr("Cancel")
            onClicked: if (benchmarkPage.transcriptionController) benchmarkPage.transcriptionController.cancelBenchmark()
            background: Rectangle {
                radius: AppSettings.borderRadius
                color: cancelButton.hovered ? "#3a3a3a" : "#2a2a2a"
                border.width: 1
                border.color: "#3a3a3a"
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
                    color: modelData.available ? "#2ecc71" : "#777777"
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
            color: "#dddddd"; font.pixelSize: 12
        }
        Label {
            text: qsTr("Models: %1").arg(benchmarkPage.results.length)
            color: "#dddddd"; font.pixelSize: 12
        }
        Label {
            visible: benchmarkPage.lastRun && benchmarkPage.lastRun.totalRuntimeMs !== undefined
            text: qsTr("Total: %1s").arg(((benchmarkPage.lastRun ? benchmarkPage.lastRun.totalRuntimeMs : 0)
                                          / 1000).toFixed(1))
            color: "#dddddd"; font.pixelSize: 12
        }
    }

    // --- Per-model result cards ----------------------------------------------
    Repeater {
        model: benchmarkPage.results
        ColumnLayout {
            id: card
            required property var modelData
            Layout.fillWidth: true
            spacing: 4

            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: cardContent.implicitHeight + 16
                radius: AppSettings.borderRadius
                color: "#2a2a2a"
                border.width: 1
                border.color: "#3a3a3a"

                ColumnLayout {
                    id: cardContent
                    // Anchor top/left/right only (NOT fill/bottom): the card
                    // Rectangle's height is derived from this column's
                    // implicitHeight, so filling it would be a binding loop.
                    anchors.top: parent.top
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.margins: 8
                    spacing: 4

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8
                        Label {
                            text: card.modelData.label
                            color: "white"
                            font.bold: true
                            font.pixelSize: 13
                        }
                        Label {
                            text: card.modelData.kind === "engine" ? qsTr("engine") : qsTr("whisper")
                            color: "#888888"
                            font.pixelSize: 10
                        }
                        Item { Layout.fillWidth: true }
                        Label {
                            visible: card.modelData.ok === true
                            text: qsTr("%1s  ·  %2× realtime")
                                  .arg(((card.modelData.runtimeMs || 0) / 1000).toFixed(1))
                                  .arg((card.modelData.realtimeFactor || 0).toFixed(1))
                            color: "#8fce9e"
                            font.pixelSize: 12
                        }
                    }

                    Label {
                        visible: card.modelData.ok === true
                        text: qsTr("%1 segments · %2 words · %3 chars")
                              .arg(card.modelData.segmentCount || 0)
                              .arg(card.modelData.wordCount || 0)
                              .arg(card.modelData.charCount || 0)
                        color: "#999999"
                        font.pixelSize: 11
                    }

                    // Failed/unavailable model: show why, no transcript.
                    Label {
                        visible: card.modelData.ok !== true
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        text: qsTr("Skipped: %1").arg(card.modelData.error || qsTr("unavailable"))
                        color: "#c98500"
                        font.pixelSize: 12
                    }

                    ScrollView {
                        visible: card.modelData.ok === true && (card.modelData.text || "") !== ""
                        Layout.fillWidth: true
                        Layout.preferredHeight: Math.min(160, transcriptText.implicitHeight + 8)
                        clip: true
                        TextArea {
                            id: transcriptText
                            readOnly: true
                            wrapMode: TextArea.WordWrap
                            selectByMouse: true
                            text: card.modelData.text || ""
                            color: "#dddddd"
                            font.pixelSize: 12
                            background: null
                            // StyleGuide "editable/selectable text" convention:
                            // a selectByMouse control shows an I-beam.
                            HoverHandler { cursorShape: Qt.IBeamCursor }
                        }
                    }
                }
            }
        }
    }

    // ===== Diarization sweep =================================================
    Rectangle {
        Layout.fillWidth: true
        Layout.topMargin: 12
        Layout.preferredHeight: 1
        color: "#3a3a3a"
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
                   + "compare how many speakers each finds. Only downloaded models run (manage them under "
                   + "Models). Nothing is saved to the vault.")
        color: "#aaaaaa"
        font.pixelSize: 12
    }

    // Segmentation model checkboxes.
    Label { text: qsTr("Segmentation models"); color: "#dddddd"; font.pixelSize: 12 }
    Flow {
        Layout.fillWidth: true
        spacing: 12
        Repeater {
            model: benchmarkPage.segModels
            CheckBox {
                required property var modelData
                text: modelData.label + (modelData.downloaded ? "" : qsTr(" (not downloaded)"))
                enabled: modelData.downloaded
                checked: benchmarkPage.selectedSegIds.indexOf(modelData.id) >= 0
                contentItem: Label {
                    text: parent.text
                    color: parent.enabled ? "white" : "#777777"
                    leftPadding: parent.indicator.width + 6
                    verticalAlignment: Text.AlignVCenter
                }
                onToggled: benchmarkPage.selectedSegIds = benchmarkPage.toggleIn(benchmarkPage.selectedSegIds,
                                                                                  modelData.id)
                HoverHandler { cursorShape: Qt.PointingHandCursor; enabled: parent.enabled }
            }
        }
    }

    // Embedding model checkboxes.
    Label { text: qsTr("Embedding models"); color: "#dddddd"; font.pixelSize: 12 }
    Flow {
        Layout.fillWidth: true
        spacing: 12
        Repeater {
            model: benchmarkPage.embedModels
            CheckBox {
                required property var modelData
                text: modelData.label + (modelData.downloaded ? "" : qsTr(" (not downloaded)"))
                enabled: modelData.downloaded
                checked: benchmarkPage.selectedEmbedIds.indexOf(modelData.id) >= 0
                contentItem: Label {
                    text: parent.text
                    color: parent.enabled ? "white" : "#777777"
                    leftPadding: parent.indicator.width + 6
                    verticalAlignment: Text.AlignVCenter
                }
                onToggled: benchmarkPage.selectedEmbedIds = benchmarkPage.toggleIn(benchmarkPage.selectedEmbedIds,
                                                                                    modelData.id)
                HoverHandler { cursorShape: Qt.PointingHandCursor; enabled: parent.enabled }
            }
        }
    }

    // Sensitivity value chips.
    Label { text: qsTr("Sensitivity values (lower = more speakers)"); color: "#dddddd"; font.pixelSize: 12 }
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
            color: benchmarkPage.diarizationComboCount() > 10 ? "#c9a227" : "#dddddd"
            font.pixelSize: 12
        }
        Button {
            id: diarRunButton
            text: benchmarkPage.diarizationRunning ? qsTr("Running…") : qsTr("Run diarization sweep")
            enabled: !benchmarkPage.diarizationRunning && benchmarkPage.selectedFilePath !== ""
                     && benchmarkPage.diarizationComboCount() > 0
            opacity: enabled ? 1 : 0.4
            onClicked: benchmarkPage.startDiarizationBenchmark()
            background: Rectangle {
                radius: AppSettings.borderRadius
                color: (diarRunButton.hovered && diarRunButton.enabled) ? "#3a3a3a" : "#2a2a2a"
                border.width: 1
                border.color: "#3a3a3a"
            }
            HoverHandler { cursorShape: Qt.PointingHandCursor; enabled: diarRunButton.enabled }
            HoverBorder { anchors.fill: parent; active: diarRunButton.enabled && diarRunButton.hovered }
        }
        Button {
            id: diarCancelButton
            visible: benchmarkPage.diarizationRunning
            text: qsTr("Cancel")
            onClicked: if (benchmarkPage.transcriptionController) benchmarkPage.transcriptionController.cancelBenchmark()
            background: Rectangle {
                radius: AppSettings.borderRadius
                color: diarCancelButton.hovered ? "#3a3a3a" : "#2a2a2a"
                border.width: 1
                border.color: "#3a3a3a"
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
            color: "#2a2a2a"
            border.width: 1
            border.color: "#3a3a3a"

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
                        elide: Text.ElideRight
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
