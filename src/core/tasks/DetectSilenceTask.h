#pragma once

#include <QString>
#include <QUrl>

#include "Task.h"

// Silence phase 1 (backfill): standalone "detect silences" on an
// ALREADY-transcribed file — the Tasks-list job for the files auto-capture
// (TranscribeTask's inline VAD/transcript-gap path) never covered: files
// transcribed before this feature landed, files whose transcribe-time VAD
// pass failed, and files transcribed via a self-segmenting out-of-process
// engine (VAD never runs for those at all — see TranscribePipeline::
// planWindows()'s selfSegmenting branch). Re-runs VAD (falling back to the
// SAME transcript-gap heuristic TranscribeTask's fallback uses when VAD is
// unavailable) and upserts the "silences:" frontmatter key IN PLACE —
// segments/speakers/chapters/label are untouched, mirroring DiarizeTask's
// "adds one thing to an existing file" shape rather than ChaptersTask's
// (which also only touches its own field, but starts from parsed segments
// rather than a fresh decode).
class DetectSilenceTask : public Task {
    Q_OBJECT
public:
    DetectSilenceTask(QUrl fileUrl, QUrl vaultUrl, QObject *parent = nullptr);

    QString kind() const override { return QStringLiteral("detectSilence"); }
    QString kindLabel() const override { return QStringLiteral("Detect silences"); }
    QString displayName() const override;
    // Pausable (decode is a real killable/SIGSTOP-able ffmpeg subprocess) but
    // NOT reliably cancellable mid-run: computeWindows() is one blocking
    // whisper.cpp VAD call with no abort hook, same shape as DiarizeTask's
    // diarization pass — a cancel requested during it only takes effect at
    // the checkpoint right after.
    Capabilities capabilities() const override { return {/*pausable=*/true, /*cancellableMidRun=*/false}; }
    bool isPersistable() const override { return true; }
    // In-place .md rewrite of an existing key, same as ChaptersTask/
    // RelabelTask/DiarizeTask — the vault tree's structure never changes, so
    // the default false (no forced full-tree reset) is correct.
    bool isStructuralFileOp() const override { return false; }
    // No extra constructor fields beyond fileUrl/vaultUrl (which the base
    // class + queue already round-trip) — the default toJson()/toVariantMap()
    // (both return {}) are correct as-is, unlike DiarizeTask's tuning params.

    // Needs the VAD model on disk before it can run; the queue turns this
    // into a DownloadTask inserted ahead of this one if not already present
    // (deduped — see Task::prerequisites()'s doc comment). No modelId: VAD is
    // a single fixed file (ModelManager::kVadModelFileName), same as
    // LabelModel's ModelPrerequisite.
    ModelPrerequisites prerequisites() const override;

    void run(TaskContext &ctx) override;
};
