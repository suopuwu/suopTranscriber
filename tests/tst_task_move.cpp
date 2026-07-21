// MoveTask (file-level move) — the instant-lane task that moves a recording
// (audio + sibling .md + .partial.jsonl) into another vault folder. Pure
// filesystem, so this exercises run() for real via the shared
// moveRecordingFile() logic, asserts the fileMoved(oldUrl, newUrl, ok, error)
// contract QML repoints on, and proves the undo (makeUndoTask == move-back) +
// the batch-undo label summary.

#include <QDir>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QUrl>
#include <QtTest>

#include "tasks/EngineCache.h"
#include "tasks/MoveTask.h"
#include "tasks/TaskContext.h"

namespace {

struct NullContextFixture {
    EngineCache engines;
    TaskControl control;
    TaskContext ctx{engines, control, [](bool) {}};
};

QString writeFile(const QString &path, const QByteArray &content)
{
    QFile f(path);
    f.open(QIODevice::WriteOnly);
    f.write(content);
    f.close();
    return path;
}

} // namespace

class TestTaskMove : public QObject {
    Q_OBJECT
private slots:
    void moveTask_reportsKindAndLane()
    {
        MoveTask task(QUrl::fromLocalFile("/vault/a.wav"), QUrl::fromLocalFile("/vault/sub"),
                      QUrl::fromLocalFile("/vault"));
        QCOMPARE(task.kind(), QStringLiteral("moveFile"));
        QCOMPARE(task.lane(), Task::Lane::Instant);
        QVERIFY(!task.isPersistable());
    }

    void moveTask_movesAudioAndSiblingMd_emitsFileMovedWithNewUrl()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QVERIFY(QDir(dir.path()).mkdir("sub"));
        const QString audio = writeFile(dir.path() + "/interview.wav", "audio");
        writeFile(dir.path() + "/interview.wav.md", "---\n---\n\n[0] hi\n"); // sibling sidecar

        MoveTask task(QUrl::fromLocalFile(audio), QUrl::fromLocalFile(dir.path() + "/sub"),
                      QUrl::fromLocalFile(dir.path()));
        QSignalSpy movedSpy(&task, &MoveTask::fileMoved);
        NullContextFixture fixture;
        task.run(fixture.ctx); // must not throw

        QCOMPARE(movedSpy.count(), 1);
        const QVariantList args = movedSpy.constFirst();
        QCOMPARE(args.at(0).toUrl(), QUrl::fromLocalFile(audio));                          // oldUrl
        QCOMPARE(args.at(1).toUrl(), QUrl::fromLocalFile(dir.path() + "/sub/interview.wav")); // newUrl
        QVERIFY(args.at(2).toBool());                                                       // ok

        // Audio AND sidecar moved together into the subfolder; originals gone.
        QVERIFY(QFileInfo::exists(dir.path() + "/sub/interview.wav"));
        QVERIFY(QFileInfo::exists(dir.path() + "/sub/interview.wav.md"));
        QVERIFY(!QFileInfo::exists(audio));
        QVERIFY(!QFileInfo::exists(dir.path() + "/interview.wav.md"));
    }

    void moveTask_makeUndoTask_movesItBack()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QVERIFY(QDir(dir.path()).mkdir("sub"));
        const QString audio = writeFile(dir.path() + "/clip.wav", "audio");

        MoveTask task(QUrl::fromLocalFile(audio), QUrl::fromLocalFile(dir.path() + "/sub"),
                      QUrl::fromLocalFile(dir.path()));
        NullContextFixture fixture;
        task.run(fixture.ctx);
        QVERIFY(QFileInfo::exists(dir.path() + "/sub/clip.wav"));

        // The inverse task moves it back to the original folder.
        std::unique_ptr<Task> undo = task.makeUndoTask();
        QVERIFY(undo != nullptr);
        undo->run(fixture.ctx);
        QVERIFY(QFileInfo::exists(audio));                       // back where it started
        QVERIFY(!QFileInfo::exists(dir.path() + "/sub/clip.wav"));
    }

    // The Tasks-list Undo button greys out once the undo can no longer run — the
    // moved file is gone from where the inverse expects it (moved/renamed again).
    void moveTask_undoStillApplicable_falseAfterResultMovedAway()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QVERIFY(QDir(dir.path()).mkdir("sub"));
        const QString audio = writeFile(dir.path() + "/clip.wav", "audio");

        MoveTask task(QUrl::fromLocalFile(audio), QUrl::fromLocalFile(dir.path() + "/sub"),
                      QUrl::fromLocalFile(dir.path()));
        // Un-run: m_resultUrl is empty, so there's nothing to undo yet.
        QVERIFY(!task.undoStillApplicable());

        NullContextFixture fixture;
        task.run(fixture.ctx);
        QVERIFY(QFileInfo::exists(dir.path() + "/sub/clip.wav"));
        // Result sits where the inverse expects it → undo still applicable.
        QVERIFY(task.undoStillApplicable());

        // The world moves on: the file leaves m_resultUrl (moved/renamed/deleted).
        QVERIFY(QFile::remove(dir.path() + "/sub/clip.wav"));
        QVERIFY(!task.undoStillApplicable());
    }

    void moveTask_batchUndoLabel_summarizesMultiFileMove()
    {
        MoveTask task(QUrl::fromLocalFile("/vault/a.wav"), QUrl::fromLocalFile("/vault/sub"),
                      QUrl::fromLocalFile("/vault"));
        // A batch of one keeps the specific single-file label; N>1 summarizes.
        QCOMPARE(task.batchUndoLabel(1), task.undoLabel());
        QCOMPARE(task.batchUndoLabel(3), QStringLiteral("Moved 3 files"));
    }

    void moveTask_collision_throwsTaskError_andReportsFailure()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QVERIFY(QDir(dir.path()).mkdir("sub"));
        const QString audio = writeFile(dir.path() + "/a.wav", "x");
        writeFile(dir.path() + "/sub/a.wav", "y"); // a same-named file already at the destination

        MoveTask task(QUrl::fromLocalFile(audio), QUrl::fromLocalFile(dir.path() + "/sub"),
                      QUrl::fromLocalFile(dir.path()));
        QSignalSpy movedSpy(&task, &MoveTask::fileMoved);
        NullContextFixture fixture;
        bool threw = false;
        try {
            task.run(fixture.ctx);
        } catch (const TaskError &) {
            threw = true;
        }
        QVERIFY(threw);
        QCOMPARE(movedSpy.count(), 1);
        QVERIFY(!movedSpy.constFirst().at(2).toBool()); // ok == false
        QVERIFY(QFileInfo::exists(audio));              // original untouched
    }
};

QTEST_GUILESS_MAIN(TestTaskMove)
#include "tst_task_move.moc"
