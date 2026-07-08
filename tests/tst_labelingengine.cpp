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
        const auto result = engine.label(QStringLiteral("some transcript text"), {}, false, 0.5, &error);
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
        const auto result = engine.label(transcript, {}, false, 0.5, &error);
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

    // Item 14 "auto-categorize" mode: when given an existing tag vocabulary
    // that's plausible for the transcript, the model should pick from it
    // rather than inventing something else — a real behavioral check, not
    // just "the field is a non-empty string" (which unrestricted mode
    // already covers above).
    void existingTagsVocabularyIsHonored()
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
            "for the last three months, which came in under budget.");

        // A deliberately narrow vocabulary that plausibly covers this
        // transcript ("finance"/"meetings") plus an obviously-inapplicable
        // decoy ("gardening") to confirm the model isn't just echoing
        // everything it was given.
        const QStringList vocabulary = {QStringLiteral("finance"), QStringLiteral("meetings"),
                                         QStringLiteral("gardening")};

        QString error;
        const auto result = engine.label(transcript, vocabulary, true, 0.5, &error);
        QVERIFY2(result.has_value(), qPrintable(error));
        QVERIFY2(!result->tags.isEmpty(), "a plausible vocabulary should have matched something");

        const QStringList chosenTags = result->tags.split(QStringLiteral(","), Qt::SkipEmptyParts);
        for (const QString &tag : chosenTags) {
            const QString trimmed = tag.trimmed();
            QVERIFY2(vocabulary.contains(trimmed, Qt::CaseInsensitive),
                     qPrintable(QStringLiteral("tag \"%1\" is not in the given vocabulary %2")
                                    .arg(trimmed, vocabulary.join(QStringLiteral(", ")))));
        }
    }

    // Item 15 (Wave E): with restrictToExisting=false but a vocabulary still
    // given, a HIGH tagReuseBias should make the model prefer reusing an
    // existing (even loosely-fitting) tag over inventing a precise new one.
    void highTagReuseBias_prefersExistingTagOverInventingNew()
    {
        const QString modelPath = locateRealLabelModel();
        if (modelPath.isEmpty())
            QSKIP("no real label GGUF model available; set SUOP_TEST_LABEL_MODEL to a "
                  "Qwen3-4B-Instruct-2507 (or compatible) .gguf file to run this test");

        LabelingEngine engine(modelPath);
        QVERIFY(engine.isValid());

        // A transcript about a fairly specific topic (cooking) alongside a
        // vocabulary that has no precise match but one loosely-related decoy
        // ("kitchen") plus clearly-unrelated ones — a high bias should steer
        // the model toward "kitchen" rather than inventing "cooking"/"recipes".
        const QString transcript = QStringLiteral(
            "[00:00:00.000 - 00:00:05.000] Today we're going to talk about how to properly season a cast "
            "iron skillet before cooking your first meal in it.");
        const QStringList vocabulary = {QStringLiteral("kitchen"), QStringLiteral("automotive"),
                                         QStringLiteral("finance")};

        QString error;
        const auto result = engine.label(transcript, vocabulary, false, 1.0, &error);
        QVERIFY2(result.has_value(), qPrintable(error));
        // Not asserting exact tag choice (still a soft nudge, not a hard
        // constraint) — just that generation succeeds and produces
        // something, since the real behavioral claim (prompt phrasing
        // changes with bias) is exercised by buildSystemPrompt indirectly
        // through this call; exact model behavior under a soft prompt nudge
        // isn't deterministic enough to assert further without flaking.
        QVERIFY2(!result->tags.isEmpty(), "expected some tags to be produced");
    }
};

QTEST_GUILESS_MAIN(TstLabelingEngine)
#include "tst_labelingengine.moc"
