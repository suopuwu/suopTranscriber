// tst_transcribe_pipeline — living spec for the SHARED transcription pipeline
// (src/core/TranscribePipeline.*), the windowing + per-window streaming loop
// used by BOTH TranscribeTask and BenchmarkTask so their segmentation/timing
// are identical (no parallel code paths).
//
// Exercised with a FakeEngine (same minimal conforming engine pattern as
// tst_engine_contract.cpp): engines emit CHUNK-RELATIVE timestamps and the
// APP — here, TranscribePipeline::runWindows — owns the offset merge, so
// every Segment the pipeline hands out is already absolute (file-relative).
//
// planWindows' VAD path needs a real Silero VAD model (network/model
// dependency) — out of scope here; the self-segmenting branch (subprocess
// engines: one whole-file window, no VAD) is asserted model-free.

#include <QtTest>

#include <utility>
#include <vector>

#include "Segment.h"
#include "TranscribePipeline.h"
#include "TranscriptionEngine.h"
#include "VadChunker.h"

namespace {

// Minimal conforming engine (see tst_engine_contract.cpp): returns two
// CHUNK-RELATIVE segments {0,2.5} / {2.5,5} regardless of the chunk's
// offset, streams each via onSegment, and reports progress via onProgress.
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
                onSegment(s);
            if (onProgress)
                onProgress(s.t1, totalSeconds);
        }
        return segments;
    }
};

// 10 seconds of silence at 16 kHz — enough for two hand-built 5s windows.
std::vector<float> tenSecondsPcm()
{
    return std::vector<float>(10 * 16000, 0.0f);
}

const std::vector<VadChunker::Window> kTwoWindows = {{0.0, 5.0}, {5.0, 10.0}};

} // namespace

class TstTranscribePipeline : public QObject
{
    Q_OBJECT

private slots:
    void runWindows_appliesOffsetMerge();
    void runWindows_streamsPerSegmentInOrder();
    void runWindows_progressIsCumulative();
    void runWindows_abortStopsEarly();
    void planWindows_selfSegmentingIsOneWholeFileWindow();
};

// The offset merge is the PIPELINE's job now: for the second window (offset
// 5.0) both the streamed and the returned segments carry ABSOLUTE t0/t1
// (5.0, 7.5, ...), not chunk-relative — proving the merge moved into
// runWindows and fires exactly once (not zero times, not twice).
void TstTranscribePipeline::runWindows_appliesOffsetMerge()
{
    FakeEngine engine;
    const std::vector<float> pcm = tenSecondsPcm();

    std::vector<Segment> streamed;
    const SegmentCallback onSegment = [&streamed](const Segment &s) { streamed.push_back(s); };

    const std::vector<Segment> returned =
        TranscribePipeline::runWindows(engine, pcm, kTwoWindows, {}, onSegment, {});

    const std::vector<std::pair<double, double>> expected = {
        {0.0, 2.5}, {2.5, 5.0}, // window 1, offset 0
        {5.0, 7.5}, {7.5, 10.0}, // window 2, offset 5 — merged, not chunk-relative
    };
    QCOMPARE(streamed.size(), expected.size());
    QCOMPARE(returned.size(), expected.size());
    for (std::size_t i = 0; i < expected.size(); ++i) {
        QCOMPARE(streamed[i].t0, expected[i].first);
        QCOMPARE(streamed[i].t1, expected[i].second);
        QCOMPARE(returned[i].t0, expected[i].first);
        QCOMPARE(returned[i].t1, expected[i].second);
    }
}

// onSegment fires once per produced segment across ALL windows (2 windows x
// 2 segments = 4), in production order — the live-streaming contract the UI
// (and the benchmark page's live flow-in) depends on.
void TstTranscribePipeline::runWindows_streamsPerSegmentInOrder()
{
    FakeEngine engine;
    const std::vector<float> pcm = tenSecondsPcm();

    std::vector<QString> texts;
    const SegmentCallback onSegment = [&texts](const Segment &s) {
        texts.push_back(QString::fromStdString(s.text));
    };

    TranscribePipeline::runWindows(engine, pcm, kTwoWindows, {}, onSegment, {});

    QCOMPARE(texts.size(), std::size_t(4));
    QCOMPARE(texts[0], QStringLiteral("First sentence."));
    QCOMPARE(texts[1], QStringLiteral("Second sentence."));
    QCOMPARE(texts[2], QStringLiteral("First sentence."));
    QCOMPARE(texts[3], QStringLiteral("Second sentence."));
}

// onProgress is CUMULATIVE against the whole file: `done` never snaps back
// to 0 at a window boundary, `total` is always the full file duration, and
// the last callback's `done` reaches the full duration.
void TstTranscribePipeline::runWindows_progressIsCumulative()
{
    FakeEngine engine;
    const std::vector<float> pcm = tenSecondsPcm();

    std::vector<std::pair<double, double>> ticks; // (done, total)
    const ProgressCallback onProgress = [&ticks](double done, double total) {
        ticks.push_back({done, total});
    };

    TranscribePipeline::runWindows(engine, pcm, kTwoWindows, onProgress, {}, {});

    QVERIFY(!ticks.empty());
    double previousDone = 0.0;
    for (const auto &[done, total] : ticks) {
        QVERIFY2(done >= previousDone, "cumulative done must never snap back at a window boundary");
        QCOMPARE(total, 10.0); // always the whole file, not the current window
        previousDone = done;
    }
    QCOMPARE(ticks.back().first, 10.0); // approaches the full duration
}

// The between-window onAbort checkpoint stops the loop early: aborting after
// the first window leaves the second window's segments unproduced.
void TstTranscribePipeline::runWindows_abortStopsEarly()
{
    FakeEngine engine;
    const std::vector<float> pcm = tenSecondsPcm();

    int abortPolls = 0;
    // False on the first (pre-window-1) poll, true from then on — so window 1
    // runs and the checkpoint before window 2 aborts.
    const AbortCallback onAbort = [&abortPolls] { return abortPolls++ > 0; };

    std::vector<Segment> streamed;
    const SegmentCallback onSegment = [&streamed](const Segment &s) { streamed.push_back(s); };

    const std::vector<Segment> returned =
        TranscribePipeline::runWindows(engine, pcm, kTwoWindows, {}, onSegment, onAbort);

    QCOMPARE(streamed.size(), std::size_t(2)); // window 1 only
    QCOMPARE(returned.size(), std::size_t(2));
    QCOMPARE(streamed.back().t1, 5.0); // nothing from the second window
}

// Self-segmenting (out-of-process/subprocess) engines do their own internal
// segmentation, so planWindows gives them exactly ONE whole-file window with
// VAD inactive — no VAD model involved (which is what makes this branch
// unit-testable; the VAD path needs a real Silero model).
void TstTranscribePipeline::planWindows_selfSegmentingIsOneWholeFileWindow()
{
    FakeEngine engine;
    const std::vector<float> pcm = tenSecondsPcm();

    const TranscribePipeline::Windowing win =
        TranscribePipeline::planWindows(/*selfSegmenting=*/true, pcm, engine);

    QCOMPARE(win.windows.size(), std::size_t(1));
    QCOMPARE(win.windows.front().startSeconds, 0.0);
    QCOMPARE(win.windows.front().endSeconds, static_cast<double>(pcm.size()) / 16000.0);
    QVERIFY(!win.vadActive);
    QVERIFY(win.vadError.isEmpty());
}

QTEST_GUILESS_MAIN(TstTranscribePipeline)
#include "tst_transcribe_pipeline.moc"
