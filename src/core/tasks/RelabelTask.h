#pragma once

#include <QString>
#include <QUrl>

#include "Task.h"

// "Regenerate auto-title": re-runs JUST the labeling pass on an
// already-transcribed file — no audio decode, no whisper, no frontmatter
// reset. Both a manual "Regenerate label and prelude" request and the
// automatic post-transcription follow-up go through this one path.
class RelabelTask : public Task {
    Q_OBJECT
public:
    // skipIfAlreadyLabeled: only ever true for the automatic follow-up pass
    // — an existing non-empty label/tags/prelude means this file was already
    // labeled by a previous run (or a manual edit), so this reports success
    // with no LLM call rather than clobbering it. false for the user's
    // explicit action, which must always force-regenerate.
    RelabelTask(QUrl fileUrl, QUrl vaultUrl, bool restrictTagsToExisting, double tagReuseBias, double wordiness,
                bool skipIfAlreadyLabeled, QObject *parent = nullptr);

    QString kind() const override { return QStringLiteral("relabel"); }
    QString displayName() const override;
    // Not pausable: one LLM call with no interruption point (see
    // pauseActive()'s old allowlist, which never included "relabel").
    bool isPersistable() const override { return true; }
    QJsonObject toJson() const override;
    QVariantMap toVariantMap() const override;
    // The label model — the same one ChaptersTask needs, so the queue's
    // dedup means a runAllJobs() pipeline queues it exactly once for both.
    ModelPrerequisites prerequisites() const override;

    void run(TaskContext &ctx) override;

private:
    bool m_restrictTagsToExisting;
    double m_tagReuseBias;
    double m_wordiness;
    bool m_skipIfAlreadyLabeled;
};
