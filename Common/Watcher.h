#pragma once
// Watcher.h — ReadDirectoryChangesW (overlapped, bWatchSubtree, 64KB buffer).
// Debounce 500ms theo relPath. Phát event qua callback.
// Buffer overflow → signal recovery; Client reconnects for a new inventory.

#include "Protocol.h"
#include <windows.h>
#include <functional>
#include <string>
#include <atomic>
#include <thread>
#include <chrono>
#include <unordered_map>
#include <vector>

namespace fsyn {
class Logger;

class Watcher {
public:
    using EventFn = std::function<void(const FileEventMsg&)>;
    using RecoveryFn = std::function<void()>;

    Watcher(const std::wstring& rootUtf16, EventFn onEvent, Logger* log, RecoveryFn onRecovery = {});
    ~Watcher();
    Watcher(const Watcher&) = delete;
    Watcher& operator=(const Watcher&) = delete;

    bool Start();
    void Stop();

private:
    void ThreadProc();
    void ProcessBuffer(DWORD bytes);
    void AddPending(const std::string& relPath, FileAction action,
                    const std::string& newRelPath = "");
    void FlushMatured();
    void FullRescan(); // Legacy fallback if no recovery callback is provided.
    void RequestRecovery();
    bool StatFile(const std::wstring& path, uint64_t& size, int64_t& mtimeNs);

    std::wstring    root_;
    std::wstring    rootNorm_;
    EventFn         onEvent_;
    RecoveryFn      onRecovery_;
    Logger*         log_;

    HANDLE          hDir_      = INVALID_HANDLE_VALUE;
    HANDLE          ovEvent_   = nullptr;
    HANDLE          stopEvent_ = nullptr;
    OVERLAPPED      ov_{};
    std::vector<BYTE> buf_;
    std::thread     th_;

    struct Pending {
        FileEventMsg                          msg;
        std::chrono::steady_clock::time_point lastChange;
    };
    std::unordered_map<std::string, Pending> pending_;  // chỉ watcher thread dùng
};

} // namespace fsyn
