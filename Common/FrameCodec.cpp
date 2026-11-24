#include "pch.h"
#include "FrameCodec.h"
#include "PerfDiagnostics.h"
#include "Util.h"   // ErrStr(WSAGetLastError)
#include <iphlpapi.h>
#include <netioapi.h>
#include <thread>
#include <condition_variable>
#include <array>
#include <chrono>
#pragma comment(lib, "Iphlpapi.lib")

namespace fsyn {
namespace {

// Global per-process limiter: all FILE_CHUNK traffic shares one budget.
// Uses interface octet counters and estimated FileSync bytes to distinguish
// other applications' traffic. No network sampling means conservative rate cap.
class AdaptiveBandwidth {
public:
    bool BeforeChunk(uint32_t bytes, const std::atomic<bool>& stop) {
        using clock = std::chrono::steady_clock;
        const bool trace=perf::Enabled();
        const auto entered=perf::Clock::now();
        std::unique_lock<std::mutex> lock(mu_);
        for (;;) {
            if (stop.load(std::memory_order_relaxed)) return false;
            auto now = clock::now();
            if (now - sampledAt_ >= std::chrono::milliseconds(1000)) Sample(now);
            // Pause only for OTHER applications' traffic. Own FileSync traffic
            // must not cause self-triggered indefinite pauses.
            if (otherUtilization_ >= 0.50) {
                lock.unlock();
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                lock.lock();
                continue;
            }
            // 100 MB/s absolute ceiling; on slower links leave room for others.
            const double bytesPerSec = (std::min)(100000000.0,
                linkBytesPerSec_ > 0.0 ? linkBytesPerSec_ * 0.40 : 5000000.0);
            if (now > nextSend_) nextSend_ = now;
            const auto delay = std::chrono::duration<double>(bytes / bytesPerSec);
            auto due = nextSend_;
            if (due <= now) {
                nextSend_ = now + std::chrono::duration_cast<clock::duration>(delay);
                accountedBytes_ += bytes;
                if(trace) {
                    statsBytes_.fetch_add(bytes,std::memory_order_relaxed);
                    statsFrames_.fetch_add(1,std::memory_order_relaxed);
                    statsWaitUs_.fetch_add(perf::Us(entered,perf::Clock::now()),std::memory_order_relaxed);
                }
                return true;
            }
            lock.unlock();
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            lock.lock();
        }
    }
    AdaptiveBandwidthStats Stats() const {
        return {statsBytes_.load(std::memory_order_relaxed),
                statsFrames_.load(std::memory_order_relaxed),
                statsWaitUs_.load(std::memory_order_relaxed)};
    }
private:
    std::atomic<uint64_t> statsBytes_{0},statsFrames_{0},statsWaitUs_{0};
    using clock = std::chrono::steady_clock;
    void Sample(clock::time_point now) {
        PMIB_IF_TABLE2 table = nullptr;
        const auto elapsed = std::chrono::duration<double>(now - sampledAt_).count();
        if (GetIfTable2(&table) == NO_ERROR && table) {
            double maxOther = 0.0;
            double activeSpeed = 0.0;
            for (ULONG i = 0; i < table->NumEntries; ++i) {
                const auto& r = table->Table[i];
                if (r.OperStatus != IfOperStatusUp || r.Type == IF_TYPE_SOFTWARE_LOOPBACK) continue;
                const double speed = static_cast<double>((std::min)(r.TransmitLinkSpeed, r.ReceiveLinkSpeed)) / 8.0;
                if (speed <= 0.0) continue;
                const uint64_t count = r.InOctets + r.OutOctets;
                auto it = lastCounts_.find(r.InterfaceLuid.Value);
                if (it != lastCounts_.end() && elapsed > 0.0 && count >= it->second) {
                    const double observed = (count - it->second) / elapsed;
                    // FileSync sends FILE_CHUNK only in this protocol; estimate
                    // its contribution to avoid self-induced pause.
                    const double external = (std::max)(0.0, observed - accountedBytes_ / elapsed);
                    const double fraction = external / speed;
                    if (fraction > maxOther) { maxOther = fraction; activeSpeed = speed; }
                }
                lastCounts_[r.InterfaceLuid.Value] = count;
            }
            otherUtilization_ = maxOther;
            if (activeSpeed > 0.0) linkBytesPerSec_ = activeSpeed;
            FreeMibTable(table);
        }
        accountedBytes_ = 0.0;
        sampledAt_ = now;
    }
    std::mutex mu_;
    clock::time_point sampledAt_ = clock::now();
    clock::time_point nextSend_ = clock::now();
    std::unordered_map<ULONG64, uint64_t> lastCounts_;
    double accountedBytes_ = 0.0;
    double otherUtilization_ = 0.0;
    double linkBytesPerSec_ = 0.0;
};
AdaptiveBandwidth& Bandwidth() { static AdaptiveBandwidth b; return b; }
// Coarse per-socket traffic clock. A hash collision only suppresses an optional
// heartbeat; it never affects payload frames or their integrity.
std::array<std::atomic<uint64_t>, 4096>& TrafficClocks() {
    static std::array<std::atomic<uint64_t>, 4096> clocks{};
    return clocks;
}
uint64_t TickMs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}
size_t TrafficSlot(SOCKET s) { return static_cast<size_t>(s) % TrafficClocks().size(); }
void TouchTraffic(SOCKET s) {
    TrafficClocks()[TrafficSlot(s)].store(TickMs(), std::memory_order_relaxed);
}
// A socket is shared by data and heartbeat senders. Serialize complete frames,
// including their header and payload, even when callers use different FrameCodec objects.
std::mutex& SocketSendMutex(SOCKET sock) {
    static std::array<std::mutex, 257> locks;
    return locks[static_cast<size_t>(sock) % locks.size()];
}


// Chờ socket sẵn sàng đọc/ghi trong tối đa timeoutMs, kiểm tra stop mỗi 200ms.
// Trả: 1 = ready, 0 = timeout hết, -1 = lỗi, -2 = stop.
int WaitSocket(SOCKET s, bool forRead, uint32_t timeoutMs,
               const std::atomic<bool>& stop) {
    using clock = std::chrono::steady_clock;
    auto deadline = clock::now() + std::chrono::milliseconds(timeoutMs);

    while (true) {
        if (stop.load(std::memory_order_relaxed)) return -2;

        auto now = clock::now();
        if (now >= deadline) return 0;

        uint32_t remain = (uint32_t)std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
        uint32_t slice  = remain < 200u ? remain : 200u;

        fd_set set;
        FD_ZERO(&set);
        FD_SET(s, &set);
        timeval tv;
        tv.tv_sec  = (long)(slice / 1000);
        tv.tv_usec = (long)((slice % 1000) * 1000);

        int r = ::select(0, forRead ? &set : nullptr, forRead ? nullptr : &set,
                         nullptr, &tv);
        if (r > 0) return 1;
        if (r == 0) continue;           // slice hết, lặp lại
        int e = WSAGetLastError();
        if (e == WSAEINTR) continue;
        return -1;
    }
}

} // namespace
AdaptiveBandwidthStats ReadAdaptiveBandwidthStats() { return Bandwidth().Stats(); }

bool RecvFull(SOCKET s, void* buf, uint32_t len,
              uint32_t timeoutMs, const std::atomic<bool>& stop,
              std::string& err) {
    uint8_t* p = (uint8_t*)buf;
    uint32_t got = 0;
    while (got < len) {
        int w = WaitSocket(s, true, timeoutMs, stop);
        if (w == -2) { err = "stopped";  return false; }
        if (w == 0)  { err = got ? "partial frame timeout" : "recv timeout"; return false; }
        if (w == -1) { err = "select: " + ErrStr(WSAGetLastError()); return false; }

        int r = ::recv(s, (char*)(p + got), (int)(len - got), 0);
        if (r == 0) { err = "peer closed"; return false; }
        if (r == SOCKET_ERROR) {
            int e = WSAGetLastError();
            if (e == WSAEWOULDBLOCK) continue;
            if (e == WSAEINTR) continue;
            err = "recv: " + ErrStr(e);
            return false;
        }
        got += (uint32_t)r;
    }
    return true;
}

bool SendFull(SOCKET s, const void* buf, uint32_t len,
              uint32_t timeoutMs, const std::atomic<bool>& stop,
              std::string& err) {
    const uint8_t* p = (const uint8_t*)buf;
    uint32_t sent = 0;
    while (sent < len) {
        int w = WaitSocket(s, false, timeoutMs, stop);
        if (w == -2) { err = "stopped";      return false; }
        if (w == 0)  { err = "send timeout"; return false; }
        if (w == -1) { err = "select: " + ErrStr(WSAGetLastError()); return false; }

        int r = ::send(s, (const char*)(p + sent), (int)(len - sent), 0);
        if (r == SOCKET_ERROR) {
            int e = WSAGetLastError();
            if (e == WSAEWOULDBLOCK) continue;
            if (e == WSAEINTR) continue;
            err = "send: " + ErrStr(e);
            return false;
        }
        sent += (uint32_t)r;
    }
    return true;
}

bool FrameCodec::RecentlyActive(uint32_t intervalMs) const {
    const uint64_t last = TrafficClocks()[TrafficSlot(sock_)].load(std::memory_order_relaxed);
    const uint64_t now = TickMs();
    return last != 0 && now >= last && now - last < intervalMs;
}

bool FrameCodec::SendFrame(MsgType type,
                           const uint8_t* payload, uint32_t payloadLen,
                           uint32_t timeoutMs,
                           const std::atomic<bool>& stop,
                           std::string& err) {
    if (payloadLen > kMaxPayload) { err = "payload too large"; return false; }
    if ((type == MsgType::FILE_CHUNK || type == MsgType::V5_BYTES || type == MsgType::V5_SIGNATURE_PAGE) && !Bandwidth().BeforeChunk(payloadLen + kHeaderSize, stop)) {
        err = "stopped during bandwidth wait"; return false;
    }

    std::lock_guard<std::mutex> sendGuard(SocketSendMutex(sock_));
    FrameHeader h{};
    h.magic      = kMagic;
    h.version    = kVersion;
    h.type       = (uint16_t)type;
    h.payloadLen = payloadLen;
    h.seq        = seq_++;
    h.reserved   = 0;

    if (!SendFull(sock_, &h, kHeaderSize, timeoutMs, stop, err)) return false;
    if (payloadLen > 0) {
        if (!SendFull(sock_, payload, payloadLen, timeoutMs, stop, err)) return false;
    }
    TouchTraffic(sock_);
    return true;
}

bool FrameCodec::RecvFrame(MsgType& outType, std::vector<uint8_t>& outPayload,
                           uint32_t timeoutMs,
                           const std::atomic<bool>& stop,
                           std::string& err) {
    FrameHeader h{};
    if (!RecvFull(sock_, &h, kHeaderSize, timeoutMs, stop, err)) return false;

    if (h.magic != kMagic)     { err = "bad magic";    return false; }
    if (h.version != kVersion) { err = "bad version";  return false; }
    if (h.reserved != 0)       { err = "bad reserved"; return false; }
    if (h.payloadLen > kMaxPayload) { err = "payload too large"; return false; }

    outType = (MsgType)h.type;
    outPayload.resize(h.payloadLen);
    if (h.payloadLen > 0) {
        if (!RecvFull(sock_, outPayload.data(), h.payloadLen, timeoutMs, stop, err)) {
            // Even if zero payload bytes arrived, the header has already been
            // consumed. Never interpret the delayed body as a fresh header.
            if (err == "recv timeout") err = "partial frame timeout";
            return false;
        }
    }
    TouchTraffic(sock_);
    return true;
}

bool SendMessage(SOCKET s, MsgType type, const std::vector<uint8_t>& payload,
                 uint32_t timeoutMs, const std::atomic<bool>& stop,
                 std::string& err) {
    FrameCodec c(s);  // seq tạm; KHÔNG dùng lại cho socket lâu dài
    return c.SendFrame(type, payload.data(), (uint32_t)payload.size(),
                       timeoutMs, stop, err);
}

} // namespace fsyn
