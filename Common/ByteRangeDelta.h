#pragma once
// V5: rolling weak checksums + a strong 64-bit candidate check.
// SHA-256 of the reconstructed COMPLETE FILE is the final integrity check.
// Independent of Windows/MFC so it can be tested on Linux and Windows.
#include <cstdint>
#include <cstddef>
#include <functional>
#include <vector>

namespace fsyn {
struct ByteSignature {
    uint64_t sourceOffset = 0;
    uint32_t length = 0;
    uint32_t weak = 0;
    uint64_t strong = 0;
};
struct ByteOp {
    enum Kind : uint8_t { Copy = 1, Literal = 2 } kind = Literal;
    uint64_t destination = 0;
    uint64_t source = 0;  // base file offset for Copy; ignored for Literal
    uint32_t length = 0;
};
using ByteRead = std::function<uint64_t(uint64_t, void*, uint32_t)>;
uint32_t SelectDeltaBlockSize(uint64_t baseSize);
uint32_t RollingWeak(const uint8_t* bytes, uint32_t length);
uint32_t RollingNext(uint32_t weak, uint8_t leaving, uint8_t entering, uint32_t length);
uint64_t Strong64(const uint8_t* bytes, uint32_t length);
bool BuildByteSignatures(uint64_t baseSize, const ByteRead& read,
                         uint32_t blockSize, std::vector<ByteSignature>& result);
bool BuildBytePlan(uint64_t newSize, const ByteRead& read,
                   uint32_t blockSize, const std::vector<ByteSignature>& signatures,
                   const std::vector<uint8_t>& oldPrefix, std::vector<ByteOp>& result);
} // namespace fsyn
