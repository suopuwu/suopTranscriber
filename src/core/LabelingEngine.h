#pragma once

#include <QList>
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
    QString prelude;
    QString tags;
};

// Feature 3a ("generate chapters"): one proposed chapter boundary from
// generateChapters() below. afterSegmentIndex is the index (into the
// caller's own segmentTexts list — see generateChapters()'s doc comment) of
// the segment the new chapter starts right AFTER, i.e. this chapter's
// content begins at segment afterSegmentIndex + 1. Deliberately NOT a
// timestamp: this app never trusts the LLM to echo one back accurately (see
// generateChapters()'s doc comment) — the caller resolves this index to the
// segment's own precisely-known t0 itself, before anything touches disk.
struct Chapter {
    int afterSegmentIndex = -1;
    QString title;
};

class LabelingEngine {
public:
    // Tries GPU offload first (fast path); if that fails — e.g. a resident
    // whisper.cpp model has already claimed most of the GPU's VRAM, see
    // ggml-vulkan's grow-only working buffers — retries fully on CPU
    // (slower, but works under VRAM pressure that would otherwise make
    // labeling an all-or-nothing failure) before giving up. isValid() is
    // false only if BOTH attempts failed.
    explicit LabelingEngine(const QString &modelPath);
    ~LabelingEngine();

    LabelingEngine(const LabelingEngine &) = delete;
    LabelingEngine &operator=(const LabelingEngine &) = delete;

    bool isValid() const { return m_model != nullptr && m_ctx != nullptr; }

    // Single GBNF-grammar-constrained generation pass producing
    // {label, prelude, tags} from transcriptText, using the model's own
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
    //
    // wordiness (Feature 1): 0.0-1.0 slider controlling how verbose the
    // generated label and prelude are, independent of the tag-related
    // options above. Low (<0.34): a very short label, a single concise
    // prelude sentence. Mid (default, 0.34-0.67): a short label (a few
    // words), a one-paragraph prelude. High (>=0.67): a descriptive label
    // (up to a short phrase), a detailed multi-sentence prelude.
    std::optional<LabelResult> label(const QString &transcriptText, const QStringList &existingTags = {},
                                      bool restrictToExisting = false, double tagReuseBias = 0.5,
                                      double wordiness = 0.5, QString *errorOut = nullptr);

    // Feature 3a ("generate chapters"): a SEPARATE grammar-constrained pass
    // sharing this SAME resident model/context as label() above — never a
    // second LabelingEngine instance (would double-load a ~2.5GB GGUF
    // alongside whisper.cpp's own resident model, a hard VRAM/RAM
    // constraint, not a style preference).
    //
    // Robustness: this app never trusts the LLM to echo back accurate raw
    // timestamps. segmentTexts[i] must be segment i's text (cheap for the
    // caller to build from its own already-parsed segment list — see
    // TranscriptionController's parsedSegmentsFromTranscriptBody()); the
    // model only ever sees text, indexed by position ("0: ...", "1: ...",
    // built internally from segmentTexts), and returns which index each
    // proposed chapter starts after. The caller is responsible for
    // resolving that index back to the segment's own precisely-known t0 —
    // and for bound-checking/deduping the returned indices against
    // segmentTexts.size() — before anything touches disk; this method
    // returns whatever the model produced, unvalidated against
    // segmentTexts.size(), the same way label() never validates its own
    // JSON's semantic content either (the grammar only constrains shape).
    //
    // segmentTexts longer than kMaxChapterInputChars (see .cpp) is
    // truncated before prompting — same truncate-and-document approach
    // label() uses for kMaxInputChars, but (unlike label()'s silent
    // truncation) this logs a qWarning when truncation actually happens: a
    // multi-hour file silently only getting chapters for its first stretch
    // would read as a bug, not a documented limit.
    //
    // Returns std::nullopt on failure (errorOut set if given).
    std::optional<QList<Chapter>> generateChapters(const QStringList &segmentTexts, QString *errorOut = nullptr);

    // Item P34: the same limit generateChapters() truncates to internally
    // (see its doc comment above and the .cpp definition site), exposed so
    // TranscriptionController::generateChaptersJob() can split a long
    // transcript into multiple windows — each within this budget — and call
    // generateChapters() once per window instead of silently losing
    // everything past this many characters in a single truncated pass.
    static constexpr int kMaxChapterInputChars = 16000;

private:
    // Attempts to load modelPath fully offloaded to GPU (nGpuLayers=-1) or
    // CPU-only (nGpuLayers=0) — see LabelingEngine()'s doc comment for why
    // the constructor tries both. Leaves m_model/m_ctx null (and any
    // partially-created one freed) on failure.
    bool loadModel(const QString &modelPath, int nGpuLayers);

    // Shared grammar-constrained single-shot generation core for label() and
    // generateChapters() above — builds the chat-templated prompt from
    // systemPrompt+userText, decodes under grammar's constraint up to
    // maxOutputTokens, and returns the raw generated text (unparsed). Both
    // callers parse the JSON shape themselves (an object for label(), an
    // array for generateChapters()) since that's the one thing that
    // actually differs between them — everything else (chat template,
    // grammar sampler wiring, context-overflow/decode-failure handling) is
    // identical and was previously duplicated between the two.
    std::optional<QString> runGrammarConstrained(const QString &systemPrompt, const QString &userText,
                                                  const char *grammar, int maxOutputTokens, QString *errorOut);

    llama_model *m_model = nullptr;
    llama_context *m_ctx = nullptr;
};
