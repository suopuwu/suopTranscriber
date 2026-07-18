#pragma once

#include <QString>
#include <QUrl>

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
