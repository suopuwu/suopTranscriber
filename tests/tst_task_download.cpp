// Pilot for the task-system refactor (.task/plan.md): validates Task +
// TaskContext + TaskRunner + EngineCache work end-to-end for ONE real kind
// before the other 11 are ported and the queue is rewritten to own Tasks.
// Two things are covered:
//   (1) the GENERIC runner contract, via a FakeTask that doesn't touch any
//       real kind's business logic — progress/phase/paused signals cross the
//       worker->GUI thread boundary; success and a thrown TaskError both
//       resolve to EXACTLY ONE taskFinished (the "one terminal report per
//       task" invariant .task/plan.md calls out); checkpoint() really blocks
//       on pause and unblocks on resume/cancel.
//   (2) DownloadTask's STATIC interface (kind/displayName/presentation/
//       capabilities/isPersistable/toVariantMap) — deliberately does NOT call
//       run() (a real curl process hitting the network — same reason
//       tst_transcription_queue.cpp's
//       transcribe_queuesDownloadJobWhenModelMissing() never waits for its
//       download job to finish).

#include <QSignalSpy>
#include <QThread>
#include <QtTest>

#include "tasks/DownloadTask.h"
#include "tasks/Task.h"
#include "tasks/TaskRunner.h"

namespace {

// Minimal concrete Task for exercising the runner mechanism in isolation.
class FakeTask : public Task {
    Q_OBJECT
public:
    using Task::Task;

    QString kind() const override { return QStringLiteral("fake"); }
    QString displayName() const override { return QStringLiteral("Fake"); }
    Capabilities capabilities() const override { return {pausable, true}; }

    void run(TaskContext &ctx) override
    {
        emit phase(QStringLiteral("working"));
        emit progress(0.5);
        if (!warningToReport.isEmpty())
            ctx.reportWarning(warningToReport);
        if (blockOnCheckpoint && ctx.checkpoint())
            throw TaskError(QStringLiteral("Cancelled"));
        if (shouldThrow)
            throw TaskError(failureMessage);
        emit progress(1.0);
    }

    bool pausable = false;
    bool blockOnCheckpoint = false;
    bool shouldThrow = false;
    QString failureMessage = QStringLiteral("boom");
    // A non-fatal diagnostic a subclass reports via
    // TaskContext::reportWarning() during run() — empty means "nothing to
    // report" (the common case every other FakeTask use leaves it as).
    QString warningToReport;
};

} // namespace

class TestTaskDownload : public QObject {
    Q_OBJECT
private slots:
    void runner_successPath_emitsExactlyOneOkTerminalReport()
    {
        TaskRunner runner;
        QThread thread;
        runner.moveToThread(&thread);
        thread.start();

        FakeTask task;
        QSignalSpy finishedSpy(&runner, &TaskRunner::taskFinished);
        QSignalSpy progressSpy(&task, &Task::progress);
        QSignalSpy phaseSpy(&task, &Task::phase);

        QMetaObject::invokeMethod(&runner, "execute", Qt::QueuedConnection, Q_ARG(Task *, &task));
        QVERIFY(finishedSpy.wait(2000));

        QCOMPARE(finishedSpy.count(), 1);
        const QVariantList args = finishedSpy.constFirst();
        QCOMPARE(args.at(0).value<Task *>(), &task);
        QVERIFY(args.at(1).toBool());
        QVERIFY(args.at(2).toString().isEmpty());
        QVERIFY(phaseSpy.count() >= 1);
        QVERIFY(progressSpy.count() >= 1);

        thread.quit();
        thread.wait();
    }

    // TaskContext::reportWarning() during a run() that otherwise
    // succeeds must surface through taskFinished's warning argument WITH
    // ok=true — a non-fatal diagnostic must never look like a failure at the
    // one place (TaskRunner) every kind's result funnels through.
    void runner_reportedWarning_survivesOnSuccessfulRun()
    {
        TaskRunner runner;
        QThread thread;
        runner.moveToThread(&thread);
        thread.start();

        FakeTask task;
        task.warningToReport = QStringLiteral("196249 corrupt samples replaced with silence");
        QSignalSpy finishedSpy(&runner, &TaskRunner::taskFinished);

        QMetaObject::invokeMethod(&runner, "execute", Qt::QueuedConnection, Q_ARG(Task *, &task));
        QVERIFY(finishedSpy.wait(2000));

        QCOMPARE(finishedSpy.count(), 1);
        const QVariantList args = finishedSpy.constFirst();
        QVERIFY2(args.at(1).toBool(), "a reported warning must not fail the task");
        QVERIFY(args.at(2).toString().isEmpty());
        QCOMPARE(args.at(3).toString(), task.warningToReport);

        thread.quit();
        thread.wait();
    }

    void runner_thrownTaskError_emitsExactlyOneFailedTerminalReport()
    {
        TaskRunner runner;
        QThread thread;
        runner.moveToThread(&thread);
        thread.start();

        FakeTask task;
        task.shouldThrow = true;
        task.failureMessage = QStringLiteral("deliberate failure");
        QSignalSpy finishedSpy(&runner, &TaskRunner::taskFinished);

        QMetaObject::invokeMethod(&runner, "execute", Qt::QueuedConnection, Q_ARG(Task *, &task));
        QVERIFY(finishedSpy.wait(2000));

        QCOMPARE(finishedSpy.count(), 1);
        const QVariantList args = finishedSpy.constFirst();
        QVERIFY(!args.at(1).toBool());
        QCOMPARE(args.at(2).toString(), QStringLiteral("deliberate failure"));

        thread.quit();
        thread.wait();
    }

    void checkpoint_blocksWhilePaused_unblocksOnResume_reportsPausedEdgesOnly()
    {
        TaskRunner runner;
        QThread thread;
        runner.moveToThread(&thread);
        thread.start();

        FakeTask task;
        task.pausable = true;
        task.blockOnCheckpoint = true;
        QSignalSpy finishedSpy(&runner, &TaskRunner::taskFinished);
        QSignalSpy pausedSpy(&task, &Task::paused);

        runner.requestPause();
        QMetaObject::invokeMethod(&runner, "execute", Qt::QueuedConnection, Q_ARG(Task *, &task));

        QTRY_COMPARE_WITH_TIMEOUT(pausedSpy.count(), 1, 2000);
        QCOMPARE(pausedSpy.constFirst().at(0).toBool(), true);
        QVERIFY(!finishedSpy.wait(300)); // still blocked — no terminal report yet

        runner.requestResume();
        QVERIFY(finishedSpy.wait(2000));
        QCOMPARE(pausedSpy.count(), 2);
        QCOMPARE(pausedSpy.at(1).at(0).toBool(), false);
        QVERIFY(finishedSpy.constFirst().at(1).toBool()); // resumed, not cancelled -> ok

        thread.quit();
        thread.wait();
    }

    void checkpoint_cancelWhilePaused_unblocksAsCancelled()
    {
        TaskRunner runner;
        QThread thread;
        runner.moveToThread(&thread);
        thread.start();

        FakeTask task;
        task.pausable = true;
        task.blockOnCheckpoint = true;
        QSignalSpy finishedSpy(&runner, &TaskRunner::taskFinished);

        runner.requestPause();
        QMetaObject::invokeMethod(&runner, "execute", Qt::QueuedConnection, Q_ARG(Task *, &task));
        QTest::qWait(200);
        runner.requestCancel();

        QVERIFY(finishedSpy.wait(2000));
        const QVariantList args = finishedSpy.constFirst();
        QVERIFY(!args.at(1).toBool());
        QCOMPARE(args.at(2).toString(), QStringLiteral("Cancelled"));

        thread.quit();
        thread.wait();
    }

    // --- DownloadTask's static interface — no run(), no network. ---

    void downloadTask_reportsKindAndPresentationAndCapabilities()
    {
        DownloadTask task(ModelDownloadTarget::WhisperModel, QStringLiteral("small.en"));
        QCOMPARE(task.kind(), QStringLiteral("download"));
        QCOMPARE(task.presentation(), Task::Presentation::Background);
        const Task::Capabilities caps = task.capabilities();
        QVERIFY(caps.pausable);
        QVERIFY(caps.cancellableMidRun);
        QVERIFY(task.isPersistable());
    }

    void downloadTask_toVariantMap_matchesOldJobsRowShape()
    {
        DownloadTask task(ModelDownloadTarget::SegmentationModel, QStringLiteral("seg-a"));
        const QVariantMap map = task.toVariantMap();
        QCOMPARE(map.value(QStringLiteral("downloadTarget")).toString(), QStringLiteral("segmentation"));
        QCOMPARE(map.value(QStringLiteral("modelId")).toString(), QStringLiteral("seg-a"));
    }

    void downloadTask_displayName_isNonEmptyForEveryTarget()
    {
        const QList<ModelDownloadTarget> targets = {
            ModelDownloadTarget::WhisperModel,
            ModelDownloadTarget::LabelModel,
            ModelDownloadTarget::VadModel,
            ModelDownloadTarget::SegmentationModel,
            ModelDownloadTarget::EmbeddingModel,
        };
        for (const auto target : targets) {
            DownloadTask task(target, QStringLiteral("some-id"));
            QVERIFY(!task.displayName().isEmpty());
        }
    }
};

QTEST_GUILESS_MAIN(TestTaskDownload)
#include "tst_task_download.moc"
