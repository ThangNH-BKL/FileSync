#include "pch.h"
#include "Crypto.h"
#include <bcrypt.h>

#pragma comment(lib, "Bcrypt.lib")

namespace fsyn {

// ================= CRC32 =================
namespace {
struct Crc32Table {
    uint32_t t[256];
    Crc32Table() {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) {
                c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            }
            t[i] = c;
        }
    }
};
const Crc32Table& GetTable() {
    static Crc32Table tbl;   // magic static: thread-safe trong C++17
    return tbl;
}
} // namespace

uint32_t Crc32Update(uint32_t crc, const void* data, size_t len) {
    const Crc32Table& T = GetTable();
    const uint8_t* p = (const uint8_t*)data;
    crc = ~crc;
    while (len--) crc = T.t[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

uint32_t Crc32(const void* data, size_t len) {
    return Crc32Update(0, data, len);
}

// ================= SHA-256 =================
Sha256::Sha256() {
    BCRYPT_ALG_HANDLE hAlg = nullptr;
    NTSTATUS st = BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
    if (!BCRYPT_SUCCESS(st)) return;

    DWORD objLen = 0, cb = 0;
    st = BCryptGetProperty(hAlg, BCRYPT_OBJECT_LENGTH,
                           (PUCHAR)&objLen, sizeof(objLen), &cb, 0);
    if (!BCRYPT_SUCCESS(st)) { BCryptCloseAlgorithmProvider(hAlg, 0); return; }

    hashObj_.assign(objLen, 0);

    BCRYPT_HASH_HANDLE hHash = nullptr;
    st = BCryptCreateHash(hAlg, &hHash, hashObj_.data(), objLen, nullptr, 0, 0);
    if (!BCRYPT_SUCCESS(st)) { BCryptCloseAlgorithmProvider(hAlg, 0); return; }

    hAlg_  = hAlg;
    hHash_ = hHash;
    valid_ = true;
}

Sha256::~Sha256() {
    if (hHash_) BCryptDestroyHash((BCRYPT_HASH_HANDLE)hHash_);
    if (hAlg_)  BCryptCloseAlgorithmProvider((BCRYPT_ALG_HANDLE)hAlg_, 0);
}

bool Sha256::Update(const void* data, size_t len) {
    if (!valid_) return false;
    NTSTATUS st = BCryptHashData((BCRYPT_HASH_HANDLE)hHash_,
                                 (PUCHAR)data, (ULONG)len, 0);
    return BCRYPT_SUCCESS(st);
}

bool Sha256::Final(uint8_t out[32]) {
    if (!valid_) return false;
    NTSTATUS st = BCryptFinishHash((BCRYPT_HASH_HANDLE)hHash_, out, 32, 0);
    valid_ = false;  // hash đã kết thúc
    return BCRYPT_SUCCESS(st);
}

bool Sha256::HashBuffer(const void* data, size_t len, uint8_t out[32]) {
    Sha256 h;
    if (!h.IsValid()) return false;
    if (!h.Update(data, len)) return false;
    return h.Final(out);
}

std::string Sha256ToHex(const uint8_t hash[32]) {
    static const char* hex = "0123456789abcdef";
    std::string s;
    s.resize(64);
    for (int i = 0; i < 32; ++i) {
        s[i * 2]     = hex[(hash[i] >> 4) & 0xF];
        s[i * 2 + 1] = hex[hash[i] & 0xF];
    }
    return s;
}

} // namespace fsyn
