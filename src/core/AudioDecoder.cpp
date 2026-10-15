#include "AudioDecoder.h"
#include "ProcessRunner.h"

#include <QElapsedTimer>
#include <QProcess>

#include <cmath>
#include <cstring>

namespace AudioDecoder {

namespace {
constexpr qint64 kDecodeStallMs = 60'000;
// A pathological file (repeated per-frame decode errors at -loglevel error)
// is failing anyway — the first lines are the informative ones, so cap
// accumulated stderr rather than let it grow unbounded across the whole run.
constexpr qint64 kMaxErrTextBytes = 64 * 1024;

// Result of running one ffmpeg subprocess to completion under the
// pause/cancel/stall watchdog. Two passes share this (see decodeTo16kMono):
// pass 1 reads the file straight off disk, pass 2 re-filters an in-memory
// buffer piped through stdin — everything else about draining/watchdogging
// them is identical, so it's factored out rather than duplicated.
struct PassOutcome {
    bool startFailed = false;
    ProcessRunner::RunOutcome outcome = ProcessRunner::RunOutcome::Finished;
    QByteArray raw;
    QByteArray errText;
    qint64 errTextDroppedBytes = 0;
    QProcess::ExitStatus exitStatus = QProcess::CrashExit;
    int exitCode = -1;
};

// stdinPayload, if non-null, is written and the write channel closed before
// entering the poll loop — QProcess buffers whatever doesn't fit immediately
// and flushes the rest opportunistically inside the same waitForReadyRead()
// calls the poll loop already makes to drain stdout (Qt's process wait
// implementation polls every pipe fd together, not just the one asked for),
// so a large buffer here does not risk the classic write-while-child-blocks-
// on-a-full-stdout-pipe deadlock.
PassOutcome runFfmpegPass(const QStringList &args, const QByteArray *stdinPayload,
                           const DecodeControl &control)
{
    PassOutcome result;

    QProcess ffmpeg;
    ProcessRunner::guardChildLifetime(ffmpeg);
    ffmpeg.start("ffmpeg", args);

    if (!ffmpeg.waitForStarted()) {
        result.startFailed = true;
        return result;
    }

    if (stdinPayload)
        ffmpeg.write(*stdinPayload);
    ffmpeg.closeWriteChannel();

    // Pausable 100ms poll (see ProcessRunner::runPausable). Draining stdout
    // every iteration is required, not optional: ffmpeg writes raw PCM to a
    // pipe with a finite OS buffer (~64KB) — falling behind stalls ffmpeg on
    // write() rather than actually pausing it via SIGSTOP.
    qint64 lastRawSize = 0;
    // stderr needs draining every iteration for exactly the reason stdout
    // does above: its pipe buffer is finite too, and an ffmpeg blocked
    // writing a flood of per-frame errors (e.g. thousands of "Header
    // missing" lines from a damaged stream, even at -loglevel error) never
    // exits, wedging the poll loop the same way a full stdout pipe would.
    const auto appendErr = [&](const QByteArray &chunk) {
        if (chunk.isEmpty())
            return;
        const qint64 room = kMaxErrTextBytes - result.errText.size();
        if (room <= 0) {
            result.errTextDroppedBytes += chunk.size();
            return;
        }
        if (chunk.size() <= room) {
            result.errText += chunk;
        } else {
            result.errText += chunk.left(room);
            result.errTextDroppedBytes += chunk.size() - room;
        }
    };
    QElapsedTimer noOutputClock;
    noOutputClock.start();
    const auto stalled = [&] {
        // A real pause (SIGSTOP on the child) isn't a stall — restart the
        // clock so the paused stretch never counts against the watchdog,
        // same exemption TranscribeTask::onAbort makes for its own pause
        // carve-out (TranscribeTask.cpp's >200ms checkpoint-blocked check).
        if (control.paused && control.paused()) {
            noOutputClock.restart();
            return false;
        }
        return isDecodeStalled(noOutputClock.elapsed());
    };
    result.outcome = ProcessRunner::runPausable(
        ffmpeg, {control.cancelled, control.paused, {}, stalled},
        [&] {
            result.raw += ffmpeg.readAllStandardOutput();
            appendErr(ffmpeg.readAllStandardError());
            if (result.raw.size() != lastRawSize) {
                lastRawSize = result.raw.size();
                noOutputClock.restart();
            }
        });
    result.raw += ffmpeg.readAllStandardOutput(); // final drain after the last poll
    appendErr(ffmpeg.readAllStandardError());
    result.exitStatus = ffmpeg.exitStatus();
    result.exitCode = ffmpeg.exitCode();
    return result;
}

// Shared failure->errorOut formatting for both passes: report the
// accumulated buffer, not a fresh readAllStandardError() (stderr was drained
// continuously above, so by now the pipe is empty and a post-exit read would
// return almost nothing), plus a truncation note if the cap was hit.
QString describeFailure(const PassOutcome &pass)
{
    QString text = QString::fromUtf8(pass.errText);
    if (pass.errTextDroppedBytes > 0)
        text += QStringLiteral(" [truncated, %1 more bytes of stderr omitted]").arg(pass.errTextDroppedBytes);
    return text;
}

}

bool isDecodeStalled(qint64 noOutputElapsedMs)
{
    return noOutputElapsedMs > kDecodeStallMs;
}

std::optional<QString> runFfprobe(const QString &path, const QStringList &args)
{
    QProcess probe;
    ProcessRunner::guardChildLifetime(probe);
    QStringList fullArgs = args;
    fullArgs << path;
    probe.start(QStringLiteral("ffprobe"), fullArgs);
    if (!probe.waitForFinished(10'000) || probe.exitStatus() != QProcess::NormalExit || probe.exitCode() != 0)
        return std::nullopt;
    return QString::fromUtf8(probe.readAllStandardOutput()).trimmed();
}

std::optional<double> probeDurationSeconds(const QString &path)
{
    const std::optional<QString> out =
        runFfprobe(path, {QStringLiteral("-v"), QStringLiteral("error"), QStringLiteral("-show_entries"),
                          QStringLiteral("format=duration"), QStringLiteral("-of"), QStringLiteral("csv=p=0")});
    if (!out)
        return std::nullopt;
    bool ok = false;
    const double seconds = out->toDouble(&ok);
    return (ok && seconds >= 0.0) ? std::make_optional(seconds) : std::nullopt;
}

std::optional<std::vector<float>> decodeTo16kMono(const QString &filePath, QString *errorOut,
                                                    const DecodeControl &control,
                                                    DecodeDiagnostics *diagnosticsOut)
{
    // Pass 1: decode WITHOUT speechnorm. speechnorm tracks a rolling local
    // peak to compute time-varying gain, and NaN compares false against
    // everything, so a NaN sample makes its peak-advance loop never
    // progress — the filter thread spins at 100% CPU forever (an ffmpeg-side
    // defect; measured on real corrupt wireless-mic recorder WAVs).
    // Plain decode/resample has no such loop and is NaN-safe (also
    // measured), so this pass is where any non-finite samples get isolated
    // — scrubbed below — before speechnorm ever sees them.
    const auto pass1 = runFfmpegPass({
        "-i", filePath,
        "-ar", "16000",
        "-ac", "1",
        "-f", "f32le",
        "-hide_banner", "-loglevel", "error",
        "-",
    }, nullptr, control);

    if (pass1.startFailed) {
        if (errorOut)
            *errorOut = QStringLiteral("failed to start ffmpeg");
        return std::nullopt;
    }
    if (pass1.outcome == ProcessRunner::RunOutcome::Cancelled) {
        if (errorOut)
            *errorOut = QStringLiteral("Cancelled");
        return std::nullopt;
    }
    if (pass1.outcome == ProcessRunner::RunOutcome::Stalled) {
        if (errorOut)
            *errorOut = QStringLiteral("decode stalled — ffmpeg produced no output for 60s "
                                        "(the file may contain corrupt audio): %1")
                            .arg(filePath);
        return std::nullopt;
    }
    if (pass1.exitStatus != QProcess::NormalExit || pass1.exitCode != 0) {
        if (errorOut)
            *errorOut = describeFailure(pass1);
        return std::nullopt;
    }

    std::vector<float> pcm(pass1.raw.size() / sizeof(float));
    std::memcpy(pcm.data(), pass1.raw.constData(), pcm.size() * sizeof(float));

    // Scrub: silence any non-finite sample. |sample| > 1.0 is LEGITIMATE
    // float headroom on these recordings (1.56M such samples measured on one
    // real file, all fine) and must survive untouched — only isfinite()
    // fails a sample here, never magnitude.
    std::size_t nonFiniteCount = 0;
    std::optional<std::size_t> firstNonFiniteIndex;
    for (std::size_t i = 0; i < pcm.size(); ++i) {
        if (!std::isfinite(pcm[i])) {
            if (!firstNonFiniteIndex)
                firstNonFiniteIndex = i;
            pcm[i] = 0.0f;
            ++nonFiniteCount;
        }
    }
    if (diagnosticsOut) {
        diagnosticsOut->nonFiniteSampleCount = nonFiniteCount;
        diagnosticsOut->firstNonFiniteSeconds =
            firstNonFiniteIndex ? std::optional<double>(static_cast<double>(*firstNonFiniteIndex) / 16000.0)
                                 : std::nullopt;
    }

    // Pass 2: apply speechnorm to the CLEANED buffer via a second ffmpeg
    // process, fed raw f32 over stdin — speechnorm never sees a NaN this way
    // regardless of how corrupt the source file was, so this pass is safe
    // unconditionally (not just when nonFiniteCount is 0) and every decode
    // still gets speechnorm's quiet-passage benefit (below). Reads f32le
    // rather than a container so no header/muxing round-trip is needed for
    // an in-memory buffer.
    //
    // speechnorm (not loudnorm/dynaudnorm): purpose-built for speech,
    // time-preserving (no effect on segment timestamps downstream). A
    // quiet voice in an otherwise-loud recording falls below whisper.cpp's
    // mel-normalization clamp floor (mmax - 8dB, see whisper.cpp's
    // log_mel_spectrogram) and is effectively discarded before the model
    // ever sees it — only time-varying gain fixes that, a single global
    // gain on an already-uniformly-quiet file would be a no-op.
    // e=25 (ffmpeg's own default e=2 barely moves a quiet passage sitting
    // well under a loud one) and r=0.05 (ffmpeg's default r=0.001 reacts
    // too slowly to meaningfully close the gap within a normal recording)
    // — empirically measured via ffmpeg astats on a synthetic loud/quiet
    // test signal (27dB raw gap narrows to ~20dB with these values,
    // r=0.001 only narrowed it to ~26dB). See
    // tst_audiodecoder.cpp's speechnorm test for the same measurement.
    const QByteArray cleaned(reinterpret_cast<const char *>(pcm.data()),
                              static_cast<int>(pcm.size() * sizeof(float)));
    const auto pass2 = runFfmpegPass({
        "-f", "f32le", "-ar", "16000", "-ac", "1", "-i", "-",
        "-af", "speechnorm=e=25:p=0.95:r=0.05",
        "-ar", "16000", "-ac", "1",
        "-f", "f32le",
        "-hide_banner", "-loglevel", "error",
        "-",
    }, &cleaned, control);

    if (pass2.startFailed) {
        if (errorOut)
            *errorOut = QStringLiteral("failed to start ffmpeg");
        return std::nullopt;
    }
    if (pass2.outcome == ProcessRunner::RunOutcome::Cancelled) {
        if (errorOut)
            *errorOut = QStringLiteral("Cancelled");
        return std::nullopt;
    }
    if (pass2.outcome == ProcessRunner::RunOutcome::Stalled) {
        if (errorOut)
            *errorOut = QStringLiteral("decode stalled — ffmpeg (speechnorm pass) produced no output "
                                        "for 60s: %1")
                            .arg(filePath);
        return std::nullopt;
    }
    if (pass2.exitStatus != QProcess::NormalExit || pass2.exitCode != 0) {
        if (errorOut)
            *errorOut = describeFailure(pass2);
        return std::nullopt;
    }

    std::vector<float> normalized(pass2.raw.size() / sizeof(float));
    std::memcpy(normalized.data(), pass2.raw.constData(), normalized.size() * sizeof(float));
    return normalized;
}

}
