#include "UsbGrabJob.h"

#include "VaultTreeModel.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QList>
#include <QRegularExpression>
#include <QVariantMap>

namespace {

// One already-in-the-vault file, reduced to just what the quick-check compares.
// Cheap by construction: every field comes from the directory entry's stat, so
// indexing the whole destination vault never reads a byte of file content.
struct DestFile {
    qint64 size;
    QDateTime lastModified;
};

// Does `candidates` (every vault file sharing the source's basename) already
// contain this exact source file? "Exact" here is rsync's quick-check — same
// basename, same size, same modification time — NOT a content comparison.
//
// This deliberately does not hash. Content hashing is O(bytes on the device),
// and the case it has to answer is precisely the one where the answer is "yes,
// already grabbed": every candidate is a byte-identical copy, so a size check
// can't short-circuit and every file gets read in full. Measured on a 13.8 GB
// recorder over USB at ~9 MB/s, that made a preview take ~25 minutes — the scan
// looked hung. The quick-check reads nothing and answers instantly.
//
// The tradeoff: a file that kept its name, size AND mtime while its bytes
// changed is wrongly skipped. Recorders don't do that (a re-recording changes
// size and stamps a new mtime), and the mtime is what makes this much stronger
// than name+size alone. The cost of being wrong is a missed import the user can
// see in the preview, not data loss — nothing is ever overwritten.
//
// Only sound because the copy side stamps the source's mtime onto the copy (see
// the QFile::copy() branch below); QFile::copy() gives the copy a fresh mtime of
// its own, which would make every grabbed file miss here forever after.
bool alreadyPresentAtDestination(const QFileInfo &sourceInfo, const QList<DestFile> &candidates)
{
    for (const DestFile &candidate : candidates) {
        if (candidate.size == sourceInfo.size() && candidate.lastModified == sourceInfo.lastModified())
            return true;
    }
    return false;
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

// Empty pattern means "no regex constraint" (every file passes) — see
// UsbGrabRule::filenameRegex's doc comment. Matched against the FILE NAME only
// and UNANCHORED, exactly as the split-recording combine rules do (see
// matchRuleCapturedKey() in TranscriptionController.cpp) so one pattern reads
// the same in both features.
//
// An invalid pattern fails CLOSED — it matches nothing, so every file is
// classified "filtered" and the grab copies zero files. The alternative
// (treating unparseable as "no constraint") would silently widen a rule the
// user was trying to narrow, and on a catch-all rule that means bulk-copying a
// whole device on a typo. Zero files copied is recoverable and visible: the
// preview shows everything filtered, and the pattern field's own live ✓/✗
// indicator already says why (see MainPanel.qml's usbGrabberPage).
bool filenameRegexMatches(const QFileInfo &info, const QRegularExpression &filenameRegex)
{
    if (filenameRegex.pattern().isEmpty())
        return true;
    return filenameRegex.match(info.fileName()).hasMatch();
}

// Copy one grabbed file into place, or leave NOTHING at destPath and say why
// (empty return = copied, whole and verified — same "empty means fine" idiom as
// usbGrabRefusalReason() below).
//
// QFile::copy()'s return value is not enough to believe on removable media.
// Verified against Qt 6.11: when the source stops delivering bytes partway — a
// recorder unplugged mid-grab — it returns **true**, having written a truncated
// file. Uncaught, that file lands in the vault wearing a recording's name (the
// tree lists it, it plays, it transcribes into garbage), the run counts it in
// filesCopied, and the history entry reports a clean success. The size check
// below is the whole reason this function exists: it turns silent corruption
// into an ordinary per-file error, which the grab already knows how to report.
//
// Compared against the stat the walk already took, and rejecting a MISMATCH
// rather than only a short file, so it also catches a source still being written
// on the device. Either way what landed isn't the file the scan decided to grab,
// and half a recording is worth less than a re-grab that gets it whole.
//
// Deliberately does NOT stage the bytes under a temp name of its own: Qt copies
// through an unnamed O_TMPFILE inode and linkat()s it into place at the end, so
// a copy that never finishes at all (crash, power loss) leaves the destination
// untouched — verified by SIGKILLing a 3 GB copy mid-flight and finding an empty
// destination folder. Hand-rolled staging would only trade that for .partial
// litter of our own to clean up.
QString copyGrabbedFile(const QFileInfo &sourceInfo, const QString &destPath)
{
    if (!QFile::copy(sourceInfo.absoluteFilePath(), destPath))
        return QStringLiteral("Failed to copy \"%1\"").arg(sourceInfo.fileName());

    if (QFileInfo(destPath).size() != sourceInfo.size()) {
        QFile::remove(destPath);
        return QStringLiteral("Incomplete copy of \"%1\" — was the device removed?")
            .arg(sourceInfo.fileName());
    }

    // Load-bearing, not a nicety: QFile::copy() stamps the copy with the time of
    // the copy, so without this the grabbed file would never match its source's
    // mtime again and EVERY subsequent grab would re-copy the whole device under
    // _1/_2/... suffixes. This is what makes alreadyPresentAtDestination() work
    // at all. Best-effort: a destination that won't reopen still copied fine, and
    // the worst case is that one file looks new next time rather than a failed
    // grab.
    QFile copied(destPath);
    if (copied.open(QIODevice::ReadWrite)) {
        copied.setFileTime(sourceInfo.lastModified(), QFileDevice::FileModificationTime);
        copied.close();
    }
    return QString();
}

QVariantMap makePreviewEntry(const QString &relativePath, const QString &status)
{
    QVariantMap entry;
    entry.insert(QStringLiteral("path"), relativePath);
    entry.insert(QStringLiteral("status"), status);
    return entry;
}

// Symlink-resolved absolute path, falling back to a lexically cleaned absolute
// path when nothing exists at `path` yet. The fallback exists for the
// destination vault specifically: a rule's destinationFolder is created lazily
// (mkpath at copy time), so it legitimately may not exist when the containment
// check below runs, and treating "not there yet" as "no path" would skip the
// check entirely. The fallback can't resolve symlinks, so containment via a
// symlinked not-yet-created destination folder isn't caught — an acceptable gap,
// since the source side (the dangerous one) is always fully canonical.
QString canonicalOrCleaned(const QString &path)
{
    const QFileInfo info(path);
    const QString canonical = info.canonicalFilePath();
    return canonical.isEmpty() ? QDir::cleanPath(info.absoluteFilePath()) : canonical;
}

}

QString usbGrabRefusalReason(const QString &scanRoot, const QString &destinationVaultPath)
{
    const QString source = QFileInfo(scanRoot).canonicalFilePath();
    if (source.isEmpty())
        return QStringLiteral("Source folder \"%1\" does not exist.").arg(scanRoot);
    if (source == QStringLiteral("/"))
        return QStringLiteral("Refusing to grab from the filesystem root \"/\".");

    const QString destination = canonicalOrCleaned(destinationVaultPath);
    // The '/' suffix keeps this a path-component test: without it, a source of
    // "/mnt/rec" would falsely "contain" a destination of "/mnt/recordings".
    if (destination == source || destination.startsWith(source + QLatin1Char('/'))) {
        return QStringLiteral("Refusing to grab from \"%1\": the destination vault is inside it.")
            .arg(source);
    }
    return QString();
}

UsbGrabScanResult runUsbGrabScan(const UsbGrabRule &rule, const QString &mountPath,
                                  const QString &destinationVaultPath, bool dryRun,
                                  const UsbGrabScanHooks &hooks)
{
    UsbGrabScanResult result;

    // Compiled ONCE here rather than inside the walk: QRegularExpression
    // parses its pattern on construction, and the walk below runs this
    // test against every file on a device that can hold tens of thousands
    // of them.
    const QRegularExpression filenameRe(rule.filenameRegex);

    QString scanRoot = mountPath;
    if (!rule.startFolder.isEmpty())
        scanRoot += QLatin1Char('/') + rule.startFolder;

    // Before ANY filesystem work — no destination indexing, no walk, and in
    // particular no copying. See usbGrabRefusalReason()'s doc comment.
    result.refusal = usbGrabRefusalReason(scanRoot, destinationVaultPath);
    if (!result.refusal.isEmpty())
        return result;

    // Index every file already in the destination vault by basename, up
    // front, so each source file's quick-check doesn't need to re-walk the
    // vault per source file. Only stat data is collected (see DestFile), so
    // this stays cheap no matter how large the vault's audio is.
    QHash<QString, QList<DestFile>> destFilesByBasename;
    QDirIterator destIt(destinationVaultPath, QDir::Files, QDirIterator::Subdirectories);
    while (destIt.hasNext()) {
        const QFileInfo info(destIt.next());
        destFilesByBasename[info.fileName()].append({info.size(), info.lastModified()});
    }

    // A denominator for onProgress, from a second walk of the same tree. Only
    // worth its cost when someone is actually watching (the queued grab, which
    // drives a progress bar); the preview reports no progress and skips it.
    // Cheap despite being a whole extra pass: this reads no file CONTENT, only
    // directory entries — the same reason the quick-check itself is instant
    // where a content hash took ~25 minutes (see alreadyPresentAtDestination()).
    int totalCandidates = 0;
    if (hooks.onProgress) {
        QDirIterator countIt(scanRoot, QDir::Files, QDirIterator::Subdirectories);
        while (countIt.hasNext()) {
            const QFileInfo info(countIt.next());
            if (extensionMatches(info, rule.extensionFilter) && filenameRegexMatches(info, filenameRe))
                ++totalCandidates;
        }
    }

    int filesProcessed = 0;

    QDirIterator it(scanRoot, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString sourcePath = it.next();
        const QFileInfo sourceInfo(sourcePath);

        // ANDed, and each is independently "no constraint" when blank —
        // a file must clear BOTH the extension set and the filename regex
        // to be a grab candidate.
        if (!extensionMatches(sourceInfo, rule.extensionFilter)
                || !filenameRegexMatches(sourceInfo, filenameRe)) {
            if (dryRun)
                result.previewEntries.append(makePreviewEntry(QDir(scanRoot).relativeFilePath(sourcePath),
                                                               QStringLiteral("filtered")));
        } else {
            ++filesProcessed;
            if (hooks.onProgress)
                hooks.onProgress(filesProcessed, totalCandidates);

            const bool alreadyPresent = alreadyPresentAtDestination(
                sourceInfo, destFilesByBasename.value(sourceInfo.fileName()));

            if (alreadyPresent) {
                ++result.filesSkipped;
                if (dryRun)
                    result.previewEntries.append(makePreviewEntry(QDir(scanRoot).relativeFilePath(sourcePath),
                                                                   QStringLiteral("duplicate")));
            } else if (dryRun) {
                result.previewEntries.append(
                    makePreviewEntry(QDir(scanRoot).relativeFilePath(sourcePath), QStringLiteral("grab")));
            } else {
                QString relativeDir;
                if (rule.preserveStructure) {
                    const QString rel = QDir(scanRoot).relativeFilePath(sourceInfo.absolutePath());
                    if (rel != QStringLiteral("."))
                        relativeDir = rel;
                }
                const QString destDir = relativeDir.isEmpty()
                    ? destinationVaultPath
                    : destinationVaultPath + QLatin1Char('/') + relativeDir;
                QDir().mkpath(destDir);

                // Same basename already sitting at this exact spot, but
                // ruled out above by the quick-check (i.e. a genuinely
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

                const QString copyError = copyGrabbedFile(sourceInfo, destPath);
                if (copyError.isEmpty()) {
                    ++result.filesCopied;

                    // Keep the index current so a later source file in
                    // this same run that shares this basename also diffs
                    // correctly against what this run has already copied.
                    // Indexed under the SOURCE's size/mtime — which, thanks
                    // to the stamp in copyGrabbedFile(), is exactly what's
                    // now on disk. Only reached for a VERIFIED copy, so a
                    // file that failed mid-copy is never indexed as present.
                    destFilesByBasename[sourceInfo.fileName()].append(
                        {sourceInfo.size(), sourceInfo.lastModified()});
                } else {
                    result.errors.append(copyError);
                }
            }
        }

        if (hooks.shouldAbort && hooks.shouldAbort()) {
            result.aborted = true;
            break;
        }
    }

    return result;
}

// Lives on UsbGrabJob's dedicated QThread; never touches the GUI thread.
// Drives runUsbGrabScan() for the PREVIEW path only — a real grab is a queued
// task on TranscriptionController's worker instead (see UsbGrabberController::
// startGrabJob()), so this worker exists purely to keep the dry-run walk off
// the GUI thread. Unlike AudioRecorderWorker (which sets up capture and then
// idles behind a QTimer, letting the worker's event loop keep running), this
// worker's start() slot runs the entire scan synchronously to completion in one
// call, since there's no ongoing hardware stream to service in the meantime.
class UsbGrabJobWorker : public QObject {
    Q_OBJECT

public:
    using QObject::QObject;

public slots:
    void start(const UsbGrabRule &rule, const QString &mountPath, const QString &destinationVaultPath)
    {
        m_cancelRequested = false;

        UsbGrabScanHooks hooks;
        // No onProgress: nothing renders preview progress, and supplying one
        // would make runUsbGrabScan() pay for its pre-count pass for nothing.
        hooks.shouldAbort = [this] {
            // Give the event loop a chance to deliver a queued requestCancel()
            // (see UsbGrabPreviewJob's destructor) between files, rather than
            // only after the entire — potentially large — device has been
            // walked.
            QCoreApplication::processEvents();
            return m_cancelRequested;
        };

        const UsbGrabScanResult result = runUsbGrabScan(rule, mountPath, destinationVaultPath,
                                                         /*dryRun=*/true, hooks);
        emit previewCompleted(result.previewEntries, result.refusal);
    }

    void requestCancel() { m_cancelRequested = true; }

signals:
    void previewCompleted(const QVariantList &entries, const QString &error);

private:
    bool m_cancelRequested = false;
};

UsbGrabPreviewJob::UsbGrabPreviewJob(QObject *parent)
    : QObject(parent)
{
    // start() hands the whole rule to the worker across a QUEUED connection,
    // which copies it through the metatype system — without this the invoke
    // fails at RUNTIME (silently doing nothing), not at compile time.
    qRegisterMetaType<UsbGrabRule>();

    m_worker = new UsbGrabJobWorker;
    m_worker->moveToThread(&m_thread);
    connect(&m_thread, &QThread::finished, m_worker, &QObject::deleteLater);
    connect(m_worker, &UsbGrabJobWorker::previewCompleted, this, &UsbGrabPreviewJob::previewReady);

    m_thread.start();
}

UsbGrabPreviewJob::~UsbGrabPreviewJob()
{
    // Same "never leave real work running in the background after this
    // object is gone" requirement as AudioRecorder's destructor, reached the
    // same way: queue the cancel request rather than touching the worker's
    // members directly from this (GUI) thread. Posted events for one target
    // are delivered in FIFO order, and the worker's scan loop itself pumps
    // its own event queue between files (see the shouldAbort hook above), so
    // this is picked up promptly rather than only after the entire device has
    // been walked.
    if (m_worker)
        QMetaObject::invokeMethod(m_worker, "requestCancel", Qt::QueuedConnection);
    m_thread.quit();
    m_thread.wait();
}

void UsbGrabPreviewJob::start(const UsbGrabRule &rule, const QString &mountPath, const QString &destinationVaultPath)
{
    QMetaObject::invokeMethod(m_worker, "start", Qt::QueuedConnection, Q_ARG(UsbGrabRule, rule),
                               Q_ARG(QString, mountPath), Q_ARG(QString, destinationVaultPath));
}

#include "UsbGrabJob.moc"
