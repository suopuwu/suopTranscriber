#pragma once

#include <QElapsedTimer>
#include <QFile>
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QThread>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>
#include <QMap>
#include <QVector>
#include <qqml.h>

#include "RollingAverage.h"
#include "Segment.h"

class QTimer;
class TranscriptionWorker;

// Writes segment lines to an open sidecar .md file (see appendSegmentLine's
// former doc comment / CLAUDE.md "vault layout"), merging a REPEATING CYCLE
// of consecutive segments (period 1..maxCycleLength — one repeated line, or
// a repeating block of 2, 3, ... lines) into just ONE copy of the cycle —
// whisper.cpp occasionally hallucinates the exact same sentence, OR the
// exact same short alternating pattern (e.g. "[MUSIC PLAYING]" / "[APPLAUSE]"
// back and forth), across many consecutive segments during a silence
// stretch, and per the user's request that must be merged IN THE FILE
// ITSELF, not just hidden by a display-layer dedup. Each line of a merged
// cycle keeps its own FIRST-occurrence timestamp and gets its own trailing
// " (×N)" repeat-count marker (see segmentLinePattern() in
// TranscriptionController.cpp for the parse side) — N is how many times
// THAT particular line occurred, which for every line in a repeating cycle
// is the same N (the cycle's repeat count). Two lines with identical text
// but DIFFERENT speakers are never merged.
//
// maxCycleLength bounds the search purely for performance — every new
// segment does an O(maxCycleLength^2) check against a small trailing
// window, never a full-file scan, so this stays cheap regardless of
// transcript length. Configurable (AppSettings.duplicateCycleWindow in QML
// -> TranscriptionController::transcribe()'s duplicateCycleWindow param),
// default 3 (checks period 1, 2, AND 3 — not just 3 alone).
//
// Durability is preserved despite the merge: each append() call leaves the
// file in a fully valid, flushed state — a duplicate cycle doesn't append
// new lines, it seeks back to the start of the EARLIEST line in the
// matched cycle (valid because writes are always sequential/at EOF, and
// every LineRecord's fileOffset is kept up to date across rewrites) and
// rewrites just that one copy in place with incremented counts, so a kill
// mid-run still leaves a correctly-formatted (if not-yet-fully-repeated)
// file. Declared here (rather than file-local in the .cpp) so tests can
// drive it directly with synthetic Segments, without needing a real
// whisper model.
class SegmentLineWriter {
public:
    explicit SegmentLineWriter(QFile &mdFile, int maxCycleLength = 3);

    // speakerLabel: empty = speaker detection wasn't on (or produced no
    // label) for this segment — no leading tag written, matching the
    // legacy no-detection format exactly. Non-empty = the real diarized
    // speaker label ("Speaker 1", "Speaker 2", ...; see DiarizationEngine).
    // Segments with empty (whitespace-only) text are silently skipped, same
    // as the old appendSegmentLine did.
    void append(const Segment &segment, const QString &speakerLabel);

private:
    // One physical line currently at the tail of the file, as it stands
    // right now (fileOffset/repeatCount reflect the CURRENT on-disk state,
    // which may already have been rewritten by an earlier merge).
    struct LineRecord {
        qint64 fileOffset = 0;
        QString speakerLabel;
        QString text;
        double t0 = 0.0;
        int repeatCount = 1;
    };

    void writeRecord(const LineRecord &record);
    void tryMergeCycle();

    QFile &m_mdFile;
    int m_maxCycleLength;
    // Bounded to the last 2*maxCycleLength entries (see tryMergeCycle()) —
    // that's all a period-<=maxCycleLength check can ever need to look at.
    QVector<LineRecord> m_tail;
};

// Orchestrates: ffmpeg decode -> WhisperCppEngine -> vault sidecar files.
// Owns a dedicated QThread; transcription (and its file IO) NEVER runs on
// the GUI thread (see CLAUDE.md). Whole-file transcription only for now —
// no VAD chunking / resume yet (DESIGN.md "Durable / resumable
// transcription" is a later milestone). Runs at most one job at a time but
// queues additional transcribe() calls (no duplicates by fileUrl) instead of
// dropping them, auto-starting the next queued job on completion.
//
// Job history is persistent for the app session: Done/Failed jobs stay in
// m_jobs (visible to QML via the jobs property) until the user dismisses
// them individually (dismiss()) or clears them in bulk (clearFinished()) —
// like a download manager, not a transient progress bar.
class TranscriptionController : public QObject {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(bool busy READ isBusy NOTIFY busyChanged)
    Q_PROPERTY(double progress READ progress NOTIFY progressChanged)
    // Item 6 (user correction, 2026-07-08): the PROGRESS BAR fill should
    // still sweep smoothly across a job's WHOLE lifetime (diarization then
    // transcription treated as one continuous 0..1, same blend `progress`
    // used before this wave) — only the PERCENTAGE TEXT should be phase-
    // relative (see `progress`'s own doc comment). QML binds the ProgressBar
    // `value` to this and the percentage text to `progress`.
    //
    // 2026-07-10: each active phase's SLICE of this 0..1 range is no longer
    // a fixed guess (used to be a hardcoded diarization=first-half/
    // transcription=second-half split, loading/decoding excluded entirely)
    // — it's this phase's share of the ROLLING AVERAGE of how long each
    // phase has actually taken over the last 10 completed jobs (see
    // RollingAverage, m_phaseTimings, recomputeOverallProgress()). "loading"
    // and "decoding" have no native progress hook, so their within-phase
    // fraction is synthesized from elapsed-time-in-phase vs. that phase's
    // own rolling average (see phaseRelativeSyntheticProgress()) rather than
    // going indeterminate — the bar now gives every phase real, historically-
    // proportioned width instead of just the two that happened to have a
    // progress callback.
    Q_PROPERTY(double overallProgress READ overallProgress NOTIFY overallProgressChanged)
    // "loading" (whisper model load — no native progress hook exists for
    // this; `progress` here is synthesized from elapsed-vs-historical-
    // average, see overallProgress's doc comment), "detecting-speakers-
    // segmenting" / "detecting-speakers-identifying" (diarization pass, REAL
    // progress via DiarizationEngine's sherpa-onnx-backed callback — split
    // into two sub-phases at fraction==0.30, the exact boundary where
    // sherpa-onnx's own internal work moves from finding WHERE speech is to
    // figuring out WHOSE voice it is, so the label switches right when the
    // real work does, not on a guess), "transcribing" (whisper's own
    // per-segment progress, as before), or empty when idle or for a
    // relabel/rename job (neither has phases worth naming). Each phase's own
    // progress() is phase-relative (its own 0->1); overallProgress blends
    // them into one continuous whole-job sweep (see its doc comment) rather
    // than each phase getting a full-width 0->1, which would look like the
    // bar finishes then restarts. Exists because "huge delay before it
    // starts transcribing" on bigger models was previously invisible — the
    // progress bar sat frozen at 0 during model load and (once diarization
    // landed) during the whole diarization pass too, indistinguishable from
    // a hang.
    Q_PROPERTY(QString currentPhase READ currentPhase NOTIFY currentPhaseChanged)
    Q_PROPERTY(QUrl currentFile READ currentFile NOTIFY currentFileChanged)
    // "transcribe"/"relabel"/"rename"/"merge" for whichever job is Active
    // right now (see jobs()' "kind" key — same four values), empty when
    // idle. Changes at exactly the same time as currentFile (both set
    // together in startJob()), so it reuses that signal rather than needing
    // its own.
    Q_PROPERTY(QString currentJobKind READ currentJobKind NOTIFY currentFileChanged)
    Q_PROPERTY(int tasksTotal READ tasksTotal NOTIFY queueChanged)
    Q_PROPERTY(int tasksCompleted READ tasksCompleted NOTIFY queueChanged)
    Q_PROPERTY(QVariantList queuedFileUrls READ queuedFileUrls NOTIFY queueChanged)
    Q_PROPERTY(QVariantList jobs READ jobs NOTIFY queueChanged)
    // Bumps once for each real new segment line written to the .md sidecar
    // during an in-progress transcription — throttled (see run()'s
    // liveUpdateTimer) so a burst of short segments still coalesces into at
    // most a few ticks per second. QML's "live updates as segments are
    // written" hooks (MainPanel.qml's transcriptBody/sentences bindings)
    // must depend on THIS, not progress: progress fires on whisper's every
    // internal decode-loop iteration (including decode retries/fallbacks),
    // far more often than real new content actually appears, and each live-
    // update tick re-reads+re-parses the whole file and rebuilds the whole
    // transcript view — tying it to progress was clogging the GUI thread.
    Q_PROPERTY(int liveUpdateTick READ liveUpdateTick NOTIFY liveUpdateTickChanged)
    // Wave N (task system): true while the Active job is paused. Only
    // "transcribe" and "download" jobs are truly pausable (see pauseActive()'s
    // doc comment) — false at all other times, including whenever nothing is
    // Active.
    Q_PROPERTY(bool activePaused READ isActivePaused NOTIFY activePausedChanged)
    // True from the moment a pause click during "loading" fires the
    // soft-cancel-as-restart (see JobStatus::Paused's doc comment) until it
    // actually resolves to Paused. QML uses this to disable the pause button
    // meanwhile — with no such guard, a second click in that window (nothing
    // visibly changes until the underlying blocking call returns) stacks a
    // real pauseActive() request on top of the still-pending cancel, which
    // permanently breaks pause for that job (see togglePauseActive()'s doc
    // comment for why).
    Q_PROPERTY(bool activeStopping READ isActiveStopping NOTIFY activeStoppingChanged)

public:
    // Paused: only reached from Active during "loading" — whisper.cpp's
    // in-process, unhookable model-load call, the one phase left with no
    // cooperative checkpoint at all (see pauseActive()'s doc comment;
    // "decoding" used to be in this bucket too, but ffmpeg runs as a real
    // subprocess — see AudioDecoder.h — so it gets a genuine pause instead).
    // Presented to the user as paused, not failed; resuming
    // restarts the job from scratch rather than continuing mid-flight.
    enum class JobStatus { Queued, Active, Paused, Done, Failed };
    Q_ENUM(JobStatus)

    // Fallback "average duration" (ms) a phase with zero recorded samples
    // yet uses — for BOTH recomputeOverallProgress()'s weight normalization
    // AND phaseRelativeSyntheticProgress()'s elapsed-vs-average synthetic
    // math ("loading"/"decoding" only). MUST be a plausible real duration,
    // not an arbitrary small constant: phaseRelativeSyntheticProgress()
    // divides elapsed-ms by it, so too small a fallback makes the bar (and,
    // before any RollingAverage has a single sample, the SYNTHESIZED
    // per-phase progress specifically) jump to ~full within milliseconds of
    // a phase starting — exactly the "whole bar filled at all times" bug
    // report (2026-07-10) from a value of 1.0 here on the very first-ever
    // run. User correction (2026-07-10): default to the SAME fixed 50/50
    // split between "detecting-speakers"/"transcribing" this feature
    // replaced, rather than a naive equal four-way split — "loading"/
    // "decoding" get their own modest, plausible default durations instead
    // of participating in that 50/50.
    //
    // Public (rather than tucked away private like the rest of this feature's
    // internals) specifically so tst_transcription_queue.cpp can assert this
    // directly, deterministically, with no wall-clock timing involved — an
    // integration test that instead tries to catch the SYMPTOM (elapsed time
    // during "loading" growing overallProgress too fast) turned out to be
    // unreliable on fast hardware, where a real model's "loading" phase can
    // complete before even one recomputeOverallProgress() timer tick fires,
    // masking the bug entirely regardless of whether it's fixed.
    static double defaultPhaseDurationMs(const QString &phase);
    // The four phase names m_phaseTimings/QSettings' "phaseTimings" group/
    // defaultPhaseDurationMs() all key on. "detecting-speakers" is included
    // even though it's conditional per-job (see m_activeSpeakerDetection) —
    // it's still one of the four things worth separately timing. Public for
    // the same testability reason as defaultPhaseDurationMs() above.
    static const QStringList kTimedPhases;

    explicit TranscriptionController(QObject *parent = nullptr);
    ~TranscriptionController() override;

    bool isBusy() const { return m_busy; }
    double progress() const { return m_progress; }
    // See the overallProgress Q_PROPERTY doc comment above.
    double overallProgress() const { return m_overallProgress; }
    QString currentPhase() const { return m_phase; }
    QUrl currentFile() const { return m_currentFile; }
    // See the liveUpdateTick Q_PROPERTY doc comment above.
    int liveUpdateTick() const { return m_liveUpdateTick; }
    // See the currentJobKind Q_PROPERTY doc comment above.
    QString currentJobKind() const;
    // See the activePaused Q_PROPERTY doc comment above.
    bool isActivePaused() const { return m_activePaused; }
    // See the activeStopping Q_PROPERTY doc comment above.
    bool isActiveStopping() const { return m_pausingActiveAsRestart; }
    // Wall-clock time the Active job has spent actually RUNNING, excluding
    // any time spent paused — this is what elapsed/duration display in the
    // UI, not finishedAt-startedAt (see pauseActive()'s doc comment for why
    // paused time shouldn't inflate it). 0 when nothing is Active. Not a
    // Q_PROPERTY: it changes continuously while a job runs and QML is
    // expected to re-poll it on its own Timer tick (see BottomBar.qml)
    // rather than have this object tick a timer of its own just to fire
    // NOTIFY signals nobody but a display needs.
    Q_INVOKABLE qint64 activeElapsedMs() const;
    // Linear-extrapolation estimate of when the Active job will finish, in
    // milliseconds from now (-1 if unknown — nothing Active, or too early in
    // the job for m_overallProgress to be above 0 yet). Deliberately uses the
    // BLENDED cross-phase fraction internally (m_overallProgress — see its
    // Q_PROPERTY doc comment), never the phase-relative one BottomBar's
    // percentage text shows (see currentPhase's doc comment for why those
    // two numbers are now different) — an ETA needs to know how much of the
    // WHOLE job is left, not just the current phase. Now meaningful even
    // during "loading"/"decoding" (their synthetic progress still advances
    // m_overallProgress — see phaseRelativeSyntheticProgress()), unlike
    // before those phases had rolling-average timing data to synthesize from.
    Q_INVOKABLE qint64 estimatedRemainingMs() const;
    // Derived from m_jobs: total entry count / Done+Failed count. Both only
    // grow until dismiss()/clearFinished() shrink the list (see class doc).
    int tasksTotal() const;
    int tasksCompleted() const;
    // fileUrls of jobs still Queued (not yet started) — used by QML to show
    // a "Queued…" state distinct from the currently-Active job.
    QVariantList queuedFileUrls() const;
    // Every job for this session (Queued+Active+Done+Failed), each as
    // {fileUrl, status ("queued"/"active"/"done"/"failed"), error,
    // kind ("transcribe"/"relabel"/"rename"/"merge")} — kind lets the UI
    // distinguish a full transcription from a "regenerate label" or a
    // speaker-rename/merge task in the same list (they all share this one
    // queue/worker thread).
    QVariantList jobs() const;

    // No-op if fileUrl already has a Queued or Active entry (no duplicate
    // in-flight-or-pending tasks). A Done/Failed entry for the same fileUrl
    // does NOT block re-queueing — deliberately allows re-transcribing after
    // dismissal or after changing settings. Starts immediately if idle,
    // otherwise queues. autoLabel opts into AI auto-labeling (see
    // LabelingEngine) — this does NOT run as a bonus step inside the
    // transcription job itself: a SEPARATE relabel()-equivalent job (same
    // "kind": "relabel" as a manual "regenerate label" click) is queued
    // immediately alongside the transcribe job, right here in this same
    // call — BOTH show up in the Tasks list together right away, not one
    // appended later once transcription finishes. It waits its turn behind
    // the transcribe job (only one job runs at a time) and, if that job
    // fails, still runs next and fails on its own (nothing to label yet) —
    // it is never silently skipped just because its upstream job failed, so
    // the Tasks list always reflects exactly what was requested up front.
    // restrictTagsToExisting (item 14 "auto-categorize" mode): when true,
    // the labeling pass is instructed to only pick tags already used
    // elsewhere in this vault (scanned fresh per job), leaving tags empty
    // rather than inventing a new one if none fit. Ignored when autoLabel
    // is false. tagReuseBias (item 15, Wave E): 0.0-1.0 soft nudge between
    // inventing new tags vs. reusing existing ones — only meaningful when
    // restrictTagsToExisting is false (that's already a hard constraint).
    Q_INVOKABLE void transcribe(const QUrl &fileUrl, const QUrl &vaultUrl, const QString &modelId,
                                 bool speakerDetection, bool autoLabel = false, bool restrictTagsToExisting = false,
                                 double tagReuseBias = 0.5);

    // Convenience batch wrapper: calls transcribe() for each url. Dedup is
    // entirely delegated to transcribe(); the caller decides which files are
    // actually untranscribed.
    Q_INVOKABLE void transcribeAll(const QVariantList &fileUrls, const QUrl &vaultUrl, const QString &modelId,
                                    bool speakerDetection, bool autoLabel = false,
                                    bool restrictTagsToExisting = false, double tagReuseBias = 0.5);

    // True if fileUrl currently has a Queued or Active entry.
    Q_INVOKABLE bool isQueued(const QUrl &fileUrl) const;

    // True if fileUrl's stored transcription (frontmatter) already used
    // modelId + speakerDetection — i.e. re-running would reproduce the same
    // result. Used by "transcribe all" (bulk) to skip files that are already
    // current, WITHOUT blocking an explicit single-file manual re-run
    // request (transcribe() itself never checks this — a user clicking
    // Transcribe on one file always re-runs it, regardless of whether
    // settings match). False for an untranscribed file (nothing to match).
    Q_INVOKABLE bool matchesExistingTranscription(const QUrl &fileUrl, const QString &modelId,
                                                   bool speakerDetection) const;

    // "Regenerate auto-title" (item 9): re-runs just the AI auto-labeling
    // pass on an already-transcribed file — reads the EXISTING transcript
    // text, does not re-transcribe audio. Queued through the same job/worker
    // infrastructure as transcribe() (dedup, persistent job list) since it's
    // real GPU work; a failure here IS the job's result (there's nothing
    // else for this job to accomplish). transcribe()'s autoLabel option
    // builds this same shape of job directly (see its doc comment) rather
    // than calling this method — a manual "regenerate label" click and an
    // automatic post-transcription label pass are the exact same job kind.
    Q_INVOKABLE void relabel(const QUrl &fileUrl, const QUrl &vaultUrl, bool restrictTagsToExisting = false,
                              double tagReuseBias = 0.5);

    // Bulk wrapper mirroring transcribeAll(): calls relabel() for each url.
    // Dedup is delegated to relabel(); the caller (Sidebar's "regenerate all
    // labels" ribbon icon) decides which files are actually transcribed.
    Q_INVOKABLE void relabelAll(const QVariantList &fileUrls, const QUrl &vaultUrl,
                                bool restrictTagsToExisting = false, double tagReuseBias = 0.5);

    // Reads the frontmatter (model/speakerDetection/transcribedAt/label/...)
    // out of a file's sibling .md, if one exists yet. Empty map if not
    // transcribed. If the requested model id wasn't recognized and
    // ModelManager fell back to its default, the map also carries
    // modelFallback (true) and modelRequested (the id that wasn't honored) —
    // model itself always names the file that actually ran. If speaker
    // detection was requested but the diarization models couldn't be
    // downloaded/loaded, speakerDetection reads false (the transcription
    // still ran, ASR-only — see DiarizationEngine) and the map carries
    // speakerDetectionError (why) — MainPanel surfaces this as a warning.
    Q_INVOKABLE QVariantMap readMetadata(const QUrl &fileUrl) const;

    // {id, label} pairs for the model dropdown (see ModelManager).
    Q_INVOKABLE QVariantList availableModels() const;

    // Removes one Done/Failed entry matching fileUrl from the job list. A
    // no-op if the only match is still Queued/Active, or if there's no
    // match at all.
    Q_INVOKABLE void dismiss(const QUrl &fileUrl);

    // Removes every Done/Failed entry from the job list; Queued/Active
    // entries are left alone.
    Q_INVOKABLE void clearFinished();

    // Wave N (task system). Pauses the Active job IN PLACE — a no-op unless
    // the Active job's kind is "transcribe" or "download" (the only two
    // kinds with a cooperative interruption point: whisper.cpp's
    // abort_callback, blocked on rather than told to abort — see
    // WhisperCppEngine.cpp's onAbortCheck; and the download job's own
    // byte-polling loop). "relabel"/"rename"/"merge" jobs run to completion
    // uninterrupted either way — this app never shows a pause/cancel control
    // as available for a job it can't actually honor, so QML should gate
    // showing the button on currentJobKind rather than call this
    // unconditionally. Pausing during diarization's sub-phases blocks
    // inside the SAME progress callback DiarizationEngine already calls
    // frequently — no native sherpa-onnx abort hook needed for a pause
    // (only for a genuine mid-diarization CANCEL, which this app does not
    // support — see cancelJob()'s doc comment).
    // ALSO a no-op during the "loading" phase specifically (user
    // correction, 2026-07-09): model loading is one blocking whisper.cpp
    // call with no callback hook at all (unlike decode's abort_callback) —
    // there is no interruption point to pause AT, so silently no-op rather
    // than queue a pause that only takes effect once the job reaches
    // transcribing/diarization later (which would look like the button did
    // nothing, then paused unexpectedly minutes on). QML offers
    // cancelActive() instead during this phase, since loading is fast
    // enough that restarting from scratch is cheap — see BottomBar.qml's
    // pauseButton.
    Q_INVOKABLE void pauseActive();
    // Resumes a paused Active job. No-op if nothing is paused.
    Q_INVOKABLE void resumeActive();
    // Bug report (2026-07-09): "pause during loading doesn't stop it, then
    // pause never works again for the rest of the session". Root cause was
    // a RACE, not a worker bug: BottomBar.qml's pause/play button used to
    // decide pauseActive() vs cancelActive() itself, by reading its OWN
    // (QML-side) copy of currentPhase — which is a queued cross-thread
    // signal one hop behind this object's own m_phase. A click landing
    // right as the job transitions from "loading" into diarization could
    // still see the STALE "loading" value and call cancelActive() instead
    // of the intended pauseActive(). Diarization has no mid-pass abort hook
    // (see pauseActive()'s doc comment on waitWhilePausedOrCancelled() — its
    // return value is deliberately ignored there), so that stray cancel
    // request just sits latched (m_cancelRequested stays true) while the
    // diarization pass runs to completion regardless — LOOKING exactly like
    // "it finished loading and moved on anyway". Worse, once cancelRequested
    // is latched, waitWhilePausedOrCancelled()'s loop condition
    // (pauseRequested && !cancelRequested) can never be true again for the
    // REST OF THAT JOB, so every subsequent pause click silently no-ops.
    //
    // Fix: this is now the ONLY entry point BottomBar.qml's pause/play
    // button calls. It reads m_phase/m_activePaused directly — this
    // object's own authoritative copy, never QML's — so the pause-vs-cancel
    // decision can't race a phase transition it hasn't heard about yet.
    Q_INVOKABLE void togglePauseActive();
    // Cancels whichever job is currently Active, without the caller needing
    // to know its id — convenience mirroring pauseActive()/resumeActive()
    // (see cancelJob()'s doc comment for the immediate-vs-next-checkpoint
    // nuance that still applies). Added specifically for the "loading"
    // phase's cancel-instead-of-pause button (see pauseActive()'s doc
    // comment and BottomBar.qml).
    Q_INVOKABLE void cancelActive();
    // Cancels one job by its stable jobs()-list "id" (NOT fileUrl — a
    // fileUrl can simultaneously name an old Done/Failed history entry and a
    // fresh Queued one after a re-transcribe, so id is the only safe key for
    // a targeted mutation). A Queued job is simply removed — nothing has
    // started, so there is nothing to interrupt. Cancelling the ACTIVE job
    // only takes effect immediately for "transcribe"/"download" kinds (same
    // interruption points pauseActive() uses); for a "relabel"/"rename"/
    // "merge" kind Active job, or if the Active job is stuck inside
    // diarization specifically (no mid-pass abort — see pauseActive()'s doc
    // comment), the cancel is recorded but only takes effect at the next
    // interruption point the job reaches (documented limitation, not a
    // silent gap: the job is marked Failed with error "Cancelled" the
    // moment it actually stops, which may be after diarization's current
    // sub-phase finishes rather than instantly). A no-op if id doesn't match
    // any current Queued/Active entry.
    Q_INVOKABLE void cancelJob(int id);
    // Cancels every Queued job, then cancels the Active job exactly like
    // cancelJob() (same immediate-vs-next-checkpoint caveat). Done/Failed
    // history entries are untouched (use clearFinished() for those).
    Q_INVOKABLE void cancelAll();
    // Moves a Queued job to newIndex within the Queued-only ordering (i.e.
    // newIndex counts only among currently-Queued entries, not the full
    // jobs() list which also has Done/Failed/Active rows interleaved) — lets
    // the Tasks popup implement drag-to-reorder without the QML side needing
    // to know how Queued/Active/Done/Failed are interleaved in the
    // underlying list. No-op if id doesn't name a currently-Queued entry.
    // Active/Done/Failed entries are never reorderable (an Active job's
    // position is meaningless to move; history is append-only).
    Q_INVOKABLE void moveQueuedJob(int id, int newIndex);

    // Sets/replaces the `label:` frontmatter key on an already-transcribed
    // file's sidecar .md, without disturbing the transcript body below it.
    // Fails (returns false, *errorOut set) if the file hasn't been
    // transcribed yet or the frontmatter block is malformed. Runs
    // synchronously on the calling (GUI) thread — single small file
    // read+write, no model/engine involvement.
    Q_INVOKABLE bool setLabel(const QUrl &fileUrl, const QString &label, QString *errorOut = nullptr) const;

    // Rewrite (2026-07-07): a speaker's name is now baked directly into
    // every "[oldName]" segment-line tag in the transcript BODY itself —
    // there is no more separate frontmatter name-lookup layer, since the
    // vault's .md file is meant to be human-readable standalone (CLAUDE.md);
    // a raw file that still showed "[Speaker 1]" everywhere after a rename,
    // with the real name hidden in YAML frontmatter, wasn't. oldName must be
    // the speaker's CURRENT literal tag text — the same string
    // parseSentences()' "speaker" field already returns (e.g. "Speaker 1",
    // or a previously-set custom name once already renamed once).
    //
    // Runs asynchronously, queued through the SAME job pipeline as
    // transcribe()/relabel() above (a full-body rewrite is real file IO) —
    // this call itself returns immediately, and the caller is expected to
    // update its own display optimistically and reconcile once
    // speakerRenamed() fires (see MainPanel.qml's pendingSpeakerRenames).
    // Being a real job means it shows up in jobs()/tasksTotal/tasksCompleted
    // like any other task, queues behind an active job for the SAME file
    // instead of racing it, and runs concurrently-in-effect with (queued
    // after, not blocked by) jobs for OTHER files. Rejects (speakerRenamed's
    // ok=false) if newName (trimmed, case-insensitively) already names a
    // DIFFERENT speaker present in the file, if oldName isn't actually
    // present, or if fileUrl already has a Queued/Active job (same dedup
    // rule as transcribe(), reported back via speakerRenamed instead of
    // silently dropped, since a caller here is actively waiting for a
    // definitive result).
    Q_INVOKABLE void renameSpeaker(const QUrl &fileUrl, const QString &oldName, const QString &newName);

    // Merges sourceName into targetName: every "[sourceName]" body tag
    // becomes "[targetName]", and sourceName drops out of distinctSpeakerIds
    // entirely (unlike renameSpeaker() above, colliding with an existing
    // name is the whole point here — see mergeSpeakersInFile()'s doc
    // comment in the .cpp). Queued through the same job pipeline as
    // renameSpeaker(), with the same async/dedup/optimistic-UI contract:
    // runs on the background thread, rejects (speakersMerged's ok=false) if
    // fileUrl already has a Queued/Active job, and the caller is expected to
    // update its own display optimistically and reconcile once
    // speakersMerged() fires.
    Q_INVOKABLE void mergeSpeakers(const QUrl &fileUrl, const QString &sourceName, const QString &targetName);

    // Everything after the frontmatter's closing "---" line (i.e. the
    // segment text), for main-panel search. Empty string if untranscribed
    // or unreadable.
    Q_INVOKABLE QString readTranscriptBody(const QUrl &fileUrl) const;

    // Parses readTranscriptBody()'s "[HH:MM:SS.mmm] [tag] text" lines (see
    // SegmentLineWriter/formatTimestamp) into a list of {t0: seconds as
    // double, speaker: QString (empty = no speaker detection), text:
    // QString, start, length: int, repeatCount: int}, in file order (which
    // is also time order — segments are appended as whisper produces them).
    // "speaker" is the literal bracketed tag text as stored in the file —
    // "Speaker N" by default, or whatever a user has renamed it to (see
    // renameSpeaker() above), never translated through a separate lookup.
    // No "t1": it's never stored (always identical to the next segment's
    // t0) and nothing consumes it. start/length are the character range of
    // text within THIS SAME readTranscriptBody() string, so a caller can
    // span-highlight the active sentence without re-deriving offsets.
    // repeatCount is 1 for a normal line, or N when SegmentLineWriter merged
    // N consecutive identical (same speaker, same text) whisper segments
    // into this one line with a trailing " (×N)" marker — the merge happens
    // at write time, in the file itself, not just in this parsed view. For
    // sentence-level highlighting synced to playback (see DESIGN.md /
    // CLAUDE.md "sentence-level only, binary-search on positionChanged").
    Q_INVOKABLE QVariantList parseSentences(const QUrl &fileUrl) const;

    // Binary-searches sentences (as returned by parseSentences(), sorted by
    // t0) for the one active at positionSeconds. Returns the index of the
    // last sentence whose t0 <= positionSeconds — i.e. "sticky": a position
    // in the silence gap between sentence i's t1 and sentence i+1's t0 still
    // reports i, so the highlight doesn't blank out between segments.
    // Returns -1 if positionSeconds is before the first sentence's t0 (or
    // the list is empty).
    Q_INVOKABLE int activeSentenceIndex(const QVariantList &sentences, double positionSeconds) const;

    // Wave N (task system, item 1): loads `<vaultUrl>/.transcriber/tasks.json`
    // (written by this same class — see persistQueue()) if present, and
    // re-enqueues every entry as a fresh Queued job (see
    // enqueueReplayedJob()). Also remembers vaultUrl as the destination for
    // subsequent persistQueue() calls (every queueChanged() from here on
    // saves back to this vault until the next loadPersistedQueue() call,
    // i.e. the next vault open). Meant to be called once per vault open
    // (Main.qml's onVaultPathChanged, alongside the existing
    // AppSettings.loadFromVault call). A job that was Active (mid-
    // transcription) when the app last closed is persisted and reloaded as
    // Queued, NOT resumed from where it left off — no true mid-file resume
    // exists yet (see CLAUDE.md / DESIGN.md "Durable / resumable
    // transcription").
    //
    // User correction (2026-07-09): restored jobs are deliberately NOT
    // auto-started, unlike a live transcribe()/relabel() call — they just
    // sit Queued (visible in jobs()/the Tasks popup, reorderable/
    // cancellable like any other Queued entry) until the user explicitly
    // resumes via startNext() (QML's play button). "Saved between
    // sessions", not "auto-resumed".
    Q_INVOKABLE void loadPersistedQueue(const QUrl &vaultUrl);

    // Wave N (task system, user correction 2026-07-09): starts the
    // front-most Queued job — the "play" button QML shows in place of the
    // pause button whenever nothing is Active but the queue isn't empty
    // (most commonly right after loadPersistedQueue() restored one).
    // No-op if something is already Active, or the queue has no Queued
    // entry at all.
    Q_INVOKABLE void startNext();

    // Bug report (2026-07-09): closing the GUI window must never leave a
    // transcription silently running in the background. The natural fix —
    // cancel the Active job, then wait for the worker thread in
    // ~TranscriptionController() — is real and correct, but its RESPONSE
    // TIME is bounded by however long the vendored sherpa-onnx diarization
    // code takes to reach its next progress-callback checkpoint, which
    // batches 32 windows/embeddings per GPU call before checking in at all
    // (see offline-speaker-diarization-pyannote-impl.h's
    // kSegmentationBatchSize/kEmbeddingBatchSize) — measured up to ~9s in
    // practice, long enough to look exactly like "still running in the
    // background".
    //
    // This sidesteps that entirely: rather than waiting for native code to
    // reach a checkpoint, it ends the OS process immediately. Provably safe
    // per this app's own durability design — CLAUDE.md/SegmentLineWriter's
    // doc comment: every segment append() already leaves the .md file fully
    // valid and flushed, BY DESIGN, specifically so a kill mid-run (this app
    // has never guaranteed anything more graceful than that) never
    // corrupts it. The one accepted risk is losing a vault settings change
    // made in the last ~250ms before closing (AppSettings.qml's
    // saveToVault() is debounced that long) — the exact same risk this
    // app's Ctrl+C shutdown path already carries today, just extended to
    // the window's close button too, not a new category of risk.
    // Deliberately NOT a graceful QCoreApplication::quit() — that still
    // waits on the same slow-to-interrupt native call underneath.
    Q_INVOKABLE void terminateApplicationImmediately() const;

    // Item 17: de-duplicated, sorted list of every tag already used anywhere
    // in the vault — feeds the sidebar's tag-input autocomplete. Same scan
    // LabelingEngine's "restrict to existing tags" mode already uses
    // internally (collectVaultTags, TranscriptionController.cpp); exposed
    // here so QML doesn't need its own duplicate frontmatter-scanning logic.
    Q_INVOKABLE QStringList existingVaultTags(const QUrl &vaultUrl) const;

signals:
    void busyChanged();
    void progressChanged();
    void overallProgressChanged();
    void currentPhaseChanged();
    void currentFileChanged();
    void queueChanged();
    void liveUpdateTickChanged();
    void activePausedChanged();
    void activeStoppingChanged();
    void finished(const QUrl &fileUrl, bool ok, const QString &error);
    // Fired when a renameSpeaker() call's background rewrite completes — IN
    // ADDITION to the generic finished() above (a rename is queued through
    // the SAME job queue as transcribe/relabel, see renameSpeaker()'s doc
    // comment, so it participates in busy/progress/tasksTotal/jobs() too).
    // oldName is echoed back (not just fileUrl) so a caller tracking
    // multiple speakers/optimistic renames at once (see MainPanel.qml's
    // pendingSpeakerRenames) knows exactly which one this result belongs to.
    void speakerRenamed(const QUrl &fileUrl, const QString &oldName, bool ok, const QString &error);
    // Fired when a mergeSpeakers() call's background rewrite completes — see
    // speakerRenamed()'s doc comment above for the shape/rationale, mirrored
    // here with both names (sourceName is echoed back so a caller tracking
    // several in-flight merges knows exactly which one this result belongs
    // to, targetName rounds out the pair for symmetry/debugging).
    void speakersMerged(const QUrl &fileUrl, const QString &sourceName, const QString &targetName, bool ok,
                         const QString &error);

private:
    // Wave N (task system, item 5): which model file(s) a download job is
    // responsible for fetching — orthogonal to the four existing job-kind
    // flags below (a download job sets NONE of those; see jobKind()).
    enum class DownloadTarget { None, WhisperModel, LabelModel, DiarizationModels };

    struct Job {
        // Stable across the job's entire lifetime (Queued through Done/
        // Failed) and never reused — the only safe key for cancelJob()/
        // moveQueuedJob() (see their doc comments for why fileUrl alone
        // isn't safe: re-queueing after Done/Failed can leave two entries
        // for the same fileUrl at once).
        int id = 0;
        QUrl fileUrl;
        QUrl vaultUrl;
        QString modelId;
        bool speakerDetection = false;
        bool autoLabel = false;
        bool relabelOnly = false;
        bool renameSpeakerOnly = false;
        QString oldSpeakerName;
        QString newSpeakerName;
        bool mergeSpeakersOnly = false;
        QString sourceSpeakerName;
        QString targetSpeakerName;
        bool restrictTagsToExisting = false;
        double tagReuseBias = 0.5;
        DownloadTarget downloadTarget = DownloadTarget::None;
        JobStatus status = JobStatus::Queued;
        QString error;
        // -1 until the job reaches Done/Failed (see persistQueue()'s doc
        // comment for why this is wall-clock-active time, not
        // finishedAt-startedAt) — shown in the Tasks popup as "took Ns".
        qint64 durationMs = -1;
    };

    // Shared by jobs()' "kind" key and currentJobKind() — the four flags
    // above (plus downloadTarget != None) are mutually exclusive by
    // construction (each enqueuing function only ever sets one).
    static QString jobKind(const Job &job);

    void startJob(int index);
    void onWorkerFinished(bool ok, const QString &error);
    // Wave N (task system, item 5): pushes a Queued "download" job for
    // whichever model file `target` names, UNLESS one is already
    // Queued/Active or the file is already on disk (checked fresh here, not
    // cached — models can be deleted out from under the app between calls).
    // Called right before transcribe()/relabel() queues its own job, so the
    // download (if needed) lands just ahead of it in m_jobs and therefore
    // runs first. Returns the new job's id, or -1 if no download was needed.
    int maybeQueueDownloadJob(DownloadTarget target, const QUrl &vaultUrl, const QString &modelId);
    // True if ANY Queued/Active job already targets this exact download
    // (same vaultUrl + target + modelId) — maybeQueueDownloadJob()'s dedup
    // check.
    bool isDownloadQueued(DownloadTarget target, const QUrl &vaultUrl, const QString &modelId) const;

    // Wave N (task system, item 1): serializes every Queued job (an Active
    // job is written as Queued — see loadPersistedQueue()'s doc comment for
    // why) to m_vaultUrl's tasks.json. Connected to queueChanged() in the
    // constructor, so this fires automatically after every mutation; a
    // no-op if m_vaultUrl is empty (no vault open yet/ever this session).
    void persistQueue() const;
    static QString tasksJsonPath(const QUrl &vaultUrl);
    // Wave N (task system, item 1) bugfix: reconstructs and pushes ONE
    // persisted entry as a fresh Queued job, mirroring the relevant public
    // enqueue function's (transcribe()/relabel()/renameSpeaker()/
    // mergeSpeakers()) job-building logic but DELIBERATELY skipping their
    // isQueued(fileUrl) dedup guard. loadPersistedQueue() previously called
    // those public functions directly, which broke exactly the common case
    // this feature exists for: a persisted "transcribe" entry followed by a
    // persisted "relabel" entry for the SAME fileUrl (from an autoLabel
    // job) — by the time the relabel entry replayed, the transcribe entry
    // was already Queued/Active for that fileUrl, so relabel()'s own dedup
    // check silently rejected it, and the label job never came back after a
    // restart. Replaying via direct Job construction has no such conflict:
    // every persisted entry is BY DEFINITION a legitimate distinct job that
    // already passed dedup once, when it was first queued live.
    void enqueueReplayedJob(const QString &kind, const QJsonObject &obj);

    // 2026-07-10 (rolling-average phase weighting): records that `phase` (one
    // of kTimedPhases) just took `durationMs` on the job that just finished
    // it, then recomputes m_overallProgress so the CURRENT phase's own slice
    // reflects the freshly-updated average right away rather than waiting
    // for the next tick. Connected to TranscriptionWorker::phaseDurationMs.
    void recordPhaseDuration(const QString &phase, qint64 durationMs);
    // Rebuilds m_overallProgress from scratch: m_phase's active-phase set
    // (loading/decoding/[detecting-speakers]/transcribing — detecting-
    // speakers only counted when m_activeSpeakerDetection is set), each
    // phase's rolling-average share of that set (m_phaseTimings, falling
    // back to defaultPhaseDurationMs() per phase with no samples yet — a
    // fixed 50/50 detecting-speakers/transcribing split by default, see its
    // doc comment), and phaseRelativeSyntheticProgress() for the current
    // phase's own within-phase fraction. Called on every phase()
    // transition, every worker progress() tick, AND on m_indeterminatePhaseTimer's
    // tick (the only way "loading"/"decoding" — which have no real progress
    // callback — ever advance mid-phase). A no-op (leaves m_overallProgress
    // untouched) while m_phase is empty or "downloading", neither of which
    // this weighting scheme covers (downloadProgress's handler sets
    // m_overallProgress directly instead — see its doc comment).
    void recomputeOverallProgress();
    // This phase's OWN 0..1 fraction — m_progress directly for phases with a
    // real progress callback (detecting-speakers/transcribing), or, for
    // "loading"/"decoding" (no callback exists — see run()'s doc comments),
    // elapsed-time-in-this-phase (activeElapsedMs() - m_phaseStartMs) divided
    // by that phase's own rolling average, clamped to 0.95 so a slower-than-
    // average run never LOOKS finished before the real phase transition
    // actually happens.
    double phaseRelativeSyntheticProgress() const;
    // Populates m_phaseTimings from QSettings' "phaseTimings" group (one key
    // per kTimedPhases entry, a QVariantList of up to RollingAverage::kCapacity
    // raw millisecond samples) — called once from the constructor. App-scoped
    // (not per-vault): how long a phase takes is a property of this machine
    // + whichever model was last used, not something that should differ by
    // vault.
    void loadPhaseTimings();
    // Writes JUST `phase`'s current sample list back to QSettings — called
    // right after recordPhaseDuration() adds a new sample. Per-phase (not a
    // single bulk save) since jobs typically complete one phase at a time,
    // not all four at once.
    void savePhaseTiming(const QString &phase);

    QThread m_thread;
    TranscriptionWorker *m_worker = nullptr;
    bool m_busy = false;
    double m_progress = 0.0;
    // Wave N (task system, item 6): the SAME 0..1 fraction run() used to
    // blend across diarization+transcription phases (see currentPhase's doc
    // comment for why that blending moved OUT of the displayed m_progress) —
    // kept purely for estimatedRemainingMs()'s ETA math, never shown
    // directly. Computed by recomputeOverallProgress() (see its doc
    // comment), not emitted by the worker anymore, EXCEPT for a "downloading"
    // job (see the downloadProgress connection in the constructor).
    double m_overallProgress = 0.0;
    // Rolling (last RollingAverage::kCapacity samples) average duration of
    // each named phase, keyed by the same strings TranscriptionWorker::phase
    // emits ("loading"/"decoding"/"detecting-speakers"/"transcribing") — see
    // recomputeOverallProgress()'s doc comment for how these become the
    // progress bar's per-phase widths. Loaded once in the constructor
    // (loadPhaseTimings()), updated+persisted per-phase as jobs complete each
    // phase (recordPhaseDuration()/savePhaseTiming()).
    QMap<QString, RollingAverage> m_phaseTimings;
    // Whether the Active job requested speaker detection — set in startJob()
    // from job.speakerDetection so recomputeOverallProgress() knows whether
    // "detecting-speakers" belongs in the active-phase set before (or even
    // absent) the worker's first phase() signal.
    bool m_activeSpeakerDetection = false;
    // activeElapsedMs() at the moment m_phase last changed — see
    // phaseRelativeSyntheticProgress()'s doc comment.
    qint64 m_phaseStartMs = 0;
    // Ticks only during "loading"/"decoding" (see recomputeOverallProgress()'s
    // doc comment) — those two have no real progress callback of their own,
    // so without this m_overallProgress would sit frozen for however long
    // they take.
    QTimer *m_indeterminatePhaseTimer = nullptr;
    QString m_phase;
    QUrl m_currentFile;
    int m_liveUpdateTick = 0;
    QVector<Job> m_jobs;
    int m_activeIndex = -1;
    int m_nextJobId = 1;
    // Set by loadPersistedQueue() (once per vault open) — where persistQueue()
    // writes back to. Empty (no persistence) until then.
    QUrl m_vaultUrl;
    // See isActivePaused()'s doc comment.
    bool m_activePaused = false;
    // Set right before killing the Active job specifically to soft-pause it
    // (togglePauseActive() during "loading" — see JobStatus::Paused's
    // doc comment), so onWorkerFinished() can tell that outcome apart from a
    // real cancel/failure of the SAME underlying "requestCancel() then
    // finished(false, "Cancelled")" mechanism. Cleared the instant it's read.
    bool m_pausingActiveAsRestart = false;
    // Wave N (task system, item 4/7): wall-clock ms the Active job has
    // actually been RUNNING, excluding paused stretches — see
    // activeElapsedMs()'s doc comment. m_activeSegmentTimer covers the
    // current (unpaused) running stretch since job start or last resume;
    // m_activeAccumulatedMs holds everything before that.
    QElapsedTimer m_activeSegmentTimer;
    qint64 m_activeAccumulatedMs = 0;
};
