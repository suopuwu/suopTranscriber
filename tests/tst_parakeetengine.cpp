// tst_parakeetengine — spec for ParakeetEngine::assembleSentenceSegments(),
// the token->sentence assembly that turns sherpa-onnx's per-token Parakeet
// output into the app's sentence-level Segments. Pure (no model, no sherpa
// recognizer) — exercised on plain token/timestamp vectors.

#include <QtTest>

#include <QByteArray>
#include <QDir>
#include <QFileInfo>
#include <QString>

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

#include "AudioDecoder.h"
#include "ParakeetEngine.h"
#include "Segment.h"

namespace {
// SentencePiece word-boundary marker "▁" (U+2581) as raw UTF-8 bytes, so the
// test doesn't depend on source/exec charset handling of \u escapes.
std::string wb(const std::string &s)
{
    return std::string("\xe2\x96\x81") + s;
}
} // namespace

class TstParakeetEngine : public QObject
{
    Q_OBJECT

private slots:
    // Spec: words are split at the "▁" boundary and grouped into sentences on
    // terminal punctuation; each Segment's t0 is its first word's onset and t1
    // is the next segment's t0 (the app's "end == next start" convention).
    void splitsSentencesOnPunctuationWithFirstWordOnset()
    {
        const std::vector<std::string> tokens = {wb("Hello"), wb("world."), wb("How"), wb("are"), wb("you?")};
        const std::vector<float> timestamps = {0.0f, 0.4f, 1.0f, 1.4f, 1.8f};

        const std::vector<Segment> segs = ParakeetEngine::assembleSentenceSegments(tokens, timestamps, "");

        QCOMPARE(segs.size(), std::size_t(2));
        QCOMPARE(segs[0].text, std::string("Hello world."));
        QCOMPARE(segs[0].t0, 0.0);
        QCOMPARE(segs[1].text, std::string("How are you?"));
        QCOMPARE(segs[1].t0, 1.0);
        // t1 == next segment's t0.
        QCOMPARE(segs[0].t1, 1.0);
    }

    // Spec (regression): sherpa-onnx decodes Parakeet's "▁" word-boundary
    // marker to a LEADING SPACE in the tokens it actually returns (empirically
    // " Help"/"el"/"p"/" me"), not the raw "▁Help" form. A leading space must
    // therefore start a new word too — otherwise every subword folds into one
    // giant word and a whole ~4-minute window collapses to a single segment,
    // breaking sentence-level highlighting.
    void splitsSentencesFromSpacePrefixedTokens()
    {
        // Real sherpa token shape: word-initial tokens carry a leading space,
        // continuation subwords do not.
        const std::vector<std::string> tokens = {
            " Help", "ing", " hands.", " Meet", " people", " where", " they", " are."};
        const std::vector<float> timestamps = {0.0f, 0.3f, 0.6f, 1.0f, 1.3f, 1.6f, 1.9f, 2.2f};

        const std::vector<Segment> segs = ParakeetEngine::assembleSentenceSegments(tokens, timestamps, "");

        QCOMPARE(segs.size(), std::size_t(2));
        QCOMPARE(segs[0].text, std::string("Helping hands."));
        QCOMPARE(segs[0].t0, 0.0);
        QCOMPARE(segs[1].text, std::string("Meet people where they are."));
        QCOMPARE(segs[1].t0, 1.0);
        QCOMPARE(segs[0].t1, 1.0); // t1 == next segment's t0
    }

    // Spec: subword tokens with no leading boundary attach to the current word.
    void reassemblesSubwordTokensIntoWords()
    {
        const std::vector<std::string> tokens = {wb("It"), wb("is"), wb("un"), "believ", "able."};
        const std::vector<float> timestamps = {0.0f, 0.2f, 0.5f, 0.7f, 0.9f};

        const std::vector<Segment> segs = ParakeetEngine::assembleSentenceSegments(tokens, timestamps, "");

        QCOMPARE(segs.size(), std::size_t(1));
        QCOMPARE(segs[0].text, std::string("It is unbelievable."));
        // The word's onset is its FIRST subword's timestamp (0.5 for "un…").
        QCOMPARE(segs[0].t0, 0.0);
    }

    // Spec: with no terminal punctuation, the whole utterance is one segment
    // (a VAD window still bounds its length).
    void noPunctuationYieldsSingleSegment()
    {
        const std::vector<std::string> tokens = {wb("just"), wb("some"), wb("words")};
        const std::vector<float> timestamps = {0.0f, 0.3f, 0.6f};

        const std::vector<Segment> segs = ParakeetEngine::assembleSentenceSegments(tokens, timestamps, "");

        QCOMPARE(segs.size(), std::size_t(1));
        QCOMPARE(segs[0].text, std::string("just some words"));
        QCOMPARE(segs[0].t0, 0.0);
    }

    // Spec: no tokens but a fallback full text ⇒ one segment carrying the text
    // (nothing is silently lost).
    void emptyTokensFallsBackToFullText()
    {
        const std::vector<Segment> segs = ParakeetEngine::assembleSentenceSegments({}, {}, "some text");
        QCOMPARE(segs.size(), std::size_t(1));
        QCOMPARE(segs[0].text, std::string("some text"));
        QCOMPARE(segs[0].t0, 0.0);
    }

    // Spec: truly empty input ⇒ no segments (not a bogus empty one).
    void fullyEmptyYieldsNothing()
    {
        const std::vector<Segment> segs = ParakeetEngine::assembleSentenceSegments({}, {}, "");
        QVERIFY(segs.empty());
    }

    // Spec: a closing quote/bracket AFTER the terminator still ends the
    // sentence (e.g. a quoted sentence).
    void terminatorFollowedByClosingQuoteStillEnds()
    {
        const std::vector<std::string> tokens = {wb("\"Stop.\""), wb("Now")};
        const std::vector<float> timestamps = {0.0f, 0.5f};

        const std::vector<Segment> segs = ParakeetEngine::assembleSentenceSegments(tokens, timestamps, "");
        QCOMPARE(segs.size(), std::size_t(2));
        QCOMPARE(segs[0].text, std::string("\"Stop.\""));
        QCOMPARE(segs[1].text, std::string("Now"));
    }

    // Spec: a chunk within the model's window limit is fed whole — exactly one
    // range covering the buffer (the unchanged single-Decode path).
    void subChunkRangesShortInputIsOneWholeRange()
    {
        const std::vector<float> pcm(100'000, 0.1f); // ~6 s
        const auto ranges = ParakeetEngine::subChunkRanges(pcm, 300 * 16000, 8 * 16000);
        QCOMPARE(ranges.size(), std::size_t(1));
        QCOMPARE(ranges[0].first, std::size_t(0));
        QCOMPARE(ranges[0].second, pcm.size());
    }

    // Spec: an over-long buffer is split into near-equal, contiguous, gapless
    // ranges that together cover the whole buffer and NONE exceeds maxSamples
    // (the invariant that keeps Parakeet under its ~400 s attention ceiling).
    void subChunkRangesSplitsLongInputWithinLimit()
    {
        // 1000 "seconds" at a tiny 100 Hz rate so the test stays cheap; the
        // logic is sample-count based, independent of the real 16 kHz.
        const std::size_t maxSamples = 300 * 100;
        const std::size_t search = 8 * 100;
        const std::vector<float> pcm(1000 * 100, 0.5f);
        const auto ranges = ParakeetEngine::subChunkRanges(pcm, maxSamples, search);

        QVERIFY(ranges.size() >= 4); // ceil(1000/300) == 4
        QCOMPARE(ranges.front().first, std::size_t(0));
        QCOMPARE(ranges.back().second, pcm.size());
        // Nominal windows are <= maxSamples; pause-seeking can nudge each
        // interior boundary by up to searchSamples on either side, so the true
        // upper bound is maxSamples + 2*searchSamples — still far under the
        // model's ~400 s ceiling (300 + 16 s here, scaled).
        const std::size_t hardBound = maxSamples + 2 * search;
        for (std::size_t i = 0; i < ranges.size(); ++i) {
            QVERIFY(ranges[i].second > ranges[i].first);                 // non-empty
            QVERIFY(ranges[i].second - ranges[i].first <= hardBound);    // within limit
            if (i > 0)
                QCOMPARE(ranges[i].first, ranges[i - 1].second);         // gapless/contiguous
        }
    }

    // Spec: an interior split boundary is nudged onto a quiet region rather
    // than cutting through loud audio (so it lands in a natural pause). Uses the
    // real 16 kHz scale so the energy frame (a fixed 20 ms = 320 samples) is
    // meaningful and the silent stretch is several frames wide.
    void subChunkRangesCutsAtQuietestNearbyPoint()
    {
        constexpr std::size_t sr = 16000;
        const std::size_t maxSamples = 10 * sr; // small, so a 20 s buffer splits in two
        const std::size_t search = 3 * sr;
        std::vector<float> pcm(20 * sr, 0.9f); // loud throughout...
        // ...except a clearly-silent stretch offset from the nominal midpoint
        // (20 s / 2 == 10 s) but well within the search radius.
        const std::size_t quietStart = 10 * sr + 1 * sr;   // +1 s past the midpoint
        const std::size_t quietEnd = quietStart + sr / 2;  // 0.5 s of silence (many frames)
        for (std::size_t i = quietStart; i < quietEnd; ++i)
            pcm[i] = 0.0f;

        const auto ranges = ParakeetEngine::subChunkRanges(pcm, maxSamples, search);
        QCOMPARE(ranges.size(), std::size_t(2));
        // The cut should land inside the silent stretch, not at the loud midpoint.
        QVERIFY(ranges[0].second >= quietStart);
        QVERIFY(ranges[0].second < quietEnd);
    }

    // Spec: transcribe() must decode at the LIVE granularity, not the attention
    // ceiling. sherpa-onnx emits a window's segments only after its Decode()
    // returns, so decoding at the 300 s ceiling means a typical file's whole
    // transcript arrives in one end-of-file burst (nothing streams / follows).
    // Splitting at kLiveChunkSamples (~30 s) is what makes it gradual. No
    // model-free way to observe the streaming cadence end-to-end (the real
    // decode below is gated on the ~630 MB bundle), so this is a source-scan
    // guard — same technique as tst_styleguide / tst_transcription_queue's
    // benchmarkJob guard — pinning that the finer granularity stays wired.
    void transcribe_decodesAtLiveGranularity_notTheCeiling()
    {
        QFile src(QStringLiteral(SUOP_SRC_DIR "/core/ParakeetEngine.cpp"));
        QVERIFY2(src.open(QIODevice::ReadOnly), "ParakeetEngine.cpp must be readable");
        const QString all = QString::fromUtf8(src.readAll());

        // The live-chunk constant exists and is meaningfully smaller than the
        // ceiling (30 s vs 300 s) — otherwise a "gradual" split is a no-op.
        QVERIFY2(all.contains(QStringLiteral("kLiveChunkSamples = static_cast<std::size_t>(30.0 * kSampleRate)")),
                 "kLiveChunkSamples (the ~30 s streaming decode granularity) must exist");
        QVERIFY2(all.contains(QStringLiteral("kMaxChunkSamples = static_cast<std::size_t>(300.0 * kSampleRate)")),
                 "kMaxChunkSamples (the ~300 s attention ceiling) must exist");

        // transcribe() must feed subChunkRanges the LIVE granularity, not the
        // ceiling — the exact regression that leaves Parakeet emitting one burst.
        const int t = all.indexOf(QStringLiteral("::transcribe("));
        QVERIFY2(t >= 0, "transcribe() must exist");
        const int call = all.indexOf(QStringLiteral("subChunkRanges(pcm,"), t);
        QVERIFY2(call > t, "transcribe() must call subChunkRanges(pcm, ...)");
        const QString callLine = all.mid(call, all.indexOf(QLatin1Char('\n'), call) - call);
        QVERIFY2(callLine.contains(QStringLiteral("kLiveChunkSamples")),
                 "transcribe() must decode at kLiveChunkSamples so segments stream; "
                 "decoding at kMaxChunkSamples brings back the end-of-file burst");
    }

    // End-to-end real decode, gated on the actual ONNX bundle being present
    // (SUOP_PARAKEET_DIR points at a dir with encoder/decoder/joiner.int8.onnx
    // + tokens.txt). Proves the sherpa-onnx config (model_type="nemo_transducer",
    // 16 kHz PCM in) actually loads and transcribes the JFK sample into the
    // expected words with chunk-relative timestamps. QSKIPs when the ~630 MB
    // bundle isn't downloaded, so the suite still passes offline.
    void realDecode_transcribesJfkSample()
    {
        const QByteArray dirEnv = qgetenv("SUOP_PARAKEET_DIR");
        if (dirEnv.isEmpty())
            QSKIP("set SUOP_PARAKEET_DIR to a downloaded Parakeet bundle dir to run the real decode");
        const QDir bundle(QString::fromUtf8(dirEnv));
        const QString encoder = bundle.filePath(QStringLiteral("encoder.int8.onnx"));
        const QString decoder = bundle.filePath(QStringLiteral("decoder.int8.onnx"));
        const QString joiner = bundle.filePath(QStringLiteral("joiner.int8.onnx"));
        const QString tokens = bundle.filePath(QStringLiteral("tokens.txt"));
        for (const QString &f : {encoder, decoder, joiner, tokens})
            QVERIFY2(QFileInfo::exists(f), qPrintable(QStringLiteral("missing bundle file: ") + f));

        const QString sample = QStringLiteral(SUOP_SAMPLES_DIR "/jfk.wav");
        QVERIFY2(QFileInfo::exists(sample), qPrintable(sample));
        QString decodeError;
        const auto pcm = AudioDecoder::decodeTo16kMono(sample, &decodeError);
        QVERIFY2(pcm.has_value(), qPrintable(decodeError));

        ParakeetEngine engine(encoder, decoder, joiner, tokens);
        QVERIFY2(engine.isValid(), "Parakeet recognizer failed to load — check model_type/bundle");
        // Regression: createSafely() writes m_backendName, so it must run only
        // after that member is constructed (see the ctor). A member-init-order
        // slip left m_backendName either garbage (crash on a dirty heap) or
        // empty (name discarded) — a populated "CUDA"/"CPU" here proves neither.
        const QString backend = engine.activeBackendName();
        QVERIFY2(backend == QStringLiteral("CUDA") || backend == QStringLiteral("CPU"),
                 qPrintable(QStringLiteral("unexpected backend name: '%1'").arg(backend)));

        AudioChunk chunk;
        chunk.pcm16kMono = *pcm;
        chunk.offsetSeconds = 0.0;

        std::vector<Segment> streamed;
        const std::vector<Segment> segs = engine.transcribe(
            chunk, {}, [&](const Segment &s) { streamed.push_back(s); });

        QVERIFY2(!segs.empty(), "Parakeet produced no segments for the JFK sample");
        QCOMPARE(streamed.size(), segs.size());

        std::string allText;
        for (const Segment &s : segs) {
            allText += ' ';
            allText += s.text;
        }
        std::transform(allText.begin(), allText.end(), allText.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        // JFK: "...ask not what your country can do for you..."
        QVERIFY2(allText.find("country") != std::string::npos,
                 qPrintable(QStringLiteral("transcript missing 'country': ")
                            + QString::fromStdString(allText)));

        // Chunk-relative timestamps: first segment starts at/after 0, in range.
        QVERIFY(segs.front().t0 >= 0.0);
        QVERIFY(segs.front().t0 < 11.0); // the sample is ~11s
    }
};

QTEST_GUILESS_MAIN(TstParakeetEngine)
#include "tst_parakeetengine.moc"
