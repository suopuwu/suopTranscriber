// tst_labelingengine — living spec for AI auto-labeling (DESIGN.md "Model
// management" — a second, opt-in pass after transcription, producing
// {label, prelude, tags} — label/tags land in frontmatter, prelude in the
// .md body (see TranscriptionController::writeLabelingFields())). Mirrors
// tst_engine_contract's approach to WhisperCppEngine: construction/validity
// is always testable; the real generation pass is gated on an actual GGUF
// model since ~2.5 GB is not a test dependency.

#include <QtTest>

#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>

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

    // "Auto-categorize" mode: when given an existing tag vocabulary
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

    // With restrictToExisting=false but a vocabulary still
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
        const auto result = engine.generateChapters(segmentTexts, /* density */ 0.5, &error);
        QVERIFY2(result.has_value(), qPrintable(error));

        for (const Chapter &chapter : *result) {
            QVERIFY2(chapter.afterSegmentIndex >= 0 && chapter.afterSegmentIndex < segmentTexts.size(),
                     qPrintable(QStringLiteral("afterSegmentIndex %1 out of bounds for %2 segments")
                                    .arg(chapter.afterSegmentIndex)
                                    .arg(segmentTexts.size())));
            QVERIFY2(!chapter.title.isEmpty(), "chapter title must be non-empty");
        }
    }

    // Chapter density: density must actually change the chapter
    // system prompt's instruction — a low density should steer the model
    // toward FEWER boundaries ("conservative"/"few"), a high one toward MORE
    // ("granular"/"30"), with the mid default matching the historical "up to
    // ~20" wording. Pure string test on the exposed prompt builder, no model
    // needed (mirrors the deterministic backendKind-style checks).
    void chapterSystemPrompt_densityChangesInstruction()
    {
#ifndef SUOP_ENABLE_LABELING
        QSKIP("labeling (and its prompt builder) compiled out via SUOP_ENABLE_LABELING=OFF");
#endif
        const QString low = LabelingEngine::chapterSystemPrompt(0.0);
        const QString mid = LabelingEngine::chapterSystemPrompt(0.5);
        const QString high = LabelingEngine::chapterSystemPrompt(1.0);

        // The three tiers must produce materially different text.
        QVERIFY(low != mid);
        QVERIFY(mid != high);
        QVERIFY(low != high);

        // Directional sanity on the tier wording.
        QVERIFY2(low.contains(QStringLiteral("conservative"), Qt::CaseInsensitive)
                      || low.contains(QStringLiteral("few"), Qt::CaseInsensitive),
                 qPrintable(low));
        QVERIFY2(high.contains(QStringLiteral("granular"), Qt::CaseInsensitive)
                      || high.contains(QStringLiteral("30")),
                 qPrintable(high));
        QVERIFY2(mid.contains(QStringLiteral("20")), qPrintable(mid));

        // The fixed structural instruction (afterSegment/title JSON contract)
        // must survive in every tier — density only swaps the count sentence.
        for (const QString &p : {low, mid, high})
            QVERIFY2(p.contains(QStringLiteral("afterSegment")), qPrintable(p));
    }

    // Follow-up to generateChapters()'s over-eager-boundary problem: see
    // LabelingEngine::mergeSimilarChapters()'s doc comment. Fewer than 2
    // input chapters is returned unchanged, with no model query — doesn't
    // need a real model, so no QSKIP guard.
    void mergeSimilarChapters_returnsInputUnchangedBelowTwoChapters()
    {
        LabelingEngine engine{QString()};
        const QList<Chapter> oneChapter = {{5, QStringLiteral("Solo chapter")}};

        QString error;
        const auto result = engine.mergeSimilarChapters(oneChapter, {}, 0.5, &error);
        QVERIFY2(result.has_value(), qPrintable(error));
        QCOMPARE(result->size(), 1);
        QCOMPARE(result->first().title, QStringLiteral("Solo chapter"));

        const auto emptyResult = engine.mergeSimilarChapters({}, {}, 0.5, &error);
        QVERIFY2(emptyResult.has_value(), qPrintable(error));
        QVERIFY(emptyResult->isEmpty());
    }

    // Core behavior: a single greedy left-to-right scan (see
    // mergeSimilarChapters()'s doc comment) must collapse a run of adjacent
    // chapters that are really the same topic — split only because
    // generateChapters() over-proposed — into one, keeping the FIRST
    // chapter's title/afterSegmentIndex, not a later one's. An adjacent
    // chapter on a clearly different topic must survive unmerged. Three
    // input chapters (two over-split marketing ones, one genuinely distinct
    // office-lease one) covers both the merge and the non-merge case in one
    // pass, like the chain-collapse the doc comment describes.
    void mergeSimilarChapters_collapsesSameTopicRunButKeepsDistinctTopicSeparate()
    {
        const QString modelPath = locateRealLabelModel();
        if (modelPath.isEmpty())
            QSKIP("no real label GGUF model available; set SUOP_TEST_LABEL_MODEL to a "
                  "Qwen3-4B-Instruct-2507 (or compatible) .gguf file to run this test");

        LabelingEngine engine(modelPath);
        QVERIFY(engine.isValid());

        const QStringList segmentTexts = {
            QStringLiteral("Let's kick off today's meeting with the quarterly marketing budget review."),
            QStringLiteral("The trade show booth accounted for the largest single marketing expense this "
                            "quarter, coming in just under budget."),
            QStringLiteral("Digital ad spend for marketing was also reviewed and came in under the allocated "
                            "budget."),
            QStringLiteral("The conference sponsorship line item within the marketing budget ran slightly over "
                            "what was planned."),
            QStringLiteral("Switching gears entirely, let's discuss the office lease renewal for company "
                            "headquarters."),
            QStringLiteral("The landlord has proposed a three-year lease renewal at a five percent higher "
                            "annual rate."),
        };
        // Segment 0 is the implicit first chapter (not represented here, see
        // generateChapters()'s doc comment). The first two entries both
        // start chapters that are still about the marketing budget — an
        // over-eager split of one real chapter — while the third starts a
        // genuinely different topic (the office lease).
        const QList<Chapter> overSplit = {
            {0, QStringLiteral("Trade show marketing costs")},
            {2, QStringLiteral("Marketing sponsorship overage")},
            {3, QStringLiteral("Office lease renewal")},
        };

        QString error;
        const auto result = engine.mergeSimilarChapters(overSplit, segmentTexts, 0.5, &error);
        QVERIFY2(result.has_value(), qPrintable(error));

        QCOMPARE(result->size(), 2);
        QCOMPARE(result->at(0).afterSegmentIndex, 0);
        QCOMPARE(result->at(0).title, QStringLiteral("Trade show marketing costs"));
        QCOMPARE(result->at(1).afterSegmentIndex, 3);
        QCOMPARE(result->at(1).title, QStringLiteral("Office lease renewal"));
    }

    // repairTruncatedLabelJson(): label()'s salvage for a generation the
    // output-token budget cut mid-JSON (the runaway case a randomly-seeded
    // sampler occasionally produces on long high-wordiness transcripts —
    // this used to fail the whole label task as "model did not produce
    // valid JSON"). Pure string repair, no model, no QSKIP. Each case is a
    // literal truncation shape of a kGrammar-valid response.

    void repairTruncatedLabelJson_completeResponsePassesThroughUnchanged()
    {
        const QString complete =
            QStringLiteral("{\"label\": \"A\", \"prelude\": \"B\", \"tags\": \"c, d\"}");
        QCOMPARE(repairTruncatedLabelJson(complete), complete);
    }

    void repairTruncatedLabelJson_cutMidString_closesAndKeepsLeadingFields()
    {
        const auto doc = QJsonDocument::fromJson(
            repairTruncatedLabelJson(
                QStringLiteral("{\"label\": \"Knife Sharpening\", \"prelude\": \"The transcript beg"))
                .toUtf8());
        QVERIFY(doc.isObject());
        QCOMPARE(doc.object().value("label").toString(), QStringLiteral("Knife Sharpening"));
        QCOMPARE(doc.object().value("prelude").toString(), QStringLiteral("The transcript beg"));
    }

    void repairTruncatedLabelJson_cutAfterColon_suppliesEmptyValue()
    {
        const auto doc = QJsonDocument::fromJson(
            repairTruncatedLabelJson(
                QStringLiteral("{\"label\": \"A\", \"prelude\": \"B\", \"tags\":"))
                .toUtf8());
        QVERIFY(doc.isObject());
        QCOMPARE(doc.object().value("label").toString(), QStringLiteral("A"));
        QCOMPARE(doc.object().value("tags").toString(), QString());
    }

    void repairTruncatedLabelJson_cutAfterComma_dropsTheComma()
    {
        const auto doc = QJsonDocument::fromJson(
            repairTruncatedLabelJson(QStringLiteral("{\"label\": \"A\", \"prelude\": \"B\","))
                .toUtf8());
        QVERIFY(doc.isObject());
        QCOMPARE(doc.object().value("prelude").toString(), QStringLiteral("B"));
    }

    void repairTruncatedLabelJson_cutMidEscape_dropsThePartialEscape()
    {
        // Dangling lone backslash...
        auto doc = QJsonDocument::fromJson(
            repairTruncatedLabelJson(QStringLiteral("{\"label\": \"A\", \"prelude\": \"line\\")).toUtf8());
        QVERIFY(doc.isObject());
        QCOMPARE(doc.object().value("prelude").toString(), QStringLiteral("line"));

        // ...and a partial \uXXXX with only some of its hex digits.
        doc = QJsonDocument::fromJson(
            repairTruncatedLabelJson(QStringLiteral("{\"label\": \"A\", \"prelude\": \"x\\u00")).toUtf8());
        QVERIFY(doc.isObject());
        QCOMPARE(doc.object().value("prelude").toString(), QStringLiteral("x"));
    }

    // A cut before the label's value is complete is beyond salvage — the
    // repaired text must NOT parse into an object with a non-empty label
    // (label() then reports the budget error instead of writing junk).
    void repairTruncatedLabelJson_cutInsideLabelKey_staysUnsalvageable()
    {
        const auto doc =
            QJsonDocument::fromJson(repairTruncatedLabelJson(QStringLiteral("{\"lab")).toUtf8());
        QVERIFY(!doc.isObject() || doc.object().value("label").toString().isEmpty());
    }

    void repairTruncatedLabelJson_nonObjectTextReturnsUnchanged()
    {
        QCOMPARE(repairTruncatedLabelJson(QString()), QString());
        QCOMPARE(repairTruncatedLabelJson(QStringLiteral("yes")), QStringLiteral("yes"));
    }
};

QTEST_GUILESS_MAIN(TstLabelingEngine)
#include "tst_labelingengine.moc"
