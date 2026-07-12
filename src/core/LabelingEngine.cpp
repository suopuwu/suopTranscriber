#include "LabelingEngine.h"

#include <QDebug>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <llama.h>

#include <vector>

namespace {

// Conservative defaults, not empirically tuned against real multi-hour
// transcripts yet (flagged during design research): Qwen3-4B-Instruct-2507
// has a 262K native context, but resident KV-cache size scales with n_ctx
// and this engine may run alongside a resident whisper.cpp model on the same
// GPU — kContextTokens caps VRAM use rather than using the model's max.
constexpr int kContextTokens = 8192;
// ~4 chars/token is a rough English estimate; leaves headroom in
// kContextTokens for the system prompt, chat-template overhead, and the
// generation budget below.
constexpr int kMaxInputChars = 24000;
constexpr int kMaxOutputTokens = 700;

// Feature 3a ("generate chapters"): a SEPARATE pair of limits from label()'s
// above — the output shape scales with however many chapters the model
// proposes (up to ~20, see kChapterSystemPrompt) rather than one small fixed
// JSON object, so kMaxOutputTokens itself would be far too small. Both
// constants are sized to fit comfortably within kContextTokens together
// (kMaxChapterInputChars is ~4000 tokens at the same ~4 chars/token estimate
// used above, kMaxChapterOutputTokens leaves ~4000 tokens of headroom for
// that plus the system prompt/chat-template overhead) — unlike label(),
// where the tiny fixed output budget makes this non-issue moot.
// kMaxChapterInputChars itself now lives on the class (LabelingEngine.h) —
// item P34 needs TranscriptionController to see it too, to window a long
// transcript across multiple calls instead of relying on the truncation
// below to silently drop everything past one window.
constexpr int kMaxChapterOutputTokens = 3072;

// Fixed key order, not a generic JSON-object grammar — a narrow, schema-exact
// grammar is far more reliable for a small model to satisfy every time than
// a fully generic one. The `string` rule is llama.cpp's own proven one from
// grammars/json.gbnf (third_party/llama.cpp), reused verbatim.
constexpr auto kGrammar = R"GBNF(
root   ::= "{" ws "\"label\"" ws ":" ws string "," ws "\"prelude\"" ws ":" ws string "," ws "\"tags\"" ws ":" ws string ws "}"
string ::= "\"" ( [^"\\\x7F\x00-\x1F] | "\\" (["\\bfnrt] | "u" [0-9a-fA-F]{4}) )* "\""
ws     ::= [ \t\n]*
)GBNF";

// Feature 3a: a JSON array of {afterSegment, title} objects — same "narrow,
// schema-exact over fully generic" philosophy as kGrammar above. `integer`
// is a deliberately NARROW, bounded rule (not the generic float-capable
// `number` rule from grammars/json.gbnf) — "0" or 1-5 digits with no leading
// zero, i.e. 0..99999, far more segments than any realistic transcript has,
// which is all afterSegment ever needs to express.
constexpr auto kChapterGrammar = R"GBNF(
root    ::= "[" ws (chapter (ws "," ws chapter)*)? ws "]"
chapter ::= "{" ws "\"afterSegment\"" ws ":" ws integer "," ws "\"title\"" ws ":" ws string ws "}"
integer ::= "0" | [1-9] [0-9]{0,4}
string  ::= "\"" ( [^"\\\x7F\x00-\x1F] | "\\" (["\\bfnrt] | "u" [0-9a-fA-F]{4}) )* "\""
ws      ::= [ \t\n]*
)GBNF";

// Only the truly-fixed structural parts of the instruction live here — the
// label/prelude phrasing in between is wordiness-dependent and built in
// buildSystemPrompt() below.
const char *kSystemPromptPrefix = "You produce a JSON object describing an audio transcript: ";
const char *kSystemPromptSuffix =
    ", and a comma-separated list of topical tags. Respond with ONLY the JSON object, nothing else.";

// Feature 3a: the user message is the indexed segment list itself (built in
// generateChapters() below, "0: text\n1: text\n..."), so this only needs to
// explain the task and the exact response shape — same register as the
// label/prelude prompt above, phrased for a small instruct model.
const char *kChapterSystemPrompt =
    "You divide an audio transcript into chapters. You are given the transcript as a "
    "numbered list of its segments, one per line, formatted \"INDEX: text\". Propose chapter "
    "boundaries for it — roughly one chapter every few minutes of spoken content, up to about "
    "20 chapters for a long transcript and fewer for a short one; a short transcript covering "
    "one continuous topic may need zero. For each chapter boundary (skip the very first "
    "chapter — it always starts at segment 0 implicitly), return \"afterSegment\": the index of "
    "the LAST segment of the PREVIOUS chapter, so the new chapter begins right after it; and "
    "\"title\": a short (a few words) descriptive title for the chapter that STARTS there. "
    "Respond with ONLY a JSON array of these objects, ordered by afterSegment ascending, "
    "nothing else.";

// Feature 1: 0.0-1.0 slider controlling how verbose the generated label and
// prelude are, independent of the tag-related options below. 3-tier phrasing
// (same threshold style as tagReuseBias below) rather than embedding the raw
// float — a small model follows a clear qualitative instruction more
// reliably than it would calibrate against a bare number. Mid tier matches
// the wording this prompt always used before wordiness/prelude existed.
QString labelPreludePhrasing(double wordiness)
{
    if (wordiness >= 0.67)
        return QStringLiteral("a descriptive label (up to a short phrase), a detailed multi-sentence prelude");
    if (wordiness >= 0.34)
        return QStringLiteral("a short display label (a few words), a one-paragraph prelude");
    return QStringLiteral("a very short label (1-3 words), a single concise prelude sentence");
}

// Item 14 "auto-categorize" mode: restrict tags to an existing vault-wide
// vocabulary instead of freely inventing new ones. This is a prompt
// instruction, not a grammar constraint (see LabelingEngine.h) — the
// vocabulary is open-ended, so it's appended to the base system prompt
// rather than baked into the GBNF grammar.
//
// Item 15 (Wave E): when NOT hard-restricted, existingTags (if any) is still
// shown to the model as context, phrased at one of 3 tiers depending on
// tagReuseBias — a graduated nudge, not a hard rule (that's what
// restrictToExisting is for).
QString buildSystemPrompt(const QStringList &existingTags, bool restrictToExisting, double tagReuseBias,
                           double wordiness)
{
    QString prompt = QString::fromUtf8(kSystemPromptPrefix) + labelPreludePhrasing(wordiness)
        + QString::fromUtf8(kSystemPromptSuffix);
    if (existingTags.isEmpty())
        return prompt;

    const QString tagList = existingTags.join(QStringLiteral(", "));
    if (restrictToExisting) {
        prompt += QStringLiteral(
                       " For tags specifically: choose ONLY from this existing list of tags already used "
                       "elsewhere in this vault, picking whichever apply: %1. If none of them fit this "
                       "transcript, return an empty string for tags — do not invent a new tag.")
                      .arg(tagList);
        return prompt;
    }

    // 3-tier phrasing rather than embedding the raw 0.0-1.0 number in the
    // prompt — a small model follows a clear qualitative instruction more
    // reliably than it would calibrate against a bare float.
    if (tagReuseBias >= 0.67) {
        prompt += QStringLiteral(
                       " For tags specifically: here are tags already used elsewhere in this vault: %1. "
                       "STRONGLY prefer reusing one of these even if it's only an approximate fit — only "
                       "invent a brand new tag if none of the existing ones are remotely relevant.")
                      .arg(tagList);
    } else if (tagReuseBias >= 0.34) {
        prompt += QStringLiteral(
                       " For tags specifically: here are tags already used elsewhere in this vault: %1. "
                       "Reuse one of these if it reasonably fits this transcript, otherwise invent a new, "
                       "precise tag.")
                      .arg(tagList);
    } else {
        prompt += QStringLiteral(
                       " For tags specifically: here are tags already used elsewhere in this vault: %1. "
                       "Prefer inventing a new, precise tag for this transcript over reusing one of these "
                       "unless it's a very close match.")
                      .arg(tagList);
    }
    return prompt;
}

}

bool LabelingEngine::loadModel(const QString &modelPath, int nGpuLayers)
{
    llama_model_params mparams = llama_model_default_params();
    mparams.n_gpu_layers = nGpuLayers;
    m_model = llama_model_load_from_file(modelPath.toUtf8().constData(), mparams);
    if (!m_model)
        return false;

    llama_context_params cparams = llama_context_default_params();
    cparams.n_ctx = kContextTokens;
    cparams.n_batch = kContextTokens;
    m_ctx = llama_init_from_model(m_model, cparams);
    if (!m_ctx) {
        llama_model_free(m_model);
        m_model = nullptr;
        return false;
    }
    return true;
}

LabelingEngine::LabelingEngine(const QString &modelPath)
{
    if (!loadModel(modelPath, /* nGpuLayers */ -1)) {
        qWarning() << "labeling model" << modelPath
                    << "failed to load fully on GPU (likely out of VRAM alongside a resident whisper model) —"
                       " retrying CPU-only";
        loadModel(modelPath, /* nGpuLayers */ 0);
    }
}

LabelingEngine::~LabelingEngine()
{
    if (m_ctx)
        llama_free(m_ctx);
    if (m_model)
        llama_model_free(m_model);
}

std::optional<QString> LabelingEngine::runGrammarConstrained(const QString &systemPrompt, const QString &userText,
                                                               const char *grammar, int maxOutputTokens,
                                                               QString *errorOut)
{
    if (!isValid()) {
        if (errorOut)
            *errorOut = QStringLiteral("labeling engine not initialized");
        return std::nullopt;
    }

    const QByteArray userUtf8 = userText.toUtf8();
    const QByteArray systemUtf8 = systemPrompt.toUtf8();
    const llama_vocab *vocab = llama_model_get_vocab(m_model);

    llama_sampler *grammarSampler = llama_sampler_init_grammar(vocab, grammar, "root");
    if (!grammarSampler) {
        if (errorOut)
            *errorOut = QStringLiteral("failed to parse the generation grammar");
        return std::nullopt;
    }
    llama_sampler *chain = llama_sampler_chain_init(llama_sampler_chain_default_params());
    llama_sampler_chain_add(chain, grammarSampler); // chain now owns grammarSampler
    llama_sampler_chain_add(chain, llama_sampler_init_temp(0.3f));
    llama_sampler_chain_add(chain, llama_sampler_init_dist(LLAMA_DEFAULT_SEED));

    std::vector<llama_chat_message> messages = {
        {"system", systemUtf8.constData()},
        {"user", userUtf8.constData()},
    };
    const char *tmpl = llama_model_chat_template(m_model, /* name */ nullptr);
    std::vector<char> formatted(userUtf8.size() + 4096);
    int len = llama_chat_apply_template(tmpl, messages.data(), messages.size(), /*add_ass=*/true, formatted.data(),
                                        int(formatted.size()));
    if (len > int(formatted.size())) {
        formatted.resize(len);
        len = llama_chat_apply_template(tmpl, messages.data(), messages.size(), true, formatted.data(),
                                         int(formatted.size()));
    }
    if (len < 0) {
        llama_sampler_free(chain);
        if (errorOut)
            *errorOut = QStringLiteral("failed to apply chat template");
        return std::nullopt;
    }
    const std::string prompt(formatted.data(), len);

    // Each call is an independent single-shot generation, not a multi-turn
    // conversation — clear any state left by a previous call (label() or
    // generateChapters(), whichever ran last on this shared context).
    llama_memory_clear(llama_get_memory(m_ctx), true);

    const int nPromptTokens =
        -llama_tokenize(vocab, prompt.c_str(), int(prompt.size()), nullptr, 0, /*add_special=*/true, true);
    std::vector<llama_token> promptTokens(nPromptTokens);
    if (llama_tokenize(vocab, prompt.c_str(), int(prompt.size()), promptTokens.data(), int(promptTokens.size()), true,
                        true)
        < 0) {
        llama_sampler_free(chain);
        if (errorOut)
            *errorOut = QStringLiteral("failed to tokenize prompt");
        return std::nullopt;
    }

    llama_batch batch = llama_batch_get_one(promptTokens.data(), int(promptTokens.size()));
    std::string response;
    const int nCtx = llama_n_ctx(m_ctx);
    bool overflowed = false;
    for (int i = 0; i < maxOutputTokens; ++i) {
        const int nCtxUsed = llama_memory_seq_pos_max(llama_get_memory(m_ctx), 0) + 1;
        if (nCtxUsed + batch.n_tokens > nCtx) {
            overflowed = true;
            break;
        }
        if (llama_decode(m_ctx, batch) != 0) {
            llama_sampler_free(chain);
            if (errorOut)
                *errorOut = QStringLiteral("decode failed during generation");
            return std::nullopt;
        }

        llama_token newToken = llama_sampler_sample(chain, m_ctx, -1);
        if (llama_vocab_is_eog(vocab, newToken))
            break;

        char buf[256];
        const int n = llama_token_to_piece(vocab, newToken, buf, sizeof(buf), 0, true);
        if (n < 0)
            break;
        response.append(buf, n);

        batch = llama_batch_get_one(&newToken, 1);
    }

    llama_sampler_free(chain);

    if (overflowed) {
        if (errorOut)
            *errorOut = QStringLiteral("context size exceeded during generation");
        return std::nullopt;
    }

    return QString::fromStdString(response);
}

std::optional<LabelResult> LabelingEngine::label(const QString &transcriptText, const QStringList &existingTags,
                                                  bool restrictToExisting, double tagReuseBias, double wordiness,
                                                  QString *errorOut)
{
    const QString systemPrompt = buildSystemPrompt(existingTags, restrictToExisting, tagReuseBias, wordiness);
    const auto response =
        runGrammarConstrained(systemPrompt, transcriptText.left(kMaxInputChars), kGrammar, kMaxOutputTokens, errorOut);
    if (!response)
        return std::nullopt;

    const QJsonDocument doc = QJsonDocument::fromJson(response->toUtf8());
    if (!doc.isObject()) {
        if (errorOut)
            *errorOut = QStringLiteral("model did not produce valid JSON: %1").arg(*response);
        return std::nullopt;
    }

    const QJsonObject obj = doc.object();
    LabelResult result;
    result.label = obj.value(QStringLiteral("label")).toString();
    result.prelude = obj.value(QStringLiteral("prelude")).toString();
    result.tags = obj.value(QStringLiteral("tags")).toString();
    return result;
}

std::optional<QList<Chapter>> LabelingEngine::generateChapters(const QStringList &segmentTexts, QString *errorOut)
{
    QString indexed;
    indexed.reserve(segmentTexts.size() * 24);
    for (int i = 0; i < segmentTexts.size(); ++i) {
        indexed += QString::number(i);
        indexed += QStringLiteral(": ");
        indexed += segmentTexts.at(i);
        indexed += QLatin1Char('\n');
    }
    // See generateChapters()'s header doc comment: unlike label()'s silent
    // truncation, flag it when it actually happens here — losing the tail of
    // a long transcript's chapters would otherwise look like a bug, not a
    // documented v1 limit.
    if (indexed.size() > LabelingEngine::kMaxChapterInputChars) {
        indexed.truncate(LabelingEngine::kMaxChapterInputChars);
        qWarning() << "generateChapters: transcript truncated to" << LabelingEngine::kMaxChapterInputChars
                   << "chars — chapters past that point will not be proposed";
    }

    const auto response = runGrammarConstrained(QString::fromUtf8(kChapterSystemPrompt), indexed, kChapterGrammar,
                                                 kMaxChapterOutputTokens, errorOut);
    if (!response)
        return std::nullopt;

    const QJsonDocument doc = QJsonDocument::fromJson(response->toUtf8());
    if (!doc.isArray()) {
        if (errorOut)
            *errorOut = QStringLiteral("model did not produce a valid JSON array: %1").arg(*response);
        return std::nullopt;
    }

    QList<Chapter> chapters;
    const QJsonArray array = doc.array();
    chapters.reserve(array.size());
    for (const QJsonValue &v : array) {
        const QJsonObject obj = v.toObject();
        Chapter chapter;
        chapter.afterSegmentIndex = obj.value(QStringLiteral("afterSegment")).toInt(-1);
        chapter.title = obj.value(QStringLiteral("title")).toString();
        chapters.append(chapter);
    }
    return chapters;
}
