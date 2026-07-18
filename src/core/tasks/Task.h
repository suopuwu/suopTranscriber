#pragma once

#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QUrl>
#include <QVariantMap>

#include "ModelDownloadTarget.h"
#include "TaskContext.h"

// One unit of queued work. Replaces the old 30-field `Job` union + 9
// mutually-exclusive bool flags + jobKind() waterfall + the 10-branch
// startJob() dispatch: each concrete kind is now ONE subclass whose doing-code
// (run()) lives next to its reporting-code (its progress/phase/result signals,
// displayName(), toVariantMap()). Adding a kind is subclassing this — not
// editing five disjoint sites and hoping you found them all.
//
// THREADING. A Task is created and owned on the GUI thread (the queue holds it
// for its whole lifetime, including as a Done/Failed history row). Its run()
// executes on the WORKER thread, driven by TaskRunner. The split is safe
// because:
//   - the metadata getters (kind/displayName/presentation/capabilities/
//     isPersistable/toVariantMap) are const and read only immutable state set
//     at construction, so QML may call them while run() executes;
//   - run() communicates ONLY by emitting this object's signals (Qt delivers
//     them queued to GUI-thread connections) and never mutates state the GUI
//     thread reads.
// The queue-owned lifecycle fields below (id/status/error/durationMs) are
// written by the SCHEDULER on the GUI thread, never by run().
class Task : public QObject {
    Q_OBJECT
public:
    // Foreground: this task owns the main-panel overlay ("Transcribing…" et
    // al.) plus the bottom-bar glow while Active. Background: glow + a Tasks
    // row, but NO overlay — a benchmark runs without hijacking the panel the
    // user is reading. There is deliberately no Silent: an invisible task is
    // exactly the failure mode this whole refactor exists to make impossible.
    // Presentation controls overlay-vs-not, NEVER whether the task appears in
    // the Tasks list — everything queued gets a row.
    enum class Presentation { Foreground, Background };

    // What the UI may offer for this task, so the controls ASK the task
    // instead of guessing (the guess is what caused the 2026-07-09 pause-vs-
    // cancel latch bug). pausable: has a real cooperative interruption point
    // (calls TaskContext::checkpoint()). cancellableMidRun: a cancel takes
    // effect before the task finishes naturally (false for atomic single-pass
    // kinds — rename/merge/diarize have no mid-pass abort hook, so a cancel is
    // only honored at the next task boundary).
    struct Capabilities {
        bool pausable = false;
        bool cancellableMidRun = true;
    };

    enum class Status { Queued, Active, Paused, Done, Failed };

    using QObject::QObject;

    // --- The kind's identity (const; safe from any thread) ---
    virtual QString kind() const = 0;        // stable slug, was jobKind()
    virtual QString displayName() const = 0; // Tasks-popup row text
    virtual Presentation presentation() const { return Presentation::Foreground; }
    virtual Capabilities capabilities() const { return {}; }
    // Opt-IN persistence (default: not saved across a restart). A kind whose
    // inputs name an external/removable path (import, usbGrab) simply never
    // overrides this — no more persistQueue() opt-OUT that a new kind forgets.
    // A kind that overrides this to true MUST also override toJson() with
    // whatever fromJson() needs to rebuild it (fileUrl/vaultUrl/kind are
    // written by the queue; everything else is this kind's own business).
    virtual bool isPersistable() const { return false; }

    // This kind's own fields for a persisted queue entry. The queue writes
    // kind/fileUrl/vaultUrl around this, and createTaskFromJson() (see
    // TaskFactory.h) rebuilds the kind from the merged object. Only ever
    // called when isPersistable() is true. Default: no extra fields.
    virtual QJsonObject toJson() const { return {}; }

    // The models this kind needs ON DISK before it can run. The queue turns
    // each into its own DownloadTask inserted AHEAD of this one (deduped) —
    // so a kind declares its model needs ONCE, here, instead of every
    // enqueue site (and the persisted-queue replay path) each remembering to
    // queue the right downloads for it. Default: needs nothing.
    virtual ModelPrerequisites prerequisites() const { return {}; }

    // This kind's Tasks-row fields for QML, merged over the base row the queue
    // builds (id/kind/status/fileUrl/displayName/error/durationMs). Was the
    // giant per-flag switch in jobs(); now each kind authors its own row.
    // Default: no extra fields.
    virtual QVariantMap toVariantMap() const { return {}; }

    // The work. Executes on the worker thread. Reports via the signals below;
    // returns normally on success, or throws TaskError (see below) on failure
    // — the runner turns either into the single terminal report the queue
    // guarantees. A pausable kind calls ctx.checkpoint() at safe points and
    // returns early when it reports cancellation.
    virtual void run(TaskContext &ctx) = 0;

    // --- Queue-owned lifecycle state (GUI thread writes; run() must not) ---
    int id = 0;
    Status status = Status::Queued;
    QString error;
    qint64 durationMs = -1; // wall-clock Active time, set when the task ends

    // --- Queue-owned identity (GUI thread writes BEFORE dispatch; run() only
    // reads) ---
    // Whatever this kind's ONE file is: the INPUT for transcribe/diarize/
    // relabel/chapters, the OUTPUT for combine/import/usbGrab, empty for
    // download (nothing to open). Same "reuse one url for whatever this kind's
    // one file is" convention the old Job struct used.
    //
    // Lives on the base, not in each subclass, because it is genuinely the
    // QUEUE's business, not the kind's: dedup (isQueued), the path->status
    // index the file tree reads, dismiss(), currentFile, and finished()'s
    // identifying url all key off it uniformly across every kind. It is also
    // MUTABLE for exactly one reason — the combine preflight retargets a
    // transcribe at a merged output before dispatching it (see
    // TranscriptionController::maybeRedirectForCombine()). Mutating it after
    // run() has started would be a data race; the preflight runs strictly
    // before dispatch, on the GUI thread.
    QUrl fileUrl;
    QUrl vaultUrl;

signals:
    // 0.0–1.0 within the current phase. Cross-thread-safe to emit from run().
    void progress(double fraction);
    // Named phase transition ("loading"/"decoding"/… or a kind-specific one),
    // driving the phase label + overall-progress weighting.
    void phase(const QString &phase);
    // True the instant this task actually stops making progress because a
    // pause was requested (a REAL transition, not fired per poll — see
    // TaskContext::reportPaused()). A kind with capabilities().pausable=false
    // never emits this at all.
    void paused(bool isPaused);
    // Subclasses add their own TYPED result signals (e.g. a benchmark's
    // perModelResult / a grab's filesCopied) — the controller connects them
    // per-kind at enqueue. The base intentionally has none.
};

// Thrown by run() to fail a task with a message. The runner catches it and
// synthesizes the terminal report — run() never emits finished itself, which
// is how the queue can promise EXACTLY ONE terminal report per task even for a
// task cancelled before run() was ever called.
class TaskError {
public:
    explicit TaskError(QString message)
        : m_message(std::move(message))
    {
    }
    const QString &message() const { return m_message; }

private:
    QString m_message;
};

// Task* crosses threads via TaskRunner::taskFinished()'s queued connection —
// register it so QMetaType can marshal the argument (TaskRunner's constructor
// calls qRegisterMetaType<Task *>() at runtime; this macro is the compile-time
// half), same reason the old controller registered UsbGrabRule.
Q_DECLARE_METATYPE(Task *)
