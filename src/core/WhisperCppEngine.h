#pragma once

#include <QString>

#include "TranscriptionEngine.h"

struct whisper_context;

// In-process, warm-model engine (see DESIGN.md "whisper.cpp — first-class,
// in-process"). One instance keeps the model resident across transcribe()
// calls. No crash isolation: a CUDA/Vulkan OOM here takes down the app —
// acceptable tradeoff for the trusted default per DESIGN.md.
class WhisperCppEngine : public TranscriptionEngine {
public:
    // enableSpeakerDetection sets whisper.cpp's tdrz_enable — only meaningful
    // when modelPath points at a "-tdrz" fine-tune (see ModelManager); it's
    // a no-op on regular models.
    explicit WhisperCppEngine(const QString &modelPath, bool enableSpeakerDetection = false);
    ~WhisperCppEngine() override;

    bool isValid() const { return m_ctx != nullptr; }

    std::vector<Segment> transcribe(const AudioChunk &chunk, const ProgressCallback &onProgress,
                                     const SegmentCallback &onSegment) override;

private:
    whisper_context *m_ctx = nullptr;
    bool m_speakerDetection = false;
};
