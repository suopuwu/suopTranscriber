#pragma once

#include <QList>
#include <QUrl>

#include "../VaultFileOps.h"
#include "Task.h"

// Permanently (IRREVERSIBLY) deletes a batch of vault entries — the counterpart
// to TrashTask, only ever reached after the user confirms the entries the
// filesystem refused to trash. Same VaultFileScope variants, same Instant lane.
//
// The filesystem work (deleteVaultEntriesPermanently) is pure and runs on the
// worker thread; the tree rebuild + per-file delete signals are applied by
// VaultTreeModel::applyRemovalOutcome() on the GUI thread once run() completes.
// NOT undoable — the files are gone.
class DeleteTask : public Task {
    Q_OBJECT
public:
    DeleteTask(VaultFileScope scope, QList<QUrl> urls, QUrl vaultUrl, QObject *parent = nullptr);

    QString kind() const override { return QStringLiteral("delete"); }
    QString kindLabel() const override { return QStringLiteral("Delete"); }
    QString displayName() const override;
    Lane lane() const override { return Lane::Instant; }

    void run(TaskContext &ctx) override;

    // Read on the GUI thread by the controller after run() completes.
    VaultFileScope scope() const { return m_scope; }
    const QList<RemovedEntry> &removed() const { return m_removed; }

signals:
    void deleteDone();

private:
    VaultFileScope m_scope;
    QList<QUrl> m_urls;
    QList<RemovedEntry> m_removed;
};
