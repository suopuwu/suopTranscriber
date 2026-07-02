#pragma once

#include <QString>
#include <QUrl>

#include <vector>

// Ensures a whisper.cpp ggml model is present at <vault>/.transcriber/models/,
// downloading it if missing (see DESIGN.md "Model management" / vault layout).
// Speaker detection (tinydiarize) only works with the "-tdrz" fine-tune, which
// is a different model file from a different HF repo, so when speakerDetection
// is set the tdrz model is used regardless of the requested modelId.
// Blocking — call off the GUI thread.
namespace ModelManager {

struct ModelInfo {
    QString id;       // stable identifier, also the AppSettings.lastModel value
    QString fileName; // ggml file name on huggingface.co/ggerganov/whisper.cpp
    QString label;    // human-readable, shown in the model dropdown
};

// Ordered fastest/smallest to slowest/most-accurate — dropdown display order.
const std::vector<ModelInfo> &availableModels();

constexpr auto kDefaultModelId = "base.en";
constexpr auto kTdrzModelName = "ggml-small.en-tdrz.bin";

// Returns the local model path, or empty on failure (errorOut set if given).
QString ensureModel(const QUrl &vaultPath, const QString &modelId, bool speakerDetection, QString *errorOut = nullptr);

}
