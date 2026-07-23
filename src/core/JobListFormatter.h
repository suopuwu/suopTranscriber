#pragma once

#include <QCoreApplication>
#include <QString>
#include <QVariantList>

// The job-list PRESENTATION collaborator split out of TranscriptionController
//. It owns exactly two things:
//
//   1. the ONE authoritative phase vocabulary — the slug -> (bottom-bar label,
//      main-panel overlay title) registry behind phaseLabel()/
//      phaseOverlayLabel(), and
//   2. the two pure display transforms a jobs()-shaped list goes through
//      before the Tasks list renders it: collapseBatches() (fold a
//      multi-select batch's rows into one aggregate row) and
//      sortJobsForDisplay() (partition into Active/Queued/Paused/finished).
//
// Everything here is a function of its arguments — it owns NO queue state and
// holds no reference to the controller. Building the raw job rows requires
// walking m_tasks (each Task authors its own row via toVariantMap(), and the
// batch labels live in the controller's m_batchLabels), so
// TranscriptionController::jobs() stays where the queue is; it hands the
// finished list here for collapsing/sorting. Likewise currentJobKindLabel()
// stays on the controller — it's a lookup of the Active task, not a mapping
// over an argument.
//
// Stateless by construction — never instantiated, hence the deleted
// constructor. The controller holds no member for it; it calls these statics
// directly, and its own Q_INVOKABLE phaseLabel()/phaseOverlayLabel() plus the
// public statics collapseBatches()/sortJobsForDisplay() remain on the
// controller as thin forwarders so QML's (and the tests') call sites are
// unchanged.
class JobListFormatter
{
    // Non-QObject, so tr() comes from here. The phase registry's user-visible
    // strings were TranscriptionController::tr() before the split; the app
    // ships no .ts catalogs, so the context change is inert.
    Q_DECLARE_TR_FUNCTIONS(JobListFormatter)

public:
    JobListFormatter() = delete;

    // Human-readable label for a phase slug ("loading" -> "loading model…"),
    // the SINGLE source of truth for the phase vocabulary's display text.
    // Replaced BottomBar's phaseLabel(phase) switch — a new emitted phase that
    // forgot a QML case rendered blank; now the label lives next to the rest
    // of the phase semantics, and tst_styleguide guards that every emitted
    // phase slug is covered here. Returns empty for an unknown slug.
    static QString phaseLabel(const QString &phase);

    // The blocking-overlay title for a phase ("loading" -> "Loading model…"),
    // the title-case action variant of phaseLabel. Empty for a phase that
    // never raises the main-panel overlay (a Background kind) or an unknown
    // slug — MainPanel falls back to a generic "Transcribing…". Same single
    // phase registry as phaseLabel(); this is its overlay column.
    static QString phaseOverlayLabel(const QString &phase);

    // Collapse the rows of a multi-select batch (each carrying the same
    // non-zero "batchId", stamped by enqueueBatch()) into ONE aggregate row so
    // a bulk op appears as a single Tasks entry — "at least visually" one task,
    // while the underlying per-file tasks keep running/reporting individually
    // (that per-file granularity is what the queue, the tree's status dots, and
    // per-file cancel all depend on; see DESIGN.md "Task system"). The
    // aggregate carries a "members" list (the original per-task rows) for the
    // expandable UI, plus aggregate status/counts. A batch of ONE member is
    // emitted flat (no pointless "1 file" row). Standalone rows (batchId 0)
    // pass through untouched. Pure + static so it's unit-testable like
    // sortJobsForDisplay(); sortedJobs() applies it before partitioning.
    static QVariantList collapseBatches(const QVariantList &jobEntries);

    // Pure partition of a jobs()-shaped list (each entry a QVariantMap with a
    // "status" key of "active"/"queued"/"paused"/"done"/"failed"): Active,
    // then Queued, then Paused, then finished (done/failed) — the last group
    // reversed so most-recently-appended (≈ most-recently-finished, since
    // jobs run and finish in append order) is first. Relative order WITHIN
    // the non-finished groups is preserved (stable), so Queued stays in run
    // order. Static + public purely so tst_transcription_queue can assert the
    // ordering deterministically without spinning up real jobs.
    static QVariantList sortJobsForDisplay(const QVariantList &jobEntries);
};
