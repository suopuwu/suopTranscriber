#pragma once

#include <QString>

#include <optional>
#include <vector>

// Decodes an arbitrary audio file to 16kHz mono float32 PCM via ffmpeg,
// piped straight into memory — no temp WAV (see DESIGN.md "Audio: decode vs
// playback"). Playback uses Qt Multimedia on the original file separately;
// this is transcription input only. Blocking — call off the GUI thread.
namespace AudioDecoder {

std::optional<std::vector<float>> decodeTo16kMono(const QString &filePath, QString *errorOut = nullptr);

}
