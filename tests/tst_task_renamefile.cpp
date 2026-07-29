// RenameFileTask (file-level rename) — the instant-lane task that renames a
// recording (audio + sibling .md) or an orphaned .md. Pure filesystem, so this
// exercises run() for real via the shared renameRecordingFile() logic, and
// asserts the fileRenamed(oldUrl, newUrl, ok, error) contract QML repoints on.

#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QUrl>
#include <QtTest>

#include "FileRename.h"
#include "tasks/EngineCache.h"
#include "tasks/RenameFileTask.h"
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

class TestTaskRenameFile : public QObject {
    Q_OBJECT
private slots:
    void renameFileTask_reportsKindAndLane()
    {
        RenameFileTask task(QUrl::fromLocalFile("/vault/a.wav"), QStringLiteral("b"));
        QCOMPARE(task.kind(), QStringLiteral("renameFile"));
        QCOMPARE(task.lane(), Task::Lane::Instant);
        QVERIFY(!task.isPersistable());
    }

    void renameFileTask_renamesAudioAndSiblingMd_emitsFileRenamedWithNewUrl()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString audio = writeFile(dir.path() + "/interview.wav", "audio");
        writeFile(dir.path() + "/interview.wav.md", "---\n---\n\n[0] hi\n"); // sibling sidecar

        RenameFileTask task(QUrl::fromLocalFile(audio), QStringLiteral("meeting"));
        QSignalSpy renamedSpy(&task, &RenameFileTask::fileRenamed);
        NullContextFixture fixture;
        task.run(fixture.ctx); // must not throw

        QCOMPARE(renamedSpy.count(), 1);
        const QVariantList args = renamedSpy.constFirst();
        QCOMPARE(args.at(0).toUrl(), QUrl::fromLocalFile(audio));               // oldUrl
        QCOMPARE(args.at(1).toUrl(), QUrl::fromLocalFile(dir.path() + "/meeting.wav")); // newUrl
        QVERIFY(args.at(2).toBool());                                            // ok

        // Both the audio AND the sibling .md moved (sidecar tracks the full new
        // audio filename — see renameRecordingFile()).
        QVERIFY(QFileInfo::exists(dir.path() + "/meeting.wav"));
        QVERIFY(QFileInfo::exists(dir.path() + "/meeting.wav.md"));
        QVERIFY(!QFileInfo::exists(audio));
        QVERIFY(!QFileInfo::exists(dir.path() + "/interview.wav.md"));
    }

    void renameFileTask_collision_throwsTaskError_andReportsFailure()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString audio = writeFile(dir.path() + "/a.wav", "x");
        writeFile(dir.path() + "/taken.wav", "y"); // target name already exists

        RenameFileTask task(QUrl::fromLocalFile(audio), QStringLiteral("taken"));
        QSignalSpy renamedSpy(&task, &RenameFileTask::fileRenamed);
        NullContextFixture fixture;
        bool threw = false;
        try {
            task.run(fixture.ctx);
        } catch (const TaskError &) {
            threw = true;
        }
        QVERIFY(threw);
        QCOMPARE(renamedSpy.count(), 1);
        QVERIFY(!renamedSpy.constFirst().at(2).toBool()); // ok == false
        // Original untouched.
        QVERIFY(QFileInfo::exists(audio));
    }

    // A transcribe task carries the AUDIO url while a rename of that recording
    // carries the sibling .md url, so plain url equality misses the pairing and
    // the scheduler wouldn't defer the rename behind a running transcription.
    // isSameRecording() pairs an audio file with its <stem>.md so
    // serialActiveConflictsWith catches it.
    void isSameRecording_pairsAudioAndSiblingMd_notUnrelatedFiles()
    {
        const QUrl wav = QUrl::fromLocalFile(QStringLiteral("/v/dir/foo.wav"));
        const QUrl md  = QUrl::fromLocalFile(QStringLiteral("/v/dir/foo.md"));
        QVERIFY(isSameRecording(wav, md));        // audio <-> its transcript
        QVERIFY(isSameRecording(wav, wav));       // identity
        QVERIFY(!isSameRecording(wav,
            QUrl::fromLocalFile(QStringLiteral("/v/dir/bar.md"))));   // different stem
        QVERIFY(!isSameRecording(wav,
            QUrl::fromLocalFile(QStringLiteral("/v/other/foo.md")))); // different dir
        QVERIFY(!isSameRecording(wav, QUrl()));    // empty
    }
};

QTEST_GUILESS_MAIN(TestTaskRenameFile)
#include "tst_task_renamefile.moc"
