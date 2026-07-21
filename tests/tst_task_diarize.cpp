// DiarizeTask, ported to the task-system refactor (.task/plan.md). Only the
// early-exit path is exercised for real (not-yet-transcribed) — the actual
// decode+diarization path needs a real downloaded sherpa-onnx model, same
// "don't exercise the heavy/network path in a unit test" precedent as the
// other engine-backed kinds.

#include <QUrl>
#include <QtTest>
#include <QTemporaryDir>

#include "ModelManager.h"
#include "tasks/DiarizeTask.h"
#include "tasks/EngineCache.h"
#include "tasks/TaskContext.h"

namespace {

struct NullContextFixture {
    EngineCache engines;
    TaskControl control;
    TaskContext ctx{engines, control, [](bool) {}};
};

} // namespace

class TestTaskDiarize : public QObject {
    Q_OBJECT
private slots:
    void diarizeTask_reportsKindAndCapabilities()
    {
        DiarizeTask task(QUrl::fromLocalFile("/vault/interview.wav"), QUrl::fromLocalFile("/vault"), QString(), QString(), 0.5);
        QCOMPARE(task.kind(), QStringLiteral("diarize"));
        QVERIFY(task.isPersistable());
        QVERIFY(task.capabilities().pausable);
        QVERIFY(!task.capabilities().cancellableMidRun); // no mid-pass abort hook
    }

    void diarizeTask_notYetTranscribed_throwsTaskError()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString audioPath = dir.path() + "/never-transcribed.wav";

        DiarizeTask task(QUrl::fromLocalFile(audioPath), QUrl::fromLocalFile(dir.path()), QString(), QString(), 0.5);
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

    void diarizeTask_toVariantMap_carriesTuning()
    {
        DiarizeTask task(QUrl::fromLocalFile("/vault/a.wav"), QUrl::fromLocalFile("/vault"), QStringLiteral("seg-x"),
                          QStringLiteral("embed-y"), 0.7);
        const QVariantMap map = task.toVariantMap();
        QCOMPARE(map.value(QStringLiteral("segmentationModelId")).toString(), QStringLiteral("seg-x"));
        QCOMPARE(map.value(QStringLiteral("embeddingModelId")).toString(), QStringLiteral("embed-y"));
        QCOMPARE(map.value(QStringLiteral("diarizationThreshold")).toDouble(), 0.7);
    }

    // When diarization finishes, DiarizeTask records which two models ran into
    // the .md frontmatter (LABELS, resolved from ids via ModelManager — matching
    // how `model:` stores a name, not an id) so the info line can name them.
    // This unit tests that wiring directly, without the heavy decode+diarize path.
    void applyDiarizationModelFrontmatter_recordsBothLabels()
    {
        QStringList lines{QStringLiteral("model: base"), QStringLiteral("speakerDetection: true")};
        applyDiarizationModelFrontmatter(&lines, QString::fromLatin1(ModelManager::kDefaultSegmentationModelId),
                                         QString::fromLatin1(ModelManager::kDefaultEmbeddingModelId));

        const QString expectedSeg =
            ModelManager::segmentationModelInfo(QString::fromLatin1(ModelManager::kDefaultSegmentationModelId)).label;
        const QString expectedEmb =
            ModelManager::embeddingModelInfo(QString::fromLatin1(ModelManager::kDefaultEmbeddingModelId)).label;
        QVERIFY(!expectedSeg.isEmpty());
        QVERIFY(!expectedEmb.isEmpty());
        QVERIFY2(lines.contains(QStringLiteral("segmentationModel: ") + expectedSeg),
                 "the segmentationModel label must be upserted into frontmatter");
        QVERIFY2(lines.contains(QStringLiteral("embeddingModel: ") + expectedEmb),
                 "the embeddingModel label must be upserted into frontmatter");
    }
};

QTEST_GUILESS_MAIN(TestTaskDiarize)
#include "tst_task_diarize.moc"
