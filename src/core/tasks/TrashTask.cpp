#include "TrashTask.h"

#include <QFileInfo>

#include "RestoreTask.h"

TrashTask::TrashTask(VaultFileScope scope, QList<QUrl> urls, QUrl vault, QObject *parent)
    : Task(parent)
    , m_scope(scope)
    , m_urls(std::move(urls))
{
    vaultUrl = std::move(vault);
    if (!m_urls.isEmpty())
        fileUrl = m_urls.first();
}

QString TrashTask::displayName() const
{
    const int n = m_urls.size();
    switch (m_scope) {
    case VaultFileScope::Folder:
        return QStringLiteral("Trash folder %1").arg(QFileInfo(fileUrl.toLocalFile()).fileName());
    case VaultFileScope::Transcript:
        return QStringLiteral("Trash %1 transcript(s)").arg(n);
    case VaultFileScope::AudioOnly:
        return QStringLiteral("Trash %1 audio file(s)").arg(n);
    case VaultFileScope::Files:
        break;
    }
    return QStringLiteral("Trash %1 item(s)").arg(n);
}

QString TrashTask::undoLabel() const
{
    const int n = m_result.trashed.size();
    if (n == 0)
        return {}; // nothing landed in the trash — not undoable
    const QString firstName =
        QFileInfo(m_result.trashed.first().first.toLocalFile()).fileName();
    switch (m_scope) {
    case VaultFileScope::Folder:
        return QStringLiteral("Moved folder %1 to trash").arg(firstName);
    case VaultFileScope::Transcript:
        return n == 1 ? QStringLiteral("Moved a transcript to trash")
                      : QStringLiteral("Moved %1 transcripts to trash").arg(n);
    case VaultFileScope::AudioOnly:
        return n == 1 ? QStringLiteral("Moved audio to trash — transcript kept")
                      : QStringLiteral("Moved %1 audio files to trash").arg(n);
    case VaultFileScope::Files:
        break;
    }
    return n == 1 ? QStringLiteral("Moved %1 to trash").arg(firstName)
                  : QStringLiteral("Moved %1 items to trash").arg(n);
}

std::unique_ptr<Task> TrashTask::makeUndoTask() const
{
    if (m_result.restorable.isEmpty())
        return nullptr;
    return std::make_unique<RestoreTask>(m_result.restorable, vaultUrl);
}

void TrashTask::run(TaskContext &ctx)
{
    Q_UNUSED(ctx);
    // Pure filesystem — safe on the worker thread. Untrashable entries are the
    // expected non-destructive case (they stay on disk for the caller to
    // confirm a permanent delete), NOT an error, so this never throws. The
    // model rebuild + per-file signals happen via applyRemovalOutcome() on the
    // GUI thread (see the class doc); the tree also refreshes via the
    // QFileSystemWatcher regardless.
    m_result = trashVaultEntries(m_urls, m_scope, vaultUrl.toLocalFile());
    emit trashDone();
}
