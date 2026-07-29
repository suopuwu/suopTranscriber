#pragma once

#include <QString>

#include <vector>

#include "Segment.h"
#include "TranscriptionEngine.h"
#include "VadChunker.h"

// Shared transcription windowing + streaming, used by BOTH TranscribeTask (the
// real transcriber) and BenchmarkTask so they segment/stream identically (no
// parallel code paths). The app owns the chunk offset merge, not
// the engines (DESIGN.md "Engine architecture").
namespace TranscribePipeline {

struct Windowing {
    std::vector<VadChunker::Window> windows;
    bool vadActive = false;
    QString vadError;
    // Raw VAD speech spans (silence-capture feature, phase 1) — populated
    // only when vadActive is true (see VadChunker::computeWindows()'s
    // speechSpansOut doc comment); empty and meaningless otherwise. The
    // caller (TranscribeTask) derives silence/noise spans from this via
    // VadChunker::complementSpans().
    std::vector<std::pair<double, double>> speechSpans;
};

// Decide how to window `pcm` for one engine, exactly as a real transcription
// does. selfSegmenting == true (out-of-process/subprocess engines that do their
// own internal segmentation) ⇒ ONE whole-file window. Otherwise VAD-window via
// ModelManager::ensureVadModel + VadChunker::computeWindows, degrading to a
// single whole-file window when the VAD model is missing/failed (vadError set
// honestly — never a silent degrade). Also sets the VAD model path on a WhisperCppEngine
// so its own per-window gating matches (a no-op for other engine types).
Windowing planWindows(bool selfSegmenting, const std::vector<float> &pcm,
                      TranscriptionEngine &engine);

// Run `engine` over each window, applying the app-owned offset merge so every
// Segment passed to onSegment carries ABSOLUTE (file-relative) t0/t1. onProgress
// reports cumulative seconds across all windows against the whole file. Returns
// all merged segments. Honors onAbort between windows (mid-window abort is the
// engine's own onAbort responsibility) and stops early when it fires.
// failureCount (optional out-param): set to the sum of
// engine.takeInferenceFailureCount() across all windows — a non-zero value
// means the returned segments are incomplete (see TranscriptionEngine's doc
// comment), not that the file genuinely had less speech.
std::vector<Segment> runWindows(TranscriptionEngine &engine, const std::vector<float> &pcm,
                                const std::vector<VadChunker::Window> &windows,
                                const ProgressCallback &onProgress, const SegmentCallback &onSegment,
                                const AbortCallback &onAbort, int *failureCount = nullptr);

} // namespace TranscribePipeline
