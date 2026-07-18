#pragma once

#include <QString>
#include <QUrl>

#include "Task.h"

// Feature 3a ("generate chapters"): a second, separate LLM pass over an
// already-transcribed file, sharing the SAME resident label engine as
// RelabelTask (see LabelingEngine::generateChapters()'s doc comment — never
// a second LabelingEngine instance). Splits the transcript into windows
// bounded by LabelingEngine::kMaxChapterInputChars, proposes chapter
// boundaries per window, merges over-eager adjacent proposals, resolves
// indices to real timestamps, and splices the result into the .md.
class ChaptersTask : public Task {
    Q_OBJECT
public:
    // density: 0.0-1.0 chapter-density slider (0.5 == neutral).
    ChaptersTask(QUrl fileUrl, QUrl vaultUrl, double density, QObject *parent = nullptr);

    QString kind() const override { return QStringLiteral("chapters"); }
    QString kindLabel() const override { return QStringLiteral("Generate chapters"); }
    QString displayName() const override;
    // Pausable BETWEEN windows (an in-flight single generateChapters() LLM
    // call can't itself be interrupted — no llama.cpp abort hook for a
    // single-shot decode — but a long multi-window transcript still gets a
    // prompt cancel between them). See pauseActive()'s old allowlist, which
    // included "chapters" for exactly this reason.
    Capabilities capabilities() const override { return {/*pausable=*/true, /*cancellableMidRun=*/true}; }
    bool isPersistable() const override { return true; }
    QJsonObject toJson() const override;
    QVariantMap toVariantMap() const override;
    // The same label model RelabelTask needs — see its prerequisites().
    ModelPrerequisites prerequisites() const override;

    void run(TaskContext &ctx) override;

private:
    double m_density;
};
