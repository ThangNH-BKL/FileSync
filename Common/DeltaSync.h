#pragma once
// DeltaSync.h — So sánh block 64KB bằng CRC32 + SHA-256 tổng.
// Cung cấp IBlockReader để test mô phỏng offset 64-bit mà không cần file >4GB thật.

#include <windows.h>
#include <cstdint>
#include <vector>
#include <string>
#include <memory>

namespace fsyn {

// Giao diện đọc block — cho phép inject trong test.
class IBlockReader {
public:
    virtual ~IBlockReader() = default;
    virtual uint64_t Size() const = 0;
    // Đọc tối đa `len` byte tại offset; trả số byte đọc được ((uint64_t)-1 = lỗi).
    virtual uint64_t ReadAt(uint64_t offset, void* buf, uint32_t len) = 0;
};

// Impl dùng FileIo
class FileBlockReader : public IBlockReader {
public:
    explicit FileBlockReader(const std::wstring& path);
    ~FileBlockReader() override;
    uint64_t Size() const override;
    uint64_t ReadAt(uint64_t offset, void* buf, uint32_t len) override;

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};

struct BlockEntry {
    uint64_t offset;
    uint32_t crc32;
    uint32_t size;   // kích thước block thực (block cuối có thể < 64KB)
};

// Tính CRC32 các block 64KB của file; tối đa `maxBytes` byte đầu (0 = toàn bộ).
// Trả false nếu đọc lỗi.
bool ComputeBlockHashes(IBlockReader& reader,
                        std::vector<BlockEntry>& out,
                        uint64_t maxBytes = 0);

// Tính SHA-256 toàn file (streaming, buffer 64KB).
bool ComputeFileSha256(IBlockReader& reader, uint8_t out[32]);

// Quyết định các offset cần client gửi:
//   - clientHashes: các block của client (đã compute).
//   - serverReader: đọc file server hiện có (có thể null nếu serverSize == 0).
//   - serverSize: kích thước file server.
//   - clientSize: kích thước file client.
//   - truncate: (out) true nếu client nhỏ hơn server → cần SetEndOfFile.
//   - needOffsets: (out) danh sách offset cần gửi.
// Trả false nếu đọc server lỗi.
bool DecideNeedOffsets(const std::vector<BlockEntry>& clientHashes,
                       IBlockReader* serverReader,
                       uint64_t serverSize,
                       uint64_t clientSize,
                       bool& truncate,
                       std::vector<uint64_t>& needOffsets);

} // namespace fsyn
