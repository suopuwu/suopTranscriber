#pragma once

#include <QObject>
#include <QThread>
#include <QUrl>
#include <QVariantMap>
#include <qqml.h>

class TranscriptionWorker;

// Orchestrates: ffmpeg decode -> WhisperCppEngine -> vault sidecar files.
// Owns a dedicated QThread; transcription (and its file IO) NEVER runs on
// the GUI thread (see CLAUDE.md). Single-shot, whole-file transcription
// only for now — no VAD chunking / resume yet (DESIGN.md "Durable /
// resumable transcription" is a later milestone).
class TranscriptionController : public QObject {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(bool busy READ isBusy NOTIFY busyChanged)
    Q_PROPERTY(double progress READ progress NOTIFY progressChanged)
    Q_PROPERTY(QUrl currentFile READ currentFile NOTIFY currentFileChanged)

public:
    explicit TranscriptionController(QObject *parent = nullptr);
    ~TranscriptionController() override;

    bool isBusy() const { return m_busy; }
    double progress() const { return m_progress; }
    QUrl currentFile() const { return m_currentFile; }

    Q_INVOKABLE void transcribe(const QUrl &fileUrl, const QUrl &vaultUrl, bool speakerDetection);

    // Reads the frontmatter (model/speakerDetection/transcribedAt) out of a
    // file's sibling .md, if one exists yet. Empty map if not transcribed.
    Q_INVOKABLE QVariantMap readMetadata(const QUrl &fileUrl) const;

signals:
    void busyChanged();
    void progressChanged();
    void currentFileChanged();
    void finished(const QUrl &fileUrl, bool ok, const QString &error);

private:
    QThread m_thread;
    TranscriptionWorker *m_worker = nullptr;
    bool m_busy = false;
    double m_progress = 0.0;
    QUrl m_currentFile;
};
