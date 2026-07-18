// TranscribeTask, ported to the task-system refactor (.task/plan.md). Only
// the structural interface and the offline "unknown engine id" early-exit
// path (resolveEngine() just scans local vault dirs, no network) are
// exercised for real — an actual decode+transcribe run needs a real
// downloaded whisper/parakeet model, same "don't exercise the heavy/network
// path in a unit test" precedent as the other engine-backed kinds.

#include <QTemporaryDir>
#include <QUrl>
#include <QtTest>

#include "tasks/EngineCache.h"
#include "tasks/TaskContext.h"
#include "tasks/TranscribeTask.h"

namespace {

struct NullContextFixture {
    EngineCache engines;
    TaskControl control;
    TaskContext ctx{engines, control, [](bool) {}};
};

} // namespace

class TestTaskTranscribe : public QObject {
    Q_OBJECT
private slots:
    void transcribeTask_reportsKindAndCapabilities()
    {
        TranscribeTask task(QUrl::fromLocalFile("/vault/a.wav"), QUrl::fromLocalFile("/vault"), QStringLiteral("small.en"));
        QCOMPARE(task.kind(), QStringLiteral("transcribe"));
        QVERIFY(task.isPersistable());
        QVERIFY(task.capabilities().pausable);
        QVERIFY(task.capabilities().cancellableMidRun);
    }

    void transcribeTask_toVariantMap_carriesModelId()
    {
        TranscribeTask task(QUrl::fromLocalFile("/vault/a.wav"), QUrl::fromLocalFile("/vault"), QStringLiteral("small.en"));
        QCOMPARE(task.toVariantMap().value(QStringLiteral("modelId")).toString(), QStringLiteral("small.en"));
    }

    void transcribeTask_unknownEngineId_throwsTaskError_noNetwork()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        // "engine:" prefix routes to an out-of-process engine; resolveEngine()
        // just scans local vault dirs for a manifest with this id — an
        // unrecognized id fails fast, no network/model load ever attempted.
        TranscribeTask task(QUrl::fromLocalFile(dir.path() + "/a.wav"), QUrl::fromLocalFile(dir.path()), QStringLiteral("engine:nonexistent-engine-id"));
        NullContextFixture fixture;
        bool threw = false;
        try {
            task.run(fixture.ctx);
        } catch (const TaskError &e) {
            threw = true;
            QVERIFY(e.message().contains(QStringLiteral("not found")));
        }
        QVERIFY(threw);
    }
};

QTEST_GUILESS_MAIN(TestTaskTranscribe)
#include "tst_task_transcribe.moc"
