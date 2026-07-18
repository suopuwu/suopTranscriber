#include "ImportTask.h"

#include <QFile>
#include <QFileInfo>
#include <QThread>

#include <algorithm>

ImportTask::ImportTask(QString sourcePath, QUrl destUrl, QObject *parent)
    : Task(parent)
    , m_sourcePath(std::move(sourcePath))
{
    fileUrl = std::move(destUrl);
}

QString ImportTask::displayName() const
{
    return QStringLiteral("Import %1").arg(QFileInfo(fileUrl.toLocalFile()).fileName());
}

QVariantMap ImportTask::toVariantMap() const
{
    return {{QStringLiteral("outputPath"), fileUrl.toLocalFile()}};
}

void ImportTask::run(TaskContext &ctx)
{
    const QString destPath = fileUrl.toLocalFile();
    emit phase(QStringLiteral("importing"));

    QFile src(m_sourcePath);
    if (!src.open(QIODevice::ReadOnly))
        throw TaskError(QStringLiteral("could not open source file"));
    // Collision could have appeared between validation and now — never
    // silently overwrite.
    if (QFileInfo::exists(destPath))
        throw TaskError(QStringLiteral("a file with that name already exists in the vault"));
    QFile dst(destPath);
    if (!dst.open(QIODevice::WriteOnly | QIODevice::Truncate))
        throw TaskError(QStringLiteral("could not create destination file"));

    const qint64 total = src.size();
    qint64 copied = 0;
    constexpr qint64 kChunk = 1 << 20; // 1 MiB
    bool wasPaused = false;
    while (true) {
        if (ctx.cancelRequested()) {
            dst.close();
            dst.remove(); // don't leave a half-written file in the vault
            throw TaskError(QStringLiteral("Cancelled"));
        }
        if (ctx.pauseRequested()) {
            // No subprocess to SIGSTOP — just block between chunks.
            if (!wasPaused) {
                wasPaused = true;
                ctx.reportPaused(true);
            }
            QThread::msleep(100);
            continue;
        }
        if (wasPaused) {
            wasPaused = false;
            ctx.reportPaused(false);
        }
        const QByteArray buf = src.read(kChunk);
        if (buf.isEmpty())
            break; // done (or read error — treated as EOF)
        if (dst.write(buf) != buf.size()) {
            dst.close();
            dst.remove();
            throw TaskError(QStringLiteral("write failed while copying into the vault"));
        }
        copied += buf.size();
        if (total > 0)
            emit progress(std::clamp(static_cast<double>(copied) / static_cast<double>(total), 0.0, 1.0));
    }
    dst.flush();
    dst.close();
    emit progress(1.0);
}
