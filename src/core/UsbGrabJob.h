#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QThread>

#include "UsbRuleStore.h"

class VaultTreeModel;
class UsbGrabJobWorker;

// Runs one USB grab (recursive scan + copy for one UsbGrabRule against one
// already-mounted device) entirely off the GUI thread. Mirrors
// AudioRecorder's threading shape EXACTLY (see AudioRecorder.h's class doc
// comment): a QThread MEMBER plus a moveToThread()'d worker object — NOT a
// QThread subclass, NOT TranscriptionController's bespoke job-queue-with-
// flags pattern, since this is its own standalone subsystem. Cross-thread
// calls into the worker go through QMetaObject::invokeMethod(...,
// Qt::QueuedConnection); the worker's own signals are consumed back on the
// GUI thread via plain Qt::AutoConnection (which resolves to queued,
// cross-thread) — the same one-way direction of flow as
// AudioRecorderWorker::stopped -> AudioRecorder::recordingFinished. Note the
// worker never calls into destinationVaultTreeModel directly (that object is
// GUI-thread-owned); instead it emits its raw scan results, and THIS
// GUI-thread-side object records the history entry once that signal
// arrives — see UsbGrabJob.cpp's handleWorkerFinished().
//
// Not QML_ELEMENT: QML never talks to this class directly. It's a
// C++-internal worker instantiated by UsbGrabberController (a later, separate
// step that owns/dispatches UsbGrabJob instances per device/rule) — the same
// role AudioRecorderWorker plays relative to AudioRecorder, except here the
// "controller" is a whole separate class rather than AudioRecorder itself,
// so UsbGrabJob is the QML_ELEMENT-less internal piece.
class UsbGrabJob : public QObject {
    Q_OBJECT

public:
    explicit UsbGrabJob(QObject *parent = nullptr);
    ~UsbGrabJob() override;

    // Kicks off the scan+copy on the worker thread: recursively walks
    // mountPath + "/" + rule.startFolder (or just mountPath if startFolder is
    // empty), copies every file whose extension matches rule.extensionFilter
    // (case-insensitive; an empty filter means "all files", not zero files)
    // and that isn't already present in destinationVaultPath under the same
    // basename + content hash, preserving relative subfolder structure under
    // destinationVaultPath if rule.preserveStructure is true (flattened into
    // destinationVaultPath's root otherwise). destinationVaultTreeModel is
    // used only once the run completes, to append one history entry via its
    // Q_INVOKABLE appendUsbGrabHistoryEntry() — it must live on the GUI
    // thread (the same thread this UsbGrabJob itself lives on), since that
    // call happens directly, not through a queued invoke (see the class doc
    // comment above).
    Q_INVOKABLE void start(const UsbGrabRule &rule, const QString &mountPath, const QString &destinationVaultPath,
                            VaultTreeModel *destinationVaultTreeModel);

signals:
    // Coarse-grained: one emission per file the scan has finished
    // considering (matched-but-skipped files count too), not per-byte — no
    // byte-level progress is needed for this feature.
    void progressed(int filesProcessedSoFar);
    // Emitted once the run is fully done, AFTER the history entry has
    // already been appended (see start()'s doc comment).
    void finished(int filesCopied, int filesSkipped, const QStringList &errors);

private slots:
    // Connected to the worker's scanCompleted() signal in the constructor —
    // records the history entry (GUI-thread-safe, since this runs here, not
    // on the worker thread) and then re-emits this object's own finished().
    void handleWorkerFinished(int filesCopied, int filesSkipped, const QStringList &errors);

private:
    QThread m_thread;
    UsbGrabJobWorker *m_worker = nullptr;

    // Snapshotted at start() time so handleWorkerFinished() can build the
    // history entry without needing the worker to carry GUI-thread state
    // back across the thread boundary.
    VaultTreeModel *m_destinationVaultTreeModel = nullptr;
    QString m_deviceSerial;
    QString m_deviceLabel;
};
