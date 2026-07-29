// TranscribeTask, ported to the task-system refactor (.task/plan.md). Only
// the structural interface and the offline "unknown engine id" early-exit
// path (resolveEngine() just scans local vault dirs, no network) are
// exercised for real — an actual decode+transcribe run needs a real
// downloaded whisper/parakeet model, same "don't exercise the heavy/network
// path in a unit test" precedent as the other engine-backed kinds.

#include <QTemporaryDir>
#include <QUrl>
#include <QtTest>

#include "TranscriptFormat.h"
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

    // TranscribeTask must declare needing ONLY the ASR engine —
    // this is what lets the dispatch-time residency policy (EngineCache::
    // applyResidencyPolicy, see TaskRunner::execute) leave asr resident
    // across a batch of transcribes while still evicting diar/label (the
    // bug this item fixes: a diarization arena surviving into a transcribe).
    void transcribeTask_declaresOnlyAsrNeed()
    {
        TranscribeTask task(QUrl::fromLocalFile("/vault/a.wav"), QUrl::fromLocalFile("/vault"), QStringLiteral("small.en"));
        const Task::EngineNeeds needs = task.engineNeeds();
        QVERIFY(needs.asr);
        QVERIFY(!needs.label);
        QVERIFY(!needs.diar);
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

    // corruptAudioWarning() is the ONE place the corrupt-audio
    // repair note is built — shared verbatim by the frontmatter's
    // corruptAudioNote and the Task-level warning surfaced via
    // TaskContext::reportWarning(). Default-constructed diagnostics (nothing
    // scrubbed) must produce nothing to say.
    void corruptAudioWarning_emptyDiagnostics_yieldsEmptyString()
    {
        QCOMPARE(TranscribeTask::corruptAudioWarning({}), QString());
    }

    void corruptAudioWarning_reportsCountAndFirstOffset()
    {
        AudioDecoder::DecodeDiagnostics diagnostics;
        diagnostics.nonFiniteSampleCount = 196249;
        diagnostics.firstNonFiniteSeconds = 12.5;
        const QString warning = TranscribeTask::corruptAudioWarning(diagnostics);
        QVERIFY2(warning.contains(QStringLiteral("196249")), qPrintable(warning));
        QVERIFY2(warning.contains(QStringLiteral("non-finite")), qPrintable(warning));
        // formatTimestamp(12.5) trims to "12" (see TranscriptFormat.h) — this
        // just needs to be present, not re-derive the exact compact format.
        QVERIFY2(warning.contains(formatTimestamp(12.5)), qPrintable(warning));
    }

    void corruptAudioWarning_unknownOffset_saysSo()
    {
        AudioDecoder::DecodeDiagnostics diagnostics;
        diagnostics.nonFiniteSampleCount = 3;
        // firstNonFiniteSeconds left nullopt.
        QVERIFY2(TranscribeTask::corruptAudioWarning(diagnostics).contains(QStringLiteral("unknown")),
                 "an unknown offset must say so rather than showing a bogus timestamp");
    }

    // --- EngineCache::applyResidencyPolicy() ---------------------
    // These exercise the residency RULE directly, without a real downloaded
    // model — same "no cheap way to observe engine identity" workaround
    // tst_task_diarize.cpp's diarEvictionCount() test already established:
    // an eviction counter stands in for "is this engine resident".

    // The actual bug: TranscribeTask declares needing only asr (see
    // transcribeTask_declaresOnlyAsrNeed above), so applying its needs must
    // evict diar (and label) — i.e. after a transcribe dispatches, no
    // diarization engine is left resident.
    void applyResidencyPolicy_asrOnlyNeed_evictsLabelAndDiar_notAsr()
    {
        EngineCache engines;
        engines.applyResidencyPolicy(/*asr=*/true, /*label=*/false, /*diar=*/false);
        QCOMPARE(engines.asrEvictionCount(), 0);
        QCOMPARE(engines.labelEvictionCount(), 1);
        QCOMPARE(engines.diarEvictionCount(), 1);
    }

    // The performance guarantee: a run of consecutive same-need dispatches
    // (a batch of transcribes, all declaring asr-only) must never evict the
    // engine they share — asrEvictionCount must stay at 0 across repeats, or
    // every file in the batch would pay a whisper reload.
    void applyResidencyPolicy_repeatedSameNeeds_neverEvictsTheSharedEngine()
    {
        EngineCache engines;
        for (int i = 0; i < 5; ++i)
            engines.applyResidencyPolicy(/*asr=*/true, /*label=*/false, /*diar=*/false);
        QCOMPARE(engines.asrEvictionCount(), 0);
        // The OTHER two are re-asserted-absent every dispatch — harmless
        // (nothing to reset once already gone), just not free of a counter
        // bump, which is why this test pins asr specifically, not the others.
        QCOMPARE(engines.labelEvictionCount(), 5);
        QCOMPARE(engines.diarEvictionCount(), 5);
    }

    // A task declaring it needs ALL THREE engines must never evict any of
    // them — the "a task must never evict an engine it is about to use"
    // constraint, checked generically rather than per-kind.
    void applyResidencyPolicy_allNeeded_evictsNothing()
    {
        EngineCache engines;
        engines.applyResidencyPolicy(/*asr=*/true, /*label=*/true, /*diar=*/true);
        QCOMPARE(engines.asrEvictionCount(), 0);
        QCOMPARE(engines.labelEvictionCount(), 0);
        QCOMPARE(engines.diarEvictionCount(), 0);
    }

    // A task declaring no needs at all (the base-class default — every
    // non-engine kind: move/rename/trash/download/combine/…) gets a fully
    // clean slate.
    void applyResidencyPolicy_noNeeds_evictsAllThree()
    {
        EngineCache engines;
        engines.applyResidencyPolicy(/*asr=*/false, /*label=*/false, /*diar=*/false);
        QCOMPARE(engines.asrEvictionCount(), 1);
        QCOMPARE(engines.labelEvictionCount(), 1);
        QCOMPARE(engines.diarEvictionCount(), 1);
    }
};

QTEST_GUILESS_MAIN(TestTaskTranscribe)
#include "tst_task_transcribe.moc"
