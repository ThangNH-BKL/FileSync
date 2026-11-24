#include "framework.h"
#include "MiniTest.h"
#include <exception>

namespace mt {
int g_failures = 0;
int g_checks   = 0;

std::vector<TestCase>& Registry() {
    static std::vector<TestCase> r;
    return r;
}

int RunAll() {
    int failedTests = 0;
    for (const auto& tc : Registry()) {
        int before = g_failures;
        std::printf("[RUN ] %s\n", tc.name);
        try {
            tc.fn();
        } catch (const FatalAssertion&) {
            // Failed REQUIRE_* has already recorded its own failure.
        } catch (const std::exception& ex) {
            ++g_failures;
            std::printf("  EXCEPTION %s: %s\n", tc.name, ex.what());
        } catch (...) {
            ++g_failures;
            std::printf("  EXCEPTION %s: unknown C++ exception\n", tc.name);
        }
        if (g_failures > before) { ++failedTests; std::printf("[FAIL] %s\n", tc.name); }
        else                     { std::printf("[ OK ] %s\n", tc.name); }
    }
    std::printf("\n==== %d test(s), %d check(s), %d failure(s) ====\n",
                (int)Registry().size(), g_checks, g_failures);
    return failedTests == 0 ? 0 : 1;
}
}
