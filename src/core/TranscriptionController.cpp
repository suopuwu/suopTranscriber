#include "TranscriptionController.h"

#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QMetaObject>
#include <QTextStream>

#include <memory>

#include "AudioDecoder.h"
#include "ModelManager.h"
#include "Segment.h"
#include "WhisperCppEngine.h"

namespace {

// HH:MM:SS.mmm, human-readable and greppable — no separate sidecar needed to
// recover per-segment timing later (e.g. for sentence-highlight sync).
QString formatTimestamp(double seconds)
{
    const qint64 totalMs = static_cast<qint64>(seconds * 1000.0 + 0.5);
    const qint64 h = totalMs / 3600000;
    const qint64 m = (totalMs / 60000) % 60;
    const qint64 s = (totalMs / 1000) % 60;
    const qint64 ms = totalMs % 1000;
    return QStringLiteral("%1:%2:%3.%4")
        .arg(h, 2, 10, QChar('0'))
        .arg(m, 2, 10, QChar('0'))
        .arg(s, 2, 10, QChar('0'))
        .arg(ms, 3, 10, QChar('0'));
}

// Single <dir>/<name>.md sibling of the audio file, per DESIGN.md's vault
// file layout — YAML frontmatter (model/speakerDetection/transcribedAt) plus
// one `[start - end] text` line per segment. No separate JSON sidecar: all
// data (including per-segment timing) lives in this one human-readable file.
void writeSidecars(const QString &audioFilePath, const std::vector<Segment> &segments,
                    const QString &modelUsed, bool speakerDetection)
{
    const QFileInfo fi(audioFilePath);
    const QString dir = fi.absolutePath();
    const QString base = fi.completeBaseName();

    QFile mdFile(dir + "/" + base + ".md");
    if (!mdFile.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return;

    QTextStream out(&mdFile);
    out << "---\n";
    out << "model: " << modelUsed << '\n';
    out << "speakerDetection: " << (speakerDetection ? "true" : "false") << '\n';
    out << "transcribedAt: " << QDateTime::currentDateTime().toString(Qt::ISODate) << '\n';
    out << "---\n\n";

    for (const Segment &segment : segments) {
        const QString text = QString::fromStdString(segment.text).trimmed();
        if (text.isEmpty())
            continue;
        out << '[' << formatTimestamp(segment.t0) << " - " << formatTimestamp(segment.t1) << "] " << text;
        if (segment.speakerTurnNext)
            out << " [SPEAKER_TURN]";
        out << '\n';
    }
}

}

// Lives on TranscriptionController's dedicated QThread; never touches the
// GUI thread. Owns the warm WhisperCppEngine so the model stays resident
// across multiple transcribe() calls.
class TranscriptionWorker : public QObject {
    Q_OBJECT

public:
    using QObject::QObject;

public slots:
    void run(const QString &filePath, const QString &vaultPath, bool speakerDetection)
    {
        QString error;
        const QString modelPath = ModelManager::ensureModel(QUrl::fromLocalFile(vaultPath), speakerDetection, &error);
        if (modelPath.isEmpty()) {
            emit finished(false, error.isEmpty() ? QStringLiteral("model unavailable") : error);
            return;
        }

        if (!m_engine || m_engine->isValid() == false || m_modelPath != modelPath) {
            m_engine = std::make_unique<WhisperCppEngine>(modelPath, speakerDetection);
            m_modelPath = modelPath;
        }
        if (!m_engine->isValid()) {
            emit finished(false, QStringLiteral("failed to load model at %1").arg(modelPath));
            return;
        }

        QString decodeError;
        auto pcm = AudioDecoder::decodeTo16kMono(filePath, &decodeError);
        if (!pcm) {
            emit finished(false, decodeError.isEmpty() ? QStringLiteral("audio decode failed") : decodeError);
            return;
        }

        AudioChunk chunk;
        chunk.pcm16kMono = std::move(*pcm);
        chunk.offsetSeconds = 0.0; // whole-file, single chunk — no VAD chunking yet

        ProgressCallback onProgress = [this](double done, double total) {
            emit progress(total > 0 ? done / total : 0.0);
        };
        SegmentCallback onSegment = [this](const Segment &) {
            // Chunk offset merge is the app's job (see WhisperCppEngine.cpp);
            // with offsetSeconds always 0 this milestone it's a no-op, kept
            // explicit here as the seam for when chunking lands.
        };

        std::vector<Segment> segments = m_engine->transcribe(chunk, onProgress, onSegment);
        writeSidecars(filePath, segments, QFileInfo(modelPath).fileName(), speakerDetection);

        emit finished(true, {});
    }

signals:
    void progress(double fraction);
    void finished(bool ok, const QString &error);

private:
    std::unique_ptr<WhisperCppEngine> m_engine;
    QString m_modelPath;
};

TranscriptionController::TranscriptionController(QObject *parent)
    : QObject(parent)
{
    m_worker = new TranscriptionWorker;
    m_worker->moveToThread(&m_thread);
    connect(&m_thread, &QThread::finished, m_worker, &QObject::deleteLater);
    connect(m_worker, &TranscriptionWorker::progress, this, [this](double fraction) {
        m_progress = fraction;
        emit progressChanged();
    });
    connect(m_worker, &TranscriptionWorker::finished, this, [this](bool ok, const QString &error) {
        m_busy = false;
        emit busyChanged();
        emit finished(m_currentFile, ok, error);
    });
    m_thread.start();
}

TranscriptionController::~TranscriptionController()
{
    m_thread.quit();
    m_thread.wait();
}

void TranscriptionController::transcribe(const QUrl &fileUrl, const QUrl &vaultUrl, bool speakerDetection)
{
    if (m_busy)
        return;

    m_busy = true;
    emit busyChanged();
    m_progress = 0.0;
    emit progressChanged();
    m_currentFile = fileUrl;
    emit currentFileChanged();

    QMetaObject::invokeMethod(m_worker, "run", Qt::QueuedConnection,
                               Q_ARG(QString, fileUrl.toLocalFile()), Q_ARG(QString, vaultUrl.toLocalFile()),
                               Q_ARG(bool, speakerDetection));
}

QVariantMap TranscriptionController::readMetadata(const QUrl &fileUrl) const
{
    QVariantMap result;

    const QFileInfo fi(fileUrl.toLocalFile());
    QFile mdFile(fi.absolutePath() + "/" + fi.completeBaseName() + ".md");
    if (!mdFile.open(QIODevice::ReadOnly | QIODevice::Text))
        return result;

    QTextStream in(&mdFile);
    if (in.readLine() != QStringLiteral("---"))
        return result;

    QString line;
    while (!(line = in.readLine()).isNull() && line != QStringLiteral("---")) {
        const int colon = line.indexOf(':');
        if (colon < 0)
            continue;
        const QString key = line.left(colon).trimmed();
        const QString value = line.mid(colon + 1).trimmed();
        if (value == QStringLiteral("true"))
            result[key] = true;
        else if (value == QStringLiteral("false"))
            result[key] = false;
        else
            result[key] = value;
    }
    return result;
}

#include "TranscriptionController.moc"
