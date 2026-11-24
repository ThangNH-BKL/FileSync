#pragma once
#include "ByteRangeDelta.h"
#include "Protocol.h"
#include <array>
#include <string>
#include <vector>
namespace fsyn {
struct V5SignaturePage {
    std::string path;
    std::string expectedInstanceId;
    uint64_t baseSize = 0;
    uint32_t blockSize = 0;
    std::array<uint8_t,32> baseSha{}; // all zero when no committed base exists
    std::vector<uint8_t> prefix;
    uint32_t firstIndex = 0;
    bool last = false;
    std::vector<ByteSignature> items;
};
struct V5Begin {
    std::string path;
    uint64_t size = 0;
    std::array<uint8_t,32> targetSha{};
    std::array<uint8_t,32> baseSha{};
};
struct V5Copy {
    std::string path;
    uint64_t dest = 0;
    uint64_t source = 0;
    uint32_t length = 0;
};
struct V5Check {
    std::string path;
    uint64_t dest = 0;
    uint32_t length = 0;
    std::array<uint8_t,32> sha{};
};
struct V5Need {
    std::string path;
    uint64_t dest = 0;
    bool need = false;
};
struct V5Bytes {
    std::string path;
    uint64_t dest = 0;
    std::vector<uint8_t> data;
};
bool Pack(const V5SignaturePage&, std::vector<uint8_t>&);
bool Unpack(const uint8_t*, uint32_t, V5SignaturePage&);
bool Pack(const V5Begin&, std::vector<uint8_t>&);
bool Unpack(const uint8_t*, uint32_t, V5Begin&);
bool Pack(const V5Copy&, std::vector<uint8_t>&);
bool Unpack(const uint8_t*, uint32_t, V5Copy&);
bool Pack(const V5Check&, std::vector<uint8_t>&);
bool Unpack(const uint8_t*, uint32_t, V5Check&);
bool Pack(const V5Need&, std::vector<uint8_t>&);
bool Unpack(const uint8_t*, uint32_t, V5Need&);
bool Pack(const V5Bytes&, std::vector<uint8_t>&);
bool Unpack(const uint8_t*, uint32_t, V5Bytes&);
} // namespace fsyn
