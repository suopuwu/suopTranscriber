#pragma once

#include <QString>

#include "TranscriptionEngine.h"

struct whisper_context;
struct whisper_context_params;
struct whisper_full_params;

// In-process, warm-model engine (see DESIGN.md "whisper.cpp — first-class,
// in-process"). One instance keeps the model resident across transcribe()
// calls. No crash isolation: a CUDA/Vulkan OOM here takes down the app —
// acceptable tradeoff for the trusted default per DESIGN.md. Speaker
// diarization is a separate pass over the same PCM (see DiarizationEngine),
// not a whisper.cpp feature — this engine has no speaker-detection concept
// of its own.
class WhisperCppEngine : public TranscriptionEngine {
public:
    explicit WhisperCppEngine(const QString &modelPath);
    ~WhisperCppEngine() override;

    bool isValid() const override { return m_ctx != nullptr; }

    std::vector<Segment> transcribe(const AudioChunk &chunk, const ProgressCallback &onProgress,
                                     const SegmentCallback &onSegment,
                                     const AbortCallback &onAbort = {}) override;

    // The decode parameters transcribe() starts from — beam search plus the
    // anti-hallucination tightenings documented at the definition. Split out
    // (and public, by value) purely so tst_engine_contract can assert them
    // without loading a model; transcribe() then layers per-call state
    // (callbacks, VAD) on top of this.
    static whisper_full_params decodeParams();

    // The context parameters the constructor builds the model with —
    // whisper defaults plus flash attention (a faster encoder-attention
    // kernel; a free speedup, most on the CUDA/Blackwell path, no quality
    // change). Split out static + public, like decodeParams() above, purely
    // so tst_engine_contract can assert flash_attn is on without loading a
    // model.
    static whisper_context_params contextParams();

    // "CUDA"/"Vulkan"/"CPU", parsed out of whisper.cpp's own
    // "using %s backend" log line (see the .cpp's whisper_log_set()
    // capture) — empty until the first model load. Backed by process-wide
    // storage rather than a plain instance member: whisper_log_set()
    // registers a GLOBAL ggml log callback (ggml_log_set), shared with
    // llama.cpp's LabelingEngine (both link the same vendored ggml target —
    // see CMakeLists.txt), so binding the callback to THIS instance's own
    // member address would leave a dangling callback pointer the moment
    // this engine is destroyed while that global registration is still
    // live — this app resets/recreates its engines constantly (see
    // TranscriptionWorker's GPU-memory-hygiene evictions), and any later
    // ggml/llama.cpp log call would then write through freed memory.
    QString activeBackendName() const override;

    // Gates whisper.cpp's own internal Silero VAD (whisper_full_params.vad —
    // see whisper.h) inside transcribe(), which skips non-speech regions of
    // the given chunk and reduces the silence-driven hallucination whisper.cpp
    // is otherwise prone to (see DESIGN.md's audio-preprocessing notes).
    // Complements (does not replace) VadChunker's app-level windowing: that
    // pass only trims silence AT window boundaries, so a window can still
    // contain an internal pause between two grouped VAD segments — this
    // mops up exactly that residual case. Empty (default) means "no VAD
    // gating", today's behavior. Reloaded from whisper_vad_model_path fresh
    // on every transcribe() call internally (see whisper.cpp's
    // whisper_full()) — acceptable, the model is ~1MB.
    void setVadModelPath(const QString &path) { m_vadModelPath = path; }

private:
    whisper_context *m_ctx = nullptr;
    QString m_vadModelPath;
};
