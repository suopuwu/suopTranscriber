#include "TranscribeTask.h"

#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>

#include <algorithm>

#include "../AudioDecoder.h"
#include "../ModelManager.h"
#include "../Segment.h"
#include "../TranscriptionController.h"
#include "../TranscriptionEngine.h"
#include "../VadChunker.h"
#include "../WhisperCppEngine.h"
#include "EngineCache.h"

namespace {
// Minimum spacing between segmentWritten() emissions — a chatty stretch of
// audio can produce several segments within milliseconds of each other, and
// each live-update tick costs QML a full file re-read/re-parse/re-render, so
// bursts are coalesced down to this rate. Same value as the old worker's
// kLiveUpdateThrottleMs.
constexpr qint64 kLiveUpdateThrottleMs = 200;
} // namespace

TranscribeTask::TranscribeTask(QUrl file, QUrl vault, QString modelId, QObject *parent)
    : Task(parent)
    , m_modelId(std::move(modelId))
{
    fileUrl = std::move(file);
    vaultUrl = std::move(vault);
}

QString TranscribeTask::displayName() const
{
    return QStringLiteral("Transcribe: %1").arg(QFileInfo(fileUrl.toLocalFile()).fileName());
}

QJsonObject TranscribeTask::toJson() const
{
    return {{QStringLiteral("modelId"), m_modelId}};
}

QVariantMap TranscribeTask::toVariantMap() const
{
    // modelId is what the Tasks popup shows for this row alongside the file.
    return {{QStringLiteral("modelId"), m_modelId}};
}

ModelPrerequisites TranscribeTask::prerequisites() const
{
    return {{ModelDownloadTarget::WhisperModel, m_modelId}};
}

void TranscribeTask::run(TaskContext &ctx)
{
    // Read fresh (not cached at construction): the combine preflight may have
    // retargeted this at a merged output — see the class doc comment.
    const QString filePath = fileUrl.toLocalFile();
    const QString vaultPath = vaultUrl.toLocalFile();

    // GPU memory hygiene: transcribe and label tasks never run concurrently
    // (one task at a time), so there's no reason to keep the label engine's
    // GPU-resident weights around while a transcription runs.
    ctx.engines.evictLabel();

    // Wall-clock timer for whichever phase is currently running, restarted
    // (recording the PREVIOUS phase's duration via phaseDurationMs) at each
    // transition — feeds the queue's rolling-average phase-weighting.
    // Deliberately only recorded right as a phase completes NORMALLY (never
    // on a cancel/failure return in between), so an aborted phase's
    // artificially-short elapsed time never pollutes the average.
    QElapsedTimer phaseTimer;
    phaseTimer.start();

    // "loading": whisper.cpp has no progress hook for model load itself
    // (whisper_init_from_file_with_params is one blocking call) — this
    // phase covers ensureModel()'s (rare, first-run) download plus the
    // model load, both otherwise invisible.
    emit phase(QStringLiteral("loading"));

    const EngineCache::AsrHandle asr = ctx.engines.ensureAsr(m_modelId, vaultPath);
    if (!asr.engine)
        throw TaskError(asr.error);
    emit backendNameChanged(asr.backendName);

    // A cancel (or pause, which resolves to cancel at this checkpoint — see
    // the old pauseActive()'s doc comment) requested DURING "loading" can't
    // be caught mid-model-load (no callback hook exists there), but must not
    // then ALSO be ignored through the entire audio decode below — check the
    // instant loading finishes, before starting anything else.
    if (ctx.cancelRequested())
        throw TaskError(QStringLiteral("Cancelled"));
    emit phaseDurationMs(QStringLiteral("loading"), phaseTimer.restart());

    // "decoding": unlike "loading" above, ffmpeg runs as a real subprocess —
    // genuinely pausable (SIGSTOP the process) and instantly cancellable
    // (kill it), same mechanism as ModelManager's downloads.
    emit phase(QStringLiteral("decoding"));
    bool decodePausedSignaled = false;
    AudioDecoder::DecodeControl decodeControl;
    decodeControl.paused = [&ctx, &decodePausedSignaled] {
        const bool p = ctx.pauseRequested();
        if (p != decodePausedSignaled) {
            decodePausedSignaled = p;
            ctx.reportPaused(p);
        }
        return p;
    };
    decodeControl.cancelled = [&ctx] { return ctx.cancelRequested(); };
    QString decodeError;
    auto pcm = AudioDecoder::decodeTo16kMono(filePath, &decodeError, decodeControl);
    if (!pcm)
        throw TaskError(decodeError.isEmpty() ? QStringLiteral("audio decode failed") : decodeError);

    // Transcribe is pure ASR — speaker diarization is a SEPARATE task
    // (DiarizeTask), never run inline here. A fresh transcript is therefore
    // always written speaker-less; a later diarize task splices "[Speaker
    // N]" tags + "speakers:" into the .md in place.

    // A cancel/pause requested during "loading" or "decoding" takes effect
    // here, right after the decode finishes and before any file I/O.
    if (ctx.checkpoint())
        throw TaskError(QStringLiteral("Cancelled"));

    // "detecting-speech": VAD windowing — one small model does double duty:
    // (a) whisper.cpp's own per-window silence gating and (b) finding
    // VAD-aligned window boundaries so a long file is transcribed as several
    // bounded calls instead of one unbounded one (reduces long-context
    // hallucination drift). A download/load failure degrades gracefully to
    // a single whole-file window with no gating — never blocks the task —
    // recorded honestly in frontmatter below.
    emit phase(QStringLiteral("detecting-speech"));
    phaseTimer.restart();
    QString vadError;
    std::vector<VadChunker::Window> windows;
    bool vadActive = false;
    const bool useSubprocessEngine = isEngineModelId(m_modelId);
    if (useSubprocessEngine) {
        // Out-of-process engines load a heavy model per process and do
        // their own internal segmentation, so feed the whole file as ONE
        // chunk (one spawn) rather than VAD-windowing it into many.
        windows.push_back({0.0, static_cast<double>(pcm->size()) / 16000.0});
    } else {
        const QString vadModelPath = ModelManager::ensureVadModel(&vadError);
        if (!vadModelPath.isEmpty()) {
            QString chunkingError;
            windows = VadChunker::computeWindows(*pcm, vadModelPath, &chunkingError);
            if (chunkingError.isEmpty())
                vadActive = true;
            else
                vadError = chunkingError;
        }
        if (windows.empty())
            windows.push_back({0.0, static_cast<double>(pcm->size()) / 16000.0});
        // m_asr is a persistent cache entry reused across tasks — a
        // PREVIOUS task's successful VAD path must not silently leak into
        // this one if this task's VAD failed. Guarded to the concrete
        // whisper engine (VAD gating is its feature); a subprocess engine
        // never reaches here.
        if (auto *whisper = dynamic_cast<WhisperCppEngine *>(asr.engine))
            whisper->setVadModelPath(vadActive ? vadModelPath : QString());
    }
    emit phaseDurationMs(QStringLiteral("detecting-speech"), phaseTimer.restart());

    QFile mdFile(sidecarPath(filePath));
    if (!mdFile.open(QIODevice::WriteOnly | QIODevice::Truncate))
        throw TaskError(QStringLiteral("could not open %1 for writing").arg(mdFile.fileName()));

    // Pure-ASR transcript: no speaker detection at transcribe time, so
    // frontmatter records speakerDetection: false with an empty speaker
    // list (a standalone diarize task splices real speakers in later).
    writeFrontmatter(mdFile, asr.modelUsedName, /*speakerDetection=*/false, asr.fellBack, m_modelId,
                      /*speakerLabels=*/{}, /*speakerDetectionError=*/QString(), vadActive, vadError);
    SegmentLineWriter lineWriter(mdFile);

    // "transcribing": the engine's own per-segment progress, blended across
    // ALL windows into one cumulative 0->1 — each window's own done/total
    // would otherwise snap the bar back to 0% at every window boundary.
    emit phase(QStringLiteral("transcribing"));
    emit progress(0.0);
    const double totalFileSeconds = static_cast<double>(pcm->size()) / 16000.0;
    double secondsDoneInPriorWindows = 0.0;

    // Drives the old liveUpdateTick — throttled independently of the
    // engine's own progress ticks, which fire on every internal decode-loop
    // iteration (including retries) rather than on real new content.
    // Starts already kLiveUpdateThrottleMs in the past so the FIRST segment's
    // tick fires immediately instead of waiting out the throttle window.
    QElapsedTimer liveTimer;
    liveTimer.start();
    qint64 lastLiveTickMs = -kLiveUpdateThrottleMs;

    // The engine checks this deep inside its own encode/decode loops
    // (WhisperCppEngine's onAbortCheck) — frequent enough that blocking here
    // for a pause is near-instant, and returning true stops the engine
    // mid-file rather than only at its next natural boundary.
    const AbortCallback onAbort = [&ctx] { return ctx.checkpoint(); };

    for (const VadChunker::Window &window : windows) {
        // Between-window pause/cancel checkpoint — mid-window pause/cancel
        // is still handled by onAbort above, unchanged.
        if (ctx.checkpoint())
            break;

        const auto startSample = static_cast<std::size_t>(window.startSeconds * 16000.0);
        const auto endSample = std::min(pcm->size(), static_cast<std::size_t>(window.endSeconds * 16000.0));
        if (endSample <= startSample)
            continue;

        AudioChunk chunk;
        chunk.pcm16kMono.assign(pcm->begin() + static_cast<std::ptrdiff_t>(startSample),
                                 pcm->begin() + static_cast<std::ptrdiff_t>(endSample));
        chunk.offsetSeconds = window.startSeconds;
        const double windowSeconds = static_cast<double>(chunk.pcm16kMono.size()) / 16000.0;

        ProgressCallback onProgress = [this, &secondsDoneInPriorWindows, totalFileSeconds](double done, double) {
            const double fraction =
                totalFileSeconds > 0 ? (secondsDoneInPriorWindows + done) / totalFileSeconds : 0.0;
            emit progress(fraction);
        };
        SegmentCallback onSegment = [&lineWriter, &liveTimer, &lastLiveTickMs, &chunk, this](const Segment &rawSegment) {
            // Chunk offset merge is the app's job — a REAL merge for
            // multi-window files, not a no-op.
            Segment segment = rawSegment;
            segment.t0 += chunk.offsetSeconds;
            segment.t1 += chunk.offsetSeconds;
            // Pure ASR — no speaker label written at transcribe time.
            lineWriter.append(segment, QString());

            const qint64 nowMs = liveTimer.elapsed();
            if (nowMs - lastLiveTickMs >= kLiveUpdateThrottleMs) {
                lastLiveTickMs = nowMs;
                emit segmentWritten();
            }
        };

        asr.engine->transcribe(chunk, onProgress, onSegment, onAbort);
        secondsDoneInPriorWindows += windowSeconds;

        if (ctx.cancelRequested())
            break;
    }
    mdFile.close();

    // A cancel mid-decode stops the engine early (see onAbort above) but
    // everything transcribed up to that point is still flushed and valid
    // (SegmentLineWriter's per-append durability contract) — reported as
    // Failed/"Cancelled" rather than Done, matching the Tasks list's
    // honesty contract.
    if (ctx.cancelRequested())
        throw TaskError(QStringLiteral("Cancelled"));
    emit phaseDurationMs(QStringLiteral("transcribing"), phaseTimer.elapsed());
}
