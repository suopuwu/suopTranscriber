#include "DownloadTask.h"

#include "../ModelManager.h"

DownloadTask::DownloadTask(ModelDownloadTarget target, QString modelId, QObject *parent)
    : Task(parent)
    , m_target(target)
    , m_modelId(std::move(modelId))
{
}

QString DownloadTask::targetToString(ModelDownloadTarget target)
{
    switch (target) {
    case ModelDownloadTarget::WhisperModel: return QStringLiteral("model");
    case ModelDownloadTarget::LabelModel: return QStringLiteral("label");
    case ModelDownloadTarget::VadModel: return QStringLiteral("vad");
    case ModelDownloadTarget::SegmentationModel: return QStringLiteral("segmentation");
    case ModelDownloadTarget::EmbeddingModel: return QStringLiteral("embedding");
    }
    return QString();
}

bool DownloadTask::targetFromString(const QString &raw, ModelDownloadTarget *out)
{
    if (raw == QStringLiteral("model"))
        *out = ModelDownloadTarget::WhisperModel;
    else if (raw == QStringLiteral("label"))
        *out = ModelDownloadTarget::LabelModel;
    else if (raw == QStringLiteral("vad"))
        *out = ModelDownloadTarget::VadModel;
    else if (raw == QStringLiteral("segmentation"))
        *out = ModelDownloadTarget::SegmentationModel;
    else if (raw == QStringLiteral("embedding"))
        *out = ModelDownloadTarget::EmbeddingModel;
    else
        return false;
    return true;
}

QString DownloadTask::displayName() const
{
    switch (m_target) {
    case ModelDownloadTarget::WhisperModel:
        return m_modelId.isEmpty() ? QStringLiteral("Download model") : m_modelId;
    case ModelDownloadTarget::LabelModel: return QStringLiteral("Download labeling model");
    case ModelDownloadTarget::VadModel: return QStringLiteral("Download VAD model");
    case ModelDownloadTarget::SegmentationModel: return QStringLiteral("Download segmentation model");
    case ModelDownloadTarget::EmbeddingModel: return QStringLiteral("Download embedding model");
    }
    return QStringLiteral("Download");
}

QJsonObject DownloadTask::toJson() const
{
    return {{QStringLiteral("downloadTarget"), targetToString(m_target)}, {QStringLiteral("modelId"), m_modelId}};
}

QVariantMap DownloadTask::toVariantMap() const
{
    return {{QStringLiteral("downloadTarget"), targetToString(m_target)}, {QStringLiteral("modelId"), m_modelId}};
}

void DownloadTask::run(TaskContext &ctx)
{
    emit phase(QStringLiteral("downloading"));

    const ModelManager::DownloadProgressCallback onProgress = [this](qint64 done, qint64 total) {
        emit progress(total > 0 ? static_cast<double>(done) / static_cast<double>(total) : 0.0);
    };
    // ModelManager polls these itself on every chunk and runs its own
    // pause-wait loop — this task never blocks, so it reports the paused
    // edge via ctx.reportPaused() rather than ctx.checkpoint().
    ModelManager::DownloadControl control;
    control.paused = [&ctx] {
        const bool p = ctx.pauseRequested();
        ctx.reportPaused(p);
        return p;
    };
    control.cancelled = [&ctx] { return ctx.cancelRequested(); };

    QString error;
    bool ok = false;
    switch (m_target) {
    case ModelDownloadTarget::WhisperModel:
        ok = !ModelManager::ensureModel(m_modelId, &error, nullptr, onProgress, control).isEmpty();
        break;
    case ModelDownloadTarget::LabelModel:
        ok = !ModelManager::ensureLabelModel(&error, onProgress, control).isEmpty();
        break;
    case ModelDownloadTarget::VadModel:
        ok = !ModelManager::ensureVadModel(&error, onProgress, control).isEmpty();
        break;
    case ModelDownloadTarget::SegmentationModel:
        ok = !ModelManager::ensureSegmentationModel(m_modelId, &error, nullptr, onProgress, control).isEmpty();
        break;
    case ModelDownloadTarget::EmbeddingModel:
        ok = !ModelManager::ensureEmbeddingModel(m_modelId, &error, nullptr, onProgress, control).isEmpty();
        break;
    }
    if (!ok)
        throw TaskError(error.isEmpty() ? QStringLiteral("download failed") : error);
}
