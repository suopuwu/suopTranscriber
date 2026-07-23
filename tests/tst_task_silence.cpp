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

    // Item 010 regression: an empty-but-error-free VAD result (chunkingError
    // empty, speechSpans empty) must NOT be trusted when the transcript has
    // real segments — trusting it would complement an empty list into a
    // single whole-file silence span, which is what the observed bug wrote
    // (0-3:17:14.04 on a 3469-segment file). This calls selectSilences()
    // directly, so it fails if the `!speechSpans.empty()` guard is reverted
    // (the un-guarded code would take the VAD branch and return {0,total}
    // instead of the leading-only gap asserted below).
    void selectSilences_emptySpeechSpansWithSegments_usesTranscriptGapFallback()
    {
        const QList<ParsedSegment> segments = {
            {423.0, QStringLiteral("first")},
            {500.0, QStringLiteral("second")},
            {11800.0, QStringLiteral("last")},
        };
        const auto silences = DetectSilenceTask::selectSilences(QString(), {}, segments, 12000.0);

        // Touching fallback spans leave only the leading gap before the
        // first segment — never the whole-file span the bug produced.
        QCOMPARE(silences.size(), std::size_t(1));
        QCOMPARE(silences.front().first, 0.0);
        QCOMPARE(silences.front().second, 423.0);
    }

    // Non-empty speechSpans is the ordinary, working case: the VAD result is
    // trusted and complemented directly, ignoring segments entirely.
    void selectSilences_nonEmptySpeechSpans_usesVadResult()
    {
        const std::vector<std::pair<double, double>> speechSpans = {{10.0, 20.0}};
        const QList<ParsedSegment> segments = {{10.0, QStringLiteral("only")}};
        const auto silences = DetectSilenceTask::selectSilences(QString(), speechSpans, segments, 30.0);

        QCOMPARE(silences.size(), std::size_t(2));
        QCOMPARE(silences[0].first, 0.0);
        QCOMPARE(silences[0].second, 10.0);
        QCOMPARE(silences[1].first, 20.0);
        QCOMPARE(silences[1].second, 30.0);
    }

    // Genuinely silent file: empty speechSpans AND no transcript segments —
    // the fallback's empty span list still complements to the correct
    // whole-file silence, so the item 010 guard doesn't regress this case.
    void selectSilences_emptySpeechSpansNoSegments_yieldsWholeFileSilence()
    {
        const auto silences = DetectSilenceTask::selectSilences(QString(), {}, {}, 42.0);
        QCOMPARE(silences.size(), std::size_t(1));
        QCOMPARE(silences.front().first, 0.0);
        QCOMPARE(silences.front().second, 42.0);
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
