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
    // Creates/moves files in the vault -> the watcher rebuild resets the model,
    // which is the authoritative reflow (see Task::isStructuralFileOp).
    bool isStructuralFileOp() const override { return true; }
    QString kindLabel() const override { return QStringLiteral("Combine"); }
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

    // Undoable in the default (move-to-.combined/) mode: the inverse moves the
    // archived parts back and removes the merge. NOT undoable when
    // deleteOriginals is on — the parts are gone, so no inverse exists and both
    // overrides deliberately return empty/null (the snackbar and the Tasks-row
    // Undo button both gate on that pair; see Task::isUndoable()).
    QString undoLabel() const override;
    std::unique_ptr<Task> makeUndoTask() const override;
    bool undoStillApplicable() const override;

    // fileUrl is the OUTPUT, so the default (fileUrl alone) would report a
    // combine as touching a file that does not exist yet and NONE of the files
    // it actually reads and then archives away. The sources are the ones that
    // must not be a take still being recorded.
    QList<QUrl> touchedFiles() const override;

    void run(TaskContext &ctx) override;

    const QStringList &sourcePaths() const { return m_sourcePaths; }

signals:
    // Emitted on a SUCCESSFUL merge (run() reached the end without throwing), so
    // the controller can write a durable combine-history entry from the GUI
    // thread — this task runs on a worker and must not touch the GUI-owned
    // VaultTreeModel itself (the same split as the USB grab history). Carries
    // what the log needs: the merged output, the parts that went into it (in
    // combine order), and whether the originals were archived into .combined/ or
    // deleted. This is the durable account the in-session Tasks row and undo
    // can't give — an auto-combine that moved the user's originals must survive
    // a restart as more than a vanished snackbar.
    void combineCompleted(const QString &outputPath, const QStringList &sourcePaths, bool deleteOriginals);

private:
    QStringList m_sourcePaths;
    bool m_deleteOriginals;
    // The sources actually archived into .combined/ by a successful run(), so
    // makeUndoTask() reverses exactly what happened (empty until then, and
    // always empty when m_deleteOriginals). Same capture-in-run() shape as
    // TrashTask::m_result.
    QStringList m_archivedSources;
};
