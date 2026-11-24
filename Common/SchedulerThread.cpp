#include "pch.h"
#include "SchedulerThread.h"

namespace fsyn {

SchedulerThread::SchedulerThread(Scheduler* sched, OnFlipFn onFlip)
    : sched_(sched), onFlip_(std::move(onFlip)) {}

SchedulerThread::~SchedulerThread() { Stop(); }

void SchedulerThread::Start() {
    stop_.store(false);
    // Khởi tạo lastIn_ theo trạng thái hiện tại, KHÔNG gọi callback
    lastIn_.store(sched_->IsInWindow());
    th_ = std::thread(&SchedulerThread::ThreadProc, this);
}

void SchedulerThread::Stop() {
    stop_.store(true);
    if (th_.joinable()) th_.join();
}

void SchedulerThread::ThreadProc() {
    while (!stop_.load()) {
        // Chờ 1s hoặc đến khi stop
        for (int i = 0; i < 10 && !stop_.load(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (stop_.load()) break;

        bool now = sched_->IsInWindow();
        bool prev = lastIn_.exchange(now);
        if (prev != now) {
            if (onFlip_) onFlip_(now);
        }
    }
}

} // namespace fsyn
