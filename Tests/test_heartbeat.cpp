#include "framework.h"
#include "MiniTest.h"
#include "FrameCodec.h"
#include "Protocol.h"
#include <atomic>
#include <thread>

using namespace fsyn;

namespace {
void MakeSocketPair2(SOCKET& a, SOCKET& b) {
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

TEST(heartbeat_received_in_time) {
    SOCKET s1 = INVALID_SOCKET, s2 = INVALID_SOCKET;
    MakeSocketPair2(s1, s2);
    std::atomic<bool> stop{ false };
    std::string err;
    FrameCodec c1(s1), c2(s2);
    // Gửi heartbeat mỗi 200ms, nhận trong 500ms
    std::thread sender([&] {
        for (int i = 0; i < 3; ++i) {
            c1.SendFrame(MsgType::HEARTBEAT, nullptr, 0, 1000, stop, err);
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    });
    MsgType t;
    std::vector<uint8_t> payload;
    bool ok = c2.RecvFrame(t, payload, 1000, stop, err);
    EXPECT_TRUE(ok);
    EXPECT_EQ((int)t, (int)MsgType::HEARTBEAT);
    stop.store(true);
    sender.join();
    closesocket(s1); closesocket(s2);
}

TEST(heartbeat_timeout_when_silent) {
    SOCKET s1 = INVALID_SOCKET, s2 = INVALID_SOCKET;
    MakeSocketPair2(s1, s2);
    std::atomic<bool> stop{ false };
    std::string err;
    FrameCodec c2(s2);
    // Không ai gửi gì trong 500ms → RecvFrame timeout
    MsgType t; std::vector<uint8_t> payload;
    EXPECT_FALSE(c2.RecvFrame(t, payload, 500, stop, err));
    EXPECT_EQ(err, std::string("recv timeout"));
    closesocket(s1); closesocket(s2);
}
