#pragma once

#include <QHash>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QThread>
#include <QUrl>
#include <QVariantList>

#include <functional>
#include <memory>
#include <vector>

#include "CombineScanner.h" // full type — CombineGroup is a by-value parameter of enqueueCombineJob
#include "tasks/ModelDownloadTarget.h" // ModelDownloadTarget — the wrinkle-1 prerequisite hook

class Task;
class TaskRunner;

// The DISPLAY-BLIND queue engine split out of TranscriptionController (2026-07-20,
// PHASE Q). This is what the A4 "extract the whole queue" attempt couldn't do as
// a mesh: TaskScheduler owns ONLY the queue mechanics — the task list, the two
// worker threads/runners, enqueue/pump/start/finish, the cancel/dismiss/undo/
// combine machinery — and NEVER touches a display field, NEVER emits a QML-facing
// typed signal, NEVER calls connectTask. The dependency is inverted: the
// scheduler knows only the abstract TaskSchedulerObserver, calling back through it
// at the exact synchronous points display state should update, so the golden
// emission ORDER the QML bindings depend on is preserved byte-for-byte.
//
// One-directional: scheduler -> observer. The only "read back" is
// observer->activeElapsedMs() — a pure value query used solely to stamp a
// finished task's durationMs, never a mutation.
//
// The controller stays the adapter: it implements TaskSchedulerObserver, does all
// display work + all typed emits, re-emits the scheduler's queueChanged as its
// own (which keeps driving persistQueue/rebuildJobStatusIndex + the QML NOTIFY),
// and forwards every queue Q_INVOKABLE here as a thin call so QML sees ZERO change.
class TaskSchedulerObserver {
public:
    virtual ~TaskSchedulerObserver() = default;

    // --- Serial start (from startTask, AFTER m_activeIndex/status/resetControl
    // set and m_activeCancelling cleared, BEFORE queueChanged + dispatch).
    // Controller emits, IN THIS ORDER: activePausedChanged; if wasCancelling
    // activeStoppingChanged; (reset segment timer, setOverallProgress(0),
    // speakerDetection, phaseStart, stop indeterminate timer); busyChanged; if
    // foreground changed activeIsForegroundChanged; progressChanged;
    // currentPhaseChanged (after m_phase.clear()); currentFileChanged (after
    // m_currentFile=task->fileUrl); currentJobKindChanged. THEN connectTask(task)
    // guarded by m_connectedTasks (emits nothing).
    virtual void onSerialTaskStarting(Task *task, bool wasCancelling) = 0;

    // --- Instant start (from startInstantTask, before queueChanged + dispatch).
    // Just connectTask(task) guarded by m_connectedTasks. Emits nothing.
    virtual void onInstantTaskStarting(Task *task) = 0;

    // --- Serial finish, phase 1 "active work ended". Controller: m_busy=false +
    // busyChanged; if m_activeIsForeground clear + activeIsForegroundChanged; stop
    // indeterminate timer; SNAPSHOT m_currentFile for the later report; if
    // wasStopping emit activeStoppingChanged.
    virtual void onSerialActiveEnded(bool wasStopping) = 0;

    // --- Serial finish, phase 2 "terminal status reached" (only when the task is
    // still in the queue AND not pausedAsRestart). Controller emits: if ok &&
    // completionChangesModelSet() -> modelsChanged; benchmark/sweep failure
    // (non-cancel) -> benchmarkFailed(error). Returns activeElapsedMs() so the
    // scheduler can store it as task->durationMs.
    virtual qint64 onSerialTaskTerminal(Task *task, bool ok, const QString &error) = 0;

    // --- Serial finish, phase 3 "active slot cleared" (after m_activeIndex=-1;
    // scheduler clears m_activeCancelling itself before calling). Controller:
    // m_activePaused=false + activePausedChanged; if wasCancelling emit
    // activeStoppingChanged.
    virtual void onSerialActiveCleared(bool wasCancelling) = 0;

    // --- Queue idle drain (pausedAsRestart branch or post-pump m_activeIndex<0).
    // Controller: m_currentFile=QUrl() + currentFileChanged; currentJobKindChanged.
    // The scheduler emits queueChanged + invokes evictIdle right after this returns.
    virtual void onQueueDrained() = 0;

    // --- Terminal report. Controller emits finished(m_finishingFileUrl, ...).
    // threeArg==true reproduces the pausedAsRestart branch's 3-arg finished()
    // (omits structural); false uses the 4-arg with structural.
    virtual void reportSerialFinished(bool ok, const QString &error, bool structural,
                                      bool threeArg) = 0;

    // --- Instant terminal report (from the instant branch, after queueChanged).
    // Controller emits finished(finishedFile, ok, error, task->isStructuralFileOp()).
    virtual void reportInstantFinished(Task *task, const QUrl &finishedFile,
                                       bool ok, const QString &error) = 0;

    // --- Undo availability. The scheduler computes token+label from queue state
    // (maybeEmitUndoAvailable); the emit is here.
    virtual void emitUndoAvailable(int token, const QString &label) = 0;

    // --- Pure value query: the controller's active-work elapsed clock. NEVER
    // mutates. Used only to stamp task->durationMs.
    virtual qint64 activeElapsedMs() const = 0;

    // --- Wrinkle 1: prerequisite/model resolution. queuePrerequisites needs
    // ModelManager (model-missing detection) + DownloadTask construction — model
    // knowledge the scheduler must NOT own. The controller resolves it
    // (maybeQueueDownloadTask), inserting the download via the scheduler's own
    // insertResolvedDownload(). Returns the new download task's id, or -1 if no
    // download was needed.
    virtual int resolveDownloadPrerequisite(ModelDownloadTarget target, const QUrl &vaultUrl,
                                            const QString &modelId, int insertIndex) = 0;

    // --- The connectTask wired-once guard (m_connectedTasks) is a controller
    // concern; the scheduler notifies it when erasing a task so a reused heap
    // address can never falsely read as already-connected.
    virtual void onTaskRemoved(Task *task) = 0;

    // --- A fresh cancel request flipped the scheduler's stopping intent while a
    // task is Active (cancelJob/cancelAll) — outside any start/finish burst, so
    // it can't ride an onSerial* method. The controller re-emits activeStoppingChanged.
    virtual void onActiveStoppingChanged() = 0;

    // --- cancelTaskAt()'s synthesized terminal for a task cancelled before it
    // ever ran (the one-terminal-report invariant). Controller emits
    // finished(fileUrl, false, "Cancelled").
    virtual void reportCancelledBeforeRun(const QUrl &fileUrl) = 0;
};

class TaskScheduler : public QObject {
    Q_OBJECT
    // Same characterization seam the test uses on the controller: the combine
    // redirect test seeds m_combineRules/m_combineAudioPaths/m_combineDeleteOriginals
    // directly (they moved here from the controller). Grants nothing to
    // production code.
    friend class TstTranscriptionQueue;

public:
    using TaskList = std::vector<std::unique_ptr<Task>>;

    explicit TaskScheduler(TaskSchedulerObserver *observer, QObject *parent = nullptr);
    ~TaskScheduler() override;

    // --- Read-only accessors (one-way; the controller reads queue state) ---
    const TaskList &tasks() const { return m_tasks; }
    int activeIndex() const { return m_activeIndex; }
    Task *activeTask() const;
    Task *instantActive() const { return m_instantActive; }
    // The pause/cancel INTENT (Q1 dormant scaffolding carried verbatim). The
    // controller's activeStopping Q_PROPERTY reads this; its NOTIFY is fired via
    // the observer.
    bool isActiveStopping() const { return m_pausingActiveAsRestart || m_activeCancelling; }
    bool isPausingActiveAsRestart() const { return m_pausingActiveAsRestart; }
    const QHash<int, QString> &batchLabels() const { return m_batchLabels; }

    // --- Queue mutation entry points (controller Q_INVOKABLEs forward here) ---
    int enqueueTask(std::unique_ptr<Task> task, int insertIndex = -1);
    int enqueueBatch(std::vector<std::unique_ptr<Task>> tasks, const QString &displayLabel = {});
    void withBatch(const QString &displayLabel, const std::function<void()> &fn);
    int frontOfQueueInsertIndex() const;
    void startNext();
    void moveQueuedJob(int id, int newIndex);
    void cancelJob(int id);
    void cancelActive();
    void cancelBatch(int batchId);
    void cancelAll();
    void dismiss(const QUrl &fileUrl);
    void dismissBatch(int batchId);
    void clearFinished();
    void undoTask(int token);
    void loadPersistedQueue(const QUrl &vaultUrl);
    void runCombineScan(const QVariantList &rules, const QStringList &audioPaths, const QUrl &vaultUrl,
                        bool deleteOriginals);
    void combineGroup(const QVariantList &rules, const QStringList &audioPaths, const QString &outputPath,
                      const QUrl &vaultUrl, bool deleteOriginals);

    // --- Runner control (the controller's pause/resume forward here) ---
    void requestPause();
    void requestResume();

    // --- Wrinkle 1: the mechanics half of a prerequisite/direct download. The
    // controller (which owns ModelManager) builds the DownloadTask and hands it
    // here; the scheduler assigns the id, marks it Queued, and inserts it.
    // Returns the id. Does NOT emit queueChanged (callers do).
    int insertResolvedDownload(std::unique_ptr<Task> download, int insertIndex);

signals:
    void queueChanged();

private:
    void pumpQueues();
    void startTask(int index);
    void startInstantTask(int index);
    bool serialActiveConflictsWith(const QUrl &fileUrl) const;
    void onTaskFinished(Task *task, bool ok, const QString &error);
    int indexOfTask(const Task *task) const;
    void removeTaskAt(int index);
    void cancelTaskAt(int index);
    void queuePrerequisites(const Task &task, const QUrl &vaultUrl, int *insertAt);
    void maybeEmitUndoAvailable(const Task *task, bool ok);
    bool batchHasMembers(int batchId) const;
    bool isCombineQueued(const QString &outputPath) const;
    void enqueueCombineJob(const CombineGroup &group, const QUrl &vaultUrl, bool deleteOriginals,
                           int insertIndex = -1);
    int enqueueReadyCombineGroups(const QVariantList &rules, const QStringList &audioPaths,
                                  const QUrl &vaultUrl, bool deleteOriginals,
                                  const QString &outputPathFilter = QString());
    bool maybeRedirectForCombine(int index);

    TaskSchedulerObserver *m_observer = nullptr;

    // The two worker lanes moved wholesale from the controller (see Task::Lane).
    QThread m_thread;
    TaskRunner *m_runner = nullptr;
    QThread m_instantThread;
    TaskRunner *m_instantRunner = nullptr;
    Task *m_instantActive = nullptr;

    // The queue proper.
    std::vector<std::unique_ptr<Task>> m_tasks;
    int m_activeIndex = -1;
    int m_nextJobId = 1;
    int m_currentBatchId = 0;
    QHash<int, QString> m_batchLabels;

    // Pause/cancel INTENT (Q1: m_pausingActiveAsRestart is INTENTIONAL dormant
    // scaffolding, carried verbatim, NOT deleted).
    bool m_pausingActiveAsRestart = false;
    bool m_activeCancelling = false;

    // Cached from the most recent runCombineScan()/combineGroup() call, so
    // maybeRedirectForCombine()'s per-job safety check has something to consult.
    // The scheduler is the sole reader; the controller's combine-config Q_INVOKABLEs
    // forward here.
    QVariantList m_combineRules;
    QStringList m_combineAudioPaths;
    bool m_combineDeleteOriginals = false;
};
