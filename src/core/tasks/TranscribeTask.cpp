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

// Stall-watchdog thresholds (TranscribeTask::isStalled()). whisper.cpp has no
// other cooperative interruption point inside a single decode call besides
// onAbort — which it polls "many times per second" deep in its own loop (see
// WhisperCppEngine::onAbortCheck) — so THAT is the one hook that can reach
// into a genuinely wedged whisper_full() and bound the damage. Deliberately
// conservative in both directions, per the 2026-07-25 decision: a FALSE trip
// destroys a real, in-progress transcription (unrecoverable), while a LATE
// trip on a genuinely stalled file only costs wall-clock time (recoverable —
// the task just fails retryably and the batch moves on). The 2026-07-24
// batch-wedge report is the motivating case: one 3-minute file stalled with
// near-zero progress for 1h46m (user-observed, before manual pause), silently
// blocking ~88 queued files behind it with no backstop at all — ANY bounded
// watchdog is a strict improvement over that.
//
// Bound to the FILE's own total duration, not the current window's — runWindows()
// (TranscribePipeline.cpp) owns per-window boundaries, which would give a
// tighter bound, but that file is out of scope for this change. Using the
// coarser whole-file bound still satisfies the non-negotiable
// ("audio-length-relative, NOT a flat wall-clock cap"): a longer
// recording earns proportionally more silent runway before it's ever
// considered wedged, so a genuinely long file is never falsely killed —
// at the cost of a proportionally later trip on a huge file that really is
// stalled, which is the accepted, explicitly-chosen tradeoff.
constexpr qint64 kStallFloorMs = 120'000; // 2 minutes, regardless of file length
constexpr double kStallRealtimeMultiplier = 20.0; // x the file's own duration
} // namespace

bool TranscribeTask::isStalled(qint64 noProgressElapsedMs, double fileSeconds)
{
    const qint64 scaledMs =
        static_cast<qint64>(kStallRealtimeMultiplier * std::max(0.0, fileSeconds) * 1000.0);
    const qint64 threshold = std::max(kStallFloorMs, scaledMs);
    return noProgressElapsedMs > threshold;
}

QString TranscribeTask::corruptAudioWarning(const AudioDecoder::DecodeDiagnostics &diagnostics)
{
    if (diagnostics.nonFiniteSampleCount == 0)
        return {};
    return QStringLiteral("%1 sample(s) were non-finite (NaN/Inf) and replaced with "
                          "silence, starting at %2")
        .arg(diagnostics.nonFiniteSampleCount)
        .arg(diagnostics.firstNonFiniteSeconds ? formatTimestamp(*diagnostics.firstNonFiniteSeconds)
                                                : QStringLiteral("unknown"));
}

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
    AudioDecoder::DecodeDiagnostics decodeDiagnostics;
    auto pcm = AudioDecoder::decodeTo16kMono(filePath, &decodeError, decodeControl, &decodeDiagnostics);
    if (!pcm)
        throw TaskError(decodeError.isEmpty() ? QStringLiteral("audio decode failed") : decodeError);

    // Corrupt (non-finite) samples were scrubbed to silence rather than left
    // to hang ffmpeg's speechnorm filter — record where, so the
    // resulting silent gap in the transcript has a written cause rather than
    // looking like a transcription bug. Surfaced twice from the one string:
    // permanently in the frontmatter below, and live in the Tasks list via
    // TaskContext::reportWarning() — the task still succeeds
    // (Status::Done), it just has something to say.
    const QString corruptAudioNote = corruptAudioWarning(decodeDiagnostics);
    if (!corruptAudioNote.isEmpty())
        ctx.reportWarning(corruptAudioNote);

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
                      /*partial=*/true, initialSilences, corruptAudioNote);
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

    // Stall watchdog (see kStallFloorMs/kStallRealtimeMultiplier's doc
    // comment above): resets on every real tick (onProgress/onSegment below)
    // and elapsed() is checked from onAbort, the one hook whisper.cpp polls
    // frequently enough to actually interrupt a wedged decode. `stalled`
    // records WHY onAbort returned true so the code after runWindows() can
    // tell a real stall apart from an ordinary cancel.
    QElapsedTimer stallClock;
    stallClock.start();
    bool stalled = false;

    // The engine checks this deep inside its own encode/decode loops
    // (WhisperCppEngine's onAbortCheck) — frequent enough that blocking here
    // for a pause is near-instant, and returning true stops the engine
    // mid-file rather than only at its next natural boundary.
    const AbortCallback onAbort = [&ctx, &stallClock, &stalled, totalFileSeconds] {
        // Time this call itself spends blocked inside checkpoint() (its
        // pause loop wakes every 100ms — see TaskContext::checkpoint()) so a
        // real pause-then-resume can be told apart from a stall without
        // racing ctx.pauseRequested() against checkpoint() actually entering
        // its wait loop. A cheap sanity margin above one wake interval.
        QElapsedTimer checkpointTimer;
        checkpointTimer.start();
        if (ctx.checkpoint())
            return true; // real cancel, or checkpoint() blocked then cancelled
        if (checkpointTimer.elapsed() > 200) {
            // Blocked for a real stretch — a pause resumed. That silence is
            // intentional, not a stall: exempt it and don't let the paused
            // time count against the watchdog.
            stallClock.restart();
            return false;
        }
        if (TranscribeTask::isStalled(stallClock.elapsed(), totalFileSeconds)) {
            stalled = true;
            return true;
        }
        return false;
    };

    ProgressCallback onProgress = [this, totalFileSeconds, &stallClock](double done, double) {
        stallClock.restart();
        emit progress(totalFileSeconds > 0 ? done / totalFileSeconds : 0.0);
    };
    // Only accumulated when VAD didn't run — feeds the transcript-gap
    // silence fallback below. Real transcribed-segment (t0, t1) pairs stand
    // in for "speech spans" when there's no VAD data to complement instead
    // (see the fallback's doc comment below).
    std::vector<std::pair<double, double>> fallbackSpeechSpans;
    SegmentCallback onSegment = [&lineWriter, &liveTimer, &lastLiveTickMs, &fallbackSpeechSpans, &stallClock,
                                  vadActive, this](const Segment &segment) {
        stallClock.restart();
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
    int failureCount = 0;
    TranscribePipeline::runWindows(*asr.engine, *pcm, windows, onProgress, onSegment, onAbort, &failureCount);
    mdFile.close();

    // The watchdog tripped: fail retryably (a Task-system
    // failure, not a silent hang) so the Tasks list shows it and the queue
    // advances to the next file rather than wedging the rest of an overnight
    // batch behind it, same as any other TaskError. Checked BEFORE the plain
    // cancel below since onAbort returning true for a stall is not a cancel.
    if (stalled) {
        throw TaskError(QStringLiteral("transcription stalled — no progress for over %1 min on a %2s file")
                             .arg(QString::number(stallClock.elapsed() / 60000.0, 'f', 1),
                                  QString::number(totalFileSeconds, 'f', 0)));
    }

    // A cancel mid-decode stops the engine early (see onAbort above) but
    // everything transcribed up to that point is still flushed and valid
    // (SegmentLineWriter's per-append durability contract) — reported as
    // Failed/"Cancelled" rather than Done, matching the Tasks list's
    // honesty contract. Its .md keeps the `partial: true` marker written above.
    if (ctx.cancelRequested())
        throw TaskError(QStringLiteral("Cancelled"));

    // An inference failure (e.g. onnxruntime OOM) dropped one or more
    // sub-windows silently at the engine level — the transcript that was
    // written is real but incomplete. Fail the task BEFORE the
    // `partial: true` marker is dropped below, so the
    // vault keeps showing this file as not-done/retryable rather than a
    // green Done hiding missing audio.
    if (failureCount > 0)
        throw TaskError(QStringLiteral("transcription failed for %1 audio segment(s) (out of memory or decode "
                                       "error) — transcript is incomplete")
                            .arg(failureCount));

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
