#include "pch.h"
#include "Scheduler.h"
#include <cstdio>

namespace fsyn {

Scheduler::Scheduler(const SchedConfig& cfg)
    : cfg_(cfg), nowFn_([]{ return std::chrono::system_clock::now(); }) {
    cycleEpoch_ = nowFn_();
}

bool Scheduler::ParseHHMM(const char* s, int& minutesOut) {
    if (!s) return false;
    int h = 0, m = 0;
    if (::sscanf_s(s, "%d:%d", &h, &m) != 2) return false;
    if (h < 0 || h > 23 || m < 0 || m > 59) return false;
    minutesOut = h * 60 + m;
    return true;
}

void Scheduler::FormatHHMM(int minutesInDay, char out[6]) {
    if (minutesInDay < 0) minutesInDay = 0;
    minutesInDay %= 1440;
    int h = minutesInDay / 60;
    int m = minutesInDay % 60;
    std::snprintf(out, 6, "%02d:%02d", h, m);
}

bool Scheduler::IsInWindow(TimePoint now) const {
    if (cfg_.mode == SchedMode::Cycle) return IsInWindowCycle(now);
    return IsInWindowFixed(now);
}

bool Scheduler::IsInWindowCycle(TimePoint now) const {
    // Đơn vị chu kỳ
    uint64_t periodSec = (uint64_t)cfg_.cycleInterval
                       * (cfg_.cycleUnit == 1 ? 3600ull : 60ull);
    if (periodSec == 0) periodSec = 60;

    uint64_t durSec = (uint64_t)cfg_.cycleDurationSec;
    if (durSec == 0) return true;
    if (durSec >= periodSec) return true;  // luôn trong cửa sổ

    // Thời gian trôi từ cycleEpoch_
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - cycleEpoch_).count();
    if (elapsed < 0) elapsed = 0;
    uint64_t phase = (uint64_t)elapsed % periodSec;
    return phase < durSec;
}

bool Scheduler::IsInWindowFixed(TimePoint now) const {
    // Lấy giờ:phút địa phương từ time_point
    std::time_t tt = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    localtime_s(&tm, &tt);
    int curMin = tm.tm_hour * 60 + tm.tm_min;

    int s = cfg_.windowStartMin;
    int e = cfg_.windowEndMin;

    if (s == e) return true;              // khung cả ngày
    if (s < e)  return curMin >= s && curMin < e;   // cùng ngày
    // Qua nửa đêm
    return curMin >= s || curMin < e;
}

} // namespace fsyn
