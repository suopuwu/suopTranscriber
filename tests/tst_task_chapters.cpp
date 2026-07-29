// ChaptersTask, ported to the task-system refactor (.task/plan.md). Only the
// early-exit paths are exercised for real (not-yet-transcribed, missing
// frontmatter, no segments to chapter) — the actual generation path needs a
// real downloaded GGUF model, same "don't exercise the heavy/network path in
// a unit test" precedent as tst_task_relabel.cpp/tst_task_download.cpp.

#include <QFile>
#include <QTemporaryDir>
#include <QUrl>
#include <QtTest>

#include "tasks/ChaptersTask.h"
#include "tasks/EngineCache.h"
#include "tasks/TaskContext.h"

namespace {

struct NullContextFixture {
    EngineCache engines;
    TaskControl control;
    TaskContext ctx{engines, control, [](bool) {}};
};

} // namespace

class TestTaskChapters : public QObject {
    Q_OBJECT
private slots:
    void chaptersTask_reportsKindAndCapabilities()
    {
        ChaptersTask task(QUrl::fromLocalFile("/vault/interview.wav"), QUrl::fromLocalFile("/vault"), 0.5);
        QCOMPARE(task.kind(), QStringLiteral("chapters"));
        QVERIFY(task.isPersistable());
        QVERIFY(task.capabilities().pausable); // pausable BETWEEN windows
    }

    // ChaptersTask must declare needing ONLY the label engine —
    // shared with RelabelTask — so the dispatch-time residency policy leaves
    // it resident across a run of relabel/chapters tasks while still
    // evicting asr/diar.
    void chaptersTask_declaresOnlyLabelNeed()
    {
        ChaptersTask task(QUrl::fromLocalFile("/vault/a.wav"), QUrl::fromLocalFile("/vault"), 0.5);
        const Task::EngineNeeds needs = task.engineNeeds();
        QVERIFY(!needs.asr);
        QVERIFY(needs.label);
        QVERIFY(!needs.diar);
    }

    void chaptersTask_notYetTranscribed_throwsTaskError()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString audioPath = dir.path() + "/never-transcribed.wav";

        ChaptersTask task(QUrl::fromLocalFile(audioPath), QUrl::fromLocalFile(dir.path()), 0.5);
        NullContextFixture fixture;
        bool threw = false;
        try {
            task.run(fixture.ctx);
        } catch (const TaskError &e) {
            threw = true;
            QVERIFY(e.message().contains(QStringLiteral("has not been transcribed")));
        }
        QVERIFY(threw);
    }

    void chaptersTask_missingFrontmatterBlock_throwsTaskError()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString audioPath = dir.path() + "/interview.wav";
        QFile md(audioPath + ".md");
        QVERIFY(md.open(QIODevice::WriteOnly));
        md.write("no frontmatter here\n");
        md.close();

        ChaptersTask task(QUrl::fromLocalFile(audioPath), QUrl::fromLocalFile(dir.path()), 0.5);
        NullContextFixture fixture;
        QVERIFY_EXCEPTION_THROWN(task.run(fixture.ctx), TaskError);
    }

    void chaptersTask_noSegments_throwsTaskError()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString audioPath = dir.path() + "/interview.wav";
        QFile md(audioPath + ".md");
        QVERIFY(md.open(QIODevice::WriteOnly));
        md.write("---\nmodel: ggml-base.en.bin\nspeakerDetection: false\n---\n\n");
        md.close();

        ChaptersTask task(QUrl::fromLocalFile(audioPath), QUrl::fromLocalFile(dir.path()), 0.5);
        NullContextFixture fixture;
        bool threw = false;
        try {
            task.run(fixture.ctx);
        } catch (const TaskError &e) {
            threw = true;
            QVERIFY(e.message().contains(QStringLiteral("no transcript text")));
        }
        QVERIFY(threw);
    }

    // withOpeningChapter() is the pure decision half of "a transcript always
    // opens under a chapter heading" — generateChapters() structurally can't
    // propose that chapter (told to skip the first one), so ChaptersTask
    // synthesizes it. The generation path itself needs a real GGUF (see this
    // file's header comment); these exercise the prepend rules directly.

    void withOpeningChapter_noBoundaries_producesSingleOpeningChapter()
    {
        // The single-topic case: the model proposed nothing, and the file
        // must still come out with exactly one chapter covering all of it.
        const QList<Chapter> result = withOpeningChapter({}, QStringLiteral("Packing the truck"));
        QCOMPARE(result.size(), 1);
        QCOMPARE(result.first().afterSegmentIndex, -1);
        QCOMPARE(result.first().title, QStringLiteral("Packing the truck"));
    }

    void withOpeningChapter_prependsBeforeExistingBoundaries()
    {
        const QList<Chapter> input{{3, QStringLiteral("B")}, {9, QStringLiteral("C")}};
        const QList<Chapter> result = withOpeningChapter(input, QStringLiteral("A"));
        QCOMPARE(result.size(), 3);
        QCOMPARE(result.at(0).afterSegmentIndex, -1);
        QCOMPARE(result.at(0).title, QStringLiteral("A"));
        QCOMPARE(result.at(1).afterSegmentIndex, 3);
        QCOMPARE(result.at(1).title, QStringLiteral("B"));
        QCOMPARE(result.at(2).afterSegmentIndex, 9);
        QCOMPARE(result.at(2).title, QStringLiteral("C"));
    }

    void withOpeningChapter_blankTitle_leavesChaptersUnchanged()
    {
        // The title pass is non-fatal: an untitled opening stretch beats a
        // "## [0]" marker with no title after it.
        const QList<Chapter> input{{3, QStringLiteral("B")}};
        for (const QString &blank : {QString(), QStringLiteral("   \t ")}) {
            const QList<Chapter> result = withOpeningChapter(input, blank);
            QCOMPARE(result.size(), 1);
            QCOMPARE(result.first().afterSegmentIndex, 3);
        }
    }

    void withOpeningChapter_alreadyOpensAtSegmentZero_leavesUnchanged()
    {
        // Two chapters sharing t0 would splice two markers at one offset.
        const QList<Chapter> input{{-1, QStringLiteral("Already first")}, {4, QStringLiteral("B")}};
        const QList<Chapter> result = withOpeningChapter(input, QStringLiteral("Another"));
        QCOMPARE(result.size(), 2);
        QCOMPARE(result.first().title, QStringLiteral("Already first"));
    }

    // The progress bar is SelfCumulative — the fraction ChaptersTask emits IS
    // the bar, so the band math is the whole smoothness fix. Pure, no model.

    void chaptersGenerateProgress_rampsWithinAWindowAndAcrossWindows()
    {
        // Monotone in tokens for a fixed window...
        double previous = -1.0;
        for (int tokens : {0, 8, 40, 120, 400, 2000}) {
            const double p = chaptersGenerateProgress(1, 4, tokens);
            QVERIFY2(p > previous, qPrintable(QStringLiteral("not monotone at tokens=%1").arg(tokens)));
            previous = p;
        }
        // ...and monotone across windows at a fixed token count.
        previous = -1.0;
        for (int w = 0; w < 4; ++w) {
            const double p = chaptersGenerateProgress(w, 4, 50);
            QVERIFY(p > previous);
            previous = p;
        }
    }

    void chaptersGenerateProgress_neverExceedsTheGenerateBand()
    {
        // Even at an absurd token count the ramp must not spill into the
        // merge band — that would make the bar jump backwards at the handoff.
        QVERIFY(chaptersGenerateProgress(3, 4, 1000000) <= kChaptersGenerateBand);
        QVERIFY(chaptersGenerateProgress(0, 1, 1000000) <= kChaptersGenerateBand);
    }

    void chaptersGenerateProgress_zeroTokensIsExactlyTheWindowLowerEdge()
    {
        for (int w = 0; w < 5; ++w)
            QCOMPARE(chaptersGenerateProgress(w, 5, 0), kChaptersGenerateBand * w / 5);
    }

    void chaptersGenerateProgress_neverDipsBelowThePreviousWindowSnap()
    {
        // ChaptersTask snaps to the window's upper edge when a window
        // returns, then the NEXT window's ramp starts from its own lower
        // edge. Those two must line up or the bar goes backwards.
        const int windows = 4;
        for (int w = 1; w < windows; ++w) {
            const double previousSnap = kChaptersGenerateBand * w / windows;
            QVERIFY2(chaptersGenerateProgress(w, windows, 0) >= previousSnap,
                      qPrintable(QStringLiteral("bar goes backwards entering window %1").arg(w)));
        }
    }

    void chaptersGenerateProgress_zeroWindowsDoesNotDivideByZero()
    {
        QCOMPARE(chaptersGenerateProgress(0, 0, 100), 0.0);
        QCOMPARE(chaptersGenerateProgress(2, -1, 100), 0.0);
    }

    void withOpeningChapter_trimsTitle()
    {
        // The raw model string is what gets passed in.
        const QList<Chapter> result = withOpeningChapter({}, QStringLiteral("  Setting up camp  "));
        QCOMPARE(result.size(), 1);
        QCOMPARE(result.first().title, QStringLiteral("Setting up camp"));
    }
};

QTEST_GUILESS_MAIN(TestTaskChapters)
#include "tst_task_chapters.moc"
