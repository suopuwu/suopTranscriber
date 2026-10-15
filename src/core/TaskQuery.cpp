#include "TaskQuery.h"

#include "FileRename.h" // isSameRecording()
#include "tasks/DownloadTask.h"
#include "tasks/Task.h"

#include <QJsonObject>
#include <QVector>

#include <algorithm>

int TaskQuery::indexOfTask(const TaskList &tasks, const Task *task)
{
    for (int i = 0; i < static_cast<int>(tasks.size()); ++i) {
        if (tasks[i].get() == task)
            return i;
    }
    return -1;
}

bool TaskQuery::batchHasMembers(const TaskList &tasks, int batchId)
{
    if (batchId == 0)
        return false;
    for (const auto &task : tasks)
        if (task->batchId == batchId)
            return true;
    return false;
}

int TaskQuery::frontOfQueueInsertIndex(const TaskList &tasks, int activeIndex)
{
    if (activeIndex >= 0)
        return activeIndex + 1;
    for (int i = 0; i < static_cast<int>(tasks.size()); ++i) {
        if (tasks[i]->status == Task::Status::Queued)
            return i;
    }
    return static_cast<int>(tasks.size());
}

int TaskQuery::tasksTotal(const TaskList &tasks)
{
    return static_cast<int>(tasks.size());
}

int TaskQuery::tasksCompleted(const TaskList &tasks)
{
    int count = 0;
    for (const auto &task : tasks) {
        if (task->status == Task::Status::Done || task->status == Task::Status::Failed)
            ++count;
    }
    return count;
}

QVariantList TaskQuery::queuedFileUrls(const TaskList &tasks)
{
    QVariantList list;
    for (const auto &task : tasks) {
        if (task->status == Task::Status::Queued && task->occupiesFile())
            list.append(task->fileUrl);
    }
    return list;
}

bool TaskQuery::isQueued(const TaskList &tasks, const QUrl &fileUrl)
{
    for (const auto &task : tasks) {
        if (!task->occupiesFile())
            continue; // a benchmark/sweep on this file doesn't block a real op
        if (task->fileUrl == fileUrl
            && (task->status == Task::Status::Queued || task->status == Task::Status::Active
                || task->status == Task::Status::Paused))
            return true;
    }
    return false;
}

bool TaskQuery::otherLiveTaskTouches(const TaskList &tasks, const Task *self)
{
    const QList<QUrl> mine = self->touchedFiles();
    for (const auto &task : tasks) {
        if (task.get() == self
            || (task->status != Task::Status::Queued && task->status != Task::Status::Active
                && task->status != Task::Status::Paused))
            continue;
        for (const QUrl &theirs : task->touchedFiles()) {
            for (const QUrl &url : mine) {
                if (isSameRecording(theirs, url))
                    return true;
            }
        }
    }
    return false;
}

bool TaskQuery::isKindQueued(const TaskList &tasks, const QUrl &fileUrl, const QString &kind)
{
    for (const auto &task : tasks) {
        if (!task->occupiesFile())
            continue;
        if (task->fileUrl == fileUrl && task->kind() == kind
            && (task->status == Task::Status::Queued || task->status == Task::Status::Active
                || task->status == Task::Status::Paused))
            return true;
    }
    return false;
}

int TaskQuery::insertIndexAfterFileTasks(const TaskList &tasks, const QUrl &fileUrl)
{
    int last = -1;
    for (int i = 0; i < static_cast<int>(tasks.size()); ++i) {
        const auto &task = tasks[i];
        if (!task->occupiesFile())
            continue;
        if (task->fileUrl == fileUrl
            && (task->status == Task::Status::Queued || task->status == Task::Status::Active
                || task->status == Task::Status::Paused))
            last = i;
    }
    return last < 0 ? -1 : last + 1;
}

bool TaskQuery::isCombineQueued(const TaskList &tasks, const QString &outputPath)
{
    const QUrl outputUrl = QUrl::fromLocalFile(outputPath);
    for (const auto &task : tasks) {
        if (task->kind() == QStringLiteral("combine") && task->fileUrl == outputUrl
            && (task->status == Task::Status::Queued || task->status == Task::Status::Active))
            return true;
    }
    return false;
}

bool TaskQuery::isDownloadQueued(const TaskList &tasks, ModelDownloadTarget target, const QUrl &vaultUrl,
                                 const QString &modelId)
{
    for (const auto &task : tasks) {
        auto *dl = qobject_cast<DownloadTask *>(task.get());
        if (!dl || dl->target() != target || dl->vaultUrl != vaultUrl)
            continue;
        const bool idMatters = target == ModelDownloadTarget::WhisperModel
            || target == ModelDownloadTarget::SegmentationModel || target == ModelDownloadTarget::EmbeddingModel;
        if ((!idMatters || dl->modelId() == modelId)
            && (task->status == Task::Status::Queued || task->status == Task::Status::Active))
            return true;
    }
    return false;
}

TaskQuery::QueuedMove TaskQuery::planQueuedMove(const TaskList &tasks, int id, int newIndex)
{
    int fromRealIndex = -1;
    for (int i = 0; i < static_cast<int>(tasks.size()); ++i) {
        if (tasks[i]->id == id && tasks[i]->status == Task::Status::Queued) {
            fromRealIndex = i;
            break;
        }
    }
    if (fromRealIndex < 0)
        return {};

    // Real (tasks) indices of every Queued entry, in current order — the
    // ordering moveQueuedJob()'s newIndex counts within (Active/Done/Failed
    // rows are never part of it).
    QVector<int> queuedIndices;
    for (int i = 0; i < static_cast<int>(tasks.size()); ++i) {
        if (tasks[i]->status == Task::Status::Queued)
            queuedIndices.append(i);
    }
    const int fromQueuedPos = queuedIndices.indexOf(fromRealIndex);
    const int clampedNewIndex = std::clamp(newIndex, 0, static_cast<int>(queuedIndices.size()) - 1);
    if (clampedNewIndex == fromQueuedPos)
        return {};

    // Recompute the remaining Queued real-indices as they will be AFTER the
    // removal shifts everything past fromRealIndex down by one, then name the
    // target Queued slot's (then-current) real index — or the very end of
    // `tasks` if newIndex names the last Queued slot.
    queuedIndices.removeAt(fromQueuedPos);
    for (int &idx : queuedIndices) {
        if (idx > fromRealIndex)
            --idx;
    }
    // Note the -1: every index here is expressed in POST-erase terms, and
    // `tasks` is still the pre-erase list, so "the end" is size() - 1.
    const int insertRealIndex = clampedNewIndex >= queuedIndices.size() ? static_cast<int>(tasks.size()) - 1
                                                                       : queuedIndices[clampedNewIndex];
    return {true, fromRealIndex, insertRealIndex};
}

QJsonArray TaskQuery::serializeQueue(const TaskList &tasks)
{
    QJsonArray array;
    for (const auto &task : tasks) {
        // Only Queued survives a restart — an Active OR Paused task is
        // persisted AS Queued (no true mid-file resume yet); Done/Failed
        // history is session-only by design.
        if (task->status != Task::Status::Queued && task->status != Task::Status::Active
            && task->status != Task::Status::Paused)
            continue;

        // Persistence is OPT-IN: import/usbGrab (external/removable paths)
        // return isPersistable() == false and are simply never written — see
        // the header doc comment on why this replaced the old opt-OUT list.
        if (!task->isPersistable())
            continue;

        // The queue owns kind/fileUrl/vaultUrl; toJson() adds whatever this
        // kind needs to rebuild itself (createTaskFromJson()).
        QJsonObject obj = task->toJson();
        obj["kind"] = task->kind();
        obj["fileUrl"] = task->fileUrl.toString();
        obj["vaultUrl"] = task->vaultUrl.toString();
        array.append(obj);
    }
    return array;
}
