// tst_vadchunking — living spec for DESIGN.md "Durable / resumable
// transcription", chunking half only (checkpoint/resume to .partial.jsonl is
// a separate, not-yet-built milestone — see tst_future_spec.cpp's
// partialJsonl_* placeholders, deliberately untouched by this feature).
//
// Chunk on silence via VAD in ~2-5 min windows, never at a fixed-time slice
// that could cut a word mid-segment — VadChunker::groupSegmentsIntoWindows()
// is the pure, unit-testable core of that rule; VadChunker::computeWindows()
// wraps it with a real whisper.cpp VAD pass and degrades gracefully (single
// whole-file window, today's pre-chunking behavior) when no VAD model is
// available.

#include <QtTest>

#include <QFileInfo>

#include "VadChunker.h"

class TstVadChunking : public QObject
{
    Q_OBJECT

private slots:
    // Spec: a file with no detected speech (silence/instrumental) degrades
    // to exactly one window spanning the whole file — same as "no chunking".
    void emptySegments_yieldsSingleWholeFileWindow()
    {
        const auto windows = VadChunker::groupSegmentsIntoWindows({}, 600.0);
        QCOMPARE(windows.size(), std::size_t(1));
        QCOMPARE(windows.front().startSeconds, 0.0);
        QCOMPARE(windows.front().endSeconds, 600.0);
    }

    // Spec: several short segments close together, well under the target
    // window length, all land in ONE window rather than each becoming its
    // own tiny window.
    void closeSegments_mergeIntoOneWindow()
    {
        const std::vector<std::pair<double, double>> segments = {
            {1.0, 5.0}, {6.0, 10.0}, {12.0, 20.0},
        };
        const auto windows = VadChunker::groupSegmentsIntoWindows(segments, 30.0, /*target=*/180.0,
                                                                    /*max=*/300.0);
        QCOMPARE(windows.size(), std::size_t(1));
        QCOMPARE(windows.front().startSeconds, 0.0);
        QCOMPARE(windows.front().endSeconds, 30.0);
    }

    // Spec: once accumulated segment span reaches the target window length,
    // the NEXT segment starts a new window instead of being absorbed — and
    // the boundary between them sits inside the silence gap (the midpoint),
    // never inside either segment.
    void reachingTarget_splitsAtTheGapNotMidSegment()
    {
        const std::vector<std::pair<double, double>> segments = {
            {0.0, 100.0},  // fills most of the 120s target on its own
            {110.0, 115.0}, // still fits before target is exceeded... see below
            {400.0, 410.0}, // far away — must start a new window
        };
        const auto windows =
            VadChunker::groupSegmentsIntoWindows(segments, 500.0, /*target=*/120.0, /*max=*/300.0);

        // First window absorbs segments 0 and 1 (span 0-115 >= 120? no —
        // 115 < 120, so it should try to absorb segment 2 next, but that
        // would jump the span to 410 which exceeds max(300) from start 0 —
        // so segment 2 is excluded, and window 1 ends at the midpoint of the
        // gap between segment 1's end (115) and segment 2's start (400).
        QCOMPARE(windows.size(), std::size_t(2));
        QCOMPARE(windows[0].startSeconds, 0.0);
        QCOMPARE(windows[0].endSeconds, (115.0 + 400.0) / 2.0);
        QCOMPARE(windows[1].startSeconds, windows[0].endSeconds);
        QCOMPARE(windows[1].endSeconds, 500.0);

        // No window ever ends strictly inside a segment's span.
        for (const auto &w : windows) {
            for (const auto &s : segments)
                QVERIFY(!(w.endSeconds > s.first && w.endSeconds < s.second));
        }
    }

    // Spec: a single segment longer than maxWindowSeconds becomes its own
    // oversized window rather than being split mid-segment — never violate
    // "never split a segment" even when that means exceeding the nominal cap.
    void segmentLongerThanMax_becomesItsOwnOversizedWindow()
    {
        const std::vector<std::pair<double, double>> segments = {
            {0.0, 400.0}, // longer than max=300
            {450.0, 460.0},
        };
        const auto windows =
            VadChunker::groupSegmentsIntoWindows(segments, 500.0, /*target=*/180.0, /*max=*/300.0);
        QCOMPARE(windows.size(), std::size_t(2));
        QCOMPARE(windows[0].startSeconds, 0.0);
        QVERIFY2(windows[0].endSeconds > 300.0,
                 "the long segment's window must not be truncated mid-segment even past the cap");
    }

    // Spec: windows always partition [0, totalDuration] with no gaps and no
    // overlaps, regardless of how segments are arranged.
    void windowsAlwaysCoverWholeFileWithNoGapsOrOverlaps()
    {
        const std::vector<std::pair<double, double>> segments = {
            {2.0, 8.0}, {50.0, 55.0}, {56.0, 200.0}, {700.0, 705.0},
        };
        const auto windows =
            VadChunker::groupSegmentsIntoWindows(segments, 800.0, /*target=*/120.0, /*max=*/250.0);
        QVERIFY(!windows.empty());
        QCOMPARE(windows.front().startSeconds, 0.0);
        QCOMPARE(windows.back().endSeconds, 800.0);
        for (std::size_t i = 1; i < windows.size(); ++i)
            QCOMPARE(windows[i].startSeconds, windows[i - 1].endSeconds);
    }

    // Spec: computeWindows() degrades gracefully (single whole-file window +
    // a recorded error) when no VAD model path is given — never blocks
    // transcription, same contract as DiarizationEngine's model-unavailable
    // path.
    void computeWindows_noModelPath_fallsBackToSingleWindow()
    {
        std::vector<float> pcm(16000 * 10, 0.0f); // 10s of silence
        QString vadError;
        const auto windows = VadChunker::computeWindows(pcm, QString(), &vadError);
        QCOMPARE(windows.size(), std::size_t(1));
        QCOMPARE(windows.front().startSeconds, 0.0);
        QCOMPARE(windows.front().endSeconds, 10.0);
        QVERIFY2(!vadError.isEmpty(), "a missing VAD model path must be recorded, not silently ignored");
    }

    // Spec: same graceful fallback for a model path that doesn't resolve to
    // a real, loadable VAD model.
    void computeWindows_badModelPath_fallsBackToSingleWindow()
    {
        std::vector<float> pcm(16000 * 5, 0.0f);
        QString vadError;
        const auto windows =
            VadChunker::computeWindows(pcm, QStringLiteral("/nonexistent/vad-model.bin"), &vadError);
        QCOMPARE(windows.size(), std::size_t(1));
        QCOMPARE(windows.front().endSeconds, 5.0);
        QVERIFY(!vadError.isEmpty());
    }

    // Gated real end-to-end run against an actual Silero VAD ggml model, if
    // one is available (set SUOP_TEST_VAD_MODEL) — proves the real whisper.cpp
    // VAD API integration produces sane, ordered windows on real speech, not
    // just the pure-function/fallback paths above.
    void computeWindows_realModel_producesOrderedWindows()
    {
        const QString modelPath = qEnvironmentVariable("SUOP_TEST_VAD_MODEL");
        if (modelPath.isEmpty() || !QFileInfo::exists(modelPath))
            QSKIP("no real Silero VAD ggml model available; set SUOP_TEST_VAD_MODEL to one "
                  "(see third_party/whisper.cpp/models/download-vad-model.sh)");

        std::vector<float> pcm(16000 * 20, 0.0f); // 20s silence — no real speech needed to prove plumbing
        QString vadError;
        const auto windows = VadChunker::computeWindows(pcm, modelPath, &vadError);
        QVERIFY2(vadError.isEmpty(), qPrintable(vadError));
        QVERIFY(!windows.empty());
        QCOMPARE(windows.front().startSeconds, 0.0);
        QCOMPARE(windows.back().endSeconds, 20.0);
    }
};

QTEST_GUILESS_MAIN(TstVadChunking)
#include "tst_vadchunking.moc"
