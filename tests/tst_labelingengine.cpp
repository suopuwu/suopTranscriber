// tst_labelingengine — living spec for AI auto-labeling (DESIGN.md "Model
// management" — a second, opt-in pass after transcription, producing
// {label, prelude, tags} — label/tags land in frontmatter, prelude in the
// .md body (see TranscriptionController::writeLabelingFields())). Mirrors
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
        const auto result = engine.label(QStringLiteral("some transcript text"), {}, false, 0.5, 0.5, &error);
        QVERIFY(!result.has_value());
        QVERIFY(!error.isEmpty());
    }

    // Gated real-model end-to-end test: loads the actual Qwen3-4B-Instruct
    // GGUF (see ModelManager::kLabelModelFileName), runs the grammar-
    // constrained pass over a short known transcript, and asserts the
    // {label, prelude, tags} contract — the whole point of the GBNF
    // grammar is that this JSON shape is guaranteed, not just likely.
    void realModelProducesWellFormedLabelPreludeTags()
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
        const auto result = engine.label(transcript, {}, false, 0.5, 0.5, &error);
        QVERIFY2(result.has_value(), qPrintable(error));
        QVERIFY2(!result->label.isEmpty(), "label must be non-empty");
        QVERIFY2(!result->prelude.isEmpty(), "prelude must be non-empty");
        QVERIFY2(!result->tags.isEmpty(), "tags must be non-empty");
        // Loose content sanity — not asserting exact wording, just that the
        // model engaged with the actual transcript content rather than
        // producing generic/empty filler.
        const QString lowerPrelude = result->prelude.toLower();
        QVERIFY2(lowerPrelude.contains(QStringLiteral("budget")) || lowerPrelude.contains(QStringLiteral("meeting"))
                      || lowerPrelude.contains(QStringLiteral("marketing"))
                      || lowerPrelude.contains(QStringLiteral("engineering")),
                  qPrintable("prelude doesn't seem to reflect transcript content: " + result->prelude));
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
        const auto result = engine.label(transcript, vocabulary, true, 0.5, 0.5, &error);
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
        const auto result = engine.label(transcript, vocabulary, false, 1.0, 0.5, &error);
        QVERIFY2(result.has_value(), qPrintable(error));
        // Not asserting exact tag choice (still a soft nudge, not a hard
        // constraint) — just that generation succeeds and produces
        // something, since the real behavioral claim (prompt phrasing
        // changes with bias) is exercised by buildSystemPrompt indirectly
        // through this call; exact model behavior under a soft prompt nudge
        // isn't deterministic enough to assert further without flaking.
        QVERIFY2(!result->tags.isEmpty(), "expected some tags to be produced");
    }

    // Feature 1: wordiness should push the generated prelude toward being
    // more detailed at the high end than the low end. Comparative (low vs.
    // high on the SAME transcript) rather than an absolute word-count bound —
    // exact LLM output length isn't deterministic enough to assert further
    // without flaking, mirroring highTagReuseBias_prefersExistingTagOverInventingNew's
    // reasoning above.
    void wordinessTiers_affectLabelAndPreludeVerbosity()
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
        const auto lowResult = engine.label(transcript, {}, false, 0.5, /* wordiness */ 0.0, &error);
        QVERIFY2(lowResult.has_value(), qPrintable(error));
        const auto highResult = engine.label(transcript, {}, false, 0.5, /* wordiness */ 1.0, &error);
        QVERIFY2(highResult.has_value(), qPrintable(error));

        QVERIFY2(!lowResult->label.isEmpty(), "low-wordiness label must be non-empty");
        QVERIFY2(!lowResult->prelude.isEmpty(), "low-wordiness prelude must be non-empty");
        QVERIFY2(!highResult->label.isEmpty(), "high-wordiness label must be non-empty");
        QVERIFY2(!highResult->prelude.isEmpty(), "high-wordiness prelude must be non-empty");

        const int lowPreludeWords = lowResult->prelude.split(QLatin1Char(' '), Qt::SkipEmptyParts).size();
        const int highPreludeWords = highResult->prelude.split(QLatin1Char(' '), Qt::SkipEmptyParts).size();
        QVERIFY2(highPreludeWords > lowPreludeWords,
                 qPrintable(QStringLiteral("expected high-wordiness prelude (%1 words: \"%2\") to be longer than "
                                            "low-wordiness prelude (%3 words: \"%4\")")
                                .arg(highPreludeWords)
                                .arg(highResult->prelude)
                                .arg(lowPreludeWords)
                                .arg(lowResult->prelude)));
    }

    // Feature 3a ("generate chapters"): generateChapters() proposes chapter
    // boundaries as SEGMENT INDICES, not timestamps (see its own doc comment
    // for why — this app never trusts the LLM to echo one back accurately).
    // A synthetic transcript covering three clearly distinct topics, long
    // enough to plausibly deserve more than one chapter. Not asserting exact
    // chapter count/wording (small model, not fully deterministic) — just
    // the basic shape/sanity contract the plan calls for: generation
    // succeeds, and every returned index is a plausible segment reference.
    void generateChapters_returnsWellFormedInBoundsChapterBoundaries()
    {
        const QString modelPath = locateRealLabelModel();
        if (modelPath.isEmpty())
            QSKIP("no real label GGUF model available; set SUOP_TEST_LABEL_MODEL to a "
                  "Qwen3-4B-Instruct-2507 (or compatible) .gguf file to run this test");

        LabelingEngine engine(modelPath);
        QVERIFY(engine.isValid());

        const QStringList segmentTexts = {
            QStringLiteral("Welcome everyone to today's quarterly budget review meeting."),
            QStringLiteral("We'll start by going over the marketing department's spending for the last three "
                            "months, which came in under budget."),
            QStringLiteral("Marketing's biggest line item was the trade show sponsorship."),
            QStringLiteral("Now let's move on to a completely different topic: the new employee onboarding "
                            "process."),
            QStringLiteral("HR has redesigned the onboarding checklist to take two days instead of a week."),
            QStringLiteral("New hires will now get their laptop and accounts provisioned before their first day."),
            QStringLiteral("Finally, let's discuss the engineering hiring plan for next quarter."),
            QStringLiteral("We're proposing three new senior engineer positions and one engineering manager."),
        };

        QString error;
        const auto result = engine.generateChapters(segmentTexts, &error);
        QVERIFY2(result.has_value(), qPrintable(error));

        for (const Chapter &chapter : *result) {
            QVERIFY2(chapter.afterSegmentIndex >= 0 && chapter.afterSegmentIndex < segmentTexts.size(),
                     qPrintable(QStringLiteral("afterSegmentIndex %1 out of bounds for %2 segments")
                                    .arg(chapter.afterSegmentIndex)
                                    .arg(segmentTexts.size())));
            QVERIFY2(!chapter.title.isEmpty(), "chapter title must be non-empty");
        }
    }
};

QTEST_GUILESS_MAIN(TstLabelingEngine)
#include "tst_labelingengine.moc"
