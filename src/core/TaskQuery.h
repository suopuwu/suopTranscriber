#pragma once

#include <QJsonArray>
#include <QString>
#include <QUrl>
#include <QVariantList>

#include <memory>
#include <vector>

#include "tasks/ModelDownloadTarget.h"

class Task;

// The task-list QUERY/SERIALIZATION collaborator split out of
// TranscriptionController (2026-07-20). Everything here is a pure function of
// the queue's contents — `const std::vector<std::unique_ptr<Task>> &tasks`,
// plus `activeIndex` where the answer depends on what's currently running.
//
// This is deliberately the NARROW seam. The scheduler proper (pumpQueues(),
// enqueueTask(), the terminal-report path, the serial-lane display state) is
// genuinely fused with the controller's member state and signals, and stays
// there; an earlier attempt to lift the whole queue engine out needed ~30
// forward accessors and ~7 reverse callbacks and was abandoned. What lives
// here is only the subset that ASKS QUESTIONS OF the task list and never
// mutates it, emits anything, or reaches back into the controller for
// m_nextJobId / m_batchLabels / any cache. That's what makes it clean: zero
// accessors back, zero reverse callbacks.
//
// Two functions here are the PURE HALF of a controller method that also
// mutates:
//   * planQueuedMove() computes moveQueuedJob()'s index math; the actual
//     erase/insert, the m_activeIndex fixups' sibling bookkeeping, and the
//     queueChanged() emission stay on the controller.
//   * serializeQueue() builds persistQueue()'s JSON; the vault-path
//     resolution and the QSaveFile write stay on the controller.
// loadPersistedQueue()'s replay is NOT here — it allocates ids from
// m_nextJobId, calls createTaskFromJson()/queuePrerequisites(), inserts, and
// emits, so it is not a query at all.
//
// Stateless by construction — never instantiated, hence the deleted
// constructor. The controller holds no member for it; it calls these statics
// directly, and its own Q_PROPERTY/Q_INVOKABLE surface (tasksTotal,
// tasksCompleted, queuedFileUrls, isQueued, moveQueuedJob, …) remains on the
// controller as thin forwarders so QML's (and the tests') call sites are
// unchanged.
class TaskQuery
{
public:
    TaskQuery() = delete;

    using TaskList = std::vector<std::unique_ptr<Task>>;

    // Index of `task` in `tasks`, or -1. The runner hands back a raw pointer;
    // the task may have been removed (cancelled/dismissed) in the meantime.
    static int indexOfTask(const TaskList &tasks, const Task *task);

    // True iff some task in `tasks` still carries this (non-zero) batchId —
    // the "does this batch still exist" check behind withBatch()'s
    // empty-batch label prune and dismissBatch()/cancelBatch()'s label
    // cleanup.
    static bool batchHasMembers(const TaskList &tasks, int batchId);

    // Item O15: where a freshly requested job should land in `tasks` so it
    // runs NEXT once whatever's currently running finishes, ahead of
    // anything already Queued — right after the Active job if one exists
    // (never ahead of it: this must not preempt work already in flight), or
    // otherwise right before the first currently-Queued entry (never before
    // a Paused job, which startNext() already treats as higher priority
    // than Queued — see its doc comment; and never before Done/Failed
    // history, which is append-only by design — see the controller's class
    // doc comment).
    static int frontOfQueueInsertIndex(const TaskList &tasks, int activeIndex);

    // Total entry count / Done+Failed count. Both only grow until
    // dismiss()/clearFinished() shrink the list (see the controller's class
    // doc).
    static int tasksTotal(const TaskList &tasks);
    static int tasksCompleted(const TaskList &tasks);

    // fileUrls of jobs still Queued (not yet started) — used by QML to show
    // a "Queued…" state distinct from the currently-Active job.
    static QVariantList queuedFileUrls(const TaskList &tasks);

    // True if fileUrl currently has a Queued, Active or Paused entry that
    // actually occupies the file (a benchmark/sweep on it doesn't block a
    // real op — see Task::occupiesFile()).
    static bool isQueued(const TaskList &tasks, const QUrl &fileUrl);

    // True if a "combine" task producing exactly this outputPath is already
    // Queued/Active — runCombineScan()'s dedup check, mirrors
    // isDownloadQueued().
    static bool isCombineQueued(const TaskList &tasks, const QString &outputPath);

    // True if ANY Queued/Active job already targets this exact download
    // (same vaultUrl + target + modelId) — maybeQueueDownloadTask()'s dedup
    // check.
    static bool isDownloadQueued(const TaskList &tasks, ModelDownloadTarget target, const QUrl &vaultUrl,
                                 const QString &modelId);

    // The pure index math behind moveQueuedJob(). `fromIndex` is the real
    // (tasks) index of the Queued task carrying `id`; `insertIndex` is the
    // real index to reinsert it at AFTER it has been erased from fromIndex.
    // valid == false means "nothing to do" — no Queued task with that id, or
    // the (clamped) target position is where it already sits.
    struct QueuedMove {
        bool valid = false;
        int fromIndex = -1;
        int insertIndex = -1;
    };
    // newIndex counts within the QUEUED SUBLIST only (Active/Done/Failed rows
    // are never part of that ordering) and is clamped into it. Computed
    // entirely before any mutation: the returned insertIndex already accounts
    // for the removal shifting every real index past fromIndex down by one,
    // and lands at the end of `tasks` when newIndex names the last Queued
    // slot.
    static QueuedMove planQueuedMove(const TaskList &tasks, int id, int newIndex);

    // persistQueue()'s serialization half: every task that survives a restart,
    // as the JSON array that gets written to the vault's tasks.json.
    //
    // Only Queued survives — an Active OR Paused task is serialized AS Queued
    // (no true mid-file resume yet); Done/Failed history is session-only by
    // design. Persistence is OPT-IN: only a task whose isPersistable()
    // returns true is written, so a kind whose inputs name an external/
    // removable path (import, usbGrab) simply never overrides it — replacing
    // the old opt-OUT list (`if (job.importOnly) continue;`) that a new kind
    // could silently forget and thereby persist a mount path that names a
    // different device next session. See .task/plan.md invariant 3.
    //
    // The queue owns kind/fileUrl/vaultUrl; each task's toJson() adds whatever
    // that kind needs to rebuild itself (createTaskFromJson()).
    static QJsonArray serializeQueue(const TaskList &tasks);
};
