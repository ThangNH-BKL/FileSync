// Cross-compiler regression for Windows min/max function-style macros.
// The previous std::numeric_limits<uint64_t>::max() failed under MSVC
// when a Windows header defined max(a,b).
#include <cstdint>
#include <limits>
#define max(a,b) (((a)>(b))?(a):(b))
static_assert(((std::numeric_limits<uint64_t>::max)()-32ull)/16ull > 1,
              "Windows max macro must not expand std::numeric_limits::max");
int main() { return 0; }
