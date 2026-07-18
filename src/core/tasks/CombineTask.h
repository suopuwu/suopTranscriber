#pragma once

#include <QString>
#include <QStringList>
#include <QUrl>

#include "Task.h"

// Concatenates sourcePaths (already in combine order) into the merged output
// via ffmpeg's concat demuxer with stream copy (lossless, fast — same-format
// parts from the same recorder), then moves (or deletes, per
// deleteOriginals) each source into a sibling .combined/ dir so the next
// vault scan doesn't re-detect the same group. This kind's `fileUrl` is the
// OUTPUT path (see Task::fileUrl's doc comment on that convention).
class CombineTask : public Task {
    Q_OBJECT
public:
    CombineTask(QStringList sourcePaths, QUrl outputUrl, bool deleteOriginals, QObject *parent = nullptr);

    QString kind() const override { return QStringLiteral("combine"); }
    QString displayName() const override;
    // capabilities: pausable via SIGSTOP/SIGCONT on the ffmpeg subprocess —
    // a real, killable/pausable process, same shape as decode. See
    // pauseActive()'s old allowlist (transcribe/download/combine/chapters).
    Capabilities capabilities() const override { return {/*pausable=*/true, /*cancellableMidRun=*/true}; }
    // Persisted in the old system (combine was never in persistQueue()'s
    // import/usbGrab opt-out list).
    bool isPersistable() const override { return true; }
    QJsonObject toJson() const override;
    QVariantMap toVariantMap() const override;

    void run(TaskContext &ctx) override;

    const QStringList &sourcePaths() const { return m_sourcePaths; }

private:
    QStringList m_sourcePaths;
    bool m_deleteOriginals;
};
