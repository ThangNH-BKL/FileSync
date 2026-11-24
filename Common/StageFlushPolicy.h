#pragma once
// Only a decision helper: FINAL stage Flush() and SHA-256 are still mandatory.
#include <cstdint>
namespace fsyn {
inline bool ShouldFlushStageLiteral(uint64_t unflushedBytes, uint64_t thresholdBytes) {
    return thresholdBytes==0 || unflushedBytes>=thresholdBytes;
}
} // namespace fsyn
