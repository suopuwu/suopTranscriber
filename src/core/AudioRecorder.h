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

// Canonical 44-byte PCM WAV header (RIFF/WAVE/fmt /data chunks) describing
// dataSize bytes of payload at the given format. Written as a placeholder
// (dataSize=0) at recording start, then rewritten in place by
// patchWavHeader() once the final size is known — this app never buffers a
// whole recording in memory just to compute its size up front (same
// flush-as-you-go durability convention as SegmentLineWriter's .md writes).
QByteArray buildWavHeader(quint32 dataSize, quint32 sampleRate = 48000, quint16 channelCount = 2,
                          quint16 bitsPerSample = 16);

// Seeks an already-open file back to byte 0 and rewrites its 44-byte header
// with the final dataSize, leaving the file position and all previously
// appended PCM payload untouched. Called once, from the worker's stop
// finalization, after every chunk has already been appended incrementally.
void patchWavHeader(QFile &file, quint32 dataSize, quint32 sampleRate = 48000, quint16 channelCount = 2,
                     quint16 bitsPerSample = 16);

// "<vaultUrl>/[outputRelativeFolder/]suopAudio_yyyyMMdd-HHmmss.wav" — see
// AudioRecorder::startRecording()'s doc comment for the exact join rule
// (outputRelativeFolder empty => straight under the vault root).
QString buildOutputFilePath(const QUrl &vaultUrl, const QString &outputRelativeFolder, const QDateTime &timestamp);

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

    // {id, name} per input device from QMediaDevices::audioInputs() — id is
    // the device's own stable QAudioDevice::id() (opaque, NOT the friendly
    // name), name is description() for the QML dropdown label.
    Q_INVOKABLE QVariantList availableMicDevices() const;

    // Builds the output path (see AudioRecorderIO::buildOutputFilePath():
    // "<vaultUrl>/<outputRelativeFolder>/suopAudio_yyyyMMdd-HHmmss.wav", or
    // straight under vaultUrl if outputRelativeFolder is empty) and starts
    // capturing on the worker thread. A no-op that sets lastError (without
    // touching recording state) if a recording is already in progress, or if
    // neither useMic nor useSystemAudio is requested.
    Q_INVOKABLE void startRecording(const QUrl &vaultUrl, const QString &outputRelativeFolder, bool useMic,
                                     const QString &micDeviceId, bool useSystemAudio, double micGain,
                                     double systemGain);

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
    // finalized the WAV file on disk.
    void recordingFinished(const QString &filePath);

private:
    QThread m_thread;
    AudioRecorderWorker *m_worker = nullptr;
    bool m_recording = false;
    qreal m_currentLevel = 0.0;
    qint64 m_elapsedMs = 0;
    QString m_lastError;
};
