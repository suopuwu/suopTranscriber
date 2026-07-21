#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QFile>
#include <QObject>
#include <QString>
#include <QThread>
#include <QUrl>
#include <QVariantList>
#include <qqml.h>

class AudioRecorderWorker;

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

// Scales every 16-bit LE sample in `pcm` by `gain`, clamping on overflow.
// Used for the mic-only/system-audio-only path — CLAUDE.md/the design doc
// for this feature says to skip the two-source mixing math entirely when
// only one source is enabled, not just mix against a silent second source.
QByteArray gainScalePcm16(const QByteArray &pcm, double gain);

// Mixes two 16-bit LE PCM buffers sample-for-sample:
// mixed[i] = clamp16(a[i]*gainA + b[i]*gainB). Both buffers are expected to
// already be a matched-duration chunk (the mixing timer pops equal byte
// counts from both queues before calling this); if lengths ever differ this
// mixes only up to the shorter buffer's sample count, and a trailing odd
// byte that can't form a whole 16-bit sample is silently dropped.
QByteArray mixPcm16(const QByteArray &a, double gainA, const QByteArray &b, double gainB);

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
//    has neither), name is application.name (falling back to media.name;
//    skipped entirely if both are empty — nothing worth showing).
// Every other media.class (mic inputs, non-audio nodes, clients, …) is
// filtered out. Malformed/empty/non-array JSON degrades to just the
// synthetic default entry — never crashes. A free function (not a QProcess
// call) specifically so it's testable against a captured pw-dump fixture
// with no real PipeWire server involved, mirroring every other helper in
// this namespace.
QVariantList parseSystemSources(const QByteArray &pwDumpJson);

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
// Per-application audio capture (recording ONE specific app like "Firefox"
// individually) is explicitly out of scope for this class — see
// tst_future_spec.cpp's perAppAudioCapture_singleApplicationIsolation
// placeholder.
class AudioRecorder : public QObject {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(bool recording READ isRecording NOTIFY recordingChanged)
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
    // Non-empty if `parec` failed to launch/exited unexpectedly (e.g. not
    // installed, or no PulseAudio/PipeWire-pulse server running) or
    // QAudioSource failed to open the requested mic device — surfaced, never
    // silently swallowed (CLAUDE.md / ModelManager's fallback-logging house
    // style, even though the mechanism here is different). Recording stops
    // cleanly (whatever was captured so far is still finalized into a valid
    // WAV) rather than crashing. Cleared at the start of every
    // startRecording() call.
    Q_PROPERTY(QString lastError READ lastError NOTIFY lastErrorChanged)

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

    // {id, name} per input device from QMediaDevices::audioInputs() — id is
    // the device's own stable QAudioDevice::id() (opaque, NOT the friendly
    // name), name is description() for the QML dropdown label.
    Q_INVOKABLE QVariantList availableMicDevices() const;

    // {id, name} per system audio source for the dropdown: output-device
    // monitors plus individual application playback streams — see
    // AudioRecorderIO::parseSystemSources() for the exact shape. Runs
    // `pw-dump` synchronously (short-lived, mirrors GpuProbe.cpp's runProbe()
    // convention) and degrades to just the synthetic default entry if
    // `pw-dump` isn't installed or times out (plain PulseAudio host, or no
    // PipeWire server running) — never throws, never blocks indefinitely.
    Q_INVOKABLE QVariantList availableSystemSources() const;

    // Builds the output path (see AudioRecorderIO::buildOutputFilePath():
    // "<vaultUrl>/<outputRelativeFolder>/suopAudio_yyyyMMdd-HHmmss.wav", or
    // straight under vaultUrl if outputRelativeFolder is empty) and starts
    // capturing on the worker thread. A no-op that sets lastError (without
    // touching recording state) if a recording is already in progress, or if
    // neither useMic nor useSystemAudio is requested. systemSourceId is one
    // of availableSystemSources()'s opaque ids (or empty, meaning the default
    // monitor) — see startSystemAudio() in the .cpp for how it's mapped to a
    // capture binary + args.
    Q_INVOKABLE void startRecording(const QUrl &vaultUrl, const QString &outputRelativeFolder, bool useMic,
                                     const QString &micDeviceId, bool useSystemAudio, double micGain,
                                     double systemGain, const QString &systemSourceId = QString());

    // Stops capture, patches the WAV header with the final size, and emits
    // recordingFinished(filePath) once the file is fully finalized on disk.
    // No-op if not currently recording.
    Q_INVOKABLE void stopRecording();

signals:
    void recordingChanged();
    void levelUpdated();
    void lastErrorChanged();
    // Emitted once stopRecording() (or an internal auto-stop triggered by a
    // fatal capture error — see lastError's doc comment) has fully
    // finalized the WAV file on disk. filePath is EMPTY when the capture
    // recorded no audio at all: that file is deleted rather than left in the
    // vault as a zero-length recording, so there is nothing to point at.
    void recordingFinished(const QString &filePath);

private:
    QThread m_thread;
    AudioRecorderWorker *m_worker = nullptr;
    bool m_recording = false;
    qreal m_currentLevel = 0.0;
    qint64 m_elapsedMs = 0;
    QString m_lastError;
};
