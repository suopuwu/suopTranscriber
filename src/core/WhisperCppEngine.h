#pragma once

#include <QString>

#include "TranscriptionEngine.h"

struct whisper_context;

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

    bool isValid() const { return m_ctx != nullptr; }

    std::vector<Segment> transcribe(const AudioChunk &chunk, const ProgressCallback &onProgress,
                                     const SegmentCallback &onSegment,
                                     const AbortCallback &onAbort = {}) override;

    // Item O35: "CUDA"/"Vulkan"/"CPU", parsed out of whisper.cpp's own
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

private:
    whisper_context *m_ctx = nullptr;
};
