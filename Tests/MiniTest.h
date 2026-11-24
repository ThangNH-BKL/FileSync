#pragma once
#include <functional>
#include <vector>
#include <string>
#include <cstdio>
#include <exception>

namespace mt {
struct FatalAssertion final {};
struct TestCase { const char* name; std::function<void()> fn; };
std::vector<TestCase>& Registry();
extern int g_failures;
extern int g_checks;
int RunAll();
}

#define TEST(name) \
    static void test_##name(); \
    static struct Reg_##name { Reg_##name() { \
        mt::Registry().push_back({#name, test_##name}); } } reg_##name; \
    static void test_##name()

#define EXPECT_TRUE(x) do { ++mt::g_checks; if (!(x)) { ++mt::g_failures; \
    std::printf("  FAIL %s:%d: EXPECT_TRUE(%s)\n", __FILE__, __LINE__, #x); } } while(0)
#define EXPECT_FALSE(x) EXPECT_TRUE(!(x))
#define EXPECT_EQ(a,b) do { ++mt::g_checks; auto _a=(a); auto _b=(b); if (!(_a==_b)) { ++mt::g_failures; \
    std::printf("  FAIL %s:%d: EXPECT_EQ(%s, %s)\n", __FILE__, __LINE__, #a, #b); } } while(0)
#define EXPECT_NE(a,b) do { ++mt::g_checks; auto _a=(a); auto _b=(b); if (_a==_b) { ++mt::g_failures; \
    std::printf("  FAIL %s:%d: EXPECT_NE\n", __FILE__, __LINE__); } } while(0)

// Fatal preconditions: Unlike EXPECT_*, a failed REQUIRE_* stops the
// individual test before it can index an empty vector or use bad handles.
#define REQUIRE_TRUE(x) do { ++mt::g_checks; if (!(x)) { ++mt::g_failures; \
    std::printf("  FAIL %s:%d: REQUIRE_TRUE(%s)\n", __FILE__, __LINE__, #x); \
    throw mt::FatalAssertion{}; } } while(0)
#define REQUIRE_EQ(a,b) do { ++mt::g_checks; auto _a=(a); auto _b=(b); if (!(_a==_b)) { \
    ++mt::g_failures; std::printf("  FAIL %s:%d: REQUIRE_EQ(%s, %s)\n", __FILE__, __LINE__, #a, #b); \
    throw mt::FatalAssertion{}; } } while(0)
