#pragma once

#include <atomic>

#include <QString>
#include <QUrl>
#include <QVariantList>

#include "../UsbRuleStore.h"
#include "Task.h"

// One mounted VOLUME walked under one resolved rule (see
// UsbGrabberController::startGrabJob()). The walk itself is
// runUsbGrabScan() — shared verbatim with the dry-run preview, see
// UsbGrabJob.h's class doc comment — this task just wires it to the
// phase/progress/pause/cancel contract. This kind's `fileUrl` is the
// destination FOLDER (see Task::fileUrl's OUTPUT convention).
//
// Reports its counts through grabCompleted() rather than TaskError alone: a
// grab's result is "copied N, skipped M, and here are the K files that
// failed", which is not a plain ok/error shape — per-file copy errors are
// non-fatal, so a grab that missed one file is a SUCCEEDED task that has
// something to report, not a failed one.
class UsbGrabTask : public Task {
    Q_OBJECT
public:
    // autoTriggered: true iff a hotplug started this grab by itself
    // (rule.autoGrab), false for a "Grab now" click. Carried purely so
    // UsbGrabberController can tell when the LAST auto-grab of a batch has
    // drained (its autoGrabsSettled() signal) — nothing about how the grab
    // itself runs depends on it.
    // combineRules: AppSettings.combineRules, forwarded to runUsbGrabScan() so
    // the walk skips parts whose split-recording group was already combined
    // (see UsbGrabJob.h). Empty disables that check.
    UsbGrabTask(UsbGrabRule rule, QString mountPath, QUrl destUrl, bool autoTriggered,
                QVariantList combineRules, QObject *parent = nullptr);

    QString kind() const override { return QStringLiteral("usbGrab"); }
    // Creates/moves files in the vault -> the watcher rebuild resets the model,
    // which is the authoritative reflow (see Task::isStructuralFileOp).
    bool isStructuralFileOp() const override { return true; }
    QString kindLabel() const override { return QStringLiteral("USB grab"); }
    QString displayName() const override;
    // Pausable between files (QFile::copy() itself is uninterruptible, so a
    // pause/cancel lands at the next file boundary — see run()'s hooks).
    Capabilities capabilities() const override { return {/*pausable=*/true, /*cancellableMidRun=*/true}; }
    // NOT persisted (old persistQueue()'s explicit opt-out, STRONGER reason
    // than import: m_mountPath is a mount point for removable media very
    // likely gone next session, and a reused mount point could make a
    // restored task walk a DIFFERENT volume entirely). Replugging re-fires
    // the auto-grab; that's the correct way to resume one. isPersistable()
    // stays false, so toJson() is never called.
    QVariantMap toVariantMap() const override;

    void run(TaskContext &ctx) override;

    const UsbGrabRule &rule() const { return m_rule; }
    const QString &mountPath() const { return m_mountPath; }
    bool autoTriggered() const { return m_autoTriggered; }

signals:
    void grabCompleted(int filesCopied, int filesSkipped, const QStringList &errors, bool cancelled);
    // Fired once the run's pre-count pass knows how many files match — lets the
    // Tasks row rename itself from "<drive>" to "<drive> — N files". Emitted on
    // the worker thread; the controller turns it into a queueChanged().
    void fileCountKnown();

private:
    UsbGrabRule m_rule;
    QString m_mountPath;
    bool m_autoTriggered;
    QVariantList m_combineRules;
    // Candidate file count from the pre-count pass, -1 until known. Written on
    // the worker thread (run()'s onTotalsKnown hook), read on the GUI thread
    // (toVariantMap) — atomic so the cross-thread read is defined.
    std::atomic<int> m_fileCount{-1};
};
