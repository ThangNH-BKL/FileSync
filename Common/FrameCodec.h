#pragma once
// FrameCodec.h — Đóng/gói frame và I/O có timeout.
// recv_full/send_full đều kiểm tra stop cờ để thoát sạch khi shutdown.

#include "Protocol.h"
#include <atomic>

namespace fsyn {

struct AdaptiveBandwidthStats {
    uint64_t bytes=0, frames=0, waitUs=0;
};
AdaptiveBandwidthStats ReadAdaptiveBandwidthStats();

class FrameCodec {
public:
    explicit FrameCodec(SOCKET s) : sock_(s) {}

    // Gửi 1 frame = header 20B + payload. Tự tăng seq_.
    // Trả false nếu socket lỗi, timeout, hoặc stop.
    bool SendFrame(MsgType type,
                   const uint8_t* payload, uint32_t payloadLen,
                   uint32_t timeoutMs,
                   const std::atomic<bool>& stop,
                   std::string& err);

    // Nhận 1 frame. Trả false nếu lỗi/timeout/stop/magic/version sai.
    // Nếu payloadLen > kMaxPayload → trả false và đặt err = "oversize".
    bool RecvFrame(MsgType& outType, std::vector<uint8_t>& outPayload,
                   uint32_t timeoutMs,
                   const std::atomic<bool>& stop,
                   std::string& err);

    // Heartbeat is unnecessary while real protocol traffic is flowing.
    // Prevents a control sender from competing with a busy FILE_CHUNK writer.
    bool RecentlyActive(uint32_t intervalMs) const;

    uint32_t NextSeq() const { return seq_; }

private:
    SOCKET   sock_;
    uint32_t seq_ = 0;
};

// Hàm tiện ích: gửi message đã marshal
bool SendMessage(SOCKET s, MsgType type, const std::vector<uint8_t>& payload,
                 uint32_t timeoutMs, const std::atomic<bool>& stop,
                 std::string& err);

// Hàm đọc đủ `len` byte với timeout, kiểm tra stop định kỳ (mỗi ~200ms).
bool RecvFull(SOCKET s, void* buf, uint32_t len,
              uint32_t timeoutMs, const std::atomic<bool>& stop,
              std::string& err);

// Hàm gửi đủ `len` byte với timeout.
bool SendFull(SOCKET s, const void* buf, uint32_t len,
              uint32_t timeoutMs, const std::atomic<bool>& stop,
              std::string& err);

} // namespace fsyn
