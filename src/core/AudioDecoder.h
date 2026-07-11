#pragma once

#include <QString>

#include <functional>
#include <optional>
#include <vector>

// Decodes an arbitrary audio file to 16kHz mono float32 PCM via ffmpeg,
// piped straight into memory — no temp WAV (see DESIGN.md "Audio: decode vs
// playback"). Playback uses Qt Multimedia on the original file separately;
// this is transcription input only. Blocking — call off the GUI thread.
namespace AudioDecoder {

// Cooperative pause/cancel for the ffmpeg subprocess this spawns — same
// SIGSTOP/SIGCONT-pause, kill()-cancel pattern as ModelManager's
// DownloadControl (see downloadToModelsDir()'s doc comment): both are polled
// from the same loop that's already draining ffmpeg's stdout pipe, so an
// unsuspendable in-process library call is never involved. Default
// (both null) means never pause or cancel.
struct DecodeControl {
    std::function<bool()> paused;
    std::function<bool()> cancelled;
};

std::optional<std::vector<float>> decodeTo16kMono(const QString &filePath, QString *errorOut = nullptr,
                                                    const DecodeControl &control = {});

}
