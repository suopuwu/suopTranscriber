#include "TranscribeTask.h"

#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>

#include <algorithm>
#include <optional>
#include <utility>

#include "../AudioDecoder.h"
#include "../MdFrontmatter.h"
#include "../ModelManager.h"
#include "../Segment.h"
#include "../TranscribePipeline.h"
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
    const TranscribePipeline::Windowing win =
        TranscribePipeline::planWindows(isEngineModelId(m_modelId), *pcm, *asr.engine);
    const std::vector<VadChunker::Window> &windows = win.windows;
    const bool vadActive = win.vadActive;
    const QString vadError = win.vadError;
    emit phaseDurationMs(QStringLiteral("detecting-speech"), phaseTimer.restart());

    const double totalFileSeconds = static_cast<double>(pcm->size()) / 16000.0;

    // Silence-capture feature, phase 1 (Decision: VAD primary). When VAD
    // actually ran, the real complement is already known up front — no need
    // to wait for the transcript to exist. When it didn't (self-segmenting
    // engine, or VAD failed), this stays nullopt (key omitted for now) and
    // gets filled in with the transcript-gap fallback once the transcript
    // exists, below (see the "so the key is always present" contract).
    std::optional<std::vector<std::pair<double, double>>> initialSilences;
    if (vadActive)
        initialSilences = VadChunker::complementSpans(win.speechSpans, totalFileSeconds);

    QFile mdFile(sidecarPath(filePath));
    if (!mdFile.open(QIODevice::WriteOnly | QIODevice::Truncate))
        throw TaskError(QStringLiteral("could not open %1 for writing").arg(mdFile.fileName()));

    // Pure-ASR transcript: no speaker detection at transcribe time, so
    // frontmatter records speakerDetection: false with an empty speaker
    // list (a standalone diarize task splices real speakers in later).
    writeFrontmatter(mdFile, asr.modelUsedName, /*speakerDetection=*/false, asr.fellBack, m_modelId,
                      /*speakerLabels=*/{}, /*speakerDetectionError=*/QString(), vadActive, vadError,
                      /*partial=*/true, initialSilences);
    SegmentLineWriter lineWriter(mdFile);

    // "transcribing": the engine's own per-segment progress, blended across
    // ALL windows into one cumulative 0->1 — each window's own done/total
    // would otherwise snap the bar back to 0% at every window boundary.
    emit phase(QStringLiteral("transcribing"));
    emit progress(0.0);

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

    ProgressCallback onProgress = [this, totalFileSeconds](double done, double) {
        emit progress(totalFileSeconds > 0 ? done / totalFileSeconds : 0.0);
    };
    // Only accumulated when VAD didn't run — feeds the transcript-gap
    // silence fallback below. Real transcribed-segment (t0, t1) pairs stand
    // in for "speech spans" when there's no VAD data to complement instead
    // (see the fallback's doc comment below).
    std::vector<std::pair<double, double>> fallbackSpeechSpans;
    SegmentCallback onSegment = [&lineWriter, &liveTimer, &lastLiveTickMs, &fallbackSpeechSpans, vadActive,
                                  this](const Segment &segment) {
        // Segment is already absolute (offset merge is the pipeline's job
        // now). Pure ASR — no speaker label written at transcribe time.
        lineWriter.append(segment, QString());
        if (!vadActive)
            fallbackSpeechSpans.emplace_back(segment.t0, segment.t1);

        const qint64 nowMs = liveTimer.elapsed();
        if (nowMs - lastLiveTickMs >= kLiveUpdateThrottleMs) {
            lastLiveTickMs = nowMs;
            emit segmentWritten();
        }
    };
    TranscribePipeline::runWindows(*asr.engine, *pcm, windows, onProgress, onSegment, onAbort);
    mdFile.close();

    // A cancel mid-decode stops the engine early (see onAbort above) but
    // everything transcribed up to that point is still flushed and valid
    // (SegmentLineWriter's per-append durability contract) — reported as
    // Failed/"Cancelled" rather than Done, matching the Tasks list's
    // honesty contract. Its .md keeps the `partial: true` marker written above.
    if (ctx.cancelRequested())
        throw TaskError(QStringLiteral("Cancelled"));

    // Fully completed: drop the `partial: true` marker so the finished .md is
    // no longer flagged incomplete. Only a completed run reaches here — a
    // cancelled OR crashed/killed run does not, so its truncated .md stays
    // marked partial and shows distinctly in the vault (the whole point).
    QStringList fmLines;
    QString fmBody;
    QString fmError;
    if (MdFrontmatter::read(sidecarPath(filePath), &fmLines, &fmBody, &fmError)) {
        MdFrontmatter::upsertKey(&fmLines, QStringLiteral("partial"), QString(), /*removeIfEmpty=*/true);
        // Silence-capture feature, phase 1: VAD didn't run (initialSilences
        // was left nullopt above, so the "silences:" key isn't in fmLines
        // yet) — now that the transcript exists, fill it with the
        // transcript-gap fallback (real segment t0/t1 pairs standing in for
        // "speech spans", complemented the exact same way the VAD path is)
        // so a fully-completed transcription always ends up with the key
        // one way or another, per this feature's "always present" contract.
        if (!vadActive) {
            const auto fallbackSilences = VadChunker::complementSpans(fallbackSpeechSpans, totalFileSeconds);
            MdFrontmatter::upsertKey(&fmLines, QStringLiteral("silences"), formatSilencesValue(fallbackSilences));
        }
        writeFrontmatterBlock(sidecarPath(filePath), fmLines, fmBody, &fmError);
    }
    emit phaseDurationMs(QStringLiteral("transcribing"), phaseTimer.elapsed());
}
