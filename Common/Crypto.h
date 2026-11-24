#pragma once
// Crypto.h — CRC32 (bảng) và SHA-256 (qua BCrypt/CNG).
// SHA-256 hỗ trợ streaming (Update nhiều lần) để không nạp cả file vào RAM.

#include <cstdint>
#include <vector>
#include <string>
#include <memory>

namespace fsyn {

// ---------------- CRC32 ----------------
// Bảng tra dựng 1 lần (thread-safe nhờ local static).
uint32_t Crc32(const void* data, size_t len);
uint32_t Crc32Update(uint32_t crc, const void* data, size_t len); // crc = 0 ban đầu
inline uint32_t Crc32Init() { return 0; }

// ---------------- SHA-256 ----------------
class Sha256 {
public:
    Sha256();
    ~Sha256();
    Sha256(const Sha256&) = delete;
    Sha256& operator=(const Sha256&) = delete;

    bool IsValid() const { return valid_; }
    bool Update(const void* data, size_t len);
    bool Final(uint8_t out[32]);

    // Tiện ích: hash toàn bộ buffer 1 lần
    static bool HashBuffer(const void* data, size_t len, uint8_t out[32]);

private:
    bool                valid_ = false;
    void*               hAlg_  = nullptr;   // BCRYPT_ALG_HANDLE
    void*               hHash_ = nullptr;   // BCRYPT_HASH_HANDLE
    std::vector<uint8_t> hashObj_;
};

// Helper: hex string của SHA-256 (để log)
std::string Sha256ToHex(const uint8_t hash[32]);

} // namespace fsyn
