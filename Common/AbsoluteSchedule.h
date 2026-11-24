#pragma once
// One-shot local-date/time schedule policy, independent of MFC and Windows.
// The UI converts local date/time controls into system_clock time_points.
#include <chrono>

namespace fsyn {

enum class AbsoluteSchedulePhase { Invalid, Waiting, Running, Finished };

struct AbsoluteScheduleWindow {
    using Clock = std::chrono::system_clock;
    Clock::time_point start{};
    Clock::time_point end{};

    bool IsValid() const noexcept { return end > start; }
    AbsoluteSchedulePhase PhaseAt(Clock::time_point now) const noexcept {
        if (!IsValid()) return AbsoluteSchedulePhase::Invalid;
        if (now >= end) return AbsoluteSchedulePhase::Finished;
        if (now < start) return AbsoluteSchedulePhase::Waiting;
        return AbsoluteSchedulePhase::Running;
    }
};

} // namespace fsyn
