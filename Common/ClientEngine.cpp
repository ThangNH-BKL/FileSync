#include "pch.h"
#include "ClientEngine.h"
#include "Watcher.h"
#include "Logger.h"
#include "FrameCodec.h"
#include "SyncQueue.h"
#include "DurableEventLog.h"
#include "UsnJournalGuard.h"
#include <filesystem>
#include "Util.h"
#include "PathSanitizer.h"
#include "FileIo.h"
#include "Crypto.h"
#include "DeltaSync.h"
#include "ByteRangeDelta.h"
#include "V5Wire.h"
#include "FileInstanceId.h"
#include "PerfDiagnostics.h"
#include <chrono>

namespace fsyn {
namespace {
constexpr uint32_t kWorkerRecvTimeoutMs = 500;
}

ClientEngine::ClientEngine(const Config& cfg, IUiCallbacks* ui)
    : cfg_(cfg), ui_(ui),
      log_(std::make_unique<Logger>()),
      queue_(std::make_unique<SyncQueue>()) {}

ClientEngine::~ClientEngine() { Stop(); }

bool ClientEngine::Start() {
    if (running_.load()) return true;

    // Logger — client_conn.log cạnh exe
    wchar_t exe[MAX_PATH] = { 0 };
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring dir(exe);
    size_t slash = dir.find_last_of(L"\\/");
    if (slash != std::wstring::npos) dir.resize(slash);
    log_->Open(JoinPath(dir, L"client_conn.log"));
    if (ui_) log_->SetSink(LoggerSinkToUi, ui_);
    log_->Log(LogLevel::Info, "ClientEngine: starting");

    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        log_->Log(LogLevel::Error, "WSAStartup failed");
        return false;
    }

    durableQueue_.reset();
    durableLockOv_={};
    // Experimental durability layer: explicit opt-in until Windows E2E tests pass.
    // Different WatchRoot / server targets must NOT share one WAL.
    wchar_t durableFlag[8]{};
    if (GetEnvironmentVariableW(L"FILESYNC_EXPERIMENTAL_DURABLE_QUEUE",
                                durableFlag,8)==1 && durableFlag[0]==L'1') {
        wchar_t appData[32768]{};
        DWORD size=GetEnvironmentVariableW(L"LOCALAPPDATA",appData,32768);
        if(!size || size>=32768) {
            log_->Log(LogLevel::Error,"V8 durable queue requires LOCALAPPDATA");
            WSACleanup();return false;
        }
        uint64_t hash=14695981039346656037ull;
        const std::wstring scope=cfg_.watchRootUtf16+L"|"+
            Utf8ToWide(cfg_.serverIp)+L"|"+std::to_wstring(cfg_.port);
        for(wchar_t c:scope) {hash^=uint16_t(c);hash*=1099511628211ull;}
        const auto root=std::filesystem::path(appData)/L"FileSync"/L"event_queues";
        auto wal=root/(std::to_wstring(hash)+L".wal");
        std::error_code ec;
        std::filesystem::create_directories(root,ec);
        if(ec){log_->Log(LogLevel::Error,"V8 durable queue: cannot create state folder");
            WSACleanup();return false;}
        const auto lockFile=std::filesystem::path(wal.wstring()+L".lock");
        durableLock_=CreateFileW(lockFile.c_str(),GENERIC_READ|GENERIC_WRITE,
            FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_ALWAYS,FILE_ATTRIBUTE_HIDDEN,nullptr);
        if(durableLock_==INVALID_HANDLE_VALUE ||
           !LockFileEx(durableLock_,LOCKFILE_EXCLUSIVE_LOCK|LOCKFILE_FAIL_IMMEDIATELY,
                       0,1,0,&durableLockOv_)) {
            if(durableLock_!=INVALID_HANDLE_VALUE)CloseHandle(durableLock_);
            durableLock_=INVALID_HANDLE_VALUE;
            log_->Log(LogLevel::Error,"V8 queue is locked by another running client");
            WSACleanup();return false;
        }
        durableQueue_=std::make_unique<DurableEventLog>(wal);
        if(!durableQueue_->Open()) {
            log_->Log(LogLevel::Error,"V8 durable queue WAL corrupt/unavailable; fail closed. Backup and investigate: "+WideToUtf8(wal.wstring()));
            durableQueue_.reset();
            UnlockFileEx(durableLock_,0,1,0,&durableLockOv_);
            CloseHandle(durableLock_);durableLock_=INVALID_HANDLE_VALUE;
            WSACleanup();return false;
        }
        log_->Log(LogLevel::Info,"V8 experimental durable queue enabled; full snapshots remain mandatory");
    }

    stop_.store(false);
    running_.store(true);

    // USN preflight only. No implicit checkpoint advancement or snapshot skip.
    char usnFlag[8]{};
    if(GetEnvironmentVariableA("FILESYNC_EXPERIMENTAL_USN_PROBE",usnFlag,sizeof(usnFlag))==1 && usnFlag[0]=='1') {
        UsnState state{};DWORD error=ERROR_SUCCESS;
        if(QueryNtfsUsnState(cfg_.watchRootUtf16,state,error)) {
            log_->Log(LogLevel::Info,"USN probe: journal="+std::to_string(state.journalId)+
                " LowestValid="+std::to_string(state.lowestValid)+
                " Next="+std::to_string(state.next)+" (diagnostic only)");
        } else {
            log_->Log(LogLevel::Warn,"USN unsupported/unavailable, full scan required, Win32="+
                std::to_string(error));
        }
    }

    // Watcher — nếu Start fail thì dọn dẹp
    watcher_ = std::make_unique<Watcher>(
        cfg_.watchRootUtf16,
        [this](const FileEventMsg& ev) { OnWatcherEvent(ev); },
        log_.get(),
        [this]() { queue_->RequireReconciliation(); });
    if (!watcher_->Start()) {
        log_->Log(LogLevel::Error, "Watcher start failed");
        running_.store(false);
        if(durableLock_!=INVALID_HANDLE_VALUE) {
            UnlockFileEx(durableLock_,0,1,0,&durableLockOv_);
            CloseHandle(durableLock_);durableLock_=INVALID_HANDLE_VALUE;
        }
        WSACleanup();
        return false;
    }

    worker_ = std::thread(&ClientEngine::WorkerThreadProc, this);
    return true;
}

void ClientEngine::Stop() {
    if (!running_.load()) return;
    stop_.store(true);
    if (watcher_) watcher_->Stop();
    if (sock_ != INVALID_SOCKET) ::shutdown(sock_, SD_BOTH);   // unblock recv/send
    if (worker_.joinable()) worker_.join();
    if (sock_ != INVALID_SOCKET) { ::closesocket(sock_); sock_ = INVALID_SOCKET; }
    WSACleanup();
    if(durableLock_!=INVALID_HANDLE_VALUE) {
        UnlockFileEx(durableLock_,0,1,0,&durableLockOv_);
        CloseHandle(durableLock_);durableLock_=INVALID_HANDLE_VALUE;
    }
    SetState(ConnStateUi::Idle);
    if(perf::Enabled()) {
        const auto tx=ReadAdaptiveBandwidthStats();
        log_->Log(LogLevel::Info,"PERF client_adaptive_tx bytes="+std::to_string(tx.bytes)+
            " frames="+std::to_string(tx.frames)+" wait_us="+std::to_string(tx.waitUs));
    }
    log_->Log(LogLevel::Info, "ClientEngine: stopped");
    running_.store(false);
    log_->Close();
}

void ClientEngine::OnWatcherEvent(const FileEventMsg& ev) {
    // Deleted: chỉ log phía client, không gửi server
    if (ev.action == FileAction::Deleted) {
        char buf[512];
        _snprintf_s(buf, sizeof(buf), _TRUNCATE,
            "File deleted: %s (size=%llu mtimeNs=%lld)",
            ev.relPath.c_str(),
            (unsigned long long)ev.size, (long long)ev.mtimeNs);
        log_->Log(LogLevel::Info, buf);
        return;
    }
    if(durableQueue_) {
        if(!durableQueue_->Push(ev)) {
            log_->Log(LogLevel::Error,"V8 WAL append failed/queue full: reconciliation required; stop client, check disk and WAL");
            queue_->RequireReconciliation();
            // Do not treat the event as durably queued.
        }
    } else queue_->Push(ev);
}

void ClientEngine::WorkerThreadProc() {
    uint32_t backoffMs = 1000;
    std::string err;
    // All reconnect paths (connect/handshake/snapshot/transfer) share one
    // bounded backoff; never spin against a permission-denied directory.
    const auto pauseAfterFailure = [&]() {
        const uint32_t jitterMs=static_cast<uint32_t>(
            (GetTickCount64()+uint64_t(GetCurrentThreadId())*7919ull)%1200ull);
        const uint32_t pauseMs=backoffMs+jitterMs;
        for(uint32_t elapsed=0;elapsed<pauseMs && !stop_.load();elapsed+=100)
            std::this_thread::sleep_for(std::chrono::milliseconds(
                (std::min)(100u,pauseMs-elapsed)));
        backoffMs=(std::min)(backoffMs*2u,30000u);
    };

    while (!stop_.load()) {
        if (state_.load() == ConnStateUi::AuthFail) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            continue;
        }

        SetState(ConnStateUi::Connecting);
        if (!DoConnect(err)) {
            log_->Log(LogLevel::Warn, "Connect failed: " + err);
            SetState(ConnStateUi::ReconnectWait);
            pauseAfterFailure();
            continue;
        }

        FrameCodec fc(sock_);
        if (!DoHandshake(fc, err)) {
            log_->Log(LogLevel::Warn, "Handshake failed: " + err);
            ::closesocket(sock_); sock_ = INVALID_SOCKET;
            if(err == "auth failed") {
                SetState(ConnStateUi::AuthFail); // Explicit server Auth error only.
            } else {
                SetState(ConnStateUi::ReconnectWait);
                pauseAfterFailure();
            }
            continue;
        }

        // Dedicated heartbeat sender: remains active during SendSnapshot and
        // long file transfers. FILE_CHUNK bandwidth waiting never owns send lock.
        std::atomic<bool> heartbeatDone{false};
        std::atomic<bool> heartbeatFailed{false};
        std::thread heartbeatThread([&]() {
            FrameCodec control(sock_);
            while (!heartbeatDone.load() && !stop_.load()) {
                for (int i = 0; i < 50 && !heartbeatDone.load() && !stop_.load(); ++i)
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                if (heartbeatDone.load() || stop_.load()) break;
                // Active FILE_CHUNK / control traffic is itself proof of liveness.
                // Avoid competing with the data sender for the TCP frame lock.
                if (control.RecentlyActive(15000)) continue;
                std::string hbErr;
                if (!control.SendFrame(MsgType::HEARTBEAT, nullptr, 0, 5000, stop_, hbErr)) {
                    log_->Log(LogLevel::Warn, "Heartbeat send failed: " + hbErr);
                    heartbeatFailed.store(true);
                    ::shutdown(sock_, SD_BOTH);
                    break;
                }
            }
        });
        awaitingCommit_.clear();
        awaitingUnchanged_ = false;
        v5_ = V5Context{};
        awaitingSize_ = 0;
        std::memset(awaitingSha_, 0, sizeof(awaitingSha_));
        SetState(ConnStateUi::Syncing);
        if(durableQueue_) durableQueue_->ResetInflight();
        // The fresh full inventory reconciles any watcher events lost earlier.
        // Do not clear events here: changes DURING traversal still need delivery.
        if (queue_->ConsumeOverflow())
            log_->Log(LogLevel::Warn, "Watcher event queue overflow: reconciling by full snapshot");
        const bool snapshotOk=SendSnapshot(fc);
        if (!snapshotOk) {
            log_->Log(LogLevel::Warn, "Snapshot send failed; reconnect after backoff");
            ::shutdown(sock_, SD_BOTH);
        } else {
            backoffMs=1000;
            SetState(ConnStateUi::ConnectedIdle);
        }

        auto lastHeartbeat = std::chrono::steady_clock::now();
        auto lastRecv      = std::chrono::steady_clock::now();

        while (snapshotOk && !stop_.load()) {
            auto now = std::chrono::steady_clock::now();
            if (heartbeatFailed.load()) break;
            if (queue_->Overflowed()) {
                log_->Log(LogLevel::Warn,
                    "Event queue overflow during session; reconnect for complete inventory");
                ::shutdown(sock_, SD_BOTH);
                break;
            }

            // Send at most 32 watcher events before returning to receive ACKs.
            // A 10,000-event burst must not monopolize the TCP send path.
            if(durableQueue_) {
                const auto pending=durableQueue_->UnsentBatch(32);
                for(const auto& item:pending) {
                    bool sent=false;
                    if(!SendFileEvent(fc,item.event,&sent)) {
                        queue_->RequireReconciliation();
                        ::shutdown(sock_,SD_BOTH);
                        break;
                    }
                    if(sent) durableQueue_->MarkSent(item.seq);
                    else if(!durableQueue_->AckMissing(item.seq)) {
                        log_->Log(LogLevel::Error,"V8 WAL cleanup of vanished source failed");
                        ::shutdown(sock_,SD_BOTH);
                        break;
                    }
                }
            } else {
                std::vector<FileEventMsg> pending;
                if(queue_->PopBatch(32,pending)) {
                    for(const auto& ev:pending) if(!SendFileEvent(fc,ev)) break;
                }
            }

            // Recv
            MsgType type;
            std::vector<uint8_t> payload;
            if (fc.RecvFrame(type, payload, kWorkerRecvTimeoutMs, stop_, err)) {
                lastRecv = std::chrono::steady_clock::now();
                HandleFrame(fc, type, payload);
            } else {
                if (err == "recv timeout") {
                    if (std::chrono::steady_clock::now() - lastRecv > std::chrono::seconds(60)) {
                        log_->Log(LogLevel::Warn, "Heartbeat timeout");
                        break;
                    }
                    continue;
                }
                if (err == "stopped") break;
                log_->Log(LogLevel::Warn, "Recv failed: " + err);
                break;
            }
        }

        heartbeatDone.store(true);
        if (heartbeatThread.joinable()) heartbeatThread.join();
        if (sock_ != INVALID_SOCKET) { ::closesocket(sock_); sock_ = INVALID_SOCKET; }
        if (!stop_.load() && state_.load() != ConnStateUi::AuthFail) {
            SetState(ConnStateUi::ReconnectWait);
            pauseAfterFailure();
        }
    }

    SetState(ConnStateUi::Stopping);
}

bool ClientEngine::DoConnect(std::string& err) {
    sock_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock_ == INVALID_SOCKET) { err = "socket()"; return false; }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(cfg_.port);
    if (inet_pton(AF_INET, cfg_.serverIp.c_str(), &addr.sin_addr) != 1) {
        err = "bad IP";
        ::closesocket(sock_); sock_ = INVALID_SOCKET;
        return false;
    }

    SetSocketNonBlocking(sock_, true);
    int r = ::connect(sock_, (sockaddr*)&addr, sizeof(addr));
    if (r == SOCKET_ERROR) {
        int e = WSAGetLastError();
        if (e != WSAEWOULDBLOCK) {
            err = "connect: " + ErrStr(e);
            ::closesocket(sock_); sock_ = INVALID_SOCKET;
            return false;
        }
        bool connected = false;
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (std::chrono::steady_clock::now() < deadline && !stop_.load()) {
            fd_set wset; FD_ZERO(&wset); FD_SET(sock_, &wset);
            timeval tv{ 0, 200000 };
            int sr = ::select(0, nullptr, &wset, nullptr, &tv);
            if (sr > 0) {
                int so = 0, slen = sizeof(so);
                ::getsockopt(sock_, SOL_SOCKET, SO_ERROR, (char*)&so, &slen);
                if (so != 0) {
                    err = "connect: " + ErrStr((DWORD)so);
                    ::closesocket(sock_); sock_ = INVALID_SOCKET;
                    return false;
                }
                connected = true;
                break;
            }
        }
        if (!connected) {
            err = "connect timeout";
            ::closesocket(sock_); sock_ = INVALID_SOCKET;
            return false;
        }
    }
    SetSocketNonBlocking(sock_, false);
    return true;
}

bool ClientEngine::DoHandshake(FrameCodec& fc, std::string& err) {
    HelloMsg hello;
    hello.hostname  = cfg_.hostname.empty() ? GetLocalHostname() : cfg_.hostname;
    hello.ip        = cfg_.serverIp;
    hello.watchRoot = WideToUtf8(cfg_.watchRootUtf16);
    hello.token     = cfg_.token;
    // Only the executable's parent folder name, never the WatchRoot name.
    wchar_t modulePath[MAX_PATH] = {};
    DWORD chars = GetModuleFileNameW(nullptr, modulePath, MAX_PATH);
    if (chars > 0 && chars < MAX_PATH) {
        std::wstring path(modulePath, chars);
        const auto sep = path.find_last_of(L"\\/");
        if (sep != std::wstring::npos) {
            path.resize(sep);
            const auto parentSep = path.find_last_of(L"\\/");
            const std::wstring folder = (parentSep == std::wstring::npos)
                ? path : path.substr(parentSep + 1);
            hello.exeFolder = WideToUtf8(folder);
        }
    }

    std::vector<uint8_t> payload;
    if (!Marshal(hello, payload)) { err = "marshal HELLO"; return false; }
    if (!fc.SendFrame(MsgType::HELLO, payload.data(), (uint32_t)payload.size(),
                      5000, stop_, err)) return false;

    MsgType type; std::vector<uint8_t> resp;
    if (!fc.RecvFrame(type, resp, 10000, stop_, err)) return false;

    if (type == MsgType::ERROR_MSG) {
        ErrorMsg em;
        if (Unmarshal(resp.data(), (uint32_t)resp.size(), em) &&
            em.code == ErrorCode::Auth) {
            err = "auth failed";
        } else {
            err = "server error";
        }
        return false;
    }
    if (type != MsgType::HELLO_ACK) { err = "expected HELLO_ACK"; return false; }
    HelloAckMsg ack;
    if (!Unmarshal(resp.data(), (uint32_t)resp.size(), ack) || ack.ok != 1) {
        err = "bad HELLO_ACK";
        return false;
    }
    return true;
}

bool ClientEngine::SendSnapshot(FrameCodec& fc) {
    // V6.3: bounded-memory snapshot: send each 500-entry batch immediately.
    // NOTE: The Server still queues all candidates in RAM; the disk-backed
    // queue and incremental USN catch-up belong to the next architecture phase.
    constexpr size_t kSnapshotBatch = 500;
    const bool trace = perf::Enabled();
    const auto started = perf::Clock::now();
    uint64_t dirs=0, files=0, skipped=0, sent=0, frames=0;
    uint64_t identityUs=0, sendUs=0;
    const auto report = [&](bool success) {
        if (!trace) return;
        log_->Log(success ? LogLevel::Info : LogLevel::Warn,
            "PERF client_snapshot result=" + std::string(success ? "ok" : "fail") +
            " directories=" + std::to_string(dirs) +
            " files=" + std::to_string(files) +
            " ignored=" + std::to_string(skipped) +
            " sent=" + std::to_string(sent) +
            " frames=" + std::to_string(frames) +
            " identity_us=" + std::to_string(identityUs) +
            " network_send_us=" + std::to_string(sendUs) +
            " wall_us=" + std::to_string(perf::Us(started,perf::Clock::now())));
    };
    std::vector<TreeEntry> entries;
    entries.reserve(kSnapshotBatch);
    std::string err;
    auto flush = [&]() -> bool {
        if (entries.empty()) return true;
        std::vector<uint8_t> payload;
        if (!Marshal(entries, payload)) return false;
        const auto startSend=perf::Clock::now();
        const bool ok=fc.SendFrame(MsgType::TREE_SNAPSHOT, payload.data(),
                          static_cast<uint32_t>(payload.size()), 10000, stop_, err);
        if(trace) sendUs+=perf::Us(startSend,perf::Clock::now());
        if(!ok) return false;
        sent+=entries.size(); ++frames;
        entries.clear();
        return true;
    };
    std::vector<std::wstring> stack{ cfg_.watchRootUtf16 };
    while (!stack.empty() && !stop_.load()) {
        std::wstring dir = std::move(stack.back()); stack.pop_back();
        ++dirs;
        const std::wstring pat = JoinPath(dir, L"*");
        WIN32_FIND_DATAW fd{};
        HANDLE h = FindFirstFileW(pat.c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) {
            // Fail closed for access-denied / I/O errors: silently skipping a
            // subtree would falsely mark an incomplete inventory as complete.
            const DWORD e = GetLastError();
            if (e == ERROR_FILE_NOT_FOUND) continue; // empty directory
            log_->LogWin32(LogLevel::Warn, "Snapshot: FindFirstFileW at " + WideToUtf8(dir), e);
            report(false);
            return false;
        }
        bool ok = true;
        do {
            if (stop_.load()) { ok = false; break; }
            if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
            const std::wstring full = JoinPath(dir, fd.cFileName);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                // Do not recursively enter links/junctions pointing outside Watch.
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) stack.push_back(full);
                continue;
            }
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) { ++skipped; continue; }
            ++files;
            std::wstring rel = full.substr(cfg_.watchRootUtf16.size());
            if (!rel.empty() && (rel[0] == L'\\' || rel[0] == L'/')) rel.erase(0,1);
            TreeEntry e;
            e.relPath = WideToUtf8(rel);
            ULARGE_INTEGER u{};
            u.LowPart = fd.ftLastWriteTime.dwLowDateTime;
            u.HighPart = fd.ftLastWriteTime.dwHighDateTime;
            constexpr uint64_t kEpoch = 116444736000000000ull;
            e.mtimeNs = u.QuadPart > kEpoch ? (int64_t)((u.QuadPart - kEpoch) * 100ull) : 0;
            e.size = ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
            const auto beforeId=perf::Clock::now();
            e.instanceId = GetFileInstanceId(full);
            if(trace) identityUs+=perf::Us(beforeId,perf::Clock::now());
            if (IsValidFileInstanceId(e.instanceId)) {
                entries.push_back(std::move(e));
                if (entries.size() == kSnapshotBatch && !flush()) { ok = false; break; }
            } else {
                // This file is NOT in the inventory. Never acknowledge a
                // complete snapshot that silently omitted an inaccessible file.
                ++skipped;
                log_->Log(LogLevel::Warn,"Snapshot: unavailable file identity at "+WideToUtf8(full));
                ok=false;
                break;
            }
        } while (FindNextFileW(h, &fd));
        const DWORD lastError = GetLastError();
        FindClose(h);
        if (!ok) { report(false); return false; }
        if (lastError != ERROR_NO_MORE_FILES) {
            log_->LogWin32(LogLevel::Warn, "Snapshot: FindNextFileW at " + WideToUtf8(dir), lastError);
            report(false);
            return false;
        }
    }
    if (stop_.load() || !flush()) { report(false); return false; }
    const auto beginEnd=perf::Clock::now();
    const bool done=fc.SendFrame(MsgType::TREE_END, nullptr, 0, 10000, stop_, err);
    if(trace) sendUs+=perf::Us(beginEnd,perf::Clock::now());
    if(!done) log_->Log(LogLevel::Warn,"Snapshot TREE_END failed: " + err);
    report(done);
    return done;
}

bool ClientEngine::SendFileEvent(FrameCodec& fc, const FileEventMsg& ev, bool* sentOnWire) {
    if(sentOnWire) *sentOnWire=false;
    FileEventMsg current = ev;
    if (ev.action != FileAction::Deleted) {
        const std::string rel = ev.action == FileAction::Renamed && !ev.newRelPath.empty()
            ? ev.newRelPath : ev.relPath;
        std::wstring source;
        if (!PathSanitizer::Validate(cfg_.watchRootUtf16, rel, source)) return false;
        current.instanceId = GetFileInstanceId(source);
        if (!IsValidFileInstanceId(current.instanceId)) {
            DWORD a=GetFileAttributesW(source.c_str());
            DWORD sourceError=a==INVALID_FILE_ATTRIBUTES?GetLastError():ERROR_SUCCESS;
            if(a==INVALID_FILE_ATTRIBUTES &&
               (sourceError==ERROR_FILE_NOT_FOUND || sourceError==ERROR_PATH_NOT_FOUND))
                return true; // file has vanished; no remote-delete semantics
            return false; // access denied / transient I/O error: do not drop event
        }
        WIN32_FILE_ATTRIBUTE_DATA stats{};
        if (GetFileAttributesExW(source.c_str(),GetFileExInfoStandard,&stats)) {
            current.size = (uint64_t(stats.nFileSizeHigh) << 32) | stats.nFileSizeLow;
            ULARGE_INTEGER timestamp{};
            timestamp.LowPart = stats.ftLastWriteTime.dwLowDateTime;
            timestamp.HighPart = stats.ftLastWriteTime.dwHighDateTime;
            constexpr uint64_t kEpoch100ns = 116444736000000000ull;
            current.mtimeNs = timestamp.QuadPart > kEpoch100ns
                ? int64_t((timestamp.QuadPart - kEpoch100ns) * 100ull) : 0;
        }
    }
    std::vector<uint8_t> payload;
    if (!Marshal(current, payload)) return false;
    std::string err;
    if (!fc.SendFrame(MsgType::FILE_EVENT, payload.data(),
                      (uint32_t)payload.size(), 10000, stop_, err)) {
        log_->Log(LogLevel::Warn, "FILE_EVENT send failed: " + err);
        ::shutdown(sock_, SD_BOTH);
        return false;
    }
    if(sentOnWire) *sentOnWire=true;
    return true;
}

void ClientEngine::HandleFrame(FrameCodec& fc, MsgType type,
                               const std::vector<uint8_t>& payload) {
    switch (type) {
        case MsgType::HEARTBEAT: {
            std::string err;
            fc.SendFrame(MsgType::HEARTBEAT_ACK, nullptr, 0, 1000, stop_, err);
            break;
        }
        case MsgType::HEARTBEAT_ACK:
            break;
        case MsgType::V5_SIGNATURE_PAGE:
            HandleV5SignaturePage(fc, payload);
            break;
        case MsgType::FILE_COMMITTED: {
            FileDoneMsg committed;
            if (!Unmarshal(payload.data(), (uint32_t)payload.size(), committed) ||
                awaitingCommit_ != committed.relPath ||
                awaitingSize_ != committed.finalSize ||
                std::memcmp(awaitingSha_, committed.sha256, 32) != 0) {
                log_->Log(LogLevel::Warn, "Unexpected FILE_COMMITTED ACK");
                ::shutdown(sock_, SD_BOTH);
                break;
            }
            awaitingCommit_.clear();
            if(durableQueue_) {
                if(!durableQueue_->AckSentPath(committed.relPath)) {
                    log_->Log(LogLevel::Error,"V8 durable queue ACK persistence failed; disconnecting to replay");
                    ::shutdown(sock_,SD_BOTH);
                    break;
                }
                if(durableQueue_->Size()==0) durableQueue_->CompactIfEmpty();
            }
            const bool unchanged = awaitingUnchanged_;
            awaitingUnchanged_ = false;
            // No per-file log or UI callback when the Server verified an
            // identical file. This stays quiet across process restarts.
            if (!unchanged) {
                log_->Log(LogLevel::Info, "File committed and SHA256 verified on server: " + committed.relPath);
                if (ui_) ui_->OnFileDone(committed.relPath, committed.finalSize);
            }
            break;
        }
        case MsgType::SYNC_REQUEST_INIT: {
            if (!awaitingCommit_.empty()) {
                log_->Log(LogLevel::Error, "Server requested next file before previous commit ACK");
                ::shutdown(sock_, SD_BOTH);
                break;
            }
            SyncRequestInitMsg m;
            if (Unmarshal(payload.data(), (uint32_t)payload.size(), m))
                HandleSyncRequestInit(fc, m);
            break;
        }
        case MsgType::SYNC_REQUEST_BLOCKS: {
            if (!awaitingCommit_.empty()) {
                log_->Log(LogLevel::Error, "Server sent blocks request before FILE_COMMITTED");
                ::shutdown(sock_, SD_BOTH);
                break;
            }
            SyncRequestBlocksMsg m;
            if (Unmarshal(payload.data(), (uint32_t)payload.size(), m))
                HandleSyncRequestBlocks(fc, m);
            break;
        }
        case MsgType::ERROR_MSG: {
            ErrorMsg m;
            if (Unmarshal(payload.data(), (uint32_t)payload.size(), m)) {
                log_->Log(LogLevel::Warn,
                          "Server error code=" + std::to_string((int)m.code) + " " + m.message);
                if (m.code == ErrorCode::Auth) SetState(ConnStateUi::AuthFail);
                // A failed file cannot complete: reconnect and rebuild the
                // sorted snapshot instead of waiting forever for its ACK.
                ::shutdown(sock_, SD_BOTH);
            }
            break;
        }
        default: break;
    }
}

void ClientEngine::DeferFile(FrameCodec& fc, const std::string& relPath,
                             const std::string& reason) {
    log_->Log(LogLevel::Warn, "File deferred: " + relPath + " (" + reason + ")");
    ResyncMsg deferred;
    deferred.relPath = relPath;
    std::vector<uint8_t> payload;
    std::string err;
    if (!Marshal(deferred, payload) ||
        !fc.SendFrame(MsgType::FILE_DEFERRED, payload.data(),
                      (uint32_t)payload.size(), 10000, stop_, err)) {
        log_->Log(LogLevel::Warn, "FILE_DEFERRED send failed: " + err);
        ::shutdown(sock_, SD_BOTH);
    }
}

void ClientEngine::HandleSyncRequestInit(FrameCodec& fc, const SyncRequestInitMsg& m) {
    std::wstring full = JoinPath(cfg_.watchRootUtf16, Utf8ToWide(m.relPath));
    FileBlockReader reader(full);
    std::vector<BlockEntry> blocks;
    if (!ComputeBlockHashes(reader, blocks)) {
        DeferFile(fc, m.relPath, "hash computation failed");
        return;
    }
    // Detect a file modified while its block hashes were being calculated.
    FileIo sourceMeta;
    int64_t sourceMtime = 0;
    if (!sourceMeta.OpenForRead(full) || !sourceMeta.GetMtimeNs(sourceMtime)) {
        DeferFile(fc, m.relPath, "source unavailable during hashing");
        return;
    }
    const uint64_t sourceSize = sourceMeta.Size();
    if (sourceSize != reader.Size()) {
        DeferFile(fc, m.relPath, "source size changed during hashing");
        return;
    }
    BlockHashesMsg bh;
    bh.relPath = m.relPath;
    bh.blocks.reserve(blocks.size());
    for (const auto& b : blocks) bh.blocks.push_back({ b.offset, b.crc32, b.size });
    int64_t afterMtime = 0;
    if (!sourceMeta.GetMtimeNs(afterMtime) || afterMtime != sourceMtime ||
        sourceMeta.Size() != sourceSize) {
        DeferFile(fc, m.relPath, "source changed while hashing");
        return;
    }
    std::vector<uint8_t> payload;
    if (!Marshal(bh, payload)) {
        DeferFile(fc, m.relPath, "block hash payload exceeds 1 MiB protocol limit");
        return;
    }
    std::string err;
    if (!fc.SendFrame(MsgType::BLOCK_HASHES, payload.data(),
                      (uint32_t)payload.size(), 10000, stop_, err)) {
        log_->Log(LogLevel::Warn, "BLOCK_HASHES send failed: " + err);
        ::shutdown(sock_, SD_BOTH);
    }
}

void ClientEngine::HandleSyncRequestBlocks(FrameCodec& fc,
                                           const SyncRequestBlocksMsg& m) {
    std::wstring full = JoinPath(cfg_.watchRootUtf16, Utf8ToWide(m.relPath));
    FileBlockReader reader(full);
    uint64_t size = reader.Size();
    FileIo sourceMeta;
    int64_t initialMtime = 0;
    if (!sourceMeta.OpenForRead(full) || !sourceMeta.GetMtimeNs(initialMtime)) {
        DeferFile(fc, m.relPath, "source unavailable for transfer");
        return;
    }
    uint8_t beforeSha[32];
    if (!ComputeFileSha256(reader, beforeSha) || sourceMeta.Size() != size) {
        DeferFile(fc, m.relPath, "source changed before transfer");
        return;
    }

    std::vector<uint8_t> buf(kMaxChunkData);
    for (uint64_t off : m.needOffsets) {
        uint64_t remain = (size > off) ? size - off : 0;
        uint32_t toRead = (uint32_t)(std::min<uint64_t>)(remain, kMaxChunkData);
        if (toRead == 0) continue;
        uint64_t got = reader.ReadAt(off, buf.data(), toRead);
        if (got == (uint64_t)-1 || got < toRead) {
            DeferFile(fc, m.relPath, "read failed at block " + std::to_string(off));
            return;
        }
        FileChunkMsg chunk;
        chunk.relPath = m.relPath;
        chunk.offset  = off;
        chunk.data.assign(buf.begin(), buf.begin() + got);
        std::vector<uint8_t> payload;
        if (!Marshal(chunk, payload)) {
            DeferFile(fc, m.relPath, "marshal chunk failed");
            return;
        }
        std::string err;
        if (!fc.SendFrame(MsgType::FILE_CHUNK, payload.data(),
                          (uint32_t)payload.size(), 10000, stop_, err)) {
            log_->Log(LogLevel::Warn, "Send chunk failed: " + err);
            ::shutdown(sock_, SD_BOTH); // Partial TCP frame: never reuse this stream.
            return;
        }
    }

    uint8_t sha[32];
    int64_t finalMtime = 0;
    if (!sourceMeta.GetMtimeNs(finalMtime) || finalMtime != initialMtime ||
        sourceMeta.Size() != size || !ComputeFileSha256(reader, sha) ||
        std::memcmp(beforeSha, sha, 32) != 0) {
        DeferFile(fc, m.relPath, "source changed during transfer");
        return;
    }
    FileDoneMsg done;
    done.relPath   = m.relPath;
    done.finalSize = size;
    std::memcpy(done.sha256, sha, 32);
    std::vector<uint8_t> payload;
    if (!Marshal(done, payload)) return;
    std::string err;
    if (!fc.SendFrame(MsgType::FILE_DONE, payload.data(), (uint32_t)payload.size(),
                      5000, stop_, err)) {
        log_->Log(LogLevel::Warn, "FILE_DONE send failed: " + err);
        ::shutdown(sock_, SD_BOTH);
        return;
    }
    // Do not declare success or send another file until FILE_COMMITTED.
    awaitingCommit_ = m.relPath;
    awaitingUnchanged_ = false;
    awaitingSize_ = size;
    std::memcpy(awaitingSha_, sha, 32);
    log_->Log(LogLevel::Info, "File bytes uploaded, waiting for Server SHA256/commit: " + m.relPath);
}

void ClientEngine::SetState(ConnStateUi s) {
    if (state_.exchange(s) != s) {
        if (ui_) ui_->OnConnState(s);
    }
}

// ============================ V5 Byte-Range Delta ============================
void ClientEngine::HandleV5SignaturePage(FrameCodec& fc,
                                         const std::vector<uint8_t>& wire) {
    V5SignaturePage page;
    if (!Unpack(wire.data(), (uint32_t)wire.size(), page) ||
        !awaitingCommit_.empty() || !page.blockSize ||
        page.blockSize > 1024u * 1024u ||
        (page.baseSize / page.blockSize > 260000u)) {
        log_->Log(LogLevel::Warn, "v5: malformed signature page");
        ::shutdown(sock_, SD_BOTH); return;
    }
    if (page.firstIndex == 0) {
        v5_ = V5Context{};
        v5_.path = page.path;
        v5_.expectedInstanceId = page.expectedInstanceId;
        v5_.baseSize = page.baseSize;
        v5_.blockSize = page.blockSize;
        v5_.baseSha = page.baseSha;
        v5_.prefix = page.prefix;
    }
    if (v5_.path.empty() || page.path != v5_.path ||
        !IsValidFileInstanceId(page.expectedInstanceId) ||
        page.expectedInstanceId != v5_.expectedInstanceId ||
        page.baseSize != v5_.baseSize || page.blockSize != v5_.blockSize ||
        page.baseSha != v5_.baseSha || page.prefix != v5_.prefix ||
        page.firstIndex != v5_.signatures.size() ||
        v5_.signatures.size() + page.items.size() > 260000u) {
        log_->Log(LogLevel::Warn, "v5: out-of-sequence signature catalogue");
        ::shutdown(sock_, SD_BOTH); return;
    }
    for (const auto& sig : page.items) {
        const uint64_t index = v5_.signatures.size();
        if (!sig.length || sig.length > page.blockSize ||
            sig.sourceOffset != index * uint64_t(page.blockSize) ||
            sig.sourceOffset >= page.baseSize ||
            sig.length != (std::min)(uint64_t(page.blockSize),
                                     page.baseSize - sig.sourceOffset)) {
            log_->Log(LogLevel::Warn, "v5: invalid signature entry");
            ::shutdown(sock_, SD_BOTH); return;
        }
        v5_.signatures.push_back(sig);
    }
    if (page.last) {
        const uint64_t count = page.baseSize / page.blockSize +
            ((page.baseSize % page.blockSize) != 0);
        if (count != v5_.signatures.size()) {
            log_->Log(LogLevel::Warn, "v5: truncated signature catalogue");
            ::shutdown(sock_, SD_BOTH); return;
        }
        SendV5Delta(fc);
    }
}

void ClientEngine::SendV5Delta(FrameCodec& fc) {
    const std::string path = v5_.path;
    std::wstring source;
    if (!PathSanitizer::Validate(cfg_.watchRootUtf16, path, source)) {
        log_->Log(LogLevel::Warn, "v5: server requested unsafe source path");
        ::shutdown(sock_, SD_BOTH); return;
    }
    const std::string currentId = GetFileInstanceId(source);
    if (currentId != v5_.expectedInstanceId) {
        DeferFile(fc, path, "source file was deleted/recreated");
        FileEventMsg changed{};
        changed.action = FileAction::Created;
        changed.relPath = path;
        SendFileEvent(fc, changed);
        return;
    }
    FileBlockReader sourceReader(source);
    FileIo sourceMeta;
    int64_t mtime = 0;
    if (!sourceMeta.OpenForRead(source) || !sourceMeta.GetMtimeNs(mtime)) {
        DeferFile(fc, path, "source unavailable");return;
    }
    const uint64_t size = sourceMeta.Size();
    uint8_t initialSha[32];
    if (!ComputeFileSha256(sourceReader, initialSha)) {
        DeferFile(fc, path, "source SHA256 failed");return;
    }
    // Fast path: the committed Server file is byte-identical. Avoid copying
    // the full base into a temp file and avoid streaming any file data.
    if (size == v5_.baseSize &&
        std::memcmp(initialSha, v5_.baseSha.data(), 32) == 0) {
        int64_t nowMtime = 0; uint8_t checked[32];
        if (GetFileInstanceId(source) != currentId ||
            !sourceMeta.GetMtimeNs(nowMtime) || nowMtime != mtime ||
            sourceMeta.Size() != size ||
            !ComputeFileSha256(sourceReader, checked) ||
            std::memcmp(initialSha, checked, 32) != 0) {
            DeferFile(fc, path, "unchanged source changed during verification");return;
        }
        FileDoneMsg confirmed{};
        confirmed.relPath=path;confirmed.finalSize=size;
        std::memcpy(confirmed.sha256,initialSha,32);
        std::vector<uint8_t> message; std::string sendError;
        if (!Marshal(confirmed,message) ||
            !fc.SendFrame(MsgType::V5_UNCHANGED,message.data(),
                 (uint32_t)message.size(),30000,stop_,sendError)) {
            log_->Log(LogLevel::Warn,"v5: unchanged ACK send failed: "+sendError);
            ::shutdown(sock_,SD_BOTH);return;
        }
        awaitingCommit_=path;awaitingSize_=size;
        awaitingUnchanged_ = true;
        std::memcpy(awaitingSha_,initialSha,32);
        // Silent unchanged: still await the Server SHA-256 verification ACK.
        return;
    }
    std::vector<ByteOp> operations;
    if (!BuildBytePlan(size,
            [&](uint64_t pos, void* b, uint32_t n) {return sourceReader.ReadAt(pos,b,n);},
            v5_.blockSize, v5_.signatures, v5_.prefix, operations)) {
        DeferFile(fc, path, "delta plan failed (file busy or too many fragments)");return;
    }
    uint8_t checkedSha[32]; int64_t checkedTime = 0;
    if (GetFileInstanceId(source) != currentId ||
        !ComputeFileSha256(sourceReader, checkedSha) ||
        !sourceMeta.GetMtimeNs(checkedTime) || mtime != checkedTime ||
        sourceMeta.Size() != size || std::memcmp(initialSha,checkedSha,32) != 0) {
        DeferFile(fc, path, "source changed while building delta");return;
    }
    V5Begin begin;
    begin.path=path; begin.size=size;
    std::memcpy(begin.targetSha.data(), initialSha, 32);
    begin.baseSha=v5_.baseSha;
    std::vector<uint8_t> payload;
    std::string err;
    if (!Pack(begin,payload) || !fc.SendFrame(MsgType::V5_BEGIN,payload.data(),
          (uint32_t)payload.size(),30000,stop_,err)) {
        log_->Log(LogLevel::Warn,"v5: BEGIN send failed: "+err);
        ::shutdown(sock_,SD_BOTH);return;
    }
    uint64_t copied = 0, literals = 0, resent = 0;
    for (const auto& op : operations) {
        if (stop_.load()) return;
        if (op.kind == ByteOp::Copy) {
            V5Copy cmd;cmd.path=path;cmd.dest=op.destination;
            cmd.source=op.source;cmd.length=op.length;
            if (!Pack(cmd,payload) || !fc.SendFrame(MsgType::V5_COPY,payload.data(),
                  (uint32_t)payload.size(),30000,stop_,err)) {
                log_->Log(LogLevel::Warn,"v5: COPY send failed: "+err);
                ::shutdown(sock_,SD_BOTH);return;
            }
            copied += op.length;
        } else {
            // At most 64 KiB of new content held in memory per operation.
            std::vector<uint8_t> bytes(op.length);
            if (sourceReader.ReadAt(op.destination,bytes.data(),op.length)!=op.length) {
                DeferFile(fc,path,"read literal failed");return;
            }
            V5Check cmd;cmd.path=path;cmd.dest=op.destination;cmd.length=op.length;
            if (!Sha256::HashBuffer(bytes.data(),bytes.size(),cmd.sha.data()) ||
                !Pack(cmd,payload) || !fc.SendFrame(MsgType::V5_CHECK,payload.data(),
                   (uint32_t)payload.size(),30000,stop_,err)) {
                log_->Log(LogLevel::Warn,"v5: literal CHECK failed: "+err);
                ::shutdown(sock_,SD_BOTH);return;
            }
            // The receiver responds BEFORE Client sends new data. On reconnect
            // it can reuse SHA-verified bytes already present in the stage.
            bool answered = false, need = false;
            while (!answered && !stop_.load()) {
                MsgType type;std::vector<uint8_t> resp;
                if (!fc.RecvFrame(type,resp,60000,stop_,err)) {
                    log_->Log(LogLevel::Warn,"v5: waiting literal ACK failed: "+err);
                    ::shutdown(sock_,SD_BOTH);return;
                }
                if (type == MsgType::HEARTBEAT) {
                    std::string ignored;
                    fc.SendFrame(MsgType::HEARTBEAT_ACK,nullptr,0,1000,stop_,ignored);
                } else if (type == MsgType::HEARTBEAT_ACK) {
                    continue;
                } else if (type == MsgType::V5_NEED) {
                    V5Need answer;
                    if (!Unpack(resp.data(),(uint32_t)resp.size(),answer) ||
                        answer.path != path || answer.dest != op.destination) {
                        log_->Log(LogLevel::Warn,"v5: incorrect NEED response");
                        ::shutdown(sock_,SD_BOTH);return;
                    }
                    need=answer.need;answered=true;
                } else {
                    log_->Log(LogLevel::Warn,"v5: unexpected server frame during literal ACK");
                    ::shutdown(sock_,SD_BOTH);return;
                }
            }
            if (!answered) return;
            literals += op.length;
            if (need) {
                V5Bytes chunk;chunk.path=path;chunk.dest=op.destination;
                chunk.data=std::move(bytes);
                if (!Pack(chunk,payload) || !fc.SendFrame(MsgType::V5_BYTES,payload.data(),
                       (uint32_t)payload.size(),60000,stop_,err)) {
                    log_->Log(LogLevel::Warn,"v5: literal BYTES failed: "+err);
                    ::shutdown(sock_,SD_BOTH);return;
                }
                resent += op.length;
            }
        }
    }
    uint8_t finalSha[32]; int64_t finalMtime=0;
    if (GetFileInstanceId(source) != currentId ||
        !sourceMeta.GetMtimeNs(finalMtime) || finalMtime!=mtime ||
        sourceMeta.Size()!=size || !ComputeFileSha256(sourceReader,finalSha) ||
        std::memcmp(finalSha,initialSha,32)!=0) {
        DeferFile(fc,path,"source changed during delta transfer");return;
    }
    FileDoneMsg done{};done.relPath=path;done.finalSize=size;
    std::memcpy(done.sha256,finalSha,32);
    if (!Marshal(done,payload) || !fc.SendFrame(MsgType::V5_FINISH,payload.data(),
          (uint32_t)payload.size(),30000,stop_,err)) {
        log_->Log(LogLevel::Warn,"v5: FINISH failed: "+err);
        ::shutdown(sock_,SD_BOTH);return;
    }
    awaitingCommit_=path;
    awaitingUnchanged_ = false;
    awaitingSize_=size;
    std::memcpy(awaitingSha_,finalSha,32);
    log_->Log(LogLevel::Info,"v5: awaiting server commit " + path +
        " TotalBytes="+std::to_string(size)+
        " CopiedBytes="+std::to_string(copied)+
        " LiteralBytes="+std::to_string(literals)+
        " SentBytes="+std::to_string(resent));
}

} // namespace fsyn
