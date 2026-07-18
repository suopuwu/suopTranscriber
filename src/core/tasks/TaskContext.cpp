#include "TaskContext.h"

#include <QThread>

void TaskContext::msleep100()
{
    QThread::msleep(100);
}
