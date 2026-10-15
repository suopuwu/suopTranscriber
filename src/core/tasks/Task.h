#pragma once

#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QVariantMap>

#include <memory>

#include "ModelDownloadTarget.h"
#include "TaskContext.h"

// One unit of queued work. Replaces the old 30-field `Job` union + 9
// mutually-exclusive bool flags + jobKind() waterfall + the 10-branch
// startJob() dispatch: each concrete kind is now ONE subclass whose doing-code
// (run()) lives next to its reporting-code (its progress/phase/result signals,
// displayName(), toVariantMap()). Adding a kind is subclassing this — not
// editing five disjoint sites and hoping you found them all.
//
// THREADING. A Task is created and owned on the GUI thread (the queue holds it
// for its whole lifetime, including as a Done/Failed history row). Its run()
// executes on the WORKER thread, driven by TaskRunner. The split is safe
// because:
//   - the metadata getters (kind/displayName/presentation/capabilities/
//     isPersistable/toVariantMap) are const and read only immutable state set
//     at construction, so QML may call them while run() executes;
//   - run() communicates ONLY by emitting this object's signals (Qt delivers
//     them queued to GUI-thread connections) and never mutates state the GUI
//     thread reads.
// The queue-owned lifecycle fields below (id/status/error/durationMs) are
// written by the SCHEDULER on the GUI thread, never by run().
class Task : public QObject {
    Q_OBJECT
public:
    // Foreground: this task owns the main-panel overlay ("Transcribing…" et
    // al.) plus the bottom-bar glow while Active. Background: glow + a Tasks
    // row, but NO overlay — a benchmark runs without hijacking the panel the
    // user is reading. There is deliberately no Silent: an invisible task is
    // exactly the failure mode this whole refactor exists to make impossible.
    // Presentation controls overlay-vs-not, NEVER whether the task appears in
    // the Tasks list — everything queued gets a row.
    enum class Presentation { Foreground, Background };

    // What the UI may offer for this task, so the controls ASK the task
    // instead of guessing (the guess is what caused the 2026-07-09 pause-vs-
    // cancel latch bug). pausable: has a real cooperative interruption point
    // (calls TaskContext::checkpoint()). cancellableMidRun: a cancel takes
    // effect before the task finishes naturally (false for atomic single-pass
    // kinds — rename/merge/diarize have no mid-pass abort hook, so a cancel is
    // only honored at the next task boundary). LOAD-BEARING: a
    // pausable && !cancellableMidRun task, when cancelled, is shown as
    // "Cancelling" (pause disabled) until its pass boundary instead of looking
    // resumed — the queue derives that honest state from THIS flag, so declaring
    // it correctly is all a new kind needs (see TranscriptionController's cancel
    // paths + isActiveStopping()).
    struct Capabilities {
        bool pausable = false;
        bool cancellableMidRun = true;
    };

    // Which EngineCache-resident engines this kind's run() calls ensureAsr()/
    // ensureLabel()/ensureDiar() for. TaskRunner::execute() evicts every OTHER
    // engine right before run() starts (EngineCache::applyResidencyPolicy) —
    // one rule replacing N hand-written evictAsr()/evictLabel()/evictDiar()
    // calls scattered per-kind, which is how a diarization arena survived
    // into an unrelated transcribe and pressured the GPU into an uncatchable
    // ggml abort. A kind that needs an engine still calls
    // ensureXxx() itself in run() as always — this only says what dispatch
    // must NOT clear out from under it.
    //
    // Default: needs nothing. That looks like it risks "silently evicting an
    // engine a task is using", but the failure mode of forgetting this
    // override is a redundant reload (ensureXxx() rebuilds unconditionally
    // once its engine is gone) — a performance regression, never a residency
    // one. The other default (needs everything) fails in the direction this
    // item exists to close: a forgotten override would leave that kind's
    // idle engines resident forever, reproducing today's asymmetric-eviction
    // bug for the next kind added. Get eviction wrong by omission, not
    // residency.
    struct EngineNeeds {
        bool asr = false;
        bool label = false;
        bool diar = false;
    };
    virtual EngineNeeds engineNeeds() const { return {}; }

    enum class Status { Queued, Active, Paused, Done, Failed };

    // Which execution lane this kind runs in.
    //   Serial (default): the one heavy-work queue — one at a time, so nothing
    //     contends for a scarce system resource. "Heavy" is NOT just "uses the
    //     GPU / a warm model": it's anything INTENSIVE on system resources,
    //     which includes a big CPU/IO job like a multi-GB model import/export
    //     (ModelIoTask) — that copy shouldn't thrash the disk against a running
    //     transcription, so it queues behind it. Everything heavy is Serial.
    //   Instant: a fast, side-effect-LIGHT kind (a speaker/file rename or move
    //     — a handful of syscalls, no GPU, no heavy IO) that must NOT wait
    //     behind a long transcription. Instant tasks run on a SEPARATE worker
    //     thread in parallel with the Serial queue, one at a time among
    //     themselves. The one exception the scheduler enforces: an Instant task
    //     on file X waits if a Serial task on X is ACTIVE (they'd both rewrite
    //     X's .md). See TranscriptionController::pumpQueues().
    enum class Lane { Serial, Instant };

    // How this kind's run() drives the OVERALL (0..1) progress bar, so the
    // controller never hardcodes a per-kind phase list (it did, in two places
    // that drifted out of sync — see progressPhases()):
    //   SelfCumulative (default): each progress(fraction) is ALREADY the whole
    //     task's fraction — a single linear pass (download, combine, chapters,
    //     model import, USB grab, benchmark). The controller assigns it to the
    //     overall bar directly.
    //   WeightedPhases: the task moves through progressPhases() IN ORDER, each
    //     phase weighted by its rolling-average duration, and within-phase
    //     progress is blended across them (transcribe/diarize's multi-stage
    //     pipeline). A kind returning this MUST also return its ordered phase
    //     slugs from progressPhases().
    enum class ProgressModel { SelfCumulative, WeightedPhases };

    using QObject::QObject;

    // --- The kind's identity (const; safe from any thread) ---
    virtual QString kind() const = 0; // stable slug (logic/persistence key), was jobKind()
    // Short human-readable CATEGORY label for the Tasks row / bottom bar (e.g.
    // "Move to trash"), the noun a row shows before its filename. Each kind
    // authors its own here — the single source of truth that replaced BottomBar's
    // kindLabel(kind) switch, which every new kind could (and did) forget to
    // update, silently mislabeling itself "Transcribe". Distinct from:
    //   kind()        — the stable slug ("trash"), for logic/persistence;
    //   displayName() — the fuller per-instance description ("Trash 3 items").
    // Default returns the slug, so a kind that forgets to override is visibly
    // its slug rather than masquerading as another kind.
    virtual QString kindLabel() const { return kind(); }
    virtual QString displayName() const = 0; // Tasks-popup row text (full description)
    virtual Lane lane() const { return Lane::Serial; }
    virtual Presentation presentation() const { return Presentation::Foreground; }
    virtual Capabilities capabilities() const { return {}; }
    // See the ProgressModel enum. Default SelfCumulative — the common single-
    // pass shape; transcribe/diarize override to WeightedPhases.
    virtual ProgressModel progressModel() const { return ProgressModel::SelfCumulative; }
    // For a WeightedPhases kind: the ordered phase slugs run() moves through,
    // so the overall bar can proportion each by its historical duration. This
    // is the SINGLE source of a kind's phase set — it replaced two hand-kept
    // phase-string lists in TranscriptionController (the progress lambda +
    // recomputeOverallProgress()) that had already drifted apart. Empty for a
    // SelfCumulative kind (which needs no per-phase weighting).
    virtual QStringList progressPhases() const { return {}; }
    // True iff this task's SUCCESSFUL completion changes the set of models on
    // disk (so the Models page must refresh via modelsChanged()). The
    // controller fires modelsChanged() ONCE, centrally, for any task that says
    // so — replacing the two scattered emit sites (a kind=="download" check in
    // onTaskFinished plus an op=="import" check in the ModelIoTask wiring) that
    // a new model-affecting kind had to remember to join. Default: false.
    virtual bool completionChangesModelSet() const { return false; }
    // True iff this task MOVES files on disk (rename/move/import/combine/grab),
    // so the vault's QFileSystemWatcher rebuild will find a changed name/path/
    // child-count, fail sameStructure(), and emit a full modelReset. That reset
    // is the authoritative tree refresh, which makes the Sidebar's generic
    // `finished`-signal refresh (refreshTreeOrdering/refreshSearchResults)
    // redundant for these kinds — running both is what reflowed the left panel
    // TWICE for one rename. Sidebar.onFinished skips its refresh when this is
    // true and lets the reset do the single reflow.
    //
    // Default FALSE, and deliberately so for the in-place kinds (relabel/tags/
    // chapters/speaker rename/merge): they rewrite an existing .md without
    // changing the node, so sameStructure() holds, the watcher emits only
    // dataChanged, NO reset ever arrives — and `finished` is their ONLY reflow
    // trigger. Marking such a kind true would leave the tree stale, so this is
    // the dangerous direction: when unsure, leave it false (worst case is the
    // benign double reflow, not a missed one).
    //
    // NOT overridden by transcribe: re-transcribing an ALREADY-transcribed file
    // leaves the node's status untouched, so sameStructure() holds and no reset
    // follows — it only resets on the first transcribe, when status flips.
    // Nor by trash/delete/restore: those apply their model change synchronously
    // (VaultTreeModel::applyRemovalOutcome/applyRestoreOutcome) rather than via
    // the watcher, a different refresh path this flag does not model.
    virtual bool isStructuralFileOp() const { return false; }
    // True iff this task OCCUPIES its fileUrl for per-file gating purposes:
    // the isQueued() enqueue-dedup, queuedFileUrls(), the path->status index
    // the file tree reads (rebuildJobStatusIndex), and the MainPanel/Sidebar
    // per-file action enable/disable. Default true — a transcribe/diarize/
    // relabel/chapters/rename/move/import genuinely owns the file while it runs,
    // so a second op on the same file must wait (and the UI must show it busy).
    // FALSE for a comparison-only run (benchmark / diarization sweep) that reads
    // the file but writes NOTHING to the vault: it must not grey out Transcribe
    // on the very file you're benchmarking, nor make isQueued() swallow a
    // transcribe queued on it. Such a run is still a real queued task (Tasks row
    // + glow) — it just doesn't monopolize its file's per-file UI.
    virtual bool occupiesFile() const { return true; }
    // EVERY vault file this task would act on. Default: just fileUrl, which is
    // the whole story for the single-file kinds. The multi-file kinds
    // (trash/delete over a selection) must override — they carry a QList<QUrl>
    // and set fileUrl to only the FIRST of it, so a fileUrl-only check reads a
    // ten-file trash as touching one file and waves the other nine through.
    // Used by the enqueue guard that refuses to act on a file being recorded.
    virtual QList<QUrl> touchedFiles() const
    {
        return fileUrl.isEmpty() ? QList<QUrl>{} : QList<QUrl>{ fileUrl };
    }
    // Opt-IN persistence (default: not saved across a restart). A kind whose
    // inputs name an external/removable path (import, usbGrab) simply never
    // overrides this — no more persistQueue() opt-OUT that a new kind forgets.
    // A kind that overrides this to true MUST also override toJson() with
    // whatever fromJson() needs to rebuild it (fileUrl/vaultUrl/kind are
    // written by the queue; everything else is this kind's own business).
    virtual bool isPersistable() const { return false; }

    // This kind's own fields for a persisted queue entry. The queue writes
    // kind/fileUrl/vaultUrl around this, and createTaskFromJson() (see
    // TaskFactory.h) rebuilds the kind from the merged object. Only ever
    // called when isPersistable() is true. Default: no extra fields.
    virtual QJsonObject toJson() const { return {}; }

    // The models this kind needs ON DISK before it can run. The queue turns
    // each into its own DownloadTask inserted AHEAD of this one (deduped) —
    // so a kind declares its model needs ONCE, here, instead of every
    // enqueue site (and the persisted-queue replay path) each remembering to
    // queue the right downloads for it. Default: needs nothing.
    virtual ModelPrerequisites prerequisites() const { return {}; }

    // --- Undo (optional; a kind opts in by overriding both) ---
    // Undo is just the INVERSE operation, expressed as another Task: a
    // rename's undo is a rename-back, a move's is a move-back, a trash's is a
    // restore. So an undoable kind returns a non-empty undoLabel() (the undo
    // snackbar's text) AND a makeUndoTask() that builds the inverse — the queue
    // shows the snackbar when this task finishes Done, and enqueues the inverse
    // (reported + re-undoable like any task) if the user clicks Undo. Both
    // must be consistent: a non-empty label with a null factory is a no-op.
    //
    // makeUndoTask() is called AFTER a successful run(), so a kind whose
    // inverse depends on run()'s result (e.g. trash, which learns the
    // .trashinfo paths only by trashing) captures that in run() and reads it
    // here. Runs on the GUI thread (it only constructs a Task); the returned
    // task then executes on a worker thread like any other. Default: not
    // undoable.
    virtual QString undoLabel() const { return {}; }
    virtual std::unique_ptr<Task> makeUndoTask() const { return nullptr; }

    // Did this task actually land something undoable — a non-empty
    // undoLabel() AND a buildable inverse? Same gate maybeEmitUndoAvailable()'s
    // standalone branch uses to decide whether to offer undo; also read by
    // jobs() so a finished Tasks row can show a per-row Undo button (the
    // persistent equivalent of the auto snackbar). Only meaningful once Done —
    // makeUndoTask() captures run()'s result, so callers gate on status first.
    bool isUndoable() const { return !undoLabel().isEmpty() && makeUndoTask() != nullptr; }

    // Is the undo STILL applicable — i.e. would makeUndoTask()'s inverse still
    // find its source where it expects it? Undo is only offered for a Done
    // task, but by the time the user clicks, the world may have moved on (the
    // file was moved/renamed again), leaving the inverse pointing at a path
    // that no longer holds the file. A kind whose undo can go stale overrides
    // this to say so; the Tasks-list Undo button greys out (stays visible,
    // becomes non-clickable) when it returns false. Default: always applicable
    // (kinds with no filesystem precondition, e.g. TrashTask's restore-from-
    // trash, never go stale this way).
    virtual bool undoStillApplicable() const { return true; }

    // The undo-snackbar label for a BATCH (TranscriptionController::
    // enqueueBatch() — batchId != 0), called on one member with every Done,
    // undoable member (this one included; size 1 is the single-item case).
    // Default: the singleton undoLabel() — a kind that supports multi-select
    // batch undo overrides this to SUMMARIZE ("Moved 5 files"), reading the
    // members for any aggregate (CompressAudioTask sums bytes saved).
    virtual QString batchUndoLabel(const QList<const Task *> &undoableMembers) const
    {
        Q_UNUSED(undoableMembers);
        return undoLabel();
    }

    // This kind's Tasks-row fields for QML, merged over the base row the queue
    // builds (id/kind/status/fileUrl/displayName/error/durationMs). Was the
    // giant per-flag switch in jobs(); now each kind authors its own row.
    // Default: no extra fields.
    virtual QVariantMap toVariantMap() const { return {}; }

    // The work. Executes on the worker thread. Reports via the signals below;
    // returns normally on success, or throws TaskError (see below) on failure
    // — the runner turns either into the single terminal report the queue
    // guarantees. A pausable kind calls ctx.checkpoint() at safe points and
    // returns early when it reports cancellation.
    virtual void run(TaskContext &ctx) = 0;

    // --- Queue-owned lifecycle state (GUI thread writes; run() must not) ---
    int id = 0;
    // 0 = a standalone task. Non-zero = this task is one member of a BATCH
    // (several sibling tasks enqueued together via enqueueBatch(), e.g. a
    // multi-select move) that share this id. The queue uses it to collapse the
    // whole batch into ONE undo snackbar, fired when the LAST member finishes
    // (see maybeEmitUndoAvailable()/undoTask()). Allocated from the same
    // counter as `id`, so a batch id can never collide with any task id.
    int batchId = 0;
    Status status = Status::Queued;
    QString error;
    // Non-fatal diagnostic surviving into a Done (never Failed-meaningful)
    // terminal state — the task fully succeeded but has something worth
    // showing, e.g. TranscribeTask reporting a corrupt-audio repair. Empty
    // for the overwhelming majority of tasks. Set from TaskRunner::
    // taskFinished()'s warning argument, exactly where `error` above is set
    // from its error argument — a subclass reports one via TaskContext::
    // reportWarning() during run(), never by writing this field directly
    // (see the threading doc comment above).
    QString warning;
    qint64 durationMs = -1; // wall-clock Active time, set when the task ends

    // --- Queue-owned identity (GUI thread writes BEFORE dispatch; run() only
    // reads) ---
    // Whatever this kind's ONE file is: the INPUT for transcribe/diarize/
    // relabel/chapters, the OUTPUT for combine/import/usbGrab, empty for
    // download (nothing to open). Same "reuse one url for whatever this kind's
    // one file is" convention the old Job struct used.
    //
    // Lives on the base, not in each subclass, because it is genuinely the
    // QUEUE's business, not the kind's: dedup (isQueued), the path->status
    // index the file tree reads, dismiss(), currentFile, and finished()'s
    // identifying url all key off it uniformly across every kind. It is also
    // MUTABLE for exactly one reason — the combine preflight retargets a
    // transcribe at a merged output before dispatching it (see
    // TranscriptionController::maybeRedirectForCombine()). Mutating it after
    // run() has started would be a data race; the preflight runs strictly
    // before dispatch, on the GUI thread.
    QUrl fileUrl;
    QUrl vaultUrl;

signals:
    // 0.0–1.0 within the current phase. Cross-thread-safe to emit from run().
    void progress(double fraction);
    // Named phase transition ("loading"/"decoding"/… or a kind-specific one),
    // driving the phase label + overall-progress weighting.
    void phase(const QString &phase);
    // True the instant this task actually stops making progress because a
    // pause was requested (a REAL transition, not fired per poll — see
    // TaskContext::reportPaused()). A kind with capabilities().pausable=false
    // never emits this at all.
    void paused(bool isPaused);
    // Subclasses add their own TYPED result signals (e.g. a benchmark's
    // perModelResult / a grab's filesCopied) — the controller connects them
    // per-kind at enqueue. The base intentionally has none.
};

// Thrown by run() to fail a task with a message. The runner catches it and
// synthesizes the terminal report — run() never emits finished itself, which
// is how the queue can promise EXACTLY ONE terminal report per task even for a
// task cancelled before run() was ever called.
class TaskError {
public:
    explicit TaskError(QString message)
        : m_message(std::move(message))
    {
    }
    const QString &message() const { return m_message; }

private:
    QString m_message;
};

// Task* crosses threads via TaskRunner::taskFinished()'s queued connection —
// register it so QMetaType can marshal the argument (TaskRunner's constructor
// calls qRegisterMetaType<Task *>() at runtime; this macro is the compile-time
// half), same reason the old controller registered UsbGrabRule.
Q_DECLARE_METATYPE(Task *)
