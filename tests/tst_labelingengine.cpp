// tst_labelingengine — living spec for AI auto-labeling (DESIGN.md "Model
// management" — a second, opt-in pass after transcription, producing
// {label, summary, tags} written into the transcript's frontmatter). Mirrors
// tst_engine_contract's approach to WhisperCppEngine: construction/validity
// is always testable; the real generation pass is gated on an actual GGUF
// model since ~2.5 GB is not a test dependency.

#include <QtTest>

#include <QFileInfo>

#include "LabelingEngine.h"

namespace {

// Locate a REAL label GGUF model for the gated end-to-end test: only the
// env override, since (unlike whisper's small models) there's no per-vault
// default location convention to fall back on yet, and the dummy
// for-tests-*.bin whisper models are a different format entirely (not GGUF
// llama.cpp can load).
QString locateRealLabelModel()
{
    const QString envModel = qEnvironmentVariable("SUOP_TEST_LABEL_MODEL");
    if (!envModel.isEmpty() && QFileInfo::exists(envModel))
        return envModel;
    return {};
}

}

class TstLabelingEngine : public QObject
{
    Q_OBJECT

private slots:
    // Spec: an invalid model path must not crash the engine (mirrors
    // WhisperCppEngine's isValid()==false contract) — this is always
    // testable, no GGUF file needed.
    void invalidModelPathIsInertNotFatal()
    {
        LabelingEngine engine(QStringLiteral("/nonexistent/path/model.gguf"));
        QVERIFY(!engine.isValid());

        QString error;
        const auto result = engine.label(QStringLiteral("some transcript text"), &error);
        QVERIFY(!result.has_value());
        QVERIFY(!error.isEmpty());
    }

    // Gated real-model end-to-end test: loads the actual Qwen3-4B-Instruct
    // GGUF (see ModelManager::kLabelModelFileName), runs the grammar-
    // constrained pass over a short known transcript, and asserts the
    // {label, summary, tags} contract — the whole point of the GBNF
    // grammar is that this JSON shape is guaranteed, not just likely.
    void realModelProducesWellFormedLabelSummaryTags()
    {
        const QString modelPath = locateRealLabelModel();
        if (modelPath.isEmpty())
            QSKIP("no real label GGUF model available; set SUOP_TEST_LABEL_MODEL to a "
                  "Qwen3-4B-Instruct-2507 (or compatible) .gguf file to run this test");

        LabelingEngine engine(modelPath);
        QVERIFY(engine.isValid());

        const QString transcript = QStringLiteral(
            "[00:00:00.000 - 00:00:05.000] Welcome everyone to today's quarterly budget review meeting.\n"
            "[00:00:05.000 - 00:00:12.000] We'll start by going over the marketing department's spending "
            "for the last three months, which came in under budget.\n"
            "[00:00:12.000 - 00:00:20.000] After that we'll discuss the proposed increase to the engineering "
            "hiring budget for next quarter.");

        QString error;
        const auto result = engine.label(transcript, &error);
        QVERIFY2(result.has_value(), qPrintable(error));
        QVERIFY2(!result->label.isEmpty(), "label must be non-empty");
        QVERIFY2(!result->summary.isEmpty(), "summary must be non-empty");
        QVERIFY2(!result->tags.isEmpty(), "tags must be non-empty");
        // Loose content sanity — not asserting exact wording, just that the
        // model engaged with the actual transcript content rather than
        // producing generic/empty filler.
        const QString lowerSummary = result->summary.toLower();
        QVERIFY2(lowerSummary.contains(QStringLiteral("budget")) || lowerSummary.contains(QStringLiteral("meeting"))
                      || lowerSummary.contains(QStringLiteral("marketing"))
                      || lowerSummary.contains(QStringLiteral("engineering")),
                  qPrintable("summary doesn't seem to reflect transcript content: " + result->summary));
    }
};

QTEST_GUILESS_MAIN(TstLabelingEngine)
#include "tst_labelingengine.moc"
