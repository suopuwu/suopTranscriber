#include "EngineCache.h"

#include <QFileInfo>

#include "../DiarizationEngine.h"
#include "../LabelingEngine.h"
#include "../ModelManager.h"
#include "../ParakeetEngine.h"
#include "../SubprocessEngine.h"
#include "../TranscriptionController.h" // isEngineModelId/engineIdFromModelId/resolveEngine, EngineDescriptor
#include "../WhisperCppEngine.h"

// Out-of-line so LabelingEngine/DiarizationEngine only need to be complete
// here, not wherever EngineCache.h is included (pimpl-style forward decls).
EngineCache::EngineCache() = default;
EngineCache::~EngineCache() = default;

EngineCache::AsrHandle EngineCache::ensureAsr(const QString &modelId, const QString &vaultPath)
{
    AsrHandle handle;

    // A model id with the "engine:" prefix routes to an out-of-process
    // SubprocessEngine; Parakeet is in-process (sherpa-onnx, like whisper)
    // but a different engine class + a multi-file ONNX bundle; anything else
    // is a whisper ggml tier resolved by ModelManager.
    const bool useSubprocessEngine = isEngineModelId(modelId);
    const bool useParakeet = ModelManager::isParakeetModelId(modelId);
    QString error;
    bool modelFallback = false;
    QString modelPath; // ggml file path (whisper) OR the "engine:" id (subprocess) — also the reuse key
    EngineDescriptor engineDesc;

    if (useSubprocessEngine) {
        const auto desc = resolveEngine(engineIdFromModelId(modelId), vaultPath, &error);
        if (!desc || !desc->available) {
            handle.error = error.isEmpty() ? QStringLiteral("engine unavailable") : error;
            return handle;
        }
        engineDesc = *desc;
        modelPath = modelId;
        handle.modelUsedName = desc->manifest.name;
    } else {
        modelPath = ModelManager::ensureModel(modelId, &error, &modelFallback);
        if (modelPath.isEmpty()) {
            handle.error = error.isEmpty() ? QStringLiteral("model unavailable") : error;
            return handle;
        }
        handle.modelUsedName = QFileInfo(modelPath).fileName();
    }
    handle.fellBack = modelFallback;

    // Item 50 (why a stale/failed load isn't retried mid-job): whisper's
    // model load is a single blocking call with no abort hook — see
    // TranscribeTask::run()'s "loading" phase doc comment for the full
    // rationale. Rebuild only when the resolved model differs from what's
    // already resident.
    if (!m_asr || !m_asr->isValid() || m_asrModelPath != modelPath) {
        if (useSubprocessEngine)
            m_asr = std::make_unique<SubprocessEngine>(engineDesc.manifest, engineDesc.dir);
        else if (useParakeet) {
            const ModelManager::ParakeetModelPaths p = ModelManager::parakeetModelPaths();
            m_asr = std::make_unique<ParakeetEngine>(p.encoder, p.decoder, p.joiner, p.tokens);
        } else
            m_asr = std::make_unique<WhisperCppEngine>(modelPath);
        m_asrModelPath = modelPath;
    }
    if (!m_asr->isValid()) {
        handle.error = QStringLiteral("failed to load model at %1").arg(modelPath);
        m_asr.reset();
        m_asrModelPath.clear();
        return handle;
    }

    handle.engine = m_asr.get();
    handle.backendName = m_asr->activeBackendName();
    return handle;
}

LabelingEngine *EngineCache::ensureLabel(QString *error)
{
    QString labelError;
    const QString labelModelPath = ModelManager::ensureLabelModel(&labelError);
    if (labelModelPath.isEmpty()) {
        if (error)
            *error = QStringLiteral("auto-label model unavailable: %1").arg(labelError);
        return nullptr;
    }
    if (!m_label || !m_label->isValid() || m_labelModelPath != labelModelPath) {
        m_label = std::make_unique<LabelingEngine>(labelModelPath);
        m_labelModelPath = labelModelPath;
    }
    if (!m_label->isValid()) {
        if (error)
            *error = QStringLiteral("failed to load labeling model at %1").arg(labelModelPath);
        return nullptr;
    }
    return m_label.get();
}

DiarizationEngine *EngineCache::ensureDiar(const QString &segmentationModelId, const QString &embeddingModelId,
                                            float threshold, QString *error)
{
    QString segPath, embedPath, diarError;
    if (!ModelManager::ensureDiarizationModels(segmentationModelId, embeddingModelId, &segPath, &embedPath,
                                                &diarError)) {
        if (error)
            *error = diarError.isEmpty() ? QStringLiteral("speaker diarization models unavailable") : diarError;
        return nullptr;
    }
    if (!m_diar || !m_diar->isValid() || m_diarSegPath != segPath || m_diarEmbedPath != embedPath
        || m_diarThreshold != threshold) {
        m_diar = std::make_unique<DiarizationEngine>(segPath, embedPath, threshold);
        m_diarSegPath = segPath;
        m_diarEmbedPath = embedPath;
        m_diarThreshold = threshold;
    }
    if (!m_diar->isValid()) {
        if (error)
            *error = QStringLiteral("failed to load speaker diarization models");
        m_diar.reset();
        return nullptr;
    }
    return m_diar.get();
}

void EngineCache::evictAsr()
{
    m_asr.reset();
    m_asrModelPath.clear();
}

void EngineCache::evictLabel()
{
    m_label.reset();
    m_labelModelPath.clear();
}

void EngineCache::evictDiar()
{
    m_diar.reset();
    m_diarSegPath.clear();
    m_diarEmbedPath.clear();
}

void EngineCache::evictAll()
{
    m_asr.reset();
    m_asrModelPath.clear();
    m_label.reset();
    m_labelModelPath.clear();
    m_diar.reset();
    m_diarSegPath.clear();
    m_diarEmbedPath.clear();
}
