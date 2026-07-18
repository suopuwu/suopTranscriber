#pragma once

#include <QStringList>
#include <QUrl>
#include <QVariantList>

#include "Task.h"

// Restores a batch of trashed entries back to their original paths — the undo of
// a TrashTask (makeUndoTask() builds one of these from the trash result's
// restorable list). Instant lane, like the trash it reverses.
//
// The filesystem work (restoreVaultEntries) is pure and runs on the worker
// thread; the tree rebuild + the restore signals MainPanel reacts to
// (fileRestored/transcriptRestored/audioRestoredForTranscript) are applied by
// VaultTreeModel::applyRestoreOutcome(), which the controller calls on the GUI
// thread once run() completes. Deliberately NOT itself undoable — a restore is
// the terminus of the trash/undo cycle.
class RestoreTask : public Task {
    Q_OBJECT
public:
    // entries is a trash result's "restorable" list ({trashPath, originalPath}
    // maps); vaultUrl bounds the same containment guard the trash side used.
    RestoreTask(QVariantList entries, QUrl vaultUrl, QObject *parent = nullptr);

    QString kind() const override { return QStringLiteral("restore"); }
    QString kindLabel() const override { return QStringLiteral("Restore"); }
    QString displayName() const override;
    Lane lane() const override { return Lane::Instant; }

    void run(TaskContext &ctx) override;

    // Read on the GUI thread by the controller after run() completes.
    const QStringList &restored() const { return m_restored; }

signals:
    void restoreDone();

private:
    QVariantList m_entries;
    QStringList m_restored;
};
