#pragma once

#include <QString>
#include <QStringList>
#include <QUrl>

#include "Task.h"

// Standalone speaker diarization on an already-transcribed file — re-runs
// (or adds) speaker detection WITHOUT re-transcribing audio. Reads the
// existing .md's segment lines, decodes the sibling audio, runs the
// diarization pass, assigns each existing segment its overlapping speaker,
// and writes "[Speaker N]" tags + updated "speakerDetection:"/"speakers:"
// frontmatter back into the .md in place (prelude/chapters/repeat markers
// preserved). A failure here IS the task's result (like RelabelTask) —
// unlike a transcribe task's inline speakerDetection path, where a
// diarization failure degrades to ASR-only instead.
class DiarizeTask : public Task {
    Q_OBJECT
public:
    // Empty model ids are resolved to ModelManager's defaults by the caller
    // (TranscriptionController::diarizeFile()) so the recorded ids stay
    // concrete for persistence + the download dedup key.
    DiarizeTask(QUrl fileUrl, QUrl vaultUrl, QString segmentationModelId, QString embeddingModelId, double threshold,
                QObject *parent = nullptr);

    QString kind() const override { return QStringLiteral("diarize"); }
    QString kindLabel() const override { return QStringLiteral("Detect speakers"); }
    QString displayName() const override;
    // Pausable (decode is a real killable/SIGSTOP-able ffmpeg subprocess;
    // the diarization pass itself blocks on pause between sherpa-onnx
    // progress ticks) but NOT reliably cancellable mid-pass: diarization has
    // no abort hook, so a cancel requested during it only takes effect at
    // the checkpoint right after.
    Capabilities capabilities() const override { return {/*pausable=*/true, /*cancellableMidRun=*/false}; }
    // Only diarization — see run()'s ensureDiar() call. Declaring
    // this keeps the dispatch-time residency policy from evicting it; run()
    // still force-evicts it itself right before ensureDiar() regardless (see
    // that call's own doc comment for why).
    EngineNeeds engineNeeds() const override { return {/*asr=*/false, /*label=*/false, /*diar=*/true}; }
    // No whisper load, no ASR: decode then the diarization pass. The overall
    // bar weights these two phases (see Task::ProgressModel). The finer
    // "detecting-speakers-segmenting"/"-identifying" sub-phases run() emits
    // both fold into the "detecting-speakers" bucket for weighting.
    ProgressModel progressModel() const override { return ProgressModel::WeightedPhases; }
    QStringList progressPhases() const override
    {
        return {QStringLiteral("decoding"), QStringLiteral("detecting-speakers")};
    }
    bool isPersistable() const override { return true; }
    QJsonObject toJson() const override;
    QVariantMap toVariantMap() const override;
    // Its chosen segmentation + embedding models — no whisper model (this
    // kind never transcribes).
    ModelPrerequisites prerequisites() const override;

    void run(TaskContext &ctx) override;

private:
    QString m_segmentationModelId;
    QString m_embeddingModelId;
    double m_threshold;
};

// Records the two diarization model LABELS (resolved from their ids via
// ModelManager) into the frontmatter lines — exposed so tst_task_diarize can
// verify the wiring without the heavy decode+diarize path. Keys:
// "segmentationModel", "embeddingModel".
void applyDiarizationModelFrontmatter(QStringList *frontmatterLines,
                                      const QString &segmentationModelId,
                                      const QString &embeddingModelId);
