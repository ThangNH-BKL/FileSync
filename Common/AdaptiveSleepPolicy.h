#pragma once
// Keep the 40%-of-link client policy unchanged, reduce avoidable 50ms oversleep.
// Rounded to 1..20ms for portable bounded stop responsiveness.
#include <algorithm>
#include <chrono>
namespace fsyn {
inline std::chrono::milliseconds AdaptiveWaitSlice(std::chrono::steady_clock::duration remaining) {
    auto ms=std::chrono::duration_cast<std::chrono::milliseconds>(remaining).count();
    return std::chrono::milliseconds((std::max)(1ll,(std::min)(20ll,static_cast<long long>(ms))));
}
} // namespace fsyn
