#include "LabelingEngine.h"

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
constexpr int kMaxOutputTokens = 400;

// Fixed key order, not a generic JSON-object grammar — a narrow, schema-exact
// grammar is far more reliable for a small model to satisfy every time than
// a fully generic one. The `string` rule is llama.cpp's own proven one from
// grammars/json.gbnf (third_party/llama.cpp), reused verbatim.
constexpr auto kGrammar = R"GBNF(
root   ::= "{" ws "\"label\"" ws ":" ws string "," ws "\"summary\"" ws ":" ws string "," ws "\"tags\"" ws ":" ws string ws "}"
string ::= "\"" ( [^"\\\x7F\x00-\x1F] | "\\" (["\\bfnrt] | "u" [0-9a-fA-F]{4}) )* "\""
ws     ::= [ \t\n]*
)GBNF";

const char *kSystemPrompt =
    "You produce a JSON object describing an audio transcript: a short "
    "display label (a few words), a one-paragraph summary, and a "
    "comma-separated list of topical tags. Respond with ONLY the JSON "
    "object, nothing else.";

}

LabelingEngine::LabelingEngine(const QString &modelPath)
{
    llama_model_params mparams = llama_model_default_params();
    m_model = llama_model_load_from_file(modelPath.toUtf8().constData(), mparams);
    if (!m_model)
        return;

    llama_context_params cparams = llama_context_default_params();
    cparams.n_ctx = kContextTokens;
    cparams.n_batch = kContextTokens;
    m_ctx = llama_init_from_model(m_model, cparams);
    if (!m_ctx) {
        llama_model_free(m_model);
        m_model = nullptr;
    }
}

LabelingEngine::~LabelingEngine()
{
    if (m_ctx)
        llama_free(m_ctx);
    if (m_model)
        llama_model_free(m_model);
}

std::optional<LabelResult> LabelingEngine::label(const QString &transcriptText, QString *errorOut)
{
    if (!isValid()) {
        if (errorOut)
            *errorOut = QStringLiteral("labeling engine not initialized");
        return std::nullopt;
    }

    const QByteArray userUtf8 = transcriptText.left(kMaxInputChars).toUtf8();
    const llama_vocab *vocab = llama_model_get_vocab(m_model);

    llama_sampler *grammarSampler = llama_sampler_init_grammar(vocab, kGrammar, "root");
    if (!grammarSampler) {
        if (errorOut)
            *errorOut = QStringLiteral("failed to parse the labeling grammar");
        return std::nullopt;
    }
    llama_sampler *chain = llama_sampler_chain_init(llama_sampler_chain_default_params());
    llama_sampler_chain_add(chain, grammarSampler); // chain now owns grammarSampler
    llama_sampler_chain_add(chain, llama_sampler_init_temp(0.3f));
    llama_sampler_chain_add(chain, llama_sampler_init_dist(LLAMA_DEFAULT_SEED));

    std::vector<llama_chat_message> messages = {
        {"system", kSystemPrompt},
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

    // Each label() call is an independent single-shot generation, not a
    // multi-turn conversation — clear any state left by a previous call.
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
    for (int i = 0; i < kMaxOutputTokens; ++i) {
        const int nCtxUsed = llama_memory_seq_pos_max(llama_get_memory(m_ctx), 0) + 1;
        if (nCtxUsed + batch.n_tokens > nCtx) {
            overflowed = true;
            break;
        }
        if (llama_decode(m_ctx, batch) != 0) {
            llama_sampler_free(chain);
            if (errorOut)
                *errorOut = QStringLiteral("decode failed during labeling");
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
            *errorOut = QStringLiteral("context size exceeded during labeling");
        return std::nullopt;
    }

    const QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(response));
    if (!doc.isObject()) {
        if (errorOut)
            *errorOut =
                QStringLiteral("model did not produce valid JSON: %1").arg(QString::fromStdString(response));
        return std::nullopt;
    }

    const QJsonObject obj = doc.object();
    LabelResult result;
    result.label = obj.value(QStringLiteral("label")).toString();
    result.summary = obj.value(QStringLiteral("summary")).toString();
    result.tags = obj.value(QStringLiteral("tags")).toString();
    return result;
}
