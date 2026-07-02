#include "TranscriptionController.h"

#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QMetaObject>
#include <QStringList>
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

QString sidecarPath(const QString &audioFilePath)
{
    const QFileInfo fi(audioFilePath);
    return fi.absolutePath() + "/" + fi.completeBaseName() + ".md";
}

QString jobStatusToString(TranscriptionController::JobStatus status)
{
    switch (status) {
    case TranscriptionController::JobStatus::Queued:
        return QStringLiteral("queued");
    case TranscriptionController::JobStatus::Active:
        return QStringLiteral("active");
    case TranscriptionController::JobStatus::Done:
        return QStringLiteral("done");
    case TranscriptionController::JobStatus::Failed:
        return QStringLiteral("failed");
    }
    return QString();
}

// Written immediately when a job starts, before whisper_full runs, so a
// killed/stopped run still leaves a file recording which settings it was
// transcribed with (see Milestone 4 Task 3 — durability, not full resume).
void writeFrontmatter(QFile &mdFile, const QString &modelUsed, bool speakerDetection)
{
    QTextStream out(&mdFile);
    out << "---\n";
    out << "model: " << modelUsed << '\n';
    out << "speakerDetection: " << (speakerDetection ? "true" : "false") << '\n';
    out << "transcribedAt: " << QDateTime::currentDateTime().toString(Qt::ISODate) << '\n';
    out << "---\n\n";
    out.flush();
    mdFile.flush();
}

// Appended + flushed as each segment is produced (see DESIGN.md's
// per-segment streaming callback + Task 3's durability goal), rather than
// buffering the whole transcript and writing it once at the end.
void appendSegmentLine(QFile &mdFile, const Segment &segment)
{
    const QString text = QString::fromStdString(segment.text).trimmed();
    if (text.isEmpty())
        return;
    QTextStream out(&mdFile);
    out << '[' << formatTimestamp(segment.t0) << " - " << formatTimestamp(segment.t1) << "] " << text;
    if (segment.speakerTurnNext)
        out << " [SPEAKER_TURN]";
    out << '\n';
    out.flush();
    mdFile.flush();
}

}

// Lives on TranscriptionController's dedicated QThread; never touches the
// GUI thread. Owns the warm WhisperCppEngine so the model stays resident
// across transcribe() calls.
class TranscriptionWorker : public QObject {
    Q_OBJECT

public:
    using QObject::QObject;

public slots:
    void run(const QString &filePath, const QString &vaultPath, const QString &modelId, bool speakerDetection)
    {
        QString error;
        const QString modelPath = ModelManager::ensureModel(QUrl::fromLocalFile(vaultPath), modelId, speakerDetection, &error);
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

        QFile mdFile(sidecarPath(filePath));
        if (!mdFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            emit finished(false, QStringLiteral("could not open %1 for writing").arg(mdFile.fileName()));
            return;
        }
        writeFrontmatter(mdFile, QFileInfo(modelPath).fileName(), speakerDetection);

        AudioChunk chunk;
        chunk.pcm16kMono = std::move(*pcm);
        chunk.offsetSeconds = 0.0; // whole-file, single chunk — no VAD chunking yet

        ProgressCallback onProgress = [this](double done, double total) {
            emit progress(total > 0 ? done / total : 0.0);
        };
        SegmentCallback onSegment = [&mdFile](const Segment &segment) {
            // Chunk offset merge is the app's job (see WhisperCppEngine.cpp);
            // with offsetSeconds always 0 this milestone it's a no-op, kept
            // explicit here as the seam for when chunking lands.
            appendSegmentLine(mdFile, segment);
        };

        m_engine->transcribe(chunk, onProgress, onSegment);

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
    connect(m_worker, &TranscriptionWorker::finished, this, &TranscriptionController::onWorkerFinished);
    m_thread.start();
}

TranscriptionController::~TranscriptionController()
{
    m_thread.quit();
    m_thread.wait();
}

bool TranscriptionController::isQueued(const QUrl &fileUrl) const
{
    for (const Job &job : m_jobs) {
        if (job.fileUrl == fileUrl && (job.status == JobStatus::Queued || job.status == JobStatus::Active))
            return true;
    }
    return false;
}

void TranscriptionController::transcribe(const QUrl &fileUrl, const QUrl &vaultUrl, const QString &modelId,
                                          bool speakerDetection)
{
    // Done/Failed entries for the same fileUrl deliberately do NOT block
    // re-queueing (see class doc) — only Queued/Active does.
    if (isQueued(fileUrl))
        return;

    Job job;
    job.fileUrl = fileUrl;
    job.vaultUrl = vaultUrl;
    job.modelId = modelId;
    job.speakerDetection = speakerDetection;
    job.status = JobStatus::Queued;
    m_jobs.push_back(job);
    emit queueChanged();

    if (m_activeIndex < 0)
        startJob(m_jobs.size() - 1);
}

void TranscriptionController::transcribeAll(const QVariantList &fileUrls, const QUrl &vaultUrl,
                                             const QString &modelId, bool speakerDetection)
{
    for (const QVariant &v : fileUrls)
        transcribe(v.toUrl(), vaultUrl, modelId, speakerDetection);
}

void TranscriptionController::startJob(int index)
{
    m_activeIndex = index;
    Job &job = m_jobs[index];
    job.status = JobStatus::Active;
    job.error.clear();

    m_busy = true;
    emit busyChanged();
    m_progress = 0.0;
    emit progressChanged();
    m_currentFile = job.fileUrl;
    emit currentFileChanged();
    emit queueChanged();

    QMetaObject::invokeMethod(m_worker, "run", Qt::QueuedConnection,
                               Q_ARG(QString, job.fileUrl.toLocalFile()), Q_ARG(QString, job.vaultUrl.toLocalFile()),
                               Q_ARG(QString, job.modelId), Q_ARG(bool, job.speakerDetection));
}

void TranscriptionController::onWorkerFinished(bool ok, const QString &error)
{
    m_busy = false;
    emit busyChanged();
    const QUrl finishedFile = m_currentFile;

    if (m_activeIndex >= 0 && m_activeIndex < m_jobs.size()) {
        Job &job = m_jobs[m_activeIndex];
        job.status = ok ? JobStatus::Done : JobStatus::Failed;
        job.error = error;
    }
    m_activeIndex = -1;

    int nextIndex = -1;
    for (int i = 0; i < m_jobs.size(); ++i) {
        if (m_jobs[i].status == JobStatus::Queued) {
            nextIndex = i;
            break;
        }
    }

    if (nextIndex >= 0) {
        startJob(nextIndex);
    } else {
        m_currentFile = QUrl();
        emit currentFileChanged();
        emit queueChanged();
    }

    emit finished(finishedFile, ok, error);
}

void TranscriptionController::dismiss(const QUrl &fileUrl)
{
    // A fileUrl may have multiple entries (re-queueing a Done/Failed job is
    // allowed) — remove only the first Done/Failed match, skipping over any
    // Queued/Active entry for the same url rather than no-op'ing on it.
    for (int i = 0; i < m_jobs.size(); ++i) {
        const Job &job = m_jobs[i];
        if (job.fileUrl == fileUrl && (job.status == JobStatus::Done || job.status == JobStatus::Failed)) {
            m_jobs.removeAt(i);
            if (m_activeIndex > i)
                --m_activeIndex;
            emit queueChanged();
            return;
        }
    }
}

void TranscriptionController::clearFinished()
{
    bool changed = false;
    for (int i = m_jobs.size() - 1; i >= 0; --i) {
        if (m_jobs[i].status == JobStatus::Done || m_jobs[i].status == JobStatus::Failed) {
            m_jobs.removeAt(i);
            if (m_activeIndex > i)
                --m_activeIndex;
            changed = true;
        }
    }
    if (changed)
        emit queueChanged();
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

bool TranscriptionController::setLabel(const QUrl &fileUrl, const QString &label, QString *errorOut) const
{
    const QFileInfo fi(fileUrl.toLocalFile());
    const QString path = fi.absolutePath() + "/" + fi.completeBaseName() + ".md";

    QFile mdFile(path);
    if (!mdFile.exists()) {
        if (errorOut)
            *errorOut = QStringLiteral("%1 has not been transcribed yet").arg(fi.fileName());
        return false;
    }

    if (!mdFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        if (errorOut)
            *errorOut = QStringLiteral("could not open %1 for reading").arg(path);
        return false;
    }

    QTextStream in(&mdFile);
    if (in.readLine() != QStringLiteral("---")) {
        mdFile.close();
        if (errorOut)
            *errorOut = QStringLiteral("%1 has no frontmatter block").arg(mdFile.fileName());
        return false;
    }

    // Mirror readMetadata()'s frontmatter-scanning shape, but keep the raw
    // lines (not parsed key/value pairs) so we can rewrite the block without
    // reformatting keys we don't care about.
    QStringList frontmatterLines;
    QString line;
    bool closed = false;
    while (!(line = in.readLine()).isNull()) {
        if (line == QStringLiteral("---")) {
            closed = true;
            break;
        }
        frontmatterLines.append(line);
    }
    if (!closed) {
        mdFile.close();
        if (errorOut)
            *errorOut = QStringLiteral("%1 has a malformed frontmatter block").arg(mdFile.fileName());
        return false;
    }

    // Everything after the closing "---" line, byte-for-byte — including
    // its leading blank line (writeFrontmatter() emits "---\n\n" before the
    // first segment line) — so the transcript body is left untouched.
    const QString body = in.readAll();
    mdFile.close();

    bool replaced = false;
    for (QString &fmLine : frontmatterLines) {
        const int colon = fmLine.indexOf(':');
        if (colon >= 0 && fmLine.left(colon).trimmed() == QStringLiteral("label")) {
            fmLine = QStringLiteral("label: ") + label;
            replaced = true;
            break;
        }
    }
    if (!replaced)
        frontmatterLines.append(QStringLiteral("label: ") + label);

    if (!mdFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (errorOut)
            *errorOut = QStringLiteral("could not open %1 for writing").arg(mdFile.fileName());
        return false;
    }
    QTextStream out(&mdFile);
    out << "---\n";
    for (const QString &fmLine : frontmatterLines)
        out << fmLine << '\n';
    out << "---\n";
    out << body;
    out.flush();
    mdFile.flush();
    mdFile.close();
    return true;
}

QString TranscriptionController::readTranscriptBody(const QUrl &fileUrl) const
{
    const QFileInfo fi(fileUrl.toLocalFile());
    QFile mdFile(fi.absolutePath() + "/" + fi.completeBaseName() + ".md");
    if (!mdFile.open(QIODevice::ReadOnly | QIODevice::Text))
        return QString();

    QTextStream in(&mdFile);
    if (in.readLine() != QStringLiteral("---"))
        return QString();

    QString line;
    while (!(line = in.readLine()).isNull() && line != QStringLiteral("---")) {
        // skip frontmatter lines
    }
    return in.readAll();
}

int TranscriptionController::tasksTotal() const
{
    return m_jobs.size();
}

int TranscriptionController::tasksCompleted() const
{
    int count = 0;
    for (const Job &job : m_jobs) {
        if (job.status == JobStatus::Done || job.status == JobStatus::Failed)
            ++count;
    }
    return count;
}

QVariantList TranscriptionController::queuedFileUrls() const
{
    QVariantList list;
    for (const Job &job : m_jobs) {
        if (job.status == JobStatus::Queued)
            list.append(job.fileUrl);
    }
    return list;
}

QVariantList TranscriptionController::jobs() const
{
    QVariantList list;
    for (const Job &job : m_jobs) {
        QVariantMap entry;
        entry["fileUrl"] = job.fileUrl;
        entry["status"] = jobStatusToString(job.status);
        entry["error"] = job.error;
        list.append(entry);
    }
    return list;
}

QVariantList TranscriptionController::availableModels() const
{
    QVariantList list;
    for (const ModelManager::ModelInfo &info : ModelManager::availableModels()) {
        QVariantMap entry;
        entry["id"] = info.id;
        entry["label"] = info.label;
        list.append(entry);
    }
    return list;
}

#include "TranscriptionController.moc"
