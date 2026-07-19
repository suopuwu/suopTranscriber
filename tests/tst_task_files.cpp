// Third batch of kinds ported to the task-system refactor (.task/plan.md):
// CombineTask, ImportTask (both get real run() execution — pure file I/O /
// a real ffmpeg subprocess, no network/GPU, same precedent as
// tst_filecombine.cpp's ffmpegOnPath()-guarded end-to-end tests) and
// UsbGrabTask (structural interface only — a real grab needs a simulated
// mounted volume, out of scope for this port; mirrors tst_task_download.cpp's
// "don't execute the network-shaped kind" precedent).

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
    }
};

QTEST_GUILESS_MAIN(TestTaskFiles)
#include "tst_task_files.moc"
