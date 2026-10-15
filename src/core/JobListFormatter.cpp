#include "JobListFormatter.h"

#include "ModelManager.h"

#include <QHash>
#include <QList>
#include <QStringList>
#include <QUrl>
#include <QVariantMap>
#include <QVector>

#include <algorithm>
#include <utility>

namespace {
// The ONE authoritative phase vocabulary. Each emitted phase slug appears
// exactly once, carrying both presentations that used to be hand-kept switches
// in QML (BottomBar's phaseLabel + MainPanel's transcribingOverlayLabel):
//   label        — the bottom-bar short form ("loading model…"), lowercase.
//   overlayLabel — the main-panel blocking-overlay title, title-case action
//                  ("Loading model…"); EMPTY for a phase that never raises the
//                  overlay (Background kinds: labeling/grabbing/benchmarking).
// Every phase any Task::run() emits MUST have a row here (label non-empty);
// tst_styleguide's phaseRegistryCoversEveryEmittedPhase guard scans the task
// sources and fails if one is missing. The three "detecting-speakers*" rows
// share one overlay title (the finer split only matters for the bottom bar).
struct PhaseInfo {
    QString slug;
    QString label;
    QString overlayLabel;
};
const PhaseInfo *phaseInfoFor(const QString &phase)
{
    // A function-local static (rather than a namespace-scope `static const`)
    // so it can never be read before it's initialized — the table holds real
    // QString objects, so a cross-TU read at static-init time would silently
    // see an empty registry (every phase label blank).
    static const QVector<PhaseInfo> kPhases = {
        {QStringLiteral("loading"), JobListFormatter::tr("loading model…"),
         JobListFormatter::tr("Loading model…")},
        {QStringLiteral("decoding"), JobListFormatter::tr("decoding audio…"),
         JobListFormatter::tr("Decoding audio…")},
        {QStringLiteral("detecting-speech"), JobListFormatter::tr("detecting speech…"),
         JobListFormatter::tr("Detecting speech…")},
        // DetectSilenceTask (silence-capture feature, phase 1's backfill job)
        // — Presentation::Foreground by default (like DiarizeTask), so it
        // needs a real overlay title, not the empty-string "no overlay" rows
        // below.
        {QStringLiteral("detecting-silence"), JobListFormatter::tr("detecting silence…"),
         JobListFormatter::tr("Detecting silence…")},
        {QStringLiteral("detecting-speakers"), JobListFormatter::tr("detecting speakers…"),
         JobListFormatter::tr("Detecting speakers…")},
        {QStringLiteral("detecting-speakers-segmenting"), JobListFormatter::tr("finding speech…"),
         JobListFormatter::tr("Detecting speakers…")},
        {QStringLiteral("detecting-speakers-identifying"), JobListFormatter::tr("identifying speakers…"),
         JobListFormatter::tr("Detecting speakers…")},
        {QStringLiteral("transcribing"), JobListFormatter::tr("transcribing…"),
         JobListFormatter::tr("Transcribing…")},
        {QStringLiteral("downloading"), JobListFormatter::tr("downloading…"),
         JobListFormatter::tr("Downloading…")},
        {QStringLiteral("combining"), JobListFormatter::tr("combining…"),
         JobListFormatter::tr("Combining…")},
        {QStringLiteral("chaptering"), JobListFormatter::tr("chaptering…"),
         JobListFormatter::tr("Generating chapters…")},
        {QStringLiteral("merging-chapters"), JobListFormatter::tr("merging chapters…"),
         JobListFormatter::tr("Generating chapters…")},
        // No overlay wording: import is Presentation::Background now (see
        // ImportTask::presentation()), so it must never claim the main-panel
        // overlay. The bottom-bar label column stays — the row itself is
        // required by tst_styleguide's emitted-phase-has-a-registry-row guard.
        {QStringLiteral("importing"), JobListFormatter::tr("importing…"), QString()},
        {QStringLiteral("grabbing"), JobListFormatter::tr("grabbing…"), QString()},
        {QStringLiteral("labeling"), JobListFormatter::tr("labeling…"), QString()},
        {QStringLiteral("benchmarking"), JobListFormatter::tr("benchmarking…"), QString()},
        // CompressAudioTask is Background: label column only.
        {QStringLiteral("encoding"), JobListFormatter::tr("compressing…"), QString()},
        {QStringLiteral("verifying"), JobListFormatter::tr("verifying…"), QString()},
    };
    for (const auto &info : kPhases) {
        if (info.slug == phase)
            return &info;
    }
    return nullptr;
}
} // namespace

QString JobListFormatter::phaseLabel(const QString &phase)
{
    const PhaseInfo *info = phaseInfoFor(phase);
    return info ? info->label : QString();
}

QString JobListFormatter::phaseOverlayLabel(const QString &phase)
{
    // The blocking-overlay title. Empty when the phase has no overlay wording
    // (a Background kind, or an unknown slug) — the caller supplies its own
    // generic fallback ("Transcribing…").
    const PhaseInfo *info = phaseInfoFor(phase);
    return info ? info->overlayLabel : QString();
}

QString JobListFormatter::modelShortLabel(const QString &modelId)
{
    auto headOf = [](const QString &label) {
        return label.section(QStringLiteral(" — "), 0, 0).trimmed();
    };
    for (const auto &info : ModelManager::availableModels()) {
        if (info.id == modelId)
            return headOf(info.label).isEmpty() ? modelId : headOf(info.label);
    }
    for (const auto &info : ModelManager::availableSegmentationModels()) {
        if (info.id == modelId)
            return headOf(info.label).isEmpty() ? modelId : headOf(info.label);
    }
    for (const auto &info : ModelManager::availableEmbeddingModels()) {
        if (info.id == modelId)
            return headOf(info.label).isEmpty() ? modelId : headOf(info.label);
    }
    return modelId;
}

QVariantList JobListFormatter::collapseBatches(const QVariantList &jobEntries)
{
    // First pass: walk in append order, recording the display order as a
    // sequence of tokens (a standalone row, or the FIRST appearance of a
    // batchId) and bucketing each batch's member rows. Preserving first-seen
    // order keeps a batch sitting where its earliest member would have.
    QVariantList out;
    QList<int> batchOrder;                 // batchIds in first-seen order
    QHash<int, QVariantList> members;      // batchId -> its member rows
    QHash<int, int> placeholderAt;         // batchId -> index in `out` reserved for it

    for (const QVariant &v : jobEntries) {
        const QVariantMap row = v.toMap();
        const int batchId = row.value(QStringLiteral("batchId")).toInt();
        if (batchId == 0) {
            out.append(row);
            continue;
        }
        if (!members.contains(batchId)) {
            placeholderAt.insert(batchId, out.size());
            out.append(QVariant()); // reserve the slot; filled in the second pass
            batchOrder.append(batchId);
        }
        members[batchId].append(row);
    }

    // Second pass: turn each batch's members into ONE aggregate row (or, for a
    // lone member, the member itself — no pointless "1 file" collapse).
    for (const int batchId : std::as_const(batchOrder)) {
        const QVariantList &group = members[batchId];
        const int slot = placeholderAt.value(batchId);
        if (group.size() == 1) {
            out[slot] = group.first();
            continue;
        }

        // Distinct files (a run-all batch has up to 4 tasks per file, so the
        // member count overstates the file count — the row should say "5
        // files", not "20"). A file counts as done only when ALL its tasks are
        // Done; genuinely failed (not a user cancel) if any of its tasks are.
        QStringList distinctFiles;
        QHash<QString, bool> fileAllDone;      // seeded true, cleared by any non-Done task
        QHash<QString, bool> fileHasFailure;   // any genuine (non-cancel) failure
        bool anyActive = false, anyQueued = false, anyPaused = false;
        bool anyDone = false, anyGenuineFailure = false, anyCancelled = false;
        bool anyUndoable = false; // any Done member advertises an undo (see jobs())
        bool anyUndoApplicable = false; // any undoable member still applicable
        qint64 durationSum = 0;
        bool allTerminal = true;
        QString label = group.first().toMap().value(QStringLiteral("batchLabel")).toString();
        if (label.isEmpty())
            label = group.first().toMap().value(QStringLiteral("kindLabel")).toString();

        for (const QVariant &mv : group) {
            const QVariantMap m = mv.toMap();
            const QString status = m.value(QStringLiteral("status")).toString();
            const QString fileKey = m.value(QStringLiteral("fileUrl")).toUrl().toString();
            const bool cancelled = status == QStringLiteral("failed")
                && m.value(QStringLiteral("error")).toString() == QStringLiteral("Cancelled");
            if (!distinctFiles.contains(fileKey)) {
                distinctFiles.append(fileKey);
                fileAllDone.insert(fileKey, true);
                fileHasFailure.insert(fileKey, false);
            }
            if (status != QStringLiteral("done"))
                fileAllDone[fileKey] = false;
            if (status == QStringLiteral("failed") && !cancelled)
                fileHasFailure[fileKey] = true;

            if (status == QStringLiteral("active")) { anyActive = true; allTerminal = false; }
            else if (status == QStringLiteral("queued")) { anyQueued = true; allTerminal = false; }
            else if (status == QStringLiteral("paused")) { anyPaused = true; allTerminal = false; }
            else if (status == QStringLiteral("done")) anyDone = true;
            else if (cancelled) anyCancelled = true;
            else anyGenuineFailure = true;

            if (m.value(QStringLiteral("undoable")).toBool()) {
                anyUndoable = true;
                if (!m.value(QStringLiteral("undoStale")).toBool())
                    anyUndoApplicable = true;
            }

            const qint64 d = m.value(QStringLiteral("durationMs")).toLongLong();
            if (d > 0)
                durationSum += d;
        }

        int filesDone = 0, filesFailed = 0;
        for (const QString &f : std::as_const(distinctFiles)) {
            if (fileHasFailure.value(f))
                ++filesFailed;
            else if (fileAllDone.value(f))
                ++filesDone;
        }

        // Aggregate status priority: any still-running phase wins so the row
        // reads as live; among terminal-only batches a genuine failure surfaces
        // as failed, an all-cancelled batch reads as a cancel, otherwise done.
        QString aggStatus;
        QString aggError;
        if (anyActive) aggStatus = QStringLiteral("active");
        else if (anyQueued) aggStatus = QStringLiteral("queued");
        else if (anyPaused) aggStatus = QStringLiteral("paused");
        else if (anyGenuineFailure) { aggStatus = QStringLiteral("failed");
            aggError = QStringLiteral("%1 of %2 failed").arg(filesFailed).arg(distinctFiles.size()); }
        else if (anyCancelled && !anyDone) { aggStatus = QStringLiteral("failed");
            aggError = QStringLiteral("Cancelled"); }
        else aggStatus = QStringLiteral("done");

        QVariantMap agg;
        agg[QStringLiteral("isBatch")] = true;
        agg[QStringLiteral("batchId")] = batchId;
        agg[QStringLiteral("id")] = batchId; // unique (shares the task-id counter) — safe row key
        agg[QStringLiteral("kind")] = QStringLiteral("batch");
        agg[QStringLiteral("kindLabel")] = label;
        agg[QStringLiteral("status")] = aggStatus;
        agg[QStringLiteral("error")] = aggError;
        agg[QStringLiteral("fileUrl")] = QUrl(); // aggregate has no single file
        agg[QStringLiteral("batchCount")] = distinctFiles.size();
        agg[QStringLiteral("batchDoneCount")] = filesDone;
        agg[QStringLiteral("batchFailedCount")] = filesFailed;
        agg[QStringLiteral("durationMs")] = allTerminal ? durationSum : qint64(-1);
        // The header's Undo button undoes every Done member at once
        // (undoTask(batchId)); offer it iff at least one member is undoable.
        agg[QStringLiteral("undoable")] = anyUndoable;
        // Grey the header undo only when EVERY undoable member is stale — a mixed
        // batch (some members still applicable) keeps the header undo live.
        agg[QStringLiteral("undoStale")] = anyUndoable && !anyUndoApplicable;
        agg[QStringLiteral("members")] = group; // the per-task rows, for the expandable UI
        out[slot] = agg;
    }
    return out;
}

QVariantList JobListFormatter::sortJobsForDisplay(const QVariantList &jobEntries)
{
    QVariantList active;
    QVariantList queued;
    QVariantList paused;
    QVariantList finished;
    for (const QVariant &v : jobEntries) {
        const QString status = v.toMap().value(QStringLiteral("status")).toString();
        if (status == QStringLiteral("active"))
            active.append(v);
        else if (status == QStringLiteral("queued"))
            queued.append(v);
        else if (status == QStringLiteral("paused"))
            paused.append(v);
        else // "done" / "failed" (a cancel is a Failed/"Cancelled" — still finished)
            finished.append(v);
    }
    // Finished group: most-recently-finished first. jobs() is append order and
    // jobs run/finish in that order, so reversing this group alone gives
    // newest-finished-on-top without needing a separate finishedAt timestamp.
    std::reverse(finished.begin(), finished.end());
    QVariantList out;
    out.reserve(jobEntries.size());
    out += active;
    out += queued;
    out += paused;
    out += finished;
    return out;
}
