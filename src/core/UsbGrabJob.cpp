#include "UsbGrabJob.h"

#include "VaultTreeModel.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QVariantMap>

namespace {

// SHA-256 of a whole file's contents, streamed through QCryptographicHash's
// QIODevice overload rather than QFile::readAll() so this never buffers an
// entire (potentially huge) media file into memory just to hash it. Returns
// an empty QByteArray if the file can't be opened for reading.
QByteArray hashFile(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return QByteArray();
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&file))
        return QByteArray();
    return hash.result();
}

// Empty filter means "all files" (not zero files) — see UsbGrabRule::
// extensionFilter's doc comment in UsbRuleStore.h. Case-insensitive, and
// tolerant of filter entries that happen to include a leading dot.
bool extensionMatches(const QFileInfo &info, const QStringList &extensionFilter)
{
    if (extensionFilter.isEmpty())
        return true;
    const QString suffix = info.suffix();
    for (const QString &rawExt : extensionFilter) {
        QString ext = rawExt;
        if (ext.startsWith(QLatin1Char('.')))
            ext.remove(0, 1);
        if (ext.compare(suffix, Qt::CaseInsensitive) == 0)
            return true;
    }
    return false;
}

}

// Lives on UsbGrabJob's dedicated QThread; never touches the GUI thread.
// Owns the whole recursive scan+copy for one run — unlike AudioRecorderWorker
// (which sets up capture and then idles behind a QTimer, letting the worker's
// event loop keep running), this worker's start() slot runs the entire scan
// synchronously to completion in one call, since there's no ongoing hardware
// stream to service in the meantime.
class UsbGrabJobWorker : public QObject {
    Q_OBJECT

public:
    using QObject::QObject;

public slots:
    void start(const QString &mountPath, const QString &startFolder, const QStringList &extensionFilter,
               bool preserveStructure, const QString &destinationVaultPath)
    {
        m_cancelRequested = false;

        QString scanRoot = mountPath;
        if (!startFolder.isEmpty())
            scanRoot += QLatin1Char('/') + startFolder;

        // Index every file already in the destination vault by basename, up
        // front, so each source file's basename+hash diff (per DESIGN.md's
        // stated intent) doesn't need to re-walk the vault per source file.
        // Hashes are computed lazily via hashOf() below and cached, since
        // most indexed basenames will never actually need hashing (no
        // matching basename shows up on the device at all).
        QHash<QString, QStringList> destPathsByBasename;
        QDirIterator destIt(destinationVaultPath, QDir::Files, QDirIterator::Subdirectories);
        while (destIt.hasNext()) {
            const QString path = destIt.next();
            destPathsByBasename[QFileInfo(path).fileName()].append(path);
        }

        QHash<QString, QByteArray> hashCache;
        auto hashOf = [&hashCache](const QString &path) -> QByteArray {
            const auto it = hashCache.constFind(path);
            if (it != hashCache.constEnd())
                return it.value();
            const QByteArray computed = hashFile(path);
            hashCache.insert(path, computed);
            return computed;
        };

        int filesCopied = 0;
        int filesSkipped = 0;
        QStringList errors;
        int filesProcessed = 0;

        QDirIterator it(scanRoot, QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            const QString sourcePath = it.next();
            const QFileInfo sourceInfo(sourcePath);

            if (extensionMatches(sourceInfo, extensionFilter)) {
                ++filesProcessed;
                emit progressed(filesProcessed);

                const QByteArray sourceHash = hashOf(sourcePath);

                bool alreadyPresent = false;
                const QStringList candidates = destPathsByBasename.value(sourceInfo.fileName());
                for (const QString &candidatePath : candidates) {
                    if (hashOf(candidatePath) == sourceHash) {
                        alreadyPresent = true;
                        break;
                    }
                }

                if (alreadyPresent) {
                    ++filesSkipped;
                } else {
                    QString relativeDir;
                    if (preserveStructure) {
                        const QString rel = QDir(scanRoot).relativeFilePath(sourceInfo.absolutePath());
                        if (rel != QStringLiteral("."))
                            relativeDir = rel;
                    }
                    const QString destDir = relativeDir.isEmpty()
                        ? destinationVaultPath
                        : destinationVaultPath + QLatin1Char('/') + relativeDir;
                    QDir().mkpath(destDir);

                    // Same basename already sitting at this exact spot, but
                    // ruled out above as a hash match (i.e. a genuinely
                    // different file) — don't silently overwrite it; append
                    // a short numeric suffix before the extension until we
                    // land on a name that isn't taken (mirrors
                    // VaultTreeModel::importFile()'s dest-exists check
                    // precedent, just resolved instead of rejected).
                    QString destPath = destDir + QLatin1Char('/') + sourceInfo.fileName();
                    if (QFileInfo::exists(destPath)) {
                        const QString baseName = sourceInfo.completeBaseName();
                        const QString suffix = sourceInfo.suffix();
                        int n = 1;
                        do {
                            const QString candidateName = suffix.isEmpty()
                                ? QStringLiteral("%1_%2").arg(baseName).arg(n)
                                : QStringLiteral("%1_%2.%3").arg(baseName).arg(n).arg(suffix);
                            destPath = destDir + QLatin1Char('/') + candidateName;
                            ++n;
                        } while (QFileInfo::exists(destPath));
                    }

                    if (QFile::copy(sourcePath, destPath)) {
                        ++filesCopied;
                        // Keep the index current so a later source file in
                        // this same run that shares this basename also diffs
                        // correctly against what this run has already copied.
                        destPathsByBasename[sourceInfo.fileName()].append(destPath);
                        hashCache.insert(destPath, sourceHash);
                    } else {
                        errors.append(QStringLiteral("Failed to copy \"%1\"").arg(sourceInfo.fileName()));
                    }
                }
            }

            // Give the event loop a chance to deliver a queued requestCancel()
            // (see UsbGrabJob's destructor) between files, rather than only
            // after the entire — potentially large — device has been walked.
            QCoreApplication::processEvents();
            if (m_cancelRequested)
                break;
        }

        emit scanCompleted(filesCopied, filesSkipped, errors);
    }

    void requestCancel() { m_cancelRequested = true; }

signals:
    void progressed(int filesProcessedSoFar);
    void scanCompleted(int filesCopied, int filesSkipped, const QStringList &errors);

private:
    bool m_cancelRequested = false;
};

UsbGrabJob::UsbGrabJob(QObject *parent)
    : QObject(parent)
{
    m_worker = new UsbGrabJobWorker;
    m_worker->moveToThread(&m_thread);
    connect(&m_thread, &QThread::finished, m_worker, &QObject::deleteLater);

    connect(m_worker, &UsbGrabJobWorker::progressed, this, &UsbGrabJob::progressed);
    connect(m_worker, &UsbGrabJobWorker::scanCompleted, this, &UsbGrabJob::handleWorkerFinished);

    m_thread.start();
}

UsbGrabJob::~UsbGrabJob()
{
    // Same "never leave real work running in the background after this
    // object is gone" requirement as AudioRecorder's destructor, reached the
    // same way: queue the cancel request rather than touching the worker's
    // members directly from this (GUI) thread. Posted events for one target
    // are delivered in FIFO order, and the worker's scan loop itself pumps
    // its own event queue between files (see start()'s processEvents() call
    // above), so this is picked up promptly rather than only after the
    // entire device has been walked.
    if (m_worker)
        QMetaObject::invokeMethod(m_worker, "requestCancel", Qt::QueuedConnection);
    m_thread.quit();
    m_thread.wait();
}

void UsbGrabJob::start(const UsbGrabRule &rule, const QString &mountPath, const QString &destinationVaultPath,
                        VaultTreeModel *destinationVaultTreeModel)
{
    m_destinationVaultTreeModel = destinationVaultTreeModel;
    m_deviceSerial = rule.deviceSerial;
    m_deviceLabel = rule.deviceLabel;

    QMetaObject::invokeMethod(m_worker, "start", Qt::QueuedConnection, Q_ARG(QString, mountPath),
                               Q_ARG(QString, rule.startFolder), Q_ARG(QStringList, rule.extensionFilter),
                               Q_ARG(bool, rule.preserveStructure), Q_ARG(QString, destinationVaultPath));
}

void UsbGrabJob::handleWorkerFinished(int filesCopied, int filesSkipped, const QStringList &errors)
{
    // Runs on the GUI thread (normal auto-connection from the worker's
    // cross-thread signal) — safe to call directly into
    // m_destinationVaultTreeModel here, since that object is GUI-thread-owned
    // and this handler is too. The worker itself never touches it (see
    // UsbGrabJob.h's class doc comment).
    if (m_destinationVaultTreeModel) {
        QVariantMap entry;
        entry[QStringLiteral("timestamp")] = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
        entry[QStringLiteral("deviceSerial")] = m_deviceSerial;
        entry[QStringLiteral("deviceLabel")] = m_deviceLabel;
        entry[QStringLiteral("filesCopied")] = filesCopied;
        entry[QStringLiteral("filesSkipped")] = filesSkipped;
        entry[QStringLiteral("errors")] = errors;

        QString appendError;
        if (!m_destinationVaultTreeModel->appendUsbGrabHistoryEntry(entry, &appendError) && !appendError.isEmpty()) {
            QStringList combinedErrors = errors;
            combinedErrors.append(QStringLiteral("Failed to record grab history: %1").arg(appendError));
            emit finished(filesCopied, filesSkipped, combinedErrors);
            return;
        }
    }
    emit finished(filesCopied, filesSkipped, errors);
}

#include "UsbGrabJob.moc"
