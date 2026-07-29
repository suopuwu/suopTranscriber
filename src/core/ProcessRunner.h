#pragma once

#include <QProcess>

#include <csignal>
#include <functional>
#include <sys/prctl.h>
#include <sys/types.h>
#include <unistd.h>

// The pause/cancel poll loop shared by every in-app subprocess that shells out
// to a long-running child (ffmpeg decode/concat, curl model download). The
// SIGSTOP-on-pause / SIGCONT-on-resume / kill-on-cancel / poll-every-100ms
// skeleton was copy-pasted character-for-character across AudioDecoder,
// ModelManager and CombineTask; this single-sources the one piece that's subtle
// enough to get wrong (the pause-transition edge detection and the kill/drain
// ordering). Header-only so it needs no per-test dependency wiring.
//
// Linux-only (SIGSTOP/SIGCONT) — Linux is the app's primary target. Does NOT
// cover SubprocessEngine: that path pauses cooperatively via its onAbort
// callback (no SIGSTOP), a different model that doesn't fit here.
namespace ProcessRunner {

// Cooperative pause/cancel signals + an optional pause-state notification. Any
// of the std::functions may be empty (treated as "never cancelled / never
// paused / nothing to notify").
struct PauseCancelControl {
    std::function<bool()> cancelled;          // true => kill the child and stop
    std::function<bool()> paused;             // true while the child should be suspended
    std::function<void(bool)> onPauseChanged; // called on each pause<->resume transition
    std::function<bool()> stalled;            // true => kill the child and report Stalled
};

enum class RunOutcome { Finished, Cancelled, Stalled };

// Drives an already-started `proc` to completion. Every ~100ms it: checks
// cancel (kill + return Cancelled), applies SIGSTOP/SIGCONT on pause
// transitions, then calls onPoll() (drain stdout / report progress — runs every
// iteration, including while paused, so a child that back-pressures on a full
// stdout pipe still gets drained), then checks the opt-in `stalled` predicate
// (kill + return Stalled). Returns Finished once the child exits on its own.
// `stalled` is empty by default, so existing callers are unaffected. The
// caller owns the exit-code / final-drain / result handling afterward.
inline RunOutcome runPausable(QProcess &proc, const PauseCancelControl &control,
                              const std::function<void()> &onPoll)
{
    bool wasPaused = false;
    while (proc.state() != QProcess::NotRunning) {
        if (control.cancelled && control.cancelled()) {
            proc.kill();
            proc.waitForFinished(2000);
            return RunOutcome::Cancelled;
        }
        const bool shouldPause = control.paused && control.paused();
        if (shouldPause && !wasPaused) {
            ::kill(static_cast<pid_t>(proc.processId()), SIGSTOP);
            wasPaused = true;
            if (control.onPauseChanged)
                control.onPauseChanged(true);
        } else if (!shouldPause && wasPaused) {
            ::kill(static_cast<pid_t>(proc.processId()), SIGCONT);
            wasPaused = false;
            if (control.onPauseChanged)
                control.onPauseChanged(false);
        }
        proc.waitForReadyRead(100);
        if (onPoll)
            onPoll();
        // A child that neither exits nor produces output would otherwise spin
        // here forever — SIGKILL, not terminate(): a wedged ffmpeg provably
        // ignores SIGTERM (an f32 stream containing NaN spins inside the
        // filter graph and never reaches its signal check).
        if (control.stalled && control.stalled()) {
            proc.kill();
            proc.waitForFinished(2000);
            return RunOutcome::Stalled;
        }
    }
    return RunOutcome::Finished;
}

// Ties the child's lifetime to ours at the KERNEL level, so a crash, a
// SIGKILL, or a force-quit can't leave it running: PR_SET_PDEATHSIG fires
// even when no destructor ever gets to run. SIGKILL, not SIGTERM — a wedged
// ffmpeg provably ignores SIGTERM (measured: 27h orphans that only SIGKILL
// could end).
//
// PDEATHSIG is armed against the spawning THREAD, not the process — it fires
// the moment that thread exits, even if other threads live on. Every current
// call site is synchronous (the spawning function waits for the child, via
// runPausable or a plain waitForFinished, within the same call on the same
// thread), so the thread always outlives the child and this is safe to call
// unconditionally before start(). A future spawn site that returns while
// deliberately leaving the child running in the background must NOT use this
// guard — the child would be killed the instant its spawning thread next
// exits, not when the caller actually wants it gone.
inline void guardChildLifetime(QProcess &proc)
{
    proc.setChildProcessModifier([] {
        ::prctl(PR_SET_PDEATHSIG, SIGKILL);
        // PDEATHSIG is armed relative to the parent as it is RIGHT NOW; if the
        // parent already died in the window between fork and here, the signal
        // would never arrive, so re-check and exit rather than linger.
        if (::getppid() == 1)
            ::_exit(1);
    });
}

} // namespace ProcessRunner
