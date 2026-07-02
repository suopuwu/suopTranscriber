#include "AudioDecoder.h"

#include <QProcess>
#include <cstring>

namespace AudioDecoder {

std::optional<std::vector<float>> decodeTo16kMono(const QString &filePath, QString *errorOut)
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

    QByteArray raw;
    while (ffmpeg.state() != QProcess::NotRunning) {
        ffmpeg.waitForReadyRead(1000);
        raw += ffmpeg.readAllStandardOutput();
    }
    raw += ffmpeg.readAllStandardOutput();

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
