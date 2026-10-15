#pragma once

#include <QUrl>
#include <QVariantMap>

#include "Task.h"

// The undo of a CompressAudioTask: restores the original from the trash, then
// deletes the compressed file. The original comes back before the compressed
// file is deleted — if the restore fails (trash emptied, name taken), the
// compressed file is all that's left, so it stays. For an in-place FLAC
// replace the compressed file is parked under a hidden name first to free the
// path. Instant lane: renames and an unlink. Not itself undoable — the encode
// it reverses is a re-run of the compress action away.
class RevertCompressionTask : public Task {
    Q_OBJECT
public:
    // trashEntry: {trashPath, originalPath}, as captured by the compress.
    RevertCompressionTask(QUrl compressedUrl, QVariantMap trashEntry, QUrl vaultUrl, QObject *parent = nullptr);

    QString kind() const override { return QStringLiteral("revertCompression"); }
    QString kindLabel() const override { return QStringLiteral("Restore original"); }
    QString displayName() const override;
    Lane lane() const override { return Lane::Instant; }
    bool isStructuralFileOp() const override { return true; }
    QList<QUrl> touchedFiles() const override { return {fileUrl, m_originalUrl}; }

    void run(TaskContext &ctx) override;

signals:
    // Same contract as CompressAudioTask::audioCompressed: oldUrl (the
    // compressed file) is replaced by newUrl (the original).
    void audioCompressed(const QUrl &oldUrl, const QUrl &newUrl);

private:
    QVariantMap m_trashEntry;
    QUrl m_originalUrl;
};
