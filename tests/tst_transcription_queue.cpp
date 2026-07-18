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
#include <QDirIterator>
#include <QElapsedTimer>
#include <QJsonObject>
#include <QProcess>
#include <QRegularExpression>
#include <QSettings>
#include <QStandardPaths>
#include <QtTest>

#include "DiarizationEngine.h" // DiarizedTurn for the spliceBodySpeakers test (item 52)
#include "LabelingEngine.h" // kMaxChapterInputChars for the item-28 window test
#include "TranscriptionController.h"
// Persistence round-trip guard (everyPersistableKind_roundTripsThroughFactory):
// build a representative of each persistable kind and rebuild it via the factory.
#include "tasks/ChaptersTask.h"
#include "tasks/CombineTask.h"
#include "tasks/DiarizeTask.h"
#include "tasks/DownloadTask.h"
#include "tasks/MergeSpeakersTask.h"
#include "tasks/ModelDownloadTarget.h"
#include "tasks/ModelIoTask.h"
#include "tasks/RelabelTask.h"
#include "tasks/RenameSpeakerTask.h"
#include "tasks/TaskFactory.h"
#include "tasks/TranscribeTask.h"

namespace {

// Writes a minimal transcribed sibling .md (frontmatter + `body`) for
// audioName inside dirPath, and returns the AUDIO file's QUrl (the key every
// chapter/prelude method takes). Never touches the real vault — dirPath is a
// per-test QTemporaryDir.
QUrl writeTranscribedFile(const QString &dirPath, const QString &audioName, const QString &body)
{
    QFile md(dirPath + QStringLiteral("/") + audioName + QStringLiteral(".md"));
    const bool opened = md.open(QIODevice::WriteOnly | QIODevice::Text);
    Q_ASSERT(opened);
    md.write((QStringLiteral("---\nmodel: ggml-base.en.bin\nspeakerDetection: false\n---\n") + body).toUtf8());
    md.close();
    return QUrl::fromLocalFile(dirPath + QStringLiteral("/") + audioName);
}

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
        // Models moved to an app-global store (ModelManager) — point it at this
        // fixture's per-vault models dir so the planted fakes are still found
        // offline without touching the real store.
        qputenv("SUOP_MODELS_DIR", modelsDir.toLocal8Bit());
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

    // Fresh empty store/bundled dirs per test so a test resolves models ONLY
    // through what it plants (VaultFixture / manual sites override
    // SUOP_MODELS_DIR). Without this, a "model missing" test would fall through
    // to the process-wide test-mode default store, which a real-model test can
    // leave populated — leaking a "present" model in.
    QScopedPointer<QTemporaryDir> m_emptyStore;
    QScopedPointer<QTemporaryDir> m_emptyBundled;

private slots:
    // The model benchmark (BenchmarkTask) is display-only: it must NEVER write
    // a transcript .md or otherwise mutate the vault. Wave N: the old
    // benchmarkJob worker slot is now BenchmarkTask::run() in its own file, so
    // this source-scan guard (same technique as tst_styleguide's) points there
    // — a future edit that reaches for sidecarPath()/writeFrontmatter()/a
    // SegmentLineWriter inside it trips.
    void benchmarkJob_writesNoTranscriptFiles()
    {
        QFile src(QStringLiteral(SUOP_SRC_DIR "/core/tasks/BenchmarkTask.cpp"));
        QVERIFY2(src.open(QIODevice::ReadOnly), "BenchmarkTask.cpp must be readable");
        const QString all = QString::fromUtf8(src.readAll());
        QVERIFY2(!all.contains(QStringLiteral("sidecarPath(")),
                 "BenchmarkTask must not resolve a transcript sidecar path (it writes no files)");
        QVERIFY2(!all.contains(QStringLiteral("writeFrontmatter(")),
                 "BenchmarkTask must not write frontmatter (it writes no files)");
        QVERIFY2(!all.contains(QStringLiteral("SegmentLineWriter")),
                 "BenchmarkTask must not open a SegmentLineWriter (it writes no files)");
    }

    // TranscriptionController persists rolling-average phase timing via a
    // plain QSettings() (see RollingAverage.h / loadPhaseTimings()) — CLAUDE.md
    // "Never let testing touch the user's real vault or app config" applies
    // here just as much as to lastVaultPath: setTestModeEnabled() redirects every
    // QSettings/QStandardPaths lookup for the rest of this process into an
    // isolated "qttest" sandbox instead of the real ~/.config/suopTranscriber.
    void initTestCase() { QStandardPaths::setTestModeEnabled(true); }

    // Each test starts pointed at fresh EMPTY store + bundled dirs, so a model
    // is "missing" unless the test itself plants it (VaultFixture / a manual
    // site then overrides SUOP_MODELS_DIR at its own temp models dir). Pointing
    // at empty dirs — rather than unsetting — is what keeps a "model missing"
    // test from resolving to the shared test-mode default store, which another
    // test can leave holding a real model.
    void init()
    {
        m_emptyStore.reset(new QTemporaryDir);
        m_emptyBundled.reset(new QTemporaryDir);
        qputenv("SUOP_MODELS_DIR", m_emptyStore->path().toLocal8Bit());
        qputenv("SUOP_BUNDLED_MODELS_DIR", m_emptyBundled->path().toLocal8Bit());
    }

    // Item 11: sortedJobs partitions the raw (append-order) jobs() list for
    // display — Active first, then Queued in run order, then Paused, then
    // finished (done/failed) most-recently-appended first. Pure static
    // helper, tested directly with synthetic entries (no real jobs needed).
    void sortJobsForDisplay_partitionsActiveQueuedPausedFinished()
    {
        auto entry = [](int id, const char *status) {
            QVariantMap m;
            m[QStringLiteral("id")] = id;
            m[QStringLiteral("status")] = QString::fromLatin1(status);
            return QVariant(m);
        };
        // Deliberately interleaved / out of display order in the raw list.
        QVariantList raw;
        raw << entry(1, "done")     // finished (oldest)
            << entry(2, "queued")   // run order 1
            << entry(3, "active")   // the running one
            << entry(4, "failed")   // finished (newer)
            << entry(5, "queued")   // run order 2
            << entry(6, "paused");

        const QVariantList out = TranscriptionController::sortJobsForDisplay(raw);
        QCOMPARE(out.size(), 6);
        auto idAt = [&](int i) { return out.at(i).toMap().value(QStringLiteral("id")).toInt(); };
        QCOMPARE(idAt(0), 3);            // Active first
        QCOMPARE(idAt(1), 2);            // Queued, run (append) order
        QCOMPARE(idAt(2), 5);
        QCOMPARE(idAt(3), 6);            // Paused after queued
        QCOMPARE(idAt(4), 4);            // finished, most-recently-appended first
        QCOMPARE(idAt(5), 1);
    }

    // Item 25: backendKind maps CUDA/Vulkan/any-non-CPU -> "gpu", CPU -> "cpu"
    // (case-insensitive), empty -> empty (so QML keeps its indicator hidden
    // until a model loads).
    void backendKind_mapsGpuCpuEmpty()
    {
        QCOMPARE(TranscriptionController::backendKind(QStringLiteral("CUDA")), QStringLiteral("gpu"));
        QCOMPARE(TranscriptionController::backendKind(QStringLiteral("Vulkan")), QStringLiteral("gpu"));
        QCOMPARE(TranscriptionController::backendKind(QStringLiteral("CPU")), QStringLiteral("cpu"));
        QCOMPARE(TranscriptionController::backendKind(QStringLiteral("cpu")), QStringLiteral("cpu"));
        QCOMPARE(TranscriptionController::backendKind(QString()), QString());
    }

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

        controller.transcribe(missing, vault.vaultUrl, kModelId);

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

        controller.transcribe(file, vault.vaultUrl, kModelId);
        // First call started synchronously (Active); the duplicate must bounce.
        QVERIFY(controller.isQueued(file));
        controller.transcribe(file, vault.vaultUrl, kModelId);
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

        controller.transcribe(file, vault.vaultUrl, kModelId);
        QTRY_COMPARE(statusOf(controller, file), QStringLiteral("failed"));

        controller.transcribe(file, vault.vaultUrl, kModelId);
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

        controller.transcribe(fileA, vault.vaultUrl, kModelId);
        controller.transcribe(fileB, vault.vaultUrl, kModelId);

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

        controller.transcribe(fileA, vault.vaultUrl, kModelId);
        controller.transcribe(fileB, vault.vaultUrl, kModelId);

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

        controller.transcribe(fileA, vault.vaultUrl, kModelId);
        controller.transcribe(fileB, vault.vaultUrl, kModelId);

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

        controller.transcribeAll({fileA, fileA, fileB}, vault.vaultUrl, kModelId);
        QCOMPARE(controller.tasksTotal(), 2); // duplicate A collapsed

        QTRY_COMPARE(controller.tasksCompleted(), 2);
        QCOMPARE(statusOf(controller, fileA), QStringLiteral("failed"));
        QCOMPARE(statusOf(controller, fileB), QStringLiteral("failed"));
    }

    // Item 37: drag-and-drop import runs its byte-copy on the SAME worker
    // thread as every other job (CLAUDE.md "never on the GUI thread"), tagged
    // as a distinct "import" kind. Enqueuing an import job copies the source
    // into the vault and the job ends Done. Uses a real source file + a temp
    // dest vault (never the user's real vault).
    void importAudio_copiesFileIntoVault_andJobEndsDone()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        QTemporaryDir source;
        QVERIFY(source.isValid());
        const QString sourcePath = source.filePath("dropped.mp3");
        {
            QFile f(sourcePath);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write(QByteArray(64 * 1024, 'a')); // >1 chunk exercises the copy loop
            f.close();
        }
        const QUrl sourceUrl = QUrl::fromLocalFile(sourcePath);
        const QString destPath = vault.path() + QStringLiteral("/dropped.mp3");
        const QUrl destUrl = QUrl::fromLocalFile(destPath);
        const QUrl vaultUrl = QUrl::fromLocalFile(vault.path());

        TranscriptionController controller;
        QSignalSpy finishedSpy(&controller, &TranscriptionController::finished);
        controller.importAudio(sourceUrl, destUrl, vaultUrl);

        // The job is an "import" kind, keyed on the destination.
        QCOMPARE(controller.jobs().at(0).toMap().value("kind").toString(), QStringLiteral("import"));

        QTRY_COMPARE(finishedSpy.count(), 1);
        const QList<QVariant> args = finishedSpy.takeFirst();
        QCOMPARE(args.at(0).toUrl(), destUrl);
        QVERIFY2(args.at(1).toBool(), qPrintable(args.at(2).toString()));

        QCOMPARE(statusOf(controller, destUrl), QStringLiteral("done"));
        QVERIFY(QFileInfo::exists(destPath));
        QVERIFY(QFileInfo::exists(sourcePath)); // a copy, not a move
        QFile copied(destPath);
        QVERIFY(copied.open(QIODevice::ReadOnly));
        QCOMPARE(copied.readAll().size(), 64 * 1024); // bytes copied intact
    }

    // Item 37: a copy that can't proceed on the worker (here: the source file
    // doesn't exist) surfaces as a Failed job with an error, exactly like any
    // other job's failure — not a crash or a silent drop. (Wrong-type /
    // collision / no-vault are rejected SYNCHRONOUSLY before enqueue, by
    // VaultTreeModel::importDestinationFor — see tst_vaulttreemodel.)
    void importAudio_missingSource_endsFailedWithError()
    {
        QTemporaryDir vault;
        QVERIFY(vault.isValid());
        const QUrl sourceUrl = QUrl::fromLocalFile(vault.path() + QStringLiteral("/nope.mp3"));
        const QUrl destUrl = QUrl::fromLocalFile(vault.path() + QStringLiteral("/nope.mp3.dest.mp3"));
        const QUrl vaultUrl = QUrl::fromLocalFile(vault.path());

        TranscriptionController controller;
        QSignalSpy finishedSpy(&controller, &TranscriptionController::finished);
        controller.importAudio(sourceUrl, destUrl, vaultUrl);

        QTRY_COMPARE(finishedSpy.count(), 1);
        QCOMPARE(finishedSpy.first().at(1).toBool(), false);
        QVERIFY(!finishedSpy.first().at(2).toString().isEmpty());
        QCOMPARE(statusOf(controller, destUrl), QStringLiteral("failed"));
        QVERIFY(!QFileInfo::exists(destUrl.toLocalFile())); // no partial file left behind
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
        controller.transcribe(file, vault.vaultUrl, kModelId);

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

    // Undo primitive (Task::undoLabel/makeUndoTask): a file rename is
    // undoable, and undo is just the inverse task. Renaming a.wav -> b, then
    // undoTask() on it, must put a.wav back — the whole "undo = enqueue the
    // inverse" design, end to end through the instant lane.
    void renameFile_isUndoable_undoTaskRestoresOriginalName()
    {
        VaultFixture vault;
        QFile audio(vault.dir.path() + "/a.wav");
        QVERIFY(audio.open(QIODevice::WriteOnly));
        audio.write("audio");
        audio.close();
        const QUrl audioUrl = QUrl::fromLocalFile(vault.dir.path() + "/a.wav");

        TranscriptionController controller;
        QSignalSpy renamedSpy(&controller, &TranscriptionController::fileRenamed);
        QSignalSpy undoSpy(&controller, &TranscriptionController::undoAvailable);

        controller.renameFile(audioUrl, QStringLiteral("b"));
        QVERIFY(renamedSpy.wait(5000));
        QVERIFY(QFileInfo::exists(vault.dir.path() + "/b.wav"));
        QVERIFY(!QFileInfo::exists(vault.dir.path() + "/a.wav"));

        // The completed rename advertised an undo.
        QTRY_COMPARE(undoSpy.count(), 1);
        const int undoableId = undoSpy.constFirst().at(0).toInt();
        QVERIFY(!undoSpy.constFirst().at(1).toString().isEmpty()); // has a label

        // Undo it — the inverse rename runs and a.wav comes back.
        renamedSpy.clear();
        controller.undoTask(undoableId);
        QVERIFY(renamedSpy.wait(5000));
        QTRY_VERIFY(QFileInfo::exists(vault.dir.path() + "/a.wav"));
        QVERIFY(!QFileInfo::exists(vault.dir.path() + "/b.wav"));
    }

    // Batch undo (moveFiles/enqueueBatch): moving N files at once must produce
    // exactly ONE undo snackbar ("Moved N files"), and undoTask() on that batch
    // token must move ALL of them back — the whole multi-select collapses into
    // a single reversible action, not N separate ones.
    void moveFiles_batchUndo_oneSnackbarRestoresAllFiles()
    {
        VaultFixture vault;
        QVERIFY(QDir(vault.dir.path()).mkdir("sub"));
        for (const char *name : {"a.wav", "b.wav"}) {
            QFile f(vault.dir.path() + "/" + name);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write("audio");
            f.close();
        }
        const QUrl destDir = QUrl::fromLocalFile(vault.dir.path() + "/sub");

        TranscriptionController controller;
        QSignalSpy movedSpy(&controller, &TranscriptionController::fileMoved);
        QSignalSpy undoSpy(&controller, &TranscriptionController::undoAvailable);

        controller.moveFiles({vault.audioUrl("a.wav"), vault.audioUrl("b.wav")}, destDir, vault.vaultUrl);

        // Both files move...
        QTRY_COMPARE(movedSpy.count(), 2);
        QVERIFY(QFileInfo::exists(vault.dir.path() + "/sub/a.wav"));
        QVERIFY(QFileInfo::exists(vault.dir.path() + "/sub/b.wav"));

        // ...but the whole batch offers exactly ONE undo, summarizing the count.
        QTRY_COMPARE(undoSpy.count(), 1);
        const int batchToken = undoSpy.constFirst().at(0).toInt();
        QCOMPARE(undoSpy.constFirst().at(1).toString(), QStringLiteral("Moved 2 files"));

        // Undoing the batch moves BOTH back in one action.
        movedSpy.clear();
        controller.undoTask(batchToken);
        QTRY_COMPARE(movedSpy.count(), 2);
        QTRY_VERIFY(QFileInfo::exists(vault.dir.path() + "/a.wav"));
        QVERIFY(QFileInfo::exists(vault.dir.path() + "/b.wav"));
        QVERIFY(!QFileInfo::exists(vault.dir.path() + "/sub/a.wav"));
        QVERIFY(!QFileInfo::exists(vault.dir.path() + "/sub/b.wav"));
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
        controller.transcribe(transcribeFile, vault.vaultUrl, kModelId);
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

    // Item 23 (behavior change): a transcribe call now ONLY transcribes. It
    // used to chain a SEPARATE "relabel" job alongside the transcribe job
    // whenever autoLabel was true (silent auto-labeling on every "Transcribe"
    // click) — that auto-chaining is removed. The autoLabel parameter is kept
    // (QML call sites still pass AppSettings.autoLabelEnabled and the invokable
    // signature must stay call-compatible) but is now a NO-OP: transcribe with
    // autoLabel=true queues exactly ONE transcribe job, no relabel job. The
    // dedicated "Regenerate label" action is the sole way to label.
    void transcribe_autoLabelParamIsNoOp_onlyTranscribes()
    {
        VaultFixture vault;
        // A fake (empty) label model on disk — so that IF the old auto-chaining
        // were still present it would queue a relabel job here (no download
        // job masking it), making this test a real regression guard.
        QFile labelModel(vault.dir.path() + "/.transcriber/models/Qwen3-4B-Instruct-2507-Q4_K_M.gguf");
        QVERIFY(labelModel.open(QIODevice::WriteOnly));
        labelModel.close();

        TranscriptionController controller;
        const QUrl file = vault.audioUrl("a.wav");

        controller.transcribe(file, vault.vaultUrl, kModelId, /*autoLabel=*/true);

        // Only the transcribe job — no chained relabel job.
        QCOMPARE(controller.tasksTotal(), 1);
        QCOMPARE(controller.jobs().at(0).toMap().value("kind").toString(), QStringLiteral("transcribe"));
        QCOMPARE(controller.jobs().at(0).toMap().value("status").toString(), QStringLiteral("active"));

        // No "relabel" kind ever appears (before or after the job completes).
        QTRY_COMPARE(controller.tasksCompleted(), 1);
        QCOMPARE(controller.tasksTotal(), 1);
        for (const QVariant &v : controller.jobs())
            QVERIFY(v.toMap().value("kind").toString() != QStringLiteral("relabel"));
    }

    // Items 24/53 ("run all jobs"): a single runAllJobs() on an UNtranscribed
    // file must enqueue exactly four jobs, IN ORDER — transcribe, then detect
    // speakers (diarize), then label (relabel), then chapters — so the single
    // FIFO worker runs the full pipeline sequentially. The diarize step is what
    // replaced transcribe()'s old inline speakerDetection toggle (Transcribe is
    // pure ASR now). All models are planted on disk (whisper via VaultFixture,
    // label + diarization here) so no "download" job masks the ordering; the
    // jobs' kinds/positions are stable in jobs() regardless of the worker's
    // async progress (finished entries keep their kind + list position).
    void runAllJobs_enqueuesTranscribeThenDiarizeThenLabelThenChapters_inOrder()
    {
        VaultFixture vault;
        // Fake (empty) label + diarization models on disk so the label/chapters
        // and diarize steps don't queue real network download jobs ahead of
        // them (same idiom as jobs_tagEachEntryWithItsKind above).
        QFile labelModel(vault.dir.path() + "/.transcriber/models/Qwen3-4B-Instruct-2507-Q4_K_M.gguf");
        QVERIFY(labelModel.open(QIODevice::WriteOnly));
        labelModel.close();
        QFile segModel(vault.dir.path() + "/.transcriber/models/pyannote-segmentation-3.0-int8.onnx");
        QVERIFY(segModel.open(QIODevice::WriteOnly));
        segModel.close();
        QFile embModel(vault.dir.path() + "/.transcriber/models/wespeaker-resnet293-lm.onnx");
        QVERIFY(embModel.open(QIODevice::WriteOnly));
        embModel.close();

        TranscriptionController controller;
        const QUrl file = vault.audioUrl("a.wav"); // no sibling .md -> untranscribed

        controller.runAllJobs(file, vault.vaultUrl, kModelId);

        QCOMPARE(controller.tasksTotal(), 4);
        QCOMPARE(controller.jobs().at(0).toMap().value("kind").toString(), QStringLiteral("transcribe"));
        QCOMPARE(controller.jobs().at(1).toMap().value("kind").toString(), QStringLiteral("diarize"));
        QCOMPARE(controller.jobs().at(2).toMap().value("kind").toString(), QStringLiteral("relabel"));
        QCOMPARE(controller.jobs().at(3).toMap().value("kind").toString(), QStringLiteral("chapters"));
        // All four name the same file, in the same queue.
        for (int i = 0; i < 4; ++i)
            QCOMPARE(controller.jobs().at(i).toMap().value("fileUrl").toUrl(), file);
        // The transcribe job started synchronously (idle when enqueued); the
        // diarize + label + chapters jobs wait their turn behind it.
        QCOMPARE(controller.jobs().at(0).toMap().value("status").toString(), QStringLiteral("active"));
        QCOMPARE(controller.jobs().at(1).toMap().value("status").toString(), QStringLiteral("queued"));
        QCOMPARE(controller.jobs().at(2).toMap().value("status").toString(), QStringLiteral("queued"));
        QCOMPARE(controller.jobs().at(3).toMap().value("status").toString(), QStringLiteral("queued"));

        // Whole pipeline drains (each fails fast on the zero-byte models, but
        // the ordering/kinds above are stable throughout — that's the point).
        QTRY_COMPARE(controller.tasksCompleted(), 4);
    }

    // Items 24/53 already-transcribed decision: runAllJobs() on a file that
    // already has a sibling .md SKIPS the transcribe step and enqueues only
    // detect speakers -> label -> chapters (the friendly "just re-detect
    // speakers + refresh the derived passes" behavior documented on the method).
    void runAllJobs_alreadyTranscribed_skipsTranscribe_diarizeLabelThenChapters()
    {
        VaultFixture vault;
        QFile labelModel(vault.dir.path() + "/.transcriber/models/Qwen3-4B-Instruct-2507-Q4_K_M.gguf");
        QVERIFY(labelModel.open(QIODevice::WriteOnly));
        labelModel.close();
        QFile segModel(vault.dir.path() + "/.transcriber/models/pyannote-segmentation-3.0-int8.onnx");
        QVERIFY(segModel.open(QIODevice::WriteOnly));
        segModel.close();
        QFile embModel(vault.dir.path() + "/.transcriber/models/wespeaker-resnet293-lm.onnx");
        QVERIFY(embModel.open(QIODevice::WriteOnly));
        embModel.close();

        const QUrl file = vault.audioUrl("a.wav");
        QFile md(vault.dir.path() + "/a.wav.md"); // sibling .md == already transcribed
        QVERIFY(md.open(QIODevice::WriteOnly));
        md.write("---\nmodel: ggml-base.en.bin\n---\n\n[0] hello\n");
        md.close();

        TranscriptionController controller;
        controller.runAllJobs(file, vault.vaultUrl, kModelId);

        QCOMPARE(controller.tasksTotal(), 3);
        QCOMPARE(controller.jobs().at(0).toMap().value("kind").toString(), QStringLiteral("diarize"));
        QCOMPARE(controller.jobs().at(1).toMap().value("kind").toString(), QStringLiteral("relabel"));
        QCOMPARE(controller.jobs().at(2).toMap().value("kind").toString(), QStringLiteral("chapters"));

        QTRY_COMPARE(controller.tasksCompleted(), 3);
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
        controller.transcribe(file, vaultUrl, kModelId);

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

        controller.transcribe(activeFile, vault.vaultUrl, kModelId);
        controller.transcribe(queuedFile, vault.vaultUrl, kModelId);
        QCOMPARE(controller.tasksTotal(), 2);

        // Nothing has started for the Queued entry — cancelling it is a
        // plain removal, no interruption needed.
        const int queuedId = controller.jobs().at(1).toMap().value("id").toInt();
        controller.cancelJob(queuedId);
        QCOMPARE(controller.tasksTotal(), 1);
        QCOMPARE(controller.jobs().at(0).toMap().value("status").toString(), QStringLiteral("active"));

        QTRY_COMPARE(controller.tasksCompleted(), 1); // let the fake (fails-fast) model job settle before teardown
    }

    // Wave N (task system) invariant 1 — "exactly one terminal report per
    // enqueued task, whether or not run() was ever called." The OLD system
    // silently dropped a cancelled-while-Queued job (removeAt, no signal), so
    // a consumer waiting on finished() (e.g. UsbGrabberController's in-flight
    // tracking) never heard back and had to reconcile against the queue to
    // notice. Now the queue synthesizes the terminal report on a pre-run
    // cancel. See TranscriptionController::cancelTaskAt().
    void cancelQueuedTask_stillEmitsFinished_soNothingWaitsForever()
    {
        VaultFixture vault;
        TranscriptionController controller;
        const QUrl activeFile = vault.audioUrl("a.wav");
        const QUrl queuedFile = vault.audioUrl("b.wav");

        controller.transcribe(activeFile, vault.vaultUrl, kModelId);
        controller.transcribe(queuedFile, vault.vaultUrl, kModelId);

        QSignalSpy finishedSpy(&controller, &TranscriptionController::finished);
        const int queuedId = controller.jobs().at(1).toMap().value("id").toInt();
        controller.cancelJob(queuedId);

        // The cancelled-while-Queued task reports finished(queuedFile, false,
        // "Cancelled") synchronously — NOT dropped silently.
        bool sawQueuedReport = false;
        for (const QList<QVariant> &args : finishedSpy) {
            if (args.at(0).toUrl() == queuedFile) {
                QCOMPARE(args.at(1).toBool(), false);
                QCOMPARE(args.at(2).toString(), QStringLiteral("Cancelled"));
                sawQueuedReport = true;
            }
        }
        QVERIFY2(sawQueuedReport, "a cancelled-while-Queued task must still emit finished() exactly once");

        QTRY_COMPARE(controller.tasksCompleted(), 1); // let the active fake job settle before teardown
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
        controller.transcribe(file, vaultUrl, kModelId);
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
        controller.transcribe(vault.audioUrl("a.wav"), vault.vaultUrl, kModelId);
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
        qputenv("SUOP_MODELS_DIR", modelsDir.toLocal8Bit()); // app-global store -> this test's dir
        QVERIFY(QFile::copy(modelPath, modelsDir + "/ggml-base.en.bin"));
        const QString samplePath = vault.path() + "/jfk.wav";
        QVERIFY(QFile::copy(QStringLiteral(SUOP_SAMPLES_DIR "/jfk.wav"), samplePath));

        TranscriptionController controller;
        QSignalSpy finishedSpy(&controller, &TranscriptionController::finished);
        controller.transcribe(QUrl::fromLocalFile(samplePath), QUrl::fromLocalFile(vault.path()), kModelId);
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
    // then queue job #2 and try to pause IT during a real, pausable phase.
    //
    // Job #2 pauses during "decoding" (a real, SIGSTOP-able ffmpeg subprocess
    // — see pauseActive_duringDecoding_reallyPauses_notRestart), NOT during
    // "transcribing": jfk.wav is short enough that whisper can finish the
    // whole clip inside a single QTRY tick, so a pause requested "during
    // transcribing" intermittently landed after the job had already completed
    // — a flake, not the bug. The suppression this test guards against (a
    // leaked cancel flag from job #1 breaking pause on job #2) lives in the
    // shared worker pause/cancel state and would break pause in ANY phase, so
    // catching it during the deterministically-observable decode window proves
    // exactly the same thing. Uses a synthetic tone long enough that decode
    // takes real, catchable wall-clock time.
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
        qputenv("SUOP_MODELS_DIR", modelsDir.toLocal8Bit()); // app-global store -> this test's dir
        QVERIFY(QFile::copy(modelPath, modelsDir + "/ggml-base.en.bin"));
        const QString sampleA = vault.path() + "/a.wav";
        const QString sampleB = vault.path() + "/b.wav";
        QVERIFY(QFile::copy(QStringLiteral(SUOP_SAMPLES_DIR "/jfk.wav"), sampleA));
        // Long tone so job #2's decode takes an observable amount of time (see
        // pauseActive_duringDecoding_reallyPauses_notRestart for the same
        // technique); jfk.wav decodes in well under one polling tick.
        QProcess gen;
        gen.start("ffmpeg", {"-y", "-f", "lavfi", "-i", QStringLiteral("sine=frequency=440:duration=300"),
                              sampleB});
        if (!gen.waitForFinished(30000) || gen.exitCode() != 0 || !QFileInfo::exists(sampleB))
            QSKIP("could not generate synthetic test audio via ffmpeg");

        TranscriptionController controller;
        QSignalSpy finishedSpy(&controller, &TranscriptionController::finished);

        // Job #1: cancel it while still "loading" (mirrors the bottom bar's
        // pause button, which maps to cancelActive() during this phase).
        controller.transcribe(QUrl::fromLocalFile(sampleA), QUrl::fromLocalFile(vault.path()), kModelId);
        controller.cancelActive();
        QVERIFY(finishedSpy.wait(10000));
        QCOMPARE(finishedSpy.at(0).at(1).toBool(), false);
        QCOMPARE(finishedSpy.at(0).at(2).toString(), QStringLiteral("Cancelled"));

        // Job #2: a fresh job for a different file. Pause it during "decoding"
        // — if job #1's cancel had leaked into the shared worker state, this
        // pause would be silently suppressed (isActivePaused stays false) or
        // the job would cancel outright instead of finishing true.
        controller.transcribe(QUrl::fromLocalFile(sampleB), QUrl::fromLocalFile(vault.path()), kModelId);
        QTRY_VERIFY_WITH_TIMEOUT(controller.currentPhase() == QStringLiteral("decoding"), 10000);

        controller.pauseActive();
        QTRY_VERIFY_WITH_TIMEOUT(controller.isActivePaused(), 5000);
        // Job #2 (sampleB) is still Active — a real cooperative pause, not the
        // restart-based Paused status. Keyed off its fileUrl, NOT jobs().at(0):
        // index 0 is the cancelled job #1, whose status is legitimately
        // "failed".
        QCOMPARE(statusOf(controller, QUrl::fromLocalFile(sampleB)), QStringLiteral("active"));

        controller.resumeActive();
        QVERIFY(finishedSpy.wait(60000));
        QCOMPARE(finishedSpy.at(1).at(1).toBool(), true);
    }

    // Item 9 (2026-07-14, supersedes the earlier "loading pause = restart-as-
    // Paused" behavior): clicking pause during "loading" must NOT reinterpret
    // the tap as a cancel — that was the "pause broken again" report (a click
    // that looked like it did nothing, and could latch a stray cancel that
    // broke pause for the rest of the job). togglePauseActive() is now a safe
    // NO-OP during "loading" (the bottom bar disables the button there — see
    // BottomBar.qml's canPauseNow()): the job is neither cancelled nor flipped
    // to the old restart-based Paused status, it simply keeps running. Proven
    // deterministically with the zero-byte fake model, which stays in
    // "loading" until it fails to LOAD on its own — the failure error is that
    // load failure, never "Cancelled".
    void togglePauseActive_duringLoading_isNoop_notReinterpretedAsCancel()
    {
        VaultFixture vault;
        TranscriptionController controller;
        QSignalSpy finishedSpy(&controller, &TranscriptionController::finished);
        controller.transcribe(vault.audioUrl("a.wav"), vault.vaultUrl, kModelId);
        QTRY_COMPARE(controller.currentPhase(), QStringLiteral("loading"));

        controller.togglePauseActive(); // Item 9: safe no-op during "loading".
        QVERIFY(!controller.isActivePaused());
        QVERIFY(!controller.isActiveStopping());

        // The click never cancelled the job: it fails on its own because the
        // fake model can't load, with the load error (not "Cancelled"), and
        // is never presented as the removed restart-based Paused status.
        QTRY_COMPARE(controller.tasksCompleted(), 1);
        QVERIFY(!finishedSpy.isEmpty());
        QCOMPARE(finishedSpy.at(0).at(1).toBool(), false);
        QVERIFY(finishedSpy.at(0).at(2).toString() != QStringLiteral("Cancelled"));
        QVERIFY(!finishedSpy.at(0).at(2).toString().isEmpty());
        QCOMPARE(controller.jobs().at(0).toMap().value("status").toString(), QStringLiteral("failed"));
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
        qputenv("SUOP_MODELS_DIR", modelsDir.toLocal8Bit()); // app-global store -> this test's dir
        QVERIFY(QFile::copy(modelPath, modelsDir + "/ggml-base.en.bin"));

        const QString samplePath = vault.path() + "/a.wav";
        QProcess gen;
        gen.start("ffmpeg", {"-y", "-f", "lavfi", "-i", QStringLiteral("sine=frequency=440:duration=300"),
                              samplePath});
        if (!gen.waitForFinished(30000) || gen.exitCode() != 0 || !QFileInfo::exists(samplePath))
            QSKIP("could not generate synthetic test audio via ffmpeg");

        TranscriptionController controller;
        QSignalSpy finishedSpy(&controller, &TranscriptionController::finished);
        controller.transcribe(QUrl::fromLocalFile(samplePath), QUrl::fromLocalFile(vault.path()), kModelId);
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
    // Active job's id itself. Unlike togglePauseActive() (which is a plain
    // no-op during "loading" now — see
    // togglePauseActive_duringLoading_isNoop_notReinterpretedAsCancel), a
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
        controller.transcribe(file, vaultUrl, kModelId);
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
            controller.transcribe(file, vaultUrl, kModelId);
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

        controller.transcribe(activeFile, vault.vaultUrl, kModelId);
        controller.transcribe(queuedFile1, vault.vaultUrl, kModelId);
        controller.transcribe(queuedFile2, vault.vaultUrl, kModelId);

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

        controller.transcribe(activeFile, vault.vaultUrl, kModelId);
        QCOMPARE(statusOf(controller, activeFile), QStringLiteral("active"));

        controller.transcribe(firstQueued, vault.vaultUrl, kModelId);
        QCOMPARE(controller.queuedFileUrls(), QVariantList{firstQueued});

        controller.transcribe(secondQueued, vault.vaultUrl, kModelId);
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
            controller.transcribe(vault.audioUrl("a.wav"), vault.vaultUrl, kModelId);
            controller.transcribe(queuedFile, vault.vaultUrl, kModelId);
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
            controller.transcribe(file, vault.vaultUrl, kModelId);
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

    // Regression (2026-07-09 bug report): two persisted entries for the SAME
    // fileUrl (a "transcribe" and a "relabel") must BOTH survive a restart.
    // Replaying them previously called relabel() directly, whose own
    // isQueued() dedup guard saw the just-replayed transcribe entry already
    // Queued/Active for that fileUrl and silently dropped the relabel entry —
    // the label job never came back after a restart. enqueueReplayedJob() now
    // bypasses that guard for replay (every persisted entry already passed
    // dedup once, when it was first queued live).
    //
    // Item 23 note: transcribe() no longer auto-chains a relabel job, so this
    // same-file pair can no longer arise from a single transcribe(autoLabel)
    // call — the persisted tasks.json is written directly here (its exact
    // on-disk shape, see persistQueue()) to still exercise the replay-dedup
    // bypass, which remains live defensive code.
    void persistedQueue_sameFilePairBothSurviveRestart()
    {
        VaultFixture vault;
        // Fake (empty) label model already "downloaded" so replaying the
        // "relabel" entry doesn't ALSO queue a download job, keeping this test
        // focused on the dedup-during-replay bypass specifically.
        QFile labelModel(vault.dir.path() + "/.transcriber/models/Qwen3-4B-Instruct-2507-Q4_K_M.gguf");
        QVERIFY(labelModel.open(QIODevice::WriteOnly));
        labelModel.close();

        const QUrl file = vault.audioUrl("a.wav");
        // Hand-write a tasks.json with a transcribe + relabel pair for the
        // SAME fileUrl — the exact persisted shape persistQueue() emits.
        const QString tasksJson =
            QStringLiteral("[{\"kind\":\"transcribe\",\"fileUrl\":\"%1\",\"vaultUrl\":\"%2\",\"modelId\":\"%3\"},"
                           "{\"kind\":\"relabel\",\"fileUrl\":\"%1\",\"vaultUrl\":\"%2\"}]")
                .arg(file.toString(), vault.vaultUrl.toString(), QString::fromLatin1(kModelId));
        QFile tasks(vault.dir.path() + "/.transcriber/tasks.json");
        QVERIFY(tasks.open(QIODevice::WriteOnly | QIODevice::Text));
        tasks.write(tasksJson.toUtf8());
        tasks.close();

        TranscriptionController fresh;
        fresh.loadPersistedQueue(vault.vaultUrl);
        QTRY_COMPARE(fresh.tasksTotal(), 2);
        QCOMPARE(fresh.jobs().at(0).toMap().value("kind").toString(), QStringLiteral("transcribe"));
        QCOMPARE(fresh.jobs().at(1).toMap().value("kind").toString(), QStringLiteral("relabel"));
    }

    // Design-review #1 + #2: the per-kind progress shape and the
    // "completion changes the model set" side-effect are authored ON the Task
    // now (they used to be hardcoded phase-name lists / kind checks in the
    // controller, in two places each that drifted). This pins the authorship.
    void progressAndModelSetHooks_areAuthoredPerKind()
    {
        const QUrl file = QUrl::fromLocalFile(QStringLiteral("/vault/a.wav"));
        const QUrl vaultUrl = QUrl::fromLocalFile(QStringLiteral("/vault"));

        // WeightedPhases kinds own their ordered phase set — the SINGLE source
        // recomputeOverallProgress() now reads (no controller-side copy).
        TranscribeTask transcribe(file, vaultUrl, QStringLiteral("ggml-base.en.bin"));
        QCOMPARE(transcribe.progressModel(), Task::ProgressModel::WeightedPhases);
        QCOMPARE(transcribe.progressPhases(),
                 (QStringList{QStringLiteral("loading"), QStringLiteral("decoding"),
                              QStringLiteral("detecting-speech"), QStringLiteral("transcribing")}));
        QVERIFY(!transcribe.completionChangesModelSet());

        DiarizeTask diarize(file, vaultUrl, QStringLiteral("seg"), QStringLiteral("emb"), 0.5);
        QCOMPARE(diarize.progressModel(), Task::ProgressModel::WeightedPhases);
        QCOMPARE(diarize.progressPhases(),
                 (QStringList{QStringLiteral("decoding"), QStringLiteral("detecting-speakers")}));

        // SelfCumulative kinds emit an already-cumulative fraction — no phase
        // set, so the controller assigns it directly.
        ChaptersTask chapters(file, vaultUrl, 0.5);
        QCOMPARE(chapters.progressModel(), Task::ProgressModel::SelfCumulative);
        QVERIFY(chapters.progressPhases().isEmpty());

        // completionChangesModelSet(): the one hook the central modelsChanged()
        // emit keys off. Download + model-import say yes; export + everything
        // else says no.
        DownloadTask download(ModelDownloadTarget::WhisperModel, QStringLiteral("ggml-base.en.bin"));
        QCOMPARE(download.progressModel(), Task::ProgressModel::SelfCumulative);
        QVERIFY(download.completionChangesModelSet());

        ModelIoTask importTask(ModelIoTask::Direction::Import, QStringLiteral("m"), file);
        ModelIoTask exportTask(ModelIoTask::Direction::Export, QStringLiteral("m"), vaultUrl);
        QVERIFY(importTask.completionChangesModelSet());
        QVERIFY(!exportTask.completionChangesModelSet());
        QVERIFY(!chapters.completionChangesModelSet());
    }

    // Design-review #3: persistence is opt-IN per kind (Task::isPersistable),
    // but the REBUILD side is one central switch (createTaskFromJson). A
    // persistable kind that forgets its factory branch would persist fine and
    // then be SILENTLY DROPPED on restart — the exact "work goes missing"
    // class the task refactor exists to kill. This round-trips a representative
    // of every persistable kind through persistQueue()'s exact serialization +
    // the factory, and the completeness tripwire at the end fails if a NEW
    // persistable kind is ever added without being listed here (which is also
    // where it'd be checked against the factory).
    void everyPersistableKind_roundTripsThroughFactory()
    {
        const QUrl file = QUrl::fromLocalFile(QStringLiteral("/vault/a.wav"));
        const QUrl vaultUrl = QUrl::fromLocalFile(QStringLiteral("/vault"));

        std::vector<std::unique_ptr<Task>> reps;
        reps.push_back(std::make_unique<TranscribeTask>(file, vaultUrl, QStringLiteral("ggml-base.en.bin")));
        reps.push_back(std::make_unique<RelabelTask>(file, vaultUrl, /*restrictTags=*/false, /*reuseBias=*/0.5,
                                                     /*wordiness=*/0.5, /*skipIfLabeled=*/false));
        reps.push_back(std::make_unique<ChaptersTask>(file, vaultUrl, /*density=*/0.5));
        reps.push_back(std::make_unique<DiarizeTask>(file, vaultUrl, QStringLiteral("seg"), QStringLiteral("emb"),
                                                     /*threshold=*/0.5));
        reps.push_back(std::make_unique<RenameSpeakerTask>(file, QStringLiteral("A"), QStringLiteral("B")));
        reps.push_back(std::make_unique<MergeSpeakersTask>(file, QStringLiteral("A"), QStringLiteral("B")));
        reps.push_back(std::make_unique<CombineTask>(QStringList{QStringLiteral("/vault/a.wav"),
                                                                 QStringLiteral("/vault/b.wav")},
                                                     file, /*deleteOriginals=*/false));
        reps.push_back(
            std::make_unique<DownloadTask>(ModelDownloadTarget::WhisperModel, QStringLiteral("ggml-base.en.bin")));

        for (const auto &task : reps) {
            QVERIFY2(task->isPersistable(),
                     qPrintable(QStringLiteral("test rep '%1' is not persistable — wrong list").arg(task->kind())));
            // Mirror persistQueue()'s exact on-disk serialization.
            QJsonObject obj = task->toJson();
            obj[QStringLiteral("kind")] = task->kind();
            obj[QStringLiteral("fileUrl")] = task->fileUrl.toString();
            obj[QStringLiteral("vaultUrl")] = task->vaultUrl.toString();

            std::unique_ptr<Task> rebuilt = createTaskFromJson(task->kind(), obj);
            QVERIFY2(rebuilt != nullptr,
                     qPrintable(QStringLiteral("persistable kind '%1' has no createTaskFromJson branch — a "
                                               "persisted entry would be SILENTLY DROPPED on restart")
                                    .arg(task->kind())));
            QCOMPARE(rebuilt->kind(), task->kind());
        }

        // Completeness tripwire: the number of kinds that opt into persistence
        // (grepped from the task headers) must equal the number round-tripped
        // above. A NEW persistable kind trips this until it's added here AND
        // shown to survive the factory — closing the "forgot the branch" gap
        // for future kinds, not just today's eight.
        int persistableInSource = 0;
        QDirIterator it(QStringLiteral(SUOP_SRC_DIR "/core/tasks"), {QStringLiteral("*.h")}, QDir::Files);
        const QRegularExpression optIn(QStringLiteral("bool isPersistable\\(\\) const override \\{ return true"));
        while (it.hasNext()) {
            QFile header(it.next());
            if (header.open(QIODevice::ReadOnly | QIODevice::Text)
                && optIn.match(QString::fromUtf8(header.readAll())).hasMatch())
                ++persistableInSource;
        }
        QCOMPARE(static_cast<int>(reps.size()), persistableInSource);
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
        qputenv("SUOP_MODELS_DIR", modelsDir.toLocal8Bit()); // app-global store -> this test's dir
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
        controller.transcribe(QUrl::fromLocalFile(samplePath), QUrl::fromLocalFile(vault.path()), kModelId);

        QVERIFY(finishedSpy.wait(30000));
        QVERIFY2(finishedSpy.at(0).at(1).toBool(), qPrintable(finishedSpy.at(0).at(2).toString()));

        // The last progress() tick before finished() should have swept
        // overallProgress all the way to (essentially) 1.0 — not stalled
        // partway because "loading"/"decoding" never got counted, and not
        // overshot past 1.0 either.
        QVERIFY2(controller.overallProgress() > 0.98,
                 qPrintable(QStringLiteral("expected overallProgress near 1.0 at completion, got %1")
                                .arg(controller.overallProgress())));

        // A transcribe job is pure ASR + VAD, so loading, detecting-speech
        // (VAD windowing) and transcribing all ran and must have a persisted
        // sample now. Item P3: keys are composite (phase|modelId[|diar]), not
        // just the bare phase name — no "|diar" suffix here (that marker is
        // only set for the standalone diarize job).
        QSettings settings;
        settings.beginGroup(QStringLiteral("phaseTimings"));
        QVERIFY2(!settings.value(QStringLiteral("loading|%1").arg(kModelId)).toList().isEmpty(),
                 "expected a persisted duration sample for the \"loading\" phase");
        QVERIFY2(!settings.value(QStringLiteral("detecting-speech|%1").arg(kModelId)).toList().isEmpty(),
                 "expected a persisted duration sample for the \"detecting-speech\" (VAD) phase");
        QVERIFY2(!settings.value(QStringLiteral("transcribing|%1").arg(kModelId)).toList().isEmpty(),
                 "expected a persisted duration sample for the \"transcribing\" phase");
        // Speaker diarization is a separate job — a pure-ASR transcribe never
        // runs the detecting-speakers phase, so no sample should exist for it.
        QVERIFY(settings.value(QStringLiteral("detecting-speakers|%1").arg(kModelId)).toList().isEmpty());
        QVERIFY(settings.value(QStringLiteral("detecting-speakers|%1|diar").arg(kModelId)).toList().isEmpty());
        settings.endGroup();
    }

    // Item 17: manual chapter add/edit/delete round-trips through the .md.
    // addChapter() writes a MANUAL marker (sentinel on disk, stripped from the
    // displayed title, "manual" flag set); setChapterTitle() rewrites it;
    // deleteChapter() removes it. Each emits finished(fileUrl, ok, error).
    void chapterManualEdits_addSetTitleDelete()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QUrl audio = writeTranscribedFile(dir.path(), QStringLiteral("a.wav"),
                                                 QStringLiteral("[0] First segment.\n"
                                                                "[5] Second segment.\n"
                                                                "[10] Third segment.\n"));
        TranscriptionController controller;
        QSignalSpy finishedSpy(&controller, &TranscriptionController::finished);

        controller.addChapter(audio, 5.0, QStringLiteral("My Chapter"));
        QCOMPARE(finishedSpy.size(), 1);
        QVERIFY(finishedSpy.at(0).at(1).toBool()); // ok

        QVariantList chapters = controller.readChapters(audio);
        QCOMPARE(chapters.size(), 1);
        QCOMPARE(chapters[0].toMap().value("t0").toDouble(), 5.0);
        QCOMPARE(chapters[0].toMap().value("title").toString(), QStringLiteral("My Chapter"));
        QVERIFY(chapters[0].toMap().value("manual").toBool());
        // The sentinel must not leak into the DISPLAYED title...
        QVERIFY(!chapters[0].toMap().value("title").toString().contains(QStringLiteral("manual")));
        // ...but must be present verbatim on disk (so regeneration preserves it).
        QVERIFY(controller.readTranscriptBody(audio).contains(QStringLiteral("## [5] My Chapter <!-- manual -->")));

        controller.setChapterTitle(audio, 5.0, QStringLiteral("Renamed"));
        chapters = controller.readChapters(audio);
        QCOMPARE(chapters.size(), 1);
        QCOMPARE(chapters[0].toMap().value("title").toString(), QStringLiteral("Renamed"));
        QVERIFY(chapters[0].toMap().value("manual").toBool());

        controller.deleteChapter(audio, 5.0);
        QVERIFY(controller.readChapters(audio).isEmpty());

        // Deleting/editing a non-existent chapter reports failure (finished ok=false).
        finishedSpy.clear();
        controller.deleteChapter(audio, 99.0);
        QCOMPARE(finishedSpy.size(), 1);
        QVERIFY(!finishedSpy.at(0).at(1).toBool());
    }

    // Item 17 (the key test): regeneration must PRESERVE manual markers while
    // replacing auto ones. Drives spliceBodyChapters() directly (the pure
    // splice helper generateChaptersJob() writes through), no model needed.
    void spliceBodyChapters_preservesManualReplacesAuto()
    {
        const QString body = QStringLiteral("## [0] Old Auto Intro\n"
                                             "[0] seg zero.\n"
                                             "## [5] Hand Written <!-- manual -->\n"
                                             "[5] seg five.\n"
                                             "[10] seg ten.\n");
        // Regenerate with a fresh AUTO set at t=0 and t=10 (not 5).
        const QList<ResolvedChapter> regen = {{0.0, QStringLiteral("New Auto Intro")},
                                              {10.0, QStringLiteral("New Auto Later")}};
        const QString out = spliceBodyChapters(body, regen);

        QVERIFY(!out.contains(QStringLiteral("Old Auto Intro")));       // old auto stripped
        QVERIFY(out.contains(QStringLiteral("## [0] New Auto Intro")));  // new auto in
        QVERIFY(out.contains(QStringLiteral("## [10] New Auto Later")));
        QVERIFY(out.contains(QStringLiteral("## [5] Hand Written <!-- manual -->"))); // manual survives
        QCOMPARE(out.count(QStringLiteral("## [")), 3); // 2 new auto + 1 preserved manual

        // An auto chapter coinciding with the manual timestamp is skipped —
        // the manual one wins, no duplicate marker at the same time.
        const QString out2 = spliceBodyChapters(body, {{5.0, QStringLiteral("Should Not Appear")}});
        QVERIFY(out2.contains(QStringLiteral("## [5] Hand Written <!-- manual -->")));
        QVERIFY(!out2.contains(QStringLiteral("Should Not Appear")));
        QCOMPARE(out2.count(QStringLiteral("## [")), 1);
    }

    // Item 28 (concrete bug): a transcript spanning multiple windows must have
    // EVERY window within kMaxChapterInputChars — otherwise
    // LabelingEngine::generateChapters() silently truncates the oversized
    // window and the folded-in tail gets zero chapters. Also verifies the
    // windows cover through the final segment (the late portion isn't dropped).
    void computeChapterWindows_everyWindowFitsBudgetAndCoversTail()
    {
        // ~200-char segments, ending in a small trailing chunk — the exact
        // shape the old unconditional fold pushed back over budget.
        QStringList segs;
        const QString big(200, QLatin1Char('x'));
        for (int i = 0; i < 165; ++i)
            segs << big;

        const QList<int> windows = computeChapterWindows(segs);
        QVERIFY2(windows.size() >= 2, "expected a multi-window split for a long transcript");
        QCOMPARE(windows.first(), 0);

        for (int w = 0; w < windows.size(); ++w) {
            const int start = windows.at(w);
            const int end = (w + 1 < windows.size()) ? windows.at(w + 1) : segs.size();
            qsizetype chars = 0;
            for (int i = start; i < end; ++i)
                chars += QString::number(i - start).size() + 2 + segs.at(i).size() + 1;
            QVERIFY2(chars <= LabelingEngine::kMaxChapterInputChars,
                     qPrintable(QStringLiteral("window %1 has %2 chars, over budget %3 — the item-28 fold bug")
                                    .arg(w).arg(chars).arg(LabelingEngine::kMaxChapterInputChars)));
        }
        // Strictly increasing starts, last window reaches the final segments.
        for (int w = 1; w < windows.size(); ++w)
            QVERIFY(windows.at(w) > windows.at(w - 1));
        QVERIFY2(windows.last() < segs.size(), "the last window must cover the transcript's tail");
    }

    // Item 58: prelude length scales with transcript size — capped DOWN for
    // short transcripts (P29), scaled UP for long ones. Pure static mapping,
    // testable without a model.
    void effectiveWordinessForLength_capsShortScalesLong()
    {
        // Short: capped down regardless of a high base slider.
        QVERIFY(TranscriptionController::effectiveWordinessForLength(1.0, 30) <= 0.15);
        QVERIFY(TranscriptionController::effectiveWordinessForLength(1.0, 100) <= 0.35);

        // Long: at the SAME modest base, a longer transcript yields a higher
        // effective wordiness (fuller prelude tier) than a mid-length one.
        const double base = 0.5;
        const double midLen = TranscriptionController::effectiveWordinessForLength(base, 500);
        const double longLen = TranscriptionController::effectiveWordinessForLength(base, 3000);
        const double veryLong = TranscriptionController::effectiveWordinessForLength(base, 25000);
        QCOMPARE(midLen, base);      // mid length: unchanged
        QVERIFY2(longLen > midLen, "a long transcript should scale wordiness up");
        QVERIFY2(veryLong > longLen, "a very long transcript should scale up further");
        QVERIFY2(veryLong >= 0.9, "a multi-hour transcript should reach the top prelude tier");

        // Upward scaling never LOWERS an already-high base.
        QVERIFY(TranscriptionController::effectiveWordinessForLength(0.95, 3000) >= 0.95);
    }

    // Item 52: diarizeFile() enqueues a standalone "diarize" job on an
    // already-transcribed file — a job of the right kind, keyed on that file,
    // through the same queue/worker as every other action. The two diarization
    // onnx models are pre-created (zero-byte, "downloaded") so no download job
    // is queued ahead and the diarize job is the front-most entry.
    void diarizeFile_enqueuesDiarizeJob()
    {
        VaultFixture vault;
        // areDiarizationModelsDownloaded() only checks presence — zero-byte is
        // enough to suppress the would-be-ahead download job here.
        for (const QString &name :
             {QStringLiteral("pyannote-segmentation-3.0-int8.onnx"), QStringLiteral("wespeaker-resnet293-lm.onnx")}) {
            QFile f(vault.dir.path() + QStringLiteral("/.transcriber/models/") + name);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.close();
        }
        const QUrl file = writeTranscribedFile(vault.dir.path(), QStringLiteral("a.wav"),
                                                QStringLiteral("[0] hello there\n[5] general kenobi\n"));

        TranscriptionController controller;
        QSignalSpy finishedSpy(&controller, &TranscriptionController::finished);
        controller.diarizeFile(file, vault.vaultUrl);

        // Enqueued synchronously (startJob runs inline in diarizeFile) with the
        // right kind, keyed on the audio file.
        QVERIFY(!controller.jobs().isEmpty());
        QCOMPARE(controller.jobs().at(0).toMap().value("kind").toString(), QStringLiteral("diarize"));
        QCOMPARE(controller.jobs().at(0).toMap().value("fileUrl").toUrl(), file);
        QVERIFY(controller.isQueued(file));

        // Dedup: a second diarizeFile() while one is Queued/Active is a no-op.
        const int before = controller.jobs().size();
        controller.diarizeFile(file, vault.vaultUrl);
        QCOMPARE(controller.jobs().size(), before);

        // Let it settle so teardown is clean — the job fails fast (no audio to
        // decode / unloadable models), which is fine; we only assert enqueue.
        QTRY_VERIFY(finishedSpy.count() >= 1);
    }

    // The chosen segmentation/embedding models + sensitivity are carried on the
    // diarize job, persisted to tasks.json, and restored by loadPersistedQueue()
    // (so a queued speaker-detection job resumes with the same tuning after a
    // restart). Deliberately does NOT spin the event loop: persistQueue() writes
    // synchronously on the enqueue's queueChanged, before the worker can run the
    // job to failure (fake audio) and de-persist it — so we read/replay the
    // just-written tasks.json rather than waiting.
    void diarizeFile_carriesTuning_andPersists()
    {
        VaultFixture vault;
        // Plant the CHOSEN pair (not the defaults) zero-byte so no download job
        // jumps ahead and the diarize job is front-most.
        for (const QString &name : {QStringLiteral("reverb-diarization-v1-int8.onnx"),
                                    QStringLiteral("nemo-titanet-large.onnx")}) {
            QFile f(vault.dir.path() + QStringLiteral("/.transcriber/models/") + name);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.close();
        }
        const QUrl file = writeTranscribedFile(vault.dir.path(), QStringLiteral("a.wav"),
                                               QStringLiteral("[0] hello there\n[5] general kenobi\n"));

        TranscriptionController controller;
        controller.loadPersistedQueue(vault.vaultUrl); // records the vault for persistQueue()
        controller.diarizeFile(file, vault.vaultUrl, QStringLiteral("reverb-diarization-v1"),
                               QStringLiteral("nemo-titanet-large"), 0.35);

        // Carried on the live job (jobs() exposes the diarization fields).
        const QVariantMap job = controller.jobs().at(0).toMap();
        QCOMPARE(job.value("kind").toString(), QStringLiteral("diarize"));
        QCOMPARE(job.value("segmentationModelId").toString(), QStringLiteral("reverb-diarization-v1"));
        QCOMPARE(job.value("embeddingModelId").toString(), QStringLiteral("nemo-titanet-large"));
        QCOMPARE(job.value("diarizationThreshold").toDouble(), 0.35);

        // Written to tasks.json synchronously (read the raw file before any event
        // loop lets the worker fail the job and rewrite without it).
        QFile tasks(vault.dir.path() + QStringLiteral("/.transcriber/tasks.json"));
        QVERIFY(tasks.open(QIODevice::ReadOnly));
        const QString json = QString::fromUtf8(tasks.readAll());
        tasks.close();
        QVERIFY2(json.contains(QStringLiteral("reverb-diarization-v1")), qPrintable(json));
        QVERIFY2(json.contains(QStringLiteral("nemo-titanet-large")), qPrintable(json));

        // Restored by a fresh controller with the tuning intact (synchronous —
        // no QTRY, so the first controller's worker can't rewrite in between).
        TranscriptionController fresh;
        fresh.loadPersistedQueue(vault.vaultUrl);
        bool found = false;
        for (const QVariant &v : fresh.jobs()) {
            const QVariantMap replayed = v.toMap();
            if (replayed.value("kind").toString() == QStringLiteral("diarize")) {
                QCOMPARE(replayed.value("segmentationModelId").toString(), QStringLiteral("reverb-diarization-v1"));
                QCOMPARE(replayed.value("embeddingModelId").toString(), QStringLiteral("nemo-titanet-large"));
                QCOMPARE(replayed.value("diarizationThreshold").toDouble(), 0.35);
                found = true;
            }
        }
        QVERIFY2(found, "diarize job did not survive the persist/replay round-trip");
    }

    // Item 52: spliceBodySpeakers() is the write-back half of a standalone
    // diarize — given synthetic diarized turns (no real engine/models), it
    // adds "[Speaker N]" tags to an already-transcribed body WITHOUT touching
    // the timestamps or spoken text, and reports the applied speaker set. This
    // is the file-level "writes speaker info back without a re-transcribe"
    // proof the full diarizeJob() relies on.
    void spliceBodySpeakers_addsTags_withoutRetranscribe()
    {
        // Two speakers plus a hallucinated non-speech line (never tagged).
        const QString body = QStringLiteral("[0] hello there\n[5] general kenobi\n[8] [MUSIC PLAYING]\n");
        const std::vector<DiarizedTurn> turns = {{0.0, 4.0, 0}, {4.0, 12.0, 1}};
        QStringList applied;
        const QString out = spliceBodySpeakers(body, /*knownSpeakers=*/{}, turns, /*totalSeconds=*/12.0, &applied);

        QVERIFY2(out.contains(QStringLiteral("[0] [Speaker 1] hello there")), qPrintable(out));
        QVERIFY2(out.contains(QStringLiteral("[5] [Speaker 2] general kenobi")), qPrintable(out));
        // Non-speech annotation line is left untouched (no speaker tag).
        QVERIFY2(out.contains(QStringLiteral("[8] [MUSIC PLAYING]")), qPrintable(out));
        QCOMPARE(applied, (QStringList{QStringLiteral("Speaker 1"), QStringLiteral("Speaker 2")}));
    }

    // Item 52: re-diarizing a file that ALREADY has speaker tags replaces them
    // in place (per its frontmatter "speakers:" allowlist) rather than
    // doubling them up.
    void spliceBodySpeakers_reDiarize_replacesExistingTags()
    {
        const QString body =
            QStringLiteral("[0] [Speaker 1] hello there\n[5] [Speaker 1] general kenobi\n");
        const QStringList known = {QStringLiteral("Speaker 1")};
        const std::vector<DiarizedTurn> turns = {{0.0, 4.0, 0}, {4.0, 10.0, 1}};
        QStringList applied;
        const QString out = spliceBodySpeakers(body, known, turns, /*totalSeconds=*/10.0, &applied);

        QVERIFY2(out.contains(QStringLiteral("[0] [Speaker 1] hello there")), qPrintable(out));
        QVERIFY2(out.contains(QStringLiteral("[5] [Speaker 2] general kenobi")), qPrintable(out));
        // No doubled tags introduced.
        QVERIFY2(!out.contains(QStringLiteral("[Speaker 1] [Speaker")), qPrintable(out));
        QCOMPARE(applied, (QStringList{QStringLiteral("Speaker 1"), QStringLiteral("Speaker 2")}));
    }
};

QTEST_GUILESS_MAIN(TstTranscriptionQueue)
#include "tst_transcription_queue.moc"
