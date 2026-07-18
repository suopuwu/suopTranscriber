#pragma once

#include <QProcess>

#include <csignal>
#include <functional>
#include <sys/types.h>

// The pause/cancel poll loop shared by every in-app subprocess that shells out
// to a long-running child (ffmpeg decode/concat, curl model download). The
// SIGSTOP-on-pause / SIGCONT-on-resume / kill-on-cancel / poll-every-100ms
// skeleton was copy-pasted character-for-character across AudioDecoder,
// ModelManager and CombineTask; this single-sources the one piece that's subtle
// enough to get wrong (the pause-transition edge detection and the kill/drain
// ordering). Header-only so it needs no per-test dependency wiring.
//
// Linux-only (SIGSTOP/SIGCONT), matching CLAUDE.md's primary target. Does NOT
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
};

enum class RunOutcome { Finished, Cancelled };

// Drives an already-started `proc` to completion. Every ~100ms it: checks
// cancel (kill + return Cancelled), applies SIGSTOP/SIGCONT on pause
// transitions, then calls onPoll() (drain stdout / report progress — runs every
// iteration, including while paused, so a child that back-pressures on a full
// stdout pipe still gets drained). Returns Finished once the child exits on its
// own. The caller owns the exit-code / final-drain / result handling afterward.
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
    }
    return RunOutcome::Finished;
}

} // namespace ProcessRunner
