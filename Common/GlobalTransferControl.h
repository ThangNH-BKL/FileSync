#pragma once
// Shared among ServerEngine's ClientSessions: admission of active file
// transfers and receive-side TCP backpressure. No SQLite/protocol change.
// This is not an exact network-wire QoS guarantee; router shaping is required
// for a strict LAN cap. Control/heartbeat frames are never delayed here.
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <deque>
#include <unordered_set>
#include <algorithm>
#include "PerfDiagnostics.h"
#include "AdaptiveServerPolicy.h"

namespace fsyn {
class GlobalTransferControl {
public:
    explicit GlobalTransferControl(unsigned activeLimit=12,uint64_t bytesPerSec=4ull*1024*1024,
                                   uint64_t spoolBudget=64ull*1024*1024*1024,
                                   bool adaptive=false,uint64_t maximum=10ull*1024*1024)
        : limit_((std::max)(1u,activeLimit)),
          rate_(bytesPerSec),
          spoolBudget_(spoolBudget),
          last_(std::chrono::steady_clock::now()),
          credit_(static_cast<double>(bytesPerSec)),
          adaptive_(adaptive), policy_(1024ull*1024,maximum,bytesPerSec) {
        if(adaptive_) rate_=policy_.Rate();
    }
    bool TryEnter() {
        std::lock_guard<std::mutex> lock(mu_);
        if(active_>=limit_) return false;
        ++active_;
        return true;
    }
    void Leave() {
        std::lock_guard<std::mutex> lock(mu_);
        if(active_>0) --active_;
    }
    // FIFO fairness among sessions that have an eligible pending file.
    bool TryEnter(const void* session) {
        if(!session) return false;
        std::lock_guard<std::mutex> lock(mu_);
        if(owners_.count(session)) return true;
        if(queued_.insert(session).second) waiters_.push_back(session);
        if(active_>=limit_ || waiters_.empty() || waiters_.front()!=session) return false;
        waiters_.pop_front(); queued_.erase(session);
        owners_.insert(session);
        ++active_;
        return true;
    }
    void Leave(const void* session) {
        std::lock_guard<std::mutex> lock(mu_);
        if(owners_.erase(session) && active_>0) --active_;
    }
    void Cancel(const void* session) {
        std::lock_guard<std::mutex> lock(mu_);
        if(owners_.erase(session) && active_>0) --active_;
        if(queued_.erase(session)) {
            auto it=std::find(waiters_.begin(),waiters_.end(),session);
            if(it!=waiters_.end()) waiters_.erase(it);
        }
    }
    // Conservative best-effort disk inventory budget shared across sessions.
    // Reserve BEFORE writing each snapshot frame; refund when spool is deleted.
    bool ReserveSpool(uint64_t n) {
        std::lock_guard<std::mutex> lock(mu_);
        if(n>spoolBudget_ || spoolUsed_>spoolBudget_-n) return false;
        spoolUsed_+=n;return true;
    }
    void ReleaseSpool(uint64_t n) {
        std::lock_guard<std::mutex> lock(mu_);
        spoolUsed_=spoolUsed_>=n?spoolUsed_-n:0;
    }
    uint64_t SpoolUsed() const {std::lock_guard<std::mutex> lock(mu_);return spoolUsed_;}
    unsigned Active() const {std::lock_guard<std::mutex> lock(mu_); return active_;}
    unsigned Limit() const {return limit_;}
    uint64_t BytesPerSecond() const {std::lock_guard<std::mutex> g(mu_);return rate_;}
    bool Adaptive() const {return adaptive_;}
    void ObserveStageFlush(uint64_t elapsedUs) {
        stageFlushUs_.fetch_add(elapsedUs,std::memory_order_relaxed);
        stageFlushCount_.fetch_add(1,std::memory_order_relaxed);
    }
    // Called by ServerEngine's 1-second monitor thread, not by recv workers.
    void UpdateAdaptive(double otherFraction) {
        if(!adaptive_) return;
        AdaptiveServerPolicy::Sample sample{};
        sample.externalFraction=otherFraction;
        sample.flushUs=stageFlushUs_.exchange(0,std::memory_order_relaxed);
        sample.flushCount=stageFlushCount_.exchange(0,std::memory_order_relaxed);
        std::lock_guard<std::mutex> lock(mu_);
        sample.activeTransfers=active_;
        rate_=policy_.Step(sample);
        credit_=(std::min)(credit_,static_cast<double>(rate_));
    }
    uint64_t FlushCount() const {return stageFlushCount_.load(std::memory_order_relaxed);}
    uint64_t ReceivedBytes() const {return pacedBytes_.load(std::memory_order_relaxed);}
    uint64_t PaceWaitUs() const {return pacedWaitUs_.load(std::memory_order_relaxed);}
    // Call after receiving a data payload, before consuming the next data
    // frame. Deliberately reserves credit for all sessions from one bucket.
    bool PaceReceived(uint64_t bytes, const std::atomic<bool>& stopped) {
        if(bytes==0) return true;
        const bool trace=perf::Enabled();
        const auto entered=perf::Clock::now();
        for(;;) {
            if(stopped.load()) return false;
            std::chrono::milliseconds pause{10};
            {
                std::lock_guard<std::mutex> lock(mu_);
                const auto now=std::chrono::steady_clock::now();
                const double elapsed=std::chrono::duration<double>(now-last_).count();
                last_=now;
                if(rate_==0) return true;
                // Allow a single oversized payload eventually to pass.
                const double bucketCap=(std::max)(static_cast<double>(rate_),static_cast<double>(bytes));
                credit_=(std::min)(bucketCap,credit_+elapsed*rate_);
                if(credit_>=bytes) {
                    credit_-=bytes;
                    // Always track received payload for network self-traffic subtraction.
                    pacedBytes_.fetch_add(bytes,std::memory_order_relaxed);
                    if(trace) pacedWaitUs_.fetch_add(perf::Us(entered,perf::Clock::now()),std::memory_order_relaxed);
                    return true;
                }
                double wait=(static_cast<double>(bytes)-credit_)/rate_;
                pause=std::chrono::milliseconds(static_cast<unsigned>((std::min)(200.0,(std::max)(5.0,wait*1000))));
            }
            std::this_thread::sleep_for(pause);
        }
    }
private:
    mutable std::mutex mu_;
    unsigned limit_;
    uint64_t rate_;
    unsigned active_=0;
    std::deque<const void*> waiters_;
    std::unordered_set<const void*> queued_, owners_;
    uint64_t spoolBudget_=0, spoolUsed_=0;
    std::chrono::steady_clock::time_point last_;
    double credit_;
    bool adaptive_=false;
    AdaptiveServerPolicy policy_;
    std::atomic<uint64_t> stageFlushUs_{0},stageFlushCount_{0};
    std::atomic<uint64_t> pacedBytes_{0},pacedWaitUs_{0};
};
} // namespace fsyn
