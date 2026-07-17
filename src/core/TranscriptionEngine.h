#pragma once

#include <QString>

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
// Called as each segment is produced, so the UI can stream results in (see
// DESIGN.md "Native callbacks"). Segment.t0/t1 are chunk-relative; the app
// applies the offset merge, not the engine.
using SegmentCallback = std::function<void(const Segment &segment)>;
// Polled periodically from inside the engine's decode loop (Wave N task
// system: pause/cancel of an in-progress transcription). Returning true
// means "stop now" — the engine bails out with whatever segments were
// produced so far, same as if the audio simply ended there. A pause is
// implemented by BLOCKING inside this callback (busy-checking the same
// condition) rather than as a separate concept, since every engine that can
// report progress already calls back into app code often enough to also
// host a pause wait there. Empty (default) means "never abort" — engines
// that ignore this entirely are still contract-valid, just not
// interruptible.
using AbortCallback = std::function<bool()>;

// Abstraction lives at the Segment level, not the transport (see DESIGN.md
// "Engine architecture"). WhisperCppEngine implements this in-process;
// SubprocessEngine implements it by driving an out-of-process JSONL engine.
class TranscriptionEngine {
public:
    virtual ~TranscriptionEngine() = default;
    virtual std::vector<Segment> transcribe(const AudioChunk &chunk, const ProgressCallback &onProgress,
                                             const SegmentCallback &onSegment,
                                             const AbortCallback &onAbort = {}) = 0;

    // Whether this engine loaded successfully and can transcribe. In-process
    // engines that load a model up front (WhisperCppEngine) override this to
    // report a failed load; out-of-process engines validate lazily at spawn
    // time and are always "valid" here (a spawn failure degrades per-call), so
    // the default is true.
    virtual bool isValid() const { return true; }

    // Item O35: which compute backend this engine's last model load actually
    // picked (e.g. "CUDA", "Vulkan", "CPU") — empty until a model has been
    // loaded, and empty forever for an engine with no such concept (e.g. an
    // out-of-process SubprocessEngine driving a JSONL manifest). Only
    // WhisperCppEngine overrides this today.
    virtual QString activeBackendName() const { return QString(); }
};
