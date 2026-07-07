#pragma once

#include <QString>

#include <optional>

struct llama_model;
struct llama_context;

// AI auto-labeling (DESIGN.md "Model management" — an opt-in second pass
// after transcription completes, in-process like WhisperCppEngine). One
// instance keeps the model resident (warm) across label() calls.
//
// tags is a comma-separated string, not a real list — TranscriptionController's
// frontmatter parser only understands flat "key: value" scalars, not YAML
// lists (see readMetadata()), so a real list would silently corrupt on
// read-back.
struct LabelResult {
    QString label;
    QString summary;
    QString tags;
};

class LabelingEngine {
public:
    explicit LabelingEngine(const QString &modelPath);
    ~LabelingEngine();

    LabelingEngine(const LabelingEngine &) = delete;
    LabelingEngine &operator=(const LabelingEngine &) = delete;

    bool isValid() const { return m_model != nullptr && m_ctx != nullptr; }

    // Single GBNF-grammar-constrained generation pass producing
    // {label, summary, tags} from transcriptText, using the model's own
    // embedded chat template. Returns std::nullopt on failure (errorOut set
    // if given — grammar/tokenize/decode error, context overflow, or invalid
    // engine). transcriptText longer than kMaxInputChars (see .cpp) is
    // truncated before prompting — a conservative default, not empirically
    // tuned against real multi-hour transcripts yet (see DESIGN.md notes on
    // this feature).
    std::optional<LabelResult> label(const QString &transcriptText, QString *errorOut = nullptr);

private:
    llama_model *m_model = nullptr;
    llama_context *m_ctx = nullptr;
};
