#pragma once
// Scheduler.h — Logic thuần (không UI) cho 2 chế độ lịch:
//   1. Cycle: cứ N (phút/giờ) mở cửa sổ DurationSec giây rồi đóng.
//   2. Window: khung giờ cố định Start-End (hỗ trợ qua nửa đêm).
// Clock injectable để test.

#include <chrono>
#include <functional>

namespace fsyn {

enum class SchedMode { Cycle, Window };

struct SchedConfig {
    SchedMode mode = SchedMode::Window;

    // Cycle:
    int cycleUnit        = 0;     // 0 = phút, 1 = giờ
    int cycleInterval    = 30;    // mỗi 30 phút/giờ
    int cycleDurationSec = 120;   // cửa sổ kéo dài 120 giây

    // Window (phút trong ngày 0..1439):
    int windowStartMin   = 22 * 60;   // 22:00
    int windowEndMin     = 6  * 60;   // 06:00
};

class Scheduler {
public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = std::chrono::time_point<std::chrono::system_clock>;

    explicit Scheduler(const SchedConfig& cfg);

    // Clock mặc định: system_clock::now. Test có thể inject.
    using NowFn = std::function<TimePoint()>;
    void SetNowFn(NowFn fn) { nowFn_ = std::move(fn); }

    // Trả true nếu thời điểm `now` nằm trong cửa sổ đồng bộ.
    bool IsInWindow(TimePoint now) const;
    bool IsInWindow() const { return IsInWindow(nowFn_()); }

    SchedConfig Config() const { return cfg_; }
    void SetConfig(const SchedConfig& c) { cfg_ = c; }

    // Tiện ích: parse/format "HH:MM"
    static bool ParseHHMM(const char* s, int& minutesOut);
    static void FormatHHMM(int minutesInDay, char out[6]);

private:
    bool IsInWindowCycle(TimePoint now) const;
    bool IsInWindowFixed(TimePoint now) const;

    SchedConfig cfg_;
    NowFn       nowFn_;
    // Cache điểm bắt đầu cycle (epoch) để tránh trôi
    TimePoint   cycleEpoch_;
};

} // namespace fsyn
