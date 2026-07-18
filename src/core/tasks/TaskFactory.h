#pragma once

#include <QJsonObject>
#include <QString>

#include <memory>

class Task;

// Rebuilds one Task from a persisted-queue entry — the inverse of
// Task::toJson() plus the queue's own kind/fileUrl/vaultUrl fields. The ONE
// place that maps a kind slug back to a concrete class, replacing the old
// enqueueReplayedJob()'s per-kind if/else chain.
//
// Returns nullptr for an unrecognized kind, or for a kind whose
// isPersistable() is false (import/usbGrab — they should never have been
// written in the first place, but a hand-edited or stale queue file must not
// resurrect one; the check makes the opt-in contract enforced on BOTH ends,
// not just at write time).
std::unique_ptr<Task> createTaskFromJson(const QString &kind, const QJsonObject &obj);
