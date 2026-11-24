#include "framework.h"
#include "MiniTest.h"
#include "FrameCodec.h"
#include "Protocol.h"
#include <atomic>

using namespace fsyn;
namespace {
void MakeSocketPair(SOCKET& a, SOCKET& b) {
    SOCKET lst = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in addr{}; addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    bind(lst, (sockaddr*)&addr, sizeof(addr));
    int len = sizeof(addr);
    getsockname(lst, (sockaddr*)&addr, &len);
    listen(lst, 1);
    b = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    connect(b, (sockaddr*)&addr, sizeof(addr));
    a = accept(lst, nullptr, nullptr);
    closesocket(lst);
}
}

TEST(framing_roundtrip) {
    SOCKET s1 = INVALID_SOCKET, s2 = INVALID_SOCKET;
    MakeSocketPair(s1, s2);
    std::atomic<bool> stop{ false };
    FrameCodec c1(s1), c2(s2);
    const char payload[] = "hello";
    std::string err;
    EXPECT_TRUE(c1.SendFrame(MsgType::HEARTBEAT, (const uint8_t*)payload, 5, 2000, stop, err));

    MsgType t; std::vector<uint8_t> got;
    EXPECT_TRUE(c2.RecvFrame(t, got, 2000, stop, err));
    EXPECT_EQ((int)t, (int)MsgType::HEARTBEAT);
    EXPECT_EQ(got.size(), (size_t)5);
    closesocket(s1); closesocket(s2);
}

TEST(framing_rejects_oversize) {
    SOCKET s1 = INVALID_SOCKET, s2 = INVALID_SOCKET;
    MakeSocketPair(s1, s2);
    std::atomic<bool> stop{ false };
    std::string err;
    FrameCodec c1(s1);
    // Gửi header với payloadLen = kMaxPayload + 1
    FrameHeader h{};
    h.magic = kMagic; h.version = kVersion; h.type = (uint16_t)MsgType::HEARTBEAT;
    h.payloadLen = kMaxPayload + 1; h.seq = 0; h.reserved = 0;
    EXPECT_TRUE(SendFull(s1, &h, sizeof(h), 2000, stop, err));

    FrameCodec c2(s2);
    MsgType t; std::vector<uint8_t> got;
    EXPECT_FALSE(c2.RecvFrame(t, got, 2000, stop, err));
    EXPECT_EQ(err, std::string("payload too large"));
    closesocket(s1); closesocket(s2);
}

TEST(framing_bad_magic) {
    SOCKET s1 = INVALID_SOCKET, s2 = INVALID_SOCKET;
    MakeSocketPair(s1, s2);
    std::atomic<bool> stop{ false };
    std::string err;
    FrameHeader h{};
    h.magic = 0xDEADBEEF; h.version = kVersion; h.type = 1;
    h.payloadLen = 0; h.seq = 0; h.reserved = 0;
    EXPECT_TRUE(SendFull(s1, &h, sizeof(h), 2000, stop, err));

    FrameCodec c2(s2);
    MsgType t; std::vector<uint8_t> got;
    EXPECT_FALSE(c2.RecvFrame(t, got, 2000, stop, err));
    closesocket(s1); closesocket(s2);
}

TEST(framing_truncated_input_fails) {
    // Gửi header nói payload 100 byte nhưng chỉ gửi 50 → RecvFrame timeout
    SOCKET s1 = INVALID_SOCKET, s2 = INVALID_SOCKET;
    MakeSocketPair(s1, s2);
    std::atomic<bool> stop{ false };
    std::string err;
    FrameHeader h{};
    h.magic = kMagic; h.version = kVersion; h.type = 1;
    h.payloadLen = 100; h.seq = 0; h.reserved = 0;
    EXPECT_TRUE(SendFull(s1, &h, sizeof(h), 2000, stop, err));
    uint8_t half[50] = { 0 };
    EXPECT_TRUE(SendFull(s1, half, 50, 2000, stop, err));
    // Không đóng socket → RecvFrame timeout (500ms)
    FrameCodec c2(s2);
    MsgType t; std::vector<uint8_t> got;
    EXPECT_FALSE(c2.RecvFrame(t, got, 500, stop, err));
    closesocket(s1); closesocket(s2);
}
