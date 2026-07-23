// RelabelTask, ported to the task-system refactor (.task/plan.md). Only the
// early-exit paths are exercised for real (not-yet-transcribed, missing
// frontmatter, empty transcript text, skipIfAlreadyLabeled short-circuit) —
// none of these touch EngineCache::ensureLabel(), so no network/real LLM
// model is needed. The actual labeling generation path (ensureLabel() +
// LabelingEngine::label()) needs a real downloaded GGUF model and is left
// untested here, same "don't exercise the heavy/network path in a unit test"
// precedent as tst_task_download.cpp.

#include <QFile>
#include <QTemporaryDir>
#include <QUrl>
#include <QtTest>

#include "tasks/EngineCache.h"
#include "tasks/RelabelTask.h"
#include "tasks/TaskContext.h"

namespace {

struct NullContextFixture {
    EngineCache engines;
    TaskControl control;
    TaskContext ctx{engines, control, [](bool) {}};
};

} // namespace

class TestTaskRelabel : public QObject {
    Q_OBJECT
private slots:
    void relabelTask_reportsKindAndPersistence()
    {
        RelabelTask task(QUrl::fromLocalFile("/vault/interview.wav"), QUrl::fromLocalFile("/vault"), false, 0.5, 0.5, false);
        QCOMPARE(task.kind(), QStringLiteral("relabel"));
        QVERIFY(task.isPersistable());
        QVERIFY(!task.capabilities().pausable); // no interruption point
    }

    void relabelTask_notYetTranscribed_throwsTaskError()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString audioPath = dir.path() + "/never-transcribed.wav";

        RelabelTask task(QUrl::fromLocalFile(audioPath), QUrl::fromLocalFile(dir.path()), false, 0.5, 0.5, false);
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

    void relabelTask_missingFrontmatterBlock_throwsTaskError()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString audioPath = dir.path() + "/interview.wav";
        QFile md(audioPath + ".md");
        QVERIFY(md.open(QIODevice::WriteOnly));
        md.write("no frontmatter here, just body text\n");
        md.close();

        RelabelTask task(QUrl::fromLocalFile(audioPath), QUrl::fromLocalFile(dir.path()), false, 0.5, 0.5, false);
        NullContextFixture fixture;
        QVERIFY_EXCEPTION_THROWN(task.run(fixture.ctx), TaskError);
    }

    void relabelTask_emptyTranscriptText_throwsTaskError()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString audioPath = dir.path() + "/interview.wav";
        QFile md(audioPath + ".md");
        QVERIFY(md.open(QIODevice::WriteOnly));
        // Frontmatter + body with only a timestamp, no spoken text.
        md.write("---\nmodel: ggml-base.en.bin\nspeakerDetection: false\n---\n\n");
        md.close();

        RelabelTask task(QUrl::fromLocalFile(audioPath), QUrl::fromLocalFile(dir.path()), false, 0.5, 0.5, false);
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

    void relabelTask_skipIfAlreadyLabeled_existingLabel_returnsWithoutThrowing()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString audioPath = dir.path() + "/interview.wav";
        QFile md(audioPath + ".md");
        QVERIFY(md.open(QIODevice::WriteOnly));
        md.write("---\nmodel: ggml-base.en.bin\nspeakerDetection: false\nlabel: Already Labeled\n---\n\n"
                 "[0] hello there\n");
        md.close();

        // skipIfAlreadyLabeled=true + an existing label -> success, no LLM
        // call attempted (which would otherwise need a real model).
        RelabelTask task(QUrl::fromLocalFile(audioPath), QUrl::fromLocalFile(dir.path()), false, 0.5, 0.5, /*skipIfAlreadyLabeled=*/true);
        NullContextFixture fixture;
        task.run(fixture.ctx); // must not throw
    }

    // The progress bar is SelfCumulative — the fraction RelabelTask emits IS
    // the bar, so this ramp is the whole smoothness fix. Pure, no model.
    // Mirrors tst_task_chapters.cpp's chaptersGenerateProgress_* tests.

    void labelGenerateProgress_rampsMonotonicallyWithTokens()
    {
        double previous = -1.0;
        for (int tokens : {0, 8, 40, 120, 400, 2000}) {
            const double p = labelGenerateProgress(tokens);
            QVERIFY2(p > previous, qPrintable(QStringLiteral("not monotone at tokens=%1").arg(tokens)));
            previous = p;
        }
    }

    void labelGenerateProgress_zeroTokensIsZero() { QCOMPARE(labelGenerateProgress(0), 0.0); }

    void labelGenerateProgress_neverReachesOne() { QVERIFY(labelGenerateProgress(1000000) <= 0.97); }

    void labelGenerateProgress_negativeTokensClampToZero() { QCOMPARE(labelGenerateProgress(-5), 0.0); }
};

QTEST_GUILESS_MAIN(TestTaskRelabel)
#include "tst_task_relabel.moc"
