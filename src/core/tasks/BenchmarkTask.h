#pragma once

#include <QString>
#include <QStringList>
#include <QUrl>
#include <QVariantMap>

#include "Task.h"

// Model benchmark (the Benchmark page's "Model benchmark" sweep): runs each
// requested model/engine over ONE audio file, in memory, purely for
// comparison — writes NO .md, touches no vault state, decodes the audio
// ONCE and reuses it across models. A normal queued Task now (not the old
// side-door runBenchmark() that bypassed the queue entirely to avoid the
// main-panel overlay) — Presentation::Background gets it the Tasks row +
// glow WITHOUT the overlay, which is the whole reason Presentation exists
// as an axis separate from "is this queued at all". Deliberately does NOT
// go through EngineCache's ASR slot: cycling through many models would
// otherwise thrash the one resident transcribe engine, so each model here
// builds and discards its own short-lived engine instance.
class BenchmarkTask : public Task {
    Q_OBJECT
public:
    BenchmarkTask(QUrl fileUrl, QUrl vaultUrl, QStringList modelIds, QObject *parent = nullptr);

    QString kind() const override { return QStringLiteral("benchmark"); }
    QString kindLabel() const override { return QStringLiteral("Benchmark"); }
    QString displayName() const override;
    Presentation presentation() const override { return Presentation::Background; }
    // A comparison-only run — writes no vault file, so it must not gate per-file
    // UI on the benchmarked file (see Task::occupiesFile()).
    bool occupiesFile() const override { return false; }
    // Cancellable (between models, and within one via the same abort-
    // callback mechanism a real transcribe uses) but never pausable — the
    // old runBenchmark()/cancelBenchmark() never offered a pause control.
    Capabilities capabilities() const override { return {/*pausable=*/false, /*cancellableMidRun=*/true}; }
    // Not persisted — an interactive comparison run, not durable work (the
    // old side-door never went through the queue/persistQueue() at all).
    QVariantMap toVariantMap() const override;

    void run(TaskContext &ctx) override;

    const QStringList &modelIds() const { return m_modelIds; }

signals:
    void benchmarkModelStarted(const QString &modelId, int index, int total);
    void benchmarkModelDone(const QVariantMap &result);
    // Live partial transcript for the model at index `modelIndex` in this run,
    // emitted (throttled) as segments stream in so the Benchmark page can grow
    // the running card sentence-by-sentence before benchmarkModelDone.
    void benchmarkModelProgressText(int modelIndex, const QString &partialText);
    // The whole run's summary, once every model has been attempted (or the
    // sweep was cancelled partway through) — what the Benchmark page
    // persists via VaultTreeModel::writeBenchmarkLastRun.
    void benchmarkRunFinished(const QVariantMap &run);

private:
    QStringList m_modelIds;
};
