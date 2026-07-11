#include "AudioDecoder.h"

#include <QProcess>
#include <cstring>

#include <csignal>
#include <sys/types.h>
#include <unistd.h>

namespace AudioDecoder {

std::optional<std::vector<float>> decodeTo16kMono(const QString &filePath, QString *errorOut,
                                                    const DecodeControl &control)
{
    QProcess ffmpeg;
    ffmpeg.start("ffmpeg", {
        "-i", filePath,
        "-ar", "16000",
        "-ac", "1",
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

    // 100ms poll (vs. a plain blocking drain) so control.paused/cancelled
    // are checked often enough to feel instant — same cadence as
    // ModelManager::downloadToModelsDir()'s curl loop. Draining stdout every
    // iteration regardless of pause state is required, not optional: ffmpeg
    // writes raw PCM to a pipe with a finite OS buffer (typically 64KB) —
    // falling behind stalls ffmpeg on write() rather than actually pausing
    // it cleanly via SIGSTOP.
    QByteArray raw;
    bool wasPaused = false;
    bool cancelled = false;
    while (ffmpeg.state() != QProcess::NotRunning) {
        if (control.cancelled && control.cancelled()) {
            cancelled = true;
            ffmpeg.kill();
            ffmpeg.waitForFinished(2000);
            break;
        }
        const bool shouldPause = control.paused && control.paused();
        if (shouldPause && !wasPaused) {
            ::kill(static_cast<pid_t>(ffmpeg.processId()), SIGSTOP);
            wasPaused = true;
        } else if (!shouldPause && wasPaused) {
            ::kill(static_cast<pid_t>(ffmpeg.processId()), SIGCONT);
            wasPaused = false;
        }
        ffmpeg.waitForReadyRead(100);
        raw += ffmpeg.readAllStandardOutput();
    }
    raw += ffmpeg.readAllStandardOutput();

    if (cancelled) {
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
