// Spec under test — TranscriptionController's task queue (Milestone 4/5)
// and CLAUDE.md's "Never run transcription on the GUI thread."
//
//   - transcribe() queues; no duplicate entry while a fileUrl is already
//     Queued/Active. Done/Failed entries deliberately do NOT block
//     re-queueing (re-transcribe after dismissal / settings change).
//   - Jobs run one at a time on a dedicated worker QThread; the job list is
//     a persistent, dismissible download-manager-style history (jobs stay
//     visible as done/failed until dismiss()/clearFinished()).
//
// Test strategy (no network, no GPU, no display): pre-create an EMPTY fake
// model file at <vault>/.transcriber/models/ggml-base.en.bin —
// ModelManager::ensureModel returns any existing file without downloading
// (ModelManager.cpp:42). The worker then fails fast when whisper refuses to
// load the zero-byte model ("failed to load model at ..."), before audio
// decode is even attempted. Every job therefore deterministically ends
// Failed, in milliseconds, exercising the full queue lifecycle.
//
// The worker is asynchronous, so all post-start assertions on job state use
// QTRY_ macros. Assertions made between transcribe() calls WITHOUT spinning
// the event loop are deterministic: startJob() runs synchronously in
// transcribe(), while completion (onWorkerFinished) only arrives via a
// queued cross-thread signal, i.e. once the test's event loop runs.
//
// Teardown: ~TranscriptionController() quits and wait()s its QThread
// (TranscriptionController.cpp:174-178), so a stack-allocated controller per
// test tears down cleanly and ctest never hangs; each test still QTRY-waits
// for its jobs to settle before returning.

#include <QtTest>

#include "TranscriptionController.h"

namespace {

// Temp vault with the fake (empty, unloadable) model already "downloaded",
// so ensureModel is satisfied offline.
struct VaultFixture {
    QTemporaryDir dir;
    QUrl vaultUrl;

    VaultFixture()
    {
        const QString modelsDir = dir.path() + "/.transcriber/models";
        QDir().mkpath(modelsDir);
        QFile model(modelsDir + "/ggml-base.en.bin"); // ModelManager fileName for id "base.en"
        const bool opened = model.open(QIODevice::WriteOnly); // zero bytes: exists, whisper can't load it
        Q_ASSERT(opened);
        model.close();
        vaultUrl = QUrl::fromLocalFile(dir.path());
    }

    QUrl audioUrl(const QString &name) const { return QUrl::fromLocalFile(dir.path() + "/" + name); }
};

QString statusOf(const TranscriptionController &c, const QUrl &fileUrl)
{
    const QVariantList jobs = c.jobs();
    for (const QVariant &v : jobs) {
        const QVariantMap job = v.toMap();
        if (job.value("fileUrl").toUrl() == fileUrl)
            return job.value("status").toString();
    }
    return {};
}

constexpr auto kModelId = "base.en";

}

class TstTranscriptionQueue : public QObject
{
    Q_OBJECT

private slots:
    // A job that can't run (here: unloadable model; a missing audio file
    // fails the same way one step later) ends as status "failed", and
    // finished(fileUrl, ok=false, error) fires with a non-empty error — the
    // job list keeps the failed entry with its error, download-manager
    // style, rather than silently dropping it.
    void failedJob_endsFailed_andEmitsFinishedWithError()
    {
        VaultFixture vault;
        TranscriptionController controller;
        QSignalSpy finishedSpy(&controller, &TranscriptionController::finished);
        const QUrl missing = vault.audioUrl("does-not-exist.wav");

        controller.transcribe(missing, vault.vaultUrl, kModelId, false);

        QTRY_COMPARE(finishedSpy.count(), 1);
        const QList<QVariant> args = finishedSpy.takeFirst();
        QCOMPARE(args.at(0).toUrl(), missing);
        QCOMPARE(args.at(1).toBool(), false);
        QVERIFY(!args.at(2).toString().isEmpty());

        QCOMPARE(statusOf(controller, missing), QStringLiteral("failed"));
        // The per-job error is exposed on the jobs list too.
        QVERIFY(!controller.jobs().first().toMap().value("error").toString().isEmpty());
    }

    // "No duplicate entries": a second transcribe() for a fileUrl that is
    // already Queued/Active is a no-op — isQueued() is true for the whole
    // in-flight window.
    void duplicateWhileQueuedOrActive_isRejected()
    {
        VaultFixture vault;
        TranscriptionController controller;
        const QUrl file = vault.audioUrl("a.wav");

        controller.transcribe(file, vault.vaultUrl, kModelId, false);
        // First call started synchronously (Active); the duplicate must bounce.
        QVERIFY(controller.isQueued(file));
        controller.transcribe(file, vault.vaultUrl, kModelId, false);
        QCOMPARE(controller.tasksTotal(), 1);

        QTRY_COMPARE(statusOf(controller, file), QStringLiteral("failed"));
        QVERIFY(!controller.isQueued(file)); // Failed no longer counts as queued
        QCOMPARE(controller.tasksTotal(), 1);
    }

    // Dedup is on Queued/Active ONLY: a Done/Failed entry does not block
    // re-queueing the same fileUrl (deliberate — re-transcribe after
    // dismissal or after changing settings). The re-run is a NEW entry, so
    // both attempts stay visible in the history.
    void finishedJob_doesNotBlockRequeue()
    {
        VaultFixture vault;
        TranscriptionController controller;
        const QUrl file = vault.audioUrl("a.wav");

        controller.transcribe(file, vault.vaultUrl, kModelId, false);
        QTRY_COMPARE(statusOf(controller, file), QStringLiteral("failed"));

        controller.transcribe(file, vault.vaultUrl, kModelId, false);
        QCOMPARE(controller.tasksTotal(), 2); // failed entry + fresh re-queued entry

        QTRY_COMPARE(controller.tasksCompleted(), 2);
        const QVariantList jobs = controller.jobs();
        QCOMPARE(jobs.size(), 2);
        for (const QVariant &v : jobs)
            QCOMPARE(v.toMap().value("status").toString(), QStringLiteral("failed"));
    }

    // One job at a time, FIFO: A starts immediately (Active), B waits as
    // Queued (visible via queuedFileUrls), and auto-starts when A finishes.
    // tasksTotal counts every entry; tasksCompleted counts Done+Failed and
    // only grows until dismiss()/clearFinished().
    void queueOrdering_secondJobWaitsThenRuns_countersTrack()
    {
        VaultFixture vault;
        TranscriptionController controller;
        const QUrl fileA = vault.audioUrl("a.wav");
        const QUrl fileB = vault.audioUrl("b.wav");

        controller.transcribe(fileA, vault.vaultUrl, kModelId, false);
        controller.transcribe(fileB, vault.vaultUrl, kModelId, false);

        // Deterministic pre-event-loop snapshot: A active, B queued.
        QCOMPARE(controller.tasksTotal(), 2);
        QCOMPARE(statusOf(controller, fileA), QStringLiteral("active"));
        QCOMPARE(statusOf(controller, fileB), QStringLiteral("queued"));
        QCOMPARE(controller.queuedFileUrls(), QVariantList{fileB});
        QCOMPARE(controller.tasksCompleted(), 0);

        QTRY_COMPARE(controller.tasksCompleted(), 2);
        QCOMPARE(controller.tasksTotal(), 2);
        // Insertion order preserved in the history.
        QCOMPARE(controller.jobs().at(0).toMap().value("fileUrl").toUrl(), fileA);
        QCOMPARE(controller.jobs().at(1).toMap().value("fileUrl").toUrl(), fileB);
        QCOMPARE(statusOf(controller, fileA), QStringLiteral("failed"));
        QCOMPARE(statusOf(controller, fileB), QStringLiteral("failed"));
    }

    // dismiss() removes exactly one Done/Failed entry for the fileUrl;
    // Queued/Active entries are never dismissible (no-op), and dismissing an
    // unknown url is a no-op. (The Queued case is deterministically testable
    // — job B below is provably still Queued before the event loop runs; the
    // Active case follows the same JobStatus guard in dismiss().)
    void dismiss_removesFinishedEntriesOnly()
    {
        VaultFixture vault;
        TranscriptionController controller;
        const QUrl fileA = vault.audioUrl("a.wav");
        const QUrl fileB = vault.audioUrl("b.wav");

        controller.transcribe(fileA, vault.vaultUrl, kModelId, false);
        controller.transcribe(fileB, vault.vaultUrl, kModelId, false);

        // A is Active, B is Queued: neither may be dismissed yet.
        controller.dismiss(fileA);
        controller.dismiss(fileB);
        QCOMPARE(controller.tasksTotal(), 2);

        QTRY_COMPARE(controller.tasksCompleted(), 2);

        controller.dismiss(vault.audioUrl("never-queued.wav")); // unknown url: no-op
        QCOMPARE(controller.tasksTotal(), 2);

        controller.dismiss(fileA);
        QCOMPARE(controller.tasksTotal(), 1);
        QCOMPARE(statusOf(controller, fileA), QString()); // gone from the list
        QCOMPARE(statusOf(controller, fileB), QStringLiteral("failed"));

        controller.dismiss(fileB);
        QCOMPARE(controller.tasksTotal(), 0);
        QVERIFY(controller.jobs().isEmpty());
    }

    // clearFinished() bulk-removes every Done/Failed entry but leaves
    // Queued/Active jobs alone — clearing the history must never cancel
    // in-flight work.
    void clearFinished_removesDoneAndFailed_leavesPendingWork()
    {
        VaultFixture vault;
        TranscriptionController controller;
        const QUrl fileA = vault.audioUrl("a.wav");
        const QUrl fileB = vault.audioUrl("b.wav");

        controller.transcribe(fileA, vault.vaultUrl, kModelId, false);
        controller.transcribe(fileB, vault.vaultUrl, kModelId, false);

        // Nothing finished yet (A active, B queued): clearFinished is a no-op.
        controller.clearFinished();
        QCOMPARE(controller.tasksTotal(), 2);

        QTRY_COMPARE(controller.tasksCompleted(), 2);

        // All-finished list -> emptied in one call.
        controller.clearFinished();
        QCOMPARE(controller.tasksTotal(), 0);
        QCOMPARE(controller.tasksCompleted(), 0);
        QVERIFY(controller.jobs().isEmpty());
    }

    // transcribeAll() is a pure batch wrapper: each url goes through
    // transcribe() and therefore through the same Queued/Active dedup — a
    // url repeated within the batch is enqueued once.
    void transcribeAll_enqueuesEachUrl_withSameDedupRules()
    {
        VaultFixture vault;
        TranscriptionController controller;
        const QUrl fileA = vault.audioUrl("a.wav");
        const QUrl fileB = vault.audioUrl("b.wav");

        controller.transcribeAll({fileA, fileA, fileB}, vault.vaultUrl, kModelId, false);
        QCOMPARE(controller.tasksTotal(), 2); // duplicate A collapsed

        QTRY_COMPARE(controller.tasksCompleted(), 2);
        QCOMPARE(statusOf(controller, fileA), QStringLiteral("failed"));
        QCOMPARE(statusOf(controller, fileB), QStringLiteral("failed"));
    }

    // CLAUDE.md: "Never run transcription on the GUI thread." The worker
    // QThread is a private member (not a QObject child of the controller),
    // so it isn't reachable via findChildren(); the observable contract from
    // the public API is:
    //   1. transcribe() returns immediately with busy already true — the
    //      job was handed off (Qt::QueuedConnection into the worker thread),
    //      not executed inline;
    //   2. the calling (GUI) thread stays free to keep invoking the API;
    //   3. busy flips back to false only once the event loop delivers the
    //      worker's queued cross-thread finished signal (QTRY_ spins it) —
    //      if transcription ran on the GUI thread, transcribe() could not
    //      have returned before completion.
    void transcription_neverRunsOnGuiThread_busyFlipsViaQueuedSignals()
    {
        VaultFixture vault;
        TranscriptionController controller;
        QSignalSpy busySpy(&controller, &TranscriptionController::busyChanged);
        const QUrl file = vault.audioUrl("a.wav");

        QVERIFY(!controller.isBusy());
        controller.transcribe(file, vault.vaultUrl, kModelId, false);

        // (1) busy true synchronously; the work itself hasn't run here.
        QVERIFY(controller.isBusy());
        QCOMPARE(busySpy.count(), 1);

        // (2) GUI thread is not blocked — the API answers mid-job.
        QVERIFY(controller.isQueued(file));
        QCOMPARE(controller.currentFile(), file);

        // (3) completion arrives asynchronously via the event loop.
        QTRY_VERIFY(!controller.isBusy());
        QCOMPARE(busySpy.count(), 2); // true -> false, exactly one round-trip
        QTRY_COMPARE(statusOf(controller, file), QStringLiteral("failed"));
        QCOMPARE(controller.currentFile(), QUrl());
    }
};

QTEST_GUILESS_MAIN(TstTranscriptionQueue)
#include "tst_transcription_queue.moc"
