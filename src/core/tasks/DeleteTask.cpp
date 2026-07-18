#include "DeleteTask.h"

#include <QFileInfo>

DeleteTask::DeleteTask(VaultFileScope scope, QList<QUrl> urls, QUrl vault, QObject *parent)
    : Task(parent)
    , m_scope(scope)
    , m_urls(std::move(urls))
{
    vaultUrl = std::move(vault);
    if (!m_urls.isEmpty())
        fileUrl = m_urls.first();
}

QString DeleteTask::displayName() const
{
    const int n = m_urls.size();
    switch (m_scope) {
    case VaultFileScope::Folder:
        return QStringLiteral("Delete folder %1").arg(QFileInfo(fileUrl.toLocalFile()).fileName());
    case VaultFileScope::Transcript:
        return QStringLiteral("Delete %1 transcript(s)").arg(n);
    case VaultFileScope::AudioOnly:
        return QStringLiteral("Delete %1 audio file(s)").arg(n);
    case VaultFileScope::Files:
        break;
    }
    return QStringLiteral("Delete %1 item(s)").arg(n);
}

void DeleteTask::run(TaskContext &ctx)
{
    Q_UNUSED(ctx);
    // Pure filesystem — safe on the worker thread. Irreversible. The model
    // rebuild + per-file signals are applied on the GUI thread via
    // applyRemovalOutcome().
    m_removed = deleteVaultEntriesPermanently(m_urls, m_scope, vaultUrl.toLocalFile());
    emit deleteDone();
}
