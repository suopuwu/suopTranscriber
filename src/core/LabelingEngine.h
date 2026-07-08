#pragma once

#include <QString>
#include <QStringList>

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
    //
    // existingTags (item 14 "auto-categorize" mode): when non-empty AND
    // restrictToExisting is true, the model is instructed to pick tags ONLY
    // from this vocabulary (or leave tags empty if none fit) instead of
    // freely inventing new ones. This is a PROMPT instruction, not a grammar
    // constraint — the vocabulary is open-ended/vault-specific, so
    // hand-building a GBNF alternation over an arbitrary, growing tag list
    // isn't practical; the grammar still only constrains the overall JSON
    // shape, same as the unrestricted mode.
    //
    // tagReuseBias (Item 15, Wave E): when existingTags is non-empty but
    // restrictToExisting is false, this 0.0-1.0 slider softly nudges the
    // model's choice between inventing a new/precise tag (near 0.0) and
    // reusing one of the existing tags even if the fit is only approximate
    // (near 1.0) — a graduated preference, not a hard rule. Ignored when
    // existingTags is empty (nothing to bias toward) or restrictToExisting
    // is true (that's already a hard constraint, not a bias).
    std::optional<LabelResult> label(const QString &transcriptText, const QStringList &existingTags = {},
                                      bool restrictToExisting = false, double tagReuseBias = 0.5,
                                      QString *errorOut = nullptr);

private:
    llama_model *m_model = nullptr;
    llama_context *m_ctx = nullptr;
};
