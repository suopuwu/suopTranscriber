#pragma once

#include <QFile>
#include <QObject>
#include <QString>
#include <QThread>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>
#include <QVector>
#include <qqml.h>

#include "Segment.h"

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
    // "loading" (whisper model load — no native progress hook exists for
    // this, so progress stays 0 here; QML shows an indeterminate bar),
    // "detecting-speakers-segmenting" / "detecting-speakers-identifying"
    // (diarization pass, REAL progress via DiarizationEngine's sherpa-onnx-
    // backed callback — split into two sub-phases at fraction==0.30, the
    // exact boundary where sherpa-onnx's own internal work moves from
    // finding WHERE speech is to figuring out WHOSE voice it is, so the
    // label switches right when the real work does, not on a guess),
    // "transcribing" (whisper's own per-segment progress, as before), or
    // empty when idle or for a relabel/rename job (neither has phases
    // worth naming). When detection actually runs, the detecting-speakers-*
    // sub-phases and "transcribing" share ONE continuous 0->1 progress
    // sweep (first half / second half — see run()) rather than each
    // getting its own 0->1, which would look like the bar finishes then
    // restarts. Exists because "huge delay before it starts transcribing"
    // on bigger models was previously invisible — the progress bar sat
    // frozen at 0 during model load and (once diarization landed) during
    // the whole diarization pass too, indistinguishable from a hang.
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

public:
    enum class JobStatus { Queued, Active, Done, Failed };
    Q_ENUM(JobStatus)

    explicit TranscriptionController(QObject *parent = nullptr);
    ~TranscriptionController() override;

    bool isBusy() const { return m_busy; }
    double progress() const { return m_progress; }
    QString currentPhase() const { return m_phase; }
    QUrl currentFile() const { return m_currentFile; }
    // See the currentJobKind Q_PROPERTY doc comment above.
    QString currentJobKind() const;
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

    // Item 17: de-duplicated, sorted list of every tag already used anywhere
    // in the vault — feeds the sidebar's tag-input autocomplete. Same scan
    // LabelingEngine's "restrict to existing tags" mode already uses
    // internally (collectVaultTags, TranscriptionController.cpp); exposed
    // here so QML doesn't need its own duplicate frontmatter-scanning logic.
    Q_INVOKABLE QStringList existingVaultTags(const QUrl &vaultUrl) const;

signals:
    void busyChanged();
    void progressChanged();
    void currentPhaseChanged();
    void currentFileChanged();
    void queueChanged();
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
    struct Job {
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
        JobStatus status = JobStatus::Queued;
        QString error;
    };

    // Shared by jobs()' "kind" key and currentJobKind() — the three flags
    // above are mutually exclusive by construction (each enqueuing function
    // only ever sets one).
    static QString jobKind(const Job &job);

    void startJob(int index);
    void onWorkerFinished(bool ok, const QString &error);

    QThread m_thread;
    TranscriptionWorker *m_worker = nullptr;
    bool m_busy = false;
    double m_progress = 0.0;
    QString m_phase;
    QUrl m_currentFile;
    QVector<Job> m_jobs;
    int m_activeIndex = -1;
};
