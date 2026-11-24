#pragma once
// ClientSession.h — 1 thread / 1 client. Xử lý handshake, snapshot, event, chunk.
// Server engine sở hữu.

#include "Protocol.h"
#include "ExtensionFilter.h"
#include "SequentialTransferQueue.h"
#include "TransferProgress.h"
#include "V5Wire.h"
#include "FileVersionStore.h"
#include "GlobalTransferControl.h"
#include "DiskSnapshotSpool.h"
#include "PerfDiagnostics.h"
#include <array>
#include <windows.h>
#include <winsock2.h>
#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include <vector>
#include <unordered_map>
#include <memory>

namespace fsyn {
class Logger;
class IUiCallbacks;
class FileIo;

class ClientSession {
public:
    struct Params {
        SOCKET          sock;            // đã accept, blocking
        std::string     peerIp;
        std::wstring    serverRootUtf16; // root gốc, session sẽ tạo <root>\<hostKey>
        std::string     expectedToken;
        ExtensionFilter filter;          // bản sao (thread-safe)
        std::shared_ptr<GlobalTransferControl> transferControl;
    };

    ClientSession(Params params, IUiCallbacks* ui, Logger* log);
    ~ClientSession();

    void Start();
    void Stop();

    bool IsInWindow() const       { return inWindow_.load(); }
    void SetInWindow(bool inWin);   // gọi từ SchedulerThread
    bool IsAlive() const            { return !dead_.load(); }
    std::string HostKey() const    { std::lock_guard<std::mutex> g(metaMu_); return hostKey_; }
    std::string PeerIp() const     { std::lock_guard<std::mutex> g(metaMu_); return peerIp_; }
    // Thread-safe snapshot; never expose the session-owned transfer queue to MFC.
    TransferProgress GetProgress() const {
        std::lock_guard<std::mutex> g(progressMu_);
        return publishedProgress_;
    }

private:
    void ThreadProc();
    bool DoHandshake();
    void HandleFrame(MsgType type, const std::vector<uint8_t>& payload);
    void HandleTreeSnapshot(const std::vector<uint8_t>& payload);
    void HandleTreeEnd();
    void HandleFileEvent(const std::vector<uint8_t>& payload);
    void HandleBlockHashes(const std::vector<uint8_t>& payload);
    void HandleFileChunk(const std::vector<uint8_t>& payload);
    void HandleFileDone(const std::vector<uint8_t>& payload);
    void HandleFileDeferred(const std::vector<uint8_t>& payload);
    bool SendV5Signatures(const std::string& path);
    void HandleV5Begin(const std::vector<uint8_t>&);
    void HandleV5Copy(const std::vector<uint8_t>&);
    void HandleV5Check(const std::vector<uint8_t>&);
    void HandleV5Bytes(const std::vector<uint8_t>&);
    void HandleV5Finish(const std::vector<uint8_t>&);
    void HandleV5Unchanged(const std::vector<uint8_t>&);
    void FailV5(const std::string& message);
    void TryStartNextFile();
    bool SendCommitted(const FileDoneMsg& committed, bool changed = true);
    void PublishProgress(); // called only on ThreadProc/session thread
    void FlushPendingEvents();
    void SendError(ErrorCode code, const std::string& msg);
    void SendHeartbeatAckIfNeeded(MsgType type);

    // Chuyển 1 file trong snapshot / event sang sync nếu cần
    void ScheduleSyncIfNeeded(const std::string& relPathUtf8,
                              uint64_t clientSize, int64_t mtimeNs,
                              const std::string& instanceId);

    // Tiện ích
    std::wstring FullPathOf(const std::string& relPathUtf8) const;
    void         NotifyTreeUpdate(const std::string& relPath);
    void         CloseSession();

    Params           params_;
    IUiCallbacks*    ui_;
    Logger*          log_;

    std::thread      thread_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> dead_{false};
    std::atomic<bool> inWindow_{true};

    mutable std::mutex metaMu_;
    std::string        hostKey_;
    std::string        peerIp_;
    std::wstring       clientRoot_;   // <serverRoot>\<hostKey>

    // Hàng đợi khi OUT_OF_WINDOW
    std::mutex                 pendingMu_;
    std::vector<FileEventMsg>  pendingEvents_;
    bool pendingOverflow_ = false;

    // Trạng thái sync đang dở, theo relPath
    struct FileSyncState {
        std::unique_ptr<FileIo> tmpFile;
        bool                    isOpen = false;
        std::wstring            journalPath;
        std::unordered_map<uint64_t, uint32_t> expectedLengths;
        std::unordered_map<uint64_t, uint32_t> expectedCrcs;
        std::unordered_map<uint64_t, bool> received;
        std::vector<uint64_t> requestedOffsets;
        uint64_t totalBytes = 0;
        uint64_t transferredBytes = 0;
        uint64_t reusedBytes = 0;
    };
    // Owned by ThreadProc only. Snapshot queues files, the server dispatches
    // just one SYNC_REQUEST_INIT at a time, and advances on FILE_COMMITTED.
    struct V5SessionState {
        bool active = false;
        std::string path;
        std::wstring destination, basePath, stagePath;
        bool destinationExistedAtStart = false;
        std::unique_ptr<FileIo> stage;
        uint64_t baseSize = 0;
        uint64_t finalSize = 0;
        uint64_t cursor = 0;
        uint64_t transferredBytes = 0;
        uint64_t copyBytes = 0;
        uint64_t stagedBytes = 0;
        std::array<uint8_t,32> baseSha{};
        std::array<uint8_t,32> finalSha{};
        bool awaitingLiteral = false;
        V5Check literal;
    } v5_;
    std::string v5AdvertisedPath_;
    std::wstring v5AdvertisedBasePath_, v5AdvertisedDestination_;
    bool v5AdvertisedDestinationExisted_ = false;
    bool v5AdvertisedLegacyAdoption_ = false;
    std::string v5AdvertisedInstanceId_;
    std::unordered_map<std::string, std::string> fileInstances_;
    uint64_t v5AdvertisedSize_ = 0;
    std::array<uint8_t,32> v5AdvertisedSha_{};
    SequentialTransferQueue                       transferQueue_;
    DiskSnapshotSpool                             snapshotSpool_;
    bool                                          snapshotSpoolFinished_ = false;
    uint64_t                                      snapshotSpoolRemaining_ = 0;
    uint64_t                                      snapshotSpoolReservedBytes_ = 0;
    void ReleaseSnapshotSpoolBudget();
    bool LoadSnapshotBatch();
    bool                                          activeTransferSlot_ = false;
    void ReleaseTransferSlot();
    // Only session thread mutates these; UI reads the published copy under lock.
    TransferProgress                             progressCounters_;
    mutable std::mutex                           progressMu_;
    TransferProgress                             publishedProgress_;
    bool                                          snapshotComplete_ = false;
    perf::Clock::time_point perfSnapshotStart_{}, perfActiveStart_{};
    uint64_t perfInventoryFiles_=0,perfInventoryFrames_=0,perfSpoolWriteUs_=0;
    uint64_t perfSpoolReadUs_=0,perfFileCompleted_=0,perfFileChanged_=0;
    uint64_t perfFileWallUs_=0,perfFilePayloadBytes_=0;
    uint64_t perfMetaResolveUs_=0,perfSignatureBuildUs_=0;
    uint64_t perfStageFlushUs_=0,perfStageFlushCount_=0;
    uint64_t perfFinalShaUs_=0,perfCommitUs_=0;
    bool perfActive_=false,perfSnapshotSeen_=false;
    std::mutex                                    syncMu_;
    std::unordered_map<std::string, FileSyncState> syncState_;
};

} // namespace fsyn
