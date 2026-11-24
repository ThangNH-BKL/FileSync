#include "pch.h"
#include "Protocol.h"
#include "DeltaSync.h"
#include "FileIo.h"
#include "Crypto.h"

namespace fsyn {

// ---------- FileBlockReader ----------
struct FileBlockReader::Impl {
    FileIo fio;
};

FileBlockReader::FileBlockReader(const std::wstring& path)
    : p_(std::make_unique<Impl>()) {
    p_->fio.OpenForRead(path);
}
FileBlockReader::~FileBlockReader() = default;

uint64_t FileBlockReader::Size() const { return p_->fio.Size(); }

uint64_t FileBlockReader::ReadAt(uint64_t offset, void* buf, uint32_t len) {
    return p_->fio.ReadAt(offset, buf, len);
}

// ---------- ComputeBlockHashes ----------
bool ComputeBlockHashes(IBlockReader& reader,
                        std::vector<BlockEntry>& out,
                        uint64_t maxBytes) {
    out.clear();
    uint64_t size = reader.Size();
    if (maxBytes > 0 && maxBytes < size) size = maxBytes;

    std::vector<uint8_t> buf(kBlockSize);
    uint64_t off = 0;
    while (off < size) {
        uint64_t remain  = size - off;
        uint32_t toRead  = (uint32_t)(remain < kBlockSize ? remain : kBlockSize);

        uint64_t got = reader.ReadAt(off, buf.data(), toRead);
        if (got == (uint64_t)-1) return false;
        if (got < toRead) {
            // File ngắn hơn dự kiến — coi như lỗi để tránh so sai.
            return false;
        }

        BlockEntry b;
        b.offset = off;
        b.size   = (uint32_t)got;
        b.crc32  = Crc32(buf.data(), (size_t)got);
        out.push_back(b);
        off += got;
    }
    return true;
}

// ---------- ComputeFileSha256 ----------
bool ComputeFileSha256(IBlockReader& reader, uint8_t out[32]) {
    Sha256 h;
    if (!h.IsValid()) return false;

    uint64_t size = reader.Size();
    std::vector<uint8_t> buf(kBlockSize);
    uint64_t off = 0;
    while (off < size) {
        uint64_t remain = size - off;
        uint32_t toRead = (uint32_t)(remain < kBlockSize ? remain : kBlockSize);
        uint64_t got = reader.ReadAt(off, buf.data(), toRead);
        if (got == (uint64_t)-1 || got < toRead) return false;
        if (!h.Update(buf.data(), (size_t)got)) return false;
        off += got;
    }
    return h.Final(out);
}

// ---------- DecideNeedOffsets ----------
bool DecideNeedOffsets(const std::vector<BlockEntry>& clientHashes,
                       IBlockReader* serverReader,
                       uint64_t serverSize,
                       uint64_t clientSize,
                       bool& truncate,
                       std::vector<uint64_t>& needOffsets) {
    needOffsets.clear();
    truncate = (clientSize < serverSize);

    // Block cuối client có thể ngắn; số block theo clientSize.
    uint64_t clientBlockCount = clientHashes.size();
    uint64_t serverBlockCount = (serverSize + kBlockSize - 1) / kBlockSize;

    // 1) Nếu server rỗng → gửi toàn bộ (tất cả các offset block của client)
    if (serverSize == 0) {
        for (const auto& b : clientHashes) needOffsets.push_back(b.offset);
        return true;
    }

    // 2) Đọc song song từng block của server để so CRC32 với client.
    //    Block nào lệch / server chưa có → thêm vào needOffsets.
    std::vector<uint8_t> buf(kBlockSize);
    for (uint64_t i = 0; i < clientBlockCount; ++i) {
        const BlockEntry& c = clientHashes[i];
        bool need = true;

        if (i < serverBlockCount) {
            uint64_t remain = serverSize - c.offset;
            uint32_t toRead = (uint32_t)(remain < kBlockSize ? remain : kBlockSize);
            // Chỉ so khi kích thước block server cũng bằng kích thước block client
            // (nếu khác, coi như lệch để an toàn)
            if (toRead == c.size && serverReader) {
                uint64_t got = serverReader->ReadAt(c.offset, buf.data(), toRead);
                if (got == (uint64_t)-1) return false;
                if (got == toRead) {
                    uint32_t serverCrc = Crc32(buf.data(), (size_t)toRead);
                    if (serverCrc == c.crc32) need = false;
                }
            }
        }
        if (need) needOffsets.push_back(c.offset);
    }

    // 3) Nếu clientSize < serverSize → truncate = true (đã set ở đầu)
    //    Nếu clientSize > serverSize → các block ngoài serverBlockCount cũng đã được
    //    thêm vào needOffsets (vì i >= serverBlockCount → need = true).
    return true;
}

} // namespace fsyn
