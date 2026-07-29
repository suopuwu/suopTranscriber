// tst_processrunner — the pause/cancel poll loop shared by AudioDecoder,
// ModelManager and CombineTask (see ProcessRunner::runPausable). Drives real
// child processes (`sleep`) so the SIGSTOP/SIGCONT/kill behavior is exercised
// end-to-end, not mocked.

#include <QtTest>

#include <QElapsedTimer>
#include <QProcess>
#include <QStandardPaths>

#include "ProcessRunner.h"

#include <cerrno>
#include <cstdio>
#include <sys/wait.h>
#include <unistd.h>

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

    // A child that never exits and never produces output (a stand-in for a
    // wedged ffmpeg — see this file's header comment and DESIGN.md's NaN/
    // speechnorm incident) is killed once the stall predicate trips, not left
    // to run out `sleep`'s full duration.
    void stalledChild_isKilledAndReportedAsStalled()
    {
        QProcess p;
        p.start(m_sleep, {QStringLiteral("30")});
        QVERIFY(p.waitForStarted(5000));

        int polls = 0;
        ProcessRunner::PauseCancelControl control;
        control.stalled = [&] { return ++polls >= 2; }; // trips on the second poll

        QElapsedTimer timer;
        timer.start();
        const auto outcome = ProcessRunner::runPausable(p, control, {});

        QVERIFY(outcome == ProcessRunner::RunOutcome::Stalled);
        QCOMPARE(p.state(), QProcess::NotRunning);
        QVERIFY2(timer.elapsed() < 3000, "stall must kill the child promptly, not wait for `sleep 30`");
    }

    // The opt-in guarantee: leaving `stalled` unset behaves exactly as before
    // (Finished), even though `stalled` is checked on every iteration.
    void noStallPredicate_behavesExactlyAsBefore()
    {
        QProcess p;
        p.start(m_sleep, {QStringLiteral("0.3")});
        QVERIFY(p.waitForStarted(5000));

        const auto outcome = ProcessRunner::runPausable(p, {}, {});

        QVERIFY(outcome == ProcessRunner::RunOutcome::Finished);
        QCOMPARE(p.state(), QProcess::NotRunning);
    }

    // ProcessRunner::guardChildLifetime (see ProcessRunner.h) ties a spawned
    // child's life to its parent via PR_SET_PDEATHSIG, closing the orphaned-
    // ffmpeg leak (six abandoned decodes found still running 27h after their
    // app died — see the 2026-07-26 incident). Proving "our process dies ⇒
    // its child dies" can't be done in-process — a self-kill would end this
    // test before it could assert anything — so this reproduces the shape one
    // level removed: a raw fork() plays a "fake app", and a second fork()
    // under it plays the guarded child, re-arming PDEATHSIG against ITS OWN
    // parent (Linux clears the flag across every fork, so each generation
    // must set it for itself — exactly what QProcess::setChildProcessModifier
    // does inside guardChildLifetime for a real QProcess spawn). Both forked
    // branches use ONLY raw POSIX calls, never Qt/QProcess: Qt's internal
    // threads (process reaper, thread pool, …) do not survive a raw fork(),
    // so exercising QProcess itself post-fork would be testing an
    // unsupported combination, not the mechanism. What this proves is the
    // exact prctl(PR_SET_PDEATHSIG, SIGKILL) + getppid() pair
    // guardChildLifetime's ChildProcessModifier runs; that every real spawn
    // site actually CALLS guardChildLifetime is covered separately by
    // tst_styleguide's everyProcessSpawnGuardsChildLifetime() source scan.
    void guardedChild_diesWithItsParent()
    {
        int pipefd[2];
        QVERIFY(::pipe(pipefd) == 0);

        const pid_t fakeApp = ::fork();
        QVERIFY(fakeApp >= 0);

        if (fakeApp == 0) {
            // ---- forked "fake app" process — raw POSIX only from here on ----
            ::close(pipefd[0]);
            const pid_t grandchild = ::fork();
            if (grandchild == 0) {
                // ---- grandchild: stand-in for a guarded QProcess child ----
                ::prctl(PR_SET_PDEATHSIG, SIGKILL);
                if (::getppid() == 1)
                    ::_exit(1); // fake app already gone in the fork/prctl race window
                ::pause();      // parked until SIGKILL, direct or via PDEATHSIG
                ::_exit(0);
            }
            char buf[32];
            const int n = std::snprintf(buf, sizeof buf, "%d\n", static_cast<int>(grandchild));
            ::write(pipefd[1], buf, static_cast<size_t>(n));
            ::close(pipefd[1]);
            ::pause(); // fake app just waits here until we kill it
            ::_exit(0);
        }

        // ---- back in the real test process ----
        ::close(pipefd[1]);
        QByteArray line;
        char c;
        while (::read(pipefd[0], &c, 1) == 1 && c != '\n')
            line += c;
        ::close(pipefd[0]);
        bool ok = false;
        const pid_t grandchild = static_cast<pid_t>(line.toLongLong(&ok));
        QVERIFY2(ok && grandchild > 0, "fake app did not report a grandchild pid over the pipe");

        // The grandchild must actually be alive before we can prove it dies.
        QCOMPARE(::kill(grandchild, 0), 0);

        QVERIFY2(::kill(fakeApp, SIGKILL) == 0, "could not SIGKILL the fake app");
        int status = 0;
        ::waitpid(fakeApp, &status, 0); // reap it — it's our direct child

        // The grandchild is reparented (to init or a subreaper) once the fake
        // app dies, so it's never OUR child and can't be waitpid()'d — poll
        // kill(pid, 0) until it reports ESRCH instead.
        QTRY_VERIFY2_WITH_TIMEOUT(
            ::kill(grandchild, 0) == -1 && errno == ESRCH,
            "grandchild must be killed via PDEATHSIG when its immediate parent dies", 3000);
    }
};

QTEST_GUILESS_MAIN(TstProcessRunner)
#include "tst_processrunner.moc"
