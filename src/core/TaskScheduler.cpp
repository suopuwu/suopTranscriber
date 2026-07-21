#include "TaskScheduler.h"

#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaObject>
#include <QSaveFile>

#include <algorithm>

#include "CombineNaming.h" // matchRuleCapturedKey()/combineOutputPathFor() (header-inline free fns)
#include "FileRename.h"    // isSameRecording() — the instant-lane same-file conflict gate
#include "TaskQuery.h"     // the pure task-list queries this engine asks
#include "tasks/CombineTask.h"
#include "tasks/Task.h"
#include "tasks/TaskFactory.h" // createTaskFromJson() for the persisted-queue replay
#include "tasks/TaskRunner.h"
#include "tasks/TranscribeTask.h" // maybeRedirectForCombine()'s qobject_cast

TaskScheduler::TaskScheduler(TaskSchedulerObserver *observer, QObject *parent)
    : QObject(parent)
    , m_observer(observer)
{
    m_runner = new TaskRunner;
    m_runner->moveToThread(&m_thread);
    connect(&m_thread, &QThread::finished, m_runner, &QObject::deleteLater);
    // The ONE terminal-report path from the runner. A task cancelled before it
    // ever ran never reaches the runner at all — the queue synthesizes its
    // report directly (cancelTaskAt()), so BOTH routes converge here and every
    // enqueued task reports exactly once. onTaskFinished() routes by the task's
    // lane, so the instant runner shares this same handler.
    connect(m_runner, &TaskRunner::taskFinished, this, &TaskScheduler::onTaskFinished);

    // The instant lane (see Task::Lane / pumpQueues()): a second worker thread
    // for fast Lane::Instant tasks so a rename never waits behind a serial
    // transcription.
    m_instantRunner = new TaskRunner;
    m_instantRunner->moveToThread(&m_instantThread);
    connect(&m_instantThread, &QThread::finished, m_instantRunner, &QObject::deleteLater);
    connect(m_instantRunner, &TaskRunner::taskFinished, this, &TaskScheduler::onTaskFinished);

    m_thread.start();
    m_instantThread.start();
}

TaskScheduler::~TaskScheduler()
{
    // Prompt shutdown (bug report 2026-07-09): request cancel first so an Active
    // job unblocks at its next checkpoint rather than the whole app hanging on a
    // long native call. requestResume() wakes it in case it was sitting paused.
    if (m_runner) {
        m_runner->requestResume();
        m_runner->requestCancel();
    }
    if (m_instantRunner) {
        m_instantRunner->requestResume();
        m_instantRunner->requestCancel();
    }
    m_thread.quit();
    m_instantThread.quit();
    m_thread.wait();
    m_instantThread.wait();
}

Task *TaskScheduler::activeTask() const
{
    if (m_activeIndex >= 0 && m_activeIndex < static_cast<int>(m_tasks.size()))
        return m_tasks[m_activeIndex].get();
    return nullptr;
}

void TaskScheduler::requestPause()
{
    m_runner->requestPause();
}

void TaskScheduler::requestResume()
{
    m_runner->requestResume();
}

int TaskScheduler::indexOfTask(const Task *task) const
{
    return TaskQuery::indexOfTask(m_tasks, task);
}

int TaskScheduler::frontOfQueueInsertIndex() const
{
    return TaskQuery::frontOfQueueInsertIndex(m_tasks, m_activeIndex);
}

bool TaskScheduler::batchHasMembers(int batchId) const
{
    return TaskQuery::batchHasMembers(m_tasks, batchId);
}

bool TaskScheduler::isCombineQueued(const QString &outputPath) const
{
    return TaskQuery::isCombineQueued(m_tasks, outputPath);
}

// The ONE way a task enters the queue.
int TaskScheduler::enqueueTask(std::unique_ptr<Task> task, int insertIndex)
{
    int insertAt = (insertIndex < 0) ? frontOfQueueInsertIndex() : insertIndex;

    // The models this kind declared it needs, each queued as its own
    // DownloadTask ahead of it if absent — resolved controller-side (it owns
    // ModelManager), inserted here.
    queuePrerequisites(*task, task->vaultUrl, &insertAt);

    task->id = m_nextJobId++;
    task->batchId = m_currentBatchId;
    task->status = Task::Status::Queued;
    const int id = task->id;
    m_tasks.insert(m_tasks.begin() + std::clamp(insertAt, 0, static_cast<int>(m_tasks.size())), std::move(task));
    if (m_activeIndex >= insertAt)
        ++m_activeIndex;

    emit queueChanged();

    pumpQueues();
    return id;
}

void TaskScheduler::queuePrerequisites(const Task &task, const QUrl &vaultUrl, int *insertAt)
{
    for (const ModelPrerequisite &prereq : task.prerequisites()) {
        // The controller owns the model-missing decision + DownloadTask
        // construction (ModelManager stays out of the scheduler); it inserts via
        // insertResolvedDownload() and returns the id, or -1 if none was needed.
        if (m_observer->resolveDownloadPrerequisite(prereq.target, vaultUrl, prereq.modelId, *insertAt) >= 0)
            ++(*insertAt);
    }
}

int TaskScheduler::insertResolvedDownload(std::unique_ptr<Task> download, int insertIndex)
{
    download->id = m_nextJobId++;
    download->status = Task::Status::Queued;
    const int id = download->id;
    const int at = (insertIndex < 0) ? static_cast<int>(m_tasks.size())
                                      : std::clamp(insertIndex, 0, static_cast<int>(m_tasks.size()));
    m_tasks.insert(m_tasks.begin() + at, std::move(download));
    return id;
}

void TaskScheduler::startTask(int index)
{
    // Safety-net preflight, before any state mutation — a true return means it
    // already recursed onto whatever should run instead (a newly-inserted combine
    // task); this call contributes nothing further.
    if (maybeRedirectForCombine(index))
        return;

    m_activeIndex = index;
    Task *task = m_tasks[index].get();
    task->status = Task::Status::Active;
    task->error.clear();

    // A pause/cancel left over from the PREVIOUS task must never leak into this
    // one. resetControl() clears the runner's control block; the cancel INTENT
    // flag is cleared here and its stopping-emit routed through the observer.
    m_runner->resetControl();
    const bool wasCancelling = m_activeCancelling;
    m_activeCancelling = false;

    // The observer does the whole display start block + signal wiring, in the
    // golden emission order.
    m_observer->onSerialTaskStarting(task, wasCancelling);

    emit queueChanged();

    // The ENTIRE dispatch, for every kind.
    QMetaObject::invokeMethod(m_runner, "execute", Qt::QueuedConnection, Q_ARG(Task *, task));
}

bool TaskScheduler::serialActiveConflictsWith(const QUrl &fileUrl) const
{
    return m_activeIndex >= 0 && m_activeIndex < static_cast<int>(m_tasks.size()) && !fileUrl.isEmpty()
        && isSameRecording(m_tasks[m_activeIndex]->fileUrl, fileUrl);
}

void TaskScheduler::pumpQueues()
{
    const int n = static_cast<int>(m_tasks.size());

    // --- Serial lane: one at a time, Paused (resume) before Queued. ---
    if (m_activeIndex < 0) {
        int next = -1;
        for (int i = 0; i < n; ++i) {
            if (m_tasks[i]->lane() == Task::Lane::Serial && m_tasks[i]->status == Task::Status::Paused) {
                next = i;
                break;
            }
        }
        if (next < 0) {
            for (int i = 0; i < n; ++i) {
                if (m_tasks[i]->lane() == Task::Lane::Serial && m_tasks[i]->status == Task::Status::Queued) {
                    next = i;
                    break;
                }
            }
        }
        if (next >= 0)
            startTask(next); // may recurse (combine preflight) and set m_activeIndex
    }

    // --- Instant lane: one at a time, front-most Queued Instant task with no
    // file conflict against the ACTIVE serial task. ---
    if (!m_instantActive) {
        for (int i = 0; i < static_cast<int>(m_tasks.size()); ++i) {
            Task *t = m_tasks[i].get();
            if (t->lane() == Task::Lane::Instant && t->status == Task::Status::Queued
                && !serialActiveConflictsWith(t->fileUrl)) {
                startInstantTask(i);
                break;
            }
        }
    }
}

void TaskScheduler::startInstantTask(int index)
{
    Task *task = m_tasks[index].get();
    task->status = Task::Status::Active;
    task->error.clear();
    m_instantActive = task;

    // Connected once, like a serial task — for its typed result signals. The
    // controller no-ops the serial-only progress/phase/paused wiring for an
    // Instant task.
    m_observer->onInstantTaskStarting(task);
    emit queueChanged(); // status -> active shows in the Tasks list

    m_instantRunner->resetControl();
    QMetaObject::invokeMethod(m_instantRunner, "execute", Qt::QueuedConnection, Q_ARG(Task *, task));
}

void TaskScheduler::onTaskFinished(Task *task, bool ok, const QString &error)
{
    // Instant lane (see Task::Lane / startInstantTask()): a separate, parallel
    // path that never touched the serial-lane UI state, so its completion doesn't
    // either — record the terminal status, emit the one report, and re-pump.
    if (task == m_instantActive) {
        m_instantActive = nullptr;
        const QUrl finishedFile = task->fileUrl;
        if (indexOfTask(task) >= 0) {
            task->status = ok ? Task::Status::Done : Task::Status::Failed;
            task->error = error;
            task->durationMs = 0; // instant by construction — no elapsed tracking
        }
        emit queueChanged();
        m_observer->reportInstantFinished(task, finishedFile, ok, error);
        if (indexOfTask(task) >= 0)
            maybeEmitUndoAvailable(task, ok); // fires per terminal; batch-aware
        pumpQueues();
        return;
    }

    // Same requestCancel()->TaskError("Cancelled") mechanism as a real cancel,
    // distinguished only by this flag (Q1 dormant scaffolding).
    const bool wasStopping = m_pausingActiveAsRestart;
    const bool pausedAsRestart = m_pausingActiveAsRestart && !ok && error == QStringLiteral("Cancelled");
    m_pausingActiveAsRestart = false;

    // Display phase 1 (observer): the "active work ended" block + the
    // wasStopping stopping-emit + the finishing-file snapshot.
    m_observer->onSerialActiveEnded(wasStopping);

    const int taskIndex = indexOfTask(task);
    if (taskIndex >= 0) {
        if (pausedAsRestart) {
            task->status = Task::Status::Paused;
            task->error.clear();
        } else {
            task->status = ok ? Task::Status::Done : Task::Status::Failed;
            task->error = error;
            // The observer returns this task's final active-work duration
            // (excludes paused stretches) and fires the terminal-status typed
            // emits controller-side.
            task->durationMs = m_observer->onSerialTaskTerminal(task, ok, error);
        }
    }
    m_activeIndex = -1;
    const bool wasCancelling = m_activeCancelling;
    m_activeCancelling = false;
    // Display phase 3 (observer): the "active slot cleared" block + the
    // wasCancelling stopping-emit.
    m_observer->onSerialActiveCleared(wasCancelling);

    if (pausedAsRestart) {
        // Deliberately do NOT advance to the next Queued task — the Paused task
        // sits at the front until the user explicitly resumes it.
        m_observer->onQueueDrained();
        emit queueChanged();
        QMetaObject::invokeMethod(m_runner, "evictIdle", Qt::QueuedConnection);
        m_observer->reportSerialFinished(ok, error, /*structural*/ false, /*threeArg*/ true);
        return;
    }

    // Advance both lanes (a just-finished serial task may unblock a same-file
    // instant task). pumpQueues() starts the next serial task if any.
    pumpQueues();
    if (m_activeIndex < 0) {
        // Serial queue drained — clear the "what's running" UI and release the
        // warm engine the finished task was using.
        m_observer->onQueueDrained();
        emit queueChanged();
        QMetaObject::invokeMethod(m_runner, "evictIdle", Qt::QueuedConnection);
    }

    m_observer->reportSerialFinished(ok, error, task->isStructuralFileOp(), /*threeArg*/ false);
    // Fires on EVERY terminal (Done or Failed) so a batch can detect its last
    // member; the standalone path inside still gates on ok. A Paused task isn't
    // terminal, so skip it.
    if (taskIndex >= 0 && !pausedAsRestart)
        maybeEmitUndoAvailable(task, ok);
}

void TaskScheduler::maybeEmitUndoAvailable(const Task *task, bool ok)
{
    // --- Batch member: one snackbar for the whole set, fired by the LAST member
    // to finish. ---
    if (task->batchId != 0) {
        const int batchId = task->batchId;
        int undoableDone = 0;
        int labelMemberIndex = -1;
        for (int i = 0; i < static_cast<int>(m_tasks.size()); ++i) {
            const Task *t = m_tasks[i].get();
            if (t->batchId != batchId)
                continue;
            if (t->status != Task::Status::Done && t->status != Task::Status::Failed)
                return; // a sibling is still pending — this isn't the last one
            if (t->status == Task::Status::Done && t->makeUndoTask()) {
                ++undoableDone;
                labelMemberIndex = i;
            }
        }
        if (undoableDone > 0 && labelMemberIndex >= 0)
            m_observer->emitUndoAvailable(batchId, m_tasks[labelMemberIndex]->batchUndoLabel(undoableDone));
        return;
    }

    // --- Standalone task: undoable iff it succeeded AND advertises a label AND
    // can build its inverse. ---
    if (!ok)
        return;
    const QString label = task->undoLabel();
    if (label.isEmpty() || !task->makeUndoTask())
        return;
    m_observer->emitUndoAvailable(task->id, label);
}

void TaskScheduler::undoTask(int token)
{
    // A batch token: undo every Done member of the batch as ONE fresh batch.
    if (token != 0) {
        std::vector<std::unique_ptr<Task>> inverses;
        for (const auto &task : m_tasks) {
            if (task->batchId == token && task->status == Task::Status::Done) {
                if (auto inverse = task->makeUndoTask())
                    inverses.push_back(std::move(inverse));
            }
        }
        if (!inverses.empty()) {
            enqueueBatch(std::move(inverses));
            return;
        }
    }

    // Otherwise a standalone task id.
    for (const auto &task : m_tasks) {
        if (task->id != token || task->status != Task::Status::Done)
            continue;
        std::unique_ptr<Task> inverse = task->makeUndoTask();
        if (inverse)
            enqueueTask(std::move(inverse), static_cast<int>(m_tasks.size()));
        return;
    }
}

int TaskScheduler::enqueueBatch(std::vector<std::unique_ptr<Task>> tasks, const QString &displayLabel)
{
    if (tasks.empty())
        return 0;
    int batchId = 0;
    withBatch(displayLabel, [&] {
        batchId = m_currentBatchId;
        for (auto &task : tasks)
            enqueueTask(std::move(task), static_cast<int>(m_tasks.size()));
    });
    return batchId;
}

void TaskScheduler::withBatch(const QString &displayLabel, const std::function<void()> &fn)
{
    // Not re-entrant: batches are built synchronously on the GUI thread, one at a
    // time.
    Q_ASSERT(m_currentBatchId == 0);
    const int batchId = m_nextJobId++;
    m_currentBatchId = batchId;
    if (!displayLabel.isEmpty())
        m_batchLabels.insert(batchId, displayLabel);
    fn();
    m_currentBatchId = 0;
    // Nothing landed under this batchId — drop the speculatively-recorded label.
    if (!batchHasMembers(batchId))
        m_batchLabels.remove(batchId);
}

void TaskScheduler::removeTaskAt(int index)
{
    Task *task = m_tasks[index].get();
    const int batchId = task->batchId;
    // The wired-once dispatch guard lives on the controller — tell it to forget
    // this pointer before it's freed (heap reuse would otherwise falsely read as
    // already-wired).
    m_observer->onTaskRemoved(task);
    if (m_instantActive == task)
        m_instantActive = nullptr;
    m_tasks.erase(m_tasks.begin() + index);
    if (m_activeIndex > index)
        --m_activeIndex;
    if (batchId != 0 && !batchHasMembers(batchId))
        m_batchLabels.remove(batchId);
}

void TaskScheduler::cancelTaskAt(int index)
{
    // The pre-run-cancel half of the one-terminal-report invariant: a task
    // cancelled while still Queued/Paused never reached the runner, so the queue
    // synthesizes its terminal report itself.
    const QUrl url = m_tasks[index]->fileUrl;
    removeTaskAt(index);
    emit queueChanged();
    m_observer->reportCancelledBeforeRun(url);
}

void TaskScheduler::dismiss(const QUrl &fileUrl)
{
    for (int i = 0; i < static_cast<int>(m_tasks.size()); ++i) {
        const Task *task = m_tasks[i].get();
        if (task->fileUrl == fileUrl
            && (task->status == Task::Status::Done || task->status == Task::Status::Failed)) {
            removeTaskAt(i);
            emit queueChanged();
            return;
        }
    }
}

void TaskScheduler::clearFinished()
{
    bool changed = false;
    for (int i = static_cast<int>(m_tasks.size()) - 1; i >= 0; --i) {
        if (m_tasks[i]->status == Task::Status::Done || m_tasks[i]->status == Task::Status::Failed) {
            removeTaskAt(i);
            changed = true;
        }
    }
    if (changed)
        emit queueChanged();
}

void TaskScheduler::cancelActive()
{
    if (m_activeIndex < 0 || m_activeIndex >= static_cast<int>(m_tasks.size()))
        return;
    cancelJob(m_tasks[m_activeIndex]->id);
}

void TaskScheduler::cancelJob(int id)
{
    for (int i = 0; i < static_cast<int>(m_tasks.size()); ++i) {
        Task *task = m_tasks[i].get();
        if (task->id != id)
            continue;
        // A Queued or Paused task has nothing running to interrupt — remove it
        // and synthesize its terminal report.
        if (task->status == Task::Status::Queued || task->status == Task::Status::Paused) {
            cancelTaskAt(i);
            return;
        }
        if (task->status == Task::Status::Active) {
            TaskRunner *runner = (task == m_instantActive) ? m_instantRunner : m_runner;
            // Enter the honest "Cancelling" state for a task that can't abort
            // mid-pass — derived from its own capability. Only the serial lane
            // has the pause UI, so only flag it there.
            if (task != m_instantActive && task->capabilities().pausable
                && !task->capabilities().cancellableMidRun) {
                m_activeCancelling = true;
                m_observer->onActiveStoppingChanged();
            }
            runner->requestCancel();
        }
        return;
    }
}

void TaskScheduler::cancelBatch(int batchId)
{
    if (batchId == 0)
        return;
    QList<int> ids;
    for (const auto &task : m_tasks) {
        if (task->batchId == batchId
            && (task->status == Task::Status::Queued || task->status == Task::Status::Paused
                || task->status == Task::Status::Active))
            ids.append(task->id);
    }
    for (const int id : std::as_const(ids))
        cancelJob(id);
}

void TaskScheduler::dismissBatch(int batchId)
{
    if (batchId == 0)
        return;
    bool changed = false;
    for (int i = static_cast<int>(m_tasks.size()) - 1; i >= 0; --i) {
        const Task *task = m_tasks[i].get();
        if (task->batchId == batchId
            && (task->status == Task::Status::Done || task->status == Task::Status::Failed)) {
            removeTaskAt(i);
            changed = true;
        }
    }
    if (changed)
        emit queueChanged();
}

void TaskScheduler::cancelAll()
{
    QList<int> queuedIds;
    for (const auto &task : m_tasks) {
        if (task->status == Task::Status::Queued || task->status == Task::Status::Paused)
            queuedIds.append(task->id);
    }
    for (const int id : std::as_const(queuedIds)) {
        const int idx = [this, id] {
            for (int i = 0; i < static_cast<int>(m_tasks.size()); ++i)
                if (m_tasks[i]->id == id)
                    return i;
            return -1;
        }();
        if (idx >= 0)
            cancelTaskAt(idx);
    }
    if (m_activeIndex >= 0) {
        Task *active = m_tasks[m_activeIndex].get();
        if (active->capabilities().pausable && !active->capabilities().cancellableMidRun) {
            m_activeCancelling = true;
            m_observer->onActiveStoppingChanged();
        }
        m_runner->requestCancel();
    }
}

void TaskScheduler::moveQueuedJob(int id, int newIndex)
{
    const TaskQuery::QueuedMove move = TaskQuery::planQueuedMove(m_tasks, id, newIndex);
    if (!move.valid)
        return;

    std::unique_ptr<Task> moved = std::move(m_tasks[move.fromIndex]);
    m_tasks.erase(m_tasks.begin() + move.fromIndex);
    if (m_activeIndex > move.fromIndex)
        --m_activeIndex;

    m_tasks.insert(m_tasks.begin() + move.insertIndex, std::move(moved));
    if (m_activeIndex >= move.insertIndex)
        ++m_activeIndex;
    emit queueChanged();
}

void TaskScheduler::enqueueCombineJob(const CombineGroup &group, const QUrl &vaultUrl, bool deleteOriginals,
                                      int insertIndex)
{
    auto task = std::make_unique<CombineTask>(group.files, QUrl::fromLocalFile(group.outputPath), deleteOriginals);
    task->id = m_nextJobId++;
    task->vaultUrl = vaultUrl;
    task->status = Task::Status::Queued;
    const int at = (insertIndex < 0) ? frontOfQueueInsertIndex() : insertIndex;
    m_tasks.insert(m_tasks.begin() + std::clamp(at, 0, static_cast<int>(m_tasks.size())), std::move(task));
}

// The scheduler-level preflight .task/plan.md flagged as the hardest port: it can
// rewrite the task's target file, insert a combine task ahead of it, and recurse —
// all strictly BEFORE dispatch, on the GUI thread, which makes mutating
// Task::fileUrl safe (run() hasn't started).
bool TaskScheduler::maybeRedirectForCombine(int index)
{
    Task *task = m_tasks[index].get();
    auto *transcribe = qobject_cast<TranscribeTask *>(task);
    if (!transcribe || transcribe->fileUrl.isEmpty())
        return false;

    const QFileInfo info(transcribe->fileUrl.toLocalFile());

    // Outcome 1: an earlier manual runCombineScan() may already have merged this
    // exact file's group. The merged output's path is recomputable from the
    // filename alone, so check for it directly.
    for (const QVariant &ruleVar : m_combineRules) {
        const QVariantMap rule = ruleVar.toMap();
        const auto key = matchRuleCapturedKey(rule, info.fileName());
        if (!key)
            continue;
        const QString outputPath = combineOutputPathFor(rule.value(QStringLiteral("name")).toString(), *key,
                                                        info.absolutePath(), info.suffix());
        if (QFileInfo::exists(outputPath))
            transcribe->fileUrl = QUrl::fromLocalFile(outputPath);
        break; // only the first matching rule is used, mirroring findCombineGroups()
    }

    // Outcome 2: this file is a member of a currently-"ready" group per the last
    // runCombineScan() scan that hasn't been merged yet — insert the combine task
    // ahead of this one and redirect.
    const QString filePath = transcribe->fileUrl.toLocalFile();
    for (const CombineGroup &group : CombineScanner::findCombineGroups(m_combineRules, m_combineAudioPaths)) {
        if (group.status != QStringLiteral("ready") || !group.files.contains(filePath))
            continue;
        transcribe->fileUrl = QUrl::fromLocalFile(group.outputPath);
        if (!isCombineQueued(group.outputPath))
            enqueueCombineJob(group, transcribe->vaultUrl, m_combineDeleteOriginals, index);
        emit queueChanged();
        startTask(index);
        return true;
    }
    return false;
}

int TaskScheduler::enqueueReadyCombineGroups(const QVariantList &rules, const QStringList &audioPaths,
                                             const QUrl &vaultUrl, bool deleteOriginals,
                                             const QString &outputPathFilter)
{
    // Cached for maybeRedirectForCombine()'s per-job safety check. Set even when
    // nothing ends up enqueued, matching the old behavior.
    m_combineRules = rules;
    m_combineAudioPaths = audioPaths;
    m_combineDeleteOriginals = deleteOriginals;

    int enqueued = 0;
    for (const CombineGroup &group : CombineScanner::findCombineGroups(rules, audioPaths)) {
        if (group.status != QStringLiteral("ready"))
            continue;
        if (!outputPathFilter.isEmpty() && group.outputPath != outputPathFilter)
            continue;
        if (isCombineQueued(group.outputPath))
            continue;
        enqueueCombineJob(group, vaultUrl, deleteOriginals);
        ++enqueued;
    }
    if (enqueued == 0)
        return 0;
    emit queueChanged();
    if (m_activeIndex < 0)
        startNext();
    return enqueued;
}

void TaskScheduler::runCombineScan(const QVariantList &rules, const QStringList &audioPaths, const QUrl &vaultUrl,
                                   bool deleteOriginals)
{
    enqueueReadyCombineGroups(rules, audioPaths, vaultUrl, deleteOriginals, /*outputPathFilter=*/QString());
}

void TaskScheduler::combineGroup(const QVariantList &rules, const QStringList &audioPaths, const QString &outputPath,
                                 const QUrl &vaultUrl, bool deleteOriginals)
{
    enqueueReadyCombineGroups(rules, audioPaths, vaultUrl, deleteOriginals, outputPath);
}

void TaskScheduler::loadPersistedQueue(const QUrl &vaultUrl)
{
    QFile file(vaultUrl.toLocalFile() + QStringLiteral("/.transcriber/tasks.json"));
    if (!file.open(QIODevice::ReadOnly))
        return;
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    if (!doc.isArray())
        return;

    // Replayed in file order — the order persistQueue() wrote. Deliberately does
    // NOT auto-start anything: every restored entry just sits Queued until the
    // user resumes via startNext(). So this appends at the tail (explicit
    // insertIndex == size) and reproduces the exact saved order.
    for (const QJsonValue &v : doc.array()) {
        const QJsonObject obj = v.toObject();
        const QString kind = obj.value("kind").toString();

        std::unique_ptr<Task> task = createTaskFromJson(kind, obj);
        if (!task)
            continue; // unrecognized kind, or one whose isPersistable() is false

        int insertAt = static_cast<int>(m_tasks.size());
        queuePrerequisites(*task, task->vaultUrl, &insertAt);

        task->id = m_nextJobId++;
        task->status = Task::Status::Queued;
        m_tasks.insert(m_tasks.begin() + std::clamp(insertAt, 0, static_cast<int>(m_tasks.size())), std::move(task));
        emit queueChanged();
    }
}

void TaskScheduler::startNext()
{
    // The QML play button's resume entry point — just the scheduler. Serial
    // Paused-before-Queued priority and the instant lane both live in pumpQueues().
    pumpQueues();
}
