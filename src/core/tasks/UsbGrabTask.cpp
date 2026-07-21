#include "UsbGrabTask.h"

#include <QThread>

#include <algorithm>

#include "../UsbGrabJob.h"

UsbGrabTask::UsbGrabTask(UsbGrabRule rule, QString mountPath, QUrl destUrl, bool autoTriggered,
                         QVariantList combineRules, QObject *parent)
    : Task(parent)
    , m_rule(std::move(rule))
    , m_mountPath(std::move(mountPath))
    , m_autoTriggered(autoTriggered)
    , m_combineRules(std::move(combineRules))
{
    fileUrl = std::move(destUrl);
}

QString UsbGrabTask::displayName() const
{
    return QStringLiteral("USB grab: %1")
        .arg(m_rule.deviceLabel.isEmpty() ? m_rule.deviceSerial : m_rule.deviceLabel);
}

QVariantMap UsbGrabTask::toVariantMap() const
{
    return {{QStringLiteral("outputPath"), fileUrl.toLocalFile()},
            {QStringLiteral("ruleName"), m_rule.deviceLabel.isEmpty() ? m_rule.deviceSerial : m_rule.deviceLabel},
            {QStringLiteral("fileCount"), m_fileCount.load()}};
}

void UsbGrabTask::run(TaskContext &ctx)
{
    emit phase(QStringLiteral("grabbing"));

    bool wasPaused = false;
    UsbGrabScanHooks hooks;
    hooks.onProgress = [this](qint64 bytesDone, qint64 bytesTotal) {
        if (bytesTotal > 0)
            emit progress(std::clamp(static_cast<double>(bytesDone) / static_cast<double>(bytesTotal), 0.0, 1.0));
    };
    hooks.onTotalsKnown = [this](int totalFiles) {
        m_fileCount.store(totalFiles);
        emit fileCountKnown();
    };
    hooks.shouldAbort = [&ctx, &wasPaused] {
        // Between files, not between bytes: a grab's unit of work is one
        // file, and QFile::copy() is uninterruptible anyway, so a pause or
        // cancel lands at the next file boundary. Mirrors ImportTask's loop
        // — no subprocess to SIGSTOP, so just block here.
        while (ctx.pauseRequested() && !ctx.cancelRequested()) {
            if (!wasPaused) {
                wasPaused = true;
                ctx.reportPaused(true);
            }
            QThread::msleep(100);
        }
        if (wasPaused) {
            wasPaused = false;
            ctx.reportPaused(false);
        }
        return ctx.cancelRequested();
    };

    const UsbGrabScanResult result =
        runUsbGrabScan(m_rule, m_mountPath, fileUrl.toLocalFile(), /*dryRun=*/false, hooks, m_combineRules);

    if (!result.refusal.isEmpty()) {
        // A refusal means the walk never ran — a genuinely failed task,
        // unlike a per-file copy error.
        emit grabCompleted(0, 0, QStringList{result.refusal}, false);
        throw TaskError(result.refusal);
    }

    emit grabCompleted(result.filesCopied, result.filesSkipped, result.errors, result.aborted);
    if (result.aborted)
        throw TaskError(QStringLiteral("Cancelled"));
    emit progress(1.0);
}
