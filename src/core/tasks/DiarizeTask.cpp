#include "DiarizeTask.h"

#include <QElapsedTimer>
#include <QFileInfo>

#include "../AudioDecoder.h"
#include "../DiarizationEngine.h"
#include "../MdFrontmatter.h"
#include "../ModelManager.h"
#include "../TranscriptionController.h"
#include "EngineCache.h"

namespace {
// Minimum spacing between progress() emissions during the diarization pass
// — sherpa-onnx fires its callback once per internal ONNX chunk (hundreds to
// thousands of times for a long recording), easily faster than the GUI
// thread can drain a queued progress signal. Same throttle value/reasoning
// as the old TranscriptionWorker's kLiveUpdateThrottleMs.
constexpr qint64 kProgressThrottleMs = 200;
} // namespace

DiarizeTask::DiarizeTask(QUrl file, QUrl vault, QString segmentationModelId, QString embeddingModelId,
                         double threshold, QObject *parent)
    : Task(parent)
    , m_segmentationModelId(std::move(segmentationModelId))
    , m_embeddingModelId(std::move(embeddingModelId))
    , m_threshold(threshold)
{
    fileUrl = std::move(file);
    vaultUrl = std::move(vault);
}

QString DiarizeTask::displayName() const
{
    return QStringLiteral("Detect speakers: %1").arg(QFileInfo(fileUrl.toLocalFile()).fileName());
}

QJsonObject DiarizeTask::toJson() const
{
    return {{QStringLiteral("segmentationModelId"), m_segmentationModelId},
            {QStringLiteral("embeddingModelId"), m_embeddingModelId},
            {QStringLiteral("diarizationThreshold"), m_threshold}};
}

QVariantMap DiarizeTask::toVariantMap() const
{
    // Diarization tuning — lets the Tasks UI / tests see which models + which
    // sensitivity a queued speaker-detection task will use.
    return {{QStringLiteral("segmentationModelId"), m_segmentationModelId},
            {QStringLiteral("embeddingModelId"), m_embeddingModelId},
            {QStringLiteral("diarizationThreshold"), m_threshold}};
}

ModelPrerequisites DiarizeTask::prerequisites() const
{
    return {{ModelDownloadTarget::SegmentationModel, m_segmentationModelId},
            {ModelDownloadTarget::EmbeddingModel, m_embeddingModelId}};
}

void applyDiarizationModelFrontmatter(QStringList *frontmatterLines,
                                      const QString &segmentationModelId,
                                      const QString &embeddingModelId)
{
    MdFrontmatter::upsertKey(frontmatterLines, QStringLiteral("segmentationModel"),
                             ModelManager::segmentationModelInfo(segmentationModelId).label);
    MdFrontmatter::upsertKey(frontmatterLines, QStringLiteral("embeddingModel"),
                             ModelManager::embeddingModelInfo(embeddingModelId).label);
}

void DiarizeTask::run(TaskContext &ctx)
{
    // Needs neither the whisper nor the label model — free them now (GPU
    // hygiene, mirrors RelabelTask's eviction).
    ctx.engines.evictAsr();
    ctx.engines.evictLabel();

    const QString filePath = fileUrl.toLocalFile();
    const QString mdPath = sidecarPath(filePath);
    if (!QFileInfo::exists(mdPath))
        throw TaskError(QStringLiteral("%1 has not been transcribed yet").arg(filePath));

    // Read frontmatter + body + the file's existing "speakers:" allowlist up
    // front (before the slow decode) so a malformed/absent file fails fast
    // — same parse the rename/merge paths use.
    QStringList frontmatterLines;
    QString body;
    int speakersLineIndex = -1;
    QStringList knownSpeakers;
    QStringList distinctTags;
    QString parseError;
    if (!parseSpeakerTaggedFile(mdPath, &frontmatterLines, &body, &speakersLineIndex, &knownSpeakers, &distinctTags,
                                 &parseError))
        throw TaskError(parseError);

    // Decode the sibling audio (same 16k mono pass whisper uses) —
    // pause/cancel honored between chunks.
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

    emit phase(QStringLiteral("detecting-speakers"));
    QString diarError;
    DiarizationEngine *diarizationEngine =
        ctx.engines.ensureDiar(m_segmentationModelId, m_embeddingModelId, static_cast<float>(m_threshold), &diarError);
    // Unlike a transcribe task's inline speakerDetection path (where a
    // diarization failure degrades to ASR-only), detecting speakers IS this
    // task's only purpose — so a failure here is the task's result.
    if (!diarizationEngine)
        throw TaskError(diarError.isEmpty() ? QStringLiteral("speaker detection failed") : diarError);

    QString detectingSubPhase;
    QElapsedTimer progressThrottle;
    progressThrottle.start();
    qint64 lastProgressTickMs = -kProgressThrottleMs;
    const std::vector<DiarizedTurn> turns = diarizationEngine->diarize(
        *pcm, [this, &ctx, &detectingSubPhase, &progressThrottle, &lastProgressTickMs](double fraction) {
            ctx.checkpoint(); // blocks on pause; cancel not actionable mid-pass (see class doc comment)
            const QString subPhase = fraction < 0.30 ? QStringLiteral("detecting-speakers-segmenting")
                                                       : QStringLiteral("detecting-speakers-identifying");
            if (subPhase != detectingSubPhase) {
                detectingSubPhase = subPhase;
                emit phase(subPhase);
            }
            const qint64 nowMs = progressThrottle.elapsed();
            if (fraction >= 1.0 || nowMs - lastProgressTickMs >= kProgressThrottleMs) {
                lastProgressTickMs = nowMs;
                emit progress(fraction);
            }
        });

    // A cancel requested DURING diarization (no mid-pass abort hook) takes
    // effect here, before any file I/O.
    if (ctx.checkpoint())
        throw TaskError(QStringLiteral("Cancelled"));

    const double totalSeconds = static_cast<double>(pcm->size()) / 16000.0;
    QStringList appliedSpeakers;
    const QString newBody = spliceBodySpeakers(body, knownSpeakers, turns, totalSeconds, &appliedSpeakers);

    // Update the frontmatter to reflect that detection now ran: set
    // speakerDetection true, replace/insert the "speakers:" allowlist with
    // exactly the labels that actually landed on a body line, and drop any
    // stale speakerDetectionError from an earlier failed attempt. Every
    // other frontmatter key is left untouched.
    MdFrontmatter::upsertKey(&frontmatterLines, QStringLiteral("speakerDetection"), QStringLiteral("true"));
    MdFrontmatter::upsertKey(&frontmatterLines, QStringLiteral("speakerDetectionError"), QString(),
                             /*removeIfEmpty=*/true);
    // Empty applied set removes the "speakers:" allowlist; otherwise upserts it.
    MdFrontmatter::upsertKey(&frontmatterLines, QStringLiteral("speakers"),
                             appliedSpeakers.join(QStringLiteral(", ")), /*removeIfEmpty=*/true);
    // Record which two models produced these speakers (see the info line in
    // MainPanel.qml). Labels, not ids — consistent with the ASR `model:` key.
    applyDiarizationModelFrontmatter(&frontmatterLines, m_segmentationModelId, m_embeddingModelId);

    QString writeError;
    if (!writeFrontmatterBlock(mdPath, frontmatterLines, newBody, &writeError))
        throw TaskError(writeError);
}
