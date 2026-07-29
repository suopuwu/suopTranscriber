#include "DiarizationSweepTask.h"

#include <QElapsedTimer>
#include <QFileInfo>

#include <algorithm>

#include "../AudioDecoder.h"
#include "../DiarizationEngine.h"
#include "../ModelManager.h"
#include "EngineCache.h"

DiarizationSweepTask::DiarizationSweepTask(QUrl file, QVariantList combos, QObject *parent)
    : Task(parent)
    , m_combos(std::move(combos))
{
    fileUrl = std::move(file);
}

QString DiarizationSweepTask::displayName() const
{
    return QStringLiteral("Diarization sweep: %1").arg(QFileInfo(fileUrl.toLocalFile()).fileName());
}

QVariantMap DiarizationSweepTask::toVariantMap() const
{
    return {};
}

void DiarizationSweepTask::run(TaskContext &ctx)
{
    const QString filePath = fileUrl.toLocalFile();

    // Decode the sibling audio once, honoring pause/cancel between chunks —
    // same DecodeControl pattern DiarizeTask uses.
    emit phase(QStringLiteral("decoding"));
    bool decodePausedSignaled = false;
    AudioDecoder::DecodeControl decodeControl;
    decodeControl.paused = [&ctx, &decodePausedSignaled] {
        const bool p = ctx.pauseRequested();
        if (p != decodePausedSignaled) {
            decodePausedSignaled = p;
            ctx.reportPaused(p);
        }
        return p;
    };
    decodeControl.cancelled = [&ctx] { return ctx.cancelRequested(); };
    QString decodeError;
    auto pcm = AudioDecoder::decodeTo16kMono(filePath, &decodeError, decodeControl);
    if (!pcm)
        throw TaskError(decodeError.isEmpty() ? QStringLiteral("audio decode failed") : decodeError);
    const double audioSeconds = static_cast<double>(pcm->size()) / 16000.0;

    // Never reuses ctx.engines' cached diar engine — each combo below builds
    // its own short-lived local DiarizationEngine instead (same reason
    // BenchmarkTask doesn't go through EngineCache for its ASR models: a
    // sweep's whole point is comparing several fresh models sequentially).
    // The default engineNeeds() (needs nothing) is correct, so TaskRunner's
    // dispatch-time residency policy already freed whatever was resident.
    QVariantList results;
    QElapsedTimer totalTimer;
    totalTimer.start();

    emit phase(QStringLiteral("benchmarking"));
    emit progress(0.0);
    for (int i = 0; i < m_combos.size(); ++i) {
        // Blocks here while paused; breaks out the instant a cancel lands —
        // both take effect at this combo boundary.
        if (ctx.checkpoint())
            break;
        const QVariantMap combo = m_combos.at(i).toMap();
        const QString segId = combo.value("segmentationModelId").toString();
        const QString embedId = combo.value("embeddingModelId").toString();
        const double threshold = combo.value("threshold").toDouble();
        const QString label = combo.value("label").toString();
        emit comboStarted(label, i, m_combos.size());

        QVariantMap result;
        result["label"] = label;
        result["segmentationModelId"] = segId;
        result["embeddingModelId"] = embedId;
        result["threshold"] = threshold;

        QString segPath, embedPath, error;
        if (!ModelManager::ensureDiarizationModels(segId, embedId, &segPath, &embedPath, &error)) {
            result["ok"] = false;
            result["error"] = error.isEmpty() ? QStringLiteral("models unavailable") : error;
            results.append(result);
            emit comboDone(result);
            continue;
        }

        DiarizationEngine engine(segPath, embedPath, static_cast<float>(threshold));
        if (!engine.isValid()) {
            result["ok"] = false;
            result["error"] = QStringLiteral("failed to load models");
            results.append(result);
            emit comboDone(result);
            continue;
        }

        QElapsedTimer comboTimer;
        comboTimer.start();
        // Callback blocks on pause between sherpa-onnx ticks (cancel is not
        // actionable mid-pass — no abort hook — so it lands at the loop
        // boundary above). Same discipline as DiarizeTask's diarize() call.
        const std::vector<DiarizedTurn> turns =
            engine.diarize(*pcm, [&ctx](double) { ctx.checkpoint(); });
        const qint64 runtimeMs = comboTimer.elapsed();
        int maxSpeaker = -1;
        for (const DiarizedTurn &turn : turns)
            maxSpeaker = std::max(maxSpeaker, turn.speaker);
        result["ok"] = true;
        result["speakerCount"] = maxSpeaker + 1;
        result["turnCount"] = static_cast<int>(turns.size());
        result["runtimeMs"] = runtimeMs;
        result["realtimeFactor"] = runtimeMs > 0 ? audioSeconds / (runtimeMs / 1000.0) : 0.0;
        results.append(result);
        emit comboDone(result);
        if (!m_combos.isEmpty())
            emit progress(static_cast<double>(i + 1) / m_combos.size());
    }
    const bool cancelled = ctx.cancelRequested();
    QVariantMap run;
    run["file"] = filePath;
    run["fileName"] = QFileInfo(filePath).fileName();
    run["audioSeconds"] = audioSeconds;
    run["totalRuntimeMs"] = static_cast<qint64>(totalTimer.elapsed());
    run["cancelled"] = cancelled;
    run["kind"] = QStringLiteral("diarization");
    run["results"] = results;
    emit sweepFinished(run);

    // See BenchmarkTask::run()'s doc comment on the same deliberate
    // interface-cleanup decision: a cancel is now uniformly Failed/
    // "Cancelled", matching every other kind in this system.
    if (cancelled)
        throw TaskError(QStringLiteral("Cancelled"));
}
