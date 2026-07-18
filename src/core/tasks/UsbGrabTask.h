#pragma once

#include <QString>
#include <QUrl>

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
    UsbGrabTask(UsbGrabRule rule, QString mountPath, QUrl destUrl, bool autoTriggered, QObject *parent = nullptr);

    QString kind() const override { return QStringLiteral("usbGrab"); }
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

private:
    UsbGrabRule m_rule;
    QString m_mountPath;
    bool m_autoTriggered;
};
