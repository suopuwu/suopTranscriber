#pragma once

#include <QString>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>

#include "Task.h"

// Diarization sweep (the Benchmark page's "Diarization sweep"): runs a
// cartesian product of chosen segmentation × embedding × sensitivity combos
// over ONE decode of the file, reporting how many speakers each combo finds
// — the money metric for "which combo actually splits a hard far-field
// recording". Writes nothing to the vault (same as BenchmarkTask). Each
// combo builds its OWN short-lived DiarizationEngine (threshold is baked in
// at construction, and a big embedding model shouldn't hold VRAM across
// combos) rather than going through EngineCache's diarize slot.
class DiarizationSweepTask : public Task {
    Q_OBJECT
public:
    // Each entry in combos: {segmentationModelId, embeddingModelId,
    // threshold, label} — mirrors the old QVariantList combo shape the
    // Benchmark page already builds.
    DiarizationSweepTask(QUrl fileUrl, QVariantList combos, QObject *parent = nullptr);

    QString kind() const override { return QStringLiteral("diarizationSweep"); }
    QString displayName() const override;
    Presentation presentation() const override { return Presentation::Background; }
    // Cancellable between combos only — diarize() has no mid-pass abort hook
    // (see DiarizeTask's capabilities() doc comment for the same limitation).
    Capabilities capabilities() const override { return {/*pausable=*/false, /*cancellableMidRun=*/false}; }
    QVariantMap toVariantMap() const override;

    void run(TaskContext &ctx) override;

signals:
    void comboStarted(const QString &label, int index, int total);
    void comboDone(const QVariantMap &result);
    void sweepFinished(const QVariantMap &run);

private:
    QVariantList m_combos;
};
