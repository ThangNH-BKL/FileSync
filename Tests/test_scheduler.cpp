#include "framework.h"
#include "MiniTest.h"
#include "Scheduler.h"
#include <chrono>
#include <ctime>

using namespace fsyn;
using SysClock = std::chrono::system_clock;

namespace {
SysClock::time_point MakeLocal(int hour, int minute) {
    std::time_t tt = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &tt);
    tm.tm_hour = hour; tm.tm_min = minute; tm.tm_sec = 0;
    std::time_t shifted = std::mktime(&tm);
    return SysClock::from_time_t(shifted);
}
}

TEST(sched_window_inside) {
    SchedConfig c;
    c.mode = SchedMode::Window;
    c.windowStartMin = 8 * 60;   // 08:00
    c.windowEndMin   = 17 * 60;  // 17:00
    Scheduler s(c);
    s.SetNowFn([]{ return MakeLocal(10, 0); });
    EXPECT_TRUE(s.IsInWindow());

    s.SetNowFn([]{ return MakeLocal(7, 59); });
    EXPECT_FALSE(s.IsInWindow());

    s.SetNowFn([]{ return MakeLocal(17, 0); });
    EXPECT_FALSE(s.IsInWindow());  // end exclusive
}

TEST(sched_window_overnight) {
    SchedConfig c;
    c.mode = SchedMode::Window;
    c.windowStartMin = 22 * 60;  // 22:00
    c.windowEndMin   = 6  * 60;  // 06:00
    Scheduler s(c);

    s.SetNowFn([]{ return MakeLocal(23, 0); });
    EXPECT_TRUE(s.IsInWindow());

    s.SetNowFn([]{ return MakeLocal(2, 0); });
    EXPECT_TRUE(s.IsInWindow());   // 02:00 vẫn trong khung

    s.SetNowFn([]{ return MakeLocal(5, 59); });
    EXPECT_TRUE(s.IsInWindow());

    s.SetNowFn([]{ return MakeLocal(6, 0); });
    EXPECT_FALSE(s.IsInWindow());  // biên end

    s.SetNowFn([]{ return MakeLocal(12, 0); });
    EXPECT_FALSE(s.IsInWindow());
}

TEST(sched_all_day) {
    SchedConfig c;
    c.mode = SchedMode::Window;
    c.windowStartMin = 0;
    c.windowEndMin = 0;
    Scheduler s(c);
    s.SetNowFn([]{ return MakeLocal(3, 0); });
    EXPECT_TRUE(s.IsInWindow());
}

TEST(sched_parse_hhmm) {
    int m = 0;
    EXPECT_TRUE(Scheduler::ParseHHMM("22:30", m));
    EXPECT_EQ(m, 22 * 60 + 30);
    EXPECT_TRUE(Scheduler::ParseHHMM("00:00", m));
    EXPECT_EQ(m, 0);
    EXPECT_FALSE(Scheduler::ParseHHMM("25:00", m));
    EXPECT_FALSE(Scheduler::ParseHHMM("bad", m));
}
