#pragma once

#include <QString>
#include <QUrl>

#include "Task.h"

// Relabels every "[oldName]" body tag in one already-transcribed .md to
// "[newName]" (see renameSpeakerInFile()'s doc comment in
// TranscriptionController.h — REJECTS colliding with an existing name, typo
// protection). A single atomic file rewrite: no interruption point, no
// engine, no phase progress worth reporting.
class RenameSpeakerTask : public Task {
    Q_OBJECT
public:
    RenameSpeakerTask(QUrl audioUrl, QString oldName, QString newName, QObject *parent = nullptr);

    QString kind() const override { return QStringLiteral("rename"); }
    QString displayName() const override;
    // capabilities(): default (not pausable, not mid-run-cancellable) —
    // matches pauseActive()'s old allowlist, which never included "rename".
    // isPersistable(): the old persistQueue() persisted every kind EXCEPT
    // import/usbGrab (an opt-OUT list) — rename names no external/removable
    // path, so it was always persisted. true here preserves that.
    bool isPersistable() const override { return true; }
    QJsonObject toJson() const override;
    QVariantMap toVariantMap() const override;

    void run(TaskContext &ctx) override;

    const QString &oldName() const { return m_oldName; }
    const QString &newName() const { return m_newName; }

signals:
    // Fires on BOTH success and failure (ok distinguishes) — MainPanel.qml's
    // pendingSpeakerRenames optimistic-rename override is cleared either way
    // once this arrives, per its doc comment ("fires... whether it succeeded
    // or not").
    void speakerRenamed(const QString &audioPath, const QString &oldName, bool ok, const QString &error);

private:
    QString m_oldName;
    QString m_newName;
};
