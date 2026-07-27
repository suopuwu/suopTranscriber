#pragma once

#include <QMap>
#include <QObject>
#include <QString>
#include <QStringList>

#include "RollingAverage.h"

// The phase-timing / progress-estimation collaborator split out of
// TranscriptionController. It owns exactly three things:
//
//   1. the per-configuration rolling averages of how long each phase has
//      actually taken (m_phaseTimings) and their QSettings persistence,
//   2. the phase vocabulary those averages key on (timedPhases(),
//      defaultPhaseDurationMs(), isSyntheticProgressPhase(), plus the
//      bucket/key folding in the .cpp), and
//   3. the blended 0..1 whole-job bar those averages produce
//      (overallProgress + its change signal).
//
// It deliberately owns NO active-job state: which phase is running, how long
// it's been running, which model, whether diarization is on, and the ordered
// phase set of the Active task all live on the controller (they're serial-lane
// display state, and the controller is the only thing that can observe them).
// They're passed in per call instead — see recomputeOverallProgress(). The
// controller forwards this object's overallProgressChanged() to its own
// same-named signal, so the QML-facing Q_PROPERTY updates exactly as before.
class PhaseTimer : public QObject
{
    Q_OBJECT
public:
    // Loads the persisted rolling averages (loadPhaseTimings()) immediately —
    // constructed once, as a member of TranscriptionController.
    explicit PhaseTimer(QObject *parent = nullptr);

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
    // The phase names m_phaseTimings/QSettings' "phaseTimings" group/
    // defaultPhaseDurationMs() all key on. "detecting-speech" (VAD windowing,
    // part of every transcribe job) and "detecting-speakers" (the standalone
    // diarize job) are both included even though each is specific to a job
    // shape — they're still things worth separately timing. Public for the
    // same testability reason as defaultPhaseDurationMs() above.
    //
    // A function over a function-local static (rather than a bare
    // `static const QStringList`) because TranscriptionController::kTimedPhases
    // — the public alias tests still spell — is initialized FROM this at
    // namespace scope in another TU; a plain static object would make that a
    // cross-TU static-initialization-order gamble that silently yields an
    // empty list, while a function-local static is guaranteed initialized on
    // first call.
    static const QStringList &timedPhases();
    // The phases with no native per-item progress callback — their within-phase
    // fraction is synthesized from elapsed-vs-average
    // (phaseRelativeSyntheticProgress()) and driven by the controller's
    // m_indeterminatePhaseTimer. ONE predicate both the timer-start gate and
    // the synthetic-fraction branch consult, so the two phase sets can never
    // drift again (they had: this logic once synthesized for "detecting-speech"
    // but only ran the timer for loading/decoding, so that phase's bar sat
    // frozen).
    static bool isSyntheticProgressPhase(const QString &phase);

    // The SAME rolling-average duration (ms) recomputeOverallProgress() and
    // phaseRelativeSyntheticProgress() weight/ramp against, exposed read-only
    // for a caller that needs the raw number rather than a fraction —
    // TranscriptionController::estimatedRemainingMs() scales its "progress
    // hasn't moved in a while ⇒ stalled" window to this phase's own historical
    // pace (a huge model's "transcribing" phase legitimately goes longer
    // between ticks than a tiny one's) instead of one fixed constant that
    // would be wrong for both ends of that range. No samples yet ⇒
    // defaultPhaseDurationMs(phase), same fallback every other caller here uses.
    double phaseAverageDurationMs(const QString &phase, const QString &modelId, bool speakerDetection) const;

    // The blended 0..1 whole-job fraction QML's progress bar binds to (via
    // TranscriptionController::overallProgress, which just reads this).
    double overallProgress() const { return m_overallProgress; }
    // Assigns the bar directly and notifies unconditionally — the two paths
    // that bypass the rolling-average blend entirely: a SelfCumulative task's
    // already-cumulative progress fraction, and the reset to 0 when a task
    // starts.
    void setOverallProgress(double value);

    // 2026-07-10 (rolling-average phase weighting): records that `phase` (one
    // of timedPhases()) just took `durationMs` on the job that just finished
    // it. The caller recomputes the overall bar right after, so the CURRENT
    // phase's own slice reflects the freshly-updated average right away rather
    // than waiting for the next tick.
    // m_phaseTimings is keyed per-CONFIGURATION (phase + modelId
    // + speakerDetection, see canonicalPhaseKey() in the .cpp), not just
    // phase name — modelId/speakerDetection describe the Active job and are
    // read off the controller at the call site (activeModelIdForTiming()/
    // m_activeSpeakerDetection) rather than threaded through the
    // phaseDurationMs signal itself.
    void recordPhaseDuration(const QString &phase, const QString &modelId,
                             bool speakerDetection, qint64 durationMs);

    // This phase's OWN 0..1 fraction — `realProgress` (the controller's
    // m_progress) directly for phases with a real progress callback
    // (detecting-speakers/transcribing), or, for "loading"/"decoding"/
    // "detecting-speech" (no callback exists — see run()'s doc comments),
    // `elapsedInPhaseMs` divided by that phase's own rolling average, clamped
    // to 0.95 so a slower-than-average run never LOOKS finished before the
    // real phase transition actually happens.
    double phaseRelativeSyntheticProgress(const QString &phase, const QString &modelId,
                                          bool speakerDetection, double realProgress,
                                          qint64 elapsedInPhaseMs) const;

    // The frozen-bar safety net for SelfCumulative tasks. A SelfCumulative kind
    // (RelabelTask, ChaptersTask, …) drives the whole-job bar straight from its
    // progress() fraction; when its run() is one long blocking call that emits
    // phase() but never progress(), the bar sits at 0 for the whole pass and
    // snaps to 1.0 at the end — the recurring "0% then 100%" report. This
    // synthesizes a within-job fraction from elapsed-vs-rolling-average for that
    // case, clamped to 0.95 so a slow run never LOOKS finished before it is. The
    // controller only ever RAISES the bar with this and abandons it the instant a
    // real progress() tick arrives (that real value wins). Keyed per
    // (phase|kind|modelId) so each kind's phase learns its own duration
    // (recordSelfCumulativePhaseDuration); no samples yet ⇒ a fixed plausible
    // default — same elapsed÷average shape as phaseRelativeSyntheticProgress()
    // above, but for a single cumulative bar with no per-phase share weighting.
    double selfCumulativeSyntheticProgress(const QString &phase, const QString &kind,
                                           const QString &modelId, qint64 elapsedMs) const;
    // Records how long a never-reported SelfCumulative phase actually took, so the
    // ramp above becomes elapsed-vs-real-average over runs. Same rolling store and
    // QSettings persistence as recordPhaseDuration(); the extra `kind` axis keeps
    // each SelfCumulative kind's phase timings separate (and clear of the
    // WeightedPhases keys, which never carry a kind).
    void recordSelfCumulativePhaseDuration(const QString &phase, const QString &kind,
                                           const QString &modelId, qint64 durationMs);

    // Rebuilds overallProgress from scratch: `activePhases` (the ordered phase
    // set the Active task authors — TranscribeTask: load/decode/detect-speech/
    // transcribe; DiarizeTask: decode/detect-speakers), each phase's
    // rolling-average share of that set (m_phaseTimings, falling back to
    // defaultPhaseDurationMs() per phase with no samples yet — a fixed 50/50
    // detecting-speakers/transcribing split by default, see its doc comment),
    // and phaseRelativeSyntheticProgress() for the current phase's own
    // within-phase fraction. Called on every phase() transition, every task
    // progress() tick, AND on the controller's m_indeterminatePhaseTimer tick
    // (the only way "loading"/"decoding" — which have no real progress
    // callback — ever advance mid-phase).
    //
    // A no-op (leaves overallProgress untouched) unless `weightedPhases` — the
    // Active task exists AND declares Task::ProgressModel::WeightedPhases — and
    // `phase` is non-empty. Everything else (an idle bar, or a SelfCumulative
    // kind that assigns the bar directly via setOverallProgress()) is not
    // covered by this weighting scheme. The Active task is the single source of
    // BOTH facts (its ProgressModel and its ordered phase set), so this never
    // hardcodes a phase list that could drift against the progress lambda's copy.
    void recomputeOverallProgress(const QStringList &activePhases, bool weightedPhases,
                                  const QString &phase, const QString &modelId,
                                  bool speakerDetection, double realProgress,
                                  qint64 elapsedInPhaseMs);

signals:
    // Forwarded verbatim to TranscriptionController::overallProgressChanged
    // (a signal-to-signal connect in the controller's constructor), which is
    // the NOTIFY of the QML-facing overallProgress Q_PROPERTY.
    void overallProgressChanged();

private:
    // Populates m_phaseTimings from QSettings' "phaseTimings" group — one key
    // per (phase, modelId, speakerDetection) combination ever recorded (see
    // canonicalPhaseKey() in the .cpp), each a QVariantList of up to
    // RollingAverage::kCapacity raw millisecond samples — called once from
    // the constructor. Since the composite key embeds an
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

    // Task system: the SAME 0..1 fraction run() used to
    // blend across diarization+transcription phases (see currentPhase's doc
    // comment for why that blending moved OUT of the displayed m_progress) —
    // also feeds estimatedRemainingMs()'s ETA math. Computed by
    // recomputeOverallProgress() (see its doc comment), not emitted by the
    // worker anymore, EXCEPT for a SelfCumulative kind (setOverallProgress()).
    double m_overallProgress = 0.0;
    // Rolling (last RollingAverage::kCapacity samples) average duration of
    // each named phase, keyed by a COMPOSITE string — phase bucket + modelId
    // + speakerDetection (see canonicalPhaseKey() in the .cpp), not just the
    // phase name Task::phase emits
    // ("loading"/"decoding"/"detecting-speakers"/"transcribing"). A
    // large model's phases take materially longer than a tiny one's, and
    // diarization adds real overhead, so different configurations must not
    // share one rolling average — see recomputeOverallProgress()'s doc
    // comment for how these become the progress bar's per-phase widths.
    // Loaded once in the constructor (loadPhaseTimings()), updated+persisted
    // per-key as jobs complete each phase (recordPhaseDuration()/
    // savePhaseTiming()).
    QMap<QString, RollingAverage> m_phaseTimings;
};
