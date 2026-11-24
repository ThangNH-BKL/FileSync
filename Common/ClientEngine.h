#pragma once
// ClientEngine.h — Engine phía Client. Thread worker làm toàn bộ I/O mạng.
// UI chỉ nhận callback qua IUiCallbacks.

#include "Protocol.h"
#include "V5Wire.h"
#include "Callbacks.h"
#include <windows.h>
#include <winsock2.h>
#include <string>
#include <thread>
#include <atomic>
#include <memory>
#include <array>

namespace fsyn {
class Watcher;
class Logger;
class FrameCodec;
class SyncQueue;
class DurableEventLog;

class ClientEngine {
public:
    struct Config {
        std::string  serverIp;
        uint16_t     port = 0;
        std::wstring watchRootUtf16;
        std::string  token;
        std::string  hostname;   // rỗng → tự lấy
    };

    ClientEngine(const Config& cfg, IUiCallbacks* ui);
    ~ClientEngine();
    ClientEngine(const ClientEngine&) = delete;
    ClientEngine& operator=(const ClientEngine&) = delete;

    bool Start();
    void Stop();
    bool IsRunning() const { return running_.load(); }

private:
    void WorkerThreadProc();
    void OnWatcherEvent(const FileEventMsg& ev);

    bool DoConnect(std::string& err);
    bool DoHandshake(FrameCodec& fc, std::string& err);
    bool SendSnapshot(FrameCodec& fc);
    void HandleFrame(FrameCodec& fc, MsgType type, const std::vector<uint8_t>& payload);
    bool SendFileEvent(FrameCodec& fc, const FileEventMsg& ev, bool* sentOnWire = nullptr);
    void HandleSyncRequestInit(FrameCodec& fc, const SyncRequestInitMsg& m);
    void HandleV5SignaturePage(FrameCodec& fc, const std::vector<uint8_t>& payload);
    void SendV5Delta(FrameCodec& fc);
    void HandleSyncRequestBlocks(FrameCodec& fc, const SyncRequestBlocksMsg& m);
    void DeferFile(FrameCodec& fc, const std::string& relPath, const std::string& reason);

    void SetState(ConnStateUi s);

    Config             cfg_;
    IUiCallbacks*      ui_;
    std::unique_ptr<Logger>    log_;
    std::unique_ptr<SyncQueue> queue_;
    std::unique_ptr<DurableEventLog> durableQueue_; // V8 experimental, off by default
    HANDLE durableLock_ = INVALID_HANDLE_VALUE;
    OVERLAPPED durableLockOv_{};
    std::unique_ptr<Watcher>   watcher_;

    std::thread           worker_;
    std::atomic<bool>     stop_{false};
    std::atomic<bool>     running_{false};
    std::atomic<ConnStateUi> state_{ConnStateUi::Idle};

    // sock_ chỉ thay đổi bởi worker, nhưng Stop() cần shutdown() → benign race
    SOCKET sock_ = INVALID_SOCKET;
    struct V5Context {
        std::string path;
        std::string expectedInstanceId;
        uint64_t baseSize = 0;
        uint32_t blockSize = 0;
        std::array<uint8_t,32> baseSha{};
        std::vector<uint8_t> prefix;
        std::vector<ByteSignature> signatures;
    } v5_;
    std::string awaitingCommit_; // Only Server FILE_COMMITTED means success.
    bool awaitingUnchanged_ = false; // Suppress unchanged-file success logs on restart.
    uint64_t awaitingSize_ = 0;
    uint8_t awaitingSha_[32] = {};
};

} // namespace fsyn
