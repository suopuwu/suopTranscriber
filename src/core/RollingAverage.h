#pragma once

#include <QVector>

// Exact rolling average of the last kCapacity samples — a small fixed-size
// circular buffer, not an exponential/decaying approximation. Used to weight
// the transcription progress bar's phase segments by how long each phase has
// actually taken recently (see TranscriptionController::recomputeOverallProgress()),
// instead of a fixed guess. Capped at 10 samples so one wildly slow/fast job
// (a huge file, a cold model download folded into "loading", etc.) ages out
// instead of permanently skewing the bar's proportions.
class RollingAverage {
public:
    static constexpr int kCapacity = 10;

    void addSample(double value)
    {
        if (m_samples.size() < kCapacity) {
            m_samples.append(value);
        } else {
            m_samples[m_nextIndex] = value;
            m_nextIndex = (m_nextIndex + 1) % kCapacity;
        }
    }

    // fallback is returned verbatim when no samples exist yet (fresh install,
    // or a phase that's never completed once) — callers use it to seed a
    // sane initial bar proportion before any real timing data exists.
    double average(double fallback) const
    {
        if (m_samples.isEmpty())
            return fallback;
        double sum = 0.0;
        for (double v : m_samples)
            sum += v;
        return sum / m_samples.size();
    }

    bool isEmpty() const { return m_samples.isEmpty(); }
    int count() const { return m_samples.size(); }

    // Order doesn't matter for the average itself, so persistence just needs
    // to round-trip the up-to-10 raw values — reloading replays them through
    // addSample() in whatever order they're given.
    QVector<double> samples() const { return m_samples; }
    void setSamples(const QVector<double> &samples)
    {
        m_samples.clear();
        m_nextIndex = 0;
        for (double v : samples)
            addSample(v);
    }

private:
    QVector<double> m_samples;
    int m_nextIndex = 0;
};
