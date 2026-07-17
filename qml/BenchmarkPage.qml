import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SuopTranscriber

// Settings "Benchmark" page (mounted by SettingsPopup as page 4). Runs every
// already-downloaded model + available engine over ONE vault audio file and
// shows their transcripts side by side with simple heuristics (audio length,
// run time, realtime factor, segment/word counts). Writes NO transcripts — the
// only on-disk footprint is the last-run cache
// (VaultTreeModel::writeBenchmarkLastRun), reloaded when the page reopens. All
// transcription runs on the controller's worker thread (TranscriptionController::
// runBenchmark), so the UI stays responsive; results stream in per model.
ColumnLayout {
    id: benchmarkPage

    property var transcriptionController: null
    property var vaultTreeModel: null

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

    onVisibleChanged: {
        if (visible) {
            // The vault is loaded by the time a user opens this page — this is
            // where the file list is actually gathered (see audioFiles).
            benchmarkPage.refreshAudioFiles()
            if (!benchmarkPage.enginesDetected)
                Qt.callLater(benchmarkPage.detectEngines)
            if (benchmarkPage.results.length === 0)
                benchmarkPage.loadLastRun()
        }
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
            benchmarkPage.statusText = qsTr("Failed: %1").arg(error)
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
}
