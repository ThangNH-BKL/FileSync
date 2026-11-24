#pragma once
// Read-only diagnostics. Never coalesces or removes a durable flush.
#include <atomic>
#include <chrono>
#include <cstdint>
namespace fsyn { namespace durability {
using Clock=std::chrono::steady_clock;
inline uint64_t Us(Clock::time_point start) {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now()-start).count());
}
struct Snapshot {uint64_t baselineCount,baselineUs,manifestCount,manifestUs;};
inline std::atomic<uint64_t>& BaselineCount(){static std::atomic<uint64_t> a{0};return a;}
inline std::atomic<uint64_t>& BaselineTime(){static std::atomic<uint64_t> a{0};return a;}
inline std::atomic<uint64_t>& ManifestCount(){static std::atomic<uint64_t> a{0};return a;}
inline std::atomic<uint64_t>& ManifestTime(){static std::atomic<uint64_t> a{0};return a;}
inline void RecordBaseline(uint64_t us){BaselineCount().fetch_add(1,std::memory_order_relaxed);BaselineTime().fetch_add(us,std::memory_order_relaxed);}
inline void RecordManifest(uint64_t us){ManifestCount().fetch_add(1,std::memory_order_relaxed);ManifestTime().fetch_add(us,std::memory_order_relaxed);}
inline Snapshot Read(){return {BaselineCount().load(),BaselineTime().load(),ManifestCount().load(),ManifestTime().load()};}
}} // namespace fsyn::durability
