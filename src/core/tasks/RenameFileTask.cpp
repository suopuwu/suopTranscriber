#include "RenameFileTask.h"

#include <QFileInfo>

#include "../FileRename.h"

RenameFileTask::RenameFileTask(QUrl file, QString newBaseName, QObject *parent)
    : Task(parent)
    , m_newBaseName(std::move(newBaseName))
{
    fileUrl = std::move(file);
}

QString RenameFileTask::displayName() const
{
    return QStringLiteral("Rename %1 → %2").arg(QFileInfo(fileUrl.toLocalFile()).fileName(), m_newBaseName);
}

QVariantMap RenameFileTask::toVariantMap() const
{
    return {{QStringLiteral("newBaseName"), m_newBaseName}};
}

QString RenameFileTask::undoLabel() const
{
    return QStringLiteral("Renamed to %1").arg(m_newBaseName);
}

std::unique_ptr<Task> RenameFileTask::makeUndoTask() const
{
    if (m_resultUrl.isEmpty())
        return nullptr; // run() hasn't succeeded — nothing to reverse
    // Rename the now-renamed file BACK to the original base name (the stem of
    // this task's original fileUrl — completeBaseName strips the last ext, so
    // "interview.wav" -> "interview" and orphaned "notes.md" -> "notes").
    const QString oldBaseName = QFileInfo(fileUrl.toLocalFile()).completeBaseName();
    return std::make_unique<RenameFileTask>(m_resultUrl, oldBaseName);
}

void RenameFileTask::run(TaskContext &ctx)
{
    Q_UNUSED(ctx);
    const QUrl oldUrl = fileUrl;
    QUrl newUrl;
    QString error;
    // Pure filesystem — safe on the worker thread (no model access). The tree
    // model refreshes itself via its QFileSystemWatcher once the files move.
    const bool ok = renameRecordingFile(oldUrl, m_newBaseName, &newUrl, &error);
    if (ok)
        m_resultUrl = newUrl; // captured for makeUndoTask()
    emit fileRenamed(oldUrl, newUrl, ok, error);
    if (!ok)
        throw TaskError(error);
}
