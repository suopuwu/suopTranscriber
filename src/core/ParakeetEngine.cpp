#include "ParakeetEngine.h"

#include <QDebug>

#include <cctype>
#include <string>
#include <vector>

#include <sherpa-onnx/c-api/cxx-api.h>

namespace sc = sherpa_onnx::cxx;

namespace {

constexpr int kSampleRate = 16000;

// Parakeet's exported conformer uses relative-position self-attention whose
// position buffer covers 5000 encoder frames — at the model's 12.5 frames/s
// (8x subsampling of a 10 ms mel hop) that's 400 s. A longer window makes
// onnxruntime throw at decode ("... Add node '/layers.0/self_attn/Add_2':
// right operand cannot broadcast ... {1,8,N,N} vs {1,8,N,N-5000}"), because
// sherpa-onnx's offline transducer decodes the whole window in ONE pass with
// no internal chunking (unlike whisper.cpp, which VAD-chunks internally). So
// transcribe() splits any longer input into sub-windows itself. 300 s leaves a
// wide margin under the 400 s ceiling (even after pause-seeking nudges the
// boundary by up to kBoundarySearchSamples) and matches the app's VAD target
// window length.
constexpr std::size_t kMaxChunkSamples = static_cast<std::size_t>(300.0 * kSampleRate);

// The window length transcribe() actually decodes at — deliberately far below
// the kMaxChunkSamples *ceiling*. sherpa-onnx's offline transducer only calls
// onSegment after a whole Decode() returns, so the decode granularity is also
// the LIVE-UPDATE granularity: at the 300 s ceiling a typical (sub-5-minute)
// file decodes in one blocking pass and its entire transcript lands in a single
// burst at the end — nothing streams, nothing scrolls-to-follow until it's done
// (unlike whisper.cpp, which emits segments continuously mid-decode). Decoding
// in ~30 s pieces instead makes a window's worth of segments appear roughly
// every second of wall-clock (Parakeet's RTF is well under 1), so the
// transcript grows and follows visibly. 30 s is a standard NeMo long-audio
// chunk size and stays a full utterance's worth of context, so accuracy is
// unaffected in practice; the cost is one extra OfflineStream Decode() per 30 s
// (cheap) and, like the ceiling split, a boundary that pause-seeking lands in a
// natural gap (quietestBoundary) rather than mid-word. Tune here if the
// streaming feels too coarse (smaller) or the per-window overhead shows on very
// long files (larger).
constexpr std::size_t kLiveChunkSamples = static_cast<std::size_t>(30.0 * kSampleRate);
static_assert(kLiveChunkSamples <= kMaxChunkSamples,
              "the decode granularity must stay under the attention ceiling or onnxruntime throws");

// A sub-window boundary is nudged onto the quietest short frame within this
// radius of its nominal position, so a split lands in a natural pause instead
// of mid-word. 8 s each way is generous enough to find a real gap without
// letting any sub-window grow anywhere near the 400 s ceiling.
constexpr std::size_t kBoundarySearchSamples = static_cast<std::size_t>(8.0 * kSampleRate);

// 20 ms energy frame used by quietestBoundary().
constexpr std::size_t kEnergyFrameSamples = 320;

// Index (a frame start) within +/-searchSamples of `nominal` with the lowest
// short-frame energy — the quietest nearby place to cut. Kept strictly after
// `low` (the previous boundary) so sub-windows never collapse to empty. Pure.
std::size_t quietestBoundary(const std::vector<float> &pcm, std::size_t nominal, std::size_t searchSamples,
                             std::size_t low)
{
    const std::size_t total = pcm.size();
    std::size_t lo = (nominal > searchSamples) ? nominal - searchSamples : 0;
    if (lo <= low)
        lo = low + 1;
    const std::size_t hi = std::min(total, nominal + searchSamples);
    if (lo + kEnergyFrameSamples > hi)
        return std::min(std::max(nominal, low + 1), total);

    std::size_t best = nominal;
    double bestEnergy = -1.0;
    for (std::size_t i = lo; i + kEnergyFrameSamples <= hi; i += kEnergyFrameSamples) {
        double energy = 0.0;
        for (std::size_t j = i; j < i + kEnergyFrameSamples; ++j)
            energy += static_cast<double>(pcm[j]) * static_cast<double>(pcm[j]);
        if (bestEnergy < 0.0 || energy < bestEnergy) {
            bestEnergy = energy;
            best = i;
        }
    }
    return best;
}

// SentencePiece word-boundary marker (U+2581 "▁") that prefixes the first
// subword token of each word in Parakeet's BPE tokens.txt.
constexpr char kWordBoundary[] = "\xe2\x96\x81";

sc::OfflineRecognizerConfig buildConfig(const QString &encoder, const QString &decoder, const QString &joiner,
                                        const QString &tokens, const char *provider)
{
    sc::OfflineRecognizerConfig config;
    config.model_config.transducer.encoder = encoder.toStdString();
    config.model_config.transducer.decoder = decoder.toStdString();
    config.model_config.transducer.joiner = joiner.toStdString();
    config.model_config.tokens = tokens.toStdString();
    // REQUIRED for the k2-fsa NeMo Parakeet export — selects the NeMo log-mel
    // feature normalization; plain "transducer" produces garbage. sherpa-onnx
    // does the mel-feature extraction internally (feat defaults 16 kHz / 80
    // dim already match this model), so the engine feeds raw 16 kHz mono PCM.
    config.model_config.model_type = "nemo_transducer";
    config.model_config.num_threads = 2;
    config.model_config.provider = provider;
    config.decoding_method = "greedy_search";
    return config;
}

// Builds the recognizer, trying CUDA first then CPU — same graceful try-CUDA-
// then-CPU shape as DiarizationEngine (a missing/mismatched CUDA runtime must
// fall back rather than leave the engine unavailable). *backendName records
// which provider actually loaded. Returns nullptr if neither works (a bad/
// missing model file fails the same way under both, cheaply).
std::unique_ptr<sc::OfflineRecognizer> createSafely(const QString &encoder, const QString &decoder,
                                                     const QString &joiner, const QString &tokens,
                                                     QString *backendName)
{
    const auto attempt = [&](const char *provider) -> std::unique_ptr<sc::OfflineRecognizer> {
        try {
            sc::OfflineRecognizer recognizer =
                sc::OfflineRecognizer::Create(buildConfig(encoder, decoder, joiner, tokens, provider));
            if (recognizer.Get() != nullptr)
                return std::make_unique<sc::OfflineRecognizer>(std::move(recognizer));
        } catch (const std::exception &e) {
            qWarning() << "ParakeetEngine:" << provider << "init failed (" << e.what() << ")";
        }
        return nullptr;
    };

    if (auto cuda = attempt("cuda")) {
        if (backendName)
            *backendName = QStringLiteral("CUDA");
        return cuda;
    }
    qWarning() << "ParakeetEngine: CUDA unavailable — falling back to CPU";
    if (auto cpu = attempt("cpu")) {
        if (backendName)
            *backendName = QStringLiteral("CPU");
        return cpu;
    }
    return nullptr;
}

bool endsSentence(const std::string &word)
{
    for (auto it = word.rbegin(); it != word.rend(); ++it) {
        const unsigned char c = static_cast<unsigned char>(*it);
        if (std::isspace(c))
            continue;
        if (c == '"' || c == '\'' || c == ')' || c == ']')
            continue; // closing quote/bracket after the terminator
        return c == '.' || c == '!' || c == '?';
    }
    return false;
}

} // namespace

// See the header for the contract. Kept sherpa-free (plain token/timestamp
// vectors) so it's unit-testable without a loaded model.
std::vector<Segment> ParakeetEngine::assembleSentenceSegments(const std::vector<std::string> &tokens,
                                                              const std::vector<float> &timestamps,
                                                              const std::string &fullText)
{
    struct Word {
        double t;
        std::string text;
    };
    std::vector<Word> words;
    const std::size_t boundaryLen = std::string(kWordBoundary).size();

    for (std::size_t i = 0; i < tokens.size(); ++i) {
        const std::string &tok = tokens[i];
        const double ts =
            i < timestamps.size() ? static_cast<double>(timestamps[i]) : (words.empty() ? 0.0 : words.back().t);
        // A word begins at the SentencePiece boundary marker; sherpa-onnx
        // decodes Parakeet's "▁" (U+2581) to a leading ASCII space in the token
        // it returns, so the real per-token output is " Help"/"el"/"p"/" me" —
        // NOT the raw "▁Help" form. Accept either marker (space or "▁"): without
        // this every continuation subword folds into one giant "word" and the
        // whole utterance collapses to a single un-splittable segment. Trailing
        // subwords (no leading marker) attach to the current word.
        std::size_t markerLen = 0;
        if (tok.rfind(kWordBoundary, 0) == 0)
            markerLen = boundaryLen;
        else if (!tok.empty() && tok.front() == ' ')
            markerLen = 1;

        if (markerLen > 0)
            words.push_back({ts, tok.substr(markerLen)});
        else if (words.empty())
            words.push_back({ts, tok});
        else
            words.back().text += tok;
    }

    std::vector<Segment> segments;
    if (words.empty()) {
        // No tokens (shouldn't happen for a transducer) — fall back to the
        // whole utterance as one segment so text isn't lost.
        if (!fullText.empty())
            segments.push_back({0.0, 0.0, fullText});
        return segments;
    }

    std::size_t start = 0;
    const auto flush = [&](std::size_t endExclusive) {
        if (endExclusive <= start)
            return;
        std::string text;
        for (std::size_t k = start; k < endExclusive; ++k) {
            if (!text.empty())
                text += ' ';
            text += words[k].text;
        }
        segments.push_back({words[start].t, words[endExclusive - 1].t, text});
        start = endExclusive;
    };

    for (std::size_t j = 0; j < words.size(); ++j) {
        if (endsSentence(words[j].text))
            flush(j + 1);
    }
    flush(words.size()); // trailing words with no terminal punctuation

    // t1 == next segment's t0 (last keeps its final-word onset).
    for (std::size_t s = 0; s + 1 < segments.size(); ++s)
        segments[s].t1 = segments[s + 1].t0;
    return segments;
}

// See the header for the contract. Kept sherpa-free so it's unit-testable on a
// synthetic buffer (tst_parakeetengine).
std::vector<std::pair<std::size_t, std::size_t>> ParakeetEngine::subChunkRanges(const std::vector<float> &pcm,
                                                                                std::size_t maxSamples,
                                                                                std::size_t searchSamples)
{
    std::vector<std::pair<std::size_t, std::size_t>> ranges;
    const std::size_t total = pcm.size();
    if (total == 0)
        return ranges;
    if (maxSamples == 0 || total <= maxSamples) {
        ranges.push_back({0, total});
        return ranges;
    }
    // ceil(total / maxSamples) near-equal windows — a 450 s input becomes
    // 2x225 s, not 300 s + 150 s (balanced windows decode more evenly and
    // avoid a tiny tail). Each nominal window is <= maxSamples; pause-seeking
    // then nudges interior boundaries by at most searchSamples.
    const std::size_t count = (total + maxSamples - 1) / maxSamples;
    std::size_t start = 0;
    for (std::size_t k = 1; k <= count; ++k) {
        std::size_t end = total;
        if (k < count) {
            const std::size_t nominalEnd = total * k / count;
            end = quietestBoundary(pcm, nominalEnd, searchSamples, start);
        }
        if (end <= start) // safety: keep boundaries strictly increasing
            end = std::min(total, start + maxSamples);
        ranges.push_back({start, end});
        start = end;
    }
    return ranges;
}

ParakeetEngine::ParakeetEngine(const QString &encoderPath, const QString &decoderPath, const QString &joinerPath,
                               const QString &tokensPath)
{
    // Assign in the BODY, not the member-init list: createSafely() writes
    // *backendName (=&m_backendName), so m_backendName must already be
    // constructed when it runs. Members initialize in DECLARATION order, and
    // m_recognizer is declared first — so initializing m_recognizer from
    // createSafely(..., &m_backendName) in the init list would write into
    // m_backendName's still-uninitialized storage. On a fresh (zeroed) heap
    // that happens to no-op; on a long-running process's recycled heap the
    // stale QString::d pointer is garbage and the write crashes (and even when
    // it didn't crash, m_backendName's later default-construct discarded the
    // detected "CUDA"/"CPU" name). Both members are fully constructed here.
    m_recognizer = createSafely(encoderPath, decoderPath, joinerPath, tokensPath, &m_backendName);
}

ParakeetEngine::~ParakeetEngine() = default;

bool ParakeetEngine::isValid() const
{
    return m_recognizer && m_recognizer->Get() != nullptr;
}

std::vector<Segment> ParakeetEngine::transcribe(const AudioChunk &chunk, const ProgressCallback &onProgress,
                                                 const SegmentCallback &onSegment, const AbortCallback &onAbort)
{
    std::vector<Segment> segments;
    m_decodeFailures = 0;
    if (!isValid() || chunk.pcm16kMono.empty())
        return segments;
    // No mid-decode abort hook (Decode() is one blocking call) — honor a cancel
    // requested before a sub-window starts; mid-Decode cancels are the caller's
    // job, and the sub-windowing below keeps each Decode() bounded.
    if (onAbort && onAbort())
        return segments;

    const std::vector<float> &pcm = chunk.pcm16kMono;
    const double totalSeconds = static_cast<double>(pcm.size()) / kSampleRate;
    if (onProgress)
        onProgress(0.0, totalSeconds);

    // sherpa-onnx's offline transducer decodes each window in a single pass with
    // no internal chunking, emitting this window's segments only once Decode()
    // returns — so the split here sets BOTH the attention-ceiling safety (never
    // exceed kMaxChunkSamples, or onnxruntime throws) AND the live-update
    // cadence. Splitting at kLiveChunkSamples (30 s, well under the ceiling) is
    // what makes segments stream in gradually instead of the whole transcript
    // landing in one end-of-file burst — see kLiveChunkSamples' doc comment.
    // Inputs shorter than one live chunk yield exactly one range: unchanged
    // single-Decode path.
    const std::vector<std::pair<std::size_t, std::size_t>> ranges =
        subChunkRanges(pcm, kLiveChunkSamples, kBoundarySearchSamples);

    for (const auto &range : ranges) {
        if (onAbort && onAbort())
            break;
        const std::size_t startSample = range.first;
        const std::size_t endSample = range.second;
        const double subOffset = static_cast<double>(startSample) / kSampleRate;

        sc::OfflineRecognizerResult result;
        try {
            sc::OfflineStream stream = m_recognizer->CreateStream();
            stream.AcceptWaveform(kSampleRate, pcm.data() + startSample,
                                   static_cast<int32_t>(endSample - startSample));
            m_recognizer->Decode(&stream);
            result = m_recognizer->GetResult(&stream);
        } catch (const std::exception &e) {
            // Same never-crash-the-app degradation as DiarizationEngine: a decode
            // failure drops this sub-window (not the whole file, not the process).
            // Counted (not just logged) so the caller can tell an incomplete
            // result from a genuinely short/silent file — see
            // takeInferenceFailureCount().
            qWarning() << "ParakeetEngine: decode failed:" << e.what();
            ++m_decodeFailures;
            continue;
        }

        // assembleSentenceSegments returns times relative to the sub-window;
        // shift them to be relative to the whole chunk (the app then adds the
        // chunk's own offsetSeconds — it owns the cross-chunk merge).
        std::vector<Segment> subSegments =
            assembleSentenceSegments(result.tokens, result.timestamps, result.text);
        for (Segment &seg : subSegments) {
            seg.t0 += subOffset;
            seg.t1 += subOffset;
            segments.push_back(seg);
            if (onSegment)
                onSegment(seg);
        }
        if (onProgress)
            onProgress(static_cast<double>(endSample) / kSampleRate, totalSeconds);
    }

    if (onProgress)
        onProgress(totalSeconds, totalSeconds);
    return segments;
}
