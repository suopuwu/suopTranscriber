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

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QProcess>
#include <QSettings>
#include <QStandardPaths>
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

// Same detection strategy as tst_transcript_format's locateRealModel(): env
// override first, else the user's real vault, preferring a plain (non-tdrz)
// .en model — needed for the one test in this file that requires a model
// that actually LOADS (VaultFixture's zero-byte fake fails too fast/on the
// wrong step to test a checkpoint that only exists AFTER a real load).
QString locateRealModel()
{
    const QString envModel = qEnvironmentVariable("SUOP_TEST_MODEL");
    if (!envModel.isEmpty() && QFileInfo::exists(envModel))
        return envModel;

    QDir vaultModels(QStringLiteral("/home/user/recordings/.transcriber/models"));
    const QStringList bins = vaultModels.entryList({QStringLiteral("*.bin")}, QDir::Files, QDir::Name);
    QString fallback;
    for (const QString &bin : bins) {
        if (bin.contains(QStringLiteral("tdrz")))
            continue;
        if (bin.contains(QStringLiteral(".en")))
            return vaultModels.filePath(bin);
        if (fallback.isEmpty())
            fallback = vaultModels.filePath(bin);
    }
    return fallback;
}

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
    // TranscriptionController persists rolling-average phase timing via a
    // plain QSettings() (see RollingAverage.h / loadPhaseTimings()) — CLAUDE.md
    // "Never let testing touch the user's real vault or app config" applies
    // here just as much as to lastVaultPath: setTestModeEnabled() redirects every
    // QSettings/QStandardPaths lookup for the rest of this process into an
    // isolated "qttest" sandbox instead of the real ~/.config/suopTranscriber.
    void initTestCase() { QStandardPaths::setTestModeEnabled(true); }

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

    // 2026-07-07: renameSpeaker() is queued through this SAME job pipeline
    // (not a side channel invisible to the UI) — it must show up in
    // tasksTotal/jobs() like any other task, the way the user expects to see
    // it as a real background task, not just "async and untracked".
    void renameSpeaker_showsUpAsATaskInTheJobQueue()
    {
        VaultFixture vault;
        const QUrl audioUrl = vault.audioUrl("interview.wav");
        QFile md(vault.dir.path() + "/interview.wav.md");
        QVERIFY(md.open(QIODevice::WriteOnly));
        md.write("---\nmodel: ggml-base.en.bin\nspeakerDetection: true\nspeakers: Speaker 1, Speaker 2\n---\n\n"
                 "[0] [Speaker 1] hi\n[3.5] [Speaker 2] hello\n");
        md.close();

        TranscriptionController controller;
        QSignalSpy queueChangedSpy(&controller, &TranscriptionController::queueChanged);
        QSignalSpy renamedSpy(&controller, &TranscriptionController::speakerRenamed);

        controller.renameSpeaker(audioUrl, QStringLiteral("Speaker 1"), QStringLiteral("Alice"));

        // Queued (and started, since nothing else is running) synchronously —
        // observable in tasksTotal/jobs() immediately, same timing as
        // transcribe()'s own startJob() call.
        QCOMPARE(controller.tasksTotal(), 1);
        QVERIFY(queueChangedSpy.count() >= 1);
        QCOMPARE(controller.jobs().first().toMap().value("fileUrl").toUrl(), audioUrl);

        QVERIFY(renamedSpy.wait(5000));
        QTRY_COMPARE(statusOf(controller, audioUrl), QStringLiteral("done"));
        QCOMPARE(controller.tasksCompleted(), 1);
    }

    // mergeSpeakers() mirrors renameSpeaker_showsUpAsATaskInTheJobQueue
    // immediately above — same job pipeline, same visibility contract.
    void mergeSpeakers_showsUpAsATaskInTheJobQueue()
    {
        VaultFixture vault;
        const QUrl audioUrl = vault.audioUrl("interview.wav");
        QFile md(vault.dir.path() + "/interview.wav.md");
        QVERIFY(md.open(QIODevice::WriteOnly));
        md.write("---\nmodel: ggml-base.en.bin\nspeakerDetection: true\nspeakers: Speaker 1, Speaker 2\n---\n\n"
                 "[0] [Speaker 1] hi\n[3.5] [Speaker 2] hello\n");
        md.close();

        TranscriptionController controller;
        QSignalSpy queueChangedSpy(&controller, &TranscriptionController::queueChanged);
        QSignalSpy mergedSpy(&controller, &TranscriptionController::speakersMerged);

        controller.mergeSpeakers(audioUrl, QStringLiteral("Speaker 2"), QStringLiteral("Speaker 1"));

        QCOMPARE(controller.tasksTotal(), 1);
        QVERIFY(queueChangedSpy.count() >= 1);
        QCOMPARE(controller.jobs().first().toMap().value("fileUrl").toUrl(), audioUrl);
        QCOMPARE(controller.jobs().first().toMap().value("kind").toString(), QStringLiteral("merge"));

        QVERIFY(mergedSpy.wait(5000));
        QTRY_COMPARE(statusOf(controller, audioUrl), QStringLiteral("done"));
        QCOMPARE(controller.tasksCompleted(), 1);
    }

    // jobs()' new "kind" key lets the UI tell a full transcription apart
    // from a "regenerate label" job or a speaker rename — they all share
    // this one queue/worker thread and otherwise look identical (same
    // fileUrl/status shape).
    void jobs_tagEachEntryWithItsKind()
    {
        VaultFixture vault;

        const QUrl transcribeFile = vault.audioUrl("a.wav");
        const QUrl relabelFile = vault.audioUrl("b.wav");
        QFile relabelMd(vault.dir.path() + "/b.wav.md");
        QVERIFY(relabelMd.open(QIODevice::WriteOnly));
        relabelMd.write("---\nmodel: ggml-base.en.bin\n---\n\n[0] hello\n");
        relabelMd.close();
        // Fake (empty) label model, same idea as VaultFixture's whisper
        // model above — makes ModelManager::ensureLabelModel find it
        // already "downloaded" instead of attempting a real network
        // fetch; the labeling engine then fails fast on the zero-byte
        // file, same "deterministic Failed status" strategy as this
        // whole file's transcribe jobs (see the file-level comment).
        QFile labelModel(vault.dir.path() + "/.transcriber/models/Qwen3-4B-Instruct-2507-Q4_K_M.gguf");
        QVERIFY(labelModel.open(QIODevice::WriteOnly));
        labelModel.close();

        const QUrl renameFile = vault.audioUrl("c.wav");
        QFile renameMd(vault.dir.path() + "/c.wav.md");
        QVERIFY(renameMd.open(QIODevice::WriteOnly));
        renameMd.write("---\nmodel: ggml-base.en.bin\nspeakerDetection: true\nspeakers: Speaker 1\n---\n\n"
                       "[0] [Speaker 1] hi\n");
        renameMd.close();

        TranscriptionController controller;
        // Queued one after another — only the first actually starts
        // immediately (single worker), the rest wait their turn, but kind
        // is set at enqueue time regardless of status.
        controller.transcribe(transcribeFile, vault.vaultUrl, kModelId, false);
        controller.relabel(relabelFile, vault.vaultUrl);
        controller.renameSpeaker(renameFile, QStringLiteral("Speaker 1"), QStringLiteral("Alice"));

        QCOMPARE(controller.tasksTotal(), 3);
        QCOMPARE(controller.jobs().at(0).toMap().value("kind").toString(), QStringLiteral("transcribe"));
        QCOMPARE(controller.jobs().at(1).toMap().value("kind").toString(), QStringLiteral("relabel"));
        QCOMPARE(controller.jobs().at(2).toMap().value("kind").toString(), QStringLiteral("rename"));

        // The first job started synchronously (Active) — currentJobKind()
        // should reflect it immediately, same timing as currentFile().
        QCOMPARE(controller.currentJobKind(), QStringLiteral("transcribe"));

        QTRY_COMPARE(controller.tasksCompleted(), 3);
        // Idle again once every job has finished.
        QCOMPARE(controller.currentJobKind(), QString());
    }

    // 2026-07-07: transcribe(autoLabel=true) no longer runs labeling as a
    // silent bonus step inside the SAME job, NOR does it wait for
    // transcription to finish before even creating a label task — it queues
    // a SEPARATE "relabel" job immediately, right alongside the transcribe
    // job, in the SAME transcribe() call (see its doc comment) — both show
    // up in tasksTotal()/jobs() together right away, the label entry
    // starting life as "queued" until the transcribe job ahead of it
    // finishes. It still runs (and, here, fails — nothing to label without a
    // real model) even when the transcription it followed failed, rather
    // than being silently dropped.
    void autoLabel_queuesBothTasksImmediately()
    {
        VaultFixture vault;
        // Fake (empty) label model already "downloaded" — same idea as
        // VaultFixture's whisper model — so this test's transcribe(autoLabel)
        // call queues exactly the transcribe+relabel pair it's actually
        // testing, without ALSO queuing a Wave N "download" job ahead of the
        // relabel job (that's its own concern, covered separately).
        QFile labelModel(vault.dir.path() + "/.transcriber/models/Qwen3-4B-Instruct-2507-Q4_K_M.gguf");
        QVERIFY(labelModel.open(QIODevice::WriteOnly));
        labelModel.close();

        TranscriptionController controller;
        const QUrl file = vault.audioUrl("a.wav");

        controller.transcribe(file, vault.vaultUrl, kModelId, false, /*autoLabel=*/true);

        // Both present immediately — not one appended later.
        QCOMPARE(controller.tasksTotal(), 2);
        QCOMPARE(controller.jobs().at(0).toMap().value("kind").toString(), QStringLiteral("transcribe"));
        QCOMPARE(controller.jobs().at(0).toMap().value("status").toString(), QStringLiteral("active"));
        QCOMPARE(controller.jobs().at(1).toMap().value("kind").toString(), QStringLiteral("relabel"));
        QCOMPARE(controller.jobs().at(1).toMap().value("status").toString(), QStringLiteral("queued"));

        // The fake model fails the transcription fast; the label job then
        // takes its turn and fails too (no transcript exists to label).
        QTRY_COMPARE(controller.tasksCompleted(), 2);
        QCOMPARE(controller.jobs().at(0).toMap().value("status").toString(), QStringLiteral("failed"));
        QCOMPARE(controller.jobs().at(1).toMap().value("status").toString(), QStringLiteral("failed"));
    }

    // Wave N (task system, item 5): transcribe() queues a "download" job
    // ahead of the transcribe job when the whisper model isn't on disk yet
    // — VaultFixture deliberately does NOT plant a model file here (unlike
    // every other test in this file), so this is the one case where that's
    // exactly what's being tested.
    void transcribe_queuesDownloadJobWhenModelMissing()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QUrl vaultUrl = QUrl::fromLocalFile(dir.path());
        const QUrl file = QUrl::fromLocalFile(dir.path() + "/a.wav");

        TranscriptionController controller;
        controller.transcribe(file, vaultUrl, kModelId, false);

        QCOMPARE(controller.tasksTotal(), 2);
        QCOMPARE(controller.jobs().at(0).toMap().value("kind").toString(), QStringLiteral("download"));
        QCOMPARE(controller.jobs().at(0).toMap().value("status").toString(), QStringLiteral("active"));
        QCOMPARE(controller.jobs().at(1).toMap().value("kind").toString(), QStringLiteral("transcribe"));
        QCOMPARE(controller.jobs().at(1).toMap().value("status").toString(), QStringLiteral("queued"));

        // Deliberately does NOT wait for the download job to actually
        // finish (it's a real curl process hitting the network — slow/
        // flaky/environment-dependent) — the enqueue-time queue SHAPE
        // above is this test's whole point. Cancel it before the
        // controller is torn down so its destructor's QThread::wait()
        // doesn't block on a real network timeout.
        controller.cancelJob(controller.jobs().at(0).toMap().value("id").toInt());
    }

    // Item 3: cancelling a Queued job removes it outright (nothing had
    // started, nothing to interrupt); cancelling the Active job lets it run
    // to a Failed/"Cancelled" completion rather than vanishing instantly
    // (see TranscriptionController::cancelJob()'s doc comment).
    void cancelJob_removesQueuedEntry()
    {
        VaultFixture vault;
        TranscriptionController controller;
        const QUrl activeFile = vault.audioUrl("a.wav");
        const QUrl queuedFile = vault.audioUrl("b.wav");

        controller.transcribe(activeFile, vault.vaultUrl, kModelId, false);
        controller.transcribe(queuedFile, vault.vaultUrl, kModelId, false);
        QCOMPARE(controller.tasksTotal(), 2);

        // Nothing has started for the Queued entry — cancelling it is a
        // plain removal, no interruption needed.
        const int queuedId = controller.jobs().at(1).toMap().value("id").toInt();
        controller.cancelJob(queuedId);
        QCOMPARE(controller.tasksTotal(), 1);
        QCOMPARE(controller.jobs().at(0).toMap().value("status").toString(), QStringLiteral("active"));

        QTRY_COMPARE(controller.tasksCompleted(), 1); // let the fake (fails-fast) model job settle before teardown
    }

    // Item 3: cancelling the ACTIVE job takes effect (Failed/"Cancelled")
    // rather than leaving it running to completion. Uses a "download" job
    // as the Active job under test — see maybeQueueDownloadJob()'s doc
    // comment for why VaultFixture's normal fake model would fail on its
    // own (bad magic) before ever reaching an interruption point, which
    // would make this test race against that unrelated failure instead of
    // actually exercising cancellation.
    void cancelJob_stopsActiveJob()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QUrl vaultUrl = QUrl::fromLocalFile(dir.path());
        const QUrl file = QUrl::fromLocalFile(dir.path() + "/a.wav");

        TranscriptionController controller;
        controller.transcribe(file, vaultUrl, kModelId, false);
        QCOMPARE(controller.jobs().at(0).toMap().value("kind").toString(), QStringLiteral("download"));
        QCOMPARE(controller.jobs().at(0).toMap().value("status").toString(), QStringLiteral("active"));

        const int downloadId = controller.jobs().at(0).toMap().value("id").toInt();
        controller.cancelJob(downloadId);
        QTRY_COMPARE(controller.jobs().at(0).toMap().value("status").toString(), QStringLiteral("failed"));
        QCOMPARE(controller.jobs().at(0).toMap().value("error").toString(), QStringLiteral("Cancelled"));
    }

    // User correction (2026-07-09): "loading" (model load) has no
    // interruption point at all (whisper_init_from_file_with_params is one
    // blocking call, no callback hook) — pauseActive() must no-op there
    // rather than queue a pause that only takes effect much later.
    void pauseActive_noopsDuringLoadingPhase()
    {
        VaultFixture vault;
        TranscriptionController controller;
        controller.transcribe(vault.audioUrl("a.wav"), vault.vaultUrl, kModelId, false);
        QTRY_COMPARE(controller.currentPhase(), QStringLiteral("loading"));

        controller.pauseActive();
        QVERIFY(!controller.isActivePaused());

        QTRY_COMPARE(controller.tasksCompleted(), 1); // let the fake model's fast failure settle
    }

    // Regression (2026-07-09 bug report — "cancel all doesn't cancel active
    // transcriptions"): a cancel requested during "loading" was previously
    // only caught AFTER the full audio decode (and diarization, if
    // requested) — for a long recording, that could look exactly like
    // "cancel does nothing" for a real user (the checkpoint existed, but
    // only far later than the phase they clicked cancel during). Gated on
    // a real model since VaultFixture's zero-byte fake fails at the
    // model-load step itself, before ever reaching this checkpoint.
    void cancelDuringLoading_caughtRightAfterModelLoad_notAfterFullDecode()
    {
        const QString modelPath = locateRealModel();
        if (modelPath.isEmpty())
            QSKIP("no real ggml model available; set SUOP_TEST_MODEL or transcribe once in the app");
        if (QStandardPaths::findExecutable(QStringLiteral("ffmpeg")).isEmpty())
            QSKIP("ffmpeg not on PATH — needed to decode the sample");

        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        const QString modelsDir = vault.path() + "/.transcriber/models";
        QVERIFY(QDir().mkpath(modelsDir));
        QVERIFY(QFile::copy(modelPath, modelsDir + "/ggml-base.en.bin"));
        const QString samplePath = vault.path() + "/jfk.wav";
        QVERIFY(QFile::copy(QStringLiteral(SUOP_SAMPLES_DIR "/jfk.wav"), samplePath));

        TranscriptionController controller;
        QSignalSpy finishedSpy(&controller, &TranscriptionController::finished);
        controller.transcribe(QUrl::fromLocalFile(samplePath), QUrl::fromLocalFile(vault.path()), kModelId, false);
        // Called the instant the job starts — mirrors clicking "Cancel all"
        // while the bottom bar still reads "loading model…".
        controller.cancelAll();

        QVERIFY(finishedSpy.wait(10000));
        QCOMPARE(finishedSpy.at(0).at(1).toBool(), false);
        QCOMPARE(finishedSpy.at(0).at(2).toString(), QStringLiteral("Cancelled"));
    }

    // Bug report (2026-07-09): "the pause button doesn't work for the rest
    // of the session" after cancelling a job during its "loading" phase.
    // Reproduces the exact user sequence: cancel job #1 while it's loading,
    // then queue job #2 and try to pause IT during real transcribing.
    void pauseActive_stillWorksOnNextJob_afterCancelDuringPreviousLoading()
    {
        const QString modelPath = locateRealModel();
        if (modelPath.isEmpty())
            QSKIP("no real ggml model available; set SUOP_TEST_MODEL or transcribe once in the app");
        if (QStandardPaths::findExecutable(QStringLiteral("ffmpeg")).isEmpty())
            QSKIP("ffmpeg not on PATH — needed to decode the sample");

        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        const QString modelsDir = vault.path() + "/.transcriber/models";
        QVERIFY(QDir().mkpath(modelsDir));
        QVERIFY(QFile::copy(modelPath, modelsDir + "/ggml-base.en.bin"));
        const QString sampleA = vault.path() + "/a.wav";
        const QString sampleB = vault.path() + "/b.wav";
        QVERIFY(QFile::copy(QStringLiteral(SUOP_SAMPLES_DIR "/jfk.wav"), sampleA));
        QVERIFY(QFile::copy(QStringLiteral(SUOP_SAMPLES_DIR "/jfk.wav"), sampleB));

        TranscriptionController controller;
        QSignalSpy finishedSpy(&controller, &TranscriptionController::finished);

        // Job #1: cancel it while still "loading" (mirrors the bottom bar's
        // pause button, which maps to cancelActive() during this phase).
        controller.transcribe(QUrl::fromLocalFile(sampleA), QUrl::fromLocalFile(vault.path()), kModelId, false);
        controller.cancelActive();
        QVERIFY(finishedSpy.wait(10000));
        QCOMPARE(finishedSpy.at(0).at(1).toBool(), false);
        QCOMPARE(finishedSpy.at(0).at(2).toString(), QStringLiteral("Cancelled"));

        // Job #2: a fresh job for a different file. Model is already loaded
        // (cached m_engine from job #1), so this should reach "transcribing"
        // quickly, real decode work in flight.
        controller.transcribe(QUrl::fromLocalFile(sampleB), QUrl::fromLocalFile(vault.path()), kModelId, false);
        QTRY_VERIFY_WITH_TIMEOUT(controller.currentPhase() == QStringLiteral("transcribing"), 10000);

        controller.pauseActive();
        QTRY_VERIFY_WITH_TIMEOUT(controller.isActivePaused(), 5000);

        controller.resumeActive();
        QVERIFY(finishedSpy.wait(10000));
        QCOMPARE(finishedSpy.at(1).at(1).toBool(), true);
    }

    // User correction (2026-07-09, follow-up round): clicking pause during
    // "loading"/"decoding" (no cooperative checkpoint to truly suspend at —
    // see JobStatus::Paused's doc comment) must not present as a failure.
    // It should read as Paused, with no error, resumable with one more
    // click of the same button — unlike cancelActive()/cancelJob(), which
    // are a real, intentional cancel and still end Failed.
    void togglePauseActive_duringLoading_pausesNotFails_andIsResumable()
    {
        const QString modelPath = locateRealModel();
        if (modelPath.isEmpty())
            QSKIP("no real ggml model available; set SUOP_TEST_MODEL or transcribe once in the app");
        if (QStandardPaths::findExecutable(QStringLiteral("ffmpeg")).isEmpty())
            QSKIP("ffmpeg not on PATH — needed to decode the sample");

        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        const QString modelsDir = vault.path() + "/.transcriber/models";
        QVERIFY(QDir().mkpath(modelsDir));
        QVERIFY(QFile::copy(modelPath, modelsDir + "/ggml-base.en.bin"));
        const QString samplePath = vault.path() + "/a.wav";
        QVERIFY(QFile::copy(QStringLiteral(SUOP_SAMPLES_DIR "/jfk.wav"), samplePath));

        TranscriptionController controller;
        QSignalSpy finishedSpy(&controller, &TranscriptionController::finished);

        controller.transcribe(QUrl::fromLocalFile(samplePath), QUrl::fromLocalFile(vault.path()), kModelId, false);
        QTRY_COMPARE(controller.currentPhase(), QStringLiteral("loading"));

        controller.togglePauseActive();
        QVERIFY(finishedSpy.wait(10000));
        QCOMPARE(finishedSpy.at(0).at(1).toBool(), false);
        QCOMPARE(finishedSpy.at(0).at(2).toString(), QStringLiteral("Cancelled"));
        QVERIFY(!controller.isBusy());
        QCOMPARE(controller.jobs().at(0).toMap().value("status").toString(), QStringLiteral("paused"));
        QVERIFY(controller.jobs().at(0).toMap().value("error").toString().isEmpty());

        // One more click of the same button (idle -> startNext()) resumes
        // it — a full restart under the hood, but the job's identity/
        // position is preserved, not a new entry.
        controller.togglePauseActive();
        QTRY_VERIFY(controller.isBusy());
        QCOMPARE(controller.jobs().at(0).toMap().value("status").toString(), QStringLiteral("active"));

        QVERIFY(finishedSpy.wait(10000));
        QCOMPARE(finishedSpy.at(1).at(1).toBool(), true);
        QCOMPARE(controller.jobs().at(0).toMap().value("status").toString(), QStringLiteral("done"));
    }

    // Bug report (2026-07-10): after pausing during "decoding", the user saw
    // nothing happen for a while (expected — no checkpoint exists mid-decode,
    // see JobStatus::Paused's doc comment), then found pause permanently
    // broken once the job reached "finding speech" (the diarization phase).
    // Root cause: nothing told the user the first click had registered, so a
    // second click landed while the soft-cancel from the first was still
    // unresolved — pauseActive()'s real requestPause() stacked on top of the
    // still-pending cancel flag, and waitWhilePausedOrCancelled()'s wait loop
    // never runs while cancelled, so the pause silently went inert. Fixed via
    // activeStopping (pauseActive() now no-ops while it's true). Reproduces
    // the race directly rather than via real timing: both calls happen
    // back-to-back on the test thread, before the worker can possibly have
    // observed the first cancel yet — deterministic, not a sleep-and-hope.
    void pauseActive_duringStopWindow_isNoop_pauseStillWorksAfterResume()
    {
        const QString modelPath = locateRealModel();
        if (modelPath.isEmpty())
            QSKIP("no real ggml model available; set SUOP_TEST_MODEL or transcribe once in the app");
        if (QStandardPaths::findExecutable(QStringLiteral("ffmpeg")).isEmpty())
            QSKIP("ffmpeg not on PATH — needed to decode the sample");

        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        const QString modelsDir = vault.path() + "/.transcriber/models";
        QVERIFY(QDir().mkpath(modelsDir));
        QVERIFY(QFile::copy(modelPath, modelsDir + "/ggml-base.en.bin"));
        const QString samplePath = vault.path() + "/a.wav";
        QVERIFY(QFile::copy(QStringLiteral(SUOP_SAMPLES_DIR "/jfk.wav"), samplePath));

        TranscriptionController controller;
        QSignalSpy finishedSpy(&controller, &TranscriptionController::finished);

        controller.transcribe(QUrl::fromLocalFile(samplePath), QUrl::fromLocalFile(vault.path()), kModelId, false);
        QTRY_COMPARE(controller.currentPhase(), QStringLiteral("loading"));

        // First click: initiates the soft-cancel.
        controller.togglePauseActive();
        QVERIFY(controller.isActiveStopping());
        // Stray second click, landing before the first has resolved — must
        // be a no-op, not a real pause request stacked on the pending cancel.
        controller.pauseActive();
        QVERIFY(!controller.isActivePaused());

        QVERIFY(finishedSpy.wait(10000));
        QCOMPARE(controller.jobs().at(0).toMap().value("status").toString(), QStringLiteral("paused"));
        QVERIFY(!controller.isActiveStopping());

        // Resume, then confirm a NORMAL pause during a real phase still
        // works — this is the actual regression: it used to go permanently
        // inert after the sequence above.
        controller.togglePauseActive();
        QTRY_VERIFY_WITH_TIMEOUT(controller.currentPhase() == QStringLiteral("transcribing"), 10000);
        controller.pauseActive();
        QTRY_VERIFY_WITH_TIMEOUT(controller.isActivePaused(), 5000);
        controller.resumeActive();
        QVERIFY(finishedSpy.wait(10000));
        QCOMPARE(finishedSpy.at(1).at(1).toBool(), true);
    }

    // User follow-up (2026-07-10): "why can we not do something similar [to
    // instant window-close] for the pause button" during decode. Answer:
    // for decode specifically, we can — ffmpeg runs as a real subprocess
    // (see AudioDecoder.h), unlike whisper.cpp's in-process model load.
    // Pausing during "decoding" is now a REAL cooperative pause (isActivePaused
    // becomes true, job stays Active — not the restart-based JobStatus::Paused
    // "loading" still needs). Uses a synthetic tone long enough that decode
    // takes a real, observable amount of time — jfk.wav decodes in well
    // under one polling tick, giving no realistic window to catch "decoding"
    // via QTRY at all.
    void pauseActive_duringDecoding_reallyPauses_notRestart()
    {
        const QString modelPath = locateRealModel();
        if (modelPath.isEmpty())
            QSKIP("no real ggml model available; set SUOP_TEST_MODEL or transcribe once in the app");
        if (QStandardPaths::findExecutable(QStringLiteral("ffmpeg")).isEmpty())
            QSKIP("ffmpeg not on PATH — needed to decode the sample");

        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        const QString modelsDir = vault.path() + "/.transcriber/models";
        QVERIFY(QDir().mkpath(modelsDir));
        QVERIFY(QFile::copy(modelPath, modelsDir + "/ggml-base.en.bin"));

        const QString samplePath = vault.path() + "/a.wav";
        QProcess gen;
        gen.start("ffmpeg", {"-y", "-f", "lavfi", "-i", QStringLiteral("sine=frequency=440:duration=300"),
                              samplePath});
        if (!gen.waitForFinished(30000) || gen.exitCode() != 0 || !QFileInfo::exists(samplePath))
            QSKIP("could not generate synthetic test audio via ffmpeg");

        TranscriptionController controller;
        QSignalSpy finishedSpy(&controller, &TranscriptionController::finished);
        controller.transcribe(QUrl::fromLocalFile(samplePath), QUrl::fromLocalFile(vault.path()), kModelId, false);
        QTRY_COMPARE(controller.currentPhase(), QStringLiteral("decoding"));

        controller.pauseActive();
        QTRY_VERIFY_WITH_TIMEOUT(controller.isActivePaused(), 5000);
        // Still Active/"decoding" — NOT the restart-based Paused status.
        QCOMPARE(controller.jobs().at(0).toMap().value("status").toString(), QStringLiteral("active"));
        QCOMPARE(controller.currentPhase(), QStringLiteral("decoding"));

        controller.resumeActive();
        QVERIFY(finishedSpy.wait(20000));
        QCOMPARE(finishedSpy.at(0).at(1).toBool(), true);
        QCOMPARE(controller.jobs().at(0).toMap().value("status").toString(), QStringLiteral("done"));
    }

    // User correction (2026-07-09): cancelActive() is a real, intentional
    // cancel of whichever job is Active (always ends Failed/"Cancelled") —
    // convenience over cancelJob() so QML doesn't need to look up the
    // Active job's id itself. togglePauseActive() also uses it as a
    // building block for the "loading" soft-pause path (see
    // togglePauseActive_duringLoading_pausesNotFails_andIsResumable), but a
    // DIRECT cancelActive() call — as here — is unconditionally a real
    // cancel. Uses a "download" job as the Active job under test, same
    // reasoning as cancelJob_stopsActiveJob (a real transcribe job's fake
    // model fails too fast to reliably race against).
    void cancelActive_cancelsWhicheverJobIsActive()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QUrl vaultUrl = QUrl::fromLocalFile(dir.path());
        const QUrl file = QUrl::fromLocalFile(dir.path() + "/a.wav");

        TranscriptionController controller;
        controller.transcribe(file, vaultUrl, kModelId, false);
        QCOMPARE(controller.jobs().at(0).toMap().value("status").toString(), QStringLiteral("active"));

        controller.cancelActive();
        QTRY_COMPARE(controller.jobs().at(0).toMap().value("status").toString(), QStringLiteral("failed"));
        QCOMPARE(controller.jobs().at(0).toMap().value("error").toString(), QStringLiteral("Cancelled"));
    }

    // Regression (2026-07-09 bug report — "transcription was still running
    // after closing the GUI... it should never run in the background"):
    // ~TranscriptionController() used to just call m_thread.quit() +
    // m_thread.wait() with nothing ever telling an Active job to stop —
    // wait() blocks until the worker's CURRENT call returns naturally, so
    // destroying the controller mid-job could hang (or keep transcribing)
    // for as long as that job's underlying call took. Uses a "download" job
    // as the Active job under test — see cancelJob_stopsActiveJob's doc
    // comment for why (real, blocking-but-interruptible poll loop, no
    // model/network dependency needed). Threshold picked empirically: in
    // this sandbox (no network), curl's own DNS failure takes ~3.6s with
    // the fix REVERTED (nothing ever cancels it, so the destructor just
    // waits that out) vs. ~0.6s with the fix present (cancelled almost
    // immediately) — 2000ms cleanly separates the two without being so
    // tight that ordinary system jitter could flake it. On a machine WITH
    // network access the "unfixed" duration would be even longer (a real
    // download attempt, not just a fast DNS failure), so this bound stays
    // valid there too.
    void destructor_cancelsActiveJob_doesNotHangWaitingForItToFinishNaturally()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QUrl vaultUrl = QUrl::fromLocalFile(dir.path());
        const QUrl file = QUrl::fromLocalFile(dir.path() + "/a.wav");

        QElapsedTimer timer;
        {
            TranscriptionController controller;
            controller.transcribe(file, vaultUrl, kModelId, false);
            QCOMPARE(controller.jobs().at(0).toMap().value("status").toString(), QStringLiteral("active"));
            timer.start();
            // Controller destructs at end of scope — must not hang waiting
            // for the download to fail/finish on its own.
        }
        QVERIFY2(timer.elapsed() < 2000,
                 "destructor took too long -- did it wait for the job to finish naturally instead of cancelling it?");
    }

    // Item 3: moveQueuedJob() reorders WITHIN the queued sublist only,
    // leaving the Active row's position alone.
    void moveQueuedJob_reordersQueuedEntriesOnly()
    {
        VaultFixture vault;
        TranscriptionController controller;
        const QUrl activeFile = vault.audioUrl("a.wav");
        const QUrl queuedFile1 = vault.audioUrl("b.wav");
        const QUrl queuedFile2 = vault.audioUrl("c.wav");

        controller.transcribe(activeFile, vault.vaultUrl, kModelId, false);
        controller.transcribe(queuedFile1, vault.vaultUrl, kModelId, false);
        controller.transcribe(queuedFile2, vault.vaultUrl, kModelId, false);

        // Item O15: each newly transcribe()d job jumps to the front of the
        // QUEUE (never ahead of the Active row) — so c.wav (queued second)
        // already landed ahead of b.wav (queued first) before any
        // moveQueuedJob() call at all. See
        // transcribe_newJobJumpsAheadOfQueuedWork_butNeverPreemptsActive()
        // for that behavior in isolation; this test only cares that
        // moveQueuedJob() itself reorders Queued entries without touching
        // the Active row.
        QVariantList jobs = controller.jobs();
        QCOMPARE(jobs.at(0).toMap().value("fileUrl").toUrl(), activeFile);
        QCOMPARE(jobs.at(1).toMap().value("fileUrl").toUrl(), queuedFile2);
        QCOMPARE(jobs.at(2).toMap().value("fileUrl").toUrl(), queuedFile1);

        const int lastQueuedId = jobs.at(2).toMap().value("id").toInt();
        controller.moveQueuedJob(lastQueuedId, 0); // move b.wav to the front of the queued sublist

        jobs = controller.jobs();
        QCOMPARE(jobs.at(0).toMap().value("fileUrl").toUrl(), activeFile); // Active row untouched
        QCOMPARE(jobs.at(1).toMap().value("fileUrl").toUrl(), queuedFile1); // moved to front
        QCOMPARE(jobs.at(2).toMap().value("fileUrl").toUrl(), queuedFile2);
    }

    // Item O15: a freshly transcribe()d job jumps ahead of whatever's
    // already Queued, so it runs NEXT once the Active job finishes — but it
    // must never preempt the job actually running right now. Three files
    // queued back-to-back land as [active, newest-queued, ..., oldest-
    // queued] — the exact reverse of plain FIFO append order — because each
    // one is inserted right behind the Active row, ahead of whatever was
    // already sitting there.
    void transcribe_newJobJumpsAheadOfQueuedWork_butNeverPreemptsActive()
    {
        VaultFixture vault;
        TranscriptionController controller;
        const QUrl activeFile = vault.audioUrl("a.wav");
        const QUrl firstQueued = vault.audioUrl("b.wav");
        const QUrl secondQueued = vault.audioUrl("c.wav");

        controller.transcribe(activeFile, vault.vaultUrl, kModelId, false);
        QCOMPARE(statusOf(controller, activeFile), QStringLiteral("active"));

        controller.transcribe(firstQueued, vault.vaultUrl, kModelId, false);
        QCOMPARE(controller.queuedFileUrls(), QVariantList{firstQueued});

        controller.transcribe(secondQueued, vault.vaultUrl, kModelId, false);
        // secondQueued jumped ahead of firstQueued — the Active job is
        // untouched (still a.wav, still "active"), but among Queued entries
        // the most-recently-requested one is now first in line.
        QCOMPARE(statusOf(controller, activeFile), QStringLiteral("active"));
        QCOMPARE(controller.queuedFileUrls(), (QVariantList{secondQueued, firstQueued}));
    }

    // Item 1: a Queued job persists to <vault>/.transcriber/tasks.json and
    // loadPersistedQueue() re-enqueues it on a FRESH controller — same
    // contract a new app launch relies on.
    void persistedQueue_survivesAcrossControllerInstances()
    {
        VaultFixture vault;
        const QUrl queuedFile = vault.audioUrl("b.wav");
        {
            TranscriptionController controller;
            // Persistence only starts once a vault has been "opened" (see
            // TranscriptionController::persistQueue()'s doc comment — it's
            // a no-op until loadPersistedQueue() has recorded a vault to
            // save back to). Real app usage always calls this on vault
            // open (Main.qml's onVaultPathChanged) before anything gets
            // queued; mirror that here rather than calling transcribe()
            // "cold".
            controller.loadPersistedQueue(vault.vaultUrl); // no tasks.json yet — just records the vault
            controller.transcribe(vault.audioUrl("a.wav"), vault.vaultUrl, kModelId, false);
            controller.transcribe(queuedFile, vault.vaultUrl, kModelId, false);
            QVERIFY(QFileInfo::exists(vault.dir.path() + "/.transcriber/tasks.json"));
        }

        TranscriptionController fresh;
        fresh.loadPersistedQueue(vault.vaultUrl);
        // The first controller's Active job (a.wav) was persisted AS Queued
        // (no true mid-file resume — see loadPersistedQueue()'s doc
        // comment), so both files should be back in the queue.
        QTRY_COMPARE(fresh.tasksTotal(), 2);
        QVERIFY(fresh.isQueued(queuedFile));
        // User correction (2026-07-09): restored jobs are saved, not
        // auto-resumed — they sit Queued until startNext() is called.
        QVERIFY(!fresh.isBusy());
        QCOMPARE(fresh.jobs().at(0).toMap().value("status").toString(), QStringLiteral("queued"));
    }

    // User correction (2026-07-09): loadPersistedQueue() restores the queue
    // but does NOT start anything — the QML "play" button calls startNext()
    // explicitly once the user chooses to resume.
    void loadPersistedQueue_restoresWithoutAutoStarting_startNextResumesIt()
    {
        VaultFixture vault;
        const QUrl file = vault.audioUrl("a.wav");
        {
            TranscriptionController controller;
            controller.loadPersistedQueue(vault.vaultUrl);
            controller.transcribe(file, vault.vaultUrl, kModelId, false);
            QVERIFY(controller.isBusy()); // a live transcribe() call still auto-starts as before
        }

        TranscriptionController fresh;
        fresh.loadPersistedQueue(vault.vaultUrl);
        QCOMPARE(fresh.tasksTotal(), 1);
        QVERIFY(!fresh.isBusy());
        QCOMPARE(fresh.jobs().at(0).toMap().value("status").toString(), QStringLiteral("queued"));

        fresh.startNext();
        QVERIFY(fresh.isBusy());
        QCOMPARE(fresh.jobs().at(0).toMap().value("status").toString(), QStringLiteral("active"));

        // A second startNext() while already busy is a no-op (still just
        // the one job, still Active — not restarted/duplicated).
        fresh.startNext();
        QCOMPARE(fresh.tasksTotal(), 1);
        QTRY_COMPARE(fresh.tasksCompleted(), 1); // let the fake model's fast failure settle before teardown
    }

    // Regression (2026-07-09 bug report): a transcribe(autoLabel=true) call
    // persists BOTH a "transcribe" and a "relabel" entry for the SAME
    // fileUrl (see autoLabel_queuesBothTasksImmediately). Replaying them on
    // restart previously called relabel() directly, whose own isQueued()
    // dedup guard saw the just-replayed transcribe entry already Queued/
    // Active for that fileUrl and silently dropped the relabel entry — the
    // label job never came back after a restart. enqueueReplayedJob() now
    // bypasses that guard for replay (every persisted entry already passed
    // dedup once, when it was first queued live).
    void persistedQueue_autoLabelPairForSameFile_bothSurviveRestart()
    {
        VaultFixture vault;
        // Fake (empty) label model already "downloaded" — same idea as
        // VaultFixture's whisper model — so replaying the "relabel" entry
        // doesn't ALSO queue a download job, keeping this test focused on
        // the dedup-during-replay bug specifically.
        QFile labelModel(vault.dir.path() + "/.transcriber/models/Qwen3-4B-Instruct-2507-Q4_K_M.gguf");
        QVERIFY(labelModel.open(QIODevice::WriteOnly));
        labelModel.close();

        const QUrl file = vault.audioUrl("a.wav");
        {
            TranscriptionController controller;
            controller.loadPersistedQueue(vault.vaultUrl);
            controller.transcribe(file, vault.vaultUrl, kModelId, false, /*autoLabel=*/true);
            QCOMPARE(controller.tasksTotal(), 2);
        }

        TranscriptionController fresh;
        fresh.loadPersistedQueue(vault.vaultUrl);
        QTRY_COMPARE(fresh.tasksTotal(), 2);
        QCOMPARE(fresh.jobs().at(0).toMap().value("kind").toString(), QStringLiteral("transcribe"));
        QCOMPARE(fresh.jobs().at(1).toMap().value("kind").toString(), QStringLiteral("relabel"));
    }

    // Bug report (2026-07-10): "my first run seems to have the whole bar
    // filled at all times". Root cause: phaseRelativeSyntheticProgress()
    // divides elapsed-time-in-phase by defaultPhaseDurationMs() for
    // "loading"/"decoding" (phases with no real progress callback) whenever
    // a phase has recorded zero samples yet — the ORIGINAL fallback
    // (kDefaultPhaseWeightMs = 1.0, meant only as a WEIGHT for
    // recomputeOverallProgress()'s normalization, never as a plausible
    // DURATION) meant that division hit its 0.95 clamp within about a
    // millisecond of a phase starting, on literally every fresh install's
    // very first job.
    //
    // This asserts against defaultPhaseDurationMs() directly rather than
    // trying to reproduce the SYMPTOM end-to-end with a real job + wall-clock
    // waits: an earlier version of this test did exactly that and turned out
    // unreliable on fast hardware (a real model's "loading" phase can
    // complete in well under m_indeterminatePhaseTimer's 200ms tick interval
    // — e.g. a small model already warm in the OS page cache on a fast GPU —
    // so the elapsed-time-based bug never even got a chance to manifest
    // before the phase moved on, regardless of whether it was fixed). A
    // direct, deterministic check of the fallback value itself has no such
    // race.
    void defaultPhaseDurationMs_isAPlausibleDuration_notAnArbitraryWeight()
    {
        for (const QString &phase : TranscriptionController::kTimedPhases) {
            const double ms = TranscriptionController::defaultPhaseDurationMs(phase);
            QVERIFY2(ms >= 500.0,
                     qPrintable(QStringLiteral("defaultPhaseDurationMs(\"%1\") == %2 — too small to be a "
                                                "plausible duration; phaseRelativeSyntheticProgress() divides "
                                                "elapsed-ms by this, so anything this small saturates to ~full "
                                                "almost instantly")
                                    .arg(phase)
                                    .arg(ms)));
        }
        // The specific default this bug report asked for: "detecting-
        // speakers" and "transcribing" split 50/50 (equal default weight)
        // absent any real history, matching the fixed split this rolling-
        // average feature replaced.
        QCOMPARE(TranscriptionController::defaultPhaseDurationMs(QStringLiteral("detecting-speakers")),
                  TranscriptionController::defaultPhaseDurationMs(QStringLiteral("transcribing")));
    }

    // 2026-07-10: rolling-average phase weighting (RollingAverage,
    // TranscriptionController::recomputeOverallProgress()). Two things this
    // feature promises: (1) overallProgress sweeps the WHOLE job, ending at
    // (or extremely near) 1.0 once every active phase — including "loading"/
    // "decoding", which have no real progress callback of their own and now
    // rely on phaseRelativeSyntheticProgress() — has been accounted for; (2)
    // each phase's actual duration gets persisted (QSettings "phaseTimings"
    // group) so the NEXT job's bar proportions reflect real timing data
    // instead of the fixed 50/50 default fallback every phase starts with.
    void rollingAveragePhaseWeighting_advancesOverallProgressToCompletion_andPersistsDurations()
    {
        const QString modelPath = locateRealModel();
        if (modelPath.isEmpty())
            QSKIP("no real ggml model available; set SUOP_TEST_MODEL or transcribe once in the app");
        if (QStandardPaths::findExecutable(QStringLiteral("ffmpeg")).isEmpty())
            QSKIP("ffmpeg not on PATH — needed to decode the sample");

        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        const QString modelsDir = vault.path() + "/.transcriber/models";
        QVERIFY(QDir().mkpath(modelsDir));
        QVERIFY(QFile::copy(modelPath, modelsDir + "/ggml-base.en.bin"));
        const QString samplePath = vault.path() + "/jfk.wav";
        QVERIFY(QFile::copy(QStringLiteral(SUOP_SAMPLES_DIR "/jfk.wav"), samplePath));

        // Isolated org/app identity (on top of initTestCase()'s
        // setTestModeEnabled()) so this test's assertions on QSettings' exact
        // contents can't collide with any OTHER test in this same run that
        // also happens to record phase timings.
        QCoreApplication::setOrganizationName(QStringLiteral("suopTranscriber-tests"));
        QCoreApplication::setApplicationName(
            QStringLiteral("tst_transcription_queue-rollingAveragePhaseWeighting"));

        TranscriptionController controller;
        QSignalSpy finishedSpy(&controller, &TranscriptionController::finished);
        controller.transcribe(QUrl::fromLocalFile(samplePath), QUrl::fromLocalFile(vault.path()), kModelId, false);

        QVERIFY(finishedSpy.wait(30000));
        QVERIFY2(finishedSpy.at(0).at(1).toBool(), qPrintable(finishedSpy.at(0).at(2).toString()));

        // The last progress() tick before finished() should have swept
        // overallProgress all the way to (essentially) 1.0 — not stalled
        // partway because "loading"/"decoding" never got counted, and not
        // overshot past 1.0 either.
        QVERIFY2(controller.overallProgress() > 0.98,
                 qPrintable(QStringLiteral("expected overallProgress near 1.0 at completion, got %1")
                                .arg(controller.overallProgress())));

        // speakerDetection was false, so only these two phases ran — both
        // must have a persisted sample now. Item P3: keys are now composite
        // (phase|modelId[|diar]), not just the bare phase name — no "|diar"
        // suffix here since speakerDetection was false for this job.
        QSettings settings;
        settings.beginGroup(QStringLiteral("phaseTimings"));
        QVERIFY2(!settings.value(QStringLiteral("loading|%1").arg(kModelId)).toList().isEmpty(),
                 "expected a persisted duration sample for the \"loading\" phase");
        QVERIFY2(!settings.value(QStringLiteral("transcribing|%1").arg(kModelId)).toList().isEmpty(),
                 "expected a persisted duration sample for the \"transcribing\" phase");
        // Never requested this job, so no sample should exist for it.
        QVERIFY(settings.value(QStringLiteral("detecting-speakers|%1").arg(kModelId)).toList().isEmpty());
        QVERIFY(settings.value(QStringLiteral("detecting-speakers|%1|diar").arg(kModelId)).toList().isEmpty());
        settings.endGroup();
    }
};

QTEST_GUILESS_MAIN(TstTranscriptionQueue)
#include "tst_transcription_queue.moc"
