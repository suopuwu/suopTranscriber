#include "TaskRunner.h"

#include "Task.h"

TaskRunner::TaskRunner(QObject *parent)
    : QObject(parent)
{
    // See Task.h's Q_DECLARE_METATYPE(Task *) comment — this is the runtime
    // half of registering Task* for queued cross-thread delivery.
    qRegisterMetaType<Task *>("Task*");
}

void TaskRunner::execute(Task *task)
{
    TaskContext ctx(m_engines, m_control, [task](bool isPaused) { emit task->paused(isPaused); });

    bool ok = true;
    QString error;
    try {
        task->run(ctx);
    } catch (const TaskError &e) {
        ok = false;
        error = e.message();
    }
    emit taskFinished(task, ok, error);
}
