#include "pch.h"
#include "Protocol.h"

namespace fsyn {
namespace {

// ---------------- Writer ----------------
void WU8(std::vector<uint8_t>& v, uint8_t x) { v.push_back(x); }
void WU16(std::vector<uint8_t>& v, uint16_t x) {
    v.push_back((uint8_t)(x & 0xFF));
    v.push_back((uint8_t)((x >> 8) & 0xFF));
}
void WU32(std::vector<uint8_t>& v, uint32_t x) {
    for (int i = 0; i < 4; ++i) v.push_back((uint8_t)((x >> (8 * i)) & 0xFF));
}
void WU64(std::vector<uint8_t>& v, uint64_t x) {
    for (int i = 0; i < 8; ++i) v.push_back((uint8_t)((x >> (8 * i)) & 0xFF));
}
void WI64(std::vector<uint8_t>& v, int64_t x) { WU64(v, (uint64_t)x); }

bool WStr(std::vector<uint8_t>& v, const std::string& s, uint16_t maxLen) {
    if (s.size() > maxLen) return false;
    WU16(v, (uint16_t)s.size());
    v.insert(v.end(), s.begin(), s.end());
    return true;
}

// ---------------- Reader ----------------
struct R {
    const uint8_t* p;
    uint32_t       n;
    uint32_t       pos = 0;
    bool           ok  = true;

    bool Need(uint32_t k) {
        if (pos + k > n) { ok = false; return false; }
        return true;
    }
    uint8_t  U8()  { if (!Need(1)) return 0; return p[pos++]; }
    uint16_t U16() { if (!Need(2)) return 0; uint16_t v = (uint16_t)p[pos] | ((uint16_t)p[pos + 1] << 8); pos += 2; return v; }
    uint32_t U32() { if (!Need(4)) return 0; uint32_t v = 0; for (int i = 0; i < 4; ++i) v |= ((uint32_t)p[pos + i]) << (8 * i); pos += 4; return v; }
    uint64_t U64() { if (!Need(8)) return 0; uint64_t v = 0; for (int i = 0; i < 8; ++i) v |= ((uint64_t)p[pos + i]) << (8 * i); pos += 8; return v; }
    int64_t  I64() { return (int64_t)U64(); }

    std::string Str(uint16_t maxLen) {
        uint16_t len = U16();
        if (!ok) return {};
        if (len > maxLen) { ok = false; return {}; }
        if (!Need(len)) return {};
        std::string s((const char*)(p + pos), len);
        pos += len;
        return s;
    }
    bool Tail() { return ok && pos == n; }
};

} // namespace

// ============================ HELLO ============================
bool Marshal(const HelloMsg& m, std::vector<uint8_t>& out) {
    out.clear();
    if (!WStr(out, m.hostname,  kMaxHostnameLen)) return false;
    if (!WStr(out, m.ip,        kMaxIpLen))       return false;
    if (!WStr(out, m.watchRoot, kMaxRelPathLen))  return false;
    if (!WStr(out, m.token,     kMaxTokenLen))    return false;
    if (!WStr(out, m.exeFolder, kMaxHostnameLen)) return false;
    return out.size() <= kMaxPayload;
}
bool Unmarshal(const uint8_t* p, uint32_t n, HelloMsg& out) {
    R r{ p, n };
    out.hostname  = r.Str(kMaxHostnameLen);
    out.ip        = r.Str(kMaxIpLen);
    out.watchRoot = r.Str(kMaxRelPathLen);
    out.token     = r.Str(kMaxTokenLen);
    out.exeFolder = r.Str(kMaxHostnameLen);
    return r.Tail();
}

// ============================ HELLO_ACK ============================
bool Marshal(const HelloAckMsg& m, std::vector<uint8_t>& out) {
    out.clear();
    WU8(out, m.ok);
    WI64(out, m.serverTimeMs);
    return true;
}
bool Unmarshal(const uint8_t* p, uint32_t n, HelloAckMsg& out) {
    R r{ p, n };
    out.ok           = r.U8();
    out.serverTimeMs = r.I64();
    return r.Tail();
}

// ============================ TREE_SNAPSHOT ============================
bool Marshal(const std::vector<TreeEntry>& v, std::vector<uint8_t>& out) {
    out.clear();
    if (v.size() > 0xFFFFFFFFull) return false;
    WU32(out, (uint32_t)v.size());
    for (const auto& e : v) {
        if (!WStr(out, e.relPath, kMaxRelPathLen)) return false;
        WU64(out, e.size);
        WI64(out, e.mtimeNs);
        if (!WStr(out, e.instanceId, 40)) return false;
        if (out.size() > kMaxPayload) return false;
    }
    return out.size() <= kMaxPayload;
}
bool Unmarshal(const uint8_t* p, uint32_t n, std::vector<TreeEntry>& out) {
    out.clear();
    R r{ p, n };
    uint32_t cnt = r.U32();
    if (!r.ok) return false;
    // Mỗi entry tối thiểu 2+0+8+8 = 18 byte → chặn trước khi cấp phát
    if ((uint64_t)cnt * 18ull > (uint64_t)n) return false;
    out.reserve(cnt);
    for (uint32_t i = 0; i < cnt; ++i) {
        TreeEntry e;
        e.relPath = r.Str(kMaxRelPathLen);
        e.size    = r.U64();
        e.mtimeNs = r.I64();
        e.instanceId = r.Str(40);
        if (!r.ok) return false;
        out.push_back(std::move(e));
    }
    return r.Tail();
}

// ============================ FILE_EVENT ============================
bool Marshal(const FileEventMsg& m, std::vector<uint8_t>& out) {
    out.clear();
    WU8(out, (uint8_t)m.action);
    if (!WStr(out, m.relPath, kMaxRelPathLen)) return false;
    WU64(out, m.size);
    WI64(out, m.mtimeNs);
    if (m.action == FileAction::Renamed) {
        if (!WStr(out, m.newRelPath, kMaxRelPathLen)) return false;
    }
    if (!WStr(out, m.instanceId, 40)) return false;
    return out.size() <= kMaxPayload;
}
bool Unmarshal(const uint8_t* p, uint32_t n, FileEventMsg& out) {
    R r{ p, n };
    uint8_t a = r.U8();
    if (a < 1 || a > 4) return false;
    out.action = (FileAction)a;
    out.relPath = r.Str(kMaxRelPathLen);
    out.size    = r.U64();
    out.mtimeNs = r.I64();
    if (out.action == FileAction::Renamed) {
        out.newRelPath = r.Str(kMaxRelPathLen);
    }
    out.instanceId = r.Str(40);
    return r.Tail();
}

// ============================ SYNC_REQUEST_INIT ============================
bool Marshal(const SyncRequestInitMsg& m, std::vector<uint8_t>& out) {
    out.clear();
    if (!WStr(out, m.relPath, kMaxRelPathLen)) return false;
    WU64(out, m.serverSize);
    return true;
}
bool Unmarshal(const uint8_t* p, uint32_t n, SyncRequestInitMsg& out) {
    R r{ p, n };
    out.relPath    = r.Str(kMaxRelPathLen);
    out.serverSize = r.U64();
    return r.Tail();
}

// ============================ BLOCK_HASHES ============================
bool Marshal(const BlockHashesMsg& m, std::vector<uint8_t>& out) {
    out.clear();
    if (!WStr(out, m.relPath, kMaxRelPathLen)) return false;
    if (m.blocks.size() > 0xFFFFFFFFull) return false;
    WU32(out, (uint32_t)m.blocks.size());
    for (const auto& b : m.blocks) {
        WU64(out, b.offset);
        WU32(out, b.crc32);
        WU32(out, b.length);
        if (out.size() > kMaxPayload) return false;
    }
    return out.size() <= kMaxPayload;
}
bool Unmarshal(const uint8_t* p, uint32_t n, BlockHashesMsg& out) {
    out.blocks.clear();
    R r{ p, n };
    out.relPath = r.Str(kMaxRelPathLen);
    uint32_t cnt = r.U32();
    if (!r.ok) return false;
    if ((uint64_t)cnt * 16ull > (uint64_t)n) return false;
    out.blocks.reserve(cnt);
    for (uint32_t i = 0; i < cnt; ++i) {
        BlockHashEntry b;
        b.offset = r.U64();
        b.crc32  = r.U32();
        b.length = r.U32();
        if (!r.ok) return false;
        out.blocks.push_back(b);
    }
    return r.Tail();
}

// ============================ SYNC_REQUEST_BLOCKS ============================
bool Marshal(const SyncRequestBlocksMsg& m, std::vector<uint8_t>& out) {
    out.clear();
    if (!WStr(out, m.relPath, kMaxRelPathLen)) return false;
    WU64(out, m.serverSize);
    WU8(out, m.truncate);
    if (m.needOffsets.size() > 0xFFFFFFFFull) return false;
    WU32(out, (uint32_t)m.needOffsets.size());
    for (uint64_t o : m.needOffsets) {
        WU64(out, o);
        if (out.size() > kMaxPayload) return false;
    }
    return out.size() <= kMaxPayload;
}
bool Unmarshal(const uint8_t* p, uint32_t n, SyncRequestBlocksMsg& out) {
    out.needOffsets.clear();
    R r{ p, n };
    out.relPath    = r.Str(kMaxRelPathLen);
    out.serverSize = r.U64();
    out.truncate   = r.U8();
    uint32_t cnt   = r.U32();
    if (!r.ok) return false;
    if ((uint64_t)cnt * 8ull > (uint64_t)n) return false;
    out.needOffsets.reserve(cnt);
    for (uint32_t i = 0; i < cnt; ++i) {
        uint64_t o = r.U64();
        if (!r.ok) return false;
        out.needOffsets.push_back(o);
    }
    return r.Tail();
}

// ============================ FILE_CHUNK ============================
bool Marshal(const FileChunkMsg& m, std::vector<uint8_t>& out) {
    out.clear();
    if (m.data.size() > kMaxChunkData) return false;
    if (!WStr(out, m.relPath, kMaxRelPathLen)) return false;
    WU64(out, m.offset);
    WU32(out, (uint32_t)m.data.size());
    out.insert(out.end(), m.data.begin(), m.data.end());
    return out.size() <= kMaxPayload;
}
bool Unmarshal(const uint8_t* p, uint32_t n, FileChunkMsg& out) {
    out.data.clear();
    R r{ p, n };
    out.relPath = r.Str(kMaxRelPathLen);
    out.offset  = r.U64();
    uint32_t dl = r.U32();
    if (!r.ok) return false;
    if (dl > kMaxChunkData) return false;
    if (!r.Need(dl)) return false;
    out.data.assign(p + r.pos, p + r.pos + dl);
    r.pos += dl;
    return r.Tail();
}

// ============================ FILE_DONE ============================
bool Marshal(const FileDoneMsg& m, std::vector<uint8_t>& out) {
    out.clear();
    if (!WStr(out, m.relPath, kMaxRelPathLen)) return false;
    WU64(out, m.finalSize);
    out.insert(out.end(), m.sha256, m.sha256 + 32);
    return true;
}
bool Unmarshal(const uint8_t* p, uint32_t n, FileDoneMsg& out) {
    R r{ p, n };
    out.relPath   = r.Str(kMaxRelPathLen);
    out.finalSize = r.U64();
    if (!r.Need(32)) return false;
    std::memcpy(out.sha256, p + r.pos, 32);
    r.pos += 32;
    return r.Tail();
}

// ============================ ERROR_MSG ============================
bool Marshal(const ErrorMsg& m, std::vector<uint8_t>& out) {
    out.clear();
    WU8(out, (uint8_t)m.code);
    if (!WStr(out, m.message, kMaxErrMsgLen)) return false;
    return true;
}
bool Unmarshal(const uint8_t* p, uint32_t n, ErrorMsg& out) {
    R r{ p, n };
    uint8_t c = r.U8();
    if (c < 1 || c > 6) return false;
    out.code    = (ErrorCode)c;
    out.message = r.Str(kMaxErrMsgLen);
    return r.Tail();
}

// ============================ RESYNC ============================
bool Marshal(const ResyncMsg& m, std::vector<uint8_t>& out) {
    out.clear();
    return WStr(out, m.relPath, kMaxRelPathLen);
}
bool Unmarshal(const uint8_t* p, uint32_t n, ResyncMsg& out) {
    R r{ p, n };
    out.relPath = r.Str(kMaxRelPathLen);
    return r.Tail();
}

} // namespace fsyn
