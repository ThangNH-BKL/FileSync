#include "ByteRangeDelta.h"
#include <algorithm>
#include <cstring>
#include <limits>
#include <unordered_map>

namespace fsyn {
namespace {
// Rsync-style rolling checksum modulo 2^16, with full-window byte order.
constexpr uint32_t mask = 0xffffu;
constexpr uint32_t kLiteralMax = 64u * 1024u;
constexpr uint32_t kCopyMax = 1024u * 1024u;
constexpr size_t kMaxOps = 2000000;
struct ReaderCache {
    uint64_t size;
    const ByteRead& read;
    uint64_t start = 0;
    uint32_t length = 0;
    bool failed = false;
    std::vector<uint8_t> buf;
    ReaderCache(uint64_t s, const ByteRead& r) : size(s), read(r), buf(256u * 1024u) {}
    uint8_t Get(uint64_t at) {
        if (at >= size) { failed = true; return 0; }
        if (at < start || at >= start + length) {
            start = at;
            length = (uint32_t)(std::min)(uint64_t(buf.size()), size - at);
            if (read(start, buf.data(), length) != length) { failed = true; return 0; }
        }
        return buf[(size_t)(at - start)];
    }
    bool Fill(uint64_t off, uint32_t n, std::vector<uint8_t>& out) {
        out.resize(n);
        for (uint32_t i = 0; i < n && !failed; ++i) out[i] = Get(off + i);
        return !failed;
    }
};
void PushOp(std::vector<ByteOp>& v, ByteOp::Kind kind,
            uint64_t dest, uint64_t source, uint64_t len) {
    const uint32_t cap = kind == ByteOp::Copy ? kCopyMax : kLiteralMax;
    while (len) {
        if (!v.empty()) {
            auto& tail = v.back();
            if (tail.kind == kind && tail.destination + tail.length == dest &&
                (kind == ByteOp::Literal || tail.source + tail.length == source) &&
                tail.length < cap) {
                uint32_t more = (uint32_t)(std::min)(len, uint64_t(cap - tail.length));
                tail.length += more;
                len -= more; dest += more; source += more;
                continue;
            }
        }
        uint32_t part = (uint32_t)(std::min)(len, uint64_t(cap));
        v.push_back({kind, dest, source, part});
        dest += part; source += part; len -= part;
    }
}
}
uint32_t SelectDeltaBlockSize(uint64_t baseSize) {
    // Bound signature memory to ~250k entries (~6MB), including large BIN files.
    uint32_t block = 64;
    while (block < 1024u * 1024u && baseSize / block > 250000u) block *= 2;
    return block;
}
uint32_t RollingWeak(const uint8_t* p, uint32_t n) {
    uint32_t a = 0, b = 0;
    for (uint32_t i = 0; i < n; ++i) {
        a = (a + p[i]) & mask;
        b = (b + (n - i) * (uint32_t)p[i]) & mask;
    }
    return (b << 16) | a;
}
uint32_t RollingNext(uint32_t value, uint8_t leaving, uint8_t entering, uint32_t n) {
    uint32_t a = ((value & mask) - leaving + entering) & mask;
    uint32_t b = (((value >> 16) & mask) - n * (uint32_t)leaving + a) & mask;
    return (b << 16) | a;
}
uint64_t Strong64(const uint8_t* bytes, uint32_t n) {
    uint64_t h = UINT64_C(14695981039346656037);
    for (uint32_t i = 0; i < n; ++i) {
        h ^= bytes[i]; h *= UINT64_C(1099511628211);
    }
    return h;
}
bool BuildByteSignatures(uint64_t baseSize, const ByteRead& read,
                         uint32_t blockSize, std::vector<ByteSignature>& result) {
    result.clear();
    if (!blockSize || blockSize > 1024u * 1024u) return false;
    std::vector<uint8_t> buf(blockSize);
    for (uint64_t off = 0; off < baseSize;) {
        uint32_t n = (uint32_t)(std::min)(uint64_t(blockSize), baseSize - off);
        if (read(off, buf.data(), n) != n) return false;
        result.push_back({off, n, RollingWeak(buf.data(), n), Strong64(buf.data(), n)});
        off += n;
        if (result.size() > 260000) return false;
    }
    return true;
}
bool BuildBytePlan(uint64_t newSize, const ByteRead& read,
                   uint32_t blockSize, const std::vector<ByteSignature>& signatures,
                   const std::vector<uint8_t>& prefix, std::vector<ByteOp>& result) {
    result.clear();
    if (!blockSize || blockSize > 1024u * 1024u) return false;
    ReaderCache src(newSize, read);
    std::unordered_multimap<uint32_t, size_t> index;
    index.reserve(signatures.size() * 2 + 1);
    for (size_t i = 0; i < signatures.size(); ++i)
        if (signatures[i].length == blockSize) index.emplace(signatures[i].weak, i);
    uint64_t pos = 0, literalStart = 0;
    // Byte-accurate prefix handling: particularly useful for a one-byte insertion
    // in a short text header ("Barcode" -> "Barcodee").
    if (!prefix.empty() && newSize) {
        uint64_t k = 0;
        const uint64_t common = (std::min)(newSize, uint64_t(prefix.size()));
        while (k < common && src.Get(k) == prefix[(size_t)k]) ++k;
        if (src.failed) return false;
        if (k) { PushOp(result, ByteOp::Copy, 0, 0, k); pos = k; literalStart = pos; }
        if (k < common) {
            unsigned bestCost = 1000; uint32_t bestNew = 0, bestOld = 0;
            // Discover small insertions/deletions/replacements within the prefix.
            for (uint32_t skipNew = 0; skipNew <= 8; ++skipNew) {
                for (uint32_t skipOld = 0; skipOld <= 8; ++skipOld) {
                    if (!skipNew && !skipOld) continue;
                    uint64_t ni = k + skipNew, oi = k + skipOld;
                    if (ni >= newSize || oi >= prefix.size()) continue;
                    uint32_t check = (uint32_t)(std::min)(uint64_t(8),
                        (std::min)(newSize - ni, uint64_t(prefix.size()) - oi));
                    if (check < 4) continue;
                    bool matches = true;
                    for (uint32_t j = 0; j < check; ++j) {
                        if (src.Get(ni+j) != prefix[(size_t)(oi+j)]) { matches = false; break; }
                    }
                    if (matches && skipNew + skipOld < bestCost) {
                        bestCost = skipNew + skipOld; bestNew = skipNew; bestOld = skipOld;
                    }
                }
            }
            if (src.failed) return false;
            if (bestCost != 1000) {
                if (bestNew) PushOp(result, ByteOp::Literal, pos, 0, bestNew);
                uint64_t copyLen = (std::min)(newSize - (pos + bestNew),
                                            uint64_t(prefix.size()) - (pos + bestOld));
                if (copyLen) PushOp(result, ByteOp::Copy, pos + bestNew, pos + bestOld, copyLen);
                pos += bestNew + copyLen;
                literalStart = pos;
            }
        }
    }
    // Streaming sliding window: load 256 KiB at a time; read source byte-wise
    // from cache, not through one ReadFile call per byte.
    std::vector<uint8_t> window;
    uint32_t weak = 0;
    bool haveWeak = false;
    while (pos + blockSize <= newSize) {
        if (!haveWeak) {
            if (!src.Fill(pos, blockSize, window)) return false;
            weak = RollingWeak(window.data(), blockSize);
            haveWeak = true;
        }
        bool matched = false;
        size_t chosen = signatures.size();
        // Prefer the next contiguous block, particularly important for BIN
        // files containing thousands of identical zero-filled blocks.
        // This avoids pathological O(N^2) scans and COPY op fragmentation.
        uint64_t contiguous = UINT64_MAX;
        if (!result.empty() && result.back().kind == ByteOp::Copy &&
            result.back().destination + result.back().length == pos)
            contiguous = result.back().source + result.back().length;
        bool strongComputed = false;
        uint64_t strong = 0;
        auto getStrong = [&]() {
            if (!strongComputed) { strong = Strong64(window.data(), blockSize); strongComputed=true; }
            return strong;
        };
        if (contiguous != UINT64_MAX && contiguous % blockSize == 0) {
            const uint64_t idx = contiguous / blockSize;
            if (idx < signatures.size() && signatures[(size_t)idx].length == blockSize &&
                signatures[(size_t)idx].sourceOffset == contiguous &&
                signatures[(size_t)idx].weak == weak &&
                signatures[(size_t)idx].strong == getStrong())
                chosen = (size_t)idx;
        }
        if (chosen == signatures.size() && !signatures.empty() &&
            signatures[0].length == blockSize && signatures[0].weak == weak &&
            signatures[0].strong == getStrong())
            chosen = 0; // common repeated/zero-filled BIN data
        if (chosen == signatures.size()) {
            auto range = index.equal_range(weak);
            for (auto it = range.first; it != range.second; ++it) {
                if (getStrong() == signatures[it->second].strong) {
                    chosen = it->second;
                    break;
                }
            }
        }
        if (chosen != signatures.size()) {
            if (pos > literalStart)
                PushOp(result, ByteOp::Literal, literalStart, 0, pos - literalStart);
            PushOp(result, ByteOp::Copy, pos, signatures[chosen].sourceOffset, blockSize);
            pos += blockSize;
            literalStart = pos;
            haveWeak = false;
            matched = true;
        }
        if (!matched) {
            uint8_t previous = window[0];
            ++pos;
            if (pos + blockSize <= newSize) {
                uint8_t next = src.Get(pos + blockSize - 1);
                if (src.failed) return false;
                std::memmove(window.data(), window.data() + 1, blockSize - 1);
                window[blockSize - 1] = next;
                weak = RollingNext(weak, previous, next, blockSize);
            } else haveWeak = false;
        }
        if (result.size() > kMaxOps) return false;
    }
    // Reuse an unchanged short final base block even after an insertion shifts
    // all offsets. Without this, up to blockSize-1 tail bytes are retransmitted.
    if (pos < newSize) {
        uint64_t remain = newSize - pos;
        bool tailMatched = false;
        if (remain < blockSize) {
            std::vector<uint8_t> tail;
            if (!src.Fill(pos, (uint32_t)remain, tail)) return false;
            for (const auto& sig : signatures) {
                if (sig.length == remain &&
                    sig.weak == RollingWeak(tail.data(), (uint32_t)remain) &&
                    sig.strong == Strong64(tail.data(), (uint32_t)remain)) {
                    if (pos > literalStart)
                        PushOp(result, ByteOp::Literal, literalStart, 0, pos-literalStart);
                    PushOp(result, ByteOp::Copy, pos, sig.sourceOffset, remain);
                    tailMatched = true;
                    break;
                }
            }
        }
        if (!tailMatched) PushOp(result, ByteOp::Literal, literalStart, 0, newSize-literalStart);
    }
    if (src.failed) return false;
    uint64_t end = 0;
    for (const auto& op : result) {
        if (op.destination != end || !op.length) return false;
        end += op.length;
    }
    return end == newSize;
}
} // namespace fsyn
