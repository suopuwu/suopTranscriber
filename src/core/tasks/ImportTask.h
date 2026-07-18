#pragma once

#include <QString>
#include <QUrl>

#include "Task.h"

// Drag-and-drop / external audio import: copies sourcePath into this kind's
// `fileUrl` (the in-vault destination, already collision-checked on the GUI
// thread — see Task::fileUrl's OUTPUT-path convention) off the GUI thread via
// a chunked read/write loop, which is what lets it report byte progress and
// honor pause/cancel between chunks.
class ImportTask : public Task {
    Q_OBJECT
public:
    ImportTask(QString sourcePath, QUrl destUrl, QObject *parent = nullptr);

    QString kind() const override { return QStringLiteral("import"); }
    QString kindLabel() const override { return QStringLiteral("Import"); }
    QString displayName() const override;
    // Pausable between chunks (no subprocess to SIGSTOP, so the task just
    // blocks itself — see run()).
    Capabilities capabilities() const override { return {/*pausable=*/true, /*cancellableMidRun=*/true}; }
    // NOT persisted (old persistQueue()'s explicit opt-out): m_sourcePath
    // names an EXTERNAL file (often removable media) that may be gone next
    // session, and a half-done copy left a partial file the collision check
    // would then refuse. A drag-and-drop import is inherently live/
    // in-session — if it didn't finish, the user just drags the file in
    // again. isPersistable() stays false, so toJson() is never called.
    QVariantMap toVariantMap() const override;

    void run(TaskContext &ctx) override;

    const QString &sourcePath() const { return m_sourcePath; }

private:
    QString m_sourcePath;
};
