// Second kind ported to the task-system refactor (.task/plan.md), after the
// download pilot: RenameSpeakerTask/MergeSpeakersTask. Unlike download, these
// are pure file I/O (no network/GPU), so — unlike tst_task_download.cpp —
// this test DOES call run() for real, verifying the actual rewrite through
// the newly-exposed TranscriptionController.h helpers (renameSpeakerInFile/
// mergeSpeakersInFile/sidecarPath), not just the task's static interface.

#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QUrl>
#include <QtTest>

#include "tasks/MergeSpeakersTask.h"
#include "tasks/RenameSpeakerTask.h"
#include "tasks/TaskContext.h"
#include "tasks/EngineCache.h"

namespace {

QString writeTaggedMd(const QString &dirPath, const QString &audioName, const QString &body)
{
    const QString path = dirPath + QStringLiteral("/") + audioName + QStringLiteral(".md");
    QFile md(path);
    md.open(QIODevice::WriteOnly | QIODevice::Text);
    md.write(("---\nmodel: ggml-base.en.bin\nspeakerDetection: true\nspeakers: Speaker 1, Speaker 2\n---\n\n" + body)
                  .toUtf8());
    md.close();
    return path;
}

// A no-op TaskContext for kinds (like rename/merge) that never touch
// engines or the pause/cancel checkpoint — mirrors run()'s Q_UNUSED(ctx).
struct NullContextFixture {
    EngineCache engines;
    TaskControl control;
    TaskContext ctx{engines, control, [](bool) {}};
};

} // namespace

class TestTaskSpeakerEdits : public QObject {
    Q_OBJECT
private slots:
    void renameSpeakerTask_rewritesBodyTagsAndFrontmatter_andReportsSuccess()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString mdPath =
            writeTaggedMd(dir.path(), QStringLiteral("interview.wav"),
                          QStringLiteral("[0] [Speaker 1] hi\n[3.5] [Speaker 2] hello\n"));
        const QString audioPath = dir.path() + QStringLiteral("/interview.wav");

        RenameSpeakerTask task(QUrl::fromLocalFile(audioPath), QStringLiteral("Speaker 1"), QStringLiteral("Alice"));
        QCOMPARE(task.kind(), QStringLiteral("rename"));
        QVERIFY(task.isPersistable());
        // Instant lane: a single .md rewrite must not queue behind a
        // transcription (the scheduler still defers it if a transcription of
        // THIS file is active). See Task::Lane.
        QCOMPARE(task.lane(), Task::Lane::Instant);

        QSignalSpy renamedSpy(&task, &RenameSpeakerTask::speakerRenamed);
        NullContextFixture fixture;
        task.run(fixture.ctx); // doesn't throw -> success

        QCOMPARE(renamedSpy.count(), 1);
        const QVariantList args = renamedSpy.constFirst();
        QCOMPARE(args.at(0).toString(), audioPath);
        QCOMPARE(args.at(1).toString(), QStringLiteral("Speaker 1"));
        QVERIFY(args.at(2).toBool());

        QFile md(mdPath);
        QVERIFY(md.open(QIODevice::ReadOnly | QIODevice::Text));
        const QString contents = QString::fromUtf8(md.readAll());
        QVERIFY(contents.contains(QStringLiteral("[Alice] hi")));
        QVERIFY(!contents.contains(QStringLiteral("[Speaker 1]")));
        QVERIFY(contents.contains(QStringLiteral("speakers: Alice, Speaker 2")));
    }

    void renameSpeakerTask_collidingName_throwsTaskError_leavesFileUnchanged()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString mdPath =
            writeTaggedMd(dir.path(), QStringLiteral("interview.wav"),
                          QStringLiteral("[0] [Speaker 1] hi\n[3.5] [Speaker 2] hello\n"));
        const QString audioPath = dir.path() + QStringLiteral("/interview.wav");

        // Renaming onto an ALREADY-existing name is rejected (typo
        // protection) — see renameSpeakerInFile()'s doc comment.
        RenameSpeakerTask task(QUrl::fromLocalFile(audioPath), QStringLiteral("Speaker 1"), QStringLiteral("Speaker 2"));
        NullContextFixture fixture;
        bool threw = false;
        try {
            task.run(fixture.ctx);
        } catch (const TaskError &e) {
            threw = true;
            QVERIFY(e.message().contains(QStringLiteral("already exists")));
        }
        QVERIFY(threw);

        QFile md(mdPath);
        QVERIFY(md.open(QIODevice::ReadOnly | QIODevice::Text));
        QVERIFY(QString::fromUtf8(md.readAll()).contains(QStringLiteral("[Speaker 1] hi")));
    }

    void mergeSpeakersTask_foldsSourceIntoTarget_andDropsFromFrontmatter()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString mdPath = writeTaggedMd(
            dir.path(), QStringLiteral("interview.wav"),
            QStringLiteral("[0] [Speaker 1] hi\n[3.5] [Speaker 2] hello\n[7] [Speaker 1] bye\n"));
        const QString audioPath = dir.path() + QStringLiteral("/interview.wav");

        MergeSpeakersTask task(QUrl::fromLocalFile(audioPath), QStringLiteral("Speaker 1"), QStringLiteral("Speaker 2"));
        QCOMPARE(task.kind(), QStringLiteral("merge"));
        QCOMPARE(task.lane(), Task::Lane::Instant);

        QSignalSpy mergedSpy(&task, &MergeSpeakersTask::speakersMerged);
        NullContextFixture fixture;
        task.run(fixture.ctx);

        QCOMPARE(mergedSpy.count(), 1);
        QVERIFY(mergedSpy.constFirst().at(3).toBool());

        QFile md(mdPath);
        QVERIFY(md.open(QIODevice::ReadOnly | QIODevice::Text));
        const QString contents = QString::fromUtf8(md.readAll());
        QCOMPARE(contents.count(QStringLiteral("[Speaker 2]")), 3); // both original + 2 folded
        QVERIFY(!contents.contains(QStringLiteral("[Speaker 1]")));
        QCOMPARE(contents.count(QStringLiteral("speakers: Speaker 2")), 1);
    }

    void mergeSpeakersTask_unknownSource_throwsTaskError()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        writeTaggedMd(dir.path(), QStringLiteral("interview.wav"), QStringLiteral("[0] [Speaker 1] hi\n"));
        const QString audioPath = dir.path() + QStringLiteral("/interview.wav");

        MergeSpeakersTask task(QUrl::fromLocalFile(audioPath), QStringLiteral("Nobody"), QStringLiteral("Speaker 1"));
        NullContextFixture fixture;
        QVERIFY_EXCEPTION_THROWN(task.run(fixture.ctx), TaskError);
    }
};

QTEST_GUILESS_MAIN(TestTaskSpeakerEdits)
#include "tst_task_speaker_edits.moc"
