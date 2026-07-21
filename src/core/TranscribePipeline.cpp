#include "TranscribePipeline.h"

#include <algorithm>
#include <cstddef>

#include "ModelManager.h"
#include "WhisperCppEngine.h"

namespace TranscribePipeline {

Windowing planWindows(bool selfSegmenting, const std::vector<float> &pcm, TranscriptionEngine &engine)
{
    Windowing win;
    if (selfSegmenting) {
        // Out-of-process engines load a heavy model per process and do
        // their own internal segmentation, so feed the whole file as ONE
        // chunk (one spawn) rather than VAD-windowing it into many.
        win.windows.push_back({0.0, static_cast<double>(pcm.size()) / 16000.0});
        return win;
    }

    const QString vadModelPath = ModelManager::ensureVadModel(&win.vadError);
    if (!vadModelPath.isEmpty()) {
        QString chunkingError;
        win.windows = VadChunker::computeWindows(pcm, vadModelPath, &chunkingError, &win.speechSpans);
        if (chunkingError.isEmpty())
            win.vadActive = true;
        else
            win.vadError = chunkingError;
    }
    if (win.windows.empty())
        win.windows.push_back({0.0, static_cast<double>(pcm.size()) / 16000.0});
    // The engine may be a persistent cache entry reused across tasks — a
    // PREVIOUS task's successful VAD path must not silently leak into this
    // one if this task's VAD failed. Guarded to the concrete whisper engine
    // (VAD gating is its feature); a self-segmenting engine never reaches
    // here.
    if (auto *whisper = dynamic_cast<WhisperCppEngine *>(&engine))
        whisper->setVadModelPath(win.vadActive ? vadModelPath : QString());
    return win;
}

std::vector<Segment> runWindows(TranscriptionEngine &engine, const std::vector<float> &pcm,
                                const std::vector<VadChunker::Window> &windows,
                                const ProgressCallback &onProgress, const SegmentCallback &onSegment,
                                const AbortCallback &onAbort)
{
    std::vector<Segment> merged;
    const double totalFileSeconds = static_cast<double>(pcm.size()) / 16000.0;
    double secondsDoneInPriorWindows = 0.0;

    for (const VadChunker::Window &window : windows) {
        // Between-window abort checkpoint — a mid-window abort is still the
        // engine's own onAbort responsibility, unchanged.
        if (onAbort && onAbort())
            break;

        const auto startSample = static_cast<std::size_t>(window.startSeconds * 16000.0);
        const auto endSample = std::min(pcm.size(), static_cast<std::size_t>(window.endSeconds * 16000.0));
        if (endSample <= startSample)
            continue;

        AudioChunk chunk;
        chunk.pcm16kMono.assign(pcm.begin() + static_cast<std::ptrdiff_t>(startSample),
                                pcm.begin() + static_cast<std::ptrdiff_t>(endSample));
        chunk.offsetSeconds = window.startSeconds;
        const double windowSeconds = static_cast<double>(chunk.pcm16kMono.size()) / 16000.0;

        // Blend each window's own done/total into one cumulative done across
        // ALL windows, reported against the whole file — a per-window
        // fraction would snap back to 0 at every window boundary.
        ProgressCallback windowProgress;
        if (onProgress) {
            windowProgress = [&onProgress, secondsDoneInPriorWindows, totalFileSeconds](double done, double) {
                onProgress(secondsDoneInPriorWindows + done, totalFileSeconds);
            };
        }
        SegmentCallback windowSegment = [&onSegment, &chunk](const Segment &rawSegment) {
            // Chunk offset merge is the app's job — a REAL merge for
            // multi-window files, not a no-op. Applied HERE, once, so every
            // Segment the caller's onSegment sees is already absolute
            // (file-relative).
            Segment segment = rawSegment;
            segment.t0 += chunk.offsetSeconds;
            segment.t1 += chunk.offsetSeconds;
            if (onSegment)
                onSegment(segment);
        };

        std::vector<Segment> produced = engine.transcribe(chunk, windowProgress, windowSegment, onAbort);
        // The returned vector gets the same offset merge as the streamed
        // segments (return value and callback are separate channels).
        for (Segment &segment : produced) {
            segment.t0 += chunk.offsetSeconds;
            segment.t1 += chunk.offsetSeconds;
            merged.push_back(std::move(segment));
        }
        secondsDoneInPriorWindows += windowSeconds;
    }
    return merged;
}

} // namespace TranscribePipeline
