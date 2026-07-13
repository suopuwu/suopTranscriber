#pragma once

#include <QDateTime>
#include <QElapsedTimer>
#include <QFile>
#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QString>
#include <QThread>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>
#include <QMap>
#include <QMutex>
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

// Feature 3a ("generate chapters"): one resolved chapter boundary, ready to
// splice into a transcript body — t0 already resolved from whatever segment
// index the LLM proposed (see LabelingEngine::generateChapters()'s doc
// comment for why the raw index is never trusted as a timestamp directly;
// that resolution happens in TranscriptionController.cpp's
// TranscriptionWorker::generateChaptersJob(), against its own freshly-parsed
// segment list, before this struct is ever built).
struct ResolvedChapter {
    double t0 = 0.0;
    QString title;
};

// Strips every existing "## [timestamp] title" chapter marker line out of
// `body` (regeneration must replace the old marker set wholesale, never
// accumulate duplicates across repeated "generate chapters" runs — same
// unconditional-overwrite policy as Feature 2's prelude), then inserts each
// of `chapters`' marker lines immediately before the segment line whose own
// leading "[timestamp]" exactly matches that chapter's t0 (formatted via the
// same formatTimestamp() every segment line already uses, so the match is
// exact by construction whenever t0 came from parsing this same file — see
// formatSegmentLine() in the .cpp). A chapter whose t0 doesn't match any
// segment line is silently skipped rather than corrupting the body.
//
// Collision-free with segmentLinePattern() by construction (not just lower
// risk, unlike the prelude's numbered-list false-positive — see
// stripPreludeIfPresent()'s doc comment): that pattern is anchored on a
// literal "^\[", and a line starting with "## " can never match it.
//
// Chapters are inserted internally in DESCENDING offset order (the
// last-in-the-file chapter first) — inserting by ascending string offset
// would invalidate every not-yet-inserted chapter's offset as each earlier
// insertion shifts the string. Callers don't need to pre-sort `chapters`.
//
// Declared here (rather than file-local in the .cpp, mirroring
// SegmentLineWriter above) so tests can drive the splicing logic directly
// with synthetic values, without needing a real label model.
QString spliceBodyChapters(const QString &body, const QList<ResolvedChapter> &chapters);

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
    // "transcribe"/"relabel"/"rename"/"merge"/"chapters" for whichever job is
    // Active right now (see jobs()' "kind" key — same values), empty when
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
    // Item O35: "CUDA"/"Vulkan"/"CPU" — whichever backend whisper.cpp picked
    // at the most recent model load (see WhisperCppEngine::activeBackendName()'s
    // doc comment). Empty until the first model finishes loading this
    // session; QML is expected to hide its indicator until then rather than
    // show a blank/placeholder value. Never populated for a relabel/rename/
    // merge/download job — none of those touch WhisperCppEngine.
    Q_PROPERTY(QString activeBackendName READ activeBackendName NOTIFY activeBackendNameChanged)

public:
    // Paused: only reached from Active during "loading" — whisper.cpp's
    // in-process, unhookable model-load call, the one phase left with no
    // cooperative checkpoint at all (see pauseActive()'s doc comment;
    // "decoding" used to be in this bucket too, but ffmpeg runs as a real
    // subprocess — see AudioDecoder.h — so it gets a genuine pause instead).
    // Presented to the user as paused, not failed; resuming
    // restarts the job from scratch rather than continuing mid-flight.
    //
    // None (item P6): a pure sentinel, never actually assigned to a real
    // Job::status — it's what jobStatusForPath() returns for a path with no
    // job at all, so QML can compare against a named value instead of a
    // magic -1. Explicit -1 underlying value so it can never collide with a
    // real status if ever compared as a raw int.
    enum class JobStatus { None = -1, Queued, Active, Paused, Done, Failed };
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
    // See the activeBackendName Q_PROPERTY doc comment above.
    QString activeBackendName() const { return m_activeBackendName; }
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
    // kind ("transcribe"/"relabel"/"rename"/"merge"/"chapters")} — kind lets
    // the UI distinguish a full transcription from a "regenerate label" or a
    // speaker-rename/merge task in the same list (they all share this one
    // queue/worker thread).
    QVariantList jobs() const;

    // Item P6: lets the sidebar's file tree query a single path's job status
    // directly, without walking the whole jobs() list itself. path must be an
    // absolute local file path (QUrl::toLocalFile()), matching what m_jobs'
    // Job::fileUrl resolves to — same convention every other path-keyed
    // lookup in this class uses. Returns JobStatus::None (-1) if path has no
    // Queued/Active/Paused/Done/Failed entry at all. When multiple jobs exist
    // for the same path (e.g. a Done history entry plus a freshly re-queued
    // run), the more "in progress" one wins — see rebuildJobStatusIndex()'s
    // doc comment for the exact priority order.
    Q_INVOKABLE int jobStatusForPath(const QString &path) const;

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
    // wordiness (Feature 1): 0.0-1.0 soft nudge on how verbose the generated
    // label and prelude are, independent of the tag settings above.
    Q_INVOKABLE void transcribe(const QUrl &fileUrl, const QUrl &vaultUrl, const QString &modelId,
                                 bool speakerDetection, bool autoLabel = false, bool restrictTagsToExisting = false,
                                 double tagReuseBias = 0.5, double wordiness = 0.5);

    // Convenience batch wrapper: calls transcribe() for each url. Dedup is
    // entirely delegated to transcribe(); the caller decides which files are
    // actually untranscribed.
    Q_INVOKABLE void transcribeAll(const QVariantList &fileUrls, const QUrl &vaultUrl, const QString &modelId,
                                    bool speakerDetection, bool autoLabel = false,
                                    bool restrictTagsToExisting = false, double tagReuseBias = 0.5,
                                    double wordiness = 0.5);

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
                              double tagReuseBias = 0.5, double wordiness = 0.5);

    // Bulk wrapper mirroring transcribeAll(): calls relabel() for each url.
    // Dedup is delegated to relabel(); the caller (Sidebar's "regenerate all
    // labels" ribbon icon) decides which files are actually transcribed.
    Q_INVOKABLE void relabelAll(const QVariantList &fileUrls, const QUrl &vaultUrl,
                                bool restrictTagsToExisting = false, double tagReuseBias = 0.5,
                                double wordiness = 0.5);

    // Feature 3a ("generate chapters"): a second, SEPARATE LLM pass from
    // relabel() above — sharing the same resident label model/context (see
    // LabelingEngine::generateChapters()'s doc comment), but a deliberately
    // separate, explicit action rather than bundled into relabel(): this is
    // a materially heavier/slower call and shouldn't silently ride along on
    // every "regenerate label" click. Reads the file's ALREADY-parsed
    // segments, proposes chapter boundaries+titles, and splices them into
    // the body as "## [timestamp] title" marker lines (see
    // spliceBodyChapters()'s doc comment) immediately before the segment
    // line each chapter starts at. Queued through the same job pipeline as
    // relabel() — identical dedup-if-already-queued behavior (see its doc
    // comment) and job-list visibility. Regenerating (calling this again on
    // an already-chaptered file) strips the old marker set first — never
    // accumulates duplicates across repeated runs.
    Q_INVOKABLE void generateChapters(const QUrl &fileUrl, const QUrl &vaultUrl);

    // Bulk wrapper mirroring relabelAll(): calls generateChapters() for each
    // url. Dedup is entirely delegated to generateChapters().
    Q_INVOKABLE void generateChaptersAll(const QVariantList &fileUrls, const QUrl &vaultUrl);

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
    // Parses every "key: value" frontmatter line generically (not just a
    // fixed allowlist) — any key LabelingEngine or a future writer adds
    // (e.g. "tags") is automatically present here with no extra code.
    // Note: the prelude (Feature 2) lives in the BODY, not frontmatter (it
    // needs real newlines) — use readPrelude() for that, not this map.
    // Item O19: cached (see m_frontmatterCache) behind a cheap
    // file-size+mtime check, since this is called on every liveUpdateTick
    // (transitively, via parseSentences()) while a transcription is
    // actively running on the currently-open file.
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

    // Item P15: sets/replaces the `tags:` frontmatter key, same reopen/
    // rewrite shape as setLabel() above. tags is joined with ", " to match
    // the exact comma-separated format collectVaultTags()/readMetadata()
    // already parse (auto-label writes this same shape via
    // writeLabelingFields()'s spliceOrAppend) — individual tags must not
    // themselves contain commas.
    Q_INVOKABLE bool setTags(const QUrl &fileUrl, const QStringList &tags, QString *errorOut = nullptr) const;

    // Feature 2: the multi-paragraph, editable "prelude" (upgrades the old
    // read-only single-line "summary") — stored in the .md BODY immediately
    // after frontmatter, terminated by the "<!-- prelude end -->" sentinel
    // (see spliceBodyPrelude()'s doc comment in the .cpp for why an explicit
    // marker rather than implicit boundary detection). Returns everything
    // before that marker, trimmed, or an empty string if the marker isn't
    // present (untranscribed file, or one predating this feature).
    Q_INVOKABLE QString readPrelude(const QUrl &fileUrl) const;

    // Manual-edit counterpart to setLabel() above, same reopen/rewrite shape
    // (frontmatter preserved byte-for-byte) — replaces just the prelude
    // region of the body via the same splice helper writeLabelingFields()
    // uses for auto-regeneration, so a later relabel() and a manual edit
    // here produce identically-formatted output. Per Feature 2's
    // regeneration semantics, a later auto-relabel unconditionally
    // overwrites whatever was set here — same precedent as a manual title
    // rename (setLabel()) already being clobbered by relabel() today.
    Q_INVOKABLE bool setPrelude(const QUrl &fileUrl, const QString &prelude, QString *errorOut = nullptr) const;

    // Feature 3a: the read-side counterpart to generateChapters() above —
    // scans the body for "## [timestamp] title" marker lines (see
    // spliceBodyChapters()'s doc comment) and returns them as
    // {t0: seconds as double, title: QString} maps, in file order (== time
    // order, since a chapter marker is always inserted immediately before
    // the segment line it introduces). Empty list if the file has no
    // chapters yet (untranscribed, or generateChapters() never run).
    Q_INVOKABLE QVariantList readChapters(const QUrl &fileUrl) const;

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
    // or unreadable. Item O19: cached (see m_bodyCache) behind a cheap
    // file-size+mtime check, since this is called on every liveUpdateTick
    // while a transcription is actively running on the currently-open file.
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
    // Item O19: cached (see m_sentenceCache) — an append-only live
    // transcription only regex-parses its NEW trailing bytes per call
    // instead of re-parsing the whole (ever-growing) body from scratch.
    Q_INVOKABLE QVariantList parseSentences(const QUrl &fileUrl) const;

    // Item P23: cold-opening a large already-transcribed file used to block
    // the GUI thread for the entire readTranscriptBody()+parseSentences()
    // pass (a single regex sweep over the whole body, cost scaling with
    // file size) — this runs that SAME pair of (cached) calls on a
    // throwaway background QThread instead, and emits transcriptLoaded()
    // with the result once done. Thread-safe with respect to the GUI
    // thread's own synchronous calls to readTranscriptBody()/parseSentences()
    // (e.g. the existing liveUpdateTick-driven polling of whatever file is
    // ACTIVELY transcribing) via m_transcriptCacheMutex — see its doc
    // comment. Fire-and-forget: callers (QML) are expected to track which
    // fileUrl they're waiting on themselves and ignore a transcriptLoaded()
    // whose fileUrl no longer matches the file currently open (the user may
    // have switched files again before this one's background pass finished).
    Q_INVOKABLE void loadTranscriptAsync(const QUrl &fileUrl);

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

    // Split-recording combine: matches vault audio filenames against
    // user-authored regex rules (AppSettings.combineRules) to find groups of
    // files that are really one recording split by the recorder — see
    // findCombineGroups() in the .cpp for the full matching/grouping
    // semantics. Pure over its inputs (only touches disk via QFileInfo stats
    // on the paths passed in), so this same entry point backs both the
    // Settings "test this rule" preview AND runCombineScan()'s real enqueue
    // decisions below. Each rule map: {name, pattern, enabled, sortGroupIndex}.
    // Returns one entry per detected group: {ruleName, key, capturedValues
    // (QStringList, the rule's GROUPING capture groups un-concatenated — key
    // is these joined with no separator), files (QStringList, in combine
    // order — see findCombineGroups()'s doc comment for the sort
    // differentiator / mtime-default ordering rules), outputPath, status
    // ("ready"/"singleton"/"alreadyTranscribed"/"alreadyCombined"/"pending")}.
    Q_INVOKABLE QVariantList detectCombineGroups(const QVariantList &rules, const QStringList &audioPaths) const;

    // Live validity check for a combine-rule pattern as the user types it in
    // Settings — validates against the EXACT SAME regex engine
    // findCombineGroups()/matchRuleCapturedKey() actually use (QRegularExpression),
    // not QML/JS RegExp, whose syntax isn't guaranteed identical. Empty pattern
    // is not an error (that's just "no rule authored yet"); returns the empty
    // string on success, QRegularExpression's own errorString() otherwise.
    Q_INVOKABLE QString regexValidationError(const QString &pattern) const;

    // Backs Settings' per-rule "differentiator" buttons (SettingsPopup.qml):
    // a differentiator is just a capturing group whose captured text joins
    // matchRuleCapturedKey()'s combine-grouping key, but ONLY if it
    // actually participates in every match — a capturing group sitting in
    // an alternation branch that isn't always taken (e.g.
    // `(sometext)|(some other text)`) doesn't reliably differentiate
    // anything for the files that took the OTHER branch. Returns the
    // subset of audioPaths where `pattern` (already the CANDIDATE pattern
    // text — this method does no wrapping/rewriting itself) matches the
    // filename overall but capture group `groupIndex` (1-based) has
    // QRegularExpressionMatch::capturedStart() < 0, i.e. did not
    // participate in that match at all. A valid-but-zero-length
    // participation (e.g. `\d*` matching "") is NOT flagged — only
    // non-participation is. Empty result = safe to use as a differentiator
    // across every file currently in the vault.
    Q_INVOKABLE QStringList differentiatorNonParticipatingFiles(const QString &pattern, int groupIndex,
                                                                  const QStringList &audioPaths) const;

    // Runs detectCombineGroups() and enqueues a "combine" job (Job::combineOnly)
    // for every "ready" group not already queued (isCombineQueued()) — never
    // also queues a transcribe job; the merged file just becomes a normal
    // untranscribed vault entry the user transcribes like any other. Called
    // from Sidebar.qml's ribbon "combine" button — a deliberate, user-
    // initiated scan, not an automatic one (see the button's own doc
    // comment for why: silently merging on every file-add was a surprise
    // recorders shouldn't spring on the user without asking).
    Q_INVOKABLE void runCombineScan(const QVariantList &rules, const QStringList &audioPaths, const QUrl &vaultUrl,
                                     bool deleteOriginals);

signals:
    void busyChanged();
    void progressChanged();
    void overallProgressChanged();
    void currentPhaseChanged();
    void currentFileChanged();
    void queueChanged();
    // Item P6: fired whenever the path->JobStatus index (m_jobStatusByPath)
    // actually changes — i.e. on every queueChanged() that changes at least
    // one path's resolved status, see rebuildJobStatusIndex(). QML's file
    // tree delegates connect to this to know when to re-poll
    // jobStatusForPath() for their own path, without depending on the
    // (unrelated-to-them) full jobs() list shape.
    void jobStatusesChanged();
    // Item P23: see loadTranscriptAsync()'s doc comment. Always emitted on
    // the GUI thread (the background QThread marshals back via
    // QMetaObject::invokeMethod/Qt::QueuedConnection) — safe for QML to
    // react to directly, no cross-thread concerns for the receiver.
    void transcriptLoaded(const QUrl &fileUrl, const QString &body, const QVariantList &sentences);
    void liveUpdateTickChanged();
    void activePausedChanged();
    void activeStoppingChanged();
    void activeBackendNameChanged();
    // const: also emitted from setLabel()/setPrelude()/setTags() (item P19),
    // which are themselves const (pure file-rewrite helpers, no member state
    // touched) — Qt signals may be declared const, and doing so here lets
    // those synchronous, non-queued edit paths notify the sidebar the same
    // way the queued-job paths (relabel()/generateChapters()/etc, all
    // non-const) already do via onWorkerFinished().
    void finished(const QUrl &fileUrl, bool ok, const QString &error) const;
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
        // Item P21: true only for the automatic follow-up label pass
        // transcribe(autoLabel=true) queues alongside its transcribe job —
        // never set on the manual "Regenerate label and prelude" job
        // (relabel()), which must always force-regenerate regardless of
        // existing content. See relabelJob()'s doc comment for what this
        // does at execution time.
        bool skipIfAlreadyLabeled = false;
        bool renameSpeakerOnly = false;
        QString oldSpeakerName;
        QString newSpeakerName;
        bool mergeSpeakersOnly = false;
        QString sourceSpeakerName;
        QString targetSpeakerName;
        // Feature 3a: see TranscriptionController::generateChapters()'s doc
        // comment. Needs only fileUrl/vaultUrl (same as relabelOnly) — no
        // extra fields of its own.
        bool generateChaptersOnly = false;
        // Split-recording combine (see findCombineGroups()'s doc comment):
        // fileUrl doubles as the combine job's OUTPUT path for this kind,
        // same "reuse fileUrl for whatever this kind's one file is" pattern
        // the other *Only kinds already use. combineSourcePaths is the
        // ordered (combine order — see findCombineGroups()'s doc comment)
        // list of parts to concatenate.
        bool combineOnly = false;
        QStringList combineSourcePaths;
        bool deleteOriginalsAfterCombine = false;
        bool restrictTagsToExisting = false;
        double tagReuseBias = 0.5;
        double wordiness = 0.5;
        DownloadTarget downloadTarget = DownloadTarget::None;
        JobStatus status = JobStatus::Queued;
        QString error;
        // -1 until the job reaches Done/Failed (see persistQueue()'s doc
        // comment for why this is wall-clock-active time, not
        // finishedAt-startedAt) — shown in the Tasks popup as "took Ns".
        qint64 durationMs = -1;
    };

    // Shared by jobs()' "kind" key and currentJobKind() — the flags above
    // (plus downloadTarget != None) are mutually exclusive by construction
    // (each enqueuing function only ever sets one).
    static QString jobKind(const Job &job);

    // One detected group of split-recording parts — see findCombineGroups()'s
    // doc comment for the matching/grouping/status semantics. Mirrors
    // detectCombineGroups()'s QVariantMap shape one-to-one; kept as a real
    // struct internally so startJob()'s per-job safety check and
    // runCombineScan() don't have to round-trip through QVariant.
    struct CombineGroup {
        QString ruleName;
        QString key;
        // The rule's GROUPING capture groups, un-concatenated (key is these
        // joined with no separator) — deliberately excludes the sort
        // differentiator's own group, if the rule has one (see
        // findCombineGroups()'s doc comment: a sort differentiator is
        // explicitly not a grouping one). Exposed to detectCombineGroups()'s
        // QVariantList so the Settings preview can show a header of exactly
        // which differentiator values grouped these files, not just the
        // flattened key.
        QStringList capturedValues;
        QStringList files; // combine order — see findCombineGroups()'s doc comment
        QString outputPath;
        QString status; // "ready"/"singleton"/"alreadyTranscribed"/"alreadyCombined"/"pending"
    };

    // Matches every path in audioPaths against every enabled rule in `rules`
    // ({name, pattern, enabled, sortGroupIndex} maps) using QRegularExpression
    // SEARCH semantics (not anchored unless the pattern itself has ^/$)
    // against the full filename (name+ext). A match's capture groups
    // (captured(1..N), concatenated, EXCLUDING the sortGroupIndex one if the
    // rule has one — see below) are the group-identity "key" — an
    // uncaptured wildcard span (bare `.`, `\d{3}`, `.*`, ...) varies freely
    // between members without affecting grouping, matching the user's
    // stated model of "the rule gets the real string with the wildcard
    // replaced; same real string = combine". A rule with zero grouping
    // capture groups therefore groups EVERY file it matches into one bucket
    // (expected regex behavior, not a special case).
    //
    // Combine ORDER within a group: `sortGroupIndex` (1-based capture-group
    // number, -1/absent = none) optionally names ONE of the rule's capture
    // groups as the SORT differentiator instead of a grouping one — the
    // Settings UI enforces "at most one, and it stops being a grouping
    // differentiator the moment it's chosen" (matchRuleCapturedKey() excludes
    // it from `key`/capturedValues entirely). When every member of a group
    // captured a value for it, members are ordered by that value — compared
    // numerically (QString::toDouble()) if EVERY member's value parses as a
    // number, else compared as plain strings; "numeric or not" is decided
    // once for the whole group, not per pair, since a comparator that
    // switches between numeric and lexicographic per pair isn't a strict
    // weak ordering (undefined behavior for std::sort). Falls back to mtime
    // ascending — the default, and the ONLY order when no sort
    // differentiator is configured — deliberately NOT file creation/birth
    // time: birth time doesn't survive a plain file copy on Linux (cp/rsync/
    // a cross-filesystem mv all stamp a fresh one on the destination), while
    // mtime is preserved by every standard copy tool, and this app has no
    // way to know (or reason to assume) whether a given vault's audio was
    // recorded directly onto the vault path or copied in from an SD card —
    // mtime is the only signal that's correct either way.
    //
    // outputPath is derived from (sanitized ruleName, sanitized key) rather
    // than any single member's filename, so it's re-derivable from just one
    // surviving filename even after the rest have been moved into
    // .combined/ — see startJob()'s safety-net re-check, which relies on
    // exactly that. status: "singleton" (only one match), "alreadyTranscribed"
    // (skip — some member already has a sibling .md, never silently discard
    // existing work), "alreadyCombined" (outputPath already exists on disk),
    // "pending" (would be ready, but the group's true most-recently-modified
    // member — independent of whatever order the group ended up sorted into
    // above — has an mtime inside kCombineStabilityWindowSecs of "now": the
    // recorder may still be writing it), else "ready".
    static QList<CombineGroup> findCombineGroups(const QVariantList &rules, const QStringList &audioPaths);
    // True if a Queued/Active "combine" job already targets this exact
    // outputPath — runCombineScan()'s dedup check, mirrors isDownloadQueued().
    bool isCombineQueued(const QString &outputPath) const;
    // Builds and inserts a Queued Job for `group` at frontOfQueueInsertIndex()
    // (or the given insertIndex, for callers that already computed a
    // specific slot — see startJob()'s safety-net re-check). Shared by
    // runCombineScan() and startJob() so both enqueue combine jobs identically.
    void enqueueCombineJob(const CombineGroup &group, const QUrl &vaultUrl, bool deleteOriginals,
                            int insertIndex = -1);
    // Safety-net re-check, called from the very top of startJob() before any
    // state mutation — "double check the file being transcribed for parts
    // before or after, whenever it is being transcribed" (the user's last
    // manual runCombineScan() run can miss a sibling part that appeared, or
    // never have run at all this session, before this job sat Queued).
    // Only acts on a plain transcribe-kind job (no other *Only
    // flag/downloadTarget set) with a non-empty fileUrl; returns false
    // immediately for anything else. Two outcomes when it does act:
    // (1) the merged output for this exact fileUrl's filename already
    // exists (recomputed from the filename alone, not group membership —
    // see findCombineGroups()'s outputPath doc comment) -> redirects
    // job.fileUrl in place and returns false, so startJob() falls through to
    // its normal dispatch against the now-merged file; (2) fileUrl is a
    // member of a currently-"ready" group per the last runCombineScan() run
    // (m_combineRules/m_combineAudioPaths) -> inserts a combine Job ahead of
    // this one (same insert-ahead-of-transcribe shape as
    // maybeQueueDownloadJob), redirects job.fileUrl to the pending output,
    // and recurses via startJob(index) onto the freshly-inserted combine job
    // (safe: combine jobs are never "transcribe kind", so this check is a
    // no-op on that recursive call) — returns true, telling startJob() NOT
    // to also run its normal dispatch for this call.
    bool maybeRedirectForCombine(int index);

    // Item P6: rebuilds m_jobStatusByPath from scratch off the current
    // m_jobs, emitting jobStatusesChanged() iff the resolved per-path status
    // actually changed. Connected to queueChanged() in the constructor
    // (alongside persistQueue()) rather than called individually from every
    // m_jobs-mutating site — every mutation (enqueue, startJob(),
    // onWorkerFinished(), cancelJob()/cancelAll(), dismiss()/clearFinished(),
    // moveQueuedJob(), the replay path) already emits queueChanged(), so this
    // one connection keeps the index current everywhere a targeted per-call-
    // site update would otherwise be needed. When more than one job shares a
    // path (a Done/Failed history entry plus a freshly re-queued run), the
    // more "in progress" one wins: Active > Paused > Queued > Failed > Done —
    // i.e. show whatever's most relevant to "is something happening to this
    // file right now", falling back to Failed over Done among purely
    // historical entries since a failure is the more actionable one to
    // surface. A download job (empty fileUrl) contributes nothing to this
    // index — it names no single vault file.
    void rebuildJobStatusIndex();

    void startJob(int index);
    void onWorkerFinished(bool ok, const QString &error);
    // Wave N (task system, item 5): pushes a Queued "download" job for
    // whichever model file `target` names, UNLESS one is already
    // Queued/Active or the file is already on disk (checked fresh here, not
    // cached — models can be deleted out from under the app between calls).
    // Called right before transcribe()/relabel() queues its own job, so the
    // download (if needed) lands just ahead of it in m_jobs and therefore
    // runs first. Returns the new job's id, or -1 if no download was needed.
    //
    // insertIndex (item O15): where to insert the download job, if one is
    // queued — -1 (default) appends to the end of m_jobs, which is what
    // queue-REPLAY callers (enqueueReplayedJob()/loadPersistedQueue()) want,
    // since restoring a persisted session must reproduce its exact saved
    // order rather than jump anything. transcribe()/relabel() instead pass
    // frontOfQueueInsertIndex()'s cursor, so a live "start this now" request
    // jumps ahead of whatever's already Queued (see their own doc comments).
    int maybeQueueDownloadJob(DownloadTarget target, const QUrl &vaultUrl, const QString &modelId,
                               int insertIndex = -1);
    // Item O15: where a freshly requested job should land in m_jobs so it
    // runs NEXT once whatever's currently running finishes, ahead of
    // anything already Queued — right after the Active job if one exists
    // (never ahead of it: this must not preempt work already in flight), or
    // otherwise right before the first currently-Queued entry (never before
    // a Paused job, which startNext() already treats as higher priority
    // than Queued — see its doc comment; and never before Done/Failed
    // history, which is append-only by design — see the class doc comment).
    int frontOfQueueInsertIndex() const;
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
    // Item P3: m_phaseTimings is now keyed per-CONFIGURATION (phase + modelId
    // + speakerDetection, see canonicalPhaseKey() in the .cpp), not just
    // phase name — modelId/speakerDetection are read from the Active job
    // (activeModelIdForTiming()/m_activeSpeakerDetection) right here rather
    // than threaded through the phaseDurationMs signal itself, since both are
    // already available on the controller side whenever this runs.
    void recordPhaseDuration(const QString &phase, qint64 durationMs);
    // modelId of whichever job is currently Active, or empty if none/not
    // applicable — see recordPhaseDuration()'s doc comment above for why this
    // reads m_jobs/m_activeIndex directly instead of needing a parameter
    // threaded in from the worker's signal.
    QString activeModelIdForTiming() const;
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
    // Populates m_phaseTimings from QSettings' "phaseTimings" group — one key
    // per (phase, modelId, speakerDetection) combination ever recorded (see
    // canonicalPhaseKey() in the .cpp), each a QVariantList of up to
    // RollingAverage::kCapacity raw millisecond samples — called once from
    // the constructor. Item P3: since the composite key embeds an
    // open-ended modelId, this enumerates whatever keys are actually
    // persisted (QSettings::childKeys()) rather than a fixed list. App-scoped
    // (not per-vault): how long a phase takes is a property of this machine
    // + whichever model was used, not something that should differ by vault.
    void loadPhaseTimings();
    // Writes JUST `key`'s (the composite canonicalPhaseKey() string) current
    // sample list back to QSettings — called right after recordPhaseDuration()
    // adds a new sample. Per-key (not a single bulk save) since jobs
    // typically complete one phase at a time, not all four at once.
    void savePhaseTiming(const QString &key);

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
    // each named phase, keyed by a COMPOSITE string — phase bucket + modelId
    // + speakerDetection (see canonicalPhaseKey() in the .cpp), not just the
    // phase name TranscriptionWorker::phase emits
    // ("loading"/"decoding"/"detecting-speakers"/"transcribing"). Item P3: a
    // large model's phases take materially longer than a tiny one's, and
    // diarization adds real overhead, so different configurations must not
    // share one rolling average — see recomputeOverallProgress()'s doc
    // comment for how these become the progress bar's per-phase widths.
    // Loaded once in the constructor (loadPhaseTimings()), updated+persisted
    // per-key as jobs complete each phase (recordPhaseDuration()/
    // savePhaseTiming()).
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
    // Item P6: rebuilt from m_jobs by rebuildJobStatusIndex() (see its own
    // doc comment) — keyed by absolute local file path, mirrors jobs()'
    // per-path status but as an O(1) lookup for jobStatusForPath() instead of
    // a linear jobs() scan.
    QHash<QString, JobStatus> m_jobStatusByPath;
    int m_activeIndex = -1;
    int m_nextJobId = 1;
    // Set by loadPersistedQueue() (once per vault open) — where persistQueue()
    // writes back to. Empty (no persistence) until then.
    QUrl m_vaultUrl;
    // Cached from the most recent runCombineScan() call (Sidebar.qml's
    // ribbon "combine" button — a user-initiated action now, not automatic)
    // so maybeRedirectForCombine()'s per-job safety check has something to
    // consult without TranscriptionController needing its own
    // VaultTreeModel/AppSettings reference. Only as fresh as the user's last
    // manual combine run — possibly empty if they've never run one this
    // session — acceptable, see maybeRedirectForCombine()'s doc comment.
    QVariantList m_combineRules;
    QStringList m_combineAudioPaths;
    bool m_combineDeleteOriginals = false;
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
    // See activeBackendName Q_PROPERTY's doc comment.
    QString m_activeBackendName;

    // Item O19: readMetadata()/readTranscriptBody()/parseSentences() are all
    // Q_INVOKABLE and called synchronously from QML bindings on every
    // liveUpdateTick (~5x/sec, see its own doc comment) while a
    // transcription is actively running on the currently-open file — each
    // one used to unconditionally re-open, re-read, and (for parseSentences)
    // re-run a regex over the ENTIRE file from scratch, every tick, on the
    // GUI thread, with cost growing with transcript length. These three
    // single-slot (not per-path map — only the currently open file is ever
    // polled this often; switching files just misses the cache once, no
    // separate eviction step needed) caches back the mitigation: a cheap
    // QFileInfo stat (size+mtime) guards the common "nothing changed since
    // last call" case entirely, and parseSentences() additionally reuses an
    // exact-prefix check to regex-parse only the NEW trailing bytes of an
    // append-only live transcript instead of the whole body (falling back to
    // a full reparse whenever that prefix check fails — e.g.
    // SegmentLineWriter rewrote the tail in place merging a repeat cycle —
    // never trading correctness for speed).
    struct FrontmatterCache {
        QString mdPath;
        qint64 size = -1;
        QDateTime mtime;
        QVariantMap data;
        bool valid = false;
    };
    mutable FrontmatterCache m_frontmatterCache;
    struct TranscriptBodyCache {
        QString mdPath;
        qint64 size = -1;
        QDateTime mtime;
        QString body;
        bool valid = false;
    };
    mutable TranscriptBodyCache m_bodyCache;
    struct SentenceCache {
        QString mdPath;
        QString body; // the exact body string `sentences` was parsed from
        QVariantList sentences;
        bool valid = false;
    };
    mutable SentenceCache m_sentenceCache;
    // Item P23: readMetadata()/readTranscriptBody()/parseSentences() are now
    // also invoked from a background QThread by loadTranscriptAsync() (see
    // its doc comment), concurrently with the GUI thread's own synchronous
    // calls to the same three functions (the existing liveUpdateTick-driven
    // polling above) — this single mutex serializes all access to the three
    // caches above so neither thread ever observes/writes a torn cache
    // entry. One coarse lock (not per-cache) is deliberate: these functions
    // are cheap and this is not a contended hot path (the async path only
    // runs once per file-open, not per-tick), so the simplicity of locking
    // each function's entire body outweighs any benefit from finer-grained
    // locking. RECURSIVE: parseSentences() locks this itself and then calls
    // readTranscriptBody()/readMetadata(), which each lock it again on the
    // SAME thread — a plain QMutex would deadlock there.
    mutable QRecursiveMutex m_transcriptCacheMutex;
};
