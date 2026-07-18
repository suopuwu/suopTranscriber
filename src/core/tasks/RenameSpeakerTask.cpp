#include "RenameSpeakerTask.h"

#include "../TranscriptionController.h"

RenameSpeakerTask::RenameSpeakerTask(QUrl audioUrl, QString oldName, QString newName, QObject *parent)
    : Task(parent)
    , m_oldName(std::move(oldName))
    , m_newName(std::move(newName))
{
    fileUrl = std::move(audioUrl);
}

QString RenameSpeakerTask::displayName() const
{
    return QStringLiteral("Rename speaker \"%1\" → \"%2\"").arg(m_oldName, m_newName);
}

QJsonObject RenameSpeakerTask::toJson() const
{
    return {{QStringLiteral("oldSpeakerName"), m_oldName}, {QStringLiteral("newSpeakerName"), m_newName}};
}

QVariantMap RenameSpeakerTask::toVariantMap() const
{
    return {{QStringLiteral("oldSpeakerName"), m_oldName}, {QStringLiteral("newSpeakerName"), m_newName}};
}

void RenameSpeakerTask::run(TaskContext &ctx)
{
    Q_UNUSED(ctx);
    const QString audioPath = fileUrl.toLocalFile();
    QString error;
    const bool ok = renameSpeakerInFile(sidecarPath(audioPath), m_oldName, m_newName, &error);
    // Reported against the audio path (not the .md sidecar), matching the old
    // finished()/speakerRenamed() convention QML's Connections target
    // compares against.
    emit speakerRenamed(audioPath, m_oldName, ok, error);
    if (!ok)
        throw TaskError(error);
}
