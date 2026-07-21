// Third batch of kinds ported to the task-system refactor (.task/plan.md):
// CombineTask, ImportTask (both get real run() execution — pure file I/O /
// a real ffmpeg subprocess, no network/GPU, same precedent as
// tst_filecombine.cpp's ffmpegOnPath()-guarded end-to-end tests) and
// UsbGrabTask (structural interface only — a real grab needs a simulated
// mounted volume, out of scope for this port; mirrors tst_task_download.cpp's
// "don't execute the network-shaped kind" precedent).

#include <QDateTime>
#include <QFile>
#include <QProcess>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QUrl>
#include <QtTest>

#include "tasks/CombineTask.h"
#include "tasks/EngineCache.h"
#include "tasks/ImportTask.h"
#include "tasks/TaskContext.h"
#include "tasks/UncombineTask.h"
#include "tasks/UsbGrabTask.h"

namespace {

struct NullContextFixture {
    EngineCache engines;
    TaskControl control;
    TaskContext ctx{engines, control, [](bool) {}};
};

bool ffmpegOnPath()
{
    return !QStandardPaths::findExecutable(QStringLiteral("ffmpeg")).isEmpty();
}

// Mirrors tst_filecombine.cpp's makeWav() — a real (tiny) wav ffmpeg's
// concat demuxer will actually accept.
QString makeWav(const QString &dir, const QString &name)
{
    const QString path = dir + "/" + name;
    QProcess gen;
    gen.start("ffmpeg", {"-y", "-loglevel", "error", "-f", "lavfi", "-i",
                          "sine=frequency=440:sample_rate=16000:duration=1", path});
    if (!gen.waitForFinished(15000) || gen.exitCode() != 0)
        return {};
    return path;
}

} // namespace

class TestTaskFiles : public QObject {
    Q_OBJECT
private slots:
    void combineTask_mergesRealFiles_andMovesSourcesIntoCombinedDir()
    {
        if (!ffmpegOnPath())
            QSKIP("ffmpeg not on PATH");
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString a = makeWav(dir.path(), "part1.wav");
        const QString b = makeWav(dir.path(), "part2.wav");
        QVERIFY(!a.isEmpty() && !b.isEmpty());
        const QString outputPath = dir.path() + "/merged.wav";

        CombineTask task({a, b}, QUrl::fromLocalFile(outputPath), /*deleteOriginals=*/false);
        QCOMPARE(task.kind(), QStringLiteral("combine"));
        QVERIFY(task.isPersistable());
        QVERIFY(task.capabilities().pausable);

        QSignalSpy progressSpy(&task, &Task::progress);
        NullContextFixture fixture;
        task.run(fixture.ctx); // doesn't throw -> success

        QVERIFY(QFileInfo::exists(outputPath));
        QVERIFY(progressSpy.count() >= 1);
        // Sources moved into a sibling .combined/ dir, not deleted.
        QVERIFY(!QFileInfo::exists(a));
        QVERIFY(!QFileInfo::exists(b));
        QVERIFY(QFileInfo::exists(dir.path() + "/.combined/part1.wav"));
        QVERIFY(QFileInfo::exists(dir.path() + "/.combined/part2.wav"));
    }

    void combineTask_missingSource_throwsTaskError()
    {
        if (!ffmpegOnPath())
            QSKIP("ffmpeg not on PATH");
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        CombineTask task({dir.path() + "/nonexistent.wav"}, QUrl::fromLocalFile(dir.path() + "/out.wav"), false);
        NullContextFixture fixture;
        QVERIFY_EXCEPTION_THROWN(task.run(fixture.ctx), TaskError);
    }

    // A default (move-to-.combined/) combine is undoable: the inverse restores
    // the parts and removes the merged file.
    void combineTask_undoRestoresPartsAndRemovesMerge()
    {
        if (!ffmpegOnPath())
            QSKIP("ffmpeg not on PATH");
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString a = makeWav(dir.path(), "part1.wav");
        const QString b = makeWav(dir.path(), "part2.wav");
        QVERIFY(!a.isEmpty() && !b.isEmpty());
        const QString outputPath = dir.path() + "/merged.wav";

        CombineTask task({a, b}, QUrl::fromLocalFile(outputPath), /*deleteOriginals=*/false);
        NullContextFixture fixture;
        task.run(fixture.ctx);

        QVERIFY(QFileInfo::exists(outputPath));
        QVERIFY(task.isUndoable());
        QVERIFY(task.undoStillApplicable());
        QCOMPARE(task.undoLabel(), QStringLiteral("Combined 2 files"));

        auto undo = task.makeUndoTask();
        QVERIFY(undo != nullptr);
        undo->run(fixture.ctx); // no throw -> success

        QVERIFY(QFileInfo::exists(a));
        QVERIFY(QFileInfo::exists(b));
        QVERIFY(!QFileInfo::exists(outputPath));
        QVERIFY(!QFileInfo::exists(dir.path() + "/.combined/part1.wav"));
        QVERIFY(!QFileInfo::exists(dir.path() + "/.combined/part2.wav"));
    }

    // deleteOriginals leaves no parts to restore, so no inverse exists.
    void combineTask_deleteOriginals_isNotUndoable()
    {
        if (!ffmpegOnPath())
            QSKIP("ffmpeg not on PATH");
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString a = makeWav(dir.path(), "part1.wav");
        const QString b = makeWav(dir.path(), "part2.wav");
        QVERIFY(!a.isEmpty() && !b.isEmpty());

        CombineTask task({a, b}, QUrl::fromLocalFile(dir.path() + "/merged.wav"), /*deleteOriginals=*/true);
        NullContextFixture fixture;
        task.run(fixture.ctx);

        QVERIFY(task.undoLabel().isEmpty());
        QVERIFY(task.makeUndoTask() == nullptr);
        QVERIFY(!task.isUndoable());
    }

    // The merge became real work once transcribed: undo goes stale, and running
    // it anyway refuses (throws) and leaves the merged file on disk.
    void combineTask_undoGoesStaleWhenMergeTranscribed()
    {
        if (!ffmpegOnPath())
            QSKIP("ffmpeg not on PATH");
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString a = makeWav(dir.path(), "part1.wav");
        const QString b = makeWav(dir.path(), "part2.wav");
        QVERIFY(!a.isEmpty() && !b.isEmpty());
        const QString outputPath = dir.path() + "/merged.wav";

        CombineTask task({a, b}, QUrl::fromLocalFile(outputPath), false);
        NullContextFixture fixture;
        task.run(fixture.ctx);

        // Sidecar is the FULL merged filename + ".md" (not completeBaseName).
        QFile md(outputPath + ".md");
        QVERIFY(md.open(QIODevice::WriteOnly));
        md.write("---\nmodel: whisper\n---\n[0] hi\n");
        md.close();

        QVERIFY(!task.undoStillApplicable());

        auto undo = task.makeUndoTask();
        QVERIFY(undo != nullptr);
        QVERIFY_EXCEPTION_THROWN(undo->run(fixture.ctx), TaskError);
        QVERIFY(QFileInfo::exists(outputPath)); // refused, not destroyed
    }

    // If an archived part is removed out from under the undo, it goes stale.
    void combineTask_undoStaleWhenArchivedPartRemoved()
    {
        if (!ffmpegOnPath())
            QSKIP("ffmpeg not on PATH");
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString a = makeWav(dir.path(), "part1.wav");
        const QString b = makeWav(dir.path(), "part2.wav");
        QVERIFY(!a.isEmpty() && !b.isEmpty());

        CombineTask task({a, b}, QUrl::fromLocalFile(dir.path() + "/merged.wav"), false);
        NullContextFixture fixture;
        task.run(fixture.ctx);

        QVERIFY(task.undoStillApplicable());
        QVERIFY(QFile::remove(dir.path() + "/.combined/part1.wav"));
        QVERIFY(!task.undoStillApplicable());
    }

    void importTask_copiesFileIntoVault_reportsProgress()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString srcPath = dir.path() + "/source.bin";
        QFile src(srcPath);
        QVERIFY(src.open(QIODevice::WriteOnly));
        src.write(QByteArray(1 << 21, 'x')); // 2 MiB, spans multiple 1 MiB chunks
        src.close();
        const QString destPath = dir.path() + "/vault/imported.bin";
        QDir().mkpath(dir.path() + "/vault");

        ImportTask task(srcPath, QUrl::fromLocalFile(destPath));
        QCOMPARE(task.kind(), QStringLiteral("import"));
        QVERIFY(!task.isPersistable());
        // An import must never claim the main-panel overlay — it would block
        // the panel for the whole copy and never lift (no live segments).
        QCOMPARE(task.presentation(), Task::Presentation::Background);

        QSignalSpy progressSpy(&task, &Task::progress);
        NullContextFixture fixture;
        task.run(fixture.ctx);

        QVERIFY(QFileInfo::exists(destPath));
        QCOMPARE(QFileInfo(destPath).size(), qint64(1 << 21));
        QVERIFY(progressSpy.count() >= 1);
    }

    void importTask_destinationAlreadyExists_throwsTaskError_neverOverwrites()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString srcPath = dir.path() + "/source.bin";
        QFile src(srcPath);
        QVERIFY(src.open(QIODevice::WriteOnly));
        src.write("new content");
        src.close();
        const QString destPath = dir.path() + "/dest.bin";
        QFile dst(destPath);
        QVERIFY(dst.open(QIODevice::WriteOnly));
        dst.write("original content");
        dst.close();

        ImportTask task(srcPath, QUrl::fromLocalFile(destPath));
        NullContextFixture fixture;
        QVERIFY_EXCEPTION_THROWN(task.run(fixture.ctx), TaskError);

        QFile check(destPath);
        QVERIFY(check.open(QIODevice::ReadOnly));
        QCOMPARE(check.readAll(), QByteArray("original content"));
    }

    // Item 078: opting into overwrite (the drop handler's Overwrite snackbar
    // action) lifts the collision guard above and replaces the destination's
    // bytes — mirrors the no-overwrite test but with overwrite=true, asserting
    // it does NOT throw and DOES replace the audio bytes with the source's.
    void importTask_overwrite_replacesExistingFile()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString srcPath = dir.path() + "/source.bin";
        QFile src(srcPath);
        QVERIFY(src.open(QIODevice::WriteOnly));
        src.write("new content");
        src.close();
        const QString destPath = dir.path() + "/dest.bin";
        QFile dst(destPath);
        QVERIFY(dst.open(QIODevice::WriteOnly));
        dst.write("original content");
        dst.close();

        ImportTask task(srcPath, QUrl::fromLocalFile(destPath), /*overwrite=*/true);
        NullContextFixture fixture;
        task.run(fixture.ctx); // must not throw

        QFile check(destPath);
        QVERIFY(check.open(QIODevice::ReadOnly));
        QCOMPARE(check.readAll(), QByteArray("new content"));
    }

    // Queue item 051: the import copy must carry the SOURCE's mtime, not "now"
    // — the file-tree "Date Modified" sort keys off the audio file's mtime as
    // recording time, so an old recording imported today must not jump to the
    // top of the newest-first list (matches UsbGrabJob/CombineTask's restamp).
    void importTask_preservesSourceMtime()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString srcPath = dir.path() + "/source.bin";
        {
            QFile src(srcPath);
            QVERIFY(src.open(QIODevice::WriteOnly));
            src.write("some audio bytes");
            src.close();
        }
        // Restamp on a CLOSED file, then reopen: setting the time before
        // close() would be undone by close() flushing the buffered write and
        // bumping mtime back to now.
        const QDateTime oldTime = QDateTime::currentDateTime().addDays(-100);
        {
            QFile src(srcPath);
            QVERIFY(src.open(QIODevice::ReadWrite));
            QVERIFY(src.setFileTime(oldTime, QFileDevice::FileModificationTime));
            src.close();
        }
        QCOMPARE(QFileInfo(srcPath).lastModified().toSecsSinceEpoch(), oldTime.toSecsSinceEpoch());

        const QString destPath = dir.path() + "/vault/imported.bin";
        QDir().mkpath(dir.path() + "/vault");

        ImportTask task(srcPath, QUrl::fromLocalFile(destPath));
        NullContextFixture fixture;
        task.run(fixture.ctx);

        QVERIFY(QFileInfo::exists(destPath));
        // Second granularity — setFileTime truncates sub-second on some FSes.
        QCOMPARE(QFileInfo(destPath).lastModified().toSecsSinceEpoch(),
                 oldTime.toSecsSinceEpoch());
    }

    // --- UsbGrabTask: structural interface only, no real mounted volume. ---

    void usbGrabTask_reportsKindAndCapabilitiesAndPersistence()
    {
        UsbGrabRule rule;
        rule.deviceLabel = QStringLiteral("Test Recorder");
        UsbGrabTask task(rule, QStringLiteral("/mnt/fake"), QUrl::fromLocalFile(QStringLiteral("/vault/dest")),
                         /*autoTriggered=*/true, /*combineRules=*/{});
        QCOMPARE(task.kind(), QStringLiteral("usbGrab"));
        QVERIFY(!task.isPersistable()); // removable-media source, never persisted
        QVERIFY(task.capabilities().pausable);
        QVERIFY(task.autoTriggered());
        QVERIFY(!task.displayName().isEmpty());
        QCOMPARE(task.toVariantMap().value(QStringLiteral("ruleName")).toString(), QStringLiteral("Test Recorder"));
        // fileCount is -1 until a run's pre-count pass reports it (BottomBar
        // shows the drive alone until then).
        QCOMPARE(task.toVariantMap().value(QStringLiteral("fileCount")).toInt(), -1);
    }
};

QTEST_GUILESS_MAIN(TestTaskFiles)
#include "tst_task_files.moc"
