#pragma once

#include <QString>

#include <memory>

#include "../TranscriptionEngine.h"

class LabelingEngine;
class DiarizationEngine;

// The worker thread's warm-engine cache. Lives on (and is only touched from)
// the worker thread, owned by TaskRunner and handed to each Task via
// TaskContext. This is where model reuse lives: a whisper/parakeet model that
// a transcribe task loaded stays resident so the NEXT transcribe task reuses
// it instead of paying the multi-second reload — the whole reason the old
// design ran every kind through one long-lived worker object. Moving it here
// (rather than owning an engine per Task) preserves that reuse while letting
// each kind's doing-code live in its own file.
//
// GPU memory hygiene: at most one big model (ASR or labeling) is kept
// resident at a time, plus diarization. Which engines survive a given task's
// dispatch is decided by ONE rule — applyResidencyPolicy(), called by
// TaskRunner::execute() with the dispatching Task's own Task::EngineNeeds —
// rather than each kind hand-clearing its siblings (that hand-
// written approach is exactly how a diarization arena survived into an
// unrelated transcribe and pressured the GPU into an uncatchable ggml
// abort). evictAll() drops everything once the queue drains (see
// TaskRunner's idle eviction, formerly evictIdle()).
//
// A DiarizationEngine is additionally built fresh for every diarize task
// even though diarize DECLARES needing it (so the policy leaves it alone):
// DiarizeTask::run() calls evictDiar() itself before ensureDiar(), so the
// reuse guard in ensureDiar() never actually reuses one across tasks —
// because letting the onnxruntime CUDA arena carry over between files let it
// grow unbounded across a batch (measured ~29 GB on one real recording; see
// patches/sherpa-onnx-batch-segmentation.patch).
class EngineCache {
public:
    EngineCache();
    ~EngineCache();

    struct AsrHandle {
        // Borrowed — owned by the cache, valid until the next ensureAsr()/
        // evict on this thread. Null iff error is non-empty.
        TranscriptionEngine *engine = nullptr;
        // Frontmatter "model" field: the ggml file name, or the engine's
        // display name for a subprocess engine.
        QString modelUsedName;
        // ModelManager substituted its default because the requested tier was
        // missing (whisper only) — the task surfaces this to the user.
        bool fellBack = false;
        // Non-empty means resolution/load failed; engine is null.
        QString error;
        // Which compute backend the load actually picked ("CUDA"/"Vulkan"/
        // "CPU"), empty for engines with no such concept.
        QString backendName;
    };

    // Resolve modelId (an "engine:" subprocess id, a parakeet bundle id, or a
    // whisper ggml tier) to a warm engine, rebuilding only when the resolved
    // model differs from what is already resident. vaultPath scopes engine
    // discovery for subprocess engines. Faithful port of the old worker
    // run()'s engine-resolution block — the caller still emits phase()/
    // backendNameChanged and does the loading-phase cancel check.
    AsrHandle ensureAsr(const QString &modelId, const QString &vaultPath);

    // Warm labeling engine (LLM) for relabel/chapters tasks; rebuilt only when
    // the resolved label model path changes. Null return + non-empty *error on
    // failure.
    LabelingEngine *ensureLabel(QString *error);

    // Resolve+load a DiarizationEngine, rebuilding when the resolved model
    // paths OR threshold change (threshold is baked into the sherpa config
    // at construction) — in practice DiarizeTask::run() calls evictDiar()
    // right before this, so every diarize task gets a fresh engine
    // regardless (see class doc comment for why). The caller drives
    // engine->diarize(pcm, onProgress) itself — the progress callback needs
    // to emit the Task's own phase()/progress() signals and call
    // TaskContext::checkpoint(), neither of which EngineCache has access to.
    DiarizationEngine *ensureDiar(const QString &segmentationModelId, const QString &embeddingModelId,
                                   float threshold, QString *error);

    // Drop the ASR engine specifically. Called by applyResidencyPolicy() for
    // any dispatching task that doesn't declare needing it.
    void evictAsr();
    // Drop the labeling engine specifically. Called by applyResidencyPolicy()
    // for any dispatching task that doesn't declare needing it.
    void evictLabel();
    // Drop the diarization engine specifically. Called by
    // applyResidencyPolicy() for any dispatching task that doesn't declare
    // needing it, AND by DiarizeTask::run() unconditionally on every run even
    // though diarize DOES declare needing it (see class doc comment for why
    // that one is force-fresh regardless of the policy).
    void evictDiar();
    // Release every resident engine — called when the queue drains so a warm
    // model doesn't sit on VRAM indefinitely (old evictIdle()).
    void evictAll();

    // The single residency rule: evict whichever of the three
    // engines the dispatching task did NOT declare needing (Task::
    // EngineNeeds), called by TaskRunner::execute() before run(). A needed
    // engine is left untouched here — its own ensureXxx() call inside run()
    // still decides reuse-vs-rebuild via its usual reuse key — which is what
    // lets a run of consecutive same-need tasks (e.g. a transcribe batch)
    // reuse one warm engine across files instead of paying a reload each time.
    void applyResidencyPolicy(bool needAsr, bool needLabel, bool needDiar);

    // Eviction counters — exposed purely so tests can assert eviction
    // happened (diarEvictionCount) or didn't (the residency
    // policy) without needing a real downloaded model to observe engine
    // IDENTITY directly. asrEvictionCount()/labelEvictionCount() mirror the
    // original diarEvictionCount().
    int asrEvictionCount() const { return m_asrEvictionCount; }
    int labelEvictionCount() const { return m_labelEvictionCount; }
    int diarEvictionCount() const { return m_diarEvictionCount; }

private:
    std::unique_ptr<TranscriptionEngine> m_asr;
    QString m_asrModelPath; // reuse key
    std::unique_ptr<LabelingEngine> m_label;
    QString m_labelModelPath; // reuse key
    std::unique_ptr<DiarizationEngine> m_diar;
    QString m_diarSegPath;
    QString m_diarEmbedPath;
    float m_diarThreshold = 0.5f; // part of the reuse key alongside the two paths
    int m_asrEvictionCount = 0;
    int m_labelEvictionCount = 0;
    int m_diarEvictionCount = 0;
};
