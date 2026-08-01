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

    // Recorder page's duration counter — fed takeDurationMs, not the shared
    // capture clock (see that property). Its own copy of
    // MainPanel.formatDuration (a tiny pure formatter) — cheaper than a
    // shared-util dependency for one call site.
    function formatDuration(ms) {
        const totalSeconds = Math.floor(ms / 1000)
        const minutes = Math.floor(totalSeconds / 60)
        const seconds = totalSeconds % 60
        return (minutes < 10 ? "0" : "") + minutes + ":" + (seconds < 10 ? "0" : "") + seconds
    }

    property bool useMic: true
    property bool useSystemAudio: false
    // Top of every source's gain range, as a multiplier — one number so the
    // three sliders and the three percent fields can't disagree about where
    // "full" is. 4x because the useful correction for a quiet source (a laptop
    // mic, an app turned down in the mixer) is a boost, and 2x often wasn't
    // enough of one; the mix clamps at 16-bit either way (see mixPcm16N).
    readonly property real maxSourceGain: 4.0
    // Top of what can be TYPED into a percent readout, which is deliberately
    // higher than the slider's: the drag range is a comfort range (unity a
    // quarter along), while a source that needs more than 4x is a real case
    // the number field is the only way to reach. The slider pins at its own
    // top while the value is above it, and dragging it drops back into
    // 0-400% — the number is the authority, not the handle position.
    readonly property real maxTypedGain: 20.0
    // Test mode: the next take goes to a temp directory instead of the vault
    // (see AudioRecorderIO::buildTestOutputFilePath()), for checking levels
    // without leaving real audio files in the tree. Session-only — a mode you
    // are in right now, not a preference; persisting it would make a
    // forgotten toggle silently discard a real recording.
    property bool testMode: false
    // What the next take is called, before the ".wav". Empty = the timestamp
    // name, which stays the default so recording is still one click. Kept
    // ACROSS takes rather than cleared after each: a second take under the
    // same name is exactly the collision the "_N" suffix exists for, so a
    // series of takes named "interview" numbers itself. Session-only, like
    // testMode above — a name is what you are recording right now, and a
    // persisted one would silently attach itself to an unrelated take weeks
    // later.
    property string recordingName: ""
    // The file name the next take would ACTUALLY land on, straight from the
    // C++ that will decide it (AudioRecorder::plannedRecordingFileName) — never
    // re-derived here, or the warning and the written file could disagree
    // about what counts as taken.
    readonly property string plannedFileName: recorderPage.audioRecorder
        ? recorderPage.audioRecorder.plannedRecordingFileName(
              recorderPage.vaultPath, AppSettings.recordingOutputFolder,
              recorderPage.recordingName, recorderPage.testMode)
        : ""
    // Non-empty when the typed name won't be used verbatim — either it
    // sanitises away to nothing, or something at the destination already
    // holds it and the take will be numbered. Shown in the page's shared
    // warning banner (see below).
    readonly property string recordingNameWarning: {
        if (!recorderPage.audioRecorder || recorderPage.recordingName.trim() === "")
            return ""
        const clean = recorderPage.audioRecorder.sanitizeRecordingName(recorderPage.recordingName)
        if (clean === "")
            return qsTr("That name can't be used as a file name — this take will be saved with a "
                        + "timestamp name instead.")
        if (recorderPage.plannedFileName !== clean + ".wav")
            return qsTr("\"%1.wav\" already exists here — this take will be saved as \"%2\".")
                   .arg(clean).arg(recorderPage.plannedFileName)
        return ""
    }
    property string selectedMicId: ""
    property var micDevices: []
    // Selected AudioRecorder::availableSystemSources() id — "" means the
    // synthetic default entry (today's default-sink-monitor behavior).
    property string selectedSystemSourceId: ""
    // Bound to AudioRecorder's own enumeration rather than pulled: the probe
    // is a subprocess and runs asynchronously, and C++ suppresses a
    // sourcesChanged that carries an identical graph, so these only churn
    // when the set of sources really changed.
    readonly property var systemSources: recorderPage.audioRecorder ? recorderPage.audioRecorder.systemSources : []
    // Live application playback streams, straight from AudioRecorder's async
    // enumeration. NOT the row model — a stream exists only while its app is
    // actually making noise, so binding rows to this made a row (and its
    // gain) vanish the moment a call ended.
    readonly property var appStreams: recorderPage.audioRecorder ? recorderPage.audioRecorder.appStreams : []
    // Every application display name seen live this session, plus every name
    // the user has configured in an earlier one. A row survives its app going
    // quiet — or the app restarting — by being remembered here, which is only
    // sound because the name is stable, unlike the "stream:<object.serial>"
    // id this state used to be keyed on (a serial dies with the connection).
    //
    // Seeded from the persisted per-app maps so an app you configured in an
    // earlier session gets a row immediately — before it has played anything
    // this session, which is the only way a row could otherwise appear.
    // Union of both maps' keys: an app can have been armed without its gain
    // moved off 1.0, or vice versa.
    property var rememberedAppNames: {
        const names = Object.keys(AppSettings.recordingAppGains)
        for (const name of Object.keys(AppSettings.recordingAppCaptureEnabled)) {
            if (names.indexOf(name) < 0)
                names.push(name)
        }
        return names
    }
    // {name, ids, active} per row — see AudioRecorderIO::buildAppCaptureRows().
    // Reads appStreams and rememberedAppNames directly so both register as
    // binding dependencies.
    readonly property var appCaptureRows: recorderPage.audioRecorder
        ? recorderPage.audioRecorder.appCaptureRows(recorderPage.appStreams, recorderPage.rememberedAppNames)
        : []
    // Per-app capture state, keyed by APPLICATION NAME (the row key), not by
    // stream id, and persisted — see AppSettings.recordingAppGains /
    // recordingAppCaptureEnabled for why the key is stable enough for that
    // and what "persisted" does and doesn't mean here.
    //
    // Every source row's leading toggle is pinned to this ONE width so all
    // three kinds of gain slider (mic, system, per-app) are geometrically
    // identical and comparable by eye — a slider sized to whatever was left
    // over after a variable-length app name is not a slider you can compare
    // against the row above it. Measured rather than guessed: TextMetrics is
    // the only way to size a column to text that hasn't been laid out yet
    // (the per-app labels live inside a Repeater delegate). Reading
    // appCaptureRows here (not appStreams) is what makes a remembered-but-
    // silent app's longer name still get measured, not just a currently-live
    // one. Capped so one pathologically long window title can't squeeze the
    // sliders down to nothing; such a label elides instead.
    //
    // Assigned imperatively, NOT bound: measureSourceLabel() writes
    // sourceLabelMetrics.text and reads back its width, so a binding here would
    // both read and write its own dependency — a binding loop, which QML
    // resolves by giving up and never re-evaluating, freezing the column at a
    // stale width once a new app name appears.
    property real sourceToggleWidth: 0
    function recomputeSourceToggleWidth() {
        let widest = Math.max(recorderPage.measureSourceLabel(qsTr("Microphone")),
                              recorderPage.measureSourceLabel(qsTr("System Audio")))
        for (const row of recorderPage.appCaptureRows)
            widest = Math.max(widest, recorderPage.measureSourceLabel(row.name))
        recorderPage.sourceToggleWidth = Math.min(Math.ceil(widest) + 16, 260)
    }
    // Recomputed from the onAppCaptureRowsChanged handler below (the row set is
    // the only thing that changes what needs measuring) and at load.

    // Width of `text` in the source-row label font. The TextMetrics item is
    // reused as a scratch measurer (it is not displayed), which is why this
    // is a function and not three separate TextMetrics instances — the
    // per-app names are only known at runtime.
    function measureSourceLabel(text) {
        sourceLabelMetrics.text = text
        return sourceLabelMetrics.width
    }

    // font.pixelSize must stay in step with the source-row labels below
    // (micToggleLabel/systemAudioToggleLabel/appCaptureToggleLabel) — the
    // measurement is only meaningful in the font it measures for.
    TextMetrics {
        id: sourceLabelMetrics
        font.pixelSize: 11
    }

    // Backing string for the shared warning banner below — cleared
    // whenever the folder picker succeeds/reopens, or whenever
    // audioRecorder.lastError clears (see the Connections block below).
    property string recordingFolderError: ""
    // Non-empty when the picked system source disappeared and the selection
    // was reset for the user (see onSystemSourcesChanged). Shown in the same
    // banner as recordingFolderError; cleared whenever the user activates the
    // dropdown themselves.
    property string systemSourceWarning: ""
    // Recent DISPLAY levels (RMS mapped through displayLevelFromRms, see
    // the Connections block below) for the scrolling waveform Canvas, oldest
    // first, capped at ~100 (see waveformCanvas below). AppSettings.
    // animationsEnabled off collapses this to a single-element "static
    // level" array instead of a scrolling history (this app's
    // convention: that flag kills continuous-update CPU/GPU work).
    property var levelSamples: []

    // Absolute path of the most recent finished recording, for the in-page
    // playback below. Its own AudioPlayer, NOT MainPanel's: that one carries
    // the open transcript's source and position, and the transcript view's
    // sentence highlighting binds to it — pointing it at a recording would
    // scrub the user's place in the file they had open.
    property string lastRecordingPath: ""
    // Whole-recording level history for the review waveform, as
    // {ms, level} pairs (ms = audioRecorder.elapsedMs at that tick, level =
    // the same DISPLAY level levelSamples holds). Timestamped rather than
    // index-counted because the level tick is throttled to "at least
    // kLevelUpdateIntervalMs AND a mix actually happened", so ticks are not
    // evenly spaced and index*75ms would drift the playhead against the audio.
    property var recordedLevels: []

    // True when `ms` opens a NEW capture session rather than continuing
    // `history`'s. The capture clock (AudioRecorderWorker::m_elapsed) is
    // monotonic WITHIN a graph and restarts at 0 with every graph, so a tick
    // that fails to advance it is proof the graph changed — which is exactly
    // what a tick still in flight when a take supersedes monitoring looks
    // like. It has to be caught here: recordedLevels/monitorLevels are the
    // waveform's x-axis, and WaveformBar both binary-searches it and walks it
    // as a fill polygon, so one out-of-order entry self-intersects the
    // outline and leaves an unfilled wedge. Pure, and the unit under test —
    // same shape as WaveformBar.windowStartFor().
    function isNewCaptureEpoch(history, ms) {
        return history.length > 0 && ms <= history[history.length - 1].ms
    }

    // The same {ms, level} pairs for the MONITOR preview, kept strictly apart
    // from recordedLevels: monitoring resumes the moment a take ends, and a
    // monitor session restarts elapsedMs at 0, so appending its ticks to the
    // take's history would run that x-axis backwards (WaveformBar binary-
    // searches it). Oldest dropped rather than halved — this only ever feeds a
    // live 30s window, where old samples are off-screen, whereas the take
    // history's axis has to keep matching the file.
    property var monitorLevels: []

    // Rolling peak DISPLAY level per capture source, keyed "<kind>:<id>" (the
    // same key sourcePeakKey() builds from an audioRecorder.sourceLevels
    // entry). Advanced once per levelUpdated tick through
    // audioRecorder.decayPeak() — see its doc comment for why a raw
    // instantaneous level and a never-decaying maximum are both unusable for
    // setting gains. Cleared when a new capture starts, so peaks never carry
    // across takes.
    property var sourcePeaks: ({})
    // How long the rolling max stays pinned where it landed, in ms. This is the
    // number that describes what you SEE: for 3 seconds the line does not move
    // at all, then it releases. A decay rate alone could never deliver that —
    // see AudioRecorderIO::holdPeak() for why sizing a rate for "3 seconds"
    // described a release that never happens in practice.
    readonly property int sourcePeakHoldMs: 3000
    // Release rate once the hold expires, as a fraction of full scale per
    // levelUpdated tick (~75ms, see AudioRecorderWorker's kLevelUpdateIntervalMs).
    readonly property real sourcePeakDecay: 1.0 / 40.0
    // Per-source timestamp (on audioRecorder.elapsedMs's clock) until which the
    // peak is pinned. Keyed like sourcePeaks, and cleared alongside it — that
    // clock restarts with each capture, so a stale hold would pin a peak from
    // the previous run.
    property var sourcePeakHoldUntil: ({})

    // Length of the take now running, or of the last one that finished — NOT
    // the capture session's clock. audioRecorder.elapsedMs is shared with
    // monitoring, which runs from the moment the page opens and restarts at 0
    // after every take, so a readout bound straight to it counted up while the
    // recorder sat idle. Frozen at the finished take's length rather than
    // zeroed: after Stop, the number beside the waveform should say how long
    // the thing under review is.
    property int lastTakeMs: 0
    // Just lastTakeMs — not a live ternary reading audioRecorder.elapsedMs
    // while recording. elapsedMs is a cached field the worker only refreshes
    // on the NEXT tick (AudioRecorder::startRecording() flips `recording`
    // synchronously but posts the worker's restart across the thread
    // boundary), so right at the tap that starts a new take it can still hold
    // the PREVIOUS session's value for one tick's worth of time. onLevelUpdated
    // keeps lastTakeMs caught up every tick during a take, and the Record
    // button zeroes it before that same tap starts the worker — reading it
    // directly is what makes "a new take starts a new clock" true at the tap,
    // not one tick later.
    readonly property int takeDurationMs: recorderPage.lastTakeMs

    // elapsedMs at which the CURRENT unbroken run of silence began, or -1 if
    // the last tick had signal. A start MARK rather than a running total: a
    // take that is quiet for ten seconds and then loud must clear the hint,
    // which a cumulative counter would not do.
    property int silentCaptureStartMs: -1
    // Long enough not to fire on a natural gap; short enough to catch a
    // misrouted tap before the user has wasted a take. A hint, never an
    // error: recording silence on purpose is legitimate.
    readonly property int silentCaptureHintMs: 6000

    function sourcePeakKey(kind, id) { return kind + ":" + id }

    // Instantaneous per-source display level, keyed like sourcePeaks. Drives
    // the in-groove meter fill; the peak drives its line.
    property var sourceLevelsByKey: ({})

    function sourceLevelFor(kind, id) {
        const level = recorderPage.sourceLevelsByKey[recorderPage.sourcePeakKey(kind, id)]
        return level !== undefined ? level : 0.0
    }

    // A per-app ROW's live level, maxed over its live ids — the appRowPeak()
    // rule, applied to the instantaneous value.
    function appRowLevel(ids) {
        let level = 0.0
        for (const id of ids)
            level = Math.max(level, recorderPage.sourceLevelFor("app", id))
        return level
    }

    // A peak rendered in the unit it is actually measured in. dBFS: 0 is full
    // scale, negatives are below it, and the band thresholds (-27 / -6) are
    // finally legible against the number. Silence has no dB value, so anything
    // at or under the display floor reads as a dash rather than "-60", which
    // would look like a real (very quiet) measurement.
    function formatPeakDb(displayLevel) {
        if (!recorderPage.audioRecorder || displayLevel <= 0.0)
            return "\u2014"
        const db = recorderPage.audioRecorder.dbFromDisplayLevel(displayLevel)
        return Math.round(db) + " dB"
    }

    // The one tooltip every peak readout shares. The ideal range is READ back
    // from the shared band rule rather than typed, so the numbers it names can
    // never disagree with the colour the readout beside them is painted.
    readonly property string peakTooltip: {
        const base = qsTr("Rolling peak over the last 3s, in dBFS (0 = full scale) \u2014 match this across sources to equalize them.")
        const rec = recorderPage.audioRecorder
        if (!rec)
            return base
        const good = Math.round(rec.dbFromDisplayLevel(rec.levelBandGoodFloor()))
        const hot = Math.round(rec.dbFromDisplayLevel(rec.levelBandHotFloor()))
        return base + " " + qsTr("While speaking, aim for %1 to %2 dB \u2014 below that is too quiet to equalize, above it clips.").arg(good).arg(hot)
    }

    function sourcePeakFor(kind, id) {
        const peak = recorderPage.sourcePeaks[recorderPage.sourcePeakKey(kind, id)]
        return peak !== undefined ? peak : 0.0
    }

    // A per-app ROW's peak, maxed over every live id it currently resolves
    // to (zero, one, or several — see AudioRecorderIO::buildAppCaptureRows()).
    // An inactive row has no ids, so this reads 0 — correctly matching its
    // unfilled dot.
    function appRowPeak(ids) {
        let peak = 0.0
        for (const id of ids)
            peak = Math.max(peak, recorderPage.sourcePeakFor("app", id))
        return peak
    }

    // Refreshes the device lists off the same trigger (page becoming visible,
    // or first load). Mic enumeration is a cheap in-process QMediaDevices
    // call, so it happens right here; the PipeWire graph is a subprocess, so
    // it is only REQUESTED — systemSources/appStreams below are bound to
    // AudioRecorder's own properties and update when the dump lands.
    function refreshDevices() {
        recorderPage.micDevices = recorderPage.audioRecorder ? recorderPage.audioRecorder.availableMicDevices() : []
        if (recorderPage.selectedMicId === "" && recorderPage.micDevices.length > 0)
            recorderPage.selectedMicId = recorderPage.micDevices[0].id

        if (recorderPage.audioRecorder)
            recorderPage.audioRecorder.refreshSources()
    }

    // {id, gain} per enabled per-app capture row — what startRecording()'s
    // appCaptureSources parameter wants.
    function enabledAppCaptureSources() {
        // A monitor tap already carries every app's audio, so capturing an app
        // individually ON TOP of it records that app twice — and the two taps
        // are separate processes whose buffers land milliseconds apart, so the
        // copies comb-filter rather than merely doubling in level. The UI makes
        // the two mutually exclusive; this is the enforcement that a stale
        // selection can never route around.
        if (recorderPage.useSystemAudio)
            return []

        // A row is an application; startRecording() wants stream ids. An
        // enabled row contributes ONE entry per live stream it currently
        // resolves to — several for an app with two windows playing, and none
        // at all for a remembered app that is silent right now. That last
        // case is why the row can be configured ahead of time and still cost
        // nothing: there is simply nothing to target yet.
        const sources = []
        for (const row of recorderPage.appCaptureRows) {
            if (!AppSettings.recordingAppCaptureEnabled[row.name])
                continue
            const gain = AppSettings.recordingAppGains[row.name]
            for (const id of row.ids)
                sources.push({ id: id, gain: gain !== undefined ? gain : 1.0 })
        }
        return sources
    }

    // A "stream:<object.serial>" id is only valid while that connection lives
    // — Discord mints a new serial every time it reconnects its playback
    // stream (joining a call, changing output device, restarting). A stale id
    // is not an error anyone sees: pw-record starts fine against a dead
    // target, produces nothing, and the take comes out silent or gets deleted
    // for being empty. So the selection is re-checked against every fresh
    // enumeration, not just filled in when blank.
    onSystemSourcesChanged: {
        if (recorderPage.systemSources.length === 0)
            return
        const stillThere = recorderPage.systemSources.some(
            source => source.id === recorderPage.selectedSystemSourceId)
        if (stillThere)
            return
        const hadSelection = recorderPage.selectedSystemSourceId !== ""
        recorderPage.selectedSystemSourceId = recorderPage.systemSources[0].id
        recorderPage.systemSourceWarning = hadSelection
            ? qsTr("The audio source you picked is gone — falling back to %1.")
              .arg(recorderPage.systemSources[0].name)
            : ""
    }

    // Union in every name the enumeration has ever shown, so a row outlives
    // its stream. Assigned only on a real addition: an unconditional
    // reassignment would re-evaluate appCaptureRows (and rebuild every
    // Repeater delegate) on each 3s poll, yanking a slider out from under a
    // drag — the same trap the old refreshAppStreams() guarded against.
    //
    // rememberedAppNames starts as a BINDING (seeded from the persisted maps
    // above); the plain assignment below destroys that binding on the first
    // real addition. That is fine, not a bug to "fix" — the seed only needs
    // to hold until the first live enumeration, and .slice() above already
    // copies the seeded value into `names` before this overwrites it.
    onAppStreamsChanged: {
        const names = recorderPage.rememberedAppNames.slice()
        let added = false
        for (const stream of recorderPage.appStreams) {
            if (stream.name !== "" && names.indexOf(stream.name) < 0) {
                names.push(stream.name)
                added = true
            }
        }
        if (added)
            recorderPage.rememberedAppNames = names
    }

    onVisibleChanged: {
        if (recorderPage.visible)
            recorderPage.refreshDevices()
        // Leaving the page must release the devices monitoring holds open.
        recorderPage.updateMonitoring()
    }
    Component.onCompleted: {
        recorderPage.refreshDevices()
        recorderPage.recomputeSourceToggleWidth()
        // onVisibleChanged only fires on a CHANGE, so a page that loads already
        // visible would never start metering without this.
        recorderPage.updateMonitoring()
    }

    // Apps come and go while the page sits open — re-poll periodically
    // rather than only on visibility/load. Gated on `visible` so `pw-dump`
    // never runs while the page isn't shown. Runs DURING a take too: an
    // armed row whose app starts playing mid-recording is joined to the live
    // mix from the enumeration that spots it (see the onAppCaptureRowsChanged
    // handler below and AudioRecorder::addAppCaptureSource()). The probe is
    // an async subprocess that drops a tick while one is in flight, and
    // capture runs on its own thread, so this costs the recording nothing.
    Timer {
        id: appStreamPollTimer
        interval: 3000
        repeat: true
        running: recorderPage.visible && recorderPage.audioRecorder
        onTriggered: recorderPage.audioRecorder.refreshSources()
    }

    // An armed row's app has started playing: hand its new stream ids to the
    // running capture. Reads activeCaptureSourceIds so this re-evaluates when
    // the worker confirms an addition, not only when the graph changes —
    // otherwise a rejected add would never be retried.
    onAppCaptureRowsChanged: {
        recorderPage.joinLateSources()
        recorderPage.recomputeSourceToggleWidth()
    }

    function joinLateSources() {
        if (!recorderPage.capturing)
            return
        const pending = recorderPage.audioRecorder.pendingCaptureAdditions(
            recorderPage.enabledAppCaptureSources(),
            recorderPage.audioRecorder.activeCaptureSourceIds)
        for (const entry of pending)
            recorderPage.audioRecorder.addAppCaptureSource(entry.id, entry.gain)
    }

    // True whenever the capture graph is actually running — during a take OR
    // while merely metering. Every mid-capture source change is gated on this
    // rather than on `recording`, because monitoring has to honour the same
    // toggles or the meters would describe a source set the user has since
    // changed.
    readonly property bool capturing: recorderPage.audioRecorder
        ? (recorderPage.audioRecorder.recording || recorderPage.audioRecorder.monitoring)
        : false
    // A finished take owns the waveform frame even though the meters are live
    // again. Keyed on the captured history rather than lastRecordingPath:
    // that path only lands when the worker finishes finalizing (parec kill +
    // up to a 1s waitForFinished after the button flips), and the frame must
    // not flicker through a live monitor wave in between.
    readonly property bool reviewing: recorderPage.recordedLevels.length > 0
        && !(recorderPage.audioRecorder && recorderPage.audioRecorder.recording)
    // What the waveform is: a live scroll, or the take under review.
    readonly property bool waveformLive: recorderPage.capturing && !recorderPage.reviewing
    readonly property var waveformSamples:
        (recorderPage.reviewing || (recorderPage.audioRecorder && recorderPage.audioRecorder.recording))
        ? recorderPage.recordedLevels : recorderPage.monitorLevels

    // Makes the live capture match the armed set, whatever changed. Toggling a
    // source used to only repaint: `useMic`/`useSystemAudio` were read once, at
    // the moment Record was pressed, so flipping either mid-take did nothing
    // audible. Everything here is idempotent, so it is safe to call on any
    // change without working out which one it was.
    function syncCapture() {
        const rec = recorderPage.audioRecorder
        if (!rec)
            return
        if (recorderPage.capturing) {
            rec.setMicEnabled(recorderPage.useMic, recorderPage.selectedMicId,
                              AppSettings.recordingMicGain)
            rec.setSystemAudioEnabled(recorderPage.useSystemAudio, recorderPage.selectedSystemSourceId,
                                      AppSettings.recordingSystemGain)
            recorderPage.reconcileAppSources()
        }
        recorderPage.updateMonitoring()
    }

    // The reader beside setAppGain()'s writer: the row's slider and its percent
    // field must resolve the same number, and the field can't go through the
    // slider (a Slider clamps value to `to`, which would hide a typed gain
    // above maxSourceGain).
    function appGainFor(name) {
        const gain = AppSettings.recordingAppGains[name]
        return gain !== undefined ? gain : 1.0
    }

    // One writer for the per-app gain map, shared by that row's slider and its
    // percent field. Whole-map reassignment is what AppSettings notifies on —
    // mutating recordingAppGains[name] in place changes nothing anyone sees.
    function setAppGain(name, gain) {
        let gains = Object.assign({}, AppSettings.recordingAppGains)
        gains[name] = gain
        AppSettings.recordingAppGains = gains
    }

    // Drops whatever is capturing but no longer armed, then upserts everything
    // armed (addAppCaptureSource re-trims a source it already holds, so this
    // carries live gain changes too).
    function reconcileAppSources() {
        const rec = recorderPage.audioRecorder
        if (!rec || !recorderPage.capturing)
            return
        const desired = recorderPage.enabledAppCaptureSources()
        const desiredIds = desired.map(entry => entry.id)
        // Safe to pass any id: removeAppCaptureSource only drops "app" sources,
        // so the system tap's id in this same list is ignored rather than
        // silently killing system audio.
        for (const id of rec.activeCaptureSourceIds) {
            if (desiredIds.indexOf(id) < 0)
                rec.removeAppCaptureSource(id)
        }
        for (const entry of desired)
            rec.addAppCaptureSource(entry.id, entry.gain)
    }

    // Monitoring runs the capture graph with nothing written, so the meters are
    // live while the page merely sits open — levels you can only see after
    // committing to a take are useless for setting gains. Held only while the
    // page is visible AND something is armed: otherwise this would keep the
    // microphone (and its system-wide in-use indicator) open for no reason.
    function updateMonitoring() {
        const rec = recorderPage.audioRecorder
        if (!rec || rec.recording)
            return // a take supersedes monitoring; stopping it is startRecording()'s job.
        const armed = recorderPage.useMic || recorderPage.useSystemAudio
                      || recorderPage.enabledAppCaptureSources().length > 0
        const want = recorderPage.visible && armed
        if (want && !rec.monitoring) {
            // A new capture session restarts elapsedMs at 0, and the peak
            // holds and preview history are both stamped on that clock — carry
            // them across and a peak from the last take stays pinned until the
            // new session catches up. The same reset the Record button does,
            // minus recordedLevels: that belongs to the take now under review.
            recorderPage.sourcePeaks = ({})
            recorderPage.sourcePeakHoldUntil = ({})
            recorderPage.sourceLevelsByKey = ({})
            recorderPage.monitorLevels = []
            rec.startMonitoring(recorderPage.useMic, recorderPage.selectedMicId,
                                recorderPage.useSystemAudio, AppSettings.recordingMicGain,
                                AppSettings.recordingSystemGain, recorderPage.selectedSystemSourceId,
                                recorderPage.enabledAppCaptureSources())
        } else if (!want && rec.monitoring) {
            rec.stopMonitoring()
        }
    }

    // Band colour for a display level — one rule, applied identically by the
    // numeric readouts and the in-groove meters (see AudioRecorderIO::levelBand).
    function bandColor(displayLevel) {
        if (!recorderPage.audioRecorder)
            return Theme.textMuted
        const band = recorderPage.audioRecorder.levelBand(displayLevel)
        if (band === "hot")
            return Theme.danger
        if (band === "good")
            return Theme.success
        return Theme.warning
    }

    onUseMicChanged: recorderPage.syncCapture()
    onUseSystemAudioChanged: recorderPage.syncCapture()
    onSelectedMicIdChanged: recorderPage.syncCapture()
    onSelectedSystemSourceIdChanged: recorderPage.syncCapture()

    Connections {
        target: AppSettings
        // Arming/disarming a per-app row, and per-app gain changes, both land
        // here — reconcileAppSources() upserts, so one handler covers both.
        function onRecordingAppCaptureEnabledChanged() { recorderPage.syncCapture() }
        function onRecordingAppGainsChanged() { recorderPage.reconcileAppSources() }
        // Without these two, dragging the mic or system slider only moved the
        // number: the worker was handed a gain once, when capture started, and
        // nothing ever revised it — so the meter (which measures POST-gain)
        // never budged either.
        function onRecordingMicGainChanged() { recorderPage.syncCapture() }
        function onRecordingSystemGainChanged() { recorderPage.syncCapture() }
    }

    // audioRecorder.lastError clearing (a fresh startRecording() call
    // clears it server-side) should also clear the shared banner if
    // it was the error currently showing.
    Connections {
        target: recorderPage.audioRecorder
        // startRecording() tears monitoring down and nothing restarted it, so
        // every meter on the page sat dead from the moment a take ended until
        // the user happened to toggle a source. updateMonitoring() is
        // idempotent and early-returns while a take runs, so one handler
        // covers the take-ended, start-rejected and worker-auto-stopped paths
        // alike. Safe to run inside this emit only because stopRecording()
        // posts the worker teardown first — see its comment.
        function onRecordingChanged() { recorderPage.updateMonitoring() }
        function onRecordingFinished(filePath) {
            // An empty path means the capture wrote nothing and the file was
            // discarded (see AudioRecorder::recordingFinished) — leave the
            // previous take playable rather than pointing at nothing.
            if (filePath === "")
                return
            recorderPage.lastRecordingPath = filePath
            recorderPlayer.source = "file://" + filePath
        }
        function onLevelUpdated() {
            // Only a take advances the readout. Monitor ticks carry the same
            // elapsedMs (see takeDurationMs) and must leave the finished take's
            // length standing.
            if (recorderPage.audioRecorder.recording)
                recorderPage.lastTakeMs = recorderPage.audioRecorder.elapsedMs

            // Mapped to a display level HERE, once per tick, not per-point
            // in onPaint — see AudioRecorderIO::displayLevelFromRms() for
            // why raw RMS is unusable as a pixel coordinate.
            const level = recorderPage.audioRecorder.displayLevel(recorderPage.audioRecorder.currentLevel)
            if (level > 0.0)
                recorderPage.silentCaptureStartMs = -1
            else if (recorderPage.silentCaptureStartMs < 0)
                recorderPage.silentCaptureStartMs = recorderPage.audioRecorder.elapsedMs
            if (AppSettings.animationsEnabled) {
                const samples = recorderPage.levelSamples.slice()
                samples.push(level)
                if (samples.length > 100)
                    samples.shift()
                recorderPage.levelSamples = samples
            } else {
                recorderPage.levelSamples = [level]
            }

            // Full history for the review waveform, independent of the
            // 100-sample live window above. Halved rather than truncated when
            // it grows past the cap: dropping the oldest samples would make
            // the playhead's x-axis stop matching the file, while keeping
            // every other sample keeps FULL coverage at half the resolution —
            // and the canvas can only draw ~width columns anyway. 12000
            // entries is roughly 15 minutes at ~13 ticks/second before the
            // first halving.
            const captureMs = recorderPage.audioRecorder.elapsedMs
            if (recorderPage.audioRecorder.recording) {
                if (recorderPage.isNewCaptureEpoch(recorderPage.recordedLevels, captureMs)) {
                    recorderPage.recordedLevels = [{ ms: captureMs, level: level }]
                } else {
                    const history = recorderPage.recordedLevels.slice()
                    history.push({ ms: captureMs, level: level })
                    if (history.length > 12000)
                        recorderPage.recordedLevels = history.filter((_, i) => i % 2 === 0)
                    else
                        recorderPage.recordedLevels = history
                }
            } else {
                // Monitoring: preview only, and it must never reach the take's
                // history. ~2 minutes at ~13 ticks/second, four times the 30s
                // the bar actually draws.
                if (recorderPage.isNewCaptureEpoch(recorderPage.monitorLevels, captureMs)) {
                    recorderPage.monitorLevels = [{ ms: captureMs, level: level }]
                } else {
                    const preview = recorderPage.monitorLevels.slice()
                    preview.push({ ms: captureMs, level: level })
                    if (preview.length > 1600)
                        preview.shift()
                    recorderPage.monitorLevels = preview
                }
            }

            // Whole-map reassignment, like AppSettings.recordingAppGains:
            // QML does not signal on in-place mutation of a var-held object.
            const peaks = Object.assign({}, recorderPage.sourcePeaks)
            const holds = Object.assign({}, recorderPage.sourcePeakHoldUntil)
            const levels = {}
            for (const entry of recorderPage.audioRecorder.sourceLevels) {
                const key = recorderPage.sourcePeakKey(entry.kind, entry.id)
                const display = recorderPage.audioRecorder.displayLevel(entry.level)
                levels[key] = display
                const advanced = recorderPage.audioRecorder.holdPeak(
                    peaks[key] !== undefined ? peaks[key] : 0.0,
                    holds[key] !== undefined ? holds[key] : 0,
                    display,
                    recorderPage.audioRecorder.elapsedMs,
                    recorderPage.sourcePeakHoldMs,
                    recorderPage.sourcePeakDecay)
                peaks[key] = advanced.peak
                holds[key] = advanced.holdUntil
            }
            recorderPage.sourcePeaks = peaks
            recorderPage.sourcePeakHoldUntil = holds
            // Rebuilt from scratch, not merged: a source that stopped
            // reporting has no current level, and carrying its last one
            // forward would leave a meter frozen mid-groove.
            recorderPage.sourceLevelsByKey = levels
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
                    objectName: "recordToggleButton"
                    Layout.preferredWidth: 32
                    Layout.preferredHeight: 32
                    enabled: recorderPage.useMic || recorderPage.useSystemAudio
                             || recorderPage.enabledAppCaptureSources().length > 0
                             || recorderPage.audioRecorder.recording

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
                                recorderPage.silentCaptureStartMs = -1
                    recorderPage.sourcePeaks = ({})
                            recorderPage.sourcePeakHoldUntil = ({})
                            recorderPage.sourceLevelsByKey = ({})
                                recorderPage.recordedLevels = []
                                recorderPage.lastTakeMs = 0
                                recorderPlayer.pause()
                                recorderPage.audioRecorder.startRecording(
                                    recorderPage.vaultPath, AppSettings.recordingOutputFolder,
                                    recorderPage.recordingName,
                                    recorderPage.useMic, recorderPage.selectedMicId,
                                    recorderPage.useSystemAudio, AppSettings.recordingMicGain,
                                    AppSettings.recordingSystemGain, recorderPage.selectedSystemSourceId,
                                    recorderPage.enabledAppCaptureSources(),
                                    recorderPage.testMode)
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

                // Play back the take you just made without leaving the page.
                Item {
                    id: playLastRecordingButton
                    Layout.preferredWidth: 32
                    Layout.preferredHeight: 32
                    enabled: recorderPage.lastRecordingPath !== ""
                             && !recorderPage.audioRecorder.recording

                    HoverHandler {
                        id: playLastRecordingHover
                        cursorShape: Qt.PointingHandCursor
                        enabled: playLastRecordingButton.enabled
                    }
                    TapHandler {
                        enabled: playLastRecordingButton.enabled
                        onTapped: recorderPlayer.togglePlay()
                    }
                    Rectangle {
                        anchors.fill: parent
                        radius: AppSettings.borderRadius
                        color: (playLastRecordingHover.hovered && playLastRecordingButton.enabled)
                               ? Theme.surfaceHover : Theme.surface
                    }
                    HoverBorder {
                        anchors.fill: parent
                        active: playLastRecordingButton.enabled && playLastRecordingHover.hovered
                    }
                    ToolTip.visible: playLastRecordingHover.hovered
                    ToolTip.text: recorderPlayer.playing ? qsTr("Pause playback")
                                                         : qsTr("Play the last recording")
                    ToolTip.delay: 500
                    AppIcon {
                        anchors.centerIn: parent
                        name: recorderPlayer.playing ? "pause" : "play"
                        iconSize: 15
                        color: playLastRecordingButton.enabled ? Theme.textPrimary : Theme.textMuted
                    }
                }

                // Route the next take to a temp directory instead of the
                // vault, for checking levels without leaving real audio
                // files in the tree.
                Item {
                    id: testModeToggleButton
                    objectName: "testModeToggleButton"
                    Layout.preferredWidth: 32
                    Layout.preferredHeight: 32
                    // The mode is fixed for the duration of a take: the output
                    // path is chosen at startRecording().
                    enabled: !recorderPage.audioRecorder.recording

                    HoverHandler {
                        id: testModeToggleHover
                        cursorShape: Qt.PointingHandCursor
                        enabled: testModeToggleButton.enabled
                    }
                    TapHandler {
                        enabled: testModeToggleButton.enabled
                        onTapped: recorderPage.testMode = !recorderPage.testMode
                    }
                    Rectangle {
                        anchors.fill: parent
                        radius: AppSettings.borderRadius
                        color: (testModeToggleHover.hovered && testModeToggleButton.enabled)
                               ? Theme.surfaceHover : Theme.surface
                    }
                    HoverBorder {
                        anchors.fill: parent
                        active: testModeToggleButton.enabled && testModeToggleHover.hovered
                    }
                    ToolTip.visible: testModeToggleHover.hovered
                    ToolTip.text: recorderPage.testMode
                                  ? qsTr("Test mode on — recordings are temporary and never enter the vault")
                                  : qsTr("Test mode — record temporarily to check levels")
                    ToolTip.delay: 500
                    AppIcon {
                        anchors.centerIn: parent
                        name: "benchmark"
                        iconSize: 15
                        color: recorderPage.testMode ? "#6fdf9f" : Theme.textPrimary
                    }
                }

                Label {
                    id: recordingDurationLabel
                    objectName: "recordingDurationLabel"
                    text: recorderPage.formatDuration(recorderPage.takeDurationMs)
                    color: "white"
                    font.pixelSize: 16
                }

                // What to call the take. A pure text box (StyleGuide.md "Text
                // fields") — it exists only to be typed into, so it keeps its
                // resting underline rather than going plainUntilHovered.
                // Disabled during a take for the same reason the test-mode
                // toggle is: the output path is chosen once, at
                // startRecording().
                TextField {
                    id: recordingNameField
                    objectName: "recordingNameField"
                    Layout.preferredWidth: 200
                    enabled: !recorderPage.audioRecorder.recording
                    placeholderText: qsTr("Recording name")
                    text: recorderPage.recordingName
                    font.pixelSize: 11
                    background: UnderlineField { focused: recordingNameField.activeFocus }
                    onTextEdited: recorderPage.recordingName = text
                    ToolTip.visible: recordingNameHover.hovered
                    ToolTip.text: qsTr("Name for the next recording — blank uses a timestamp")
                    ToolTip.delay: 500
                    // Editable text needs an I-beam.
                    HoverHandler {
                        id: recordingNameHover
                        cursorShape: Qt.IBeamCursor
                    }
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
                    text: recorderPage.testMode
                          ? qsTr("Test recording (temporary)")
                          : (AppSettings.recordingOutputFolder === ""
                             ? qsTr("Vault root")
                             : AppSettings.recordingOutputFolder)
                    color: recorderPage.testMode ? "#6fdf9f" : Theme.textMuted
                    font.pixelSize: 11
                    elide: Text.ElideLeft

                    HoverHandler {
                        id: recordingDestinationHover
                    }
                    ToolTip.visible: recordingDestinationHover.hovered
                    ToolTip.text: recorderPage.testMode
                                  ? qsTr("Test recordings are temporary and are not saved to the vault")
                                  : qsTr("Recordings are saved to %1")
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

            // Time-windowed waveform + interactive playhead — see
            // WaveformBar.qml's doc comment for why it's windowed rather than
            // squashing the whole take into one frame.
            WaveformBar {
                id: waveformCanvas
                objectName: "waveformBar"
                Layout.fillWidth: true
                Layout.preferredHeight: 80

                // Live while the capture graph is running (a take OR merely
                // monitoring — elapsedMs advances for both, see AudioRecorder's
                // shared worker start), EXCEPT once a finished take is under
                // review: monitoring resumes the instant a take ends, and
                // `capturing` alone would throw that take's playhead away.
                live: recorderPage.waveformLive
                samples: recorderPage.waveformSamples
                windowMs: 30000
                waveStyle: AppSettings.recordingWaveformStyle
                mirrored: AppSettings.recordingWaveformMirrored
                positionMs: recorderPage.waveformLive
                            ? (recorderPage.audioRecorder ? recorderPage.audioRecorder.elapsedMs : 0)
                            : recorderPlayer.position
                durationMs: recorderPage.waveformLive
                            ? (recorderPage.audioRecorder ? recorderPage.audioRecorder.elapsedMs : 0)
                            : recorderPlayer.duration
                // Static bar at the last-known level — no scrolling history —
                // while animations are off (that flag kills
                // continuous-update CPU/GPU work app-wide).
                staticLevel: AppSettings.animationsEnabled ? -1
                             : (recorderPage.levelSamples.length > 0
                                ? recorderPage.levelSamples[recorderPage.levelSamples.length - 1] : -1)

                interactive: true
                // AudioPlayer's `position` is a writable Q_PROPERTY
                // (AudioPlayer.h:17, READ position WRITE setPosition), so the
                // seek is a plain assignment — no extra C++ needed.
                onSeekRequested: ms => recorderPlayer.position = ms
            }

            // Shared warning banner (StyleGuide.md "Warning banners"): shows
            // whichever of the folder-picker rejection / audioRecorder's own
            // lastError / the recording-name advisory is currently non-empty.
            // No dismiss button — clears itself the next time the folder
            // picker succeeds/reopens, lastErrorChanged reports an empty
            // string, or the name stops colliding. Two-tone colour below: a
            // rejection/lastError is a real error, the name advisory is a
            // caution.
            Label {
                objectName: "recorderWarningBanner"
                Layout.fillWidth: true
                visible: text !== ""
                text: recorderPage.recordingFolderError !== "" ? recorderPage.recordingFolderError
                      : recorderPage.systemSourceWarning !== "" ? recorderPage.systemSourceWarning
                      : (recorderPage.audioRecorder && recorderPage.audioRecorder.lastError !== "")
                        ? recorderPage.audioRecorder.lastError
                        : recorderPage.recordingNameWarning
                // Two tones, because the banner now carries two kinds of
                // message: a rejection or a capture failure is an error, while
                // "your name is taken, the take will be numbered" is a caution
                // the user may well accept as-is. Theme.warning is the app's
                // existing "not an error" tone (the recorder's own too-quiet
                // level band already uses it). Keyed on which message actually
                // won the precedence chain above, not on the name warning
                // merely being non-empty — a real error outranks the advisory
                // in colour exactly as it does in text.
                color: (recorderPage.recordingFolderError === ""
                        && recorderPage.systemSourceWarning === ""
                        && (!recorderPage.audioRecorder || recorderPage.audioRecorder.lastError === "")
                        && recorderPage.recordingNameWarning !== "")
                       ? Theme.warning : Theme.danger
                font.pixelSize: 11
                wrapMode: Text.WordWrap
            }

            // Causes C and D of the "system audio didn't pick up Discord"
            // report are both invisible from inside the app: an app routed to
            // a NON-default sink is simply not in @DEFAULT_MONITOR@, and a
            // monitor tap is post-fader so an app turned down in the mixer
            // records quiet. Both look identical to "recording works fine" —
            // a running timer and a flat waveform. This is the only thing
            // that tells the user where to look.
            Label {
                id: silentCaptureHint
                Layout.fillWidth: true
                visible: recorderPage.audioRecorder && recorderPage.audioRecorder.recording
                         && recorderPage.silentCaptureStartMs >= 0
                         && recorderPage.audioRecorder.elapsedMs - recorderPage.silentCaptureStartMs
                            > recorderPage.silentCaptureHintMs
                text: qsTr("No audio captured yet. A sink monitor only carries apps routed to THAT output "
                           + "device, and it records them at their current mixer volume — check the app's own "
                           + "output-device setting, or capture it directly from the per-app rows below.")
                color: Theme.textMuted
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

                // Holds the column the per-app rows fill with their
                // active-stream dot, so every source row's toggle starts at
                // the same x. Mic and system capture have no equivalent
                // "is it playing" state — the mic is a device and a monitor
                // always exists.
                Item {
                    id: micIndicatorSpacer
                    Layout.preferredWidth: 14
                    Layout.preferredHeight: 1
                }

                Item {
                    id: micToggle
                    objectName: "micToggle"
                    Layout.preferredWidth: recorderPage.sourceToggleWidth
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

                AppComboBox {
                    id: micCombo
                    objectName: "micCombo"
                    Layout.preferredWidth: 220
                    enabled: recorderPage.useMic
                    labelRole: "name"
                    model: recorderPage.micDevices
                    selectedId: recorderPage.selectedMicId
                    onPicked: id => recorderPage.selectedMicId = id
                }

                GainSlider {
                    id: micGainSlider
                    objectName: "micGainSlider"
                    level: recorderPage.sourceLevelFor("mic", "")
                    peak: recorderPage.sourcePeakFor("mic", "")
                    levelColor: recorderPage.bandColor(level)
                    peakColor: recorderPage.bandColor(peak)
                    metering: recorderPage.capturing
                    Layout.fillWidth: true
                    enabled: recorderPage.useMic
                    hoverEnabled: true
                    from: 0.0
                    to: recorderPage.maxSourceGain
                    value: AppSettings.recordingMicGain
                    onMoved: AppSettings.recordingMicGain = value

                    HoverHandler {
                        cursorShape: Qt.PointingHandCursor
                        enabled: micGainSlider.enabled
                    }
                }

                GainPercentField {
                    id: micGainPercentField
                    objectName: "micGainPercentField"
                    enabled: micGainSlider.enabled
                    // The setting, NOT micGainSlider.value: a Slider clamps
                    // its value to `to`, so reading the handle would redisplay
                    // an above-ceiling gain as 400%.
                    gain: AppSettings.recordingMicGain
                    maxGain: recorderPage.maxTypedGain
                    onGainEdited: g => AppSettings.recordingMicGain = g
                }

                Label {
                    id: micPeakLabel
                    // Fixed width for the same reason as the gain readout — a
                    // peak swinging between "\u2014" and "-60 dB" must not
                    // resize the slider beside it mid-recording.
                    Layout.preferredWidth: 56
                    horizontalAlignment: Text.AlignRight
                    text: recorderPage.formatPeakDb(recorderPage.sourcePeakFor("mic", ""))
                    color: recorderPage.capturing
                           ? recorderPage.bandColor(recorderPage.sourcePeakFor("mic", ""))
                           : Theme.textMuted
                    font.pixelSize: 11
                    ToolTip.visible: micPeakHover.hovered
                    ToolTip.text: recorderPage.peakTooltip
                    ToolTip.delay: 500
                    HoverHandler { id: micPeakHover }
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

                // Holds the column the per-app rows fill with their
                // active-stream dot, so every source row's toggle starts at
                // the same x. Mic and system capture have no equivalent
                // "is it playing" state — the mic is a device and a monitor
                // always exists.
                Item {
                    id: systemIndicatorSpacer
                    Layout.preferredWidth: 14
                    Layout.preferredHeight: 1
                }

                Item {
                    id: systemAudioToggle
                    objectName: "systemAudioToggle"
                    Layout.preferredWidth: recorderPage.sourceToggleWidth
                    Layout.preferredHeight: 24

                    HoverHandler {
                        id: systemAudioToggleHover
                        cursorShape: Qt.PointingHandCursor
                    }
                    TapHandler {
                        onTapped: {
                            recorderPage.useSystemAudio = !recorderPage.useSystemAudio
                            // The two capture modes still cannot compose — a
                            // monitor tap already contains every app, so
                            // adding a per-app tap on top records that app
                            // twice, milliseconds apart, and the copies comb-
                            // filter. What changed is who yields: the click
                            // wins and clears the other mode, instead of the
                            // other mode making this control dead and leaving
                            // the user to hunt for what blocked them.
                            if (recorderPage.useSystemAudio)
                                AppSettings.recordingAppCaptureEnabled = ({})
                        }
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

                    ToolTip.visible: systemAudioToggleHover.hovered
                    ToolTip.text: qsTr("Captures the selected output device's monitor — every app playing to "
                                       + "THAT device, at the volume it is playing. An app routed elsewhere, or "
                                       + "turned down in the mixer, is missing or quiet here.")
                    ToolTip.delay: 500
                }

                AppComboBox {
                    id: systemSourceCombo
                    objectName: "systemSourceCombo"
                    Layout.preferredWidth: 220
                    enabled: recorderPage.useSystemAudio
                    labelRole: "name"
                    model: recorderPage.systemSources
                    selectedId: recorderPage.selectedSystemSourceId
                    // The currentIndex re-enumeration hardening (systemSources
                    // is re-fetched every few seconds) now lives in
                    // AppComboBox's resolveIndex() — see its top-of-file
                    // comment. Only the warning-clear side effect stays here.
                    onPicked: id => {
                        recorderPage.selectedSystemSourceId = id
                        recorderPage.systemSourceWarning = ""
                    }
                }

                GainSlider {
                    id: systemGainSlider
                    objectName: "systemGainSlider"
                    level: recorderPage.sourceLevelFor("system", recorderPage.selectedSystemSourceId)
                    peak: recorderPage.sourcePeakFor("system", recorderPage.selectedSystemSourceId)
                    levelColor: recorderPage.bandColor(level)
                    peakColor: recorderPage.bandColor(peak)
                    metering: recorderPage.capturing
                    Layout.fillWidth: true
                    enabled: recorderPage.useSystemAudio
                    hoverEnabled: true
                    from: 0.0
                    to: recorderPage.maxSourceGain
                    value: AppSettings.recordingSystemGain
                    onMoved: AppSettings.recordingSystemGain = value

                    HoverHandler {
                        cursorShape: Qt.PointingHandCursor
                        enabled: systemGainSlider.enabled
                    }
                }

                GainPercentField {
                    id: systemGainPercentField
                    objectName: "systemGainPercentField"
                    enabled: systemGainSlider.enabled
                    gain: AppSettings.recordingSystemGain
                    maxGain: recorderPage.maxTypedGain
                    onGainEdited: g => AppSettings.recordingSystemGain = g
                }

                Label {
                    id: systemPeakLabel
                    // Fixed width for the same reason as the gain readout — a
                    // peak swinging between "\u2014" and "-60 dB" must not
                    // resize the slider beside it mid-recording.
                    Layout.preferredWidth: 56
                    horizontalAlignment: Text.AlignRight
                    text: recorderPage.formatPeakDb(recorderPage.sourcePeakFor("system", recorderPage.selectedSystemSourceId))
                    color: recorderPage.capturing
                           ? recorderPage.bandColor(recorderPage.sourcePeakFor("system", recorderPage.selectedSystemSourceId))
                           : Theme.textMuted
                    font.pixelSize: 11
                    ToolTip.visible: systemPeakHover.hovered
                    ToolTip.text: recorderPage.peakTooltip
                    ToolTip.delay: 500
                    HoverHandler { id: systemPeakHover }
                }
            }

            // Per-app capture: N application streams captured SIMULTANEOUSLY,
            // each at its own gain, mixed into the same one WAV alongside
            // the mic (see AudioRecorder.h's class doc comment and
            // AudioRecorderIO::mixPcm16N/planMixTick). Mutually exclusive with
            // the System Audio row above — a monitor tap already contains
            // every app, so composing the two records the same app twice out
            // of phase (see enabledAppCaptureSources()). Must stay the LAST
            // section here — the styleguide guard requires the waveform above
            // every config row, and appending keeps that true.
            ColumnLayout {
                id: appCaptureSection
                Layout.fillWidth: true
                Layout.fillHeight: false

                Label {
                    text: qsTr("Per-app capture")
                    color: Theme.textMuted
                    font.pixelSize: 11
                }

                // The rows below are live now; what this says is what happens
                // if you use them, not why you can't.
                Label {
                    id: appCaptureExclusionNote
                    Layout.fillWidth: true
                    text: qsTr("System Audio is on and already captures every application. Enabling a row "
                               + "below switches to per-app capture instead.")
                    color: Theme.textMuted
                    font.pixelSize: 11
                    wrapMode: Text.WordWrap
                    visible: recorderPage.useSystemAudio
                }

                // Rows persist once seen, so the empty state is now "nothing
                // has played yet this session" — not "nothing is playing".
                // An app that has never made a sound cannot be listed: there
                // is no PipeWire node to learn its name from.
                Label {
                    text: qsTr("No applications have played audio yet — start one to set its level")
                    color: Theme.textMuted
                    font.pixelSize: 11
                    visible: recorderPage.appCaptureRows.length === 0 && !recorderPage.useSystemAudio
                }

                Repeater {
                    id: appCaptureRepeater
                    model: recorderPage.appCaptureRows

                    RowLayout {
                        id: appCaptureRow
                        Layout.fillWidth: true
                        Layout.fillHeight: false
                        spacing: 12

                        // Whether this application is producing audio right
                        // now. A remembered row stays put when its app goes
                        // quiet — which is the point — so something has to say
                        // which rows would actually capture anything if you
                        // pressed record. Hover explains it: an unexplained
                        // coloured dot is decoration.
                        Item {
                            id: appCaptureActiveDot
                            objectName: "appCaptureActiveDot"
                            Layout.preferredWidth: 14
                            Layout.preferredHeight: 14

                            // A third state beyond "playing"/"not playing":
                            // playing but not yet spliced into the running
                            // mix (the enumeration that would join it hasn't
                            // landed yet — see joinLateSources()). Only the
                            // tooltip distinguishes it; the dot's own fill
                            // stays modelData.active, which is still what it
                            // means.
                            readonly property bool capturing: {
                                if (!recorderPage.audioRecorder || !recorderPage.audioRecorder.recording)
                                    return false
                                for (const id of modelData.ids) {
                                    if (recorderPage.audioRecorder.activeCaptureSourceIds.indexOf(id) >= 0)
                                        return true
                                }
                                return false
                            }

                            Rectangle {
                                anchors.centerIn: parent
                                width: 8
                                height: 8
                                radius: 4
                                color: modelData.active ? "#6fdf9f" : "transparent"
                                border.width: 1
                                border.color: modelData.active ? "#6fdf9f" : Theme.borderStrong

                                Behavior on color { ColorAnimation { duration: AppSettings.animationsEnabled ? 100 : 0 } }
                            }
                            HoverHandler {
                                id: appCaptureActiveDotHover
                            }
                            ToolTip.visible: appCaptureActiveDotHover.hovered
                            ToolTip.text: recorderPage.audioRecorder && recorderPage.audioRecorder.recording
                                          && modelData.active && !appCaptureActiveDot.capturing
                                          ? qsTr("%1 is playing but has not joined this recording yet — it joins "
                                                 + "within a few seconds.")
                                            .arg(modelData.name)
                                          : modelData.active
                                          ? qsTr("%1 is playing audio right now and will be captured if this row is on.")
                                            .arg(modelData.name)
                                          : qsTr("%1 is not playing audio right now. You can still set its level here — "
                                                 + "it is captured only if it is playing when you press record.")
                                            .arg(modelData.name)
                            ToolTip.delay: 500
                        }

                        Item {
                            id: appCaptureToggle
                            objectName: "appCaptureToggle"
                            Layout.preferredWidth: recorderPage.sourceToggleWidth
                            Layout.preferredHeight: 24
                            // Deliberately NOT gated on `recording`: arming or
                            // disarming an app mid-take now takes effect on the
                            // live mix (see recorderPage.syncCapture()), so
                            // blocking it would only hide a working capability.

                            HoverHandler {
                                id: appCaptureToggleHover
                                cursorShape: Qt.PointingHandCursor
                                enabled: appCaptureToggle.enabled
                            }
                            TapHandler {
                                enabled: appCaptureToggle.enabled
                                onTapped: {
                                    let e = Object.assign({}, AppSettings.recordingAppCaptureEnabled)
                                    e[modelData.name] = !e[modelData.name]
                                    AppSettings.recordingAppCaptureEnabled = e
                                    // Reciprocal of systemAudioToggle's clear
                                    // — turning a per-app row ON drops System
                                    // Audio rather than being blocked by it.
                                    // Only on the ON edge: turning a row off
                                    // must not silently enable anything.
                                    if (e[modelData.name])
                                        recorderPage.useSystemAudio = false
                                }
                            }
                            Rectangle {
                                anchors.fill: parent
                                radius: AppSettings.borderRadius
                                color: AppSettings.recordingAppCaptureEnabled[modelData.name]
                                       ? (appCaptureToggleHover.hovered ? Theme.success : Theme.successDim)
                                       : (appCaptureToggleHover.hovered ? Theme.surfaceHover : Theme.surface)
                                border.width: 1
                                border.color: AppSettings.recordingAppCaptureEnabled[modelData.name] ? Theme.success : Theme.borderStrong

                                Behavior on color { ColorAnimation { duration: AppSettings.animationsEnabled ? 100 : 0 } }
                            }
                            Label {
                                id: appCaptureToggleLabel
                                anchors.centerIn: parent
                                width: parent.width - 8
                                horizontalAlignment: Text.AlignHCenter
                                elide: Text.ElideRight
                                text: modelData.name
                                color: AppSettings.recordingAppCaptureEnabled[modelData.name] ? "white" : Theme.textMuted
                                font.pixelSize: 11
                            }
                        }

                        // Holds the column the mic/system rows fill with their
                        // 220px device ComboBox, so a per-app gain slider
                        // starts at the same x as the two above it. A per-app
                        // stream has no device to choose — it IS the choice.
                        Item {
                            id: appCaptureComboSpacer
                            Layout.preferredWidth: 220
                            Layout.preferredHeight: 1
                        }

                        GainSlider {
                            id: appCaptureGainSlider
                            objectName: "appCaptureGainSlider"
                            Layout.fillWidth: true
                            // Same gating as micGainSlider/systemGainSlider: a
                            // gain for a source that isn't being captured is
                            // inert, and must LOOK inert. No longer gated on
                            // `recording` — a live take honours gain changes.
                            enabled: AppSettings.recordingAppCaptureEnabled[modelData.name] === true
                            level: recorderPage.appRowLevel(modelData.ids)
                            peak: recorderPage.appRowPeak(modelData.ids)
                            levelColor: recorderPage.bandColor(level)
                            peakColor: recorderPage.bandColor(peak)
                            metering: recorderPage.capturing
                            from: 0.0
                            to: recorderPage.maxSourceGain
                            value: recorderPage.appGainFor(modelData.name)
                            onMoved: recorderPage.setAppGain(modelData.name, value)

                            HoverHandler {
                                cursorShape: Qt.PointingHandCursor
                                enabled: appCaptureGainSlider.enabled
                            }
                        }

                        GainPercentField {
                            id: appCaptureGainPercentField
                            objectName: "appCaptureGainPercentField"
                            enabled: appCaptureGainSlider.enabled
                            gain: recorderPage.appGainFor(modelData.name)
                            maxGain: recorderPage.maxTypedGain
                            onGainEdited: g => recorderPage.setAppGain(modelData.name, g)
                        }

                        Label {
                            id: appCapturePeakLabel
                            // Fixed width for the same reason as the gain
                            // readout — a peak that grows from "0" to "100"
                            // must not resize the slider beside it
                            // mid-recording.
                            Layout.preferredWidth: 56
                            horizontalAlignment: Text.AlignRight
                            text: recorderPage.formatPeakDb(recorderPage.appRowPeak(modelData.ids))
                            color: recorderPage.capturing
                                   ? recorderPage.bandColor(recorderPage.appRowPeak(modelData.ids))
                                   : Theme.textMuted
                            font.pixelSize: 11
                            ToolTip.visible: appCapturePeakHover.hovered
                            ToolTip.text: recorderPage.peakTooltip
                            ToolTip.delay: 500
                            HoverHandler { id: appCapturePeakHover }
                        }
                    }
                }
            }
        }
    }

    AudioPlayer {
        id: recorderPlayer
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
