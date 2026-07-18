#pragma once

#include <QString>
#include <QUrl>

#include "Task.h"

// Relabels every "[sourceName]" body tag in one already-transcribed .md to
// "[targetName]" and drops sourceName from the "speakers:" allowlist — see
// mergeSpeakersInFile()'s doc comment in TranscriptionController.h. Unlike
// RenameSpeakerTask, deliberately ALLOWS colliding with an existing name
// (that's the point: collapsing two over-split diarized speakers into one).
class MergeSpeakersTask : public Task {
    Q_OBJECT
public:
    MergeSpeakersTask(QUrl audioUrl, QString sourceName, QString targetName, QObject *parent = nullptr);

    QString kind() const override { return QStringLiteral("merge"); }
    QString kindLabel() const override { return QStringLiteral("Merge speakers"); }
    QString displayName() const override;
    // Instant lane — see RenameSpeakerTask (same single-.md-rewrite reasoning).
    Lane lane() const override { return Lane::Instant; }
    // isPersistable(): see RenameSpeakerTask's doc comment — same reasoning.
    bool isPersistable() const override { return true; }
    QJsonObject toJson() const override;
    QVariantMap toVariantMap() const override;

    void run(TaskContext &ctx) override;

    const QString &sourceName() const { return m_sourceName; }
    const QString &targetName() const { return m_targetName; }

signals:
    // Fires on BOTH success and failure — same optimistic-UI-rollback
    // convention as RenameSpeakerTask::speakerRenamed.
    void speakersMerged(const QString &audioPath, const QString &sourceName, const QString &targetName, bool ok,
                         const QString &error);

private:
    QString m_sourceName;
    QString m_targetName;
};
