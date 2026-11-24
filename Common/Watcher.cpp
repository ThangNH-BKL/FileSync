#include "pch.h"
#include "Watcher.h"
#include "Logger.h"
#include "Util.h"
#include "PathSanitizer.h"

namespace fsyn {
namespace {
constexpr auto kDebounce = std::chrono::milliseconds(500);
constexpr DWORD kNotifyFilter =
    FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
    FILE_NOTIFY_CHANGE_SIZE     | FILE_NOTIFY_CHANGE_LAST_WRITE;
constexpr uint64_t kEpochFiletime = 116444736000000000ull;

int64_t FiletimeToUnixNs(ULARGE_INTEGER u) {
    return u.QuadPart > kEpochFiletime
        ? (int64_t)((u.QuadPart - kEpochFiletime) * 100ull)
        : 0;
}
} // namespace

Watcher::Watcher(const std::wstring& rootUtf16, EventFn onEvent, Logger* log, RecoveryFn onRecovery)
    : root_(rootUtf16),
      onEvent_(std::move(onEvent)),
      onRecovery_(std::move(onRecovery)),
      log_(log),
      buf_(64 * 1024) {
    PathSanitizer::NormalizeRoot(root_, rootNorm_);
}

Watcher::~Watcher() { Stop(); }

bool Watcher::Start() {
    if (hDir_ != INVALID_HANDLE_VALUE) return true;

    hDir_ = CreateFileW(root_.c_str(),
        FILE_LIST_DIRECTORY,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
        nullptr);
    if (hDir_ == INVALID_HANDLE_VALUE) {
        if (log_) log_->LogWin32(LogLevel::Error, "Watcher: CreateFileW failed", GetLastError());
        return false;
    }

    ovEvent_   = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ovEvent_ || !stopEvent_) {
        Stop();
        return false;
    }
    ov_.hEvent = ovEvent_;

    th_ = std::thread(&Watcher::ThreadProc, this);
    return true;
}

void Watcher::Stop() {
    if (stopEvent_) SetEvent(stopEvent_);
    if (hDir_ != INVALID_HANDLE_VALUE) CancelIoEx(hDir_, &ov_);
    if (th_.joinable()) th_.join();

    if (hDir_ != INVALID_HANDLE_VALUE) {
        CloseHandle(hDir_);
        hDir_ = INVALID_HANDLE_VALUE;
    }
    if (ovEvent_)   { CloseHandle(ovEvent_);   ovEvent_ = nullptr; }
    if (stopEvent_) { CloseHandle(stopEvent_); stopEvent_ = nullptr; }
}

void Watcher::ThreadProc() {
    ResetEvent(ovEvent_);
    if (!ReadDirectoryChangesW(hDir_, buf_.data(), (DWORD)buf_.size(),
                               TRUE, kNotifyFilter, nullptr, &ov_, nullptr)) {
        if (log_) log_->LogWin32(LogLevel::Error, "Watcher: RDWC initial", GetLastError());
        return;
    }

    HANDLE waits[2] = { stopEvent_, ovEvent_ };

    while (true) {
        DWORD w = WaitForMultipleObjects(2, waits, FALSE, 100);
        if (w == WAIT_OBJECT_0) break;   // stop

        if (w == WAIT_OBJECT_0 + 1) {
            DWORD bytes = 0;
            if (!GetOverlappedResult(hDir_, &ov_, &bytes, FALSE)) {
                DWORD e = GetLastError();
                if (e == ERROR_NOTIFY_ENUM_DIR) {
                    if (log_) log_->Log(LogLevel::Warn, "Watcher: buffer overflow, reconciliation needed");
                    RequestRecovery();
                } else if (e != ERROR_OPERATION_ABORTED) {
                    if (log_) log_->LogWin32(LogLevel::Error, "Watcher: GetOverlappedResult", e);
                    break;
                }
            } else if (bytes > 0) {
                ProcessBuffer(bytes);
            } else {
                // Microsoft documents a successful zero-byte completion when
                // the notification buffer overflows. Recover, do not ignore.
                if (log_) log_->Log(LogLevel::Warn, "Watcher: zero-byte completion, reconciliation needed");
                RequestRecovery();
            }

            ResetEvent(ovEvent_);
            if (!ReadDirectoryChangesW(hDir_, buf_.data(), (DWORD)buf_.size(),
                                       TRUE, kNotifyFilter, nullptr, &ov_, nullptr)) {
                if (log_) log_->LogWin32(LogLevel::Error, "Watcher: RDWC reissue", GetLastError());
                break;
            }
        }
        // Timeout hoặc wake → flush matured
        FlushMatured();
    }

    FlushMatured();
}

void Watcher::ProcessBuffer(DWORD bytes) {
    const BYTE* p   = buf_.data();
    const BYTE* end = p + bytes;
    std::wstring pendingOld;
    bool         haveOld = false;

    while (p < end) {
        auto* fni = (const FILE_NOTIFY_INFORMATION*)p;
        std::wstring name(fni->FileName, fni->FileNameLength / sizeof(WCHAR));

        switch (fni->Action) {
            case FILE_ACTION_ADDED:
                AddPending(WideToUtf8(name), FileAction::Created);
                break;
            case FILE_ACTION_MODIFIED:
                AddPending(WideToUtf8(name), FileAction::Modified);
                break;
            case FILE_ACTION_REMOVED:
                AddPending(WideToUtf8(name), FileAction::Deleted);
                break;
            case FILE_ACTION_RENAMED_OLD_NAME:
                pendingOld = name;
                haveOld = true;
                break;
            case FILE_ACTION_RENAMED_NEW_NAME:
                if (haveOld) {
                    AddPending(WideToUtf8(pendingOld), FileAction::Renamed, WideToUtf8(name));
                    haveOld = false;
                } else {
                    AddPending(WideToUtf8(name), FileAction::Created);
                }
                break;
            default: break;
        }

        if (fni->NextEntryOffset == 0) break;
        p += fni->NextEntryOffset;
    }
}

void Watcher::AddPending(const std::string& relPath, FileAction action,
                         const std::string& newRelPath) {
    if (relPath.empty()) return;

    FileEventMsg msg;
    msg.action     = action;
    msg.relPath    = relPath;
    msg.newRelPath = newRelPath;
    msg.size       = 0;
    msg.mtimeNs    = 0;

    // Stat target của event (deleted: file có thể đã biến mất → 0)
    std::wstring targetRel = (action == FileAction::Renamed && !newRelPath.empty())
        ? Utf8ToWide(newRelPath) : Utf8ToWide(relPath);
    std::wstring full = JoinPath(root_, targetRel);

    uint64_t sz = 0;
    int64_t  mt = 0;
    if (StatFile(full, sz, mt)) {
        msg.size    = sz;
        msg.mtimeNs = mt;
    }

    auto& slot = pending_[relPath];
    slot.msg        = msg;
    slot.lastChange = std::chrono::steady_clock::now();
}

void Watcher::FlushMatured() {
    auto now = std::chrono::steady_clock::now();
    std::vector<std::string> ready;
    for (const auto& kv : pending_) {
        if (now - kv.second.lastChange >= kDebounce) ready.push_back(kv.first);
    }
    for (const auto& k : ready) {
        auto it = pending_.find(k);
        if (it != pending_.end()) {
            if (onEvent_) onEvent_(it->second.msg);
            pending_.erase(it);
        }
    }
}

void Watcher::RequestRecovery() {
    if (onRecovery_) {
        // Do not enumerate millions of paths in watcher memory on overflow.
        // The worker reconnects and streams a fresh inventory in bounded batches.
        onRecovery_();
    } else {
        FullRescan();
    }
}

void Watcher::FullRescan() {
    // Chỉ gọi khi buffer overflow. Emit Created cho mọi file hiện có.
    std::vector<std::wstring> stack{ rootNorm_ };
    while (!stack.empty()) {
        std::wstring dir = stack.back(); stack.pop_back();
        std::wstring pat = JoinPath(dir, L"*");
        WIN32_FIND_DATAW fd{};
        HANDLE h = FindFirstFileW(pat.c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
            std::wstring full = JoinPath(dir, fd.cFileName);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                stack.push_back(full);
            } else {
                std::wstring rel = full.substr(rootNorm_.size());
                if (!rel.empty() && (rel[0] == L'\\' || rel[0] == L'/')) rel = rel.substr(1);
                FileEventMsg ev;
                ev.action  = FileAction::Created;
                ev.relPath = WideToUtf8(rel);
                ULARGE_INTEGER u; u.LowPart = fd.ftLastWriteTime.dwLowDateTime;
                u.HighPart = fd.ftLastWriteTime.dwHighDateTime;
                ev.mtimeNs = FiletimeToUnixNs(u);
                ev.size    = ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
                pending_[ev.relPath] = { ev, std::chrono::steady_clock::now() };
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
}

bool Watcher::StatFile(const std::wstring& path, uint64_t& size, int64_t& mtimeNs) {
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad)) return false;
    if (fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) return false;
    size = ((uint64_t)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
    ULARGE_INTEGER u; u.LowPart = fad.ftLastWriteTime.dwLowDateTime;
    u.HighPart = fad.ftLastWriteTime.dwHighDateTime;
    mtimeNs = FiletimeToUnixNs(u);
    return true;
}

} // namespace fsyn
