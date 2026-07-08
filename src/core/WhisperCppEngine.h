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
                                     const SegmentCallback &onSegment) override;

private:
    whisper_context *m_ctx = nullptr;
};
