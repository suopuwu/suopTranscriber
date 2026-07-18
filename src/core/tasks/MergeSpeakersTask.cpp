#include "MergeSpeakersTask.h"

#include "../TranscriptionController.h"

MergeSpeakersTask::MergeSpeakersTask(QUrl audioUrl, QString sourceName, QString targetName, QObject *parent)
    : Task(parent)
    , m_sourceName(std::move(sourceName))
    , m_targetName(std::move(targetName))
{
    fileUrl = std::move(audioUrl);
}

QString MergeSpeakersTask::displayName() const
{
    return QStringLiteral("Merge speaker \"%1\" into \"%2\"").arg(m_sourceName, m_targetName);
}

QJsonObject MergeSpeakersTask::toJson() const
{
    return {{QStringLiteral("sourceSpeakerName"), m_sourceName}, {QStringLiteral("targetSpeakerName"), m_targetName}};
}

QVariantMap MergeSpeakersTask::toVariantMap() const
{
    return {{QStringLiteral("sourceSpeakerName"), m_sourceName}, {QStringLiteral("targetSpeakerName"), m_targetName}};
}

void MergeSpeakersTask::run(TaskContext &ctx)
{
    Q_UNUSED(ctx);
    const QString audioPath = fileUrl.toLocalFile();
    QString error;
    const bool ok = mergeSpeakersInFile(sidecarPath(audioPath), m_sourceName, m_targetName, &error);
    emit speakersMerged(audioPath, m_sourceName, m_targetName, ok, error);
    if (!ok)
        throw TaskError(error);
}
