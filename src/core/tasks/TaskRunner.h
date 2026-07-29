#pragma once

#include <QObject>
#include <QString>

#include "EngineCache.h"
#include "TaskControl.h"

class Task;

// Thread host for Task::run(). Replaces TranscriptionWorker: owns the warm
// EngineCache (so model reuse survives across tasks the way it always did)
// and the ONE TaskControl shared with whichever task is currently Active —
// only one task ever runs at a time, same invariant as the old worker.
//
// Lives on the worker thread (moveToThread'd by the queue, same pattern as
// the old TranscriptionWorker construction). `execute()` is the only slot;
// everything else is called directly and cross-thread, same immediate,
// non-queued contract TaskControl documents (a pause/cancel must land NOW,
// not wait behind whatever's running on this thread's event loop).
class TaskRunner : public QObject {
    Q_OBJECT
public:
    explicit TaskRunner(QObject *parent = nullptr);

    void requestPause() { m_control.requestPause(); }
    void requestResume() { m_control.requestResume(); }
    void requestCancel() { m_control.requestCancel(); }
    bool isCancelRequested() const { return m_control.cancelRequested(); }
    // Called by the queue right before dispatching the next task, so a
    // pause/cancel left over from whatever the PREVIOUS task did never leaks
    // into this one.
    void resetControl() { m_control.reset(); }

    // Read-only access to this runner's own engine cache — exposed purely so
    // a test can verify the residency policy actually fires
    // through the REAL dispatch path (execute(), below) by reading the
    // eviction counters after driving real tasks through a TaskScheduler,
    // rather than only against a bare EngineCache/Task in isolation. Safe to
    // read cross-thread once the caller has observed the corresponding
    // task's completion (the queued taskFinished signal happens-after
    // execute() returns).
    const EngineCache &engines() const { return m_engines; }

public slots:
    // Release every warm engine — called when the queue drains, so a resident
    // model doesn't sit on VRAM indefinitely. A slot (not a plain method)
    // because the controller invokes it via a QUEUED cross-thread call so the
    // eviction runs ON the worker thread, right after the last task's run()
    // returns rather than racing it from the GUI thread.
    void evictIdle() { m_engines.evictAll(); }

    // Applies this task's engine residency policy (EngineCache::
    // applyResidencyPolicy(), driven by task->engineNeeds()), then runs
    // task->run() against a TaskContext wired to this runner's engine cache
    // and control, then emits EXACTLY ONE taskFinished — whether run()
    // returned normally or threw TaskError. Ownership of `task` stays with
    // the caller (the queue); this only borrows the pointer for the call.
    void execute(Task *task);

signals:
    // `warning` is whatever the task reported via TaskContext::reportWarning()
    // during run() (empty for the overwhelming majority) — carried alongside
    // ok/error so the scheduler can thread it onto Task::warning the same way
    // it threads `error` onto Task::error, regardless of whether run() threw.
    void taskFinished(Task *task, bool ok, const QString &error, const QString &warning);

private:
    EngineCache m_engines;
    TaskControl m_control;
};
