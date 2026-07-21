#pragma once

#include <QStringList>
#include <QUrl>

#include "Task.h"

// The inverse of a CombineTask: moves each archived part back out of the
// sibling .combined/ dir to the path it was combined from, then removes the
// merged output (regenerable losslessly by re-running the combine). Instant
// lane — a handful of QFile::rename syscalls, same shape as MoveTask; the tree
// model refreshes itself via its QFileSystemWatcher as the parts reappear.
//
// Only ever built by CombineTask::makeUndoTask() (never enqueued directly, and
// never persisted — an in-session undo). fileUrl is the merged OUTPUT, matching
// CombineTask's use of the same field.
class UncombineTask : public Task {
    Q_OBJECT
public:
    // originalPaths are the pre-combine source paths (each part's archived copy
    // is <dir>/.combined/<filename>); outputUrl is the merged file to remove.
    UncombineTask(QStringList originalPaths, QUrl outputUrl, QUrl vaultUrl, QObject *parent = nullptr);

    QString kind() const override { return QStringLiteral("uncombine"); }
    QString kindLabel() const override { return QStringLiteral("Uncombine"); }
    QString displayName() const override;
    Lane lane() const override { return Lane::Instant; }
    // Moves files on disk -> the watcher rebuild resets the model, which is the
    // authoritative reflow (see Task::isStructuralFileOp).
    bool isStructuralFileOp() const override { return true; }
    QVariantMap toVariantMap() const override;

    void run(TaskContext &ctx) override;

private:
    QStringList m_originalPaths;
};
