#pragma once

#include <QString>
#include <QUrl>

#include <vector>

// Ensures a whisper.cpp ggml model is present at <vault>/.transcriber/models/,
// downloading it if missing (see DESIGN.md "Model management" / vault layout).
// Speaker detection (tinydiarize) only works with the "-tdrz" fine-tune, which
// is a different model file from a different HF repo, so when speakerDetection
// is set the tdrz model is used regardless of the requested modelId. That is
// an intentional, documented override — NOT the fallback below.
//
// If modelId is not one of availableModels()'s ids (e.g. a stale
// AppSettings.lastModel from a removed model tier), ensureModel silently
// *would* have resolved to kDefaultModelId with no way for the caller to
// know a substitution happened — the caller would believe the requested
// model was used. To avoid ever misrepresenting which model actually ran,
// pass usedFallback: it is set to true exactly when this default-substitution
// occurs (never set for the speakerDetection/tdrz override, which isn't a
// fallback). Callers that persist the result (see TranscriptionController's
// modelFallback/modelRequested frontmatter fields) should surface this to the
// user rather than only logging it.
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
// usedFallback, if given, is set to true/false depending on whether an
// unrecognized modelId forced a default substitution (see class comment).
QString ensureModel(const QUrl &vaultPath, const QString &modelId, bool speakerDetection, QString *errorOut = nullptr,
                     bool *usedFallback = nullptr);

// The GGUF model used for AI auto-labeling (see DESIGN.md "Model management" —
// LabelingEngine's second pass after transcription). One hardcoded model for
// now, same vault-scoped storage/download pattern as ensureModel() above, no
// per-size picker — auto-labeling is a single opt-in setting, not a dropdown.
constexpr auto kLabelModelFileName = "Qwen3-4B-Instruct-2507-Q4_K_M.gguf";

// Returns the local label-model path, or empty on failure (errorOut set if
// given). Downloads on first use (~2.5 GB); Q_INVOKABLE callers must run this
// off the GUI thread, same as ensureModel().
QString ensureLabelModel(const QUrl &vaultPath, QString *errorOut = nullptr);

}
