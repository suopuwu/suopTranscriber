// DetectSilenceTask, following tst_task_diarize.cpp's shape: the structural
// interface + the not-yet-transcribed early-exit are exercised directly via
// NullContextFixture (no real decode needed for either). A real end-to-end
// run through the QUEUE (TranscriptionController::detectSilences()) is ALSO
// exercised below — unlike DiarizeTask/TranscribeTask's heavy-model-gated
// real paths, DetectSilenceTask needs only ffmpeg (already required to build
// at all) plus whisper.cpp's own vendored for-tests Silero VAD model
// (third_party/whisper.cpp/models/for-tests-silero-v6.2.0-ggml.bin, no
// network download needed — same plant-don't-download precedent
// tst_transcript_format.cpp's writtenFileFormat_endToEnd() uses), so this one
// runs for real rather than QSKIPping.

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QUrl>
#include <QtTest>

#include "MdFrontmatter.h"
#include "ModelManager.h"
#include "TranscriptionController.h"
#include "tasks/DetectSilenceTask.h"
#include "tasks/EngineCache.h"
#include "tasks/TaskContext.h"

namespace {

struct NullContextFixture {
    EngineCache engines;
    TaskControl control;
    TaskContext ctx{engines, control, [](bool) {}};
};

QString writeFixture(const QString &dirPath, const QString &mdName, const QString &content)
{
    const QString path = dirPath + "/" + mdName;
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return {};
    f.write(content.toUtf8());
    f.close();
    return path;
}

QString readAllBytes(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return {};
    return QString::fromUtf8(f.readAll());
}

} // namespace

class TestTaskSilence : public QObject {
    Q_OBJECT
private slots:
    // TranscriptionController persists rolling-average phase timing via a
    // plain QSettings() (see RollingAverage.h) — CLAUDE.md "Never let
    // testing touch the user's real vault or app config" applies here just
    // as much as to lastVaultPath: setTestModeEnabled() redirects every
    // QSettings/QStandardPaths lookup for the rest of this process into an
    // isolated "qttest" sandbox instead of the real ~/.config/suopTranscriber
    // — needed because (unlike the other tst_task_*.cpp files, which only
    // ever build a bare TaskContext) the real-run test below constructs a
    // full TranscriptionController to drain via its finished() signal.
    void initTestCase() { QStandardPaths::setTestModeEnabled(true); }

    void detectSilenceTask_reportsKindAndCapabilities()
    {
        DetectSilenceTask task(QUrl::fromLocalFile("/vault/interview.wav"), QUrl::fromLocalFile("/vault"));
        QCOMPARE(task.kind(), QStringLiteral("detectSilence"));
        QCOMPARE(task.kindLabel(), QStringLiteral("Detect silences"));
        QVERIFY(task.isPersistable());
        QVERIFY(!task.isStructuralFileOp());
        QVERIFY(task.capabilities().pausable);
        QVERIFY(!task.capabilities().cancellableMidRun); // no mid-pass abort hook, same as DiarizeTask
    }

    void detectSilenceTask_declaresVadModelPrerequisite()
    {
        DetectSilenceTask task(QUrl::fromLocalFile("/vault/a.wav"), QUrl::fromLocalFile("/vault"));
        const ModelPrerequisites prereqs = task.prerequisites();
        QCOMPARE(prereqs.size(), 1);
        QVERIFY(prereqs.first().target == ModelDownloadTarget::VadModel);
    }

    void detectSilenceTask_notYetTranscribed_throwsTaskError()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString audioPath = dir.path() + "/never-transcribed.wav";

        DetectSilenceTask task(QUrl::fromLocalFile(audioPath), QUrl::fromLocalFile(dir.path()));
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

    // Real end-to-end run through the queue: on a fixture .md that already
    // carries segments/speakers/chapters, DetectSilenceTask must add
    // "silences:" WITHOUT disturbing any of that — draining via finished()
    // before asserting (instant-lane-race precedent, see
    // mergeSpeakers_failsWhenEitherNameMissing() in tst_transcript_format.cpp
    // — DetectSilenceTask is Serial-lane, not Instant, but the same "don't
    // read state before the queue has actually reaped the task" rule
    // applies: onTaskFinished() is what flips status to Done/Failed, and
    // finished() only fires from there).
    void detectSilenceTask_realRun_addsSilencesWithoutDisturbingRestOfFile()
    {
        if (QStandardPaths::findExecutable(QStringLiteral("ffmpeg")).isEmpty())
            QSKIP("ffmpeg not on PATH — needed to decode the sample (DESIGN.md: ffmpeg decodes, Qt only plays)");
        const QString vadModelSrc =
            QStringLiteral(SUOP_REPO_DIR "/third_party/whisper.cpp/models/for-tests-silero-v6.2.0-ggml.bin");
        if (!QFileInfo::exists(vadModelSrc))
            QSKIP("vendored for-tests Silero VAD model missing from the third_party checkout");

        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        const QString modelsDir = vault.path() + "/.transcriber/models";
        QVERIFY(QDir().mkpath(modelsDir));
        qputenv("SUOP_MODELS_DIR", modelsDir.toLocal8Bit()); // app-global store -> this test's dir
        QVERIFY(QFile::copy(vadModelSrc, modelsDir + "/" + ModelManager::kVadModelFileName));

        const QString samplePath = vault.path() + "/jfk.wav";
        QVERIFY(QFile::copy(QStringLiteral(SUOP_SAMPLES_DIR "/jfk.wav"), samplePath));

        // A fixture .md as if jfk.wav had already been transcribed (no VAD,
        // hence no "silences:" key yet) WITH speakers/chapters already
        // present — the "without disturbing" half of this test's contract.
        const QString original = QStringLiteral("---\n"
                                                  "model: ggml-base.en.bin\n"
                                                  "speakerDetection: true\n"
                                                  "speakers: Speaker 1\n"
                                                  "label: JFK speech\n"
                                                  "transcribedAt: 2026-07-06T10:12:13\n"
                                                  "---\n\n"
                                                  "## [0] Opening\n"
                                                  "[0] [Speaker 1] We choose to go to the moon.\n");
        const QString mdPath = writeFixture(vault.path(), "jfk.wav.md", original);
        QVERIFY(!mdPath.isEmpty());

        TranscriptionController controller;
        QSignalSpy finishedSpy(&controller, &TranscriptionController::finished);
        const QUrl audioUrl = QUrl::fromLocalFile(samplePath);
        controller.detectSilences(audioUrl, QUrl::fromLocalFile(vault.path()));

        QVERIFY(finishedSpy.wait(30000));
        QVERIFY2(finishedSpy.at(0).at(1).toBool(), qPrintable(finishedSpy.at(0).at(2).toString()));

        const QString rewritten = readAllBytes(mdPath);
        QVERIFY2(rewritten.contains(QStringLiteral("silences:")), qPrintable(rewritten));
        // Everything from the original fixture is still present verbatim —
        // only the new key was added.
        for (const QString &originalLine : original.split(QLatin1Char('\n'))) {
            if (originalLine.isEmpty())
                continue;
            QVERIFY2(rewritten.contains(originalLine), qPrintable(originalLine));
        }

        QStringList lines;
        QString body;
        QString error;
        QVERIFY(MdFrontmatter::read(mdPath, &lines, &body, &error));
        QVERIFY2(body.contains(QStringLiteral("## [0] Opening")), "chapter marker must survive untouched");
        QVERIFY2(body.contains(QStringLiteral("[Speaker 1] We choose to go to the moon.")),
                 "speaker tag + segment text must survive untouched");
    }
};

QTEST_GUILESS_MAIN(TestTaskSilence)
#include "tst_task_silence.moc"
