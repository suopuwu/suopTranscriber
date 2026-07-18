#pragma once

#include <QString>
#include <QVariantMap>

#include "ModelDownloadTarget.h"
#include "Task.h"

// Downloads one model file into app-global storage via ModelManager — the
// same queue path used both for an explicit "Download" click and a lazy
// predownload queued ahead of a transcribe/relabel/diarize task that needs a
// model it doesn't have yet (see Task::prerequisites()).
class DownloadTask : public Task {
    Q_OBJECT
public:
    // modelId is only meaningful for WhisperModel/SegmentationModel/
    // EmbeddingModel (empty for LabelModel/VadModel, which are each a single
    // fixed file) — mirrors the old Job::modelId's doc comment.
    DownloadTask(ModelDownloadTarget target, QString modelId, QObject *parent = nullptr);

    QString kind() const override { return QStringLiteral("download"); }
    QString displayName() const override;
    // Background: a predownload or an explicit model-page download has no
    // business hijacking the main panel overlay.
    Presentation presentation() const override { return Presentation::Background; }
    // ModelManager's download loop polls TaskContext::pauseRequested()/
    // cancelRequested() itself on every chunk (see run()) — a real,
    // immediately-effective interruption point.
    Capabilities capabilities() const override { return {/*pausable=*/true, /*cancellableMidRun=*/true}; }
    // Survives a restart — an interrupted download resumes into the same
    // queued state next launch (unlike import/usbGrab, nothing here names an
    // external/removable path that might be gone).
    bool isPersistable() const override { return true; }
    QJsonObject toJson() const override;
    QVariantMap toVariantMap() const override;

    void run(TaskContext &ctx) override;

    ModelDownloadTarget target() const { return m_target; }
    const QString &modelId() const { return m_modelId; }
    static QString targetToString(ModelDownloadTarget target);
    // Inverse of targetToString(), for the persisted-queue replay. Returns
    // false for an unrecognized string rather than guessing a target.
    static bool targetFromString(const QString &raw, ModelDownloadTarget *out);

private:
    ModelDownloadTarget m_target;
    QString m_modelId;
};
