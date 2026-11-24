#include "pch.h"
#include "ServerEngine.h"
#include "Logger.h"
#include "ClientSession.h"
#include "SchedulerThread.h"
#include "Util.h"
#include "IocpAcceptLoop.h"
#include "PerfDiagnostics.h"
#include "ServerNetworkSampler.h"
#include "DurabilityTelemetry.h"
#include "DurableWalGroupCommit.h"
#include <cstdlib>

namespace fsyn {

ServerEngine::ServerEngine(const Config& cfg, IUiCallbacks* ui)
    : cfg_(cfg), ui_(ui), log_(std::make_unique<Logger>()) {
    filter_.Parse(cfg_.extFilterSpec);
    sched_ = std::make_unique<Scheduler>(cfg_.sched);
}

ServerEngine::~ServerEngine() { Stop(); }

bool ServerEngine::Start() {
    if (running_.load()) return true;

    // Logger
    wchar_t exe[MAX_PATH] = { 0 };
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring dir(exe);
    size_t slash = dir.find_last_of(L"\\/");
    if (slash != std::wstring::npos) dir.resize(slash);
    log_->Open(JoinPath(dir, L"server_conn.log"));
    if (ui_) log_->SetSink(LoggerSinkToUi, ui_);
    log_->Log(LogLevel::Info, "ServerEngine: starting");
    // Optional operational override. Restrict the value to prevent unbounded
    // worker-thread allocations; stress testing is still mandatory.
    char maxBuf[24]{};
    const DWORD length = GetEnvironmentVariableA("FILESYNC_MAX_CLIENTS", maxBuf, sizeof(maxBuf));
    if (!cfg_.limitsFromDialog && length > 0 && length < sizeof(maxBuf)) {
        char* end = nullptr;
        const unsigned long parsed = std::strtoul(maxBuf, &end, 10);
        if (end != maxBuf && *end == '\0' && parsed >= 128 && parsed <= 4096)
            cfg_.maxClients = static_cast<size_t>(parsed);
        else
            log_->Log(LogLevel::Warn, "Invalid FILESYNC_MAX_CLIENTS; using configured default");
    }
    log_->Log(LogLevel::Info, "Connection admission limit: " + std::to_string(cfg_.maxClients) +
        (cfg_.limitsFromDialog ? " (dialog setting)" : " (config / optional env override)"));
    // Receive-side shared budget; do NOT confuse it with a hard wire-rate QoS.
    char setting[32]{};
    DWORD n=GetEnvironmentVariableA("FILESYNC_ACTIVE_TRANSFERS",setting,sizeof(setting));
    if(!cfg_.limitsFromDialog && n>0 && n<sizeof(setting)) {
        char* end=nullptr; unsigned long v=std::strtoul(setting,&end,10);
        if(end!=setting && *end=='\0' && v>=1 && v<=64) cfg_.maxActiveTransfers=static_cast<unsigned>(v);
    }
    // An explicit legacy global limit selects fixed mode for A/B regression.
    n=GetEnvironmentVariableA("FILESYNC_GLOBAL_MIBPS",setting,sizeof(setting));
    if(n>0 && n<sizeof(setting)) {
        char* end=nullptr; unsigned long v=std::strtoul(setting,&end,10);
        if(end!=setting && *end=='\0' && v>=1 && v<=50) {
            cfg_.inboundBytesPerSecond=uint64_t(v)*1024u*1024u;
            cfg_.adaptiveServerFlow=false;
        }
    }
    n=GetEnvironmentVariableA("FILESYNC_ADAPTIVE_MAX_MIBPS",setting,sizeof(setting));
    if(n>0 && n<sizeof(setting)) {
        char* end=nullptr; unsigned long v=std::strtoul(setting,&end,10);
        if(end!=setting && *end=='\0' && v>=2 && v<=100)
            cfg_.adaptiveMaximumBytesPerSecond=uint64_t(v)*1024u*1024u;
    }
    n=GetEnvironmentVariableA("FILESYNC_SPOOL_LIMIT_GIB",setting,sizeof(setting));
    if(n>0 && n<sizeof(setting)) {
        char* end=nullptr; unsigned long v=std::strtoul(setting,&end,10);
        if(end!=setting && *end=='\0' && v>=1 && v<=1024)
            cfg_.maxInventorySpoolBytes=uint64_t(v)*1024u*1024u*1024u;
    }
    transferControl_=std::make_shared<GlobalTransferControl>(
        cfg_.maxActiveTransfers,cfg_.inboundBytesPerSecond,cfg_.maxInventorySpoolBytes,
        cfg_.adaptiveServerFlow,cfg_.adaptiveMaximumBytesPerSecond);
    log_->Log(LogLevel::Info,"Global active-file limit="+
        std::to_string(cfg_.maxActiveTransfers)+" receive pacing="+
        std::to_string(cfg_.inboundBytesPerSecond)+" B/s initial (best effort), mode="+
        (cfg_.adaptiveServerFlow ? "adaptive" : "fixed")+
        " ceiling="+std::to_string(cfg_.adaptiveMaximumBytesPerSecond));

    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        log_->Log(LogLevel::Error, "WSAStartup failed");
        return false;
    }

    listenSock_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listenSock_ == INVALID_SOCKET) {
        log_->LogWin32(LogLevel::Error, "socket()", WSAGetLastError());
        WSACleanup();
        return false;
    }

    BOOL reuse = TRUE;
    ::setsockopt(listenSock_, SOL_SOCKET, SO_REUSEADDR,
                 (const char*)&reuse, sizeof(reuse));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(cfg_.port);

    if (::bind(listenSock_, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        log_->LogWin32(LogLevel::Error, "bind", WSAGetLastError());
        ::closesocket(listenSock_); listenSock_ = INVALID_SOCKET;
        WSACleanup();
        return false;
    }
    if (::listen(listenSock_, SOMAXCONN) == SOCKET_ERROR) {
        log_->LogWin32(LogLevel::Error, "listen", WSAGetLastError());
        ::closesocket(listenSock_); listenSock_ = INVALID_SOCKET;
        WSACleanup();
        return false;
    }

    stop_.store(false);
    running_.store(true);

    acceptThread_ = std::thread(&ServerEngine::AcceptThreadProc, this);
    if(cfg_.adaptiveServerFlow)
        flowMonitorThread_ = std::thread(&ServerEngine::FlowMonitorProc,this);

    schedThread_ = std::make_unique<SchedulerThread>(
        sched_.get(), [this](bool in) { OnSchedulerFlip(in); });
    schedThread_->Start();

    log_->Log(LogLevel::Info, "ServerEngine: listening on port " + std::to_string(cfg_.port));
    return true;
}

void ServerEngine::Stop() {
    if (!running_.load()) return;
    stop_.store(true);

    if (schedThread_) schedThread_->Stop();
    if (listenSock_ != INVALID_SOCKET) {
        ::shutdown(listenSock_, SD_BOTH);
        ::closesocket(listenSock_);
        listenSock_ = INVALID_SOCKET;
    }
    if (acceptThread_.joinable()) acceptThread_.join();
    if (flowMonitorThread_.joinable()) flowMonitorThread_.join();

    // Never block joining a session worker while holding sessionsMu_.
    std::vector<std::unique_ptr<ClientSession>> stopping;
    {
        std::lock_guard<std::mutex> g(sessionsMu_);
        stopping.swap(sessions_);
    }
    for(auto& s:stopping) s->Stop();
    stopping.clear();

    if(perf::Enabled()) {
        const auto d=durability::Read();
        log_->Log(LogLevel::Info,"PERF metadata_flush baseline_count="+
            std::to_string(d.baselineCount)+" baseline_us="+std::to_string(d.baselineUs)+
            " manifest_count="+std::to_string(d.manifestCount)+
            " manifest_us="+std::to_string(d.manifestUs));
        char v85[8]{};
        if(GetEnvironmentVariableA("FILESYNC_EXPERIMENTAL_LSM_WAL",v85,sizeof(v85))==1 &&
           v85[0]=='1') {
            const auto wal=DurableWalGroupCommit::Global().Stats();
            log_->Log(LogLevel::Info,"PERF v85_group_wal records="+
                std::to_string(wal.records)+" wal_flush_count="+
                std::to_string(wal.flushes)+" wal_flush_us="+
                std::to_string(wal.totalFlushUs)+" failed_batches="+
                std::to_string(wal.failedBatches));
        }
    }
    if(perf::Enabled() && transferControl_) {
        log_->Log(LogLevel::Info,"PERF server_pacing rate_bytes_per_sec="+
            std::to_string(transferControl_->BytesPerSecond())+
            " received_bytes="+std::to_string(transferControl_->ReceivedBytes())+
            " wait_us="+std::to_string(transferControl_->PaceWaitUs()));
    }
    WSACleanup();
    log_->Log(LogLevel::Info, "ServerEngine: stopped");
    running_.store(false);
    log_->Close();
}

void ServerEngine::FlowMonitorProc() {
    ServerNetworkSampler sampler;
    unsigned elapsed=0;
    while(!stop_.load()) {
        for(int i=0;i<10 && !stop_.load();++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if(stop_.load()) break;
        const double external=sampler.MeasureOtherFraction(
            transferControl_->ReceivedBytes(),cfg_.adaptiveMaximumBytesPerSecond);
        transferControl_->UpdateAdaptive(external);
        if(perf::Enabled() && ++elapsed%10==0) {
            log_->Log(LogLevel::Info,"PERF server_flow mode=adaptive effective_bytes_per_sec="+
                std::to_string(transferControl_->BytesPerSecond())+
                " external_fraction="+std::to_string(external)+
                " active="+std::to_string(transferControl_->Active()));
        }
    }
}

void ServerEngine::AcceptThreadProc() {
    // Stage 1 only: IOCP handles accepts, existing ClientSession retains its
    // threads and synchronous framing. This flag is not a scale certification.
    char flag[8]{};
    if(GetEnvironmentVariableA("FILESYNC_EXPERIMENTAL_ACCEPTEX",flag,sizeof(flag))==1 && flag[0]=='1') {
        log_->Log(LogLevel::Warn,"Experimental IOCP/AcceptEx acceptor enabled; session I/O remains thread-per-client");
        IocpAcceptLoop acceptor;
        if(!acceptor.Run(listenSock_,stop_,[this](SOCKET client,const sockaddr_in& peer){
           AdmitSocket(client,peer);
        })) log_->Log(LogLevel::Error,"AcceptEx/IOCP accept loop failed or shutdown drain incomplete");
        return;
    }
    while (!stop_.load()) {
        fd_set rset; FD_ZERO(&rset); FD_SET(listenSock_, &rset);
        timeval tv{ 0, 500000 };
        int sr = ::select(0, &rset, nullptr, nullptr, &tv);
        if (sr == SOCKET_ERROR) {
            log_->LogWin32(LogLevel::Error, "select(accept)", WSAGetLastError());
            break;
        }
        if (sr == 0) continue;

        sockaddr_in peer{};
        int peerLen = sizeof(peer);
        SOCKET client = ::accept(listenSock_, (sockaddr*)&peer, &peerLen);
        if (client == INVALID_SOCKET) {
            if (stop_.load()) break;
            log_->LogWin32(LogLevel::Warn, "accept", WSAGetLastError());
            continue;
        }

        AdmitSocket(client,peer);
    }
}

void ServerEngine::AdmitSocket(SOCKET client,const sockaddr_in& peer) {
        // Reap completed sessions before enforcing the active-connection limit.
        // Destruction joins worker threads; never join while holding sessionsMu_.
        std::vector<std::unique_ptr<ClientSession>> finished;
        {
            std::lock_guard<std::mutex> g(sessionsMu_);
            auto it = sessions_.begin();
            while (it != sessions_.end()) {
                if (!(*it)->IsAlive()) {
                    finished.push_back(std::move(*it));
                    it = sessions_.erase(it);
                } else ++it;
            }
        }
        if (!finished.empty() && ui_) ui_->OnClientListChanged();
        finished.clear();

        // Đếm session hiện có
        {
            std::lock_guard<std::mutex> g(sessionsMu_);
            if (sessions_.size() >= cfg_.maxClients) {
                log_->Log(LogLevel::Warn, "Too many clients, rejecting");
                ::closesocket(client);
                return;
            }
        }

        char ipBuf[64] = { 0 };
        inet_ntop(AF_INET, &peer.sin_addr, ipBuf, sizeof(ipBuf));

        ClientSession::Params p;
        p.sock = client;
        p.peerIp = ipBuf;
        p.serverRootUtf16 = cfg_.serverRootUtf16;
        p.expectedToken = cfg_.token;
        p.filter = filter_;
        p.transferControl = transferControl_;

        auto session = std::make_unique<ClientSession>(std::move(p), ui_, log_.get());
        session->Start();

        {
            std::lock_guard<std::mutex> g(sessionsMu_);
            sessions_.push_back(std::move(session));
        }
}

void ServerEngine::OnSessionDead(ClientSession* /*s*/) {
    // Cleanup is performed by the accept loop; never destroy a session
    // on its own worker thread or while holding sessionsMu_.
    if (ui_) ui_->OnClientListChanged();
}

void ServerEngine::OnSchedulerFlip(bool inWindow) {
    log_->Log(LogLevel::Info, inWindow
        ? "Scheduler: enter IN_WINDOW"
        : "Scheduler: enter OUT_OF_WINDOW");
    std::lock_guard<std::mutex> g(sessionsMu_);
    for (auto& s : sessions_) {
        if (s->IsAlive()) s->SetInWindow(inWindow);
    }
}

TransferProgress ServerEngine::GetProgress() const {
    TransferProgress total;
    std::lock_guard<std::mutex> g(sessionsMu_);
    for (const auto& session : sessions_) {
        if (session->IsAlive()) total.Accumulate(session->GetProgress());
    }
    return total;
}

std::vector<std::pair<std::string, std::string>> ServerEngine::ClientList() const {
    std::vector<std::pair<std::string, std::string>> out;
    std::lock_guard<std::mutex> g(sessionsMu_);
    for (const auto& s : sessions_) {
        if (s->IsAlive()) out.emplace_back(s->HostKey(), s->PeerIp());
    }
    return out;
}

} // namespace fsyn

namespace fsyn {
std::vector<ClientConnectionInfo> ServerEngine::ClientListDetails() const {
    std::vector<ClientConnectionInfo> out;
    std::lock_guard<std::mutex> g(sessionsMu_);
    for (const auto& session : sessions_) {
        if (session->IsAlive()) {
            auto info = session->ConnectionInfo();
            if (!info.hostKey.empty() && !info.ip.empty()) out.push_back(std::move(info));
        }
    }
    return out;
}
} // namespace fsyn
