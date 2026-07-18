#include "AudioDecoder.h"
#include "ProcessRunner.h"

#include <QProcess>
#include <cstring>

namespace AudioDecoder {

std::optional<std::vector<float>> decodeTo16kMono(const QString &filePath, QString *errorOut,
                                                    const DecodeControl &control)
{
    QProcess ffmpeg;
    ffmpeg.start("ffmpeg", {
        "-i", filePath,
        "-ar", "16000",
        "-ac", "1",
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
        "-af", "speechnorm=e=25:p=0.95:r=0.05",
        "-f", "f32le",
        "-hide_banner", "-loglevel", "error",
        "-",
    });

    if (!ffmpeg.waitForStarted()) {
        if (errorOut)
            *errorOut = QStringLiteral("failed to start ffmpeg");
        return std::nullopt;
    }

    ffmpeg.closeWriteChannel();

    // Pausable 100ms poll (see ProcessRunner::runPausable). Draining stdout
    // every iteration is required, not optional: ffmpeg writes raw PCM to a
    // pipe with a finite OS buffer (~64KB) — falling behind stalls ffmpeg on
    // write() rather than actually pausing it via SIGSTOP.
    QByteArray raw;
    const auto outcome = ProcessRunner::runPausable(
        ffmpeg, {control.cancelled, control.paused, {}},
        [&] { raw += ffmpeg.readAllStandardOutput(); });
    raw += ffmpeg.readAllStandardOutput(); // final drain after the last poll

    if (outcome == ProcessRunner::RunOutcome::Cancelled) {
        if (errorOut)
            *errorOut = QStringLiteral("Cancelled");
        return std::nullopt;
    }

    if (ffmpeg.exitStatus() != QProcess::NormalExit || ffmpeg.exitCode() != 0) {
        if (errorOut)
            *errorOut = QString::fromUtf8(ffmpeg.readAllStandardError());
        return std::nullopt;
    }

    std::vector<float> pcm(raw.size() / sizeof(float));
    std::memcpy(pcm.data(), raw.constData(), pcm.size() * sizeof(float));
    return pcm;
}

}
