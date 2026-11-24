#include "pch.h"
#include "ClientSession.h"
#include "Logger.h"
#include "Callbacks.h"
#include "FrameCodec.h"
#include "Util.h"
#include "PathSanitizer.h"
#include "FileIo.h"
#include "Crypto.h"
#include "DeltaSync.h"
#include "ByteRangeDelta.h"
#include "V5Wire.h"
#include "FileVersionStore.h"
#include "StageFlushPolicy.h"
#include <filesystem>
#include <cstdlib>

namespace fsyn {
namespace {
constexpr uint32_t kRecvTimeoutMs = 500;
}

ClientSession::ClientSession(Params p, IUiCallbacks* ui, Logger* log)
    : params_(std::move(p)), ui_(ui), log_(log), peerIp_(params_.peerIp) {}

ClientSession::~ClientSession() { Stop(); }

void ClientSession::Start() {
    thread_ = std::thread(&ClientSession::ThreadProc, this);
}

void ClientSession::Stop() {
    stop_.store(true);
    if (params_.sock != INVALID_SOCKET) ::shutdown(params_.sock, SD_BOTH);
    if (thread_.joinable()) thread_.join();
}

void ClientSession::SetInWindow(bool inWin) {
    bool prev = inWindow_.exchange(inWin);
    if (!prev && inWin) {
        // Vừa chuyển IN → session loop sẽ tự FlushPendingEvents() khi có dịp
        // (không gọi trực tiếp ở đây vì đây là thread của SchedulerThread)
    }
}

std::wstring ClientSession::FullPathOf(const std::string& relPathUtf8) const {
    return JoinPath(clientRoot_, Utf8ToWide(relPathUtf8));
}

void ClientSession::NotifyTreeUpdate(const std::string& relPath) {
    if (ui_) ui_->OnTreeUpdate(hostKey_ + "\\" + relPath);
}

void ClientSession::ReleaseSnapshotSpoolBudget() {
    if(snapshotSpoolReservedBytes_ && params_.transferControl)
        params_.transferControl->ReleaseSpool(snapshotSpoolReservedBytes_);
    snapshotSpoolReservedBytes_=0;
}

void ClientSession::ReleaseTransferSlot() {
    if(activeTransferSlot_ && params_.transferControl) params_.transferControl->Leave(this);
    activeTransferSlot_=false;
}

void ClientSession::CloseSession() {
    if(perf::Enabled() && !hostKey_.empty()) {
        log_->Log(LogLevel::Info,"PERF session_summary client="+hostKey_+
            " inventory_files="+std::to_string(perfInventoryFiles_)+
            " spool_read_us="+std::to_string(perfSpoolReadUs_)+
            " files_acked="+std::to_string(perfFileCompleted_)+
            " files_changed="+std::to_string(perfFileChanged_)+
            " logical_bytes="+std::to_string(perfFilePayloadBytes_)+
            " file_wall_us_sum="+std::to_string(perfFileWallUs_)+
            " resolve_us="+std::to_string(perfMetaResolveUs_)+
            " signature_us="+std::to_string(perfSignatureBuildUs_)+
            " stage_flush_us="+std::to_string(perfStageFlushUs_)+
            " stage_flush_count="+std::to_string(perfStageFlushCount_)+
            " final_sha_us="+std::to_string(perfFinalShaUs_)+
            " commit_us="+std::to_string(perfCommitUs_));
    }
    ReleaseTransferSlot();
    if(params_.transferControl) params_.transferControl->Cancel(this);
    snapshotSpool_.Close(true);
    ReleaseSnapshotSpoolBudget();
    if (params_.sock != INVALID_SOCKET) {
        ::shutdown(params_.sock, SD_BOTH);
        ::closesocket(params_.sock);
        params_.sock = INVALID_SOCKET;
    }
    if (v5_.stage) v5_.stage->Close();
    v5_.active = false;
    // Đóng mọi tmp file đang mở
    std::lock_guard<std::mutex> g(syncMu_);
    for (auto& kv : syncState_) {
        if (kv.second.tmpFile) kv.second.tmpFile->Close();
    }
    syncState_.clear();
    dead_.store(true);
}

void ClientSession::ThreadProc() {
    std::string err;

    if (!DoHandshake()) {
        CloseSession();
        return;
    }

    FrameCodec fc(params_.sock);
    std::atomic<bool> heartbeatDone{false};
    std::atomic<bool> heartbeatFailed{false};
    std::thread heartbeatThread([&]() {
        FrameCodec control(params_.sock);
        while (!heartbeatDone.load() && !stop_.load()) {
            for (uint32_t i = 0; i < kHeartbeatIntervalMs / 100 && !heartbeatDone.load() && !stop_.load(); ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            if (heartbeatDone.load() || stop_.load()) break;
            // Active FILE_CHUNK / control traffic is itself proof of liveness.
            // Avoid competing with the data sender for the TCP frame lock.
            if (control.RecentlyActive(15000)) continue;
            std::string hbErr;
            if (!control.SendFrame(MsgType::HEARTBEAT, nullptr, 0, 5000, stop_, hbErr)) {
                log_->Log(LogLevel::Warn, "session: heartbeat send fail " + hbErr);
                heartbeatFailed.store(true);
                ::shutdown(params_.sock, SD_BOTH);
                break;
            }
        }
    });
    auto lastHeartbeat = std::chrono::steady_clock::now();
    auto lastRecv      = std::chrono::steady_clock::now();

    while (!stop_.load()) {
        auto now = std::chrono::steady_clock::now();

        if (heartbeatFailed.load()) break;

        // Flush pending nếu đã vào window
        if (inWindow_.load()) {
            FlushPendingEvents();
            TryStartNextFile(); // Also wakes deferred files after cooldown.
        }

        // Recv frame
        MsgType type;
        std::vector<uint8_t> payload;
        if (fc.RecvFrame(type, payload, kRecvTimeoutMs, stop_, err)) {
            lastRecv = std::chrono::steady_clock::now();
            if ((type==MsgType::V5_BYTES || type==MsgType::FILE_CHUNK) &&
                params_.transferControl &&
                !params_.transferControl->PaceReceived(payload.size(),stop_)) break;
            HandleFrame(type, payload);
        } else {
            if (err == "recv timeout") {
                if (std::chrono::steady_clock::now() - lastRecv >
                    std::chrono::milliseconds(kHeartbeatTimeoutMs)) {
                    log_->Log(LogLevel::Warn, "session: heartbeat timeout from " + peerIp_);
                    break;
                }
                continue;
            }
            if (err == "stopped") break;
            log_->Log(LogLevel::Warn, "session: recv fail " + err);
            break;
        }
    }

    heartbeatDone.store(true);
    if (heartbeatThread.joinable()) heartbeatThread.join();
    // Quiet normal disconnect: the Clients UI still updates presence. WARN/ERR
    // above remains visible for timeouts and failures.
    CloseSession();
}

bool ClientSession::DoHandshake() {
    FrameCodec fc(params_.sock);
    MsgType type;
    std::vector<uint8_t> payload;
    std::string err;

    if (!fc.RecvFrame(type, payload, 10000, stop_, err)) return false;
    if (type != MsgType::HELLO) {
        SendError(ErrorCode::Proto, "expected HELLO");
        return false;
    }

    HelloMsg hello;
    if (!Unmarshal(payload.data(), (uint32_t)payload.size(), hello)) {
        SendError(ErrorCode::Proto, "bad HELLO");
        return false;
    }

    if (!ConstantTimeEquals(hello.token, params_.expectedToken)) {
        SendError(ErrorCode::Auth, "invalid token");
        log_->Log(LogLevel::Warn, "session: auth fail from " + peerIp_);
        return false;
    }

    // IP comes from the accepted TCP socket, not the untrusted HELLO.ip.
    const std::string ipFolder = SanitizeHostKey(peerIp_);
    const std::string exeFolder = SanitizeHostKey(hello.exeFolder);
    if (ipFolder.empty() || exeFolder.empty() ||
        ipFolder == "." || ipFolder == ".." ||
        exeFolder == "." || exeFolder == "..") {
        SendError(ErrorCode::Path, "invalid peer IP or executable folder");
        return false;
    }
    const std::string key = ipFolder + "\\" + exeFolder;
    {
        std::lock_guard<std::mutex> g(metaMu_);
        hostKey_ = key;
        clientWatchRoot_ = hello.watchRoot;
        clientRoot_ = JoinPath(JoinPath(params_.serverRootUtf16,
                                       Utf8ToWide(ipFolder)), Utf8ToWide(exeFolder));
    }

    if (!FileIo::EnsureParentDir(JoinPath(clientRoot_, L"dummy"))) {
        SendError(ErrorCode::Io, "cannot create client root");
        log_->LogWin32(LogLevel::Error, "session: ensure client root", GetLastError());
        return false;
    }

    // Inventory is staged on disk, never accumulated as 100M RAM entries.
    // The file is session-scoped; on crash, reconnect sends a new inventory.
    static std::atomic<uint64_t> spoolSequence{0};
    std::wstring spoolName=L"session_"+std::to_wstring(GetCurrentProcessId())+
        L"_"+std::to_wstring(GetTickCount64())+L"_"+
        std::to_wstring(++spoolSequence)+L".tmp";
    std::filesystem::path root(params_.serverRootUtf16);
    std::filesystem::path spoolDir=root/L".fsync_state"/L"inventory_spool"/
        Utf8ToWide(ipFolder)/Utf8ToWide(exeFolder);
    if(!snapshotSpool_.Open(spoolDir/spoolName)) {
        SendError(ErrorCode::Io,"cannot create disk inventory spool");
        return false;
    }
    // .fsync_state is private from the first snapshot.
    const std::wstring privateDir=(root/L".fsync_state").wstring();
    DWORD attrs=GetFileAttributesW(privateDir.c_str());
    if(attrs!=INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_HIDDEN))
        SetFileAttributesW(privateDir.c_str(),attrs|FILE_ATTRIBUTE_HIDDEN);

    HelloAckMsg ack;
    ack.ok = 1;
    ack.serverTimeMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    std::vector<uint8_t> out;
    Marshal(ack, out);
    if (!fc.SendFrame(MsgType::HELLO_ACK, out.data(), (uint32_t)out.size(),
                      5000, stop_, err)) return false;

    // Healthy reconnection is not a file change; avoid an INFO log on each
    // reconnect when every file is already identical.
    if (ui_) ui_->OnClientListChanged();
    return true;
}

void ClientSession::HandleFrame(MsgType type, const std::vector<uint8_t>& payload) {
    switch (type) {
        case MsgType::HEARTBEAT: {
            FrameCodec fc(params_.sock);
            std::string err;
            fc.SendFrame(MsgType::HEARTBEAT_ACK, nullptr, 0, 1000, stop_, err);
            break;
        }
        case MsgType::HEARTBEAT_ACK:
            break;
        case MsgType::TREE_SNAPSHOT:  HandleTreeSnapshot(payload); break;
        case MsgType::TREE_END:       HandleTreeEnd(); break;
        case MsgType::FILE_EVENT:     HandleFileEvent(payload); break;
        case MsgType::BLOCK_HASHES:   HandleBlockHashes(payload); break;
        case MsgType::FILE_CHUNK:     HandleFileChunk(payload); break;
        case MsgType::FILE_DONE:      HandleFileDone(payload); break;
        case MsgType::FILE_DEFERRED:  HandleFileDeferred(payload); break;
        case MsgType::V5_BEGIN:      HandleV5Begin(payload); break;
        case MsgType::V5_COPY:       HandleV5Copy(payload); break;
        case MsgType::V5_CHECK:      HandleV5Check(payload); break;
        case MsgType::V5_BYTES:      HandleV5Bytes(payload); break;
        case MsgType::V5_FINISH:     HandleV5Finish(payload); break;
        case MsgType::V5_UNCHANGED:  HandleV5Unchanged(payload); break;
        default: break;
    }
}

void ClientSession::PublishProgress() {
    progressCounters_.pending = static_cast<uint64_t>(transferQueue_.PendingCount()) +
        snapshotSpoolRemaining_;
    progressCounters_.active = transferQueue_.Active() ? 1u : 0u;
    std::lock_guard<std::mutex> guard(progressMu_);
    publishedProgress_ = progressCounters_;
}

void ClientSession::HandleTreeSnapshot(const std::vector<uint8_t>& payload) {
    std::vector<TreeEntry> entries;
    if(!Unmarshal(payload.data(),static_cast<uint32_t>(payload.size()),entries)) {
        SendError(ErrorCode::Proto,"invalid TREE_SNAPSHOT");
        ::shutdown(params_.sock,SD_BOTH);
        return;
    }
    if(snapshotComplete_) {
        SendError(ErrorCode::Proto,"snapshot after TREE_END");
        ::shutdown(params_.sock,SD_BOTH);
        return;
    }
    if(snapshotSpoolReservedBytes_ > 32ull*1024*1024*1024 - payload.size() ||
       (params_.transferControl && !params_.transferControl->ReserveSpool(payload.size()))) {
        SendError(ErrorCode::Io,"inventory disk-spool quota exhausted");
        ::shutdown(params_.sock,SD_BOTH);
        return;
    }
    snapshotSpoolReservedBytes_+=payload.size();
    if(perf::Enabled()) {
        if(!perfSnapshotSeen_) {perfSnapshotStart_=perf::Clock::now();perfSnapshotSeen_=true;}
        ++perfInventoryFrames_;
    }
    const auto beforeAppend=perf::Clock::now();
    for(const auto& e:entries) {
        if(!params_.filter.Accept(e.relPath)) continue;
        std::wstring full;
        if(!PathSanitizer::Validate(clientRoot_,e.relPath,full) ||
           !IsValidFileInstanceId(e.instanceId)) {
            SendError(ErrorCode::Path,"invalid snapshot entry");
            ::shutdown(params_.sock,SD_BOTH);
            return;
        }
        DiskSnapshotSpool::Entry item{e.relPath,e.instanceId,e.size,e.mtimeNs};
        if(!snapshotSpool_.Append(item)) {
            log_->Log(LogLevel::Error,"snapshot spool write failed (disk full?)");
            SendError(ErrorCode::Io,"snapshot spool write failed");
            ::shutdown(params_.sock,SD_BOTH);
            return;
        }
        ++snapshotSpoolRemaining_;
        if(perf::Enabled()) ++perfInventoryFiles_;
    }
    if(perf::Enabled()) perfSpoolWriteUs_+=perf::Us(beforeAppend,perf::Clock::now());
    PublishProgress();
}

bool ClientSession::LoadSnapshotBatch() {
    if(snapshotSpoolFinished_) return true;
    std::vector<DiskSnapshotSpool::Entry> entries;
    bool end=false;
    const auto beforeRead=perf::Clock::now();
    if(!snapshotSpool_.ReadBatch(500,entries,end)) {
        log_->Log(LogLevel::Error,"snapshot spool read / CRC error");
        ::shutdown(params_.sock,SD_BOTH);
        return false;
    }
    if(snapshotSpoolRemaining_<entries.size()) {
        log_->Log(LogLevel::Error,"inventory cursor exceeds snapshot count");
        ::shutdown(params_.sock,SD_BOTH);
        return false;
    }
    if(perf::Enabled()) perfSpoolReadUs_+=perf::Us(beforeRead,perf::Clock::now());
    snapshotSpoolRemaining_-=entries.size();
    for(const auto& item:entries) {
        fileInstances_[item.path]=item.instance;
        transferQueue_.Enqueue(item.path,item.size,item.mtime);
    }
    if(end) {
        snapshotSpoolFinished_=true;
        snapshotSpool_.Close(true);
        ReleaseSnapshotSpoolBudget();
    }
    PublishProgress();
    return true;
}

void ClientSession::HandleTreeEnd() {
    if(!snapshotSpool_.Finish()) {
        log_->Log(LogLevel::Error,"snapshot spool flush/open failed");
        ::shutdown(params_.sock,SD_BOTH);
        return;
    }
    snapshotComplete_ = true;
    if(perf::Enabled()) {
        const auto now=perf::Clock::now();
        log_->Log(LogLevel::Info,"PERF server_inventory client="+hostKey_+
            " files="+std::to_string(perfInventoryFiles_)+
            " frames="+std::to_string(perfInventoryFrames_)+
            " spool_write_us="+std::to_string(perfSpoolWriteUs_)+
            " receive_wall_us="+std::to_string(perfSnapshotSeen_ ?
                 perf::Us(perfSnapshotStart_,now) : 0));
    }
    if(!LoadSnapshotBatch()) return;
    PublishProgress();
    // Snapshot entries are candidates, not changed files. Logging their
    // count on every reconnect creates noise even if all files are identical.
    TryStartNextFile();
}

void ClientSession::HandleFileEvent(const std::vector<uint8_t>& payload) {
    FileEventMsg ev;
    if (!Unmarshal(payload.data(), (uint32_t)payload.size(), ev)) return;

    if (ev.action == FileAction::Deleted) return;   // Never delete server data.
    // Camera software may rename a temporary file to *.bin on completion.
    // Filter the final path, not the old source name of a rename event.
    const std::string targetRel =
        (ev.action == FileAction::Renamed && !ev.newRelPath.empty())
        ? ev.newRelPath : ev.relPath;
    if (!params_.filter.Accept(targetRel)) return;

    std::wstring full;
    if (!PathSanitizer::Validate(clientRoot_, ev.relPath, full)) {
        SendError(ErrorCode::Path, "bad relPath: " + ev.relPath);
        return;
    }

    // Renamed files retain the final extension and destination path.
    if (ev.action == FileAction::Renamed) {
        std::wstring fullNew;
        if (!PathSanitizer::Validate(clientRoot_, targetRel, fullNew)) {
            SendError(ErrorCode::Path, "bad newRelPath");
            return;
        }
        full = fullNew;
    }

    // The Tree View will be updated only after FILE_DONE is verified/committed.
    if (!inWindow_.load()) {
        std::lock_guard<std::mutex> g(pendingMu_);
        if (!pendingOverflow_) {
            if (pendingEvents_.size() >= 10000) {
                // Reconnect requests a complete inventory when the schedule opens.
                // Clearing is safe only because we force that reconciliation.
                pendingOverflow_ = true;
                pendingEvents_.clear();
                log_->Log(LogLevel::Warn,
                    "Scheduled event buffer overflow; full resnapshot required on reconnect");
            } else {
                pendingEvents_.push_back(ev);
            }
        }
        return;
    }

    ScheduleSyncIfNeeded(targetRel, ev.size, ev.mtimeNs, ev.instanceId);
}

void ClientSession::ScheduleSyncIfNeeded(const std::string& relPathUtf8,
                                         uint64_t clientSize, int64_t mtimeNs,
                                         const std::string& instanceId) {
    if (!IsValidFileInstanceId(instanceId)) {
        log_->Log(LogLevel::Warn, "versioning: missing/invalid file identity: " + relPathUtf8);
        return; // Do not risk overwriting another generation.
    }
    fileInstances_[relPathUtf8] = instanceId;
    // All queue changes occur on the session thread. Deduplicate watcher
    // updates; an update to the active file is queued for a second pass.
    transferQueue_.Enqueue(relPathUtf8, clientSize, mtimeNs);
    if (snapshotComplete_) PublishProgress();
    TryStartNextFile();
}

void ClientSession::TryStartNextFile() {
    if (!snapshotComplete_ || !inWindow_.load() || stop_.load() ||
        params_.sock == INVALID_SOCKET || transferQueue_.Active()) return;
    if (!transferQueue_.PendingCount() && !LoadSnapshotBatch()) return;
    if (!transferQueue_.HasEligible()) return;
    if (params_.transferControl && !params_.transferControl->TryEnter(this)) return;
    activeTransferSlot_=true;
    SequentialTransferQueue::File candidate;
    if (!transferQueue_.BeginNext(candidate)) { ReleaseTransferSlot(); return; }
    if(perf::Enabled()) {perfActiveStart_=perf::Clock::now();perfActive_=true;}
    PublishProgress(); // Pending -> Active. Completion is NOT counted here.
    std::wstring full;
    if (!PathSanitizer::Validate(clientRoot_, candidate.path, full)) {
        transferQueue_.Commit(candidate.path);
        ReleaseTransferSlot();
        ++progressCounters_.failed;
        PublishProgress();
        log_->Log(LogLevel::Warn, "sync queue: invalid path: " + candidate.path);
        return;
    }
    if (!SendV5Signatures(candidate.path)) {
        log_->Log(LogLevel::Warn, "v5: signature exchange failed: " + candidate.path);
        ::shutdown(params_.sock, SD_BOTH);
        return;
    }
    // Do not log a file as changed before the Client checks SHA-256.
    // V5_BEGIN is the first confirmed changed-file transfer; unchanged files
    // only exchange the silent V5_UNCHANGED / FILE_COMMITTED handshake.
}

void ClientSession::HandleFileDeferred(const std::vector<uint8_t>& payload) {
    ResyncMsg msg;
    if (!Unmarshal(payload.data(), (uint32_t)payload.size(), msg) ||
        !transferQueue_.Active() || transferQueue_.Active()->path != msg.relPath) {
        SendError(ErrorCode::Proto, "unexpected FILE_DEFERRED");
        ::shutdown(params_.sock, SD_BOTH);
        return;
    }
    {
        std::lock_guard<std::mutex> g(syncMu_);
        auto it = syncState_.find(msg.relPath);
        if (it != syncState_.end()) {
            if (it->second.tmpFile) it->second.tmpFile->Close();
            syncState_.erase(it);
        }
    }
    if (v5_.active && v5_.path == msg.relPath) {
        if (v5_.stage) v5_.stage->Close();
        v5_ = V5SessionState{}; // keep persistent .fsync.v5.tmp for next attempt
    }
    if(perf::Enabled()) perfActive_=false;
    transferQueue_.Defer(msg.relPath,
        SequentialTransferQueue::Clock::now() + std::chrono::seconds(5));
    ReleaseTransferSlot();
    PublishProgress(); // Active -> pending; do not count this as completed.
    log_->Log(LogLevel::Warn, "sync queue: deferred unstable source " + msg.relPath);
    TryStartNextFile();
}

bool ClientSession::SendCommitted(const FileDoneMsg& committed, bool changed) {
    std::vector<uint8_t> payload;
    if (!Marshal(committed, payload)) return false;
    FrameCodec fc(params_.sock);
    std::string err;
    if (!fc.SendFrame(MsgType::FILE_COMMITTED, payload.data(),
                      (uint32_t)payload.size(), 10000, stop_, err)) {
        log_->Log(LogLevel::Warn, "FILE_COMMITTED failed: " + err);
        ::shutdown(params_.sock, SD_BOTH);
        return false;
    }
    if (!transferQueue_.Commit(committed.relPath)) {
        log_->Log(LogLevel::Error, "sync queue: commit path mismatch " + committed.relPath);
        ::shutdown(params_.sock, SD_BOTH);
        return false;
    }
    // Count a file only after the Server completed its SHA-256 verification,
    // committed (or verified unchanged) and sent FILE_COMMITTED successfully.
    ReleaseTransferSlot();
    if(perf::Enabled()) {
        ++perfFileCompleted_;
        if(changed) ++perfFileChanged_;
        perfFilePayloadBytes_+=committed.finalSize;
        if(perfActive_) perfFileWallUs_+=perf::Us(perfActiveStart_,perf::Clock::now());
        perfActive_=false;
        if(perfFileCompleted_%1000==0)
            log_->Log(LogLevel::Info,"PERF server_files client="+hostKey_+
                " acked="+std::to_string(perfFileCompleted_)+
                " changed="+std::to_string(perfFileChanged_)+
                " logical_bytes="+std::to_string(perfFilePayloadBytes_)+
                " per_file_wall_us_sum="+std::to_string(perfFileWallUs_)+
            " resolve_us="+std::to_string(perfMetaResolveUs_)+
            " signature_us="+std::to_string(perfSignatureBuildUs_)+
            " stage_flush_us="+std::to_string(perfStageFlushUs_)+
            " stage_flush_count="+std::to_string(perfStageFlushCount_)+
            " final_sha_us="+std::to_string(perfFinalShaUs_)+
            " commit_us="+std::to_string(perfCommitUs_));
    }
    // A re-queued change to this path must retain its new instance identity.
    if(!transferQueue_.HasPending(committed.relPath)) fileInstances_.erase(committed.relPath);
    ++progressCounters_.completed;
    if (changed) ++progressCounters_.updated;
    else ++progressCounters_.unchanged;
    PublishProgress();
    TryStartNextFile(); // No new file until durable commit and ACK sent.
    return true;
}

void ClientSession::HandleBlockHashes(const std::vector<uint8_t>& payload) {
    BlockHashesMsg m;
    if (!Unmarshal(payload.data(), (uint32_t)payload.size(), m)) return;
    if (!transferQueue_.Active() || transferQueue_.Active()->path != m.relPath) {
        SendError(ErrorCode::Proto, "block hashes for inactive file");
        ::shutdown(params_.sock, SD_BOTH);
        return;
    }

    std::wstring full;
    if (!PathSanitizer::Validate(clientRoot_, m.relPath, full)) return;

    // Resume source is the durable staging file when it exists. Do not delete
    // it on reconnect. Otherwise seed staging from the committed destination.
    const std::wstring tmpPath = full + L".fsync.tmp";
    std::wstring journalPath;
    if (!FileVersionStore::JournalPath(params_.serverRootUtf16,clientRoot_,full,journalPath) ||
        !FileIo::EnsureParentDir(journalPath)) {
        SendError(ErrorCode::Io,"cannot create private block journal path");
        ::shutdown(params_.sock, SD_BOTH);
        return;
    }
    // Migrate any old visible chunk-journal without discarding it.
    const std::wstring oldJournal=full+L".fsync.blocks";
    if (GetFileAttributesW(oldJournal.c_str())!=INVALID_FILE_ATTRIBUTES) {
        if (GetFileAttributesW(journalPath.c_str())!=INVALID_FILE_ATTRIBUTES ||
            !MoveFileExW(oldJournal.c_str(),journalPath.c_str(),MOVEFILE_WRITE_THROUGH)) {
            log_->Log(LogLevel::Error,"resume: conflicting legacy chunk journal");
            SendError(ErrorCode::Io,"cannot migrate old block journal");
            ::shutdown(params_.sock, SD_BOTH);
            return;
        }
    }
    if (!FileIo::EnsureParentDir(tmpPath)) {
        SendError(ErrorCode::Io, "cannot create staging directory");
        ::shutdown(params_.sock, SD_BOTH);
        return;
    }
    FileIo committed;
    const bool hasCommitted = committed.OpenForRead(full);
    const uint64_t serverSize = hasCommitted ? committed.Size() : 0;
    FileIo staged;
    bool hasStage = staged.OpenForRead(tmpPath);
    if (!hasStage) {
        if (hasCommitted) {
            committed.Close();
            if (!CopyFileW(full.c_str(), tmpPath.c_str(), FALSE)) {
                SendError(ErrorCode::Io, "seed staging failed");
                return;
            }
        } else {
            FileIo created;
            if (!created.OpenForWrite(tmpPath, true) || !created.TruncateTo(0)) {
                SendError(ErrorCode::Io, "create staging failed");
                return;
            }
            created.Close();
        }
        hasStage = staged.OpenForRead(tmpPath);
    }
    if (!hasStage) {
        SendError(ErrorCode::Io, "open staging failed");
        return;
    }
    const uint64_t stagedSize = staged.Size();
    // Last block length must be derived from the next offset or source size.
    // For the final block, a short staged block is only considered matching
    // when its actual byte count matches the CRC for the client's block.
    std::vector<uint64_t> needOffsets;
    std::unordered_map<uint64_t, uint32_t> lengths, crcs;
    uint64_t reusedBytes = 0;
    std::vector<uint8_t> sbuf(kBlockSize);
    for (size_t i = 0; i < m.blocks.size(); ++i) {
        const auto& cb = m.blocks[i];
        bool need = true;
        // The protocol carries the exact length, including the final short block.
        if (cb.length == 0 || cb.length > kBlockSize ||
            cb.offset != uint64_t(i) * kBlockSize) {
            SendError(ErrorCode::Proto, "invalid block layout");
            return;
        }
        const uint32_t expected = cb.length;
        if (i + 1 < m.blocks.size() && expected != kBlockSize) {
            SendError(ErrorCode::Proto, "short non-final block");
            return;
        }
        lengths[cb.offset] = expected;
        crcs[cb.offset] = cb.crc32;
        if (cb.offset < stagedSize) {
            const uint32_t available = static_cast<uint32_t>(
                (std::min)(uint64_t(expected), stagedSize - cb.offset));
            // The final block may be short; its CRC disambiguates actual length.
            if (available == expected) {
                const uint64_t got = staged.ReadAt(cb.offset, sbuf.data(), available);
                if (got == available && Crc32(sbuf.data(), available) == cb.crc32)
                    need = false;
            }
        }
        if (need) needOffsets.push_back(cb.offset);
        else reusedBytes += (std::min)(uint64_t(expected), stagedSize - cb.offset);
    }
    staged.Close();
    {
        std::lock_guard<std::mutex> g(syncMu_);
        auto& state = syncState_[m.relPath];
        if (state.tmpFile) state.tmpFile->Close();
        state.tmpFile = std::make_unique<FileIo>();
        if (!state.tmpFile->OpenForWrite(tmpPath, false)) {
            syncState_.erase(m.relPath);
            SendError(ErrorCode::Io, "open staging for write failed");
            return;
        }
        state.isOpen = true;
        state.journalPath = journalPath;
        state.expectedLengths = std::move(lengths);
        state.expectedCrcs = std::move(crcs);
        state.received.clear();
        state.requestedOffsets = needOffsets;
        state.reusedBytes = reusedBytes;
        state.transferredBytes = 0;
        state.totalBytes = 0;
        for (const auto& b : m.blocks) state.totalBytes += state.expectedLengths[b.offset];
        // Preserve the journal across reconnects. Staged bytes are independently
        // CRC-checked above; the journal is not blindly trusted.

    }
    // An already-matching file must remain silent even in the legacy
    // block protocol. WARN/ERR and changed-file logging are unaffected.
    if (!needOffsets.empty())
        log_->Log(LogLevel::Info, "resume: " + m.relPath +
            " requested_blocks=" + std::to_string(needOffsets.size()) +
            " reused_blocks=" + std::to_string(m.blocks.size() - needOffsets.size()));

    // Gửi SYNC_REQUEST_BLOCKS
    SyncRequestBlocksMsg resp;
    resp.relPath    = m.relPath;
    resp.serverSize = serverSize;
    resp.truncate   = 0;   // SetEndOfFile làm ở FILE_DONE dựa trên finalSize
    resp.needOffsets = std::move(needOffsets);

    std::vector<uint8_t> out;
    if (!Marshal(resp, out)) return;
    FrameCodec fc(params_.sock);
    std::string err;
    fc.SendFrame(MsgType::SYNC_REQUEST_BLOCKS, out.data(), (uint32_t)out.size(),
                 5000, stop_, err);
}

void ClientSession::HandleFileChunk(const std::vector<uint8_t>& payload) {
    FileChunkMsg m;
    if (!Unmarshal(payload.data(), (uint32_t)payload.size(), m)) return;
    if (!transferQueue_.Active() || transferQueue_.Active()->path != m.relPath) {
        SendError(ErrorCode::Proto, "chunk for inactive file");
        ::shutdown(params_.sock, SD_BOTH);
        return;
    }
    if (m.data.size() > kMaxChunkData) return;

    std::lock_guard<std::mutex> g(syncMu_);
    auto it = syncState_.find(m.relPath);
    if (it == syncState_.end() || !it->second.isOpen) {
        log_->Log(LogLevel::Warn, "session: chunk without sync state: " + m.relPath);
        return;
    }

    auto& state = it->second;
    const auto expected = state.expectedLengths.find(m.offset);
    const auto crc = state.expectedCrcs.find(m.offset);
    if (expected == state.expectedLengths.end() || crc == state.expectedCrcs.end() ||
        m.data.size() != expected->second ||
        Crc32(m.data.data(), m.data.size()) != crc->second) {
        log_->Log(LogLevel::Warn, "resume: unexpected or corrupted block: " + m.relPath);
        SendError(ErrorCode::Proto, "unexpected block offset, length or CRC");
        return;
    }
    if (state.received[m.offset]) return; // duplicate retransmission
    uint64_t w = state.tmpFile->WriteAt(m.offset, m.data.data(),
                                             (uint32_t)m.data.size());
    if (w != m.data.size()) {
        log_->LogWin32(LogLevel::Error, "session: write chunk", GetLastError());
        SendError(ErrorCode::Io, "write chunk failed");
        return;
    }
    if (!state.tmpFile->Flush()) {
        SendError(ErrorCode::Io, "flush staged block failed");
        return;
    }
    // Durable journal entry: offset (8), length (4), crc32 (4).
    // Each entry is appended only AFTER the staged data is flushed.
    FileIo journal;
    if (!journal.OpenForWrite(state.journalPath, true)) {
        SendError(ErrorCode::Io, "open block journal failed");
        return;
    }
    struct BlockRecord { uint64_t offset; uint32_t length; uint32_t crc; };
    const BlockRecord rec{ m.offset, static_cast<uint32_t>(m.data.size()), crc->second };
    if (journal.WriteAt(journal.Size(), &rec, sizeof(rec)) != sizeof(rec) || !journal.Flush()) {
        SendError(ErrorCode::Io, "persist block journal failed");
        return;
    }
    state.received[m.offset] = true;
    state.transferredBytes += m.data.size();
}

void ClientSession::HandleFileDone(const std::vector<uint8_t>& payload) {
    FileDoneMsg m;
    if (!Unmarshal(payload.data(), (uint32_t)payload.size(), m)) return;
    if (!transferQueue_.Active() || transferQueue_.Active()->path != m.relPath) {
        SendError(ErrorCode::Proto, "FILE_DONE for inactive file");
        ::shutdown(params_.sock, SD_BOTH);
        return;
    }

    std::wstring full;
    if (!PathSanitizer::Validate(clientRoot_, m.relPath, full)) return;
    std::wstring tmpPath = full + L".fsync.tmp";

    std::unique_ptr<FileIo> tmpHolder;
    uint64_t transferredBytes = 0, reusedBytes = 0;
    {
        std::lock_guard<std::mutex> g(syncMu_);
        auto it = syncState_.find(m.relPath);
        if (it != syncState_.end() && it->second.isOpen) {
            // Reject premature completion; a missing block must remain resumable.
            for (uint64_t off : it->second.requestedOffsets) {
                auto received = it->second.received.find(off);
                if (received == it->second.received.end() || !received->second) {
                    SendError(ErrorCode::Proto, "FILE_DONE before all requested blocks");
                    ::shutdown(params_.sock, SD_BOTH);
                    return;
                }
            }
            transferredBytes = it->second.transferredBytes;
            reusedBytes = it->second.reusedBytes;
            tmpHolder = std::move(it->second.tmpFile);
            syncState_.erase(it);
        } else {
            // Không có state - có thể client đã gửi FILE_DONE cho file không đổi
            // (needOffsets rỗng). Mở tmp đã tồn tại để verify.
            tmpHolder = std::make_unique<FileIo>();
            if (!tmpHolder->OpenForWrite(tmpPath, false)) {
                // A retried FILE_DONE for an already-committed identical file
                // is a valid duplicate ACK, NOT a per-file success or warning.
                // Coi như thành công nếu file đích đã có sẵn với size đúng
                FileIo existing;
                if (existing.OpenForRead(full) && existing.Size() == m.finalSize) {
                    // Verify SHA trên file đích
                    FileBlockReader r(full);
                    uint8_t sha[32];
                    if (ComputeFileSha256(r, sha) &&
                        std::memcmp(sha, m.sha256, 32) == 0) {
                        NotifyTreeUpdate(m.relPath);
                        SendCommitted(m,false);
                        return;
                    }
                }
                log_->Log(LogLevel::Warn,"session: done without state: " + m.relPath);
                SendError(ErrorCode::Io, "no tmp for FILE_DONE");
                return;
            }
        }
    }

    // Truncate to final size
    if (!tmpHolder->TruncateTo(m.finalSize)) {
        log_->LogWin32(LogLevel::Error, "session: SetEndOfFile", GetLastError());
        SendError(ErrorCode::Io, "truncate failed");
        ::shutdown(params_.sock, SD_BOTH);
        return;
    }
    if (!tmpHolder->Flush()) {
        SendError(ErrorCode::Io, "flush before commit failed");
        ::shutdown(params_.sock, SD_BOTH);
        return;
    }
    tmpHolder->Close();

    // Verify SHA
    FileBlockReader r(tmpPath);
    uint8_t sha[32];
    if (!ComputeFileSha256(r, sha)) {
        log_->Log(LogLevel::Error, "session: SHA-256 compute failed");
        SendError(ErrorCode::Io, "sha compute failed");
        ::shutdown(params_.sock, SD_BOTH);
        return;
    }
    if (std::memcmp(sha, m.sha256, 32) != 0) {
        log_->Log(LogLevel::Error, "session: SHA-256 mismatch for " + m.relPath);
        SendError(ErrorCode::Io, "sha mismatch");
        ::shutdown(params_.sock, SD_BOTH);
        return;
    }

    // Even the older block protocol suppresses unchanged-file success logs.
    // Do not replace a perfectly matching committed file only to claim it was
    // updated; the SHA-256 comparison also guards against size/mtime tricks.
    {
        FileIo existing;
        if (existing.OpenForRead(full) && existing.Size()==m.finalSize) {
            existing.Close();
            FileBlockReader current(full);
            uint8_t currentSha[32]{};
            if (ComputeFileSha256(current,currentSha) &&
                std::memcmp(currentSha,m.sha256,32)==0) {
                FileIo::DeleteFileQuiet(tmpPath);
                std::wstring journal;
                if (FileVersionStore::JournalPath(params_.serverRootUtf16,clientRoot_,full,journal))
                    FileIo::DeleteFileQuiet(journal);
                NotifyTreeUpdate(m.relPath);
                SendCommitted(m,false);
                return;
            }
        }
    }
    // MoveFileEx thay thế
    if (!FileIo::AtomicReplace(tmpPath, full)) {
        log_->LogWin32(LogLevel::Error, "session: AtomicReplace", GetLastError());
        SendError(ErrorCode::Io, "replace failed");
        ::shutdown(params_.sock, SD_BOTH);
        return;
    }

    std::wstring journal;
    if (FileVersionStore::JournalPath(params_.serverRootUtf16,clientRoot_,full,journal))
        FileIo::DeleteFileQuiet(journal);
    log_->Log(LogLevel::Info, "resume: completed " + m.relPath +
        " TotalBytes=" + std::to_string(m.finalSize) +
        " TransferredBytes=" + std::to_string(transferredBytes) +
        " ReusedBytes=" + std::to_string(reusedBytes) + " SHA256=PASS");
    log_->Log(LogLevel::Info, "session: synced " + m.relPath +
              " (" + std::to_string(m.finalSize) + " bytes)");
    NotifyTreeUpdate(m.relPath);
    SendCommitted(m);
}

void ClientSession::FlushPendingEvents() {
    std::vector<FileEventMsg> drain;
    {
        std::lock_guard<std::mutex> g(pendingMu_);
        if (pendingOverflow_) {
            pendingOverflow_ = false;
            pendingEvents_.clear();
            log_->Log(LogLevel::Warn, "Reconnecting to reconcile scheduled buffer overflow");
            ::shutdown(params_.sock, SD_BOTH);
            return;
        }
        if (pendingEvents_.empty()) return;
        // Giới hạn 100 event mỗi vòng để không block heartbeat
        size_t n = (std::min)(pendingEvents_.size(), (size_t)100);
        drain.assign(pendingEvents_.begin(), pendingEvents_.begin() + n);
        pendingEvents_.erase(pendingEvents_.begin(), pendingEvents_.begin() + n);
    }
    for (const auto& ev : drain) {
        std::string target = (ev.action == FileAction::Renamed && !ev.newRelPath.empty())
            ? ev.newRelPath : ev.relPath;
        ScheduleSyncIfNeeded(target, ev.size, ev.mtimeNs, ev.instanceId);
    }
}

void ClientSession::SendError(ErrorCode code, const std::string& msg) {
    if (params_.sock == INVALID_SOCKET) return;
    ErrorMsg em; em.code = code; em.message = msg;
    std::vector<uint8_t> out;
    if (!Marshal(em, out)) return;
    FrameCodec fc(params_.sock);
    std::string err;
    fc.SendFrame(MsgType::ERROR_MSG, out.data(), (uint32_t)out.size(),
                 2000, stop_, err);
}

void ClientSession::SendHeartbeatAckIfNeeded(MsgType) {}

// ============================ V5 Byte-Range Delta ============================
void ClientSession::FailV5(const std::string& message) {
    log_->Log(LogLevel::Warn, "v5: " + message);
    SendError(ErrorCode::Proto, "v5: " + message);
    ::shutdown(params_.sock, SD_BOTH);
}

bool ClientSession::SendV5Signatures(const std::string& path) {
    std::wstring full;
    if (!PathSanitizer::Validate(clientRoot_, path, full)) return false;
    auto id = fileInstances_.find(path);
    if (id == fileInstances_.end()) return false;
    VersionPlan plan;
    const auto beforeResolve=perf::Clock::now();
    const bool resolved=FileVersionStore::Resolve(params_.serverRootUtf16,
                                                  clientRoot_,full,id->second,plan);
    if(perf::Enabled()) perfMetaResolveUs_+=perf::Us(beforeResolve,perf::Clock::now());
    if (!resolved) {
        log_->Log(LogLevel::Error, "versioning: cannot reserve destination for " + path);
        return false;
    }
    const auto beforeSig=perf::Clock::now();
    FileIo committed;
    const bool baseExists = !plan.base.empty() && committed.OpenForRead(plan.base);
    const uint64_t baseSize = baseExists ? committed.Size() : 0;
    const uint32_t blockSize = SelectDeltaBlockSize(baseSize);
    V5SignaturePage pg;
    pg.path = path;
    pg.expectedInstanceId = id->second;
    pg.blockSize = blockSize;
    pg.baseSize = baseSize;
    if (baseExists) {
        FileBlockReader baseReader(plan.base);
        if (!ComputeFileSha256(baseReader, pg.baseSha.data())) return false;
        uint32_t prefixLen = static_cast<uint32_t>((std::min)(baseSize, uint64_t(64)));
        pg.prefix.resize(prefixLen);
        if (prefixLen && committed.ReadAt(0, pg.prefix.data(), prefixLen) != prefixLen) return false;
    }
    std::vector<ByteSignature> all;
    if (baseExists && !BuildByteSignatures(baseSize,
            [&](uint64_t pos, void* b, uint32_t n) { return committed.ReadAt(pos, b, n); },
            blockSize, all)) return false;
    // Signatures represent ONE immutable base version. Detect even changes that
    // retain file size and mtime before giving the client the catalogue.
    if (baseExists) {
        FileBlockReader after(plan.base);
        uint8_t finalHash[32];
        if (!ComputeFileSha256(after, finalHash) ||
            std::memcmp(finalHash, pg.baseSha.data(), 32) != 0) return false;
    }
    if(perf::Enabled()) perfSignatureBuildUs_+=perf::Us(beforeSig,perf::Clock::now());
    v5AdvertisedPath_ = path;
    v5AdvertisedBasePath_ = plan.base;
    v5AdvertisedDestination_ = plan.destination;
    v5AdvertisedDestinationExisted_ = plan.destinationExisted;
    v5AdvertisedLegacyAdoption_ = plan.legacyAdoption;
    v5AdvertisedInstanceId_ = id->second;
    v5AdvertisedSize_ = baseSize;
    v5AdvertisedSha_ = pg.baseSha;
    FrameCodec fc(params_.sock);
    std::vector<uint8_t> payload;
    std::string err;
    constexpr size_t kSignaturesPerPage = 5000;
    for (size_t from = 0;; from += kSignaturesPerPage) {
        const size_t last = (std::min)(all.size(), from + kSignaturesPerPage);
        pg.firstIndex = static_cast<uint32_t>(from);
        pg.last = (last == all.size());
        pg.items.assign(all.begin() + from, all.begin() + last);
        if (!Pack(pg, payload) || !fc.SendFrame(MsgType::V5_SIGNATURE_PAGE,
                payload.data(), (uint32_t)payload.size(), 30000, stop_, err)) return false;
        if (pg.last) break;
    }
    return true;
}

void ClientSession::HandleV5Begin(const std::vector<uint8_t>& payload) {
    V5Begin m;
    if (!Unpack(payload.data(), (uint32_t)payload.size(), m) ||
        !transferQueue_.Active() || transferQueue_.Active()->path != m.path ||
        v5AdvertisedPath_ != m.path || v5_.active ||
        m.baseSha != v5AdvertisedSha_) {
        FailV5("bad V5_BEGIN or inconsistent base hash"); return;
    }
    std::wstring full;
    if (!PathSanitizer::Validate(clientRoot_, m.path, full)) { FailV5("unsafe path"); return; }
    FileIo base;
    if (v5AdvertisedSize_) {
        if (!base.OpenForRead(v5AdvertisedBasePath_) || base.Size() != v5AdvertisedSize_) {
            FailV5("base file changed"); return;
        }
    } else {
        // A zero-length file may exist; the previous SHA check still protects it.
        if (!std::all_of(m.baseSha.begin(), m.baseSha.end(), [](uint8_t v){return v==0;})) {
            FileBlockReader reader(v5AdvertisedBasePath_);
            uint8_t sha[32];
            if (!ComputeFileSha256(reader, sha) ||
                std::memcmp(sha, m.baseSha.data(), 32) != 0) {
                FailV5("base file changed");return;
            }
        } else if (!v5AdvertisedBasePath_.empty() && base.OpenForRead(v5AdvertisedBasePath_)) {
            FailV5("base appeared after signature scan");return;
        }
    }
    if (v5AdvertisedSize_) {
        FileBlockReader reader(v5AdvertisedBasePath_);
        uint8_t sha[32];
        if (!ComputeFileSha256(reader, sha) ||
            std::memcmp(sha, m.baseSha.data(), 32) != 0) {
            FailV5("base hash changed before delta");return;
        }
    }
    v5_ = V5SessionState{};
    v5_.path = m.path;
    v5_.baseSize = v5AdvertisedSize_;
    v5_.baseSha = m.baseSha;
    v5_.finalSize = m.size;
    v5_.finalSha = m.targetSha;
    v5_.destination = v5AdvertisedDestination_;
    v5_.basePath = v5AdvertisedBasePath_;
    v5_.destinationExistedAtStart = v5AdvertisedDestinationExisted_;
    v5_.stagePath = v5_.destination + L".fsync.v5.tmp";
    if (!FileIo::EnsureParentDir(v5_.stagePath)) { FailV5("create destination directory failed"); return; }
    v5_.stage = std::make_unique<FileIo>();
    // Keep staged data from previous network outages. Literal checks use
    // SHA-256, not an untrusted journal or stale file length.
    if (!v5_.stage->OpenForWrite(v5_.stagePath, true)) {
        FailV5("cannot open resume stage"); return;
    }
    v5_.active = true;
    log_->Log(LogLevel::Info, "sync queue: started " + m.path +
        " size=" + std::to_string(m.size) +
        " pending=" + std::to_string(transferQueue_.PendingCount()));
}

void ClientSession::HandleV5Copy(const std::vector<uint8_t>& payload) {
    V5Copy m;
    if (!Unpack(payload.data(), (uint32_t)payload.size(), m) || !v5_.active ||
        v5_.awaitingLiteral || m.path != v5_.path || !m.length ||
        m.length > 1024u * 1024u || m.dest != v5_.cursor ||
        m.length > v5_.finalSize - (std::min)(m.dest, v5_.finalSize) ||
        m.dest > v5_.finalSize || m.source > v5_.baseSize ||
        m.length > v5_.baseSize - m.source) {
        FailV5("invalid COPY_RANGE");return;
    }
    FileIo base;
    if (!base.OpenForRead(v5_.basePath)) { FailV5("COPY base not found"); return; }
    std::vector<uint8_t> buf(64u * 1024u);
    uint64_t count = 0;
    while (count < m.length) {
        uint32_t n = static_cast<uint32_t>((std::min)(uint64_t(buf.size()), uint64_t(m.length) - count));
        if (base.ReadAt(m.source + count, buf.data(), n) != n ||
            v5_.stage->WriteAt(m.dest + count, buf.data(), n) != n) {
            FailV5("COPY read/write failed");return;
        }
        count += n;
    }
    v5_.cursor += m.length;
    v5_.copyBytes += m.length;
}

void ClientSession::HandleV5Check(const std::vector<uint8_t>& payload) {
    V5Check m;
    if (!Unpack(payload.data(), (uint32_t)payload.size(), m) || !v5_.active ||
        v5_.awaitingLiteral || m.path != v5_.path || !m.length ||
        m.length > kMaxChunkData || m.dest != v5_.cursor ||
        m.dest > v5_.finalSize || m.length > v5_.finalSize - m.dest) {
        FailV5("invalid literal CHECK");return;
    }
    bool need = true;
    if (v5_.stage->Size() >= m.dest + m.length) {
        std::vector<uint8_t> old(m.length);
        std::array<uint8_t,32> sha{};
        if (v5_.stage->ReadAt(m.dest, old.data(), m.length) == m.length &&
            Sha256::HashBuffer(old.data(), old.size(), sha.data()) && sha == m.sha)
            need = false;
    }
    V5Need resp; resp.path = m.path; resp.dest = m.dest; resp.need = need;
    std::vector<uint8_t> wire; std::string err;
    FrameCodec fc(params_.sock);
    if (!Pack(resp, wire) || !fc.SendFrame(MsgType::V5_NEED, wire.data(),
             (uint32_t)wire.size(), 30000, stop_, err)) {
        FailV5("send literal response failed: " + err);return;
    }
    if (need) {
        v5_.awaitingLiteral = true;
        v5_.literal = m;
    } else {
        v5_.cursor += m.length;
        v5_.stagedBytes += m.length;
    }
}

void ClientSession::HandleV5Bytes(const std::vector<uint8_t>& payload) {
    V5Bytes m;
    if (!Unpack(payload.data(), (uint32_t)payload.size(), m) || !v5_.active ||
        !v5_.awaitingLiteral || m.path != v5_.path ||
        m.dest != v5_.literal.dest || m.dest != v5_.cursor ||
        m.data.size() != v5_.literal.length) {
        FailV5("unexpected literal payload");return;
    }
    uint8_t sha[32];
    if (!Sha256::HashBuffer(m.data.data(), m.data.size(), sha) ||
        std::memcmp(sha, v5_.literal.sha.data(), 32) != 0) {
        FailV5("literal SHA mismatch");return;
    }
    const bool wrote=v5_.stage->WriteAt(m.dest,m.data.data(),(uint32_t)m.data.size())==m.data.size();
    // EXPERIMENTAL: postpone stage flush until N MiB. No FILE_COMMITTED can
    // be emitted until the mandatory final Flush() succeeds. Resume must
    // revalidate each staged literal's SHA before reuse after a crash.
    static const uint64_t batchThreshold=[]() -> uint64_t {
        char b[24]{};DWORD n=GetEnvironmentVariableA("FILESYNC_EXPERIMENTAL_STAGE_BATCH_MIB",b,sizeof(b));
        if(!n || n>=sizeof(b)) return 0;
        char* end=nullptr;unsigned long v=std::strtoul(b,&end,10);
        return end!=b && *end=='\0' && v>=1 && v<=16 ? uint64_t(v)*1024*1024 : 0;
    }();
    if(wrote) v5_.unflushedLiteralBytes+=m.data.size();
    const bool shouldFlush=wrote && ShouldFlushStageLiteral(
        v5_.unflushedLiteralBytes,batchThreshold);
    const auto beforeFlush=perf::Clock::now();
    const bool flushed=wrote && (!shouldFlush || v5_.stage->Flush());
    if(shouldFlush) {
        const uint64_t us=perf::Us(beforeFlush,perf::Clock::now());
        if(params_.transferControl)params_.transferControl->ObserveStageFlush(us);
        if(perf::Enabled()) {perfStageFlushUs_+=us;++perfStageFlushCount_;}
        if(flushed)v5_.unflushedLiteralBytes=0;
    }
    if (!flushed) {
        FailV5("persist literal failed");return;
    }
    v5_.transferredBytes += m.data.size();
    v5_.cursor += m.data.size();
    v5_.awaitingLiteral = false;
}

void ClientSession::HandleV5Finish(const std::vector<uint8_t>& payload) {
    FileDoneMsg done;
    if (!Unmarshal(payload.data(), (uint32_t)payload.size(), done) || !v5_.active ||
        v5_.awaitingLiteral || done.relPath != v5_.path ||
        done.finalSize != v5_.finalSize || v5_.cursor != v5_.finalSize ||
        std::memcmp(done.sha256, v5_.finalSha.data(), 32) != 0) {
        FailV5("FINISH before complete delta or wrong file hash");return;
    }
    const bool truncated=v5_.stage->TruncateTo(v5_.finalSize);
    const auto beforeFinalFlush=perf::Clock::now();
    const bool finalFlushed=truncated && v5_.stage->Flush(); // ALWAYS durable before commit.
    if(truncated) {
        const uint64_t us=perf::Us(beforeFinalFlush,perf::Clock::now());
        if(params_.transferControl)params_.transferControl->ObserveStageFlush(us);
        if(perf::Enabled()) {perfStageFlushUs_+=us;++perfStageFlushCount_;}
    }
    if(!finalFlushed) {
        FailV5("staged file flush failed");return;
    }
    v5_.stage->Close();
    // Prevent modification of the committed base while COPY ops were running.
    FileIo base;
    bool exists = !v5_.basePath.empty() && base.OpenForRead(v5_.basePath);
    if (v5_.baseSize || !std::all_of(v5_.baseSha.begin(),v5_.baseSha.end(),
        [](uint8_t v){return v==0;})) {
        if (!exists || base.Size() != v5_.baseSize) {
            FailV5("base disappeared before commit");return;
        }
        base.Close();
        FileBlockReader r(v5_.basePath);
        uint8_t oldSha[32];
        if (!ComputeFileSha256(r, oldSha) ||
            std::memcmp(oldSha, v5_.baseSha.data(), 32) != 0) {
            FailV5("base changed before commit");return;
        }
    } else if (exists) { FailV5("base appeared before commit");return; }
    if (!v5_.destinationExistedAtStart &&
        GetFileAttributesW(v5_.destination.c_str()) != INVALID_FILE_ATTRIBUTES) {
        FailV5("version target unexpectedly appeared; refusing overwrite"); return;
    }
    FileBlockReader staged(v5_.stagePath);
    uint8_t finalSha[32];
    const auto beforeFinalHash=perf::Clock::now();
    const bool hashed=ComputeFileSha256(staged,finalSha);
    if(perf::Enabled()) perfFinalShaUs_+=perf::Us(beforeFinalHash,perf::Clock::now());
    if (!hashed || std::memcmp(finalSha, done.sha256, 32) != 0) {
        FailV5("reconstructed file SHA256 mismatch");return;
    }
    // New generation: NO replace flag. Even if another process creates this
    // name between the collision check and commit, preserve its file.
    const auto beforeCommit=perf::Clock::now();
    const bool committed = v5_.destinationExistedAtStart
        ? FileIo::AtomicReplace(v5_.stagePath, v5_.destination)
        : MoveFileExW(v5_.stagePath.c_str(),v5_.destination.c_str(),
                      MOVEFILE_WRITE_THROUGH) != FALSE;
    if(perf::Enabled()) perfCommitUs_+=perf::Us(beforeCommit,perf::Clock::now());
    if (!committed) {
        FailV5("atomic version commit failed");return;
    }
    const uint64_t copied = v5_.copyBytes;
    const uint64_t stagedReuse = v5_.stagedBytes;
    const std::wstring savedPath = v5_.destination;
    const std::string savedRel = savedPath.size() > clientRoot_.size()+1
        ? WideToUtf8(savedPath.substr(clientRoot_.size()+1)) : v5_.path;
    if(!perf::QuietSuccess()) log_->Log(LogLevel::Info, "v5: completed " + v5_.path +
        (savedRel == v5_.path ? "" : " SavedAs=" + savedRel) +
        " TotalBytes=" + std::to_string(v5_.finalSize) +
        " TransferredBytes=" + std::to_string(v5_.transferredBytes) +
        " ReusedBytes=" + std::to_string(copied + stagedReuse) +
        " CopiedBytes=" + std::to_string(copied) +
        " StagedReusedBytes=" + std::to_string(stagedReuse) + " SHA256=PASS");
    const std::string committedPath = v5_.path;
    const std::wstring actualDestination = v5_.destination;
    v5_ = V5SessionState{};
    if (actualDestination.size() > clientRoot_.size() + 1)
        NotifyTreeUpdate(WideToUtf8(actualDestination.substr(clientRoot_.size() + 1)));
    else NotifyTreeUpdate(committedPath);
    SendCommitted(done);
}

void ClientSession::HandleV5Unchanged(const std::vector<uint8_t>& payload) {
    FileDoneMsg done;
    if (!Unmarshal(payload.data(), (uint32_t)payload.size(), done) ||
        !transferQueue_.Active() || transferQueue_.Active()->path != done.relPath ||
        v5AdvertisedPath_ != done.relPath || v5_.active ||
        done.finalSize != v5AdvertisedSize_ ||
        std::memcmp(done.sha256, v5AdvertisedSha_.data(), 32) != 0) {
        FailV5("invalid unchanged-file confirmation");return;
    }
    if (v5AdvertisedBasePath_.empty()) {
        FailV5("unchanged base unavailable"); return;
    }
    FileIo base;
    if (!base.OpenForRead(v5AdvertisedBasePath_) || base.Size() != done.finalSize) {
        FailV5("unchanged base unavailable"); return;
    }
    base.Close();
    FileBlockReader r(v5AdvertisedBasePath_);
    uint8_t sha[32];
    if (!ComputeFileSha256(r,sha) ||
        std::memcmp(sha,done.sha256,32) != 0) {
        FailV5("unchanged base SHA mismatch");return;
    }
    const bool newVersion = _wcsicmp(v5AdvertisedDestination_.c_str(),
                                     v5AdvertisedBasePath_.c_str()) != 0;
    if (newVersion && v5AdvertisedLegacyAdoption_) {
        std::wstring logical;
        if (!PathSanitizer::Validate(clientRoot_,done.relPath,logical) ||
            !FileVersionStore::AdoptIdenticalLegacy(params_.serverRootUtf16,clientRoot_,logical,v5AdvertisedInstanceId_)) {
            FailV5("cannot adopt identical legacy generation"); return;
        }
        NotifyTreeUpdate(done.relPath);
        SendCommitted(done,false);
        return;
    }
    if (newVersion) {
        if (!v5AdvertisedDestinationExisted_ &&
            GetFileAttributesW(v5AdvertisedDestination_.c_str()) != INVALID_FILE_ATTRIBUTES) {
            FailV5("unchanged version target collision"); return;
        }
        const std::wstring tmp = v5AdvertisedDestination_ + L".fsync.v5.tmp";
        if (!FileIo::EnsureParentDir(tmp) ||
            !CopyFileW(v5AdvertisedBasePath_.c_str(), tmp.c_str(), FALSE)) {
            FailV5("cannot copy identical content into new version"); return;
        }
        FileBlockReader staged(tmp);
        uint8_t stagedSha[32];
        if (!ComputeFileSha256(staged,stagedSha) ||
            std::memcmp(stagedSha,done.sha256,32) != 0 ||
            !(v5AdvertisedDestinationExisted_
                ? FileIo::AtomicReplace(tmp,v5AdvertisedDestination_)
                : MoveFileExW(tmp.c_str(),v5AdvertisedDestination_.c_str(),
                              MOVEFILE_WRITE_THROUGH) != FALSE)) {
            FailV5("cannot verify/commit identical new version");return;
        }
        const std::wstring relative =
            v5AdvertisedDestination_.substr(clientRoot_.size() + 1);
        NotifyTreeUpdate(WideToUtf8(relative));
        log_->Log(LogLevel::Info, "versioning: identical content saved as new file " +
                  WideToUtf8(relative) + " SHA256=PASS");
    } else {
        NotifyTreeUpdate(done.relPath); // unchanged: deliberately no success log
    }
    SendCommitted(done,newVersion);
}

} // namespace fsyn
