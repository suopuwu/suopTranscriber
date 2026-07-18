// tst_processrunner — the pause/cancel poll loop shared by AudioDecoder,
// ModelManager and CombineTask (see ProcessRunner::runPausable). Drives real
// child processes (`sleep`) so the SIGSTOP/SIGCONT/kill behavior is exercised
// end-to-end, not mocked.

#include <QtTest>

#include <QElapsedTimer>
#include <QProcess>
#include <QStandardPaths>

#include "ProcessRunner.h"

class TstProcessRunner : public QObject
{
    Q_OBJECT

    QString m_sleep;

private slots:
    void initTestCase()
    {
        m_sleep = QStandardPaths::findExecutable(QStringLiteral("sleep"));
        if (m_sleep.isEmpty())
            QSKIP("no `sleep` binary on PATH — cannot drive a real child process");
    }

    // A child that exits on its own returns Finished, leaves the process not
    // running, and onPoll ran at least once.
    void finishes_whenChildExitsOnItsOwn()
    {
        QProcess p;
        p.start(m_sleep, {QStringLiteral("0.3")});
        QVERIFY(p.waitForStarted(5000));

        int polls = 0;
        const auto outcome = ProcessRunner::runPausable(p, {}, [&] { ++polls; });

        QVERIFY(outcome == ProcessRunner::RunOutcome::Finished);
        QCOMPARE(p.state(), QProcess::NotRunning);
        QVERIFY2(polls >= 1, "onPoll must run at least once");
    }

    // A cancel predicate that trips mid-run kills the child promptly (does NOT
    // wait out the long sleep) and returns Cancelled.
    void cancels_killsChildPromptly()
    {
        QProcess p;
        p.start(m_sleep, {QStringLiteral("30")});
        QVERIFY(p.waitForStarted(5000));

        int polls = 0;
        ProcessRunner::PauseCancelControl control;
        control.cancelled = [&] { return ++polls >= 2; };

        QElapsedTimer timer;
        timer.start();
        const auto outcome = ProcessRunner::runPausable(p, control, {});

        QVERIFY(outcome == ProcessRunner::RunOutcome::Cancelled);
        QCOMPARE(p.state(), QProcess::NotRunning);
        QVERIFY2(timer.elapsed() < 3000, "cancel must kill the child promptly, not wait for `sleep 30`");
    }

    // Pausing then resuming fires exactly one pause and one resume transition
    // (edge-detected, not once-per-iteration) and the child still completes.
    void pauseResume_firesTransitionsAndStillFinishes()
    {
        QProcess p;
        p.start(m_sleep, {QStringLiteral("0.4")});
        QVERIFY(p.waitForStarted(5000));

        int polls = 0;
        QList<bool> transitions;
        ProcessRunner::PauseCancelControl control;
        control.paused = [&] { return polls < 2; }; // paused for the first two polls only
        control.onPauseChanged = [&](bool paused) { transitions.append(paused); };

        const auto outcome = ProcessRunner::runPausable(p, control, [&] { ++polls; });

        QVERIFY(outcome == ProcessRunner::RunOutcome::Finished);
        QCOMPARE(p.state(), QProcess::NotRunning);
        // Exactly one suspend edge and one resume edge — not one per paused poll.
        QCOMPARE(transitions, (QList<bool>{true, false}));
    }
};

QTEST_GUILESS_MAIN(TstProcessRunner)
#include "tst_processrunner.moc"
