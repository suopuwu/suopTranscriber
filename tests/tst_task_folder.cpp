// FolderRenameTask / FolderMoveTask — the instant-lane tasks behind folder
// rename/move. Pure filesystem run() via renameVaultFolder()/moveVaultFolder(),
// asserting the report signals and the undo (rename-back / move-back).

#include <QDir>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QUrl>
#include <QtTest>

#include "tasks/EngineCache.h"
#include "tasks/FolderMoveTask.h"
#include "tasks/FolderRenameTask.h"
#include "tasks/TaskContext.h"

namespace {

struct NullContextFixture {
    EngineCache engines;
    TaskControl control;
    TaskContext ctx{engines, control, [](bool) {}};
};

} // namespace

class TestTaskFolder : public QObject {
    Q_OBJECT
private slots:
    void folderTasks_reportKindAndLane()
    {
        FolderRenameTask rn(QUrl::fromLocalFile("/vault/sub"), QStringLiteral("sub2"),
                            QUrl::fromLocalFile("/vault"));
        QCOMPARE(rn.kind(), QStringLiteral("renameFolder"));
        QCOMPARE(rn.lane(), Task::Lane::Instant);
        FolderMoveTask mv(QUrl::fromLocalFile("/vault/sub"), QUrl::fromLocalFile("/vault/dest"),
                          QUrl::fromLocalFile("/vault"));
        QCOMPARE(mv.kind(), QStringLiteral("moveFolder"));
        QCOMPARE(mv.lane(), Task::Lane::Instant);
    }

    void folderRename_renamesInPlace_thenUndoRestores()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QVERIFY(QDir(dir.path()).mkdir("old"));

        FolderRenameTask task(QUrl::fromLocalFile(dir.path() + "/old"), QStringLiteral("new"),
                              QUrl::fromLocalFile(dir.path()));
        QSignalSpy spy(&task, &FolderRenameTask::folderRenamed);
        NullContextFixture fixture;
        task.run(fixture.ctx);

        QCOMPARE(spy.count(), 1);
        QVERIFY(spy.constFirst().at(2).toBool());
        QVERIFY(QDir(dir.path() + "/new").exists());
        QVERIFY(!QDir(dir.path() + "/old").exists());
        QVERIFY(!task.undoLabel().isEmpty());

        task.makeUndoTask()->run(fixture.ctx); // rename back
        QVERIFY(QDir(dir.path() + "/old").exists());
        QVERIFY(!QDir(dir.path() + "/new").exists());
    }

    void folderMove_movesIntoParent_thenUndoRestores()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QVERIFY(QDir(dir.path()).mkdir("sub"));
        QVERIFY(QDir(dir.path()).mkdir("dest"));
        // A file inside sub proves the whole tree moves intact.
        QFile f(dir.path() + "/sub/note.txt");
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.close();

        FolderMoveTask task(QUrl::fromLocalFile(dir.path() + "/sub"), QUrl::fromLocalFile(dir.path() + "/dest"),
                            QUrl::fromLocalFile(dir.path()));
        QSignalSpy spy(&task, &FolderMoveTask::folderMoved);
        NullContextFixture fixture;
        task.run(fixture.ctx);

        QCOMPARE(spy.count(), 1);
        QVERIFY(spy.constFirst().at(2).toBool());
        QVERIFY(QDir(dir.path() + "/dest/sub").exists());
        QVERIFY(QFileInfo::exists(dir.path() + "/dest/sub/note.txt"));
        QVERIFY(!QDir(dir.path() + "/sub").exists());

        task.makeUndoTask()->run(fixture.ctx); // move back
        QVERIFY(QDir(dir.path() + "/sub").exists());
        QVERIFY(!QDir(dir.path() + "/dest/sub").exists());
    }

    void folderRename_collision_throwsAndReportsFailure()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QVERIFY(QDir(dir.path()).mkdir("a"));
        QVERIFY(QDir(dir.path()).mkdir("taken"));

        FolderRenameTask task(QUrl::fromLocalFile(dir.path() + "/a"), QStringLiteral("taken"),
                              QUrl::fromLocalFile(dir.path()));
        NullContextFixture fixture;
        bool threw = false;
        try {
            task.run(fixture.ctx);
        } catch (const TaskError &) {
            threw = true;
        }
        QVERIFY(threw);
        QVERIFY(QDir(dir.path() + "/a").exists()); // untouched
    }
};

QTEST_GUILESS_MAIN(TestTaskFolder)
#include "tst_task_folder.moc"
