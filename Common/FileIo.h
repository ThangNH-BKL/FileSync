#pragma once
// FileIo.h — Đọc/ghi file theo block 64KB, share mode READ|WRITE|DELETE, retry 3x1s.
// RAII cho HANDLE. Không nạp cả file vào RAM.

#include <windows.h>
#include <string>
#include <cstdint>
#include <memory>
#include <vector>

namespace fsyn {

// RAII cho HANDLE
struct HandleCloser {
    void operator()(HANDLE h) const { if (h && h != INVALID_HANDLE_VALUE) CloseHandle(h); }
};
using UniqueHandle = std::unique_ptr<void, HandleCloser>;

class FileIo {
public:
    FileIo() = default;
    ~FileIo() { Close(); }
    FileIo(const FileIo&) = delete;
    FileIo& operator=(const FileIo&) = delete;

    // Mở file để đọc (share READ|WRITE|DELETE).
    bool OpenForRead(const std::wstring& path);

    // Mở file để ghi tuần tự/random, tạo mới nếu chưa có, share READ|WRITE|DELETE.
    // Không truncate mặc định — caller quyết định qua SetEndOfFile.
    bool OpenForWrite(const std::wstring& path, bool createIfMissing = true);

    void Close();

    bool IsOpen() const { return h_ != nullptr && h_ != INVALID_HANDLE_VALUE; }

    uint64_t Size() const;  // 0 nếu lỗi

    // Đọc tối đa `len` byte tại offset; trả số byte đọc thực (0 = EOF, (uint64_t)-1 = lỗi).
    uint64_t ReadAt(uint64_t offset, void* buf, uint32_t len);

    // Ghi đủ `len` byte tại offset; trả số byte ghi được ((uint64_t)-1 = lỗi).
    uint64_t WriteAt(uint64_t offset, const void* buf, uint32_t len);

    // SetEndOfFile tại vị trí `size`.
    bool TruncateTo(uint64_t size);

    // Đảm bảo dữ liệu đã xuống đĩa.
    bool Flush();

    // Lấy mtime (unix ns). false nếu lỗi.
    bool GetMtimeNs(int64_t& outNs);

    // ---- Static helpers ----
    // Đọc toàn bộ file thành vector<uint8_t> (dùng cho test nhỏ; KHÔNG dùng cho file lớn).
    static bool ReadAll(const std::wstring& path, std::vector<uint8_t>& out);

    // Ghi toàn bộ buffer vào file (tạo mới/truncate).
    static bool WriteAll(const std::wstring& path, const void* data, size_t len);

    // Xóa file nếu tồn tại.
    static bool DeleteFileQuiet(const std::wstring& path);

    // Đảm bảo thư mục cha của `path` tồn tại (tạo đệ quy).
    static bool EnsureParentDir(const std::wstring& path);

    // MoveFileEx với REPLACE_EXISTING|WRITE_THROUGH (retry 3x1s nếu bị lock).
    static bool AtomicReplace(const std::wstring& src, const std::wstring& dst);

private:
    HANDLE h_ = INVALID_HANDLE_VALUE;
    std::wstring path_;
};

} // namespace fsyn
