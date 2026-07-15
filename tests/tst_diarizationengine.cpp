// Living spec: DiarizationEngine — real speaker diarization via sherpa-onnx's
// OfflineSpeakerDiarization (pyannote segmentation + WeSpeaker ResNet293-LM
// embedding + agglomerative clustering), see DESIGN.md "Speaker diarization".
//
// Network/model policy: no model is downloaded here. The gated real-model
// test below requires SUOP_TEST_DIARIZATION_SEG_MODEL and
// SUOP_TEST_DIARIZATION_EMBED_MODEL env vars pointing at real ONNX files
// (see ModelManager::ensureDiarizationModels for where the app itself gets
// them) — QSKIPped otherwise, same convention as tst_labelingengine.cpp's
// SUOP_TEST_LABEL_MODEL.

#include <QtTest>

#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QTemporaryDir>

#include "AudioDecoder.h"
#include "DiarizationEngine.h"

namespace sc = sherpa_onnx::cxx;

namespace {

QString locateRealModel(const char *envVar)
{
    const QString path = qEnvironmentVariable(envVar);
    if (!path.isEmpty() && QFileInfo::exists(path))
        return path;
    return {};
}

}

class TstDiarizationEngine : public QObject
{
    Q_OBJECT

private slots:
    // A bogus model path must never crash — isValid() reports false and
    // diarize() returns an empty (not garbage) result, mirroring
    // WhisperCppEngine/LabelingEngine's same "inert, not fatal" contract for
    // an unloadable model.
    void invalidModelPathsAreInertNotFatal()
    {
        DiarizationEngine engine(QStringLiteral("/nonexistent/segmentation.onnx"),
                                  QStringLiteral("/nonexistent/embedding.onnx"));
        QVERIFY(!engine.isValid());

        const std::vector<float> pcm(16000, 0.0f); // 1s of silence
        const std::vector<DiarizedTurn> turns = engine.diarize(pcm);
        QVERIFY(turns.empty());
    }

    // Empty audio must never crash, valid engine or not.
    void emptyAudioProducesNoTurns()
    {
        DiarizationEngine engine(QStringLiteral("/nonexistent/segmentation.onnx"),
                                  QStringLiteral("/nonexistent/embedding.onnx"));
        const std::vector<float> empty;
        QVERIFY(engine.diarize(empty).empty());
    }

    // Regression: a model path that EXISTS but isn't valid ONNX (corrupt/
    // truncated/wrong-format file — e.g. a partial download) makes the
    // underlying ONNX Runtime throw Ort::Exception, which sherpa-onnx does
    // NOT catch internally — confirmed to previously crash the whole
    // process (SIGABRT via std::terminate) before DiarizationEngine's own
    // try/catch was added. No real model weights needed to exercise this —
    // any file that exists but isn't parseable ONNX triggers the same path.
    void corruptModelFileIsInertNotFatal()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString segPath = dir.path() + "/corrupt-segmentation.onnx";
        const QString embedPath = dir.path() + "/corrupt-embedding.onnx";
        for (const QString &path : {segPath, embedPath}) {
            QFile f(path);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write("not a real onnx file");
            f.close();
        }

        DiarizationEngine engine(segPath, embedPath);
        QVERIFY(!engine.isValid());

        const std::vector<float> pcm(16000, 0.0f);
        QVERIFY(engine.diarize(pcm).empty());
    }

    // Full pipeline against the real ONNX models and a real (single-speaker)
    // sample: the segmentation+embedding+clustering pipeline must find at
    // least one speaker turn, with sane sorted, non-overlapping-in-order
    // timing that plausibly covers real speech in the clip.
    void realModels_produceSortedNonEmptyTurns_forJfkSample()
    {
        const QString segModel = locateRealModel("SUOP_TEST_DIARIZATION_SEG_MODEL");
        const QString embedModel = locateRealModel("SUOP_TEST_DIARIZATION_EMBED_MODEL");
        if (segModel.isEmpty() || embedModel.isEmpty())
            QSKIP("no real diarization models available; set SUOP_TEST_DIARIZATION_SEG_MODEL and "
                  "SUOP_TEST_DIARIZATION_EMBED_MODEL to real .onnx files to run this test");
        if (QStandardPaths::findExecutable(QStringLiteral("ffmpeg")).isEmpty())
            QSKIP("ffmpeg not on PATH — needed to decode the sample (DESIGN.md: ffmpeg decodes, Qt only plays)");

        DiarizationEngine engine(segModel, embedModel);
        QVERIFY(engine.isValid());

        QString decodeError;
        auto pcm = AudioDecoder::decodeTo16kMono(QStringLiteral(SUOP_SAMPLES_DIR "/jfk.wav"), &decodeError);
        QVERIFY2(pcm.has_value(), qPrintable(decodeError));

        const std::vector<DiarizedTurn> turns = engine.diarize(*pcm);
        QVERIFY2(!turns.empty(), "expected at least one speaker turn from a real speech sample");

        double prevStart = -1.0;
        for (const DiarizedTurn &turn : turns) {
            QVERIFY2(turn.end > turn.start, "a turn's end must be after its start");
            QVERIFY2(turn.start >= prevStart, "turns must come back sorted by start time");
            QVERIFY2(turn.speaker >= 0, "renumbered speaker ids are 0-based, never negative");
            prevStart = turn.start;
        }
    }

    // Regression: mergeMinorSpeakersIntoNearestMajor() used to judge a
    // cluster "minor" by total duration share of the largest cluster (see
    // its doc comment in DiarizationEngine.cpp) — a real recurring second
    // speaker whose turns are individually short could fall entirely under
    // that cutoff and get silently erased into the dominant speaker
    // (confirmed on a real 68-minute two-person recording that collapsed
    // to a single "Speaker 1" for its ENTIRE length). The fixed criterion
    // is singleton (exactly one turn) cluster count instead: speaker 1
    // here has three short turns totalling far less airtime than speaker
    // 0's two long turns, but both recur (turn count > 1), so both must
    // survive as their own speaker regardless of the airtime imbalance.
    void minorButRecurringSpeakerSurvives()
    {
        std::vector<sc::OfflineSpeakerDiarizationSegment> segments = {
            {0.0f, 100.0f, 0},   // speaker 0: two dominant, long turns
            {150.0f, 250.0f, 0},
            {105.0f, 105.5f, 1}, // speaker 1: three short, recurring turns
            {200.0f, 200.5f, 1},
            {300.0f, 300.5f, 1},
        };
        mergeMinorSpeakersIntoNearestMajor(segments);
        QCOMPARE(segments[0].speaker, 0);
        QCOMPARE(segments[1].speaker, 0);
        QCOMPARE(segments[2].speaker, 1);
        QCOMPARE(segments[3].speaker, 1);
        QCOMPARE(segments[4].speaker, 1);
    }

    // A true singleton (a cluster with exactly one turn, never recurring
    // elsewhere) is the actual noise signature this function targets — it
    // gets folded into whichever major (recurring) speaker has the
    // temporally NEAREST turn. Speaker 2's one turn at [44,46] sits closer
    // to speaker 0's turn at [40,50] (midpoint 45, distance 0) than to
    // either of speaker 1's turns (midpoints 105/145), so it must be
    // reassigned to speaker 0.
    void singletonClusterMergesIntoNearestMajor()
    {
        std::vector<sc::OfflineSpeakerDiarizationSegment> segments = {
            {0.0f, 10.0f, 0},
            {40.0f, 50.0f, 0},
            {44.0f, 46.0f, 2}, // singleton noise cluster
            {100.0f, 110.0f, 1},
            {140.0f, 150.0f, 1},
        };
        mergeMinorSpeakersIntoNearestMajor(segments);
        QCOMPARE(segments[2].speaker, 0);
        // Major speakers' own segments are untouched.
        QCOMPARE(segments[0].speaker, 0);
        QCOMPARE(segments[1].speaker, 0);
        QCOMPARE(segments[3].speaker, 1);
        QCOMPARE(segments[4].speaker, 1);
    }

    // Degenerate case: every cluster is a singleton (e.g. a very short
    // clip with no recurring speaker at all) — there's no major cluster to
    // merge into, so segments must be left exactly as they came in rather
    // than crashing or reassigning to a nonexistent speaker.
    void allSingletonClustersAreLeftUnchanged()
    {
        std::vector<sc::OfflineSpeakerDiarizationSegment> segments = {
            {0.0f, 1.0f, 0},
            {5.0f, 6.0f, 1},
        };
        mergeMinorSpeakersIntoNearestMajor(segments);
        QCOMPARE(segments[0].speaker, 0);
        QCOMPARE(segments[1].speaker, 1);
    }

};

QTEST_GUILESS_MAIN(TstDiarizationEngine)
#include "tst_diarizationengine.moc"
