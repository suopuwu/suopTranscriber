#pragma once

#include <QDateTime>
#include <QElapsedTimer>
#include <QFile>
#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QSet>
#include <QString>
#include <QThread>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>
#include <QMap>
#include <QMutex>
#include <QVector>
#include <qqml.h>

#include <functional>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "CombineScanner.h" // full type — CombineGroup is a by-value parameter/return
                            // type of the combine queue glue below
#include "EngineRegistry.h" // EngineDescriptor — discoverEngines()/resolveEngine() return it
                            // by value (std::vector<>/std::optional<>), which needs the
                            // complete type, not just a forward declaration.
#include "PhaseTimer.h" // full type — m_phaseTimer is called from inline getters
#include "RollingAverage.h"
#include "Segment.h"
#include "UsbRuleStore.h"
#include "VaultTreeModel.h" // full type (not just a forward decl) — the
                            // Q_PROPERTY(VaultTreeModel*) below needs it complete
                            // for moc's metatype registration, same as
                            // UsbGrabberController. The controller calls its
                            // applyRemovalOutcome()/applyRestoreOutcome() on the
                            // GUI thread after a trash/delete/restore task ends,
                            // so the model still owns the per-file signal emission.
#include "tasks/ModelDownloadTarget.h"
#include "tasks/Task.h"

#include "TaskScheduler.h" // the display-blind queue engine (PHASE Q) + the
                          // TaskSchedulerObserver interface this class implements.
                          // Full type: the observer overrides + the inline getters
                          // below call m_scheduler-> directly.

class QTimer;
class TaskRunner;
// (VaultTreeModel is included above, not forward-declared — see the include note.)
                      // so the model still owns the per-file delete/restore
                      // signal emission MainPanel depends on.

#include "TranscriptFormat.h" // the transcript .md file format (SegmentLineWriter,
                             // ResolvedChapter/ParsedSegment, the frontmatter
                             // writers, the body splices) — split out of
                             // TranscriptionController.cpp verbatim. Included
                             // (not forward-declared) so every existing caller of
                             // these helpers keeps compiling against this header.

// Item P34 / item 28: splits a transcript's segment list into consecutive
// windows, each formatted-char count kept within LabelingEngine::
// kMaxChapterInputChars, so TranscriptionWorker::generateChaptersJob() can
// call generateChapters() once per window instead of silently truncating one
// oversized call. Returns the segment index each window STARTS at (always
// [0, ...]); window w spans [result[w], result[w+1]) (or to the end for the
// last). Item 28 bug: a tiny trailing window is folded into the previous one
// ONLY when the combined window still fits kMaxChapterInputChars — otherwise
// the fold would push the merged window back over budget and
// generateChapters() would truncate exactly the folded-in tail, so the final
// stretch of a long transcript got zero chapters. Declared here (rather than
// file-local in the .cpp) so the windowing/budget invariant is unit-testable
// directly with synthetic segment text, without a real label model.
QList<int> computeChapterWindows(const QStringList &segmentTexts);


// A model id with the "engine:" prefix routes to an out-of-process
// SubprocessEngine (DESIGN.md "Pluggable engines"); anything else is a
// whisper ggml tier (or the sherpa-onnx in-process Parakeet bundle) resolved
// by ModelManager.
bool isEngineModelId(const QString &modelId);
QString engineIdFromModelId(const QString &modelId);

// Discovers every out-of-process engine visible to vaultPath, capability-
// checked against this machine. resolveEngine finds one by id; a found-but-
// unavailable engine is still returned so the caller can surface
// .unavailableReason rather than a bare "not found".
std::vector<EngineDescriptor> discoverEngines(const QString &vaultPath);
std::optional<EngineDescriptor> resolveEngine(const QString &engineId, const QString &vaultPath, QString *error);

// Orchestrates: ffmpeg decode -> WhisperCppEngine -> vault sidecar files.
// Owns a dedicated QThread; transcription (and its file IO) NEVER runs on
// the GUI thread (see CLAUDE.md). Whole-file transcription only for now —
// no VAD chunking / resume yet (DESIGN.md "Durable / resumable
// transcription" is a later milestone). Runs at most one job at a time but
// queues additional transcribe() calls (no duplicates by fileUrl) instead of
// dropping them, auto-starting the next queued job on completion.
//
// Job history is persistent for the app session: Done/Failed jobs stay in
// m_jobs (visible to QML via the jobs property) until the user dismisses
// them individually (dismiss()) or clears them in bulk (clearFinished()) —
// like a download manager, not a transient progress bar.
class TranscriptionController : public QObject, public TaskSchedulerObserver {
    Q_OBJECT
    QML_ELEMENT
    // Test-only seam: the queue's cancel/pause state machine can only be
    // exercised against a task that is Active AND pausable && !cancellableMidRun
    // (diarization's shape — the cancel-while-paused "phantom resume" class of
    // bug). There is no public enqueue for an arbitrary Task, and the only such
    // real kind (DiarizeTask) needs heavy diarization models, so the queue test
    // injects a throwaway fake Task via enqueueTask(). Grants nothing to
    // production code.
    friend class TstTranscriptionQueue;
    Q_PROPERTY(bool busy READ isBusy NOTIFY busyChanged)
    Q_PROPERTY(double progress READ progress NOTIFY progressChanged)
    // Item 6 (user correction, 2026-07-08): the PROGRESS BAR fill should
    // still sweep smoothly across a job's WHOLE lifetime (diarization then
    // transcription treated as one continuous 0..1, same blend `progress`
    // used before this wave) — only the PERCENTAGE TEXT should be phase-
    // relative (see `progress`'s own doc comment). QML binds the ProgressBar
    // `value` to this and the percentage text to `progress`.
    //
    // 2026-07-10: each active phase's SLICE of this 0..1 range is no longer
    // a fixed guess (used to be a hardcoded diarization=first-half/
    // transcription=second-half split, loading/decoding excluded entirely)
    // — it's this phase's share of the ROLLING AVERAGE of how long each
    // phase has actually taken over the last 10 completed jobs (see
    // RollingAverage, m_phaseTimings, recomputeOverallProgress()). "loading"
    // and "decoding" have no native progress hook, so their within-phase
    // fraction is synthesized from elapsed-time-in-phase vs. that phase's
    // own rolling average (see phaseRelativeSyntheticProgress()) rather than
    // going indeterminate — the bar now gives every phase real, historically-
    // proportioned width instead of just the two that happened to have a
    // progress callback.
    Q_PROPERTY(double overallProgress READ overallProgress NOTIFY overallProgressChanged)
    // Wave N (task system): benchmarkActive/benchmarkProgress are GONE.
    // They existed because a benchmark needed the bottom-bar glow but must
    // NOT raise MainPanel's blocking overlay, and `busy` was overloaded to
    // mean both — so a benchmark left the queue entirely and grew a parallel
    // glow path, which is exactly how a task ends up invisible to the Tasks
    // list. The axis is now explicit: EVERY task is queued and gets a row +
    // the glow (`busy`), while Task::presentation() alone decides whether it
    // also takes the overlay. `activeIsForeground` below is what the overlay
    // binds to. See .task/plan.md invariant 2.
    Q_PROPERTY(bool activeIsForeground READ activeIsForeground NOTIFY activeIsForegroundChanged)
    // "loading" (whisper model load — no native progress hook exists for
    // this; `progress` here is synthesized from elapsed-vs-historical-
    // average, see overallProgress's doc comment), "detecting-speakers-
    // segmenting" / "detecting-speakers-identifying" (diarization pass, REAL
    // progress via DiarizationEngine's sherpa-onnx-backed callback — split
    // into two sub-phases at fraction==0.30, the exact boundary where
    // sherpa-onnx's own internal work moves from finding WHERE speech is to
    // figuring out WHOSE voice it is, so the label switches right when the
    // real work does, not on a guess), "transcribing" (whisper's own
    // per-segment progress, as before), or empty when idle or for a
    // relabel/rename job (neither has phases worth naming). Each phase's own
    // progress() is phase-relative (its own 0->1); overallProgress blends
    // them into one continuous whole-job sweep (see its doc comment) rather
    // than each phase getting a full-width 0->1, which would look like the
    // bar finishes then restarts. Exists because "huge delay before it
    // starts transcribing" on bigger models was previously invisible — the
    // progress bar sat frozen at 0 during model load and (once diarization
    // landed) during the whole diarization pass too, indistinguishable from
    // a hang.
    Q_PROPERTY(QString currentPhase READ currentPhase NOTIFY currentPhaseChanged)
    Q_PROPERTY(QUrl currentFile READ currentFile NOTIFY currentFileChanged)
    // The kind slug of whichever task is Active right now (see jobs()' "kind"
    // key — same values), empty when idle. Wave N bugfix: this now has its
    // OWN NOTIFY, fired in startTask()/onTaskFinished(). It used to reuse
    // currentFileChanged, which only fires when the FILE changes — so a
    // pipeline of same-file steps (transcribe→diarize→relabel→chapters, all
    // on one fileUrl) left currentJobKind reading "transcribe" the whole way
    // through, and the overlay label had to key off currentPhase to work
    // around it. A dedicated signal makes the property honest mid-pipeline.
    Q_PROPERTY(QString currentJobKind READ currentJobKind NOTIFY currentJobKindChanged)
    // Whether the Active task occupies its file for per-file gating (Task::
    // occupiesFile()) — false when idle or when the Active task is a comparison-
    // only benchmark/sweep. The MainPanel per-file action buttons AND their
    // to-a-benchmark of the open file: `busy && currentFile == open` is true for
    // an Active benchmark too, so they additionally check this to stay enabled.
    // Same NOTIFY as currentJobKind — both change exactly when the Active task
    // does (startTask/onTaskFinished).
    Q_PROPERTY(bool activeOccupiesFile READ activeOccupiesFile NOTIFY currentJobKindChanged)
    // The Active task's human category label (Task::kindLabel(), e.g. "Move to
    // trash") — what the bottom bar shows before the filename. Parallel to
    // currentJobKind (the slug); same NOTIFY. Lets the bottom bar read the label
    // straight off the task instead of re-deriving it from the slug in QML.
    Q_PROPERTY(QString currentJobKindLabel READ currentJobKindLabel NOTIFY currentJobKindChanged)
    // Whether the Active task can be paused at all — read straight off its
    // Task::capabilities().pausable. Wave N: this REPLACES BottomBar's own
    // canPause(kind) string allowlist ("transcribe"/"download"/…), which was a
    // re-implementation of the same fact that could drift from the C++ side.
    // The task is the single source of truth now, which is the whole point of
    // capabilities() (the 2026-07-09 pause bug was exactly such a drift). This
    // is coarse "could this kind ever pause" — the finer "may I pause RIGHT
    // NOW" (not mid-"loading", not while a soft-restart is in flight) still
    // lives in BottomBar.canPauseNow(), gated additionally on phase.
    Q_PROPERTY(bool activeCanPause READ activeCanPause NOTIFY currentJobKindChanged)
    Q_PROPERTY(int tasksTotal READ tasksTotal NOTIFY queueChanged)
    Q_PROPERTY(int tasksCompleted READ tasksCompleted NOTIFY queueChanged)
    Q_PROPERTY(QVariantList queuedFileUrls READ queuedFileUrls NOTIFY queueChanged)
    Q_PROPERTY(QVariantList jobs READ jobs NOTIFY queueChanged)
    // Item 11: the SAME entries jobs() returns, but partitioned for display —
    // Active first, then Queued in run order, then Paused, then finished
    // (Done/Failed, most-recently-finished first). jobs() stays in raw
    // append/insertion order (run order == append order — see startNext());
    // this is the ordering the Tasks popup's ListView binds to so the
    // currently-running/next-to-run work is always at the TOP and finished
    // history sinks to the bottom. Everything else (queuedPositionOf(),
    // moveQueuedJob(), etc.) keeps walking the raw jobs() order, unaffected.
    Q_PROPERTY(QVariantList sortedJobs READ sortedJobs NOTIFY queueChanged)
    // Bumps once for each real new segment line written to the .md sidecar
    // during an in-progress transcription — throttled (see run()'s
    // liveUpdateTimer) so a burst of short segments still coalesces into at
    // most a few ticks per second. QML's "live updates as segments are
    // written" hooks (MainPanel.qml's transcriptBody/sentences bindings)
    // must depend on THIS, not progress: progress fires on whisper's every
    // internal decode-loop iteration (including decode retries/fallbacks),
    // far more often than real new content actually appears, and each live-
    // update tick re-reads+re-parses the whole file and rebuilds the whole
    // transcript view — tying it to progress was clogging the GUI thread.
    Q_PROPERTY(int liveUpdateTick READ liveUpdateTick NOTIFY liveUpdateTickChanged)
    Q_PROPERTY(int pendingRenamesRevision READ pendingRenamesRevision NOTIFY pendingRenamesRevisionChanged)
    // Wave N (task system): true while the Active job is paused. Only
    // "transcribe" and "download" jobs are truly pausable (see pauseActive()'s
    // doc comment) — false at all other times, including whenever nothing is
    // Active.
    Q_PROPERTY(bool activePaused READ isActivePaused NOTIFY activePausedChanged)
    // True from the moment a pause click during "loading" fires the
    // soft-cancel-as-restart (see JobStatus::Paused's doc comment) until it
    // actually resolves to Paused. QML uses this to disable the pause button
    // meanwhile — with no such guard, a second click in that window (nothing
    // visibly changes until the underlying blocking call returns) stacks a
    // real pauseActive() request on top of the still-pending cancel, which
    // permanently breaks pause for that job (see togglePauseActive()'s doc
    // comment for why).
    Q_PROPERTY(bool activeStopping READ isActiveStopping NOTIFY activeStoppingChanged)
    // Item O35: "CUDA"/"Vulkan"/"CPU" — whichever backend whisper.cpp picked
    // at the most recent model load (see WhisperCppEngine::activeBackendName()'s
    // doc comment). Empty until the first model finishes loading this
    // session; QML is expected to hide its indicator until then rather than
    // show a blank/placeholder value. Never populated for a relabel/rename/
    // merge/download job — none of those touch WhisperCppEngine.
    Q_PROPERTY(QString activeBackendName READ activeBackendName NOTIFY activeBackendNameChanged)
    // Item 25: a coarser, user-facing reduction of activeBackendName — "gpu"
    // for any real accelerator (CUDA/Vulkan/…), "cpu" for the CPU backend,
    // empty (same as activeBackendName) until the first model finishes
    // loading. Exposed as its own derived property so every display site can
    // show just "gpu"/"cpu" (the user asked for exactly those two words,
    // lowercase) off ONE mapping instead of each re-deriving it. QML hides
    // its indicator on empty, same convention as activeBackendName.
    Q_PROPERTY(QString activeBackendKind READ activeBackendKind NOTIFY activeBackendNameChanged)
    // The vault tree model, bound from Main.qml (`vaultTreeModel: vaultModel`),
    // same as UsbGrabberController. The controller calls its applyRemovalOutcome()/
    // applyRestoreOutcome() after a trash/delete/restore task completes so the
    // model — which owns the orphaned-vs-audio classification — emits the per-file
    // delete/restore signals MainPanel reacts to. Null in headless tests, which
    // exercise the tasks' filesystem work directly; the apply* calls no-op then.
    Q_PROPERTY(VaultTreeModel *vaultTreeModel READ vaultTreeModel WRITE setVaultTreeModel NOTIFY
                   vaultTreeModelChanged)

public:
    // Wave N (task system): the job status enum lives on Task now
    // (Task::Status). Its member VALUES are unchanged (Queued=0..Failed=4),
    // so jobStatusForPath()'s int contract with QML/Sidebar is preserved;
    // "no task for this path" is still the -1 sentinel that enum deliberately
    // has no member for (jobStatusForPath returns a raw -1).

    // The phase-timing subsystem moved to PhaseTimer (2026-07-20) — see its
    // class doc comment. These two stay here as the names tests (and any
    // caller holding only a controller) already spell; both are pure
    // forwarders to the canonical definitions on PhaseTimer, where their full
    // doc comments now live.
    static double defaultPhaseDurationMs(const QString &phase);
    static const QStringList kTimedPhases;

    explicit TranscriptionController(QObject *parent = nullptr);
    ~TranscriptionController() override;

    bool isBusy() const { return m_busy; }
    // The CURRENT phase's own 0..1 fraction: the real callback value for phases
    // that report one (detecting-speakers/transcribing), or an
    // elapsed-vs-average synthetic sweep for the no-callback phases
    // (loading/decoding/detecting-speech), so the bottom bar's per-phase
    // percentage counts up instead of sitting at 0% until the phase flips (see
    // phaseRelativeSyntheticProgress()).
    // PhaseTimer owns the math; the active-job context it needs (current
    // phase, model, diarization flag, the real callback fraction, and how long
    // this phase has been running) is serial-lane display state that only this
    // object can observe, so it's passed in per call.
    double progress() const
    {
        return m_phaseTimer->phaseRelativeSyntheticProgress(m_phase, activeModelIdForTiming(),
                                                            m_activeSpeakerDetection, m_progress,
                                                            activeElapsedMs() - m_phaseStartMs);
    }
    // See the overallProgress Q_PROPERTY doc comment above. Stored on
    // PhaseTimer, which also owns the change signal this class re-emits.
    double overallProgress() const { return m_phaseTimer->overallProgress(); }
    // True iff a task is Active AND it declares Presentation::Foreground —
    // i.e. it owns the main-panel overlay. False when idle, and false for a
    // Background task (benchmark/sweep/download), which still shows a Tasks
    // row + the glow via isBusy(). See the activeIsForeground Q_PROPERTY doc
    // comment above.
    bool activeIsForeground() const { return m_activeIsForeground; }
    bool activeOccupiesFile() const
    {
        Task *task = m_scheduler->activeTask();
        return task && task->occupiesFile();
    }
    bool activeCanPause() const;
    QString currentPhase() const { return m_phase; }
    // The phase-vocabulary REGISTRY (slug -> bottom-bar label / main-panel
    // overlay title) moved to JobListFormatter (2026-07-20) — see its class
    // doc comment, where these two carry their full docs. They stay here as
    // the Q_INVOKABLE names QML already spells (BottomBar's phase text,
    // MainPanel's transcribingOverlayLabel); both are pure forwarders.
    Q_INVOKABLE QString phaseLabel(const QString &phase) const;
    Q_INVOKABLE QString phaseOverlayLabel(const QString &phase) const;
    QUrl currentFile() const { return m_currentFile; }
    // See the liveUpdateTick Q_PROPERTY doc comment above.
    int liveUpdateTick() const { return m_liveUpdateTick; }
    // See the currentJobKind Q_PROPERTY doc comment above.
    QString currentJobKind() const;
    // See the currentJobKindLabel Q_PROPERTY doc comment above.
    QString currentJobKindLabel() const;
    // See the activePaused Q_PROPERTY doc comment above.
    bool isActivePaused() const { return m_activePaused; }
    // See the activeStopping Q_PROPERTY doc comment above. The pause/cancel
    // INTENT lives on the scheduler now (Q1); the NOTIFY is re-emitted through
    // the observer methods + onActiveStoppingChanged().
    bool isActiveStopping() const { return m_scheduler->isActiveStopping(); }
    // See the activeBackendName Q_PROPERTY doc comment above.
    QString activeBackendName() const { return m_activeBackendName; }
    // See the activeBackendKind Q_PROPERTY doc comment above.
    QString activeBackendKind() const { return backendKind(m_activeBackendName); }
    // Item 25: pure mapping "CUDA"/"Vulkan"/…/"CPU"/"" -> "gpu"/"cpu"/"".
    // Static + public so it's unit-testable directly without a live backend
    // load. Case-insensitive "CPU" match; anything else non-empty is "gpu".
    static QString backendKind(const QString &backendName);
    // Item P29 / item 58: the transcript-length-aware EFFECTIVE wordiness a
    // label/prelude pass actually uses, given the user's base wordiness slider
    // and the transcript's word count. Caps DOWN for short transcripts (a
    // 3-sentence transcript shouldn't get a 5-paragraph prelude — P29) AND
    // scales UP for very long ones (a 2-3 hour transcript earns a fuller
    // prelude tier — item 58). Static + public so the length->tier mapping is
    // unit-testable directly without spinning up a real label model (the
    // relabelJob worker path that consumes it needs a ~2.5GB GGUF loaded).
    static double effectiveWordinessForLength(double baseWordiness, int wordCount);
    // Wall-clock time the Active job has spent actually RUNNING, excluding
    // any time spent paused — this is what elapsed/duration display in the
    // UI, not finishedAt-startedAt (see pauseActive()'s doc comment for why
    // paused time shouldn't inflate it). 0 when nothing is Active. Not a
    // Q_PROPERTY: it changes continuously while a job runs and QML is
    // expected to re-poll it on its own Timer tick (see BottomBar.qml)
    // rather than have this object tick a timer of its own just to fire
    // NOTIFY signals nobody but a display needs.
    Q_INVOKABLE qint64 activeElapsedMs() const override;
    // Linear-extrapolation estimate of when the Active job will finish, in
    // milliseconds from now (-1 if unknown — nothing Active, or too early in
    // the job for m_overallProgress to be above 0 yet). Deliberately uses the
    // BLENDED cross-phase fraction internally (m_overallProgress — see its
    // Q_PROPERTY doc comment), never the phase-relative one BottomBar's
    // percentage text shows (see currentPhase's doc comment for why those
    // two numbers are now different) — an ETA needs to know how much of the
    // WHOLE job is left, not just the current phase. Now meaningful even
    // during "loading"/"decoding" (their synthetic progress still advances
    // m_overallProgress — see phaseRelativeSyntheticProgress()), unlike
    // before those phases had rolling-average timing data to synthesize from.
    Q_INVOKABLE qint64 estimatedRemainingMs() const;
    // Derived from m_jobs: total entry count / Done+Failed count. Both only
    // grow until dismiss()/clearFinished() shrink the list (see class doc).
    // Thin forwarders to TaskQuery — pure over the task list; they stay here
    // because they're the Q_PROPERTY surface QML binds to.
    int tasksTotal() const;
    int tasksCompleted() const;
    // fileUrls of jobs still Queued (not yet started) — used by QML to show
    // a "Queued…" state distinct from the currently-Active job. Forwards to
    // TaskQuery::queuedFileUrls().
    QVariantList queuedFileUrls() const;
    // Every job for this session (Queued+Active+Done+Failed), each as
    // {fileUrl, status ("queued"/"active"/"done"/"failed"), error,
    // kind ("transcribe"/"relabel"/"rename"/"merge"/"chapters")} — kind lets
    // the UI distinguish a full transcription from a "regenerate label" or a
    // speaker-rename/merge task in the same list (they all share this one
    // queue/worker thread).
    QVariantList jobs() const;

    // Item 11: jobs() reordered for display — see the sortedJobs Q_PROPERTY
    // doc comment. Walking m_tasks (jobs()) is the part only this class can
    // do; the collapsing and partitioning are pure transforms over that list
    // and live on JobListFormatter.
    QVariantList sortedJobs() const;

    // The two pure jobs()-list display transforms moved to JobListFormatter
    // (2026-07-20) — see its class doc comment, where their full doc comments
    // now live. They stay here as the public static names
    // tst_transcription_queue already spells; both are pure forwarders.
    static QVariantList sortJobsForDisplay(const QVariantList &jobEntries);
    static QVariantList collapseBatches(const QVariantList &jobEntries);

    // Item P6: lets the sidebar's file tree query a single path's job status
    // directly, without walking the whole jobs() list itself. path must be an
    // absolute local file path (QUrl::toLocalFile()), matching what m_jobs'
    // Job::fileUrl resolves to — same convention every other path-keyed
    // lookup in this class uses. Returns JobStatus::None (-1) if path has no
    // Queued/Active/Paused/Done/Failed entry at all. When multiple jobs exist
    // for the same path (e.g. a Done history entry plus a freshly re-queued
    // run), the more "in progress" one wins — see rebuildJobStatusIndex()'s
    // doc comment for the exact priority order.
    Q_INVOKABLE int jobStatusForPath(const QString &path) const;

    // No-op if fileUrl already has a Queued or Active entry (no duplicate
    // in-flight-or-pending tasks). A Done/Failed entry for the same fileUrl
    // does NOT block re-queueing — deliberately allows re-transcribing after
    // dismissal or after changing settings. Starts immediately if idle,
    // otherwise queues. autoLabel opts into AI auto-labeling (see
    // LabelingEngine) — this does NOT run as a bonus step inside the
    // transcription job itself: a SEPARATE relabel()-equivalent job (same
    // "kind": "relabel" as a manual "regenerate label" click) is queued
    // immediately alongside the transcribe job, right here in this same
    // call — BOTH show up in the Tasks list together right away, not one
    // appended later once transcription finishes. It waits its turn behind
    // the transcribe job (only one job runs at a time) and, if that job
    // fails, still runs next and fails on its own (nothing to label yet) —
    // it is never silently skipped just because its upstream job failed, so
    // the Tasks list always reflects exactly what was requested up front.
    // restrictTagsToExisting (item 14 "auto-categorize" mode): when true,
    // the labeling pass is instructed to only pick tags already used
    // elsewhere in this vault (scanned fresh per job), leaving tags empty
    // rather than inventing a new one if none fit. Ignored when autoLabel
    // is false. tagReuseBias (item 15, Wave E): 0.0-1.0 soft nudge between
    // inventing new tags vs. reusing existing ones — only meaningful when
    // restrictTagsToExisting is false (that's already a hard constraint).
    // wordiness (Feature 1): 0.0-1.0 soft nudge on how verbose the generated
    // label and prelude are, independent of the tag settings above.
    Q_INVOKABLE void transcribe(const QUrl &fileUrl, const QUrl &vaultUrl, const QString &modelId,
                                 bool autoLabel = false, bool restrictTagsToExisting = false,
                                 double tagReuseBias = 0.5, double wordiness = 0.5);

    // Convenience batch wrapper: calls transcribe() for each url. Dedup is
    // entirely delegated to transcribe(); the caller decides which files are
    // actually untranscribed.
    Q_INVOKABLE void transcribeAll(const QVariantList &fileUrls, const QUrl &vaultUrl, const QString &modelId,
                                    bool autoLabel = false,
                                    bool restrictTagsToExisting = false, double tagReuseBias = 0.5,
                                    double wordiness = 0.5);

    // Item 37: drag-and-drop / external audio import as a tracked, async
    // task. sourceUrl is the external file the user dropped; destUrl is the
    // in-vault destination VaultTreeModel::importDestinationFor() already
    // validated (wrong-type / collision / no-vault are rejected there,
    // synchronously, before this is ever called — so this method assumes a
    // valid, non-colliding destination and only owns the slow part: the
    // byte-copy). Queued through the SAME worker-thread job pipeline as
    // transcribe()/download()/combine() (a large copy must never block the
    // GUI thread — CLAUDE.md), so it shows up in jobs()/tasksTotal/the Tasks
    // popup like any other task, with the same cancel/progress plumbing. The
    // job's fileUrl is destUrl (so the Tasks row names the destination file
    // and clicking it opens the imported file once done); a copy failure that
    // happens on the worker (e.g. source vanished mid-copy, disk full)
    // surfaces as a Failed job, mirroring every other kind. Dedup follows the
    // same Queued/Active rule as transcribe() (keyed on destUrl).
    // `overwrite` (default false) threads through to ImportTask: true lifts
    // its collision guard so the copy replaces the existing destination file's
    // bytes (audio only — see ImportTask.h's doc comment on the sibling .md).
    Q_INVOKABLE void importAudio(const QUrl &sourceUrl, const QUrl &destUrl, const QUrl &vaultUrl, bool overwrite = false);

    // One USB grab — the recursive scan+copy of ONE mounted volume under ONE
    // resolved rule — as a tracked, queued task. Returns the new job's id so
    // UsbGrabberController can correlate the usbGrabFinished() below back to
    // the device that asked for it (and know when its auto-grabs have drained).
    //
    // Called from C++ (UsbGrabberController), not QML: the rule has to be
    // RESOLVED first (device rule, else the "*" catch-all — plus the mount
    // gate), which is exactly the knowledge UsbGrabberController owns and this
    // class has no business duplicating. So this is deliberately not
    // Q_INVOKABLE — QML asks the grabber controller for a grab, not this.
    //
    // Queued through the SAME worker-thread pipeline as importAudio()/combine()
    // rather than the standalone thread grabs used to own, which is the whole
    // point: a grab is a long file-copy that now gets a Tasks row, the progress
    // glow, and pause/cancel for free, and can't fight a running transcription
    // for the same disk. The cost, accepted: a hotplug grab that lands mid-
    // transcription waits its turn instead of starting immediately.
    //
    // No dedup on fileUrl (unlike transcribe()/importAudio()): a grab's fileUrl
    // is its DESTINATION FOLDER, which two volumes of one device legitimately
    // share — deduping on it would silently drop the second volume's grab, the
    // exact bug the per-volume job map already exists to prevent.
    // combineRules is AppSettings.combineRules (passed through from
    // UsbGrabberController, NOT the m_combineRules cache — which is empty until
    // the first combine scan and reset every launch): the walk skips parts
    // whose split-recording group was already combined. See UsbGrabJob.h.
    int enqueueUsbGrab(const UsbGrabRule &rule, const QString &mountPath, const QUrl &destUrl, const QUrl &vaultUrl,
                       bool autoTriggered, const QVariantList &combineRules = {});

    // Item 52: standalone speaker diarization on an ALREADY-transcribed file
    // — adds (or re-runs) speaker detection WITHOUT re-transcribing the audio.
    // Reads the existing sibling .md's segment lines, decodes the audio,
    // runs the SAME diarization core the inline transcribe-time speakerDetection
    // path uses (TranscriptionWorker::runDiarizationPass()), assigns each
    // existing segment its overlapping speaker, and splices the "[Speaker N]"
    // tags + "speakerDetection: true"/"speakers:" frontmatter back into the .md
    // in place (see spliceBodySpeakers()). Queued through the same one-worker
    // job pipeline as transcribe()/relabel() ("diarize" kind), with the same
    // dedup rule (a Queued/Active entry for fileUrl blocks a duplicate) and
    // job-list visibility; emits finished(fileUrl, ok, error) like every other
    // job so the sidebar/main panel refresh. A failure (models unavailable,
    // file not transcribed yet, decode error) IS the job's result, reported via
    // finished(ok=false). Cancel takes effect at the post-diarize checkpoint
    // (diarization has no mid-pass abort hook — see the diarize() lambda in
    // run()). The diarization models are queued as their own download job ahead
    // of this one if not already on disk, same as transcribe()'s
    // speakerDetection path.
    // segmentationModelId/embeddingModelId/sensitivity: the diarization tuning
    // (from AppSettings — see availableSegmentationModels()/the sensitivity
    // slider). Empty ids ⇒ ModelManager defaults; the matching download jobs
    // queue the CHOSEN models ahead of this one.
    Q_INVOKABLE void diarizeFile(const QUrl &fileUrl, const QUrl &vaultUrl,
                                 const QString &segmentationModelId = QString(),
                                 const QString &embeddingModelId = QString(), double sensitivity = 0.5);

    // Bulk wrapper mirroring transcribeAll()/relabelAll(): calls diarizeFile()
    // for each url. Dedup is entirely delegated to diarizeFile(). Added now so
    // the OWED Sidebar ribbon / per-row "detect speakers" buttons (CLAUDE.md
    // per-file-action parity — deferred this wave, another agent owns
    // Sidebar.qml) only need wiring, no new controller method.
    Q_INVOKABLE void diarizeFilesForPaths(const QVariantList &fileUrls, const QUrl &vaultUrl,
                                          const QString &segmentationModelId = QString(),
                                          const QString &embeddingModelId = QString(), double sensitivity = 0.5);

    // Silence phase 1 (backfill): standalone "detect silences" on an
    // ALREADY-transcribed file — reruns VAD (or its transcript-gap fallback,
    // same rule TranscribeTask's inline capture uses) and upserts the
    // "silences:" frontmatter key WITHOUT touching segments/speakers/
    // chapters/label. For files transcribed before this feature landed, or
    // whose transcribe-time VAD pass failed/was unavailable (a subprocess
    // engine). Mirrors diarizeFile(): same dedup rule (Queued/Active entry on
    // fileUrl blocks a duplicate), queued as a DetectSilenceTask (Serial
    // lane — see its class doc comment), reports via the generic finished()
    // signal like every other Serial-lane kind (no dedicated result signal
    // needed, same as DiarizeTask).
    Q_INVOKABLE void detectSilences(const QUrl &fileUrl, const QUrl &vaultUrl);

    // True if fileUrl currently has a Queued or Active entry. Forwards to
    // TaskQuery::isQueued().
    Q_INVOKABLE bool isQueued(const QUrl &fileUrl) const;

    // True if fileUrl's stored transcription (frontmatter) already used
    // modelId — i.e. re-running would reproduce the same result. Used by
    // "transcribe all" (bulk) to skip files that are already current, WITHOUT
    // blocking an explicit single-file manual re-run request (transcribe()
    // itself never checks this — a user clicking Transcribe on one file always
    // re-runs it, regardless of whether the model matches). False for an
    // untranscribed file (nothing to match). Speaker detection is no longer a
    // transcribe-time option (it's the standalone diarizeFile() job), so it's
    // not part of this equality check.
    Q_INVOKABLE bool matchesExistingTranscription(const QUrl &fileUrl, const QString &modelId) const;

    // "Regenerate auto-title" (item 9): re-runs just the AI auto-labeling
    // pass on an already-transcribed file — reads the EXISTING transcript
    // text, does not re-transcribe audio. Queued through the same job/worker
    // infrastructure as transcribe() (dedup, persistent job list) since it's
    // real GPU work; a failure here IS the job's result (there's nothing
    // else for this job to accomplish). transcribe()'s autoLabel option
    // builds this same shape of job directly (see its doc comment) rather
    // than calling this method — a manual "regenerate label" click and an
    // automatic post-transcription label pass are the exact same job kind.
    Q_INVOKABLE void relabel(const QUrl &fileUrl, const QUrl &vaultUrl, bool restrictTagsToExisting = false,
                              double tagReuseBias = 0.5, double wordiness = 0.5);

    // Bulk wrapper mirroring transcribeAll(): calls relabel() for each url.
    // Dedup is delegated to relabel(); the caller (Sidebar's "regenerate all
    // labels" ribbon icon) decides which files are actually transcribed.
    Q_INVOKABLE void relabelAll(const QVariantList &fileUrls, const QUrl &vaultUrl,
                                bool restrictTagsToExisting = false, double tagReuseBias = 0.5,
                                double wordiness = 0.5);

    // Feature 3a ("generate chapters"): a second, SEPARATE LLM pass from
    // relabel() above — sharing the same resident label model/context (see
    // LabelingEngine::generateChapters()'s doc comment), but a deliberately
    // separate, explicit action rather than bundled into relabel(): this is
    // a materially heavier/slower call and shouldn't silently ride along on
    // every "regenerate label" click. Reads the file's ALREADY-parsed
    // segments, proposes chapter boundaries+titles, and splices them into
    // the body as "## [timestamp] title" marker lines (see
    // spliceBodyChapters()'s doc comment) immediately before the segment
    // line each chapter starts at. Queued through the same job pipeline as
    // relabel() — identical dedup-if-already-queued behavior (see its doc
    // comment) and job-list visibility. Regenerating (calling this again on
    // an already-chaptered file) strips the old AUTO marker set first — never
    // accumulates duplicates across repeated runs — but PRESERVES any manual
    // markers the user added/edited (see spliceBodyChapters() / addChapter()).
    // Item 27: density (0.0-1.0, default 0.5) controls how many chapters to
    // propose (see LabelingEngine::generateChapters()); the QML will pass an
    // AppSettings value once its slider lands (owed, later wave) — defaulted
    // here so existing call sites still compile.
    Q_INVOKABLE void generateChapters(const QUrl &fileUrl, const QUrl &vaultUrl, double density = 0.5);

    // Bulk wrapper mirroring relabelAll(): calls generateChapters() for each
    // url. Dedup is entirely delegated to generateChapters().
    Q_INVOKABLE void generateChaptersAll(const QVariantList &fileUrls, const QUrl &vaultUrl, double density = 0.5);

    // Items 24/53 ("run all jobs"): enqueues the FULL per-file pipeline for
    // one file, IN ORDER — transcribe, then detect speakers (diarize), then
    // label (relabel), then chapters — as separate queued jobs so the single
    // background worker runs them strictly in that order (FIFO, one at a time).
    // Because the worker never starts job N+1 until job N finishes, the diarize
    // job reads the .md the transcribe job just wrote (splicing in speaker
    // tags), the label job reads that, and the chapters job reads the labeled
    // .md — no explicit prerequisite tracking needed beyond queue order. Note
    // this deliberately builds and inserts all the jobs itself rather than
    // calling transcribe()/diarizeFile()/relabel()/generateChapters() in
    // sequence: each of those inserts at the FRONT of the queue
    // (frontOfQueueInsertIndex()), so sequential calls would REVERSE the later
    // ones relative to the first; inserting here at one forward-advancing index
    // keeps them contiguous and correctly ordered.
    //
    // This is the one-click "do everything" path that replaced the old inline
    // speaker-detection toggle on transcribe() — Transcribe alone is now pure
    // ASR (see transcribe()), and speaker detection is only ever a separate
    // job (diarizeFile() standalone, or this pipeline's diarize step).
    //
    // Already-transcribed decision (documented per items 24/53): if fileUrl
    // already has a sibling .md, the transcribe step is SKIPPED and only
    // diarize+label+chapters are queued — re-transcribing a file that's already
    // done is the surprising choice; "run all" on a done file just re-detects
    // speakers and refreshes its derived label+chapters. An UNtranscribed file
    // gets all four. (An orphaned .md — passed as its own path — is "already
    // transcribed" by this rule; it too skips transcribe, and its diarize step
    // is a no-op result since there's no audio to decode.)
    //
    // Params mirror the union of transcribe()'s + generateChapters()' inputs
    // (the QML call sites read them from AppSettings / the model combo, same as
    // the individual actions do). No-op if fileUrl already has a Queued/Active
    // entry, same dedup gate as the individual actions (checked once, up front,
    // before any of the jobs is inserted).
    Q_INVOKABLE void runAllJobs(const QUrl &fileUrl, const QUrl &vaultUrl, const QString &modelId,
                                 bool restrictTagsToExisting = false,
                                 double tagReuseBias = 0.5, double wordiness = 0.5, double chapterDensity = 0.5,
                                 const QString &segmentationModelId = QString(),
                                 const QString &embeddingModelId = QString(), double sensitivity = 0.5);

    // Bulk wrapper mirroring transcribeAll()/relabelAll(): calls runAllJobs()
    // for each url. Dedup + ordering are entirely delegated to runAllJobs();
    // the caller (Sidebar's ribbon / search / multi-select) decides which
    // files to run the full pipeline on.
    Q_INVOKABLE void runAllJobsForPaths(const QVariantList &fileUrls, const QUrl &vaultUrl, const QString &modelId,
                                         bool restrictTagsToExisting = false,
                                         double tagReuseBias = 0.5, double wordiness = 0.5,
                                         double chapterDensity = 0.5, const QString &segmentationModelId = QString(),
                                         const QString &embeddingModelId = QString(), double sensitivity = 0.5);

    // Item 17 (manual chapters). Inserts a MANUAL "## [timestamp] title" marker
    // (carrying the manual sentinel — see spliceBodyChapters()'s doc comment)
    // at timestampSeconds, in sorted position (before the first segment line
    // at/after that time). A manual marker is preserved across later
    // generateChapters() regeneration, unlike auto ones. Any existing marker
    // at the same timestamp is replaced. Emits finished(fileUrl, ok, error)
    // so the sidebar/main panel refresh, exactly like setPrelude()/setLabel().
    Q_INVOKABLE void addChapter(const QUrl &fileUrl, double timestampSeconds, const QString &title);

    // Item 17 / item 35 backend. Rewrites the title of the chapter marker at
    // timestampSeconds in place (works for auto OR manual markers). Editing an
    // AUTO chapter's title PROMOTES it to manual (adds the sentinel) so the
    // next regeneration won't clobber the user's edit — the least-surprising
    // behavior. finished ok=false if no marker exists at that timestamp. Emits
    // finished(fileUrl, ok, error).
    Q_INVOKABLE void setChapterTitle(const QUrl &fileUrl, double timestampSeconds, const QString &newTitle);

    // Item 17. Removes the chapter marker at timestampSeconds (auto or manual).
    // finished ok=false if none exists there. Emits finished(fileUrl, ok, error).
    Q_INVOKABLE void deleteChapter(const QUrl &fileUrl, double timestampSeconds);

    // Reads the frontmatter (model/speakerDetection/transcribedAt/label/...)
    // out of a file's sibling .md, if one exists yet. Empty map if not
    // transcribed. If the requested model id wasn't recognized and
    // ModelManager fell back to its default, the map also carries
    // modelFallback (true) and modelRequested (the id that wasn't honored) —
    // model itself always names the file that actually ran. If speaker
    // detection was requested but the diarization models couldn't be
    // downloaded/loaded, speakerDetection reads false (the transcription
    // still ran, ASR-only — see DiarizationEngine) and the map carries
    // speakerDetectionError (why) — MainPanel surfaces this as a warning.
    // Parses every "key: value" frontmatter line generically (not just a
    // fixed allowlist) — any key LabelingEngine or a future writer adds
    // (e.g. "tags") is automatically present here with no extra code. This is
    // how the silence-capture feature's "silences:" key round-trips too (see
    // writeFrontmatter()'s doc comment) — no special-casing needed here,
    // mirroring vad/vadError exactly (see tst_transcript_format.cpp's
    // readMetadata_parsesVadKeys()).
    // Note: the prelude (Feature 2) lives in the BODY, not frontmatter (it
    // needs real newlines) — use readPrelude() for that, not this map.
    // Item O19: cached (see m_frontmatterCache) behind a cheap
    // file-size+mtime check, since this is called on every liveUpdateTick
    // (transitively, via parseSentences()) while a transcription is
    // actively running on the currently-open file.
    Q_INVOKABLE QVariantMap readMetadata(const QUrl &fileUrl) const;

    // {id, label} pairs for the model dropdown (see ModelManager).
    Q_INVOKABLE QVariantList availableModels() const;

    // {id, label, downloaded, nonCommercial} for the diarization SEGMENTATION /
    // EMBEDDING model pickers (Settings → Transcription & AI), and the
    // benchmark's diarization combinations. Parallel to availableModels(); see
    // ModelManager::availableSegmentationModels()/availableEmbeddingModels().
    Q_INVOKABLE QVariantList availableSegmentationModels() const;
    Q_INVOKABLE QVariantList availableEmbeddingModels() const;

    // Settings → Models page: every app-managed model (the whisper tiers +
    // parakeet, kind "transcription"; plus the label / VAD / diarization
    // support models, kind "support"), each entry:
    //   { id, label, kind, downloaded(bool), bundledOnly(bool),
    //     sizeBytes(double), sizeText(string) }
    // bundledOnly ⇒ present only in the read-only bundled dir (Delete must be
    // disabled). Uses the app-global store (no vault dependency).
    Q_INVOKABLE QVariantList manageableModels() const;

    // Predownload a model ahead of use: queues a "download" job (same job kind
    // used lazily before a transcribe) for modelId — a whisper tier,
    // kParakeetModelId, or a support pseudo-id (ModelManager::kLabelModelId /
    // kVadModelId / kDiarizationModelId). No-op if already downloaded/queued.
    Q_INVOKABLE void downloadModel(const QString &modelId);

    // Delete modelId's file(s) from the writable store (ModelManager::
    // deleteModel). Synchronous (an unlink); emits modelOperationFinished
    // ("delete", …) and, on success, modelsChanged().
    Q_INVOKABLE void deleteModel(const QString &modelId);

    // Import model file(s) the user supplied (a file for single-file models, a
    // folder for the parakeet bundle / diarization pair) into the store, so
    // modelId resolves as downloaded with no network. Runs the (possibly
    // multi-GB) copy on a background thread; emits modelOperationFinished
    // ("import", …) and, on success, modelsChanged().
    Q_INVOKABLE void importModel(const QString &modelId, const QUrl &sourceUrl);

    // Export modelId's downloaded file(s) into destDirUrl for backup /
    // re-import (ModelManager::exportModel). Background copy; emits
    // modelOperationFinished("export", …).
    Q_INVOKABLE void exportModel(const QString &modelId, const QUrl &destDirUrl);

    // Capability-detected out-of-process engines (DESIGN.md "Pluggable
    // engines") for the currently-open vault. Each entry:
    //   { id, modelId ("engine:<id>"), label, available(bool),
    //     unavailableReason(string), downloaded(bool) }
    // Grayed-out engines are still listed with a reason (never silently
    // dropped) — same never-misrepresent contract as availableModels()'s
    // `downloaded`. Blocking (probes GPU + runs a deps self-test), so QML
    // should call it off the hot path (e.g. when the benchmark/settings page
    // opens), not per frame.
    Q_INVOKABLE QVariantList availableEngines() const;

    // Model benchmark (Settings "Benchmark" page): runs each id in modelIds
    // over the one file at fileUrl, in memory, writing NO files and touching no
    // queue/vault state (see the worker's benchmarkJob()). modelIds is what to
    // run — the page filters to downloaded whisper tiers + available engines
    // before calling. Runs on the transcription worker thread (serialized with
    // real jobs, so no GPU contention); results arrive via the benchmark*
    // signals above. Uses the currently-open vault (m_vaultUrl), same as
    // availableModels()/availableEngines(). cancelBenchmark() stops it between
    // models / mid-model.
    Q_INVOKABLE void runBenchmark(const QUrl &fileUrl, const QStringList &modelIds);
    // Diarization sweep: combos is a list of {segmentationModelId, embeddingModelId,
    // threshold, label} maps — each run over ONE decode of fileUrl, reporting how
    // many speakers it found (see diarizationBenchmark* signals). Shares the same
    // worker + cancel atomics as runBenchmark(); cancelBenchmark() stops both.
    Q_INVOKABLE void runDiarizationBenchmark(const QUrl &fileUrl, const QVariantList &combos);
    Q_INVOKABLE void cancelBenchmark();

    // Removes one Done/Failed entry matching fileUrl from the job list. A
    // no-op if the only match is still Queued/Active, or if there's no
    // match at all.
    Q_INVOKABLE void dismiss(const QUrl &fileUrl);

    // Removes every Done/Failed entry from the job list; Queued/Active
    // entries are left alone.
    Q_INVOKABLE void clearFinished();

    // Wave N (task system). Pauses the Active job IN PLACE — a no-op unless
    // the Active job's kind is "transcribe" or "download" (the only two
    // kinds with a cooperative interruption point: whisper.cpp's
    // abort_callback, blocked on rather than told to abort — see
    // WhisperCppEngine.cpp's onAbortCheck; and the download job's own
    // byte-polling loop). "relabel"/"rename"/"merge" jobs run to completion
    // uninterrupted either way — this app never shows a pause/cancel control
    // as available for a job it can't actually honor, so QML should gate
    // showing the button on currentJobKind rather than call this
    // unconditionally. Pausing during diarization's sub-phases blocks
    // inside the SAME progress callback DiarizationEngine already calls
    // frequently — no native sherpa-onnx abort hook needed for a pause
    // (only for a genuine mid-diarization CANCEL, which this app does not
    // support — see cancelJob()'s doc comment).
    // ALSO a no-op during the "loading" phase specifically (user
    // correction, 2026-07-09): model loading is one blocking whisper.cpp
    // call with no callback hook at all (unlike decode's abort_callback) —
    // there is no interruption point to pause AT, so silently no-op rather
    // than queue a pause that only takes effect once the job reaches
    // transcribing/diarization later (which would look like the button did
    // nothing, then paused unexpectedly minutes on). QML offers
    // cancelActive() instead during this phase, since loading is fast
    // enough that restarting from scratch is cheap — see BottomBar.qml's
    // pauseButton.
    Q_INVOKABLE void pauseActive();
    // Resumes a paused Active job. No-op if nothing is paused.
    Q_INVOKABLE void resumeActive();
    // Bug report (2026-07-09): "pause during loading doesn't stop it, then
    // pause never works again for the rest of the session". Root cause was
    // a RACE, not a worker bug: BottomBar.qml's pause/play button used to
    // decide pauseActive() vs cancelActive() itself, by reading its OWN
    // (QML-side) copy of currentPhase — which is a queued cross-thread
    // signal one hop behind this object's own m_phase. A click landing
    // right as the job transitions from "loading" into diarization could
    // still see the STALE "loading" value and call cancelActive() instead
    // of the intended pauseActive(). Diarization has no mid-pass abort hook
    // (see pauseActive()'s doc comment on waitWhilePausedOrCancelled() — its
    // return value is deliberately ignored there), so that stray cancel
    // request just sits latched (m_cancelRequested stays true) while the
    // diarization pass runs to completion regardless — LOOKING exactly like
    // "it finished loading and moved on anyway". Worse, once cancelRequested
    // is latched, waitWhilePausedOrCancelled()'s loop condition
    // (pauseRequested && !cancelRequested) can never be true again for the
    // REST OF THAT JOB, so every subsequent pause click silently no-ops.
    //
    // Fix: this is now the ONLY entry point BottomBar.qml's pause/play
    // button calls. It reads m_phase/m_activePaused directly — this
    // object's own authoritative copy, never QML's — so the pause-vs-cancel
    // decision can't race a phase transition it hasn't heard about yet.
    Q_INVOKABLE void togglePauseActive();
    // Cancels whichever job is currently Active, without the caller needing
    // to know its id — convenience mirroring pauseActive()/resumeActive()
    // (see cancelJob()'s doc comment for the immediate-vs-next-checkpoint
    // nuance that still applies). Added specifically for the "loading"
    // phase's cancel-instead-of-pause button (see pauseActive()'s doc
    // comment and BottomBar.qml).
    Q_INVOKABLE void cancelActive();
    // Cancels one job by its stable jobs()-list "id" (NOT fileUrl — a
    // fileUrl can simultaneously name an old Done/Failed history entry and a
    // fresh Queued one after a re-transcribe, so id is the only safe key for
    // a targeted mutation). A Queued job is simply removed — nothing has
    // started, so there is nothing to interrupt. Cancelling the ACTIVE job
    // only takes effect immediately for "transcribe"/"download" kinds (same
    // interruption points pauseActive() uses); for a "relabel"/"rename"/
    // "merge" kind Active job, or if the Active job is stuck inside
    // diarization specifically (no mid-pass abort — see pauseActive()'s doc
    // comment), the cancel is recorded but only takes effect at the next
    // interruption point the job reaches (documented limitation, not a
    // silent gap: the job is marked Failed with error "Cancelled" the
    // moment it actually stops, which may be after diarization's current
    // sub-phase finishes rather than instantly). A no-op if id doesn't match
    // any current Queued/Active entry.
    Q_INVOKABLE void cancelJob(int id);
    // Batch analogues of cancelJob()/dismiss(), for the collapsed batch row's
    // single close button (see collapseBatches()). cancelBatch() cancels every
    // still-pending (Queued/Paused/Active) member of the batch, exactly like
    // cancelJob() per member (same immediate-vs-next-checkpoint caveat);
    // dismissBatch() removes every Done/Failed member of the batch (the
    // batch-wide "clear this finished row"). Both no-op on an unknown batchId.
    Q_INVOKABLE void cancelBatch(int batchId);
    Q_INVOKABLE void dismissBatch(int batchId);
    // Cancels every Queued job, then cancels the Active job exactly like
    // cancelJob() (same immediate-vs-next-checkpoint caveat). Done/Failed
    // history entries are untouched (use clearFinished() for those).
    Q_INVOKABLE void cancelAll();
    // Moves a Queued job to newIndex within the Queued-only ordering (i.e.
    // newIndex counts only among currently-Queued entries, not the full
    // jobs() list which also has Done/Failed/Active rows interleaved) — lets
    // the Tasks popup implement drag-to-reorder without the QML side needing
    // to know how Queued/Active/Done/Failed are interleaved in the
    // underlying list. No-op if id doesn't name a currently-Queued entry.
    // Active/Done/Failed entries are never reorderable (an Active job's
    // position is meaningless to move; history is append-only).
    //
    // Split: the index math (which real index moves where, and the no-op
    // cases) is TaskQuery::planQueuedMove(); what stays here is the actual
    // reorder of the owning vector, the m_activeIndex fixups, and the
    // queueChanged() emission.
    Q_INVOKABLE void moveQueuedJob(int id, int newIndex);

    // Sets/replaces the `label:` frontmatter key on an already-transcribed
    // file's sidecar .md, without disturbing the transcript body below it.
    // Fails (returns false, *errorOut set) if the file hasn't been
    // transcribed yet or the frontmatter block is malformed. Runs
    // synchronously on the calling (GUI) thread — single small file
    // read+write, no model/engine involvement.
    Q_INVOKABLE bool setLabel(const QUrl &fileUrl, const QString &label, QString *errorOut = nullptr) const;

    // Item P15: sets/replaces the `tags:` frontmatter key, same reopen/
    // rewrite shape as setLabel() above. tags is joined with ", " to match
    // the exact comma-separated format collectVaultFrontmatterList()/readMetadata()
    // already parse (auto-label writes this same shape via
    // writeLabelingFields()'s spliceOrAppend) — individual tags must not
    // themselves contain commas.
    Q_INVOKABLE bool setTags(const QUrl &fileUrl, const QStringList &tags, QString *errorOut = nullptr) const;

    // Persists which chapters are collapsed in the panel's view, as a
    // `collapsedChapters:` frontmatter key holding the comma-joined chapter t0
    // keys (the same String(t0) form MainPanel keys its collapse map by), so
    // collapse survives closing/reopening a file. Same reopen/rewrite shape as
    // setLabel(); an empty list REMOVES the key rather than leaving a blank
    // one. Deliberately does NOT emit finished() — collapse is pure view state
    // no listener needs to react to, so a toggle mustn't trigger a metadata/
    // transcript reload cycle. Runs synchronously on the GUI thread.
    Q_INVOKABLE bool setCollapsedChapters(const QUrl &fileUrl, const QStringList &chapterKeys, QString *errorOut = nullptr) const;

    // Manual-edit counterpart for the transcript BODY's segment text itself
    // (MainPanel's always-editable transcriptArea) — edits is a list of
    // {t0: seconds as double, text: QString} maps, one per changed sentence
    // (QML only sends the ones that actually differ, computed by diffing
    // against displaySentences). Same splice-by-captured-offset technique as
    // renameSpeakerInFile()/mergeSpeakersInFile() (TranscriptionController.cpp),
    // just targeting segmentLinePattern()'s text capture group (3) instead of
    // the speaker group (2) — timestamp/speaker tag/repeat-count marker/
    // SPEAKER_TURN suffix are all left byte-for-byte untouched. Fails (no
    // partial write) if ANY requested t0 doesn't match an existing segment
    // line — same all-or-nothing shape as the other setters above.
    Q_INVOKABLE bool setSentenceTexts(const QUrl &fileUrl, const QVariantList &edits, QString *errorOut = nullptr) const;

    // Feature 2: the multi-paragraph, editable "prelude" (upgrades the old
    // read-only single-line "summary") — stored in the .md BODY immediately
    // after frontmatter, terminated by the "<!-- prelude end -->" sentinel
    // (see spliceBodyPrelude()'s doc comment in the .cpp for why an explicit
    // marker rather than implicit boundary detection). Returns everything
    // before that marker, trimmed, or an empty string if the marker isn't
    // present (untranscribed file, or one predating this feature).
    Q_INVOKABLE QString readPrelude(const QUrl &fileUrl) const;

    // Manual-edit counterpart to setLabel() above, same reopen/rewrite shape
    // (frontmatter preserved byte-for-byte) — replaces just the prelude
    // region of the body via the same splice helper writeLabelingFields()
    // uses for auto-regeneration, so a later relabel() and a manual edit
    // here produce identically-formatted output. Per Feature 2's
    // regeneration semantics, a later auto-relabel unconditionally
    // overwrites whatever was set here — same precedent as a manual title
    // rename (setLabel()) already being clobbered by relabel() today.
    Q_INVOKABLE bool setPrelude(const QUrl &fileUrl, const QString &prelude, QString *errorOut = nullptr) const;

    // Feature 3a: the read-side counterpart to generateChapters() above —
    // scans the body for "## [timestamp] title" marker lines (see
    // spliceBodyChapters()'s doc comment) and returns them as
    // {t0: seconds as double, title: QString} maps, in file order (== time
    // order, since a chapter marker is always inserted immediately before
    // the segment line it introduces). Empty list if the file has no
    // chapters yet (untranscribed, or generateChapters() never run).
    Q_INVOKABLE QVariantList readChapters(const QUrl &fileUrl) const;

    // Rewrite (2026-07-07): a speaker's name is now baked directly into
    // every "[oldName]" segment-line tag in the transcript BODY itself —
    // there is no more separate frontmatter name-lookup layer, since the
    // vault's .md file is meant to be human-readable standalone (CLAUDE.md);
    // a raw file that still showed "[Speaker 1]" everywhere after a rename,
    // with the real name hidden in YAML frontmatter, wasn't. oldName must be
    // the speaker's CURRENT literal tag text — the same string
    // parseSentences()' "speaker" field already returns (e.g. "Speaker 1",
    // or a previously-set custom name once already renamed once).
    //
    // Runs asynchronously, queued through the SAME job pipeline as
    // transcribe()/relabel() above (a full-body rewrite is real file IO) —
    // this call itself returns immediately, and the caller is expected to
    // update its own display optimistically and reconcile once
    // speakerRenamed() fires (see MainPanel.qml's pendingSpeakerRenames).
    // Being a real job means it shows up in jobs()/tasksTotal/tasksCompleted
    // like any other task, queues behind an active job for the SAME file
    // instead of racing it, and runs concurrently-in-effect with (queued
    // after, not blocked by) jobs for OTHER files. Rejects (speakerRenamed's
    // ok=false) if newName (trimmed, case-insensitively) already names a
    // DIFFERENT speaker present in the file, if oldName isn't actually
    // present, or if fileUrl already has a Queued/Active job (same dedup
    // rule as transcribe(), reported back via speakerRenamed instead of
    // silently dropped, since a caller here is actively waiting for a
    // definitive result).
    Q_INVOKABLE void renameSpeaker(const QUrl &fileUrl, const QString &oldName, const QString &newName);

    // Merges sourceName into targetName: every "[sourceName]" body tag
    // becomes "[targetName]", and sourceName drops out of distinctSpeakerIds
    // entirely (unlike renameSpeaker() above, colliding with an existing
    // name is the whole point here — see mergeSpeakersInFile()'s doc
    // comment in the .cpp). Queued through the same job pipeline as
    // renameSpeaker(), with the same async/dedup/optimistic-UI contract:
    // runs on the background thread, rejects (speakersMerged's ok=false) if
    // fileUrl already has a Queued/Active job, and the caller is expected to
    // update its own display optimistically and reconcile once
    // speakersMerged() fires.
    Q_INVOKABLE void mergeSpeakers(const QUrl &fileUrl, const QString &sourceName, const QString &targetName);

    // Renames a recording file (+ its sibling .md) as an instant-lane task —
    // visible in the Tasks list, run off the GUI thread, and deferred behind a
    // transcribe of THIS SAME file if one is active (renaming would move the
    // .md out from under it). Reports via fileRenamed(); QML repoints the
    // player to newUrl on success and rolls back its optimistic field on
    // failure. Replaces QML's old synchronous VaultTreeModel::renameFile()
    // calls (that method stays for direct callers/tests).
    Q_INVOKABLE void renameFile(const QUrl &fileUrl, const QString &newBaseName);

    // Optimistic pending renames: a file rename is an instant-lane task DEFERRED
    // behind a same-file transcription, so its fileRenamed (which repoints panels
    // / updates the tree) can be a whole job away. renameFile() records the new
    // stem here synchronously so every view shows the new name AT ONCE; the entry
    // is erased when fileRenamed lands (ok or fail). Returns the pending new stem
    // for fileUrl, or "" if none. NOTE: read pendingRenamesRevision in the same
    // binding or this subscribes to nothing (bare Q_INVOKABLE ⇒ no dependency).
    Q_INVOKABLE QString pendingRenameStem(const QUrl &fileUrl) const;
    int pendingRenamesRevision() const { return m_pendingRenamesRevision; }

    // Moves one or more recording files (each carrying its .md sidecar +
    // .partial.jsonl resume state) into destDirUrl, as instant-lane tasks —
    // one MoveTask per file, all in ONE undo batch (a single "Moved N files"
    // snackbar; see enqueueBatch()/undoTask()). vaultUrl bounds the
    // inside-vault check MoveTask enforces on both ends. Each task defers
    // behind an active transcribe of its OWN file (moving the .md out from
    // under it), exactly like renameFile(). Reports per file via fileMoved();
    // QML repoints the player when the currently-open file is the one moved.
    // Replaces QML's old synchronous VaultTreeModel::moveFile() loop (that
    // method stays for direct callers/tests / the no-controller fallback).
    Q_INVOKABLE void moveFiles(const QVariantList &fileUrls, const QUrl &destDirUrl, const QUrl &vaultUrl);

    VaultTreeModel *vaultTreeModel() const { return m_vaultModel; }
    void setVaultTreeModel(VaultTreeModel *model);

    // Moves vault entries to the trash as an instant-lane TrashTask — the async,
    // undoable replacement for QML's synchronous VaultTreeModel::trashFiles()/
    // trashTranscript()/trashAudioOnly()/trashFolder() (which stay as the
    // no-controller fallback). `scope` is "files" | "transcript" | "audioOnly" |
    // "folder"; urls is the selection (a single folder url for "folder").
    // vaultUrl bounds the containment guard. The whole batch is ONE task ⇒ ONE
    // generic undo snackbar (undoAvailable → RestoreTask). Entries the filesystem
    // refuses come back via trashResult() to drive the confirm-then-permanent
    // flow. The tree refreshes via the model's file-watcher + applyRemovalOutcome().
    Q_INVOKABLE void trash(const QString &scope, const QVariantList &urls, const QUrl &vaultUrl);
    // The irreversible counterpart, only ever called (from QML) for the entries
    // trashResult() reported as untrashable, after the user confirms. Same scope
    // strings; a DeleteTask, not undoable.
    Q_INVOKABLE void deletePermanently(const QString &scope, const QVariantList &urls, const QUrl &vaultUrl);

    // Renames / moves a FOLDER as an instant-lane task (FolderRenameTask /
    // FolderMoveTask) — the folder analogues of renameFile()/moveFiles(),
    // undoable via the generic snackbar. Pure filesystem (no per-file model
    // signals, unlike trash/delete — the tree refreshes via the watcher).
    // vaultUrl bounds the guards. The synchronous VaultTreeModel::renameFolder()/
    // moveFolder() stay as the no-controller fallback. Reports via
    // folderRenamed()/folderMoved() so QML can surface an error.
    Q_INVOKABLE void renameFolder(const QUrl &folderUrl, const QString &newName, const QUrl &vaultUrl);
    Q_INVOKABLE void moveFolder(const QUrl &folderUrl, const QUrl &destParentUrl, const QUrl &vaultUrl);

    // Undo a completed task OR batch: `token` is either a single Done task's id
    // (enqueues that task's inverse) or a batchId (enqueues the inverse of
    // every Done member of the batch, itself as a fresh one-snackbar-undoable
    // batch). No-op if the token matches no undoable Done task/batch. The undo
    // snackbar the UI shows (on undoAvailable) calls this.
    Q_INVOKABLE void undoTask(int token);

    // Everything after the frontmatter's closing "---" line (i.e. the
    // segment text), for main-panel search. Empty string if untranscribed
    // or unreadable. Item O19: cached (see m_bodyCache) behind a cheap
    // file-size+mtime check, since this is called on every liveUpdateTick
    // while a transcription is actively running on the currently-open file.
    Q_INVOKABLE QString readTranscriptBody(const QUrl &fileUrl) const;

    // Parses readTranscriptBody()'s "[HH:MM:SS.mmm] [tag] text" lines (see
    // SegmentLineWriter/formatTimestamp) into a list of {t0: seconds as
    // double, speaker: QString (empty = no speaker detection), text:
    // QString, start, length: int, repeatCount: int}, in file order (which
    // is also time order — segments are appended as whisper produces them).
    // "speaker" is the literal bracketed tag text as stored in the file —
    // "Speaker N" by default, or whatever a user has renamed it to (see
    // renameSpeaker() above), never translated through a separate lookup.
    // No "t1": it's never stored (always identical to the next segment's
    // t0) and nothing consumes it. start/length are the character range of
    // text within THIS SAME readTranscriptBody() string, so a caller can
    // span-highlight the active sentence without re-deriving offsets.
    // repeatCount is 1 for a normal line, or N when SegmentLineWriter merged
    // N consecutive identical (same speaker, same text) whisper segments
    // into this one line with a trailing " (×N)" marker — the merge happens
    // at write time, in the file itself, not just in this parsed view. For
    // sentence-level highlighting synced to playback (see DESIGN.md /
    // CLAUDE.md "sentence-level only, binary-search on positionChanged").
    // Item O19: cached (see m_sentenceCache) — an append-only live
    // transcription only regex-parses its NEW trailing bytes per call
    // instead of re-parsing the whole (ever-growing) body from scratch.
    Q_INVOKABLE QVariantList parseSentences(const QUrl &fileUrl) const;

    // Item P23: cold-opening a large already-transcribed file used to block
    // the GUI thread for the entire readTranscriptBody()+parseSentences()
    // pass (a single regex sweep over the whole body, cost scaling with
    // file size) — this runs that SAME pair of (cached) calls on a
    // throwaway background QThread instead, and emits transcriptLoaded()
    // with the result once done. Thread-safe with respect to the GUI
    // thread's own synchronous calls to readTranscriptBody()/parseSentences()
    // (e.g. the existing liveUpdateTick-driven polling of whatever file is
    // ACTIVELY transcribing) via m_transcriptCacheMutex — see its doc
    // comment. Fire-and-forget: callers (QML) are expected to track which
    // fileUrl they're waiting on themselves and ignore a transcriptLoaded()
    // whose fileUrl no longer matches the file currently open (the user may
    // have switched files again before this one's background pass finished).
    Q_INVOKABLE void loadTranscriptAsync(const QUrl &fileUrl);

    // Binary-searches sentences (as returned by parseSentences(), sorted by
    // t0) for the one active at positionSeconds. Returns the index of the
    // last sentence whose t0 <= positionSeconds — i.e. "sticky": a position
    // in the silence gap between sentence i's t1 and sentence i+1's t0 still
    // reports i, so the highlight doesn't blank out between segments.
    // Returns -1 if positionSeconds is before the first sentence's t0 (or
    // the list is empty). Two kinds of zero-length pseudo-entries can be
    // interleaved into `sentences` (chapter headings, and — silence phase 3
    // — silence-divider markers, both flagged rather than spoken content);
    // this walks back over either to the previous REAL sentence so the
    // sentence-level highlight (DESIGN.md) never lands on pure decoration,
    // with one exception: a landed-on COLLAPSED chapter heading, which is
    // the sole visible line for its (hidden) body.
    Q_INVOKABLE int activeSentenceIndex(const QVariantList &sentences, double positionSeconds) const;

    // Silence phase 2 (skip-silence during playback): inverse of
    // formatSilencesValue() — parses a frontmatter "silences: 12.4-18.9,
    // 45-52.3" raw value (as returned by readMetadata()'s generic
    // key/QString map — see MainPanel.qml's mainPanel.metadata.silences)
    // back into a QVariantList of {t0, t1} maps, the same shape convention
    // parseSentences()'s sentence maps use, so nextSilenceSkipTarget() below
    // (and activeSentenceIndex()'s existing idiom) can read spans via
    // toMap().value("t0")/"t1". An empty/missing raw value yields an empty
    // list (VAD hasn't run yet, or found no silence).
    Q_INVOKABLE QVariantList parseSilences(const QString &raw) const;

    // Pure, deterministic lookup over ALREADY-PARSED spans (parseSilences()
    // above) — this is what makes "changing the threshold" a UI filter and
    // NEVER a re-analysis trigger (locked Decision, queue item 084): no
    // audio, VAD, or task involvement whatsoever, just a scan of the spans
    // already stored in frontmatter. Returns the END of the first span
    // containing positionSeconds whose length is >= thresholdSeconds, or -1
    // (sentinel: nothing to skip) when positionSeconds isn't inside a
    // skip-worthy span. Used by MainPanel.qml's player.positionChanged
    // handler, gated on AppSettings.skipSilenceEnabled.
    Q_INVOKABLE double nextSilenceSkipTarget(const QVariantList &silences, double positionSeconds,
                                              double thresholdSeconds) const;

    // Wave N (task system, item 1): loads `<vaultUrl>/.transcriber/tasks.json`
    // (written by this same class — see persistQueue()) if present, and
    // re-enqueues every entry as a fresh Queued job (see
    // enqueueReplayedJob()). Also remembers vaultUrl as the destination for
    // subsequent persistQueue() calls (every queueChanged() from here on
    // saves back to this vault until the next loadPersistedQueue() call,
    // i.e. the next vault open). Meant to be called once per vault open
    // (Main.qml's onVaultPathChanged, alongside the existing
    // AppSettings.loadFromVault call). A job that was Active (mid-
    // transcription) when the app last closed is persisted and reloaded as
    // Queued, NOT resumed from where it left off — no true mid-file resume
    // exists yet (see CLAUDE.md / DESIGN.md "Durable / resumable
    // transcription").
    //
    // User correction (2026-07-09): restored jobs are deliberately NOT
    // auto-started, unlike a live transcribe()/relabel() call — they just
    // sit Queued (visible in jobs()/the Tasks popup, reorderable/
    // cancellable like any other Queued entry) until the user explicitly
    // resumes via startNext() (QML's play button). "Saved between
    // sessions", not "auto-resumed".
    Q_INVOKABLE void loadPersistedQueue(const QUrl &vaultUrl);

    // Wave N (task system, user correction 2026-07-09): starts the
    // front-most Queued job — the "play" button QML shows in place of the
    // pause button whenever nothing is Active but the queue isn't empty
    // (most commonly right after loadPersistedQueue() restored one).
    // No-op if something is already Active, or the queue has no Queued
    // entry at all.
    Q_INVOKABLE void startNext();

    // Bug report (2026-07-09): closing the GUI window must never leave a
    // transcription silently running in the background. The natural fix —
    // cancel the Active job, then wait for the worker thread in
    // ~TranscriptionController() — is real and correct, but its RESPONSE
    // TIME is bounded by however long the vendored sherpa-onnx diarization
    // code takes to reach its next progress-callback checkpoint, which
    // batches 32 windows/embeddings per GPU call before checking in at all
    // (see offline-speaker-diarization-pyannote-impl.h's
    // kSegmentationBatchSize/kEmbeddingBatchSize) — measured up to ~9s in
    // practice, long enough to look exactly like "still running in the
    // background".
    //
    // This sidesteps that entirely: rather than waiting for native code to
    // reach a checkpoint, it ends the OS process immediately. Provably safe
    // per this app's own durability design — CLAUDE.md/SegmentLineWriter's
    // doc comment: every segment append() already leaves the .md file fully
    // valid and flushed, BY DESIGN, specifically so a kill mid-run (this app
    // has never guaranteed anything more graceful than that) never
    // corrupts it. The one accepted risk is losing a vault settings change
    // made in the last ~250ms before closing (AppSettings.qml's
    // saveToVault() is debounced that long) — the exact same risk this
    // app's Ctrl+C shutdown path already carries today, just extended to
    // the window's close button too, not a new category of risk.
    // Deliberately NOT a graceful QCoreApplication::quit() — that still
    // waits on the same slow-to-interrupt native call underneath.
    Q_INVOKABLE void terminateApplicationImmediately() const;

    // Item 17: de-duplicated, sorted list of every tag already used anywhere
    // in the vault — feeds the sidebar's tag-input autocomplete. Same scan
    // LabelingEngine's "restrict to existing tags" mode already uses
    // internally (collectVaultFrontmatterList, TranscriptionController.cpp); exposed
    // here so QML doesn't need its own duplicate frontmatter-scanning logic.
    //
    // GUI THREAD ONLY — it consults m_vaultTagFileCache, which is not
    // mutex-guarded (unlike the transcript caches). The labeling job's own
    // call to collectVaultFrontmatterList() runs on the worker thread and deliberately
    // passes no cache, so the two never share state.
    //
    // Backed by that per-file cache because Sidebar.qml's tagSuggestions()
    // calls this from a BINDING (the suggestion ListView's `model`), so it
    // re-runs on every keystroke in the tag filter field — and the underlying
    // scan opens and frontmatter-parses every .md in the vault. The cache
    // keeps the directory walk (a stat per file, cheap) but skips the open +
    // parse for any file whose mtime+size is unchanged, the same correctness
    // argument VaultTreeModel's two caches use.
    Q_INVOKABLE QStringList existingVaultTags(const QUrl &vaultUrl) const;

    // Every distinct speaker name used anywhere in the vault — the speaker
    // equivalent of existingVaultTags() above, backing the sidebar's speaker
    // filter autocomplete. Same contract in every respect (GUI THREAD ONLY,
    // per-file mtime+size cache, deduped, case-insensitively sorted), just
    // over the `speakers:` frontmatter key instead of `tags:`; see that
    // function's doc comment for the full rationale.
    //
    // Frontmatter is the right source, not a body scan: writeFrontmatter()
    // only emits `speakers:` when speaker detection ran, and
    // syncSpeakersFrontmatterToBody() keeps that line equal to the speaker
    // tags actually present in the body — so the line is authoritative and
    // costs one parse instead of a full-body regex pass per file.
    Q_INVOKABLE QStringList existingVaultSpeakers(const QUrl &vaultUrl) const;

    // One .md file's parsed values for ONE frontmatter key (`tags:` or
    // `speakers:`) plus the stat they were read at. Public only so the
    // file-local collectVaultFrontmatterList() helper can name it; the caches
    // themselves (m_vaultTagFileCache/m_vaultSpeakerFileCache) are private.
    // Per-path, unlike the three single-slot transcript caches below, since
    // each scan touches every .md in the vault on each call. One cache per
    // key — a single shared cache would key-collide, since two different keys
    // yield different values for the same unchanged file.
    struct CachedFileValues {
        qint64 mtimeMs = 0;
        qint64 size = -1;
        QStringList values;
    };

    // The split-recording combine DETECTION subsystem moved to CombineScanner
    // (2026-07-20) — see its class doc comment. These three stay here as the
    // Q_INVOKABLE names QML (SettingsPopup.qml's combine page, Sidebar.qml's
    // group outlines) and tests already spell; all three are pure forwarders
    // to the canonical definitions on CombineScanner, where their full doc
    // comments now live. The queue glue below (runCombineScan()/
    // combineGroup() and their private helpers) is what did NOT move: it
    // mutates m_tasks.
    Q_INVOKABLE QVariantList detectCombineGroups(const QVariantList &rules, const QStringList &audioPaths) const;
    Q_INVOKABLE QString regexValidationError(const QString &pattern) const;
    Q_INVOKABLE QStringList differentiatorNonParticipatingFiles(const QString &pattern, int groupIndex,
                                                                  const QStringList &audioPaths) const;

    // Runs detectCombineGroups() and enqueues a "combine" job (Job::combineOnly)
    // for every "ready" group not already queued (isCombineQueued()) — never
    // also queues a transcribe job; the merged file just becomes a normal
    // untranscribed vault entry the user transcribes like any other. Called
    // from Sidebar.qml's ribbon "combine" button — a deliberate, user-
    // initiated scan, not an automatic one (see the button's own doc
    // comment for why: silently merging on every file-add was a surprise
    // recorders shouldn't spring on the user without asking).
    Q_INVOKABLE void runCombineScan(const QVariantList &rules, const QStringList &audioPaths, const QUrl &vaultUrl,
                                     bool deleteOriginals);

    // Sidebar per-row "combine this group" hover button (item 51): same
    // matching/enqueue machinery as runCombineScan() above but scoped to the
    // ONE group whose outputPath matches — see runCombineScan()'s doc
    // comment for why the ribbon button's "scan everything ready" behavior
    // is deliberately not what a single-row action should do. No-ops
    // (returns silently) if that group is no longer "ready" or already
    // queued by the time this runs.
    Q_INVOKABLE void combineGroup(const QVariantList &rules, const QStringList &audioPaths,
                                   const QString &outputPath, const QUrl &vaultUrl, bool deleteOriginals);

signals:
    // Model-benchmark signals (see runBenchmark()), re-emitted from the worker
    // on the GUI thread so QML can react directly. benchmarkModelStarted per
    // model (index/total for a progress bar); benchmarkModelDone per finished
    // result row; benchmarkFinished once with the whole run (the Benchmark page
    // persists it via VaultTreeModel::writeBenchmarkLastRun); benchmarkFailed if
    // it couldn't start (e.g. decode failed).
    void benchmarkModelStarted(const QString &modelId, int index, int total);
    void benchmarkModelDone(const QVariantMap &result);
    // Live partial transcript for the model at index `modelIndex`, re-emitted on
    // the GUI thread so the Benchmark page grows the running card as segments
    // stream in (before benchmarkModelDone).
    void benchmarkModelProgressText(int modelIndex, const QString &partialText);
    void benchmarkFinished(const QVariantMap &run);
    void benchmarkFailed(const QString &error);
    // Diarization sweep (see runDiarizationBenchmark()) — per-combo start, per
    // finished combo (result carries speakerCount/turnCount/runtimeMs/
    // realtimeFactor + the combo's seg/embed/threshold), and once with the run.
    // A decode failure before any combo reuses benchmarkFailed.
    void diarizationBenchmarkComboStarted(const QString &label, int index, int total);
    void diarizationBenchmarkComboDone(const QVariantMap &result);
    void diarizationBenchmarkFinished(const QVariantMap &run);

    // Settings → Models page: modelsChanged fires when the set of downloaded
    // models changes (delete/import success, or a predownload job finishing) so
    // the page re-queries manageableModels(). modelOperationFinished reports the
    // result of a delete/import/export (operation is "delete"/"import"/
    // "export"), letting the page show success or the error.
    void modelsChanged();
    void modelOperationFinished(const QString &operation, const QString &modelId, bool ok, const QString &error);

    void busyChanged();
    void progressChanged();
    void overallProgressChanged();
    void activeIsForegroundChanged();
    void currentJobKindChanged();
    void currentPhaseChanged();
    void currentFileChanged();
    void queueChanged();
    // Item P6: fired whenever the path->JobStatus index (m_jobStatusByPath)
    // actually changes — i.e. on every queueChanged() that changes at least
    // one path's resolved status, see rebuildJobStatusIndex(). QML's file
    // tree delegates connect to this to know when to re-poll
    // jobStatusForPath() for their own path, without depending on the
    // (unrelated-to-them) full jobs() list shape.
    void jobStatusesChanged();
    // Item P23: see loadTranscriptAsync()'s doc comment. Always emitted on
    // the GUI thread (the background QThread marshals back via
    // QMetaObject::invokeMethod/Qt::QueuedConnection) — safe for QML to
    // react to directly, no cross-thread concerns for the receiver.
    void transcriptLoaded(const QUrl &fileUrl, const QString &body, const QVariantList &sentences);
    void liveUpdateTickChanged();
    void activePausedChanged();
    void activeStoppingChanged();
    void activeBackendNameChanged();
    // const: also emitted from setLabel()/setPrelude()/setTags() (item P19),
    // which are themselves const (pure file-rewrite helpers, no member state
    // touched) — Qt signals may be declared const, and doing so here lets
    // those synchronous, non-queued edit paths notify the sidebar the same
    // way the queued-job paths (relabel()/generateChapters()/etc, all
    // non-const) already do via onWorkerFinished().
    //
    // Bug fix ("label always delayed by one file in the file explorer"):
    // every in-place sidecar .md rewrite this class does (auto-label,
    // "regenerate label", "generate chapters", speaker rename/merge, and
    // the setLabel()/setTags()/setPrelude()/setSentenceTexts() editors)
    // already emits this, with fileUrl naming the file that changed — a
    // listener wired to this signal is expected to call
    // VaultTreeModel::refreshFile(fileUrl) so that file's row re-reads its
    // now-current metadata immediately. Without that call, VaultTreeModel
    // has no other way to learn about an in-place content rewrite:
    // QFileSystemWatcher::directoryChanged (which VaultTreeModel::rebuild()
    // is otherwise driven by) only fires on a directory's file add/remove/
    // rename, never on an existing file's content changing — see
    // VaultTreeModel::refreshFile()'s doc comment for the full explanation.
    //
    // `structural` is the finishing task's Task::isStructuralFileOp() — true
    // when it MOVED files, so the vault watcher's rebuild will emit a full
    // modelReset that reflows the tree authoritatively. A listener that also
    // reflows on this signal (Sidebar's refreshTreeOrdering/refreshSearchResults)
    // must SKIP that work when structural is true, or the panel reflows twice
    // for one rename. Defaults to false, which is the correct value for every
    // emit that isn't a task completion (an in-place edit like setLabel, or a
    // cancel): those never produce a reset, so `finished` is their only reflow
    // trigger and it must keep doing that work.
    void finished(const QUrl &fileUrl, bool ok, const QString &error, bool structural = false) const;
    // Fired when a renameSpeaker() call's background rewrite completes — IN
    // ADDITION to the generic finished() above (a rename is queued through
    // the SAME job queue as transcribe/relabel, see renameSpeaker()'s doc
    // comment, so it participates in busy/progress/tasksTotal/jobs() too).
    // oldName is echoed back (not just fileUrl) so a caller tracking
    // multiple speakers/optimistic renames at once (see MainPanel.qml's
    // pendingSpeakerRenames) knows exactly which one this result belongs to.
    void speakerRenamed(const QUrl &fileUrl, const QString &oldName, bool ok, const QString &error);
    // Fired when a renameFile() task completes. oldUrl is the original file;
    // newUrl is the renamed primary file (empty on failure). QML repoints the
    // player to newUrl on success, rolls back its optimistic filename field on
    // failure.
    void fileRenamed(const QUrl &oldUrl, const QUrl &newUrl, bool ok, const QString &error);
    // Bumped whenever the pending-rename table changes (a rename enqueued, or its
    // task completing/failing). Views read pendingRenameStem() gated on this so
    // the read is reactive — see pendingRenameStem()'s doc comment.
    void pendingRenamesRevisionChanged();
    // Fired per file when a moveFiles() task completes. oldUrl is the file's
    // original location; newUrl is its new location (empty on failure). QML
    // repoints the player to newUrl when the moved file was the open one.
    void fileMoved(const QUrl &oldUrl, const QUrl &newUrl, bool ok, const QString &error);
    // Fired when a trash() task completes. `scope` echoes the request
    // ("files"/"transcript"/"audioOnly"/"folder"); `untrashable` is the url
    // strings the filesystem refused (STILL on disk, untouched). QML opens the
    // confirm-then-permanent-delete prompt for exactly these. The undo snackbar
    // for whatever DID go to the trash is the generic undoAvailable, not this.
    void trashResult(const QString &scope, const QVariantList &untrashable);
    // Fired when a renameFolder()/moveFolder() task completes. newUrl is the
    // folder's new location (empty on failure); QML surfaces the error on !ok.
    void folderRenamed(const QUrl &oldUrl, const QUrl &newUrl, bool ok, const QString &error);
    void folderMoved(const QUrl &oldUrl, const QUrl &newUrl, bool ok, const QString &error);
    void vaultTreeModelChanged();
    // Emitted when an UNDOABLE task or batch finishes. `token` is the task id
    // for a standalone undoable task, or the batchId for a multi-select batch
    // (ONE snackbar for the whole set — see maybeEmitUndoAvailable()). The UI
    // shows an undo snackbar labeled `label`; clicking Undo calls
    // undoTask(token). Auto-wired for every undoable kind — no per-kind QML.
    void undoAvailable(int token, const QString &label);
    // Fired when a mergeSpeakers() call's background rewrite completes — see
    // speakerRenamed()'s doc comment above for the shape/rationale, mirrored
    // here with both names (sourceName is echoed back so a caller tracking
    // several in-flight merges knows exactly which one this result belongs
    // to, targetName rounds out the pair for symmetry/debugging).
    void speakersMerged(const QUrl &fileUrl, const QString &sourceName, const QString &targetName, bool ok,
                         const QString &error);
    // Fired when an enqueueUsbGrab() job's scan+copy completes — IN ADDITION
    // to the generic finished() above (a grab is a normal queued job, so it
    // participates in busy/progress/tasksTotal/jobs() like every other kind).
    //
    // Exists because finished(fileUrl, ok, error) can't carry a grab's actual
    // result: "copied 4, skipped 118, 1 error" is not ok/error-shaped, and the
    // history entry + snackbar both need the counts. jobId is echoed back (not
    // just the serial) because one device's two volumes are two concurrent
    // grabs under one serial — the serial alone can't tell them apart, which
    // is the same reason the in-flight job map is keyed per-volume.
    //
    // cancelled distinguishes a grab the user stopped from one that ran to the
    // end of the device: the counts are a partial tally in that case, so it
    // must not be recorded or announced as a completed grab.
    void usbGrabFinished(int jobId, int filesCopied, int filesSkipped, const QStringList &errors, bool cancelled);

private:
    // Wave N (task system): the Job struct, the DownloadTarget enum, and
    // jobKind() that used to live here are GONE. A queued unit of work is now
    // a polymorphic Task (src/core/tasks/Task.h) — one subclass per kind,
    // owning its own doing-code, result signals, row fields, capabilities,
    // presentation, persistence opt-in, and model prerequisites. See
    // .task/plan.md: the 30-field union + 9 mutually-exclusive bool flags +
    // the order-dependent jobKind() waterfall + the 10-branch startJob()
    // dispatch were the reason a kind's knowledge lived in five disjoint
    // places and one could silently be forgotten.


    // PHASE Q: the queue MECHANICS (enqueue/pump/start/finish, the cancel/
    // dismiss/undo/combine machinery, the two worker threads, and the pause/
    // cancel INTENT) moved WHOLESALE to TaskScheduler (see its header). This
    // class now implements TaskSchedulerObserver: the scheduler calls back
    // through the observer overrides below at the exact points display state
    // should update, so the golden emission ORDER is preserved, and the
    // controller re-emits the scheduler's queueChanged as its own (which keeps
    // driving persistQueue/rebuildJobStatusIndex + the QML NOTIFY). Every queue
    // Q_INVOKABLE stays a thin forwarder to m_scheduler.

    // --- TaskSchedulerObserver overrides (see TaskScheduler.h for the exact
    // contract each fulfills). All display work + all typed emits live here; the
    // scheduler owns none of it. ---
    void onSerialTaskStarting(Task *task, bool wasCancelling) override;
    void onInstantTaskStarting(Task *task) override;
    void onSerialActiveEnded(bool wasStopping) override;
    qint64 onSerialTaskTerminal(Task *task, bool ok, const QString &error) override;
    void onSerialActiveCleared(bool wasCancelling) override;
    void onQueueDrained() override;
    void reportSerialFinished(bool ok, const QString &error, bool structural, bool threeArg) override;
    void reportInstantFinished(Task *task, const QUrl &finishedFile, bool ok, const QString &error) override;
    void emitUndoAvailable(int token, const QString &label) override;
    // Wrinkle 1: keep ModelManager + DownloadTask construction here (out of the
    // scheduler). Forwards to maybeQueueDownloadTask(); the download is inserted
    // via the scheduler's insertResolvedDownload().
    int resolveDownloadPrerequisite(ModelDownloadTarget target, const QUrl &vaultUrl, const QString &modelId,
                                    int insertIndex) override;
    // Forget a task the scheduler is erasing, from the connectTask wired-once
    // guard (m_connectedTasks) — heap reuse would otherwise falsely read as
    // already-connected.
    void onTaskRemoved(Task *task) override;
    // The cancel-request set-transition (cancelJob/cancelAll) crosses the seam
    // here — re-emit activeStoppingChanged.
    void onActiveStoppingChanged() override { emit activeStoppingChanged(); }
    // cancelTaskAt()'s synthesized terminal for a task cancelled before it ran.
    void reportCancelledBeforeRun(const QUrl &fileUrl) override
    {
        emit finished(fileUrl, false, QStringLiteral("Cancelled"));
    }

    // Item P6: rebuilds m_jobStatusByPath from scratch off the current
    // m_jobs, emitting jobStatusesChanged() iff the resolved per-path status
    // actually changed. Connected to queueChanged() in the constructor
    // (alongside persistQueue()) rather than called individually from every
    // m_jobs-mutating site — every mutation (enqueue, startJob(),
    // onWorkerFinished(), cancelJob()/cancelAll(), dismiss()/clearFinished(),
    // moveQueuedJob(), the replay path) already emits queueChanged(), so this
    // one connection keeps the index current everywhere a targeted per-call-
    // site update would otherwise be needed. When more than one job shares a
    // path (a Done/Failed history entry plus a freshly re-queued run), the
    // more "in progress" one wins: Active > Paused > Queued > Failed > Done —
    // i.e. show whatever's most relevant to "is something happening to this
    // file right now", falling back to Failed over Done among purely
    // historical entries since a failure is the more actionable one to
    // surface. A download job (empty fileUrl) contributes nothing to this
    // index — it names no single vault file.
    void rebuildJobStatusIndex();

    // Wires one task's generic (progress/phase/paused) + kind-specific result
    // signals to this controller. Called exactly once per task (guarded by
    // m_connectedTasks), right before its first dispatch — from
    // onSerialTaskStarting()/onInstantTaskStarting(). Stays fully controller-side
    // (it reads m_vaultModel/m_pendingRenames and emits typed signals).
    void connectTask(Task *task);
    // The ONE way a task enters the queue — a thin forwarder to
    // m_scheduler->enqueueTask() so the friend test seam and every single-file
    // helper keep the same call site. Returns the new task's id.
    int enqueueTask(std::unique_ptr<Task> task, int insertIndex = -1);

    // Item 17: the three public chapter editors (addChapter/setChapterTitle/
    // deleteChapter) all reopen the .md, carry frontmatter through untouched,
    // mutate the body's "## [timestamp] title" marker set, and write back —
    // the exact reopen/rewrite shape setPrelude() uses. Factored here so the
    // three differ only by their body mutation (the enum). Runs synchronously
    // on the calling (GUI) thread — a single small file read+write, no
    // model/engine involvement, like setLabel()/setPrelude(). Returns false
    // (errorOut set) without writing if the file isn't transcribed, the
    // frontmatter is malformed, or the mutation found nothing to act on
    // (SetTitle/Delete against a timestamp with no marker).
    enum class ChapterEditOp { Add, SetTitle, Delete };
    bool editChapterMarker(const QUrl &fileUrl, ChapterEditOp op, double timestampSeconds, const QString &title,
                            QString *errorOut) const;
    // Wave N (task system, item 5): pushes a Queued "download" job for
    // whichever model file `target` names, UNLESS one is already
    // Queued/Active or the file is already on disk (checked fresh here, not
    // cached — models can be deleted out from under the app between calls).
    // Called right before transcribe()/relabel() queues its own job, so the
    // download (if needed) lands just ahead of it in m_jobs and therefore
    // runs first. Returns the new job's id, or -1 if no download was needed.
    //
    // insertIndex (item O15): where to insert the download job, if one is
    // queued — -1 (default) appends to the end of m_jobs, which is what
    // queue-REPLAY callers (enqueueReplayedJob()/loadPersistedQueue()) want,
    // since restoring a persisted session must reproduce its exact saved
    // order rather than jump anything. transcribe()/relabel() instead pass
    // frontOfQueueInsertIndex()'s cursor, so a live "start this now" request
    // jumps ahead of whatever's already Queued (see their own doc comments).
    int maybeQueueDownloadTask(ModelDownloadTarget target, const QUrl &vaultUrl, const QString &modelId,
                                int insertIndex = -1);
    // Item O15: where a freshly requested job should land in m_jobs so it
    // runs NEXT once whatever's currently running finishes, ahead of
    // anything already Queued — right after the Active job if one exists
    // (never ahead of it: this must not preempt work already in flight), or
    // otherwise right before the first currently-Queued entry (never before
    // a Paused job, which startNext() already treats as higher priority
    // than Queued — see its doc comment; and never before Done/Failed
    // history, which is append-only by design — see the class doc comment).
    // Forwards to TaskQuery::frontOfQueueInsertIndex(m_tasks, m_activeIndex).
    int frontOfQueueInsertIndex() const;
    // True if ANY Queued/Active job already targets this exact download
    // (same vaultUrl + target + modelId) — maybeQueueDownloadJob()'s dedup
    // check. Forwards to TaskQuery::isDownloadQueued().
    bool isDownloadQueued(ModelDownloadTarget target, const QUrl &vaultUrl, const QString &modelId) const;

    // Wave N (task system, item 1): serializes every Queued job (an Active
    // job is written as Queued — see loadPersistedQueue()'s doc comment for
    // why) to m_vaultUrl's tasks.json. Connected to queueChanged() in the
    // constructor, so this fires automatically after every mutation; a
    // no-op if m_vaultUrl is empty (no vault open yet/ever this session).
    // Persistence is OPT-IN: only a task whose isPersistable() returns true is
    // written. A kind whose inputs name an external/removable path (import,
    // usbGrab) simply never overrides it — replacing the old opt-OUT list
    // (`if (job.importOnly) continue;`) that a new kind could silently forget
    // and thereby persist a mount path that names a different device next
    // session. See .task/plan.md invariant 3.
    //
    // Split: the JSON is built by TaskQuery::serializeQueue() (pure over the
    // task list — that's where the Queued-only / opt-in-persistable rules
    // actually live); what's left here is the m_vaultUrl guard, the path
    // resolution, and the QSaveFile write.
    void persistQueue() const;
    static QString tasksJsonPath(const QUrl &vaultUrl);

    // Still the slot TranscribeTask::phaseDurationMs connects to — the signal
    // carries only (phase, durationMs), while the per-CONFIGURATION timing key
    // also needs modelId + speakerDetection, both of which are Active-job state
    // that lives here (activeModelIdForTiming()/m_activeSpeakerDetection).
    // So this stays the connect target, reads that context, hands it to
    // PhaseTimer::recordPhaseDuration(), and then recomputes the overall bar so
    // the CURRENT phase's own slice reflects the freshly-updated average right
    // away rather than waiting for the next tick.
    void recordPhaseDuration(const QString &phase, qint64 durationMs);
    // modelId of whichever job is currently Active, or empty if none/not
    // applicable — see recordPhaseDuration()'s doc comment above for why this
    // reads m_jobs/m_activeIndex directly instead of needing a parameter
    // threaded in from the worker's signal.
    QString activeModelIdForTiming() const;
    // Gathers the Active-job context PhaseTimer::recomputeOverallProgress()
    // needs — the Active task's ProgressModel and its ordered phase set, plus
    // m_phase/m_progress/m_phaseStartMs/m_activeSpeakerDetection — and hands it
    // over. Called on every phase() transition, every task progress() tick, AND
    // on m_indeterminatePhaseTimer's tick (the only way "loading"/"decoding" —
    // which have no real progress callback — ever advance mid-phase). See
    // PhaseTimer's doc comments for the weighting itself and for when it's a
    // no-op.
    void recomputeOverallProgress();

    // PHASE Q: the display-blind queue engine. Owns the task list, the two
    // worker threads/runners, and the pause/cancel INTENT — everything this
    // class no longer touches directly. Not a QObject child (deleted explicitly
    // in ~TranscriptionController, so the thread shutdown runs while this
    // observer is still fully valid). Constructed AFTER m_phaseTimer since the
    // inline getters need neither yet, but before anything can enqueue.
    TaskScheduler *m_scheduler = nullptr;
    // onSerialActiveEnded() snapshots m_currentFile here so the terminal
    // finished() report names the file that finished, even after pumpQueues()
    // has already moved m_currentFile on to the next task.
    QUrl m_finishingFileUrl;
    // Bound from QML (setVaultTreeModel); null in headless tests. Used only to
    // call applyRemovalOutcome()/applyRestoreOutcome() after a trash/delete/
    // restore task completes — see setVaultTreeModel()'s doc comment.
    VaultTreeModel *m_vaultModel = nullptr;
    QHash<QString, QString> m_pendingRenames; // oldLocalFilePath -> new stem
    int m_pendingRenamesRevision = 0;
    bool m_busy = false;
    double m_progress = 0.0;
    // See the activeIsForeground Q_PROPERTY doc comment — recomputed in
    // startTask()/onTaskFinished() from the Active task's presentation().
    bool m_activeIsForeground = false;
    // The phase-timing / progress-estimation collaborator: owns the persisted
    // rolling averages AND the blended 0..1 overallProgress the QML bar binds
    // to. Owned (QObject child, constructed first in the constructor so the
    // inline progress()/overallProgress() getters are safe from that point on);
    // its overallProgressChanged is forwarded to this class's same-named signal
    // there too, which is what keeps the QML-facing Q_PROPERTY notifying
    // exactly as it did before the split.
    PhaseTimer *m_phaseTimer = nullptr;
    // Whether the Active job is the standalone diarize job — set in startJob()
    // from job.diarizeOnly so recomputeOverallProgress() knows whether the
    // active-phase set is the diarize shape (decoding/detecting-speakers) or
    // the transcribe shape (loading/decoding/detecting-speech/transcribing),
    // before (or even absent) the worker's first phase() signal. A pure-ASR
    // transcribe never runs diarization, so this is only ever true for the
    // diarize job.
    bool m_activeSpeakerDetection = false;
    // activeElapsedMs() at the moment m_phase last changed — see
    // phaseRelativeSyntheticProgress()'s doc comment.
    qint64 m_phaseStartMs = 0;
    // Ticks only during "loading"/"decoding" (see recomputeOverallProgress()'s
    // doc comment) — those two have no real progress callback of their own,
    // so without this m_overallProgress would sit frozen for however long
    // they take.
    QTimer *m_indeterminatePhaseTimer = nullptr;
    QString m_phase;
    QUrl m_currentFile;
    int m_liveUpdateTick = 0;
    // Tasks whose signals connectTask() has already wired. A task is
    // dispatched more than once across its life (a Paused task re-enters
    // startTask() when resumed), and re-connecting would double-count every
    // progress tick. Raw pointers, only ever compared — the entries are owned
    // by the scheduler's task list and forgotten via onTaskRemoved() as they're
    // erased.
    QSet<const Task *> m_connectedTasks;
    // Item P6: rebuilt from m_jobs by rebuildJobStatusIndex() (see its own
    // doc comment) — keyed by absolute local file path, mirrors jobs()'
    // per-path status but as an O(1) lookup for jobStatusForPath() instead of
    // a linear jobs() scan.
    QHash<QString, Task::Status> m_jobStatusByPath;
    // Set by loadPersistedQueue() (once per vault open) — where persistQueue()
    // writes back to. Empty (no persistence) until then.
    QUrl m_vaultUrl;
    // See isActivePaused()'s doc comment.
    bool m_activePaused = false;
    // Wave N (task system, item 4/7): wall-clock ms the Active job has
    // actually been RUNNING, excluding paused stretches — see
    // activeElapsedMs()'s doc comment. m_activeSegmentTimer covers the
    // current (unpaused) running stretch since job start or last resume;
    // m_activeAccumulatedMs holds everything before that.
    QElapsedTimer m_activeSegmentTimer;
    qint64 m_activeAccumulatedMs = 0;
    // See activeBackendName Q_PROPERTY's doc comment.
    QString m_activeBackendName;

    // Item O19: readMetadata()/readTranscriptBody()/parseSentences() are all
    // Q_INVOKABLE and called synchronously from QML bindings on every
    // liveUpdateTick (~5x/sec, see its own doc comment) while a
    // transcription is actively running on the currently-open file — each
    // one used to unconditionally re-open, re-read, and (for parseSentences)
    // re-run a regex over the ENTIRE file from scratch, every tick, on the
    // GUI thread, with cost growing with transcript length. These three
    // single-slot (not per-path map — only the currently open file is ever
    // polled this often; switching files just misses the cache once, no
    // separate eviction step needed) caches back the mitigation: a cheap
    // QFileInfo stat (size+mtime) guards the common "nothing changed since
    // last call" case entirely, and parseSentences() additionally reuses an
    // exact-prefix check to regex-parse only the NEW trailing bytes of an
    // append-only live transcript instead of the whole body (falling back to
    // a full reparse whenever that prefix check fails — e.g.
    // SegmentLineWriter rewrote the tail in place merging a repeat cycle —
    // never trading correctness for speed).
    struct FrontmatterCache {
        QString mdPath;
        qint64 size = -1;
        QDateTime mtime;
        QVariantMap data;
        bool valid = false;
    };
    mutable FrontmatterCache m_frontmatterCache;
    struct TranscriptBodyCache {
        QString mdPath;
        qint64 size = -1;
        QDateTime mtime;
        QString body;
        bool valid = false;
    };
    mutable TranscriptBodyCache m_bodyCache;
    struct SentenceCache {
        QString mdPath;
        QString body; // the exact body string `sentences` was parsed from
        QVariantList sentences;
        bool valid = false;
    };
    mutable SentenceCache m_sentenceCache;
    // Item P23: readMetadata()/readTranscriptBody()/parseSentences() are now
    // also invoked from a background QThread by loadTranscriptAsync() (see
    // its doc comment), concurrently with the GUI thread's own synchronous
    // calls to the same three functions (the existing liveUpdateTick-driven
    // polling above) — this single mutex serializes all access to the three
    // caches above so neither thread ever observes/writes a torn cache
    // entry. One coarse lock (not per-cache) is deliberate: these functions
    // are cheap and this is not a contended hot path (the async path only
    // runs once per file-open, not per-tick), so the simplicity of locking
    // each function's entire body outweighs any benefit from finer-grained
    // locking. RECURSIVE: parseSentences() locks this itself and then calls
    // readTranscriptBody()/readMetadata(), which each lock it again on the
    // SAME thread — a plain QMutex would deadlock there.
    mutable QRecursiveMutex m_transcriptCacheMutex;

    // Per-.md-file parsed `tags:` values, keyed by absolute path — backs
    // existingVaultTags() only; see its doc comment for why it exists and why
    // it needs no mutex (GUI thread only).
    mutable QHash<QString, CachedFileValues> m_vaultTagFileCache;

    // The `speakers:` counterpart of m_vaultTagFileCache, backing
    // existingVaultSpeakers(). Separate hash, not a shared one: the cached
    // value is per-KEY, so one hash keyed by path alone would hand a tags
    // lookup the speakers answer for any file both scans have seen.
    mutable QHash<QString, CachedFileValues> m_vaultSpeakerFileCache;
};

// Every distinct value of ONE frontmatter key (`key` == "tags" or "speakers")
// used anywhere in the vault. For "tags" it feeds a labeling pass's tag
// vocabulary (existing-tag reuse bias / restrict-to-existing); for "speakers"
// it backs the sidebar's speaker-filter autocomplete (existingVaultSpeakers()).
// cache: nullptr when called from a Task's run() (worker thread; must not
// touch the GUI thread's cache) — pass a real cache only from a GUI-thread
// caller that re-enters this on every keystroke (see existingVaultTags()).
// Declared after the class (not alongside the other transcript-format
// helpers above) because CachedFileValues is a nested type of
// TranscriptionController and isn't visible until the class body closes.
QStringList collectVaultFrontmatterList(const QString &vaultLocalPath, const QString &key,
                                        QHash<QString, TranscriptionController::CachedFileValues> *cache);
