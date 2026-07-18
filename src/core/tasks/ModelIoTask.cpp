#include "ModelIoTask.h"

#include "../ModelManager.h"

ModelIoTask::ModelIoTask(Direction direction, QString modelId, QUrl path, QObject *parent)
    : Task(parent)
    , m_direction(direction)
    , m_modelId(std::move(modelId))
    , m_path(std::move(path))
{
}

QString ModelIoTask::opName() const
{
    return m_direction == Direction::Import ? QStringLiteral("import") : QStringLiteral("export");
}

QString ModelIoTask::kind() const
{
    return m_direction == Direction::Import ? QStringLiteral("importModel") : QStringLiteral("exportModel");
}

QString ModelIoTask::kindLabel() const
{
    return m_direction == Direction::Import ? QStringLiteral("Import model") : QStringLiteral("Export model");
}

QString ModelIoTask::displayName() const
{
    return (m_direction == Direction::Import ? QStringLiteral("Import model %1") : QStringLiteral("Export model %1"))
        .arg(m_modelId);
}

QVariantMap ModelIoTask::toVariantMap() const
{
    return {{QStringLiteral("modelId"), m_modelId}};
}

void ModelIoTask::run(TaskContext &ctx)
{
    Q_UNUSED(ctx);
    // Blocking multi-GB copy — safe here (worker thread). No progress callback
    // in the ModelManager API, so the Tasks row/glow just shows Active for the
    // copy's duration (still strictly better than the old row-less thread).
    const QString localPath = m_path.toLocalFile();
    QString error;
    const bool ok = (m_direction == Direction::Import) ? ModelManager::importModel(m_modelId, localPath, &error)
                                                       : ModelManager::exportModel(m_modelId, localPath, &error);
    emit modelIoFinished(opName(), m_modelId, ok, error);
    if (!ok)
        throw TaskError(error);
}
