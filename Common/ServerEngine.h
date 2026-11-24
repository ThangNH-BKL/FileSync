#pragma once
// ServerEngine.h — listen socket, accept thread, quản lý ClientSession*.

#include "Protocol.h"
#include "Callbacks.h"
#include "TransferProgress.h"
#include "GlobalTransferControl.h"
#include "ExtensionFilter.h"
#include "Scheduler.h"
#include <windows.h>
#include <winsock2.h>
#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include <vector>
#include <memory>

namespace fsyn {
class Logger;
class ClientSession;
class SchedulerThread;

class ServerEngine {
public:
    struct Config {
        uint16_t     port = 0;
        std::string  token;
        std::wstring serverRootUtf16;
        std::string  extFilterSpec;   // "*.txt,*.log" hoặc "*"
        SchedConfig  sched;
        size_t       maxClients = 512;
        unsigned     maxActiveTransfers = 12;
        uint64_t     inboundBytesPerSecond = 4ull * 1024 * 1024;
        uint64_t     maxInventorySpoolBytes = 64ull * 1024 * 1024 * 1024; // Admission limit; NOT proof of load capacity.
    };

    ServerEngine(const Config& cfg, IUiCallbacks* ui);
    ~ServerEngine();
    ServerEngine(const ServerEngine&) = delete;
    ServerEngine& operator=(const ServerEngine&) = delete;

    bool Start();
    void Stop();
    bool IsRunning() const { return running_.load(); }

    // UI có thể gọi để biết danh sách client
    std::vector<std::pair<std::string, std::string>> ClientList() const; // <hostKey, peerIp>
    TransferProgress GetProgress() const; // aggregate of currently connected sessions

private:
    void AcceptThreadProc();
    void AdmitSocket(SOCKET client, const sockaddr_in& peer);
    void OnSessionDead(ClientSession* s);
    void OnSchedulerFlip(bool inWindow);

    Config                        cfg_;
    IUiCallbacks*                 ui_;
    std::unique_ptr<Logger>       log_;
    ExtensionFilter               filter_;
    std::unique_ptr<Scheduler>    sched_;
    std::unique_ptr<SchedulerThread> schedThread_;

    SOCKET                        listenSock_ = INVALID_SOCKET;
    std::thread                   acceptThread_;
    std::atomic<bool>             stop_{false};
    std::atomic<bool>             running_{false};

    mutable std::mutex            sessionsMu_;
    std::vector<std::unique_ptr<ClientSession>> sessions_;
    std::shared_ptr<GlobalTransferControl> transferControl_;
};

} // namespace fsyn
