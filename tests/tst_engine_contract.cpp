// tst_engine_contract — living spec for DESIGN.md "Engine architecture".
//
// The engine abstraction lives at the **Segment level, not the transport**
// (a load-bearing constraint, DESIGN.md "Engine architecture"): every engine — the in-process
// whisper.cpp default or a future out-of-process JSONL subprocess — consumes
// an AudioChunk and returns std::vector<Segment>. Only the adapter differs.
//
// Engines emit CHUNK-RELATIVE timestamps. The APP owns the chunk
// timestamp-offset merge (adding AudioChunk::offsetSeconds); no engine may
// pre-apply it.

#include <QtTest>

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <type_traits>

#include "AudioDecoder.h"
#include "Segment.h"
#include "TranscriptionEngine.h"
#include "WhisperCppEngine.h"

#include <whisper.h>

// ---------------------------------------------------------------------------
// Spec: Segment is the unit of abstraction — a plain aggregate holding
// sentence-level start/end times (seconds, doubles) plus text. No word
// timestamps exist anywhere (DESIGN.md "Sentence highlighting").
static_assert(std::is_aggregate_v<Segment>, "Segment is a plain aggregate — engines fill it directly");
static_assert(std::is_same_v<decltype(Segment::t0), double>, "t0 is seconds as double");
static_assert(std::is_same_v<decltype(Segment::t1), double>, "t1 is seconds as double");
static_assert(std::is_same_v<decltype(Segment::text), std::string>, "text is std::string (UTF-8)");

// Spec: AudioChunk is what every engine consumes — 16 kHz mono float PCM
// (decoded by ffmpeg upstream, see TranscriptionEngine.h) plus the chunk's
// position in the source file. The offset is metadata for the APP's merge;
// engines never add it to their timestamps.
static_assert(std::is_aggregate_v<AudioChunk>, "AudioChunk is a plain aggregate");
static_assert(std::is_same_v<decltype(AudioChunk::pcm16kMono), std::vector<float>>,
              "engines receive 16kHz mono float32 PCM");
static_assert(std::is_same_v<decltype(AudioChunk::offsetSeconds), double>,
              "offsetSeconds positions the chunk in the source file");

namespace {

// Minimal conforming engine, defined here to document the interface contract:
// transcribe(chunk, onProgress, onSegment) returns std::vector<Segment> with
// CHUNK-RELATIVE t0/t1, streams each segment through onSegment as it is
// produced, and reports progress via onProgress. Nothing about the transport
// (in-process, subprocess, ...) leaks into this signature.
class FakeEngine : public TranscriptionEngine {
public:
    std::vector<Segment> transcribe(const AudioChunk &chunk, const ProgressCallback &onProgress,
                                     const SegmentCallback &onSegment, const AbortCallback &onAbort = {}) override
    {
        Q_UNUSED(onAbort);
        const double totalSeconds = static_cast<double>(chunk.pcm16kMono.size()) / 16000.0;
        // Timestamps start at 0.0 regardless of chunk.offsetSeconds — the
        // engine deliberately ignores the offset. That is the contract.
        const std::vector<Segment> segments = {
            {0.0, 2.5, "First sentence."},
            {2.5, 5.0, "Second sentence."},
        };
        for (const Segment &s : segments) {
            if (onSegment)
                onSegment(s); // streamed per segment, so the UI can show partial results
            if (onProgress)
                onProgress(s.t1, totalSeconds);
        }
        return segments;
    }
};

// Locate a REAL ggml model for the gated end-to-end test, in order:
//   1. env SUOP_TEST_MODEL (explicit path)
//   2. the user's real vault model dir, preferring a plain (non-tdrz) .en model
// The truncated for-tests-ggml-*.bin dummies under third_party are NOT usable —
// they carry no real weights.
QString locateRealModel()
{
    const QString envModel = qEnvironmentVariable("SUOP_TEST_MODEL");
    if (!envModel.isEmpty() && QFileInfo::exists(envModel))
        return envModel;

    QDir vaultModels(QStringLiteral("/home/user/recordings/.transcriber/models"));
    const QStringList bins = vaultModels.entryList({QStringLiteral("*.bin")}, QDir::Files, QDir::Name);
    QString fallback;
    for (const QString &bin : bins) {
        if (bin.contains(QStringLiteral("tdrz")))
            continue; // tdrz fine-tunes are for speaker-turn detection, not this test
        if (bin.contains(QStringLiteral(".en")))
            return vaultModels.filePath(bin);
        if (fallback.isEmpty())
            fallback = vaultModels.filePath(bin);
    }
    return fallback; // may be empty — caller QSKIPs
}

std::string toLower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

} // namespace

class TstEngineContract : public QObject
{
    Q_OBJECT

private slots:
    // Spec: Segment's fields default to a zeroed state, so a freshly
    // value-initialized Segment is valid and inert.
    void segmentShapeAndDefaults()
    {
        const Segment s;
        QCOMPARE(s.t0, 0.0);
        QCOMPARE(s.t1, 0.0);
        QVERIFY(s.text.empty());

        // Designated/positional aggregate init is how engines and tests
        // build Segments — no constructor to keep in sync.
        const Segment filled{1.0, 2.0, "hello"};
        QCOMPARE(filled.t0, 1.0);
        QCOMPARE(filled.t1, 2.0);
        QCOMPARE(filled.text, std::string("hello"));
    }

    // Spec: AudioChunk defaults to an empty buffer at the start of the file
    // (offsetSeconds == 0.0).
    void audioChunkShapeAndDefaults()
    {
        const AudioChunk chunk;
        QVERIFY(chunk.pcm16kMono.empty());
        QCOMPARE(chunk.offsetSeconds, 0.0);
    }

    // Spec: WhisperCppEngine's decode parameters carry the
    // repetition-hallucination hardening documented at decodeParams()'s
    // definition — beam search (not greedy), a tightened entropy tripwire,
    // and non-speech-token suppression — while leaving whisper.cpp's
    // temperature-fallback retry loop and cross-window context reset
    // (no_context) at their defaults, which the hardening relies on.
    void decodeParamsHardenAgainstRepetitionHallucination()
    {
        const whisper_full_params wparams = WhisperCppEngine::decodeParams();

        // Beam search with whisper.cpp's own beam_size default (5) — the
        // same strategy its CLI uses; greedy is the nonstandard path.
        QCOMPARE(wparams.strategy, WHISPER_SAMPLING_BEAM_SEARCH);
        QCOMPARE(wparams.beam_search.beam_size, 5);

        // Repetition tripwire tightened below the 2.4 default; low-entropy
        // (repetitive) windows get rejected and retried at a higher
        // temperature, so the fallback ladder must remain enabled for the
        // rejection to have somewhere to go.
        QVERIFY(wparams.entropy_thold < 2.4f);
        QCOMPARE(wparams.entropy_thold, 2.2f);
        QVERIFY(wparams.temperature_inc > 0.0f);

        // Non-speech tokens (music notation, sound-effect markers) stay
        // suppressed, and a fresh text context per 30s window keeps one
        // window's repetition loop from seeding the next.
        QVERIFY(wparams.suppress_nst);
        QVERIFY(wparams.no_context);
    }

    // flash attention is opted in at context creation (whisper defaults leave
    // it off) — a faster encoder-attention kernel, free on the CUDA path.
    void contextParamsEnablesFlashAttention()
    {
        const whisper_context_params cparams = WhisperCppEngine::contextParams();
        QVERIFY(cparams.flash_attn);
    }

    // Spec (DESIGN.md "Engine architecture"): engines return
    // chunk-relative timestamps; the APP applies offsetSeconds during the
    // merge. Demonstrated with a FakeEngine standing in for "any engine" —
    // the same assertions must hold for WhisperCppEngine and any future
    // SubprocessEngine, because only the adapter differs.
    void engineReturnsChunkRelativeAppOwnsOffsetMerge()
    {
        FakeEngine engine;

        AudioChunk chunk;
        chunk.pcm16kMono.assign(5 * 16000, 0.0f); // 5 s of audio...
        chunk.offsetSeconds = 60.0;               // ...starting 1 minute into the file

        std::vector<Segment> streamed;
        int progressCalls = 0;
        double lastTotal = 0.0;
        const std::vector<Segment> returned = engine.transcribe(
            chunk,
            [&](double, double secondsTotal) {
                ++progressCalls;
                lastTotal = secondsTotal;
            },
            [&](const Segment &s) { streamed.push_back(s); });

        // All engines return std::vector<Segment> — and stream the same
        // segments through onSegment as they are produced.
        QCOMPARE(returned.size(), std::size_t(2));
        QCOMPARE(streamed.size(), returned.size());
        for (std::size_t i = 0; i < returned.size(); ++i) {
            QCOMPARE(streamed[i].t0, returned[i].t0);
            QCOMPARE(streamed[i].t1, returned[i].t1);
            QCOMPARE(streamed[i].text, returned[i].text);
        }

        // CHUNK-RELATIVE: the first segment starts at 0.0, not at 60.0 —
        // the engine did not (must not) bake in the offset.
        QCOMPARE(returned.front().t0, 0.0);
        QVERIFY(returned.back().t1 <= 5.0 + 1e-9);

        // The APP layer performs the offset merge itself; this is the whole
        // merge — trivial precisely because the contract keeps it out of the
        // engine.
        std::vector<Segment> merged = returned;
        for (Segment &s : merged) {
            s.t0 += chunk.offsetSeconds;
            s.t1 += chunk.offsetSeconds;
        }
        QCOMPARE(merged.front().t0, 60.0);
        QCOMPARE(merged.back().t1, 65.0);

        // Progress is reported against the chunk's own duration.
        QVERIFY(progressCalls > 0);
        QCOMPARE(lastTotal, 5.0);
    }

    // Spec (DESIGN.md "Durable / resumable transcription", chunking half —
    // see VadChunker.h): once a file is split into multiple windows, each
    // window is its own transcribe() call with its own offsetSeconds, and
    // the APP stitches their chunk-relative segments back into one globally-
    // ordered, correctly-offset stream — the real multi-chunk case the
    // single-chunk test above stands in for.
    void multiChunkStitch_producesGloballyOrderedOffsetSegments()
    {
        FakeEngine engine;

        // Two chunks, as if VadChunker had split a 65s file into windows
        // [0, 60) and [60, 65) — FakeEngine always reports the same two
        // chunk-relative segments {0-2.5, 2.5-5} regardless of chunk content,
        // so each window's raw output is identical and only the offset
        // merge distinguishes them.
        AudioChunk chunk1;
        chunk1.pcm16kMono.assign(60 * 16000, 0.0f);
        chunk1.offsetSeconds = 0.0;

        AudioChunk chunk2;
        chunk2.pcm16kMono.assign(5 * 16000, 0.0f);
        chunk2.offsetSeconds = 60.0;

        std::vector<Segment> stitched;
        for (const AudioChunk &chunk : {chunk1, chunk2}) {
            const std::vector<Segment> raw = engine.transcribe(
                chunk, [](double, double) {}, [](const Segment &) {});
            for (Segment s : raw) {
                s.t0 += chunk.offsetSeconds;
                s.t1 += chunk.offsetSeconds;
                stitched.push_back(s);
            }
        }

        QCOMPARE(stitched.size(), std::size_t(4));
        // Strictly increasing, globally-offset timestamps across the
        // chunk boundary — this is the whole point of the offset merge.
        for (std::size_t i = 1; i < stitched.size(); ++i) {
            QVERIFY2(stitched[i].t0 >= stitched[i - 1].t0,
                     "segments must stay in global chronological order across chunks");
        }
        QCOMPARE(stitched.front().t0, 0.0);
        QCOMPARE(stitched[2].t0, 60.0); // first segment of the second chunk
        QCOMPARE(stitched.back().t1, 65.0);
    }

    // Spec: constructing WhisperCppEngine with a bad model path must not
    // crash (whisper_init returns nullptr); isValid() reports the failure and
    // transcribe() is a documented no-op — empty vector, no callbacks fired
    // (WhisperCppEngine.cpp early-returns when the context is null).
    void invalidModelPathIsInertNotFatal()
    {
        // whisper.cpp prints a load-failure line to stderr here — expected noise.
        WhisperCppEngine engine(QStringLiteral("/nonexistent/path/to/ggml-model.bin"));
        QVERIFY(!engine.isValid());

        AudioChunk chunk;
        chunk.pcm16kMono.assign(16000, 0.0f);
        int segmentCalls = 0;
        int progressCalls = 0;
        const std::vector<Segment> segments = engine.transcribe(
            chunk,
            [&](double, double) { ++progressCalls; },
            [&](const Segment &) { ++segmentCalls; });

        QVERIFY(segments.empty());
        QCOMPARE(segmentCalls, 0);
        QCOMPARE(progressCalls, 0);
    }

    // Gated REAL end-to-end run of the in-process pipeline (DESIGN.md
    // "whisper.cpp — first-class, in-process"): warm model loaded once,
    // ffmpeg-decoded PCM handed straight to whisper_full, native
    // new_segment/progress callbacks — no temp WAV anywhere. This is the only
    // test that exercises the whole pipeline end to end; it skips when no
    // real model is on disk. GPU note: on this machine whisper.cpp
    // initializes Vulkan and prints model-load noise to stderr — harmless.
    void realWhisperCppEndToEnd()
    {
        const QString modelPath = locateRealModel();
        if (modelPath.isEmpty())
            QSKIP("no real ggml model available; set SUOP_TEST_MODEL or transcribe once in the app");
        if (QStandardPaths::findExecutable(QStringLiteral("ffmpeg")).isEmpty())
            QSKIP("ffmpeg not on PATH — needed to decode the sample (DESIGN.md: ffmpeg decodes, Qt only plays)");

        // ffmpeg decodes/resamples to 16 kHz mono float PCM in memory.
        QString decodeError;
        const auto pcm = AudioDecoder::decodeTo16kMono(QStringLiteral(SUOP_SAMPLES_DIR "/jfk.wav"), &decodeError);
        QVERIFY2(pcm.has_value(), qPrintable(decodeError));

        WhisperCppEngine engine(modelPath);
        QVERIFY2(engine.isValid(), qPrintable(QStringLiteral("failed to load model: ") + modelPath));

        // Item O35: a real load must always resolve to SOME backend name —
        // "CPU" when no GPU line fired, or whatever whisper.cpp's own log
        // line named (see WhisperCppEngine::activeBackendName()'s doc
        // comment) — never empty once a model has actually loaded.
        QVERIFY2(!engine.activeBackendName().isEmpty(),
                 "activeBackendName() must be populated after a real model load");

        AudioChunk chunk;
        chunk.pcm16kMono = *pcm;
        const double chunkSeconds = static_cast<double>(chunk.pcm16kMono.size()) / 16000.0;

        std::vector<Segment> streamed;
        int progressCalls = 0;
        const std::vector<Segment> segments = engine.transcribe(
            chunk,
            [&](double, double) { ++progressCalls; },
            [&](const Segment &s) { streamed.push_back(s); });

        // Engines return std::vector<Segment>, non-empty for real speech.
        QVERIFY(!segments.empty());

        // Chunk-relative, well-ordered timestamps within the chunk.
        std::string allText;
        for (const Segment &s : segments) {
            QVERIFY(s.t0 >= 0.0);
            QVERIFY(s.t1 >= s.t0);
            QVERIFY(s.t1 <= chunkSeconds + 2.0); // whisper may round up to a token boundary
            allText += s.text;
        }

        // The native new_segment callback streamed every returned segment.
        QCOMPARE(streamed.size(), segments.size());
        for (std::size_t i = 0; i < segments.size(); ++i) {
            QCOMPARE(streamed[i].t0, segments[i].t0);
            QCOMPARE(streamed[i].t1, segments[i].t1);
            QCOMPARE(streamed[i].text, segments[i].text);
        }

        // The native progress callback fired at least once.
        QVERIFY(progressCalls >= 1);

        // The JFK sample: "...ask not what your country can do for you..."
        QVERIFY2(toLower(allText).find("country") != std::string::npos,
                 qPrintable(QStringLiteral("transcript missing 'country': ") + QString::fromStdString(allText)));
    }

    // Spec: the bundled out-of-process NeMo engine
    // (engines/parakeet/manifest.json) and the in-process sherpa-onnx model
    // parakeet-tdt-0.6b-v2 are two DIFFERENT Parakeet backends. Their labels
    // must not collide — the NeMo engine, being the optional fallback that is
    // "unavailable" until NeMo is installed, must never read as the in-process
    // Parakeet the user actually transcribes with. Guard the bundled manifest's
    // display name at its source: it must carry a disambiguator and must not
    // regress to the "Parakeet TDT 0.6B v2 (…)" parenthetical form that reads
    // as the in-process model row.
    void bundledParakeetManifestLabelIsDisambiguated()
    {
        QFile file(QStringLiteral(SUOP_REPO_DIR "/engines/parakeet/manifest.json"));
        QVERIFY2(file.open(QIODevice::ReadOnly), qPrintable(file.errorString()));

        QJsonParseError parseError{};
        const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parseError);
        QVERIFY2(parseError.error == QJsonParseError::NoError, qPrintable(parseError.errorString()));

        const QString name = doc.object().value(QStringLiteral("name")).toString();
        QVERIFY2(!name.isEmpty(), "bundled parakeet manifest must have a name");

        // Carries a disambiguator so it reads as a separate optional backend.
        QVERIFY2(name.contains(QStringLiteral("out-of-process"), Qt::CaseInsensitive)
                     || name.contains(QStringLiteral("optional"), Qt::CaseInsensitive),
                 qPrintable(QStringLiteral("NeMo engine label must disambiguate itself: ") + name));

        // Never the parenthetical-suffix form that collides with the in-process
        // model's "Parakeet TDT 0.6B v2 (English, NVIDIA) — …" display label.
        QVERIFY2(!name.startsWith(QStringLiteral("Parakeet TDT 0.6B v2 (")),
                 qPrintable(QStringLiteral("NeMo engine label must not read as the in-process model: ") + name));
    }

    // subprocessEngineJsonlProtocol: BUILT. DESIGN.md's out-of-process engines
    // behind the JSONL manifest protocol now exist — SubprocessEngine
    // implements TranscriptionEngine by driving a child process and adapting
    // its JSONL output to Segments. The full protocol (progress/segment/done/
    // error parsing, chunk-relative timestamps with the app owning the offset
    // merge, abort, graceful degradation) is exercised in tst_subprocessengine
    // against a fake engine script, with manifest parsing in tst_enginemanifest
    // and capability detection in tst_engineregistry.
};

QTEST_GUILESS_MAIN(TstEngineContract)
#include "tst_engine_contract.moc"
