#pragma once
// V8.3 opt-in diagnostics. Disabled by default; no protocol or durability change.
#include <chrono>
#include <cstdlib>
#include <cstdint>
namespace fsyn { namespace perf {
inline bool EnvironmentOne(const char* name) {
#ifdef _MSC_VER
    // MSVC: avoid C4996 security warning from getenv().
    char* value=nullptr;
    size_t length=0;
    const errno_t error=_dupenv_s(&value,&length,name);
    const bool enabled=(error==0 && value && length==2 &&
                        value[0]=='1' && value[1]=='\0');
    std::free(value);
    return enabled;
#else
    const char* value=std::getenv(name);
    return value && value[0]=='1' && value[1]=='\0';
#endif
}
inline bool Enabled() {
    static const bool enabled=EnvironmentOne("FILESYNC_PERF_METRICS");
    return enabled;
}
inline bool QuietSuccess() {
    static const bool quiet=Enabled() && EnvironmentOne("FILESYNC_PERF_QUIET_SUCCESS");
    return quiet;
}
using Clock=std::chrono::steady_clock;
inline uint64_t Us(Clock::time_point a,Clock::time_point b) {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(b-a).count());
}
}} // fsyn::perf
