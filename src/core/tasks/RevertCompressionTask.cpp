#include "RevertCompressionTask.h"

#include <QFile>
#include <QFileInfo>

#include "../VaultFileOps.h"

RevertCompressionTask::RevertCompressionTask(QUrl compressedUrl, QVariantMap trashEntry, QUrl vault, QObject *parent)
    : Task(parent)
    , m_trashEntry(std::move(trashEntry))
    , m_originalUrl(QUrl::fromLocalFile(m_trashEntry.value(QStringLiteral("originalPath")).toString()))
{
    fileUrl = std::move(compressedUrl);
    vaultUrl = std::move(vault);
}

QString RevertCompressionTask::displayName() const
{
    return QStringLiteral("Restore original: %1").arg(QFileInfo(m_originalUrl.toLocalFile()).fileName());
}

void RevertCompressionTask::run(TaskContext &ctx)
{
    const QString originalPath = m_originalUrl.toLocalFile();
    const QString compressedPath = fileUrl.toLocalFile();
    if (!QFileInfo::exists(m_trashEntry.value(QStringLiteral("trashPath")).toString()))
        throw TaskError(QStringLiteral("the original is no longer in the trash"));

    // In place: the compressed file holds the original's name until parked.
    QString parked = compressedPath;
    if (originalPath == compressedPath) {
        const QFileInfo info(compressedPath);
        parked = info.absolutePath() + QStringLiteral("/.") + info.fileName() + QStringLiteral(".reverting");
        QFile::remove(parked);
        if (!QFile::rename(compressedPath, parked))
            throw TaskError(QStringLiteral("could not move the compressed file aside"));
    } else if (QFileInfo::exists(originalPath)) {
        throw TaskError(QStringLiteral("%1 already exists").arg(QFileInfo(originalPath).fileName()));
    }

    if (restoreVaultEntries({m_trashEntry}, vaultUrl.toLocalFile()).isEmpty()) {
        if (parked != compressedPath)
            QFile::rename(parked, compressedPath);
        throw TaskError(QStringLiteral("could not restore the original from the trash"));
    }

    if (!QFile::remove(parked))
        ctx.reportWarning(QStringLiteral("Original restored, but the compressed copy could not be deleted"));
    emit audioCompressed(fileUrl, m_originalUrl);
}
