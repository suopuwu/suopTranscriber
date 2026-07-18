#include "RestoreTask.h"

#include "../VaultFileOps.h"

RestoreTask::RestoreTask(QVariantList entries, QUrl vault, QObject *parent)
    : Task(parent)
    , m_entries(std::move(entries))
{
    vaultUrl = std::move(vault);
    // Key the Tasks row / same-file gate on the primary file being restored —
    // the first entry's originalPath (the audio/primary, since trash records it
    // before its sidecars). Without this the undo row would show no filename.
    for (const QVariant &v : m_entries) {
        const QString originalPath = v.toMap().value(QStringLiteral("originalPath")).toString();
        if (!originalPath.isEmpty()) {
            fileUrl = QUrl::fromLocalFile(originalPath);
            break;
        }
    }
}

QString RestoreTask::displayName() const
{
    return QStringLiteral("Restore %1 item(s) from trash").arg(m_entries.size());
}

void RestoreTask::run(TaskContext &ctx)
{
    Q_UNUSED(ctx);
    // Pure filesystem — safe on the worker thread. The model rebuild + restore
    // signals are applied on the GUI thread via applyRestoreOutcome().
    m_restored = restoreVaultEntries(m_entries, vaultUrl.toLocalFile());
    emit restoreDone();
}
