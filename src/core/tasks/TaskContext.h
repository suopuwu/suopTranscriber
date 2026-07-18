#pragma once

#include <functional>

#include "TaskControl.h"

class EngineCache;

// Everything a Task's run() needs from its SHARED ENVIRONMENT, handed in by
// the TaskRunner on the worker thread. Per-task input (a file path, a model
// id, ...) is NOT here — it belongs on the Task subclass itself, exactly like
// the old worker slots took filePath/vaultPath/modelId as call arguments; here
// each Task instance just carries its own as constructor-injected members.
//
// A Task NEVER reaches back into the controller or any GUI-thread object — it
// talks to the world through this context (for shared resources) and through
// its own QObject signals (for progress/phase/results, which Qt delivers
// queued to the GUI thread). That one-way rule is what keeps run() thread-safe
// without locks.
class TaskContext {
public:
    TaskContext(EngineCache &engines, TaskControl &control, std::function<void(bool)> emitPaused)
        : engines(engines)
        , m_control(control)
        , m_emitPaused(std::move(emitPaused))
    {
    }

    // The ONE cooperative interruption point a BLOCKING pausable Task funnels
    // through (whisper.cpp's abort_callback, the diarization progress tick,
    // ffmpeg decode control all call it). Blocks — waking every 100ms to
    // re-check — while paused and not cancelled; returns true the instant
    // cancellation should take effect (whether requested before this call or
    // while blocked inside it). A Task opts into pausability by CALLING this
    // at safe points and DECLARING capabilities().pausable; a kind with no
    // interruption point simply never calls it (and reports pausable=false).
    bool checkpoint()
    {
        while (m_control.pauseRequested() && !m_control.cancelRequested()) {
            reportPaused(true);
            msleep100();
        }
        reportPaused(false);
        return m_control.cancelRequested();
    }

    // For a task that doesn't block at all (e.g. a download: ModelManager
    // polls control.paused()/cancelled() itself on every chunk and runs its
    // own wait loop) but still wants the "Paused" state to reach the UI —
    // call this instead of checkpoint() around your own poll. Fires the
    // paused(true)/paused(false) edge through m_emitPaused only on a real
    // transition, not once per poll (same discipline checkpoint() uses
    // internally, in fact checkpoint() is just this plus the sleep loop).
    void reportPaused(bool isPaused)
    {
        if (isPaused != m_pausedSignaled) {
            m_pausedSignaled = isPaused;
            m_emitPaused(isPaused);
        }
    }

    // Non-blocking probes for spots that can't afford to sleep (e.g. the
    // instant after a non-abortable model load, checked before starting the
    // decode) or that hand raw callbacks to lower-level code (ModelManager's
    // DownloadControl). Same semantics as the old isCancelRequested().
    bool cancelRequested() const { return m_control.cancelRequested(); }
    bool pauseRequested() const { return m_control.pauseRequested(); }

    EngineCache &engines;

private:
    // Out-of-line (defined in TaskContext.cpp) so this header stays free of
    // QThread — keeps it cheap to include from every Task subclass.
    static void msleep100();

    TaskControl &m_control;
    std::function<void(bool)> m_emitPaused;
    bool m_pausedSignaled = false;
};
