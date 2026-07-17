#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QThread>
#include <QVariantList>

#include <functional>

#include "UsbRuleStore.h"

class UsbGrabJobWorker;

// Why a grab of scanRoot into destinationVaultPath must not run, as a
// user-facing sentence — or an empty string if it's safe to proceed.
//
// Defence in depth, deliberately redundant with UsbDeviceMonitor's
// usbDeviceIsExternalMedia() gate: that one decides what may become a device at
// all, this one re-checks the concrete paths a run is about to walk, so a rule
// pointed somewhere catastrophic is refused even if enumeration ever hands up
// something it shouldn't have. The two failures it catches:
//   - a scan root that resolves to "/" — the whole-filesystem recursive copy that
//     an over-broad catch-all rule would otherwise perform;
//   - a destination vault that lives INSIDE the scan root, which makes the walk
//     feed itself the files it is copying.
// Both are checked against canonical (symlink-resolved) paths so neither can be
// slipped past with a link or a "..". A scan root that doesn't exist is refused
// too, since a non-existent source is a misconfigured rule, not an empty device.
QString usbGrabRefusalReason(const QString &scanRoot, const QString &destinationVaultPath);

// What one runUsbGrabScan() pass did (or, in dry-run mode, would have done).
struct UsbGrabScanResult {
    int filesCopied = 0;
    int filesSkipped = 0;
    // Per-file copy failures. Non-fatal by design: a grab that can't copy one
    // file still copies the rest and reports what it missed.
    QStringList errors;
    // Dry-run only — the {path, status} classification of EVERY file the scan
    // saw (see UsbGrabJob::startPreview()). Always empty for a real grab.
    QVariantList previewEntries;
    // Non-empty iff usbGrabRefusalReason() rejected the run before any
    // filesystem work happened; every other field is then at its default.
    QString refusal;
    // True iff shouldAbort() ended the walk early, so the counts above are a
    // partial tally rather than the whole device. Callers must distinguish
    // these: a cancelled grab must not be reported as a completed one.
    bool aborted = false;
};

// The two things the walk needs from whoever is driving it. Both are optional
// (a default-constructed UsbGrabScanHooks means "no progress reporting, never
// abort"), because the callers want opposite things from each: the preview
// worker pumps its own event loop to catch a queued cancel, while the task
// queue's worker polls atomics and blocks on pause — neither shape belongs
// inside the walk itself.
struct UsbGrabScanHooks {
    // Called once per CANDIDATE file considered (filter-rejected files don't
    // count), with the running total and the total number of candidates the
    // pre-count pass found. Coarse by design — no byte-level progress.
    std::function<void(int filesProcessedSoFar, int totalCandidates)> onProgress;
    // Polled between files. Returning true stops the walk where it stands and
    // sets UsbGrabScanResult::aborted. Also the hook where a caller that needs
    // to pump an event loop or block on a pause does so.
    std::function<bool()> shouldAbort;
};

// The whole recursive scan+filter+quick-check-dedup+copy for ONE rule against
// ONE mounted volume, as a plain synchronous function over its inputs — the
// single copy of this logic in the codebase, called from two very different
// threading contexts (see UsbGrabScanHooks):
//   - UsbGrabJobWorker, on UsbGrabJob's own QThread, for the dry-run preview;
//   - TranscriptionWorker::usbGrabJob(), on the task queue's worker thread,
//     for a real queued grab (see TranscriptionController's "usbGrab" kind).
//
// Deliberately knows nothing about QThread, the job queue, history entries, or
// VaultTreeModel: it walks, it copies, it returns what happened. Everything
// GUI-thread-owned (the history entry, the snackbar) is the caller's job.
//
// dryRun classifies every file the scan sees without touching the filesystem at
// all — no mkpath, no QFile::copy — filling previewEntries instead of copying.
// Everything else (filters, the dedup index, the abort check) is identical
// between the two modes, which is exactly why they share one function.
UsbGrabScanResult runUsbGrabScan(const UsbGrabRule &rule, const QString &mountPath,
                                  const QString &destinationVaultPath, bool dryRun,
                                  const UsbGrabScanHooks &hooks = {});

// Runs one DRY-RUN grab preview (runUsbGrabScan(dryRun=true) for one
// UsbGrabRule against one already-mounted volume) off the GUI thread. Mirrors
// AudioRecorder's threading shape (see AudioRecorder.h's class doc comment): a
// QThread MEMBER plus a moveToThread()'d worker object — not a QThread
// subclass. Cross-thread calls into the worker go through
// QMetaObject::invokeMethod(..., Qt::QueuedConnection); the worker's signal is
// consumed back on the GUI thread via plain Qt::AutoConnection (which resolves
// to queued, cross-thread).
//
// PREVIEW ONLY, deliberately. A real grab is not this class — it's a queued
// "usbGrab" task on TranscriptionController's shared worker (see
// UsbGrabberController::startGrabJob()), which is what gives it a Tasks row,
// pause/cancel, and progress like every other long operation in the app. This
// class survives that move because a preview must NOT queue behind a running
// transcription: it's an interactive answer to "what would this rule do?",
// wanted immediately while editing the rule, and it writes nothing.
//
// Not QML_ELEMENT: QML never talks to this class directly. It's a
// C++-internal helper instantiated by UsbGrabberController — the same role
// AudioRecorderWorker plays relative to AudioRecorder, except here the
// "controller" is a whole separate class rather than AudioRecorder itself.
class UsbGrabPreviewJob : public QObject {
    Q_OBJECT

public:
    explicit UsbGrabPreviewJob(QObject *parent = nullptr);
    ~UsbGrabPreviewJob() override;

    // Kicks off the dry-run scan on the worker thread. Copies nothing, writes
    // no history entry, and touches the filesystem only to read directory
    // entries — see runUsbGrabScan()'s dryRun parameter for the classification
    // it produces, and previewReady() below for how it comes back.
    void start(const UsbGrabRule &rule, const QString &mountPath, const QString &destinationVaultPath);

signals:
    // Emitted once the dry run finishes. entries is a list of
    // {"path": <relative to the scan root>, "status": ...} maps, status one of
    // "grab" (matches the filters, not already present — would be copied),
    // "duplicate" (matches the filters, a file with the same basename, size and
    // mtime already exists at the destination — would be skipped), or
    // "filtered" (failed the extension filter and/or the filename regex —
    // never considered for copying). One "filtered" status covers both filters
    // rather than distinguishing them: the entry carries the path, and the
    // pattern field's own live ✓/✗ indicator is what explains a regex that
    // doesn't compile.
    //
    // error is empty on a normal run; when usbGrabRefusalReason() rejected the
    // scan it carries that reason and entries is empty, so a refused preview
    // reports WHY rather than looking like a device with nothing on it.
    void previewReady(const QVariantList &entries, const QString &error);

private:
    QThread m_thread;
    UsbGrabJobWorker *m_worker = nullptr;
};
