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
};

QTEST_GUILESS_MAIN(TestTaskChapters)
#include "tst_task_chapters.moc"
