// BenchmarkTask/DiarizationSweepTask, ported to the task-system refactor
// (.task/plan.md). These fold the old side-door (runBenchmark()/
// runDiarizationBenchmark() bypassed the queue entirely to avoid the
// main-panel overlay) into real queued Tasks with Presentation::Background —
// the whole reason Presentation exists as an axis separate from "is this
// queued at all". Only structural interface + the offline decode-failure
// path are exercised for real; an actual model run needs a real downloaded
// model, same precedent as the other engine-backed kinds.

#include <QTemporaryDir>
#include <QUrl>
#include <QtTest>

#include "tasks/BenchmarkTask.h"
#include "tasks/DiarizationSweepTask.h"
#include "tasks/EngineCache.h"
#include "tasks/TaskContext.h"

namespace {

struct NullContextFixture {
    EngineCache engines;
    TaskControl control;
    TaskContext ctx{engines, control, [](bool) {}};
};

} // namespace

class TestTaskBenchmark : public QObject {
    Q_OBJECT
private slots:
    void benchmarkTask_isBackgroundPresentation_notForeground()
    {
        BenchmarkTask task(QUrl::fromLocalFile("/vault/a.wav"), QUrl::fromLocalFile("/vault"),
                            {QStringLiteral("small.en")});
        QCOMPARE(task.kind(), QStringLiteral("benchmark"));
        // The whole point of this port: benchmark no longer needs a side
        // door to avoid the main-panel overlay — it just declares Background.
        QCOMPARE(task.presentation(), Task::Presentation::Background);
        QVERIFY(!task.capabilities().pausable);
        QVERIFY(task.capabilities().cancellableMidRun);
    }

    void benchmarkTask_decodeFailure_throwsTaskError_noNetwork()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        BenchmarkTask task(QUrl::fromLocalFile(dir.path() + "/nonexistent.wav"), QUrl::fromLocalFile(dir.path()), {QStringLiteral("small.en")});
        NullContextFixture fixture;
        QVERIFY_EXCEPTION_THROWN(task.run(fixture.ctx), TaskError);
    }

    void diarizationSweepTask_isBackgroundPresentation_notPausable()
    {
        DiarizationSweepTask task(QUrl::fromLocalFile("/vault/a.wav"), QVariantList{});
        QCOMPARE(task.kind(), QStringLiteral("diarizationSweep"));
        QCOMPARE(task.presentation(), Task::Presentation::Background);
        QVERIFY(!task.capabilities().pausable);
        QVERIFY(!task.capabilities().cancellableMidRun); // no mid-pass abort hook
    }

    void diarizationSweepTask_decodeFailure_throwsTaskError_noNetwork()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        DiarizationSweepTask task(QUrl::fromLocalFile(dir.path() + "/nonexistent.wav"), QVariantList{});
        NullContextFixture fixture;
        QVERIFY_EXCEPTION_THROWN(task.run(fixture.ctx), TaskError);
    }
};

QTEST_GUILESS_MAIN(TestTaskBenchmark)
#include "tst_task_benchmark.moc"
