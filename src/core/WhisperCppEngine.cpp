#include "WhisperCppEngine.h"

#include <whisper.h>

namespace {

struct CallbackContext {
    const ProgressCallback *onProgress;
    const SegmentCallback *onSegment;
    std::vector<Segment> *out;
    double totalSeconds;
};

void onNewSegment(struct whisper_context *ctx, struct whisper_state *, int n_new, void *userData)
{
    auto *cb = static_cast<CallbackContext *>(userData);
    const int nSegments = whisper_full_n_segments(ctx);
    for (int i = nSegments - n_new; i < nSegments; ++i) {
        Segment segment;
        segment.t0 = whisper_full_get_segment_t0(ctx, i) / 100.0; // centiseconds -> seconds
        segment.t1 = whisper_full_get_segment_t1(ctx, i) / 100.0;
        segment.text = whisper_full_get_segment_text(ctx, i);
        cb->out->push_back(segment);
        if (*cb->onSegment)
            (*cb->onSegment)(segment);
    }
}

void onProgressUpdate(struct whisper_context *, struct whisper_state *, int progress, void *userData)
{
    auto *cb = static_cast<CallbackContext *>(userData);
    if (*cb->onProgress)
        (*cb->onProgress)(cb->totalSeconds * progress / 100.0, cb->totalSeconds);
}

// whisper.cpp checks this deep inside its encode/decode loops (many times
// per second of audio, see whisper.cpp:2455/2977's
// "!(abort_callback && abort_callback(...))" guards) — frequent enough that
// blocking here for a pause is effectively instant, and returning true
// cancels the whole whisper_full() call immediately.
bool onAbortCheck(void *userData)
{
    const auto *onAbort = static_cast<const AbortCallback *>(userData);
    return *onAbort && (*onAbort)();
}

}

WhisperCppEngine::WhisperCppEngine(const QString &modelPath)
{
    whisper_context_params cparams = whisper_context_default_params();
    m_ctx = whisper_init_from_file_with_params(modelPath.toUtf8().constData(), cparams);
}

WhisperCppEngine::~WhisperCppEngine()
{
    if (m_ctx)
        whisper_free(m_ctx);
}

std::vector<Segment> WhisperCppEngine::transcribe(const AudioChunk &chunk, const ProgressCallback &onProgress,
                                                    const SegmentCallback &onSegment, const AbortCallback &onAbort)
{
    std::vector<Segment> segments;
    if (!m_ctx)
        return segments;

    CallbackContext cbCtx{&onProgress, &onSegment, &segments,
                           static_cast<double>(chunk.pcm16kMono.size()) / 16000.0};

    whisper_full_params wparams = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
    wparams.print_progress = false;
    wparams.print_realtime = false;
    wparams.print_special = false;
    wparams.new_segment_callback = onNewSegment;
    wparams.new_segment_callback_user_data = &cbCtx;
    wparams.progress_callback = onProgressUpdate;
    wparams.progress_callback_user_data = &cbCtx;
    if (onAbort) {
        wparams.abort_callback = onAbortCheck;
        wparams.abort_callback_user_data = const_cast<AbortCallback *>(&onAbort);
    }

    whisper_full(m_ctx, wparams, chunk.pcm16kMono.data(), static_cast<int>(chunk.pcm16kMono.size()));

    // Segments are chunk-relative on purpose — the app (not the engine)
    // owns the offset merge (see DESIGN.md "Engine architecture" and
    // CLAUDE.md's load-bearing constraints).
    return segments;
}
