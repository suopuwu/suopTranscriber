// TrashTask / DeleteTask / RestoreTask — the instant-lane tasks behind the vault
// trash/permanent-delete/undo flow. Exercises run() for real via the shared
// VaultFileOps functions (trashVaultEntries/deleteVaultEntriesPermanently/
// restoreVaultEntries), and asserts the undo wiring: a TrashTask's makeUndoTask()
// is a RestoreTask that puts everything back.
//
// Trash tests move files to the REAL system trash, so every trash here is paired
// with a restore that brings it back — nothing is left in the trash.

#include <QDir>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QUrl>
#include <QtTest>

#include "tasks/DeleteTask.h"
#include "tasks/EngineCache.h"
#include "tasks/RestoreTask.h"
#include "tasks/TaskContext.h"
#include "tasks/TrashTask.h"

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

class TestTaskTrash : public QObject {
    Q_OBJECT
private slots:
    void trashTask_reportsKindAndLane()
    {
        TrashTask task(VaultFileScope::Files, {QUrl::fromLocalFile("/vault/a.wav")},
                       QUrl::fromLocalFile("/vault"));
        QCOMPARE(task.kind(), QStringLiteral("trash"));
        QCOMPARE(task.lane(), Task::Lane::Instant);
        QVERIFY(!task.isPersistable());
    }

    // The human category label lives on the Task primitive now (Task::kindLabel())
    // — each kind authors its own, replacing BottomBar's kindLabel(kind) switch.
    void kindLabel_isAuthoredPerKind()
    {
        const QUrl v = QUrl::fromLocalFile("/vault");
        const QList<QUrl> one{QUrl::fromLocalFile("/vault/a.wav")};
        QCOMPARE(TrashTask(VaultFileScope::Files, one, v).kindLabel(), QStringLiteral("Move to trash"));
        QCOMPARE(DeleteTask(VaultFileScope::Files, one, v).kindLabel(), QStringLiteral("Delete"));
        QCOMPARE(RestoreTask(QVariantList{}, v).kindLabel(), QStringLiteral("Restore"));
        // kindLabel() (human "Move to trash") is distinct from kind() (slug "trash").
        TrashTask t(VaultFileScope::Files, one, v);
        QVERIFY(t.kindLabel() != t.kind());
    }

    void trashTask_trashesFileAndSidecar_thenUndoRestores()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString audio = writeFile(dir.path() + "/interview.wav", "audio");
        writeFile(dir.path() + "/interview.wav.md", "---\n---\n\n[0] hi\n"); // sibling sidecar

        TrashTask task(VaultFileScope::Files, {QUrl::fromLocalFile(audio)}, QUrl::fromLocalFile(dir.path()));
        QSignalSpy doneSpy(&task, &TrashTask::trashDone);
        NullContextFixture fixture;
        task.run(fixture.ctx);

        QCOMPARE(doneSpy.count(), 1);
        // Both audio AND sidecar left the vault, and both are recorded as
        // restorable (the whole set, so undo brings it all back).
        QVERIFY(!QFileInfo::exists(audio));
        QVERIFY(!QFileInfo::exists(dir.path() + "/interview.wav.md"));
        QCOMPARE(task.result().trashed.size(), 1);          // one user-visible entry
        QVERIFY(task.result().restorable.size() >= 2);      // audio + sidecar
        QVERIFY(!task.undoLabel().isEmpty());               // undoable

        // The inverse RestoreTask brings everything back.
        std::unique_ptr<Task> undo = task.makeUndoTask();
        QVERIFY(undo != nullptr);
        QVERIFY(qobject_cast<RestoreTask *>(undo.get()) != nullptr);
        undo->run(fixture.ctx);
        QVERIFY(QFileInfo::exists(audio));
        QVERIFY(QFileInfo::exists(dir.path() + "/interview.wav.md"));
    }

    void trashTask_undoLabel_scopeAndCountAware()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString a = writeFile(dir.path() + "/a.wav", "x");
        const QString b = writeFile(dir.path() + "/b.wav", "y");

        TrashTask task(VaultFileScope::Files, {QUrl::fromLocalFile(a), QUrl::fromLocalFile(b)},
                       QUrl::fromLocalFile(dir.path()));
        NullContextFixture fixture;
        task.run(fixture.ctx);
        QCOMPARE(task.undoLabel(), QStringLiteral("Moved 2 items to trash"));

        // Clean up: restore both so nothing lingers in the trash.
        task.makeUndoTask()->run(fixture.ctx);
        QVERIFY(QFileInfo::exists(a));
        QVERIFY(QFileInfo::exists(b));
    }

    void deleteTask_permanentlyRemoves_notUndoable()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString audio = writeFile(dir.path() + "/gone.wav", "x");
        writeFile(dir.path() + "/gone.wav.md", "---\n---\n");

        DeleteTask task(VaultFileScope::Files, {QUrl::fromLocalFile(audio)}, QUrl::fromLocalFile(dir.path()));
        QSignalSpy doneSpy(&task, &DeleteTask::deleteDone);
        NullContextFixture fixture;
        task.run(fixture.ctx);

        QCOMPARE(doneSpy.count(), 1);
        QCOMPARE(task.kind(), QStringLiteral("delete"));
        QVERIFY(!QFileInfo::exists(audio));
        QVERIFY(!QFileInfo::exists(dir.path() + "/gone.wav.md"));
        // Permanent delete is not undoable.
        QVERIFY(task.undoLabel().isEmpty());
        QVERIFY(task.makeUndoTask() == nullptr);
        QCOMPARE(task.removed().size(), 1);
    }

    void trashTask_transcriptScope_keepsAudio()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString audio = writeFile(dir.path() + "/keep.wav", "audio");
        writeFile(dir.path() + "/keep.wav.md", "---\n---\n\n[0] hi\n");

        TrashTask task(VaultFileScope::Transcript, {QUrl::fromLocalFile(audio)}, QUrl::fromLocalFile(dir.path()));
        NullContextFixture fixture;
        task.run(fixture.ctx);

        // The transcript went; the audio stayed.
        QVERIFY(!QFileInfo::exists(dir.path() + "/keep.wav.md"));
        QVERIFY(QFileInfo::exists(audio));

        // Restore for cleanliness.
        if (auto undo = task.makeUndoTask())
            undo->run(fixture.ctx);
    }
};

QTEST_GUILESS_MAIN(TestTaskTrash)
#include "tst_task_trash.moc"
