#pragma once

#include <QString>
#include <QUrl>

#include "../AudioDecoder.h"
#include "Task.h"

// Orchestrates: ffmpeg decode -> ASR engine -> vault sidecar .md. Pure ASR —
// speaker diarization is a SEPARATE task (DiarizeTask, standalone or as a
// runAllJobs() pipeline step) — a fresh transcript is always written
// speaker-less; a later diarize task splices "[Speaker N]" tags in place.
// See DESIGN.md "Speaker diarization".
//
// The ONLY redirect-eligible kind: the combine preflight may retarget this
// task's `fileUrl` at a merged output before dispatch (see
// TranscriptionController::maybeRedirectForCombine()), which is why run()
// reads `fileUrl` fresh rather than caching a path at construction.
class TranscribeTask : public Task {
    Q_OBJECT
public:
    TranscribeTask(QUrl fileUrl, QUrl vaultUrl, QString modelId, QObject *parent = nullptr);

    QString kind() const override { return QStringLiteral("transcribe"); }
    QString kindLabel() const override { return QStringLiteral("Transcribe"); }
    QString displayName() const override;
    // Pausable during decode/transcribing (real cooperative checkpoints);
    // "loading" (whisper's blocking, unhookable model-load call) has no
    // interruption point at all — see run()'s doc comment — so a pause
    // requested then only takes visible effect once loading finishes. The UI
    // layer (not this flag) is responsible for treating a pause click during
    // "loading" as a no-op rather than queuing it.
    Capabilities capabilities() const override { return {/*pausable=*/true, /*cancellableMidRun=*/true}; }
    // Only the ASR engine — see run()'s ensureAsr() call. Declaring this
    // (rather than relying on the default) is what makes a batch of
    // transcribes keep one warm model across files: the dispatch-time
    // residency policy leaves asr alone between them.
    EngineNeeds engineNeeds() const override { return {/*asr=*/true, /*label=*/false, /*diar=*/false}; }
    // Multi-stage ASR pipeline — the overall bar blends these four phases by
    // their rolling-average durations (see Task::ProgressModel).
    ProgressModel progressModel() const override { return ProgressModel::WeightedPhases; }
    QStringList progressPhases() const override
    {
        return {QStringLiteral("loading"), QStringLiteral("decoding"), QStringLiteral("detecting-speech"),
                QStringLiteral("transcribing")};
    }
    bool isPersistable() const override { return true; }
    QJsonObject toJson() const override;
    QVariantMap toVariantMap() const override;
    // Its whisper/parakeet/engine model. An unrecognized id yields no
    // prerequisite at all (there's no real file to pre-download —
    // ensureModel()'s own fallback-to-default resolves it inline instead;
    // see maybeQueueDownloadTask()).
    ModelPrerequisites prerequisites() const override;

    void run(TaskContext &ctx) override;

    const QString &modelId() const { return m_modelId; }

    // Stall-watchdog predicate — pure function so it's unit-testable
    // (tst_transcription_queue.cpp) without a live/wedged whisper.cpp process,
    // which isn't reproducible read-only. `noProgressElapsedMs` is wall-clock
    // since the last onProgress/onSegment tick, EXCLUDING any stretch spent
    // legitimately paused (see run()'s onAbort doc comment); `fileSeconds` is
    // the audio's own duration. Audio-length-relative by design — never a
    // flat wall-clock cap — so a genuinely long recording (which legitimately
    // goes quiet between ticks for longer) is never falsely killed; see the
    // .cpp for the chosen constants and why they're conservative.
    static bool isStalled(qint64 noProgressElapsedMs, double fileSeconds);

    // The corrupt-audio repair note (see AudioDecoder::DecodeDiagnostics),
    // shared verbatim between the transcript's frontmatter `corruptAudioNote`
    // (a permanent written record) and the Task-level non-fatal
    // warning reported via TaskContext::reportWarning() (a live Tasks-list
    // surfacing): one string, not two that could drift apart.
    // Empty when nothing was scrubbed. Pure + static so it's unit-testable
    // (tst_task_transcribe.cpp) without a real decode, same reason
    // isStalled() is.
    static QString corruptAudioWarning(const AudioDecoder::DecodeDiagnostics &diagnostics);

signals:
    // Which compute backend the model load actually picked ("CUDA"/
    // "Vulkan"/"CPU") — fired once, right after "loading" resolves.
    void backendNameChanged(const QString &name);
    // Feeds the rolling-average phase-weighting the queue uses to blend
    // overallProgress across loading/decoding/detecting-speech/transcribing
    // (phases with no native 0..1 progress hook of their own).
    void phaseDurationMs(const QString &phase, qint64 durationMs);
    // A new segment landed in the sidecar .md — throttled (not one signal
    // per segment) so a chatty stretch of audio doesn't flood the GUI
    // thread. Drives TranscriptionController's liveUpdateTick.
    void segmentWritten();

private:
    QString m_modelId;
};
