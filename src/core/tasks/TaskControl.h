#pragma once

#include <atomic>

// The pause/cancel control surface for one running task, shared across the
// GUI thread (which sets requests) and the worker thread (which reads them
// via TaskContext::checkpoint()). Two plain atomics — deliberately the ONLY
// mutable state touched cross-thread, exactly as the old TranscriptionWorker's
// control atomics were: set requests must land immediately, never queue behind
// the job currently running on the worker's event loop (that is the moment a
// pause/cancel most needs to take effect).
//
// One TaskControl belongs to the currently-Active task. reset() is called by
// the scheduler right before dispatching the next task so a pause/cancel left
// over from the previous task never leaks into this one.
class TaskControl {
public:
    void requestPause() { m_pauseRequested.store(true); }
    void requestResume() { m_pauseRequested.store(false); }
    void requestCancel() { m_cancelRequested.store(true); }

    void reset()
    {
        m_pauseRequested.store(false);
        m_cancelRequested.store(false);
    }

    bool pauseRequested() const { return m_pauseRequested.load(); }
    bool cancelRequested() const { return m_cancelRequested.load(); }

private:
    std::atomic<bool> m_pauseRequested{false};
    std::atomic<bool> m_cancelRequested{false};
};
