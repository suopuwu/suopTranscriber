#pragma once

#include <QObject>
#include <QThread>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>
#include <QVector>
#include <qqml.h>

class TranscriptionWorker;

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
    Q_PROPERTY(QUrl currentFile READ currentFile NOTIFY currentFileChanged)
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
    QUrl currentFile() const { return m_currentFile; }
    // Derived from m_jobs: total entry count / Done+Failed count. Both only
    // grow until dismiss()/clearFinished() shrink the list (see class doc).
    int tasksTotal() const;
    int tasksCompleted() const;
    // fileUrls of jobs still Queued (not yet started) — used by QML to show
    // a "Queued…" state distinct from the currently-Active job.
    QVariantList queuedFileUrls() const;
    // Every job for this session (Queued+Active+Done+Failed), each as
    // {fileUrl, status ("queued"/"active"/"done"/"failed"), error}.
    QVariantList jobs() const;

    // No-op if fileUrl already has a Queued or Active entry (no duplicate
    // in-flight-or-pending tasks). A Done/Failed entry for the same fileUrl
    // does NOT block re-queueing — deliberately allows re-transcribing after
    // dismissal or after changing settings. Starts immediately if idle,
    // otherwise queues. autoLabel opts into the AI auto-labeling second pass
    // (see LabelingEngine) — a labeling failure never fails the overall job,
    // since the primary transcript is already the thing that matters; it is
    // logged and the job still reports success.
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
    // Accounts for speakerDetection always forcing the tdrz model
    // regardless of modelId (see ModelManager's documented override), so a
    // model-id mismatch alone doesn't cause a false "differs" when speaker
    // detection is what actually determined the model both times.
    Q_INVOKABLE bool matchesExistingTranscription(const QUrl &fileUrl, const QString &modelId,
                                                   bool speakerDetection) const;

    // "Regenerate auto-title" (item 9): re-runs just the AI auto-labeling
    // pass on an already-transcribed file — reads the EXISTING transcript
    // text, does not re-transcribe audio. Queued through the same job/worker
    // infrastructure as transcribe() (dedup, persistent job list) since it's
    // real GPU work; unlike transcribe()'s autoLabel option, a failure here
    // IS the job's result (there's nothing else for this job to accomplish).
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
    // model itself always names the file that actually ran.
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

    // Everything after the frontmatter's closing "---" line (i.e. the
    // segment text), for main-panel search. Empty string if untranscribed
    // or unreadable.
    Q_INVOKABLE QString readTranscriptBody(const QUrl &fileUrl) const;

    // Parses readTranscriptBody()'s "[HH:MM:SS.mmm] [tag] text" lines (see
    // appendSegmentLine/formatTimestamp) into a list of {t0: seconds as
    // double, speaker: QString (empty = no speaker detection), text:
    // QString, start, length: int}, in file order (which is also time order
    // — segments are appended as whisper produces them). "speaker" is the
    // literal bracketed tag text as stored in the file — "Speaker N" by
    // default, or whatever a user has renamed it to (see renameSpeaker()
    // above), never translated through a separate lookup. No "t1": it's
    // never stored (always identical to the next segment's t0) and nothing
    // consumes it. start/length are the character range of text within THIS
    // SAME readTranscriptBody() string, so a caller can span-highlight the
    // active sentence without re-deriving offsets. For sentence-level
    // highlighting synced to playback (see DESIGN.md / CLAUDE.md "sentence-
    // level only, binary-search on positionChanged").
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
        bool restrictTagsToExisting = false;
        double tagReuseBias = 0.5;
        JobStatus status = JobStatus::Queued;
        QString error;
    };

    void startJob(int index);
    void onWorkerFinished(bool ok, const QString &error);

    QThread m_thread;
    TranscriptionWorker *m_worker = nullptr;
    bool m_busy = false;
    double m_progress = 0.0;
    QUrl m_currentFile;
    QVector<Job> m_jobs;
    int m_activeIndex = -1;
};
