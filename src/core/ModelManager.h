#pragma once

#include <QString>
#include <QUrl>

// Ensures a whisper.cpp ggml model is present at <vault>/.transcriber/models/,
// downloading it if missing (see DESIGN.md "Model management" / vault layout).
// Minimal for now: one hardcoded model per mode (a full model-registry/picker
// UI is a later milestone) — this just needs *a* model on disk. Speaker
// detection (tinydiarize) only works with the "-tdrz" fine-tune, which is a
// different model file from a different HF repo, so the model actually used
// is derived from the speakerDetection flag rather than freely chosen.
// Blocking — call off the GUI thread.
namespace ModelManager {

constexpr auto kModelName = "ggml-base.en.bin";
constexpr auto kTdrzModelName = "ggml-small.en-tdrz.bin";

// Returns the local model path, or empty on failure (errorOut set if given).
QString ensureModel(const QUrl &vaultPath, bool speakerDetection, QString *errorOut = nullptr);

}
