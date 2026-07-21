#include "ImportTask.h"

#include <QFile>
#include <QFileInfo>
#include <QThread>

#include <algorithm>

ImportTask::ImportTask(QString sourcePath, QUrl destUrl, bool overwrite, QObject *parent)
    : Task(parent)
    , m_sourcePath(std::move(sourcePath))
    , m_overwrite(overwrite)
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
    // silently overwrite, unless the caller explicitly opted into overwrite
    // (the drop handler's Overwrite/Skip snackbar action).
    if (!m_overwrite && QFileInfo::exists(destPath))
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

    // Preserve the source's mtime on the copy, exactly like UsbGrabJob's
    // restamp (and CombineTask's): the file-tree "Date Modified" sort keys off
    // the audio file's mtime to mean recording time, and QFile writes stamp the
    // copy with the time of the copy — so without this an old recording
    // imported today sorts as if it were brand new. Best-effort, matching
    // UsbGrabJob: a copy that won't reopen still imported fine.
    QFile copiedFile(destPath);
    if (copiedFile.open(QIODevice::ReadWrite)) {
        copiedFile.setFileTime(QFileInfo(m_sourcePath).lastModified(),
                               QFileDevice::FileModificationTime);
        copiedFile.close();
    }

    emit progress(1.0);
}
