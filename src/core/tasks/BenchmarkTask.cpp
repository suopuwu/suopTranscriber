#include "BenchmarkTask.h"

#include <QElapsedTimer>
#include <QFileInfo>
#include <QRegularExpression>

#include <memory>

#include "../AudioDecoder.h"
#include "../ModelManager.h"
#include "../ParakeetEngine.h"
#include "../Segment.h"
#include "../SubprocessEngine.h"
#include "../TranscriptionController.h"
#include "../TranscriptionEngine.h"
#include "../WhisperCppEngine.h"

namespace {

// Builds and runs a SHORT-LIVED engine for one model over the already-
// decoded chunk — deliberately not EngineCache-backed (see BenchmarkTask.h's
// class doc comment). onAbort is checked deep inside the engine's own
// decode loop, same mechanism a real transcribe uses.
QVariantMap runOneBenchmarkModel(const QString &modelId, const QString &vaultPath, const AudioChunk &chunk,
                                  double audioSeconds, const AbortCallback &onAbort)
{
    QVariantMap row;
    row["modelId"] = modelId;

    std::unique_ptr<TranscriptionEngine> engine;
    if (isEngineModelId(modelId)) {
        QString error;
        const auto desc = resolveEngine(engineIdFromModelId(modelId), vaultPath, &error);
        row["kind"] = QStringLiteral("engine");
        row["label"] = desc && desc->manifestValid ? desc->manifest.name : engineIdFromModelId(modelId);
        if (!desc || !desc->available) {
            row["ok"] = false;
            row["error"] = error.isEmpty() ? QStringLiteral("engine unavailable") : error;
            return row;
        }
        engine = std::make_unique<SubprocessEngine>(desc->manifest, desc->dir);
    } else if (ModelManager::isParakeetModelId(modelId)) {
        row["kind"] = QStringLiteral("parakeet");
        row["label"] = modelId;
        const ModelManager::ParakeetModelPaths p = ModelManager::parakeetModelPaths();
        if (!p.isComplete()) {
            row["ok"] = false;
            row["error"] = QStringLiteral("model not downloaded");
            return row;
        }
        auto parakeet = std::make_unique<ParakeetEngine>(p.encoder, p.decoder, p.joiner, p.tokens);
        if (!parakeet->isValid()) {
            row["ok"] = false;
            row["error"] = QStringLiteral("failed to load Parakeet model");
            return row;
        }
        engine = std::move(parakeet);
    } else {
        row["kind"] = QStringLiteral("whisper");
        row["label"] = modelId;
        QString error;
        // Only downloaded tiers are passed in, so ensureModel() resolves the
        // existing file without a network fetch.
        const QString modelPath = ModelManager::ensureModel(modelId, &error);
        if (modelPath.isEmpty()) {
            row["ok"] = false;
            row["error"] = error.isEmpty() ? QStringLiteral("model unavailable") : error;
            return row;
        }
        auto whisper = std::make_unique<WhisperCppEngine>(modelPath);
        if (!whisper->isValid()) {
            row["ok"] = false;
            row["error"] = QStringLiteral("failed to load model");
            return row;
        }
        engine = std::move(whisper);
    }

    QElapsedTimer timer;
    timer.start();
    const std::vector<Segment> segments = engine->transcribe(chunk, {}, {}, onAbort);
    const qint64 runtimeMs = timer.elapsed();

    QString text;
    int wordCount = 0;
    for (const Segment &seg : segments) {
        const QString segText = QString::fromStdString(seg.text).trimmed();
        if (segText.isEmpty())
            continue;
        if (!text.isEmpty())
            text += QLatin1Char(' ');
        text += segText;
        wordCount += segText.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts).size();
    }

    row["ok"] = true;
    row["runtimeMs"] = runtimeMs;
    row["segmentCount"] = static_cast<int>(segments.size());
    row["wordCount"] = wordCount;
    row["charCount"] = text.size();
    // Realtime factor: seconds of audio processed per second of wall clock
    // (>1 = faster than realtime).
    row["realtimeFactor"] = runtimeMs > 0 ? audioSeconds / (runtimeMs / 1000.0) : 0.0;
    row["text"] = text;
    return row;
}

} // namespace

BenchmarkTask::BenchmarkTask(QUrl file, QUrl vault, QStringList modelIds, QObject *parent)
    : Task(parent)
    , m_modelIds(std::move(modelIds))
{
    fileUrl = std::move(file);
    vaultUrl = std::move(vault);
}

QString BenchmarkTask::displayName() const
{
    return QStringLiteral("Benchmark: %1").arg(QFileInfo(fileUrl.toLocalFile()).fileName());
}

QVariantMap BenchmarkTask::toVariantMap() const
{
    return {};
}

void BenchmarkTask::run(TaskContext &ctx)
{
    const QString filePath = fileUrl.toLocalFile();
    const QString vaultPath = vaultUrl.toLocalFile();

    QString decodeError;
    auto pcm = AudioDecoder::decodeTo16kMono(filePath, &decodeError);
    if (!pcm)
        throw TaskError(decodeError.isEmpty() ? QStringLiteral("audio decode failed") : decodeError);
    const double audioSeconds = static_cast<double>(pcm->size()) / 16000.0;

    AudioChunk chunk;
    chunk.pcm16kMono = *pcm;
    chunk.offsetSeconds = 0.0; // whole file, single chunk — no offset merge needed for display

    const AbortCallback onAbort = [&ctx] { return ctx.cancelRequested(); };
    QVariantList results;
    QElapsedTimer totalTimer;
    totalTimer.start();

    // Drive the bottom-bar glow: a benchmark is a real Active task now, so it
    // feeds the same phase/progress plumbing every other kind does (fraction
    // of models completed). "benchmarking" is one of the phases the controller
    // assigns straight to overallProgress (no rolling-average blend).
    emit phase(QStringLiteral("benchmarking"));
    emit progress(0.0);
    for (int i = 0; i < m_modelIds.size(); ++i) {
        if (ctx.cancelRequested())
            break;
        const QString modelId = m_modelIds.at(i);
        emit benchmarkModelStarted(modelId, i, m_modelIds.size());

        QVariantMap result = runOneBenchmarkModel(modelId, vaultPath, chunk, audioSeconds, onAbort);
        results.append(result);
        emit benchmarkModelDone(result);
        if (!m_modelIds.isEmpty())
            emit progress(static_cast<double>(i + 1) / m_modelIds.size());
    }

    const bool cancelled = ctx.cancelRequested();
    QVariantMap run;
    run["file"] = filePath;
    run["fileName"] = QFileInfo(filePath).fileName();
    run["audioSeconds"] = audioSeconds;
    run["totalRuntimeMs"] = static_cast<qint64>(totalTimer.elapsed());
    run["cancelled"] = cancelled;
    run["results"] = results;
    // Emitted regardless of cancellation — the partial run summary (whatever
    // models finished before the cancel) is itself the useful artifact, same
    // as the old side-door's runBenchmark()/benchmarkFinished() contract.
    emit benchmarkRunFinished(run);

    // Interface cleanup (deliberate behavior change from the old side-door,
    // which returned successfully either way): every OTHER kind in this
    // system reports a cancel as Failed/"Cancelled" — still finished, per
    // sortJobsForDisplay()'s doc comment — and now that benchmark is a real
    // queued Task instead of bypassing the queue, it follows that same
    // uniform convention rather than perpetuating its old inconsistency.
    if (cancelled)
        throw TaskError(QStringLiteral("Cancelled"));
}
