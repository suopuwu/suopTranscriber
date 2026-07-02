#pragma once

#include <functional>
#include <vector>

#include "Segment.h"

// 16kHz mono PCM, decoded/resampled by ffmpeg upstream of any engine.
// offsetSeconds is this chunk's position in the source file; engines report
// timestamps relative to the chunk — the app applies the offset merge.
struct AudioChunk {
    std::vector<float> pcm16kMono;
    double offsetSeconds = 0.0;
};

using ProgressCallback = std::function<void(double secondsDone, double secondsTotal)>;

// Abstraction lives at the Segment level, not the transport (see DESIGN.md
// "Engine architecture"). WhisperCppEngine implements this in-process;
// SubprocessEngine implements it by driving an out-of-process JSONL engine.
class TranscriptionEngine {
public:
    virtual ~TranscriptionEngine() = default;
    virtual std::vector<Segment> transcribe(const AudioChunk &chunk, const ProgressCallback &onProgress) = 0;
};
