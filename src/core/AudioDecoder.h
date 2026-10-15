#pragma once

#include <QString>
#include <QStringList>

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

// Non-finite (NaN/Inf) samples scrubbed to silence during decode — see
// decodeTo16kMono()'s doc comment for why they can't just pass through.
// Default-constructed means "none found". nonFiniteSampleCount and
// firstNonFiniteSeconds (post-resample, 16kHz-relative) are kept even though
// the scrub always proceeds, so a caller can surface WHERE a file was
// corrupt rather than silently producing a gap nothing explains.
struct DecodeDiagnostics {
    std::size_t nonFiniteSampleCount = 0;
    std::optional<double> firstNonFiniteSeconds;
};

std::optional<std::vector<float>> decodeTo16kMono(const QString &filePath, QString *errorOut = nullptr,
                                                    const DecodeControl &control = {},
                                                    DecodeDiagnostics *diagnosticsOut = nullptr);

// Decode output is CONTINUOUS — ffmpeg streams raw PCM the whole time it works,
// so unlike the engine's per-window silence (TranscribeTask::isStalled) a decode
// that has emitted nothing for this long is wedged regardless of file length.
// Generous enough to absorb a slow disk or a paused-then-resumed process.
bool isDecodeStalled(qint64 noOutputElapsedMs);

// Runs ffprobe on `path` with the given extra arguments and returns trimmed
// stdout, or nullopt on any failure (missing binary, non-zero exit, timeout).
// The app's one ffprobe spawn site, shared by every caller that probes.
std::optional<QString> runFfprobe(const QString &path, const QStringList &args);

// A media file's duration in seconds, or nullopt on any failure. Callers treat
// "couldn't check" as a validation failure rather than skipping the check:
// ffprobe ships in the same package as ffmpeg, which the app hard-requires.
std::optional<double> probeDurationSeconds(const QString &path);

}
