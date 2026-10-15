#pragma once

#include <QString>
#include <QUrl>
#include <QVariantMap>

#include <optional>

#include "Task.h"

// The explicit "compress" action, to one of two targets:
//  - Voice: a WAV, or a FLAC that isn't already FLAC voice, becomes 16 kHz
//    mono FLAC — exactly what every engine decodes to, so transcripts are
//    unchanged.
//  - Lossless: a WAV becomes FLAC at its own rate and channels.
// Either lands at <stem>.flac. Staged as a hidden same-folder temp, verified
// (full decode + duration within kMaxDurationDriftSeconds of the original's),
// then the original goes to the trash and the encode is renamed into place
// WITHOUT overwriting — so a FLAC → FLAC voice replace keeps its path. Any failure or cancel removes the
// temp and keeps the original; undo is RevertCompressionTask.
//
// Never part of run-all (TranscriptionController::pendingJobKinds()) and
// never run on a take automatically (the recorder encodes live): it replaces
// the original, so it only ever runs on explicit request.
//
// Serial lane: an encode is CPU-intensive (see Task::Lane). Background: it
// must not raise the transcript overlay over whatever the user is reading.
class CompressAudioTask : public Task {
    Q_OBJECT
public:
    enum class Target { Voice, Lossless };

    CompressAudioTask(QUrl audioUrl, QUrl vaultUrl, Target target, QObject *parent = nullptr);

    // "voice" / "lossless" — the QML-facing spelling; nullopt otherwise.
    static std::optional<Target> targetFromString(const QString &target);
    // Lossless: a .wav. Voice: a .wav, or a .flac whose STREAMINFO isn't
    // already 16 kHz mono. Reads at most the first 21 bytes, so QML can ask
    // it per row.
    static bool isCompressible(const QString &path, Target target);
    // "<dir>/<stem>.flac" — where the compressed file lands.
    static QString destinationPath(const QString &audioPath);
    static constexpr double kMaxDurationDriftSeconds = 0.5;

    QString kind() const override { return QStringLiteral("compressAudio"); }
    QString kindLabel() const override { return QStringLiteral("Compress"); }
    QString displayName() const override;
    Presentation presentation() const override { return Presentation::Background; }
    // SIGSTOP/SIGCONT on the ffmpeg child, same shape as CombineTask.
    Capabilities capabilities() const override { return {/*pausable=*/true, /*cancellableMidRun=*/true}; }
    // The node's extension may change; the controller applies it in place
    // (applyRenameOutcome) the same way a file rename does.
    bool isStructuralFileOp() const override { return true; }
    // Only once started: queued, it must not swallow (isQueued dedup) a
    // transcribe asked for after it — that transcribe makes this task stand
    // down at dispatch instead (setContended).
    bool occupiesFile() const override { return status == Status::Active || status == Status::Paused; }
    // Not persisted: the queue drops it across a restart (the original stays).

    QString undoLabel() const override;
    QString batchUndoLabel(const QList<const Task *> &undoableMembers) const override;
    std::unique_ptr<Task> makeUndoTask() const override;
    bool undoStillApplicable() const override;

    // Original size minus compressed size; 0 until run() succeeds.
    qint64 savedBytes() const { return m_savedBytes; }

    // Set by the controller on the GUI thread right before dispatch (the same
    // "queue writes, run() reads" contract as fileUrl): another queued or
    // active task names this file, so replacing it would pull it out from
    // under that task. run() then keeps the original and says so.
    void setContended(bool contended) { m_contended = contended; }

    void run(TaskContext &ctx) override;

signals:
    // Emitted only after the compressed file is in place and the original
    // trashed. oldUrl == newUrl for a FLAC → FLAC voice replace.
    void audioCompressed(const QUrl &oldUrl, const QUrl &newUrl);

private:
    Target m_target;
    bool m_contended = false;
    qint64 m_savedBytes = 0;
    QUrl m_compressedUrl;
    QVariantMap m_trashEntry; // {trashPath, originalPath} — restoreVaultEntries() shape
};
