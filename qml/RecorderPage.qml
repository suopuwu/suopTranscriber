import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import SuopTranscriber

// Recorder top-level screen — extracted verbatim from MainPanel's inline
// recorderPage Item (2026-07-19 decomposition). Self-contained: owns its
// mic/system-audio/level state, waveform Canvas, and folder picker. Host wires
// the recorder + vault info, toggles `visible`, and handles openVaultRequested.
Item {
    id: recorderPage

    property AudioRecorder audioRecorder
    property string vaultLocalPath: ""
    property url vaultPath
    property VaultTreeModel vaultTreeModel
    signal openVaultRequested()

    // Recorder page's live duration counter — audioRecorder.elapsedMs is
    // wall-clock ms since capture actually began (see AudioRecorder.h). Its own
    // copy of MainPanel.formatDuration (a tiny pure formatter) — cheaper than a
    // shared-util dependency for one call site.
    function formatDuration(ms) {
        const totalSeconds = Math.floor(ms / 1000)
        const minutes = Math.floor(totalSeconds / 60)
        const seconds = totalSeconds % 60
        return (minutes < 10 ? "0" : "") + minutes + ":" + (seconds < 10 ? "0" : "") + seconds
    }

    property bool useMic: true
    property bool useSystemAudio: false
    property string selectedMicId: ""
    property var micDevices: []
    // Selected AudioRecorder::availableSystemSources() id — "" means the
    // synthetic default entry (today's default-sink-monitor behavior).
    property string selectedSystemSourceId: ""
    property var systemSources: []
    // Backing string for the shared warning banner below — cleared
    // whenever the folder picker succeeds/reopens, or whenever
    // audioRecorder.lastError clears (see the Connections block below).
    property string recordingFolderError: ""
    // Recent DISPLAY levels (RMS mapped through displayLevelFromRms, see
    // the Connections block below) for the scrolling waveform Canvas, oldest
    // first, capped at ~100 (see waveformCanvas below). AppSettings.
    // animationsEnabled off collapses this to a single-element "static
    // level" array instead of a scrolling history (CLAUDE.md/this
    // app's convention: that flag kills continuous-update CPU/GPU work).
    property var levelSamples: []

    // Refreshes both device lists off the same trigger (page becoming visible,
    // or first load) — renamed from refreshMicDevices() when system-source
    // enumeration (queue item 036) was added alongside it.
    function refreshDevices() {
        recorderPage.micDevices = recorderPage.audioRecorder ? recorderPage.audioRecorder.availableMicDevices() : []
        if (recorderPage.selectedMicId === "" && recorderPage.micDevices.length > 0)
            recorderPage.selectedMicId = recorderPage.micDevices[0].id

        recorderPage.systemSources = recorderPage.audioRecorder ? recorderPage.audioRecorder.availableSystemSources() : []
        if (recorderPage.selectedSystemSourceId === "" && recorderPage.systemSources.length > 0)
            recorderPage.selectedSystemSourceId = recorderPage.systemSources[0].id
    }

    onVisibleChanged: if (recorderPage.visible) recorderPage.refreshDevices()
    Component.onCompleted: recorderPage.refreshDevices()

    // audioRecorder.lastError clearing (a fresh startRecording() call
    // clears it server-side) should also clear the shared banner if
    // it was the error currently showing.
    Connections {
        target: recorderPage.audioRecorder
        function onLevelUpdated() {
            // Mapped to a display level HERE, once per tick, not per-point
            // in onPaint — see AudioRecorderIO::displayLevelFromRms() for
            // why raw RMS is unusable as a pixel coordinate.
            const level = recorderPage.audioRecorder.displayLevel(recorderPage.audioRecorder.currentLevel)
            if (AppSettings.animationsEnabled) {
                const samples = recorderPage.levelSamples.slice()
                samples.push(level)
                if (samples.length > 100)
                    samples.shift()
                recorderPage.levelSamples = samples
            } else {
                recorderPage.levelSamples = [level]
            }
            waveformCanvas.requestPaint()
        }
    }

    // No vault open: don't let the user configure mic/folder pickers
    // with nothing to record into. Same "Open Vault" button precedent
    // as the empty-vault block above.
    Item {
        anchors.fill: parent
        visible: recorderPage.vaultPath.toString() === ""

        Button {
            id: recorderOpenVaultButton
            anchors.centerIn: parent
            text: qsTr("Open Vault")
            hoverEnabled: true
            onClicked: recorderPage.openVaultRequested()

            background: Rectangle {
                radius: AppSettings.borderRadius
                color: (recorderOpenVaultButton.hovered && recorderOpenVaultButton.enabled) ? Theme.surfaceHover : Theme.surface
                border.width: 1
                border.color: Theme.border
            }
            HoverHandler {
                cursorShape: Qt.PointingHandCursor
            }
            HoverBorder {
                anchors.fill: parent
                active: recorderOpenVaultButton.hovered
            }
        }
    }

    ScrollPage {
        id: recorderScroll
        anchors.fill: parent
        visible: recorderPage.vaultPath.toString() !== ""

        ColumnLayout {
            id: recorderContent
            width: recorderScroll.availableContentWidth
            spacing: 12

            // Start/Stop + disable-animations + duration counter row.
            RowLayout {
                Layout.fillWidth: true
                // Nested layouts default Layout.fillHeight to true, which
                // would let these rows soak up the page's leftover height and
                // push the waveform away from the controls row above it.
                Layout.fillHeight: false
                spacing: 12

                Item {
                    id: recordToggleButton
                    Layout.preferredWidth: 32
                    Layout.preferredHeight: 32
                    enabled: recorderPage.useMic || recorderPage.useSystemAudio || recorderPage.audioRecorder.recording

                    HoverHandler {
                        id: recordToggleHover
                        cursorShape: Qt.PointingHandCursor
                        enabled: recordToggleButton.enabled
                    }
                    TapHandler {
                        enabled: recordToggleButton.enabled
                        onTapped: {
                            if (recorderPage.audioRecorder.recording) {
                                recorderPage.audioRecorder.stopRecording()
                            } else {
                                recorderPage.recordingFolderError = ""
                                recorderPage.audioRecorder.startRecording(
                                    recorderPage.vaultPath, AppSettings.recordingOutputFolder,
                                    recorderPage.useMic, recorderPage.selectedMicId,
                                    recorderPage.useSystemAudio, AppSettings.recordingMicGain,
                                    AppSettings.recordingSystemGain, recorderPage.selectedSystemSourceId)
                            }
                        }
                    }
                    Rectangle {
                        anchors.fill: parent
                        radius: AppSettings.borderRadius
                        color: (recordToggleHover.hovered && recordToggleButton.enabled) ? Theme.surfaceHover : Theme.surface
                    }
                    HoverBorder {
                        anchors.fill: parent
                        active: recordToggleButton.enabled && recordToggleHover.hovered
                    }
                    ToolTip.visible: recordToggleHover.hovered
                    ToolTip.text: recorderPage.audioRecorder.recording ? qsTr("Stop recording") : qsTr("Start recording")
                    ToolTip.delay: 500
                    AppIcon {
                        anchors.centerIn: parent
                        name: recorderPage.audioRecorder.recording ? "stop" : "record"
                        iconSize: 16
                        color: recorderPage.audioRecorder.recording ? "#e07a7a" : "#6fdf9f"
                    }
                }

                // Global animations/blur kill switch — placed here per the
                // user's explicit placement decision, but affects the whole
                // app (every existing Behavior/compositor blur already gates
                // on AppSettings.animationsEnabled), not just this page.
                Item {
                    id: animationsToggleButton
                    Layout.preferredWidth: 32
                    Layout.preferredHeight: 32

                    HoverHandler {
                        id: animationsToggleHover
                        cursorShape: Qt.PointingHandCursor
                    }
                    TapHandler {
                        onTapped: AppSettings.animationsEnabled = !AppSettings.animationsEnabled
                    }
                    Rectangle {
                        anchors.fill: parent
                        radius: AppSettings.borderRadius
                        color: animationsToggleHover.hovered ? Theme.surfaceHover : Theme.surface
                    }
                    HoverBorder {
                        anchors.fill: parent
                        active: animationsToggleHover.hovered
                    }
                    ToolTip.visible: animationsToggleHover.hovered
                    ToolTip.text: AppSettings.animationsEnabled ? qsTr("Disable animations") : qsTr("Enable animations")
                    ToolTip.delay: 500
                    AppIcon {
                        anchors.centerIn: parent
                        name: "freeze"
                        iconSize: 15
                        color: AppSettings.animationsEnabled ? Theme.textPrimary : "#6fdf9f"
                    }
                }

                Label {
                    id: recordingDurationLabel
                    text: recorderPage.formatDuration(recorderPage.audioRecorder ? recorderPage.audioRecorder.elapsedMs : 0)
                    color: "white"
                    font.pixelSize: 16
                }

                Item { Layout.fillWidth: true }

                // Where the next recording lands. "" means the vault root —
                // the same join rule AudioRecorderIO::buildOutputFilePath()
                // applies, spelled out here so the destination is never
                // implicit (the picker button alone showed nothing). Elides
                // from the left so a deep subfolder still shows its leaf.
                Label {
                    id: recordingDestinationLabel
                    Layout.maximumWidth: 260
                    text: AppSettings.recordingOutputFolder === ""
                          ? qsTr("Vault root")
                          : AppSettings.recordingOutputFolder
                    color: Theme.textMuted
                    font.pixelSize: 11
                    elide: Text.ElideLeft

                    HoverHandler {
                        id: recordingDestinationHover
                    }
                    ToolTip.visible: recordingDestinationHover.hovered
                    ToolTip.text: qsTr("Recordings are saved to %1")
                                  .arg(recorderPage.vaultLocalPath
                                       + (AppSettings.recordingOutputFolder === ""
                                          ? "" : "/" + AppSettings.recordingOutputFolder))
                    ToolTip.delay: 500
                }

                // Output folder picker.
                Item {
                    id: changeFolderButton
                    Layout.preferredWidth: 32
                    Layout.preferredHeight: 32

                    HoverHandler {
                        id: changeFolderHover
                        cursorShape: Qt.PointingHandCursor
                    }
                    TapHandler {
                        onTapped: {
                            const sub = AppSettings.recordingOutputFolder
                            recordingFolderDialog.currentFolder =
                                "file://" + recorderPage.vaultLocalPath + (sub ? "/" + sub : "")
                            recordingFolderDialog.open()
                        }
                    }
                    Rectangle {
                        anchors.fill: parent
                        radius: AppSettings.borderRadius
                        color: changeFolderHover.hovered ? Theme.surfaceHover : Theme.surface
                    }
                    HoverBorder {
                        anchors.fill: parent
                        active: changeFolderHover.hovered
                    }
                    ToolTip.visible: changeFolderHover.hovered
                    ToolTip.text: qsTr("Change output folder")
                    ToolTip.delay: 500
                    AppIcon {
                        anchors.centerIn: parent
                        name: "folder"
                        iconSize: 15
                        color: Theme.textPrimary
                    }
                }
            }

            // Scrolling waveform/level indicator — mirrors playPauseIcon's
            // Canvas idiom above (getContext("2d")/onPaint), fed from
            // recorderPage.levelSamples (pushed on each throttled
            // levelUpdated tick, see the Connections block above).
            Canvas {
                id: waveformCanvas
                Layout.fillWidth: true
                Layout.preferredHeight: 80

                onWidthChanged: requestPaint()
                onHeightChanged: requestPaint()

                onPaint: {
                    const ctx = getContext("2d")
                    ctx.reset()
                    ctx.fillStyle = "#1a1a1a"
                    ctx.fillRect(0, 0, width, height)

                    const samples = recorderPage.levelSamples
                    if (samples.length === 0)
                        return

                    // Mirrored about the vertical centre — a soundwave, not a
                    // baseline-anchored graph. Doubles the visible excursion
                    // for the same level and reads as audio rather than as a
                    // line chart.
                    const mid = height / 2

                    if (!AppSettings.animationsEnabled) {
                        // Static bar at the last-known level — no scrolling
                        // history while animations are off.
                        const half = samples[samples.length - 1] * mid
                        ctx.fillStyle = "#6fdf9f"
                        ctx.fillRect(0, mid - half, width, half * 2)
                        return
                    }

                    const maxSamples = 100
                    const stepX = width / Math.max(1, maxSamples - 1)
                    ctx.beginPath()
                    for (let i = 0; i < samples.length; i++) {
                        const x = width - (samples.length - i) * stepX
                        const y = mid - samples[i] * mid
                        if (i === 0)
                            ctx.moveTo(x, y)
                        else
                            ctx.lineTo(x, y)
                    }
                    // ...and back along the mirrored bottom edge to close the
                    // filled body.
                    for (let i = samples.length - 1; i >= 0; i--) {
                        const x = width - (samples.length - i) * stepX
                        ctx.lineTo(x, mid + samples[i] * mid)
                    }
                    ctx.closePath()
                    ctx.fillStyle = "#6fdf9f"
                    ctx.fill()
                }
            }

            // Shared warning banner (StyleGuide.md "Warning banners"): shows
            // whichever of the folder-picker rejection / audioRecorder's own
            // lastError is currently non-empty. No dismiss button — clears
            // itself the next time the folder picker succeeds/reopens, or
            // the next time lastErrorChanged reports an empty string.
            Label {
                Layout.fillWidth: true
                visible: text !== ""
                text: recorderPage.recordingFolderError !== "" ? recorderPage.recordingFolderError
                      : (recorderPage.audioRecorder ? recorderPage.audioRecorder.lastError : "")
                color: Theme.danger
                font.pixelSize: 11
                wrapMode: Text.WordWrap
            }

            // Microphone row: toggle + device combo + gain slider.
            RowLayout {
                Layout.fillWidth: true
                // Nested layouts default Layout.fillHeight to true, which
                // would let these rows soak up the page's leftover height and
                // push the waveform away from the controls row above it.
                Layout.fillHeight: false
                spacing: 12

                Item {
                    id: micToggle
                    Layout.preferredWidth: micToggleLabel.implicitWidth + 16
                    Layout.preferredHeight: 24

                    HoverHandler {
                        id: micToggleHover
                        cursorShape: Qt.PointingHandCursor
                    }
                    TapHandler {
                        onTapped: recorderPage.useMic = !recorderPage.useMic
                    }
                    Rectangle {
                        anchors.fill: parent
                        radius: AppSettings.borderRadius
                        color: recorderPage.useMic
                               ? (micToggleHover.hovered ? Theme.success : Theme.successDim)
                               : (micToggleHover.hovered ? Theme.surfaceHover : Theme.surface)
                        border.width: 1
                        border.color: recorderPage.useMic ? Theme.success : Theme.borderStrong

                        Behavior on color { ColorAnimation { duration: AppSettings.animationsEnabled ? 100 : 0 } }
                    }
                    Label {
                        id: micToggleLabel
                        anchors.centerIn: parent
                        text: qsTr("Microphone")
                        color: recorderPage.useMic ? "white" : Theme.textMuted
                        font.pixelSize: 11
                    }
                }

                ComboBox {
                    id: micCombo
                    Layout.preferredWidth: 220
                    enabled: recorderPage.useMic
                    hoverEnabled: true
                    textRole: "name"
                    valueRole: "id"
                    model: recorderPage.micDevices
                    onActivated: recorderPage.selectedMicId = micCombo.currentValue

                    HoverHandler {
                        cursorShape: Qt.PointingHandCursor
                        enabled: micCombo.enabled
                    }
                    HoverBorder {
                        anchors.fill: parent
                        active: micCombo.enabled && micCombo.hovered
                    }
                }

                Slider {
                    id: micGainSlider
                    Layout.fillWidth: true
                    enabled: recorderPage.useMic
                    hoverEnabled: true
                    from: 0.0
                    to: 2.0
                    value: AppSettings.recordingMicGain
                    onMoved: AppSettings.recordingMicGain = value

                    HoverHandler {
                        cursorShape: Qt.PointingHandCursor
                        enabled: micGainSlider.enabled
                    }
                    HoverBorder {
                        anchors.fill: parent
                        active: micGainSlider.enabled && micGainSlider.hovered
                    }
                }
            }

            // System audio row: toggle (parec's default-sink monitor, "all
            // windows" mix — see AudioRecorder.h) + its own gain slider.
            RowLayout {
                Layout.fillWidth: true
                // Nested layouts default Layout.fillHeight to true, which
                // would let these rows soak up the page's leftover height and
                // push the waveform away from the controls row above it.
                Layout.fillHeight: false
                spacing: 12

                Item {
                    id: systemAudioToggle
                    Layout.preferredWidth: systemAudioToggleLabel.implicitWidth + 16
                    Layout.preferredHeight: 24

                    HoverHandler {
                        id: systemAudioToggleHover
                        cursorShape: Qt.PointingHandCursor
                    }
                    TapHandler {
                        onTapped: recorderPage.useSystemAudio = !recorderPage.useSystemAudio
                    }
                    Rectangle {
                        anchors.fill: parent
                        radius: AppSettings.borderRadius
                        color: recorderPage.useSystemAudio
                               ? (systemAudioToggleHover.hovered ? Theme.success : Theme.successDim)
                               : (systemAudioToggleHover.hovered ? Theme.surfaceHover : Theme.surface)
                        border.width: 1
                        border.color: recorderPage.useSystemAudio ? Theme.success : Theme.borderStrong

                        Behavior on color { ColorAnimation { duration: AppSettings.animationsEnabled ? 100 : 0 } }
                    }
                    Label {
                        id: systemAudioToggleLabel
                        anchors.centerIn: parent
                        text: qsTr("System Audio")
                        color: recorderPage.useSystemAudio ? "white" : Theme.textMuted
                        font.pixelSize: 11
                    }
                }

                ComboBox {
                    id: systemSourceCombo
                    Layout.preferredWidth: 220
                    enabled: recorderPage.useSystemAudio
                    hoverEnabled: true
                    textRole: "name"
                    valueRole: "id"
                    model: recorderPage.systemSources
                    onActivated: recorderPage.selectedSystemSourceId = systemSourceCombo.currentValue

                    HoverHandler {
                        cursorShape: Qt.PointingHandCursor
                        enabled: systemSourceCombo.enabled
                    }
                    HoverBorder {
                        anchors.fill: parent
                        active: systemSourceCombo.enabled && systemSourceCombo.hovered
                    }
                }

                Slider {
                    id: systemGainSlider
                    Layout.fillWidth: true
                    enabled: recorderPage.useSystemAudio
                    hoverEnabled: true
                    from: 0.0
                    to: 2.0
                    value: AppSettings.recordingSystemGain
                    onMoved: AppSettings.recordingSystemGain = value

                    HoverHandler {
                        cursorShape: Qt.PointingHandCursor
                        enabled: systemGainSlider.enabled
                    }
                    HoverBorder {
                        anchors.fill: parent
                        active: systemGainSlider.enabled && systemGainSlider.hovered
                    }
                }
            }
        }
    }

    FolderDialog {
        id: recordingFolderDialog
        onAccepted: {
            const localPath = recordingFolderDialog.selectedFolder.toString().replace("file://", "")
            if (!recorderPage.vaultTreeModel || !recorderPage.vaultTreeModel.isPathInsideVault(localPath)) {
                recorderPage.recordingFolderError = qsTr("That folder is outside the vault — pick a location inside it.")
                return
            }
            recorderPage.recordingFolderError = ""
            const prefix = recorderPage.vaultLocalPath + "/"
            AppSettings.recordingOutputFolder = localPath === recorderPage.vaultLocalPath ? ""
                : (localPath.startsWith(prefix) ? localPath.slice(prefix.length) : localPath)
        }
    }
}
