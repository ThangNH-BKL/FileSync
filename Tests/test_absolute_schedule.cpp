#include "framework.h"
#include "MiniTest.h"
#include "AbsoluteSchedule.h"
#include <chrono>

using namespace fsyn;

TEST(absolute_schedule_before_at_and_after) {
    using Clock = AbsoluteScheduleWindow::Clock;
    const auto anchor = Clock::time_point{};
    AbsoluteScheduleWindow window{ anchor + std::chrono::seconds(10),
                                    anchor + std::chrono::seconds(20) };
    EXPECT_EQ(window.PhaseAt(anchor), AbsoluteSchedulePhase::Waiting);
    EXPECT_EQ(window.PhaseAt(anchor + std::chrono::seconds(10)), AbsoluteSchedulePhase::Running);
    EXPECT_EQ(window.PhaseAt(anchor + std::chrono::seconds(19)), AbsoluteSchedulePhase::Running);
    EXPECT_EQ(window.PhaseAt(anchor + std::chrono::seconds(20)), AbsoluteSchedulePhase::Finished);
}

TEST(absolute_schedule_invalid_window) {
    using Clock = AbsoluteScheduleWindow::Clock;
    AbsoluteScheduleWindow equal{Clock::time_point{}, Clock::time_point{}};
    EXPECT_FALSE(equal.IsValid());
    EXPECT_EQ(equal.PhaseAt(Clock::now()), AbsoluteSchedulePhase::Invalid);
}
