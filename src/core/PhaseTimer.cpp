#include "PhaseTimer.h"

#include <QSettings>
#include <QVariant>
#include <QVariantList>
#include <QtGlobal>

#include <utility>

PhaseTimer::PhaseTimer(QObject *parent)
    : QObject(parent)
{
    loadPhaseTimings();
}

const QStringList &PhaseTimer::timedPhases()
{
    static const QStringList phases = {
        QStringLiteral("loading"),
        QStringLiteral("decoding"),
        QStringLiteral("detecting-speech"),   // VAD windowing (every transcribe job)
        QStringLiteral("detecting-speakers"), // diarization (standalone diarize job)
        QStringLiteral("transcribing"),
    };
    return phases;
}

// DiarizationEngine's progress callback emits phase() as
// "detecting-speakers-segmenting"/"detecting-speakers-identifying" (see
// run()'s doc comment on the 0.30 sub-phase boundary) — both belong to the
// single "detecting-speakers" bucket for timing/weighting purposes. Every
// other phase name is already its own bucket. This is the BUCKET identity
// only (used to compare against recomputeOverallProgress()'s `active` list
// and to look up defaultPhaseDurationMs()) — NOT the m_phaseTimings storage
// key, see canonicalPhaseKey() below for that.
static QString phaseBucket(const QString &phase)
{
    if (phase.startsWith(QStringLiteral("detecting-speakers")))
        return QStringLiteral("detecting-speakers");
    return phase;
}

// 2026-07-10: m_phaseTimings' actual storage key — folds modelId
// (and, only when relevant, an explicit "diar" marker for speakerDetection)
// onto the phase bucket name. A large model's "loading"/"transcribing" takes
// materially longer than a tiny one, and diarization adds real per-job
// overhead — sharing one rolling average across every model/config produced
// meaningless bar proportions and ETAs the instant a user switched models.
// modelId is deliberately part of the key for EVERY phase (not just
// "loading"/"transcribing" where it obviously matters) — keeping one
// consistent key shape is simpler than special-casing which phases care.
static QString canonicalPhaseKey(const QString &phase, const QString &modelId, bool speakerDetection,
                                 const QString &kind = {})
{
    QString key = phaseBucket(phase) + QLatin1Char('|') + modelId;
    if (speakerDetection)
        key += QStringLiteral("|diar");
    // Only the SelfCumulative synthetic-ramp path passes a kind; folding it in
    // keeps a RelabelTask "labeling" timing from ever sharing a bucket with a
    // WeightedPhases key (those never carry a kind, so their shape is unchanged).
    if (!kind.isEmpty())
        key += QLatin1Char('|') + kind;
    return key;
}

// Fixed plausible duration selfCumulativeSyntheticProgress() ramps against
// before any real sample exists — a single LLM-ish pass (label / chapters
// generation) is the common case. Same role defaultPhaseDurationMs() plays for
// the WeightedPhases phases: too small and the bar saturates within a tick;
// this just sets ramp SHAPE, never correctness (the 0.95 clamp guards the top).
static constexpr double kSelfCumulativeDefaultDurationMs = 8000.0;

void PhaseTimer::loadPhaseTimings()
{
    // 2026-07-10: keys are now composite (phase|modelId[|diar]),
    // so there's no fixed upfront list to load by name — enumerate whatever
    // keys are actually persisted instead. childKeys() naturally covers every
    // model/config combination ever recorded on this machine.
    QSettings settings;
    settings.beginGroup(QStringLiteral("phaseTimings"));
    for (const QString &key : settings.childKeys()) {
        const QVariantList stored = settings.value(key).toList();
        QVector<double> samples;
        samples.reserve(stored.size());
        for (const QVariant &v : stored)
            samples.append(v.toDouble());
        RollingAverage avg;
        avg.setSamples(samples);
        m_phaseTimings.insert(key, avg);
    }
    settings.endGroup();
}

void PhaseTimer::savePhaseTiming(const QString &key)
{
    QSettings settings;
    settings.beginGroup(QStringLiteral("phaseTimings"));
    QVariantList list;
    for (double v : m_phaseTimings.value(key).samples())
        list.append(v);
    settings.setValue(key, list);
    settings.endGroup();
}

void PhaseTimer::setOverallProgress(double value)
{
    m_overallProgress = value;
    emit overallProgressChanged();
}

void PhaseTimer::recordPhaseDuration(const QString &phase, const QString &modelId,
                                     bool speakerDetection, qint64 durationMs)
{
    const QString key = canonicalPhaseKey(phase, modelId, speakerDetection);
    m_phaseTimings[key].addSample(static_cast<double>(durationMs));
    savePhaseTiming(key);
}

// See this method's header doc comment. "detecting-speakers"/"transcribing"
// share the same estimate so their DEFAULT split is exactly 50/50 (the fixed
// split this whole feature replaced) until real per-machine/per-model timing
// data overrides it; "loading"/"decoding"/"detecting-speech" (VAD) get their
// own modest estimates instead of being folded into that 50/50.
double PhaseTimer::defaultPhaseDurationMs(const QString &phase)
{
    if (phase == QStringLiteral("loading"))
        return 2000.0;
    if (phase == QStringLiteral("decoding"))
        return 1500.0;
    if (phase == QStringLiteral("detecting-speech"))
        return 1500.0; // VAD windowing — a small model + one computeWindows() pass
    return 10000.0; // "detecting-speakers" / "transcribing"
}

bool PhaseTimer::isSyntheticProgressPhase(const QString &phase)
{
    return phase == QStringLiteral("loading") || phase == QStringLiteral("decoding")
        || phase == QStringLiteral("detecting-speech");
}

double PhaseTimer::phaseRelativeSyntheticProgress(const QString &phase, const QString &modelId,
                                                  bool speakerDetection, double realProgress,
                                                  qint64 elapsedInPhaseMs) const
{
    // "loading"/"decoding"/"detecting-speech" have no native per-item progress
    // callback (each is one blocking call), so their within-phase fraction is
    // synthesized from elapsed-vs-historical-average; every other phase reports
    // real progress via the controller's m_progress.
    if (!isSyntheticProgressPhase(phase))
        return realProgress;
    const QString key = canonicalPhaseKey(phase, modelId, speakerDetection);
    const double avg = m_phaseTimings.value(key).average(defaultPhaseDurationMs(phase));
    if (avg <= 0.0)
        return 0.0;
    return qMin(0.95, static_cast<double>(elapsedInPhaseMs) / avg);
}

double PhaseTimer::selfCumulativeSyntheticProgress(const QString &phase, const QString &kind,
                                                   const QString &modelId, qint64 elapsedMs) const
{
    const QString key = canonicalPhaseKey(phase, modelId, false, kind);
    const double avg = m_phaseTimings.value(key).average(kSelfCumulativeDefaultDurationMs);
    if (avg <= 0.0)
        return 0.0;
    return qMin(0.95, static_cast<double>(qMax<qint64>(0, elapsedMs)) / avg);
}

void PhaseTimer::recordSelfCumulativePhaseDuration(const QString &phase, const QString &kind,
                                                   const QString &modelId, qint64 durationMs)
{
    const QString key = canonicalPhaseKey(phase, modelId, false, kind);
    m_phaseTimings[key].addSample(static_cast<double>(durationMs));
    savePhaseTiming(key);
}

void PhaseTimer::recomputeOverallProgress(const QStringList &activePhases, bool weightedPhases,
                                          const QString &phase, const QString &modelId,
                                          bool speakerDetection, double realProgress,
                                          qint64 elapsedInPhaseMs)
{
    // Only a WeightedPhases kind (transcribe/diarize) uses this rolling-average
    // phase-weighting scheme. Everything else — an idle bar, or a
    // SelfCumulative kind that assigns m_overallProgress directly from
    // progress() — is not covered here. The active serial task is the single
    // source of BOTH facts (its ProgressModel and its ordered phase set), so
    // this no longer hardcodes the phase list that used to drift against the
    // progress lambda's copy.
    if (!weightedPhases || phase.isEmpty())
        return;

    // The ordered phase set for the Active kind, so each phase gets its
    // rolling-average-proportioned slice of the 0..1 bar — authored by the
    // task (TranscribeTask: load/decode/detect-speech/transcribe; DiarizeTask:
    // decode/detect-speakers).
    const QStringList &active = activePhases;

    double totalWeight = 0.0;
    QMap<QString, double> weight;
    for (const QString &p : std::as_const(active)) {
        const double w = m_phaseTimings.value(canonicalPhaseKey(p, modelId, speakerDetection))
                              .average(defaultPhaseDurationMs(p));
        weight[p] = w;
        totalWeight += w;
    }
    if (totalWeight <= 0.0)
        totalWeight = 1.0;

    const QString currentBucket = phaseBucket(phase);
    double offset = 0.0;
    for (const QString &p : std::as_const(active)) {
        const double share = weight[p] / totalWeight;
        if (p == currentBucket) {
            m_overallProgress = offset + phaseRelativeSyntheticProgress(phase, modelId,
                                                                       speakerDetection,
                                                                       realProgress,
                                                                       elapsedInPhaseMs)
                                             * share;
            emit overallProgressChanged();
            return;
        }
        offset += share;
    }
}
