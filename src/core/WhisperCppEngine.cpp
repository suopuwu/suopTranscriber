#include "WhisperCppEngine.h"

#include <QRegularExpression>

#include <cstdio>

#include <whisper.h>

namespace {

// See WhisperCppEngine::activeBackendName()'s doc comment for why this is
// process-wide storage rather than an instance member.
QString &activeBackendNameStorage()
{
    static QString name;
    return name;
}

// Registered via whisper_log_set() before every model load (see the
// constructor below) — whisper.cpp logs the backend it picked as
// "<func>: using <DeviceName><index> backend\n" (see
// whisper_backend_init_gpu() in third_party/whisper.cpp/src/whisper.cpp),
// where <DeviceName> is ggml_backend_dev_name()'s own naming ("CUDA0",
// "Vulkan0", ...) — the trailing device-index digit is stripped since this
// app only ever cares which BACKEND was picked, not which device ordinal.
// Every other log line (there are many, at every log level) simply doesn't
// match and is left alone.
//
// Also replicates whisper_log_callback_default()'s own behavior (fputs to
// stderr, skipping DEBUG-level lines) — registering a custom callback via
// whisper_log_set() REPLACES that default outright, and this app's existing
// tests/dev workflow already rely on whisper.cpp's load/backend noise
// reaching stderr (see tst_engine_contract.cpp's "expected noise" comments)
// — this callback must not silently swallow it.
void onWhisperLog(ggml_log_level level, const char *text, void *userData)
{
    Q_UNUSED(userData);
    if (!text)
        return;
    static const QRegularExpression pattern(QStringLiteral("using ([A-Za-z]+)\\d* backend"));
    const QRegularExpressionMatch m = pattern.match(QString::fromUtf8(text));
    if (m.hasMatch())
        activeBackendNameStorage() = m.captured(1);

    if (level != GGML_LOG_LEVEL_DEBUG) {
        std::fputs(text, stderr);
        std::fflush(stderr);
    }
}

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
    // Must be set BEFORE whisper_init_from_file_with_params() — that's the
    // one call that logs the "using X backend" line this engine parses (see
    // onWhisperLog's doc comment).
    whisper_log_set(onWhisperLog, nullptr);
    whisper_context_params cparams = contextParams();
    m_ctx = whisper_init_from_file_with_params(modelPath.toUtf8().constData(), cparams);
}

QString WhisperCppEngine::activeBackendName() const
{
    if (!m_ctx)
        return QString(); // no model has loaded yet — unknown, not "CPU"
    const QString parsed = activeBackendNameStorage();
    // No GPU line ever fired for a successfully loaded model — whisper.cpp
    // only logs "using X backend" for the GPU/ACCEL path (see
    // whisper_backend_init_gpu()); the CPU backend it always falls back to
    // is added silently, with no equivalent log line of its own.
    return parsed.isEmpty() ? QStringLiteral("CPU") : parsed;
}

WhisperCppEngine::~WhisperCppEngine()
{
    if (m_ctx)
        whisper_free(m_ctx);
}

// Repetition-hallucination hardening (2026-07-16), on top of what the
// pipeline already had (VadChunker's bounded windows, Silero VAD gating,
// whisper.cpp's default no_context=true and temperature-fallback retries):
//
// - BEAM_SEARCH (beam_size 5, whisper.cpp's own default for this strategy)
//   instead of GREEDY. Repetition loops are the classic greedy failure —
//   one bad argmax step enters a cycle it can't back out of, while beam
//   search keeps alternatives alive. whisper.cpp's own CLI has defaulted to
//   beam search (see examples/cli's params.beam_size = 5 → strategy
//   selection), so greedy was the nonstandard path here, not the reference
//   behavior. Cost is roughly 1.5-2x decode time — acceptable for this
//   quality-first app (DESIGN.md).
//
// - entropy_thold 2.2 (default 2.4). This is whisper.cpp's repetition
//   tripwire: a decoded 30s window whose token entropy falls below the
//   threshold (highly repetitive text compresses well = low entropy) is
//   rejected and retried at the next temperature step. Lowering it makes
//   the tripwire more sensitive, at the price of extra retry passes and
//   occasional false trips on genuinely repetitive speech (counting,
//   reading lists) — 2.2 is a deliberately modest tightening, not the
//   floor.
//
// - suppress_nst true (default false): suppresses non-speech tokens (music
//   notation, bracketed sound-effect markers) that seed a different,
//   smaller class of noise-driven hallucination.
whisper_context_params WhisperCppEngine::contextParams()
{
    whisper_context_params cparams = whisper_context_default_params();
    cparams.flash_attn = true;
    return cparams;
}

whisper_full_params WhisperCppEngine::decodeParams()
{
    whisper_full_params wparams = whisper_full_default_params(WHISPER_SAMPLING_BEAM_SEARCH);
    wparams.entropy_thold = 2.2f;
    wparams.suppress_nst = true;
    wparams.print_progress = false;
    wparams.print_realtime = false;
    wparams.print_special = false;
    return wparams;
}

std::vector<Segment> WhisperCppEngine::transcribe(const AudioChunk &chunk, const ProgressCallback &onProgress,
                                                    const SegmentCallback &onSegment, const AbortCallback &onAbort)
{
    std::vector<Segment> segments;
    if (!m_ctx)
        return segments;

    CallbackContext cbCtx{&onProgress, &onSegment, &segments,
                           static_cast<double>(chunk.pcm16kMono.size()) / 16000.0};

    whisper_full_params wparams = decodeParams();
    wparams.new_segment_callback = onNewSegment;
    wparams.new_segment_callback_user_data = &cbCtx;
    wparams.progress_callback = onProgressUpdate;
    wparams.progress_callback_user_data = &cbCtx;
    if (onAbort) {
        wparams.abort_callback = onAbortCheck;
        wparams.abort_callback_user_data = const_cast<AbortCallback *>(&onAbort);
    }

    // Kept alive for the whisper_full() call below — vad_model_path is a raw
    // pointer into this buffer, not a copy (see setVadModelPath()'s doc
    // comment).
    QByteArray vadModelPathUtf8;
    if (!m_vadModelPath.isEmpty()) {
        vadModelPathUtf8 = m_vadModelPath.toUtf8();
        wparams.vad = true;
        wparams.vad_model_path = vadModelPathUtf8.constData();
        wparams.vad_params = whisper_vad_default_params();
    }

    whisper_full(m_ctx, wparams, chunk.pcm16kMono.data(), static_cast<int>(chunk.pcm16kMono.size()));

    // Segments are chunk-relative on purpose — the app (not the engine)
    // owns the offset merge (see DESIGN.md "Engine architecture" and
    // CLAUDE.md's load-bearing constraints).
    return segments;
}
