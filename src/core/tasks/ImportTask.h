#pragma once

#include <QString>
#include <QUrl>

#include "Task.h"

// Drag-and-drop / external audio import: copies sourcePath into this kind's
// `fileUrl` (the in-vault destination, already collision-checked on the GUI
// thread — see Task::fileUrl's OUTPUT-path convention) off the GUI thread via
// a chunked read/write loop, which is what lets it report byte progress and
// honor pause/cancel between chunks.
//
// `overwrite` (opt-in, from the drop handler's Overwrite/Skip snackbar) lifts
// the collision guard in run() and lets the copy truncate the existing
// destination. It replaces the audio bytes ONLY — a pre-existing sibling
// `.md` transcript is left untouched (import never owned it; the destructive
// alternative was considered and NOT chosen).
class ImportTask : public Task {
    Q_OBJECT
public:
    ImportTask(QString sourcePath, QUrl destUrl, bool overwrite = false, QObject *parent = nullptr);

    QString kind() const override { return QStringLiteral("import"); }
    // Creates/moves files in the vault -> the watcher rebuild resets the model,
    // which is the authoritative reflow (see Task::isStructuralFileOp).
    bool isStructuralFileOp() const override { return true; }
    QString kindLabel() const override { return QStringLiteral("Import"); }
    QString displayName() const override;
    // Pausable between chunks (no subprocess to SIGSTOP, so the task just
    // blocks itself — see run()).
    Capabilities capabilities() const override { return {/*pausable=*/true, /*cancellableMidRun=*/true}; }
    // Background, NOT the Foreground default: an import is a byte copy into a
    // file that has no transcript yet, so the main-panel overlay would block
    // the panel for the whole copy (minutes on a long file) while showing the
    // user nothing — and it would never lift, since the overlay is cleared by
    // the first live segment and an import emits none. Same reasoning as
    // BenchmarkTask/DownloadTask/ModelIoTask: the Tasks row + bottom-bar glow
    // + progress are the right surface. See MainPanel.qml's
    // transcribingThisFile / activeIsForeground.
    Presentation presentation() const override { return Presentation::Background; }
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
    bool m_overwrite = false;
};
