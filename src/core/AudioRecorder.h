#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QFile>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QThread>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>
#include <qqml.h>

#include <functional>

class AudioRecorderWorker;
class QProcess;

// Pure, hardware-free helpers for AudioRecorder's WAV/mixing math — declared
// here (mirroring SegmentLineWriter/spliceBodyChapters in
// TranscriptionController.h) specifically so tests can drive them directly
// with synthetic int16 buffers, without a real QAudioSource or `parec`
// process. AudioRecorderWorker (the actual capture plumbing) is the only
// caller in production and lives entirely in AudioRecorder.cpp.
namespace AudioRecorderIO {

// Clamps a wider-than-16-bit accumulator (a post-gain mix can overflow the
// int16 range even when both inputs were in range) down to the valid signed
// 16-bit range instead of wrapping.
qint16 clampToInt16(qint32 value);

// Mixes N 16-bit LE PCM buffers sample-for-sample with independent per-buffer
// gains: out[i] = clamp16(sum_k(buffers[k][i] * gains[k])). Output length is
// the LONGEST buffer's whole-sample count; a shorter or EMPTY buffer
// contributes silence past its end — that is how planMixTick()'s stalled
// sources are mixed in without holding up the tick. A gains list shorter than
// buffers treats the missing entries as 1.0; an empty buffers list yields an
// empty result. Subsumes the old two-source mixer and the single-source
// gain-scale path: one buffer at gain g IS a gain-scale, so there is no
// separate one-source branch to keep in sync.
QByteArray mixPcm16N(const QList<QByteArray> &buffers, const QList<double> &gains);

// How much of each capture source the next mix tick consumes.
struct MixTickPlan {
    // Frame-aligned bytes to pop from every non-stalled source; 0 = nothing
    // can be mixed yet.
    int bytes = 0;
    // Per-source: mixed as silence this tick, with NOTHING popped.
    QList<bool> stalled;
};

// Decides the next mix tick from each source's currently queued byte count.
// The naive rule (pop min(all queues)) deadlocks the whole recording the
// moment ONE source stops delivering — with N user-chosen app streams, an app
// that pauses playback is the expected case, not an edge case. So a source
// lagging the best-supplied source by more than stallThresholdBytes is marked
// stalled: it is mixed as silence and nothing is popped from it, and `bytes`
// is the frame-aligned min over the REMAINING sources. The mark is sticky
// while a stalled source stays empty (wasStalled[k] && queuedBytes[k] == 0) —
// without stickiness a dead source's gap collapses as the live queues drain,
// un-stalls it, and the recording proceeds in threshold-sized bursts instead
// of smoothly. A stalled source that delivers again un-stalls immediately;
// its audio then lands late by up to the stall, which is the deliberate
// trade against freezing capture outright.
MixTickPlan planMixTick(const QList<int> &queuedBytes, const QList<bool> &wasStalled, int stallThresholdBytes,
                        int frameSize);

// The individual application playback streams out of a parseSystemSources()
// result — its "stream:<serial>" entries, in order, with monitors and the
// synthetic default entry dropped. A filter rather than a second parser: the
// per-app capture list and the source dropdown are the same enumeration, and
// two parsers over one pw-dump would be two things to keep in step.
QVariantList filterAppStreams(const QVariantList &systemSources);

// The recorder's per-app capture rows: one entry per APPLICATION rather than
// per live stream. {name, ids, active} — ids is every live filterAppStreams()
// id whose display name is `name` (empty for a remembered app that is
// currently silent), active is ids non-empty. Entries are the union of the
// live streams and `rememberedNames`, sorted by name.
//
// Keyed on the display name because that is the only stable handle there is:
// a "stream:<object.serial>" id dies with the connection and a new one is
// minted when the app reconnects, so a row keyed on the id vanishes the
// moment the app goes quiet and can never come back. The name survives,
// which is what lets a user set Discord's level before the call starts.
// Several live ids can share one name (two windows of the same app), and
// they are all kept: the user is choosing an application, not a stream.
//
// Sorted rather than live-first so a row never moves under the cursor when
// an unrelated app starts or stops — the `active` flag carries that
// information instead.
QVariantList buildAppCaptureRows(const QVariantList &appStreams, const QStringList &rememberedNames);

// The subset of `desired` ({id, gain} maps, as startRecording() takes) whose
// ids are not already in `activeIds` — what a running capture still has to
// spawn a process for. Pure so the late-join diff is testable without a
// PipeWire graph or a live recording, and so it cannot quietly gain a
// duplicate: adding a source twice would put that application into the mix
// twice, out of phase, which is the same comb-filtering failure the
// System-Audio exclusion exists to prevent. Entries with an empty id are
// dropped.
QVariantList pendingCaptureAdditions(const QVariantList &desired, const QStringList &activeIds);

// The capture command for one source id, program first, arguments after —
// {"parec", "-d", "@DEFAULT_MONITOR@", …} for the default/unrecognized case,
// {"parec", "-d", "<node.name>.monitor", …} for "monitor:<node.name>", and
// {"pw-record", "--target", "<target>", …} for "stream:<target>". Every form
// emits bare s16le/48000/2ch PCM, so all sources are directly mixable. Pulled
// out of the worker so the id→command mapping is testable without a PipeWire
// server (it is the same mapping every per-app source uses).
QStringList captureCommand(const QString &sourceId);

// RMS amplitude of a 16-bit LE PCM buffer, normalized to 0.0-1.0 against the
// int16 magnitude (32768) — feeds AudioRecorder::currentLevel. 0.0 for an
// empty (or sub-one-sample) buffer.
qreal rmsPcm16(const QByteArray &pcm);

// Maps a linear RMS level (see rmsPcm16) onto a 0.0-1.0 DISPLAY level for the
// recorder's waveform, over a -60 dBFS floor. Linear RMS is the honest
// measurement but a terrible pixel coordinate: conversational speech sits at
// roughly 0.01-0.10 RMS, so a straight linear map pins the trace to the
// bottom tenth of the canvas. Over the dB scale the same speech lands at
// ~0.33-0.66 and only genuine clipping reaches 1.0. Monotonic, clamped at
// both ends; silence (and anything at or below the floor) is exactly 0.0.
qreal displayLevelFromRms(qreal rms);

// One tick of a peak-hold meter: rises INSTANTLY to a new maximum, falls by
// at most decayPerTick otherwise, and never goes below the current level or
// below 0. An instantaneous per-tick level is useless for setting gains — at
// ~13 ticks a second it flickers faster than it can be read — while a
// never-decaying maximum latches onto one cough and stays there for the rest
// of the session. Holding the peak and bleeding it off slowly is what makes
// two sources comparable at a glance. Pure and parameterised precisely so
// the decay rate is a caller's choice and the rule itself is unit-testable
// with no audio hardware.
qreal decayPeak(qreal previousPeak, qreal currentLevel, qreal decayPerTick);

// One tick of a peak meter that actually HOLDS. decayPeak() alone never does:
// it bleeds toward the CURRENT level, not toward silence, so the peak only ever
// travels the gap between itself and the running signal — for speech that gap
// is small and closes in a fraction of a second, however slow the decay rate
// is. Sizing the rate for "3 seconds" therefore described a full-scale-to-
// silence release that never happens in practice.
//
// This is the rule a hardware meter uses instead: rise instantly to a new
// maximum and pin it there for holdMs, THEN release at decayPerTick. The line
// stays where the loudest recent moment was, for a duration you can state.
//
// Returns {"peak", "holdUntil"} — holdUntil is a timestamp on the same clock as
// nowMs, to be fed back in on the next tick. Pure, so the hold duration is
// unit-testable without audio hardware or a wall clock.
QVariantMap holdPeak(qreal previousPeak, qint64 previousHoldUntilMs, qreal currentLevel,
                     qint64 nowMs, qint64 holdMs, qreal decayPerTick);

// Which band a DISPLAY level (see displayLevelFromRms) falls in, for the
// recorder's colour-coded meters: "low" (too quiet to be worth keeping),
// "good", or "hot" (close enough to full scale that peaks will clip).
// Thresholds are stated in dBFS below because that is what they mean; the
// display scale is a linear remap of it, so they convert exactly.
//   kGoodFloorDb  -27 dBFS  -> display 0.55
//   kHotFloorDb    -6 dBFS  -> display 0.90
// -27 dBFS is deliberately below conversational speech's own RMS range
// (~-40..-20 dBFS, see displayLevelFromRms) so ordinary talking at a sane
// gain reads "good" rather than nagging yellow; it flags a source that is
// genuinely too quiet to equalise against the others. Pure so the bands are
// unit-testable and identical everywhere they are drawn.
QString levelBand(qreal displayLevel);

// The dBFS a display level came from — the inverse of displayLevelFromRms()'s
// remap, so the meters can be labelled in the unit the thresholds were chosen
// in. Exposed rather than re-derived in QML because the -60 dB floor is shared
// with displayLevelFromRms and levelBand: three copies of that constant would
// be three things to keep in step. A display level of 0 is silence (or
// anything under the floor) and reports the floor itself.
qreal dbFromDisplayLevel(qreal displayLevel);
// The floor the display scale bottoms out at, in dBFS (-60).
qreal displayFloorDb();
// The band boundaries as display levels, for drawing the meter's own scale.
qreal levelBandGoodFloor();
qreal levelBandHotFloor();

// Canonical 44-byte PCM WAV header (RIFF/WAVE/fmt /data chunks) describing
// dataSize bytes of payload at the given format. Written as a placeholder
// (dataSize=0) at recording start, then re-stamped in place by
// patchWavHeader() roughly once a second during capture and once more at
// stop — this app never buffers a whole recording in memory just to compute
// its size up front (same flush-as-you-go durability convention as
// SegmentLineWriter's .md writes).
QByteArray buildWavHeader(quint32 dataSize, quint32 sampleRate = 48000, quint16 channelCount = 2,
                          quint16 bitsPerSample = 16);

// Rewrites an already-open file's 44-byte header with the dataSize captured
// so far, flushes, and RESTORES the write position — all previously appended
// PCM payload untouched. Called periodically during capture (so a crash
// leaves a playable file up to the last checkpoint instead of one whose
// header still claims zero samples) and once more from the worker's stop
// finalization. The position restore is load-bearing for the periodic call:
// without it the next payload append would overwrite the recording from its
// first sample.
void patchWavHeader(QFile &file, quint32 dataSize, quint32 sampleRate = 48000, quint16 channelCount = 2,
                     quint16 bitsPerSample = 16);

// "<vaultUrl>/[outputRelativeFolder/]suopAudio_yyyyMMdd-HHmmss.wav" — see
// AudioRecorder::startRecording()'s doc comment for the exact join rule
// (outputRelativeFolder empty => straight under the vault root).
QString buildOutputFilePath(const QUrl &vaultUrl, const QString &outputRelativeFolder, const QDateTime &timestamp);

// "<tempRoot>/suopTranscriber-test/suopAudio_yyyyMMdd-HHmmss.wav" — where a
// TEST recording goes: outside the vault entirely, so a level check never
// leaves a real untranscribed audio file in the tree. tempRoot is
// QDir::tempPath() in production; injected so this is testable against a
// QTemporaryDir. On this app's target (CachyOS) /tmp is tmpfs, so a test take
// never reaches a disk in the first place — which is why there is no separate
// in-memory capture path to maintain.
QString buildTestOutputFilePath(const QString &tempRoot, const QDateTime &timestamp);

// Deletes all but the `keepNewest` most recent suopAudio_*.wav files in `dir`,
// newest by filename (the timestamp in the name sorts lexicographically —
// mtime does not survive a copy, see the combine scanner's note on the same
// trap). Returns how many it removed. Called before each test recording
// starts: test takes are disposable by definition, and on tmpfs they are
// spending RAM until something removes them. Only ever touches files matching
// the app's own naming, never the directory itself and never anything else in
// it.
int pruneTestRecordings(const QString &dir, int keepNewest);

// Parses `pw-dump`'s JSON array (PipeWire's node graph dump) into the same
// {id, name} shape as availableMicDevices(), for the System Audio source
// dropdown (see queue item 036). The first entry is always the synthetic
// default {"@DEFAULT_MONITOR@", "Default output"} so today's behavior stays
// the default even when nothing else parses, followed by:
//  - one entry per "Audio/Sink" node (a real output device's monitor) — id
//    is "monitor:<node.name>" (AudioRecorder::startSystemAudio() strips the
//    prefix and appends ".monitor" for `parec -d`), name is node.description
//    (falling back to node.name if description is missing).
//  - one entry per "Stream/Output/Audio" node (an individual application's
//    playback stream) — id is "stream:<object.serial>" (falling back to
//    "stream:<node.name>" if the node has no serial; skipped entirely if it
//    has neither), name per streamDisplayName() (skipped entirely when that
//    yields nothing — a stream with no label is nothing worth showing).
// Every other media.class (mic inputs, non-audio nodes, clients, …) is
// filtered out. Malformed/empty/non-array JSON degrades to just the
// synthetic default entry — never crashes. A free function (not a QProcess
// call) specifically so it's testable against a captured pw-dump fixture
// with no real PipeWire server involved, mirroring every other helper in
// this namespace — which is also why the process-ancestry lookup is INJECTED
// rather than read from /proc in here: a fixture's pids would otherwise
// resolve against whatever happens to be running on the test machine.
// Passing no resolver keeps the parse hermetic (streamDisplayName() then
// works from the node's own properties alone).
QVariantList parseSystemSources(const QByteArray &pwDumpJson,
                                const std::function<QStringList(qint64)> &ancestryFor = {});

// True for the names that identify a shell, launcher or runtime rather than
// an application — "Chromium", "Electron", "exe", "node". These are worse
// than useless as a label: an Electron app's audio stream reports
// application.name "Chromium", so Discord, Slack and VS Code all arrive
// wearing the same name. Seeing one means keep looking (up the process
// ancestry, then at the binary).
bool isGenericProcessName(const QString &name);

// Process names for `pid` and its /proc PPid ancestors, nearest first, up to
// maxDepth hops (stopping at pid 0/1 so it never walks into init). The
// ancestry walk is the trick that makes Electron apps nameable: the process
// that owns the audio stream is a helper CHILD of the real application, so
// its own name is the generic shell name while an ancestor's is "Discord" —
// ported from suopPanel's mixer, including the gotcha that /proc/<pid>/status
// reports size 0, so it must be read with readAll() rather than an
// atEnd()/readLine() loop that silently reads nothing. Each hop's name is
// status' Name: (comm), because argv[0] is untrustworthy for exactly the apps
// this exists for — Chromium rewrites argv into a long process title, and an
// Electron re-exec via /proc/self/exe leaves argv[0] "exe". argv[0]'s
// basename is used only to repair comm's 15-char kernel truncation. Empty on
// any platform without /proc.
QStringList processAncestryNames(qint64 pid, int maxDepth = 8);

// Human-readable label for one application playback stream, from its pw-dump
// props plus the process ancestry of its application.process.id (see
// processAncestryNames()). Precedence:
//   1. application.name, unless it is a generic Chromium/Electron shell name
//   2. the nearest non-generic ancestry process name, title-cased — this is
//      what turns "Chromium" into "Discord"
//   3. application.process.binary's basename, if not itself generic
//   4. media.name
//   5. application.name after all, generic or not — a real Chromium browser
//      has no better name to offer, and "Chromium" is the right label for it
// Empty when a node offers none of these; the caller drops such streams.
QString streamDisplayName(const QVariantMap &props, const QStringList &ancestryNames);

}

// Records microphone + system audio (captured via `parec`'s default-sink
// monitor, the same QProcess-based shell-out convention AudioDecoder.cpp
// already uses for ffmpeg) into a single mixed WAV file, each source at its
// own independent gain. Mic capture uses an explicit Int16/48000Hz/2-channel
// QAudioFormat matching parec's `--format=s16le --rate=48000 --channels=2`
// exactly, so both PCM streams are directly mixable sample-for-sample with
// no resampling step (see CLAUDE.md's load-bearing constraints for this
// feature).
//
// Owns a dedicated QThread (mirrors TranscriptionController): the
// QAudioSource, the parec QProcess, both byte-queue buffers, and the mixing
// QTimer all live on AudioRecorderWorker, moved to m_thread — recording (and
// its file IO) never touches the GUI thread. Only start/stop are ever
// invoked from QML; everything else is worker-internal and reached via
// queued cross-thread calls/signals only (never called directly from the
// GUI thread, since the worker's members — QFile, QProcess, QAudioSource —
// aren't safe to touch from two threads at once).
//
// Per-application audio capture IS supported: N application streams can be
// captured at once, each at its own capture gain, mixed into the one WAV
// (see startRecording()'s appCaptureSources and AudioRecorderIO::mixPcm16N).
// What remains out of scope is separate output TRACKS per source — still one
// mixed WAV, never a multi-track file — see tst_future_spec.cpp's
// perAppAudioCapture_singleApplicationIsolation placeholder.
class AudioRecorder : public QObject {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(bool recording READ isRecording NOTIFY recordingChanged)
    // True while the capture graph is running purely to drive the meters —
    // same sources, same level ticks, nothing written. See startMonitoring().
    Q_PROPERTY(bool monitoring READ isMonitoring NOTIFY monitoringChanged)
    // RMS of the most recently mixed chunk, normalized 0.0-1.0 (see
    // AudioRecorderIO::rmsPcm16). Throttled to ~50-100ms server-side (see
    // AudioRecorderWorker's mixing-tick doc comment in the .cpp) — never
    // updated per-sample or per-raw-callback, so QML's level-meter animation
    // can't clog the GUI thread with a signal per audio buffer, mirroring
    // TranscriptionController's liveUpdateTick throttling convention.
    Q_PROPERTY(qreal currentLevel READ currentLevel NOTIFY levelUpdated)
    // Wall-clock ms since startRecording() actually began capturing —
    // updated on the SAME throttled tick as currentLevel rather than running
    // a second independent timer just for elapsed time.
    Q_PROPERTY(qint64 elapsedMs READ elapsedMs NOTIFY levelUpdated)
    // Per-capture-source levels for the most recent throttled tick, in the
    // order the sources were started: {kind, id, level} maps, where kind is
    // "mic" / "system" / "app", id is the source id ("" for the mic), and
    // level is that source's POST-GAIN RMS (see currentLevel for the
    // normalization). Post-gain because the question a per-source meter
    // answers is "how much is this source contributing to the mix", which is
    // what the gain sliders adjust — a pre-gain meter would sit still while
    // the user drags the very control they are trying to set. Empty when not
    // recording. Same tick as currentLevel, never a second timer.
    Q_PROPERTY(QVariantList sourceLevels READ sourceLevels NOTIFY levelUpdated)
    // Stream ids of every capture source currently feeding the mix, in
    // m_sources order (the microphone contributes an empty id and is
    // omitted). The truth about what a running take is recording, as opposed
    // to what the page asked for — the late-join diff is computed against
    // this, and it is what lets the UI show a row as armed-but-not-yet-joined.
    // Empty when not recording.
    Q_PROPERTY(QStringList activeCaptureSourceIds READ activeCaptureSourceIds NOTIFY activeCaptureSourcesChanged)
    // Non-empty if `parec` failed to launch/exited unexpectedly (e.g. not
    // installed, or no PulseAudio/PipeWire-pulse server running) or
    // QAudioSource failed to open the requested mic device — surfaced, never
    // silently swallowed (CLAUDE.md / ModelManager's fallback-logging house
    // style, even though the mechanism here is different). Recording stops
    // cleanly (whatever was captured so far is still finalized into a valid
    // WAV) rather than crashing. Cleared at the start of every
    // startRecording() call.
    Q_PROPERTY(QString lastError READ lastError NOTIFY lastErrorChanged)
    // Latest enumeration from refreshSources(): every system audio source for
    // the dropdown (monitors + app streams, see
    // AudioRecorderIO::parseSystemSources()), and the app-stream subset that
    // feeds the per-app capture rows (AudioRecorderIO::filterAppStreams()).
    // sourcesChanged fires ONLY when the enumeration actually differs, so a
    // poll that finds the same graph doesn't rebuild the QML delegates under
    // an in-progress slider drag.
    Q_PROPERTY(QVariantList systemSources READ systemSources NOTIFY sourcesChanged)
    Q_PROPERTY(QVariantList appStreams READ appStreams NOTIFY sourcesChanged)

public:
    explicit AudioRecorder(QObject *parent = nullptr);
    ~AudioRecorder() override;

    bool isRecording() const { return m_recording; }
    qreal currentLevel() const { return m_currentLevel; }
    qint64 elapsedMs() const { return m_elapsedMs; }
    QString lastError() const { return m_lastError; }

    // Thin QML-facing wrapper over AudioRecorderIO::displayLevelFromRms() —
    // the recorder page maps each currentLevel sample through this ONCE, as
    // it is pushed into the waveform history, rather than per-point on every
    // repaint.
    Q_INVOKABLE qreal displayLevel(qreal rms) const { return AudioRecorderIO::displayLevelFromRms(rms); }

    QVariantList sourceLevels() const { return m_sourceLevels; }

    QStringList activeCaptureSourceIds() const { return m_activeCaptureSourceIds; }

    // Thin QML-facing wrapper over AudioRecorderIO::decayPeak(), matching the
    // displayLevel() precedent — the recorder page holds one peak per source
    // and advances it on each levelUpdated tick.
    Q_INVOKABLE qreal decayPeak(qreal previousPeak, qreal currentLevel, qreal decayPerTick) const
    {
        return AudioRecorderIO::decayPeak(previousPeak, currentLevel, decayPerTick);
    }

    // Thin QML-facing wrapper over AudioRecorderIO::holdPeak() — the recorder
    // page advances one entry per source per levelUpdated tick.
    Q_INVOKABLE QVariantMap holdPeak(qreal previousPeak, qint64 previousHoldUntilMs,
                                      qreal currentLevel, qint64 nowMs, qint64 holdMs,
                                      qreal decayPerTick) const
    {
        return AudioRecorderIO::holdPeak(previousPeak, previousHoldUntilMs, currentLevel, nowMs,
                                          holdMs, decayPerTick);
    }

    // Thin QML-facing wrapper over AudioRecorderIO::buildAppCaptureRows().
    // appStreams is passed IN rather than read from m_appStreams so a QML
    // binding that calls this registers the dependency by reading the
    // property itself — a binding over a Q_INVOKABLE that silently consulted
    // a member would never re-evaluate on sourcesChanged.
    Q_INVOKABLE QVariantList appCaptureRows(const QVariantList &appStreams,
                                            const QStringList &rememberedNames) const
    {
        return AudioRecorderIO::buildAppCaptureRows(appStreams, rememberedNames);
    }

    // Thin QML-facing wrapper over AudioRecorderIO::pendingCaptureAdditions(),
    // matching the displayLevel()/appCaptureRows() precedent.
    Q_INVOKABLE QVariantList pendingCaptureAdditions(const QVariantList &desired,
                                                     const QStringList &activeIds) const
    {
        return AudioRecorderIO::pendingCaptureAdditions(desired, activeIds);
    }

    // {id, name} per input device from QMediaDevices::audioInputs() — id is
    // the device's own stable QAudioDevice::id() (opaque, NOT the friendly
    // name), name is description() for the QML dropdown label.
    Q_INVOKABLE QVariantList availableMicDevices() const;

    QVariantList systemSources() const { return m_systemSources; }
    QVariantList appStreams() const { return m_appStreams; }

    // Kicks off a `pw-dump` probe, updating systemSources/appStreams when it
    // returns. ASYNCHRONOUS on purpose: the recorder page re-enumerates on a
    // few-second cadence while it is open, and pw-dump dumps the whole
    // PipeWire graph — waiting on that inline is a GUI-thread stall every
    // tick, which is exactly the "never do real work on the GUI thread" rule
    // this codebase holds everywhere else. A no-op while a probe is already
    // in flight, so a slow dump can't make polls pile up.
    Q_INVOKABLE void refreshSources();

    // Builds the output path (see AudioRecorderIO::buildOutputFilePath():
    // "<vaultUrl>/<outputRelativeFolder>/suopAudio_yyyyMMdd-HHmmss.wav", or
    // straight under vaultUrl if outputRelativeFolder is empty) and starts
    // capturing on the worker thread. A no-op that sets lastError (without
    // touching recording state) if a recording is already in progress, or if
    // NONE of useMic / useSystemAudio / appCaptureSources is requested.
    // systemSourceId is one of availableSystemSources()'s opaque ids (or
    // empty, meaning the default monitor) — see startSystemAudio() in the
    // .cpp for how it's mapped to a capture binary + args. appCaptureSources
    // is one {id, gain} map per application stream to capture
    // SIMULTANEOUSLY into the same mixed WAV, each at its own gain — id is
    // an availableAppStreams() id, gain is a capture gain only (the user's
    // own system volume is never touched). An entry whose id equals
    // systemSourceId is skipped rather than captured twice. testMode true
    // writes the take to a temporary directory (see
    // AudioRecorderIO::buildTestOutputFilePath()) instead of the vault, and
    // prunes older test takes first — vaultUrl and outputRelativeFolder are
    // ignored. Everything else about the capture is identical, so a test
    // take is a faithful preview of what a real one would sound like.
    Q_INVOKABLE void startRecording(const QUrl &vaultUrl, const QString &outputRelativeFolder, bool useMic,
                                     const QString &micDeviceId, bool useSystemAudio, double micGain,
                                     double systemGain, const QString &systemSourceId = QString(),
                                     const QVariantList &appCaptureSources = {}, bool testMode = false);

    // Stops capture, patches the WAV header with the final size, and emits
    // recordingFinished(filePath) once the file is fully finalized on disk.
    // No-op if not currently recording.
    Q_INVOKABLE void stopRecording();

    // Adds one application stream to a recording ALREADY in progress, at its
    // own gain — how an app that starts playing mid-take gets captured (see
    // AudioRecorderIO::pendingCaptureAdditions()). A no-op when not
    // recording, when that id is already being captured, or while System
    // Audio is the source (a monitor tap already contains every app; adding
    // one on top double-records it out of phase). Failure to start the
    // capture process is reported through lastError and leaves the take
    // running on its existing sources.
    Q_INVOKABLE void addAppCaptureSource(const QString &id, double gain);

    // Runs the capture graph WITHOUT recording, so every armed source meters
    // live while the recorder page is merely open — the levels are useless for
    // setting gains if they only appear once you have committed to a take. A
    // no-op while recording (a take supersedes it) and when nothing is armed
    // (no reason to hold a device open).
    Q_INVOKABLE void startMonitoring(bool useMic, const QString &micDeviceId, bool useSystemAudio,
                                      double micGain, double systemGain,
                                      const QString &systemSourceId = QString(),
                                      const QVariantList &appCaptureSources = {});
    Q_INVOKABLE void stopMonitoring();

    // Mid-capture source changes — valid during a take AND while monitoring.
    // Toggling a source used to only repaint the QML: the worker read useMic /
    // useSystemAudio once at start() and never again, so flipping either
    // mid-take did nothing audible.
    Q_INVOKABLE void setMicEnabled(bool enabled, const QString &micDeviceId, double gain);
    Q_INVOKABLE void setSystemAudioEnabled(bool enabled, const QString &sourceId, double gain);
    Q_INVOKABLE void removeAppCaptureSource(const QString &id);

    // Thin QML-facing wrappers over the colour-band rule, matching the
    // displayLevel()/decayPeak() precedent.
    Q_INVOKABLE QString levelBand(qreal displayLevel) const
    {
        return AudioRecorderIO::levelBand(displayLevel);
    }
    Q_INVOKABLE qreal levelBandGoodFloor() const { return AudioRecorderIO::levelBandGoodFloor(); }
    Q_INVOKABLE qreal levelBandHotFloor() const { return AudioRecorderIO::levelBandHotFloor(); }
    Q_INVOKABLE qreal dbFromDisplayLevel(qreal displayLevel) const
    {
        return AudioRecorderIO::dbFromDisplayLevel(displayLevel);
    }
    Q_INVOKABLE qreal displayFloorDb() const { return AudioRecorderIO::displayFloorDb(); }

    bool isMonitoring() const { return m_monitoring; }

signals:
    void recordingChanged();
    void levelUpdated();
    void lastErrorChanged();
    void sourcesChanged();
    // m_activeCaptureSourceIds changed — see activeCaptureSourceIds.
    void activeCaptureSourcesChanged();
    void monitoringChanged();
    // Emitted once stopRecording() (or an internal auto-stop triggered by a
    // fatal capture error — see lastError's doc comment) has fully
    // finalized the WAV file on disk. filePath is EMPTY when the capture
    // recorded no audio at all: that file is deleted rather than left in the
    // vault as a zero-length recording, so there is nothing to point at.
    void recordingFinished(const QString &filePath);

private:
    // Swaps in a fresh enumeration, emitting sourcesChanged() only on a real
    // difference — see the systemSources property.
    void applySources(const QVariantList &sources);

    QThread m_thread;
    AudioRecorderWorker *m_worker = nullptr;
    bool m_recording = false;
    bool m_monitoring = false;
    qreal m_currentLevel = 0.0;
    qint64 m_elapsedMs = 0;
    QVariantList m_sourceLevels;
    QStringList m_activeCaptureSourceIds;
    QString m_lastError;
    QProcess *m_sourceProbe = nullptr; // non-null only while a pw-dump is in flight
    QVariantList m_systemSources;
    QVariantList m_appStreams;
};
