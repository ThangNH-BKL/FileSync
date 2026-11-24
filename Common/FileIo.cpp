#include "pch.h"
#include "FileIo.h"
#include "Util.h"
#include <shlwapi.h>
#pragma comment(lib, "Shlwapi.lib")

namespace fsyn {
namespace {

constexpr int    kRetryCount = 3;
constexpr DWORD  kRetrySleepMs = 1000;

bool IsRetryableIoError(DWORD e) {
    return e == ERROR_SHARING_VIOLATION
        || e == ERROR_LOCK_VIOLATION
        || e == ERROR_ACCESS_DENIED;
}

} // namespace

bool FileIo::OpenForRead(const std::wstring& path) {
    Close();
    for (int i = 0; i < kRetryCount; ++i) {
        HANDLE h = CreateFileW(path.c_str(), GENERIC_READ,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               nullptr, OPEN_EXISTING,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            h_ = h;
            path_ = path;
            return true;
        }
        DWORD e = GetLastError();
        if (!IsRetryableIoError(e) || i == kRetryCount - 1) return false;
        Sleep(kRetrySleepMs);
    }
    return false;
}

bool FileIo::OpenForWrite(const std::wstring& path, bool createIfMissing) {
    Close();
    DWORD disp = createIfMissing ? OPEN_ALWAYS : OPEN_EXISTING;
    for (int i = 0; i < kRetryCount; ++i) {
        HANDLE h = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               nullptr, disp,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            h_ = h;
            path_ = path;
            return true;
        }
        DWORD e = GetLastError();
        if (!IsRetryableIoError(e) || i == kRetryCount - 1) return false;
        Sleep(kRetrySleepMs);
    }
    return false;
}

void FileIo::Close() {
    if (h_ != INVALID_HANDLE_VALUE && h_ != nullptr) {
        CloseHandle(h_);
    }
    h_ = INVALID_HANDLE_VALUE;
    path_.clear();
}

uint64_t FileIo::Size() const {
    if (!IsOpen()) return 0;
    LARGE_INTEGER li{};
    if (!GetFileSizeEx(h_, &li)) return 0;
    return (uint64_t)li.QuadPart;
}

uint64_t FileIo::ReadAt(uint64_t offset, void* buf, uint32_t len) {
    if (!IsOpen() || len == 0) return 0;

    OVERLAPPED ov{};
    ov.Offset     = (DWORD)(offset & 0xFFFFFFFFull);
    ov.OffsetHigh = (DWORD)(offset >> 32);

    DWORD got = 0;
    for (int i = 0; i < kRetryCount; ++i) {
        if (::ReadFile(h_, buf, len, &got, &ov)) return got;
        DWORD e = GetLastError();
        if (e == ERROR_HANDLE_EOF) return got;
        if (!IsRetryableIoError(e) || i == kRetryCount - 1) return (uint64_t)-1;
        Sleep(kRetrySleepMs);
        ov.Offset     = (DWORD)(offset & 0xFFFFFFFFull);
        ov.OffsetHigh = (DWORD)(offset >> 32);
    }
    return (uint64_t)-1;
}

uint64_t FileIo::WriteAt(uint64_t offset, const void* buf, uint32_t len) {
    if (!IsOpen() || len == 0) return 0;

    OVERLAPPED ov{};
    ov.Offset     = (DWORD)(offset & 0xFFFFFFFFull);
    ov.OffsetHigh = (DWORD)(offset >> 32);

    DWORD written = 0;
    for (int i = 0; i < kRetryCount; ++i) {
        if (::WriteFile(h_, buf, len, &written, &ov)) return written;
        DWORD e = GetLastError();
        if (!IsRetryableIoError(e) || i == kRetryCount - 1) return (uint64_t)-1;
        Sleep(kRetrySleepMs);
        ov.Offset     = (DWORD)(offset & 0xFFFFFFFFull);
        ov.OffsetHigh = (DWORD)(offset >> 32);
    }
    return (uint64_t)-1;
}

bool FileIo::TruncateTo(uint64_t size) {
    if (!IsOpen()) return false;
    LARGE_INTEGER li{};
    li.QuadPart = (LONGLONG)size;
    if (!SetFilePointerEx(h_, li, nullptr, FILE_BEGIN)) return false;
    return SetEndOfFile(h_) != 0;
}

bool FileIo::Flush() {
    if (!IsOpen()) return false;
    return FlushFileBuffers(h_) != 0;
}

bool FileIo::GetMtimeNs(int64_t& outNs) {
    if (!IsOpen()) return false;
    FILETIME ft{};
    if (!GetFileTime(h_, nullptr, nullptr, &ft)) return false;

    ULARGE_INTEGER u;
    u.LowPart  = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;

    // FILETIME = 100ns từ 1601-01-01. Unix epoch = 116444736000000000 (100ns).
    constexpr uint64_t kEpochDiff100ns = 116444736000000000ull;
    if (u.QuadPart < kEpochDiff100ns) { outNs = 0; return true; }
    outNs = (int64_t)((u.QuadPart - kEpochDiff100ns) * 100ull);
    return true;
}

bool FileIo::ReadAll(const std::wstring& path, std::vector<uint8_t>& out) {
    FileIo f;
    if (!f.OpenForRead(path)) return false;
    uint64_t sz = f.Size();
    if (sz > (uint64_t)0x7FFFFFFFull) return false;  // >2GB: chặn cho helper này
    out.resize((size_t)sz);
    if (sz == 0) return true;
    uint64_t r = f.ReadAt(0, out.data(), (uint32_t)sz);
    return r == sz;
}

bool FileIo::WriteAll(const std::wstring& path, const void* data, size_t len) {
    FileIo f;
    if (!f.OpenForWrite(path, true)) return false;
    if (!f.TruncateTo(0)) return false;
    if (len > 0) {
        uint64_t w = f.WriteAt(0, data, (uint32_t)len);
        if (w != len) return false;
    }
    f.Flush();
    return true;
}

bool FileIo::DeleteFileQuiet(const std::wstring& path) {
    for (int i = 0; i < kRetryCount; ++i) {
        if (DeleteFileW(path.c_str())) return true;
        DWORD e = GetLastError();
        if (e == ERROR_FILE_NOT_FOUND) return true;
        if (!IsRetryableIoError(e) || i == kRetryCount - 1) return false;
        Sleep(kRetrySleepMs);
    }
    return false;
}

bool FileIo::EnsureParentDir(const std::wstring& path) {
    size_t p = path.find_last_of(L"\\/");
    if (p == std::wstring::npos) return true;
    std::wstring dir = path.substr(0, p);
    if (dir.empty()) return true;

    // Tạo đệ quy: nếu dir cha chưa có, gọi trước
    DWORD attr = GetFileAttributesW(dir.c_str());
    if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) return true;

    if (!EnsureParentDir(dir)) return false;
    if (CreateDirectoryW(dir.c_str(), nullptr)) return true;
    DWORD e = GetLastError();
    return e == ERROR_ALREADY_EXISTS;
}

bool FileIo::AtomicReplace(const std::wstring& src, const std::wstring& dst) {
    for (int i = 0; i < kRetryCount; ++i) {
        if (MoveFileExW(src.c_str(), dst.c_str(),
                        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            return true;
        }
        DWORD e = GetLastError();
        if (!IsRetryableIoError(e) || i == kRetryCount - 1) return false;
        Sleep(kRetrySleepMs);
    }
    return false;
}

} // namespace fsyn
