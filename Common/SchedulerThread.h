#pragma once
// SchedulerThread.h — tick 1s, cập nhật in-window state cho mọi session.

#include "Scheduler.h"
#include <atomic>
#include <thread>
#include <functional>

namespace fsyn {

class SchedulerThread {
public:
    using OnFlipFn = std::function<void(bool inWindow)>;

    SchedulerThread(Scheduler* sched, OnFlipFn onFlip);
    ~SchedulerThread();

    void Start();
    void Stop();
    bool LastInWindow() const { return lastIn_.load(); }

private:
    void ThreadProc();

    Scheduler*          sched_;
    OnFlipFn            onFlip_;
    std::thread         th_;
    std::atomic<bool>   stop_{false};
    std::atomic<bool>   lastIn_{false};
};

} // namespace fsyn
