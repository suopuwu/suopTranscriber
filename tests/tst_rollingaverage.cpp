// Living spec: RollingAverage — the fixed-size (last-10-samples) rolling
// average TranscriptionController uses to weight each transcription phase's
// slice of the progress bar by how long that phase has actually taken
// recently, instead of a fixed guess (see
// TranscriptionController::recomputeOverallProgress()).

#include <QtTest>

#include "RollingAverage.h"

class TstRollingAverage : public QObject
{
    Q_OBJECT

private slots:
    // No samples yet -> average() returns the caller's fallback verbatim,
    // never 0 or a crash — this is what lets a fresh install render a sane
    // (equal-weight) bar before any real timing data exists.
    void empty_returnsFallback()
    {
        RollingAverage avg;
        QCOMPARE(avg.average(1234.0), 1234.0);
        QVERIFY(avg.isEmpty());
        QCOMPARE(avg.count(), 0);
    }

    // Plain mean while under capacity.
    void underCapacity_isPlainMean()
    {
        RollingAverage avg;
        avg.addSample(10.0);
        avg.addSample(20.0);
        avg.addSample(30.0);
        QCOMPARE(avg.count(), 3);
        QCOMPARE(avg.average(0.0), 20.0);
    }

    // Capped at kCapacity (10) entries: the 11th sample evicts the OLDEST
    // (1st), not the newest — a true sliding window, not an exponential
    // decay approximation.
    void overCapacity_evictsOldestNotNewest()
    {
        RollingAverage avg;
        for (int i = 1; i <= RollingAverage::kCapacity; ++i)
            avg.addSample(static_cast<double>(i)); // 1..10, mean == 5.5
        QCOMPARE(avg.count(), RollingAverage::kCapacity);
        QCOMPARE(avg.average(0.0), 5.5);

        // 11th sample (100) evicts the 1 -> {2..10, 100}, mean == 15.4
        avg.addSample(100.0);
        QCOMPARE(avg.count(), RollingAverage::kCapacity);
        QCOMPARE(avg.average(0.0), 15.4);
    }

    // Many samples past capacity still never exceeds kCapacity entries, and
    // the buffer keeps wrapping correctly rather than only evicting once.
    void manySamplesPastCapacity_staysCapped()
    {
        RollingAverage avg;
        for (int i = 0; i < 25; ++i)
            avg.addSample(static_cast<double>(i));
        // Last 10 samples added were 15..24.
        QCOMPARE(avg.count(), RollingAverage::kCapacity);
        QCOMPARE(avg.average(0.0), 19.5);
    }

    // Persistence round-trip (TranscriptionController::loadPhaseTimings()/
    // savePhaseTiming() store exactly this vector via QSettings): reloading
    // samples() through setSamples() reproduces the same average and count,
    // and replaying MORE than kCapacity stored values still caps correctly.
    void samplesRoundTrip_preservesAverage()
    {
        RollingAverage avg;
        avg.addSample(1.0);
        avg.addSample(2.0);
        avg.addSample(3.0);

        RollingAverage reloaded;
        reloaded.setSamples(avg.samples());
        QCOMPARE(reloaded.count(), avg.count());
        QCOMPARE(reloaded.average(0.0), avg.average(0.0));

        QVector<double> oversized;
        for (int i = 0; i < RollingAverage::kCapacity + 5; ++i)
            oversized.append(static_cast<double>(i));
        RollingAverage capped;
        capped.setSamples(oversized);
        QCOMPARE(capped.count(), RollingAverage::kCapacity);
    }
};

QTEST_GUILESS_MAIN(TstRollingAverage)
#include "tst_rollingaverage.moc"
