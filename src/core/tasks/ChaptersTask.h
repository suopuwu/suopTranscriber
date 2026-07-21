#pragma once

#include <QString>
#include <QUrl>

#include "../LabelingEngine.h" // Chapter — withOpeningChapter() takes/returns it by value
#include "Task.h"

// Feature 3a ("generate chapters"): a second, separate LLM pass over an
// already-transcribed file, sharing the SAME resident label engine as
// RelabelTask (see LabelingEngine::generateChapters()'s doc comment — never
// a second LabelingEngine instance). Splits the transcript into windows
// bounded by LabelingEngine::kMaxChapterInputChars, proposes chapter
// boundaries per window, merges over-eager adjacent proposals, resolves
// indices to real timestamps, and splices the result into the .md.
// Progress bands for ChaptersTask's SelfCumulative bar. Generation is
// kChaptersGenerateBand of the bar, subdivided per window and — via the token
// callback — within a window; the merge pass owns the rest up to
// kChaptersMergeBandEnd; the resolve/write tail closes it to 1.0.
inline constexpr double kChaptersGenerateBand = 0.75;
inline constexpr double kChaptersMergeBandEnd = 0.98;

// Fraction of the FULL bar while decoding window `w` of `windowCount`, with
// `tokens` output tokens seen so far in that window. The token count has no
// known total (grammar-constrained generation stops when the grammar
// completes), so the within-window ramp is asymptotic: it approaches, but
// never reaches, the window's own upper edge, and the caller snaps to that
// edge when the window actually returns. Monotone in both w and tokens, and
// never exceeds kChaptersGenerateBand.
double chaptersGenerateProgress(int w, int windowCount, int tokens);

// Returns `chapters` with a synthetic OPENING chapter prepended — one whose
// afterSegmentIndex is -1, i.e. whose content starts at segment 0, so a
// transcript always opens under a heading. LabelingEngine::generateChapters()
// structurally cannot propose this chapter itself (it's told to skip the first
// one, and kChapterGrammar's afterSegment can't go below 0), so ChaptersTask
// synthesizes it after the merge pass, titled by
// LabelingEngine::openingChapterTitle().
//
// Returned unchanged when `title` is blank (the title pass failed — better an
// untitled opening stretch than a "## [0]" marker with no title) or when
// `chapters` already opens at segment 0 (defensive: nothing can currently
// produce that, and two chapters sharing t0 would splice two markers at the
// same offset).
//
// Declared here (rather than file-local in the .cpp) so the prepend rules are
// unit-testable directly with synthetic chapters, without a real label model —
// same precedent as spliceBodyChapters()/computeChapterWindows() in
// TranscriptionController.h.
QList<Chapter> withOpeningChapter(const QList<Chapter> &chapters, const QString &title);

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
