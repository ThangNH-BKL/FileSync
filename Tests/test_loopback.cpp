#include "framework.h"
#include "MiniTest.h"
#include "ServerEngine.h"
#include "ClientEngine.h"
#include "Callbacks.h"
#include "Util.h"
#include <filesystem>
#include <fstream>
#include <vector>
#include <mutex>
#include <chrono>
#include <functional>

using namespace fsyn;
namespace fs = std::filesystem;

namespace {
class RecordingUi : public IUiCallbacks {
public:
    void OnLog(LogLevel, const std::string& line) override {
        std::lock_guard<std::mutex> lock(mu_);
        if (logs_.size() >= 128) logs_.erase(logs_.begin());
        logs_.push_back(line);
    }
    void OnConnState(ConnStateUi) override {}
    void DumpLogTail() const {
        std::lock_guard<std::mutex> lock(mu_);
        std::printf("  loopback diagnostics (%zu recent lines):\n", logs_.size());
        for (const auto& item : logs_) std::printf("    %s\n", item.c_str());
    }
private:
    mutable std::mutex mu_;
    std::vector<std::string> logs_;
};

bool WaitFor(const std::function<bool()>& predicate, DWORD timeoutMs) {
    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::milliseconds(timeoutMs);
    do {
        if (predicate()) return true;
        Sleep(100);
    } while (std::chrono::steady_clock::now() < deadline);
    return predicate();
}

// Ask Windows for an unused ephemeral port. A small bind/close race remains,
// but this avoids a frequently occupied hardcoded port (54321).
uint16_t FreeLoopbackPort() {
    SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return 0;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (::bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
        ::closesocket(s);
        return 0;
    }
    int len = sizeof(addr);
    if (::getsockname(s, reinterpret_cast<sockaddr*>(&addr), &len) == SOCKET_ERROR) {
        ::closesocket(s);
        return 0;
    }
    const uint16_t port = ntohs(addr.sin_port);
    ::closesocket(s);
    return port;
}
}

TEST(loopback_full_sync) {
    wchar_t tmp[MAX_PATH]{};
    REQUIRE_TRUE(GetTempPathW(MAX_PATH,tmp)>0);
    const auto base = fs::path(tmp) / (L"filesync_loopback_" +
        std::to_wstring(GetCurrentProcessId()) + L"_" +
        std::to_wstring(GetTickCount64()));
    const auto watchDir = base / L"watch";
    const auto serverDir = base / L"server";
    std::error_code ec;
    REQUIRE_TRUE(fs::create_directories(watchDir, ec) && !ec);
    ec.clear();
    REQUIRE_TRUE(fs::create_directories(serverDir, ec) && !ec);
    const uint16_t port = FreeLoopbackPort();
    REQUIRE_TRUE(port != 0);

    ServerEngine::Config scfg;
    scfg.port=port;
    scfg.token="test";
    scfg.serverRootUtf16=serverDir.wstring();
    scfg.extFilterSpec="*";
    scfg.sched.mode=SchedMode::Window;
    scfg.sched.windowStartMin=0;
    scfg.sched.windowEndMin=0;

    RecordingUi ui;
    ServerEngine srv(scfg,&ui);
    REQUIRE_TRUE(srv.Start());

    ClientEngine::Config ccfg;
    ccfg.serverIp="127.0.0.1";
    ccfg.port=port;
    ccfg.watchRootUtf16=watchDir.wstring();
    ccfg.token="test";
    ccfg.hostname="testhost";
    ClientEngine cli(ccfg,&ui);
    REQUIRE_TRUE(cli.Start());

    // V6.2+ stores files as <ServerRoot>/<TCP peer IP>/<EXE folder>/<relPath>.
    // The old expectation <ServerRoot>/testhost/hello.txt is incorrect.
    std::string hostKey;
    const bool connected=WaitFor([&] {
        for (const auto& client : srv.ClientList()) {
            if (client.second=="127.0.0.1" && !client.first.empty() &&
                client.first.find('\\')!=std::string::npos) {
                hostKey=client.first;return true;
            }
        }
        return false;
    }, 10000);
    if (!connected) ui.DumpLogTail();
    REQUIRE_TRUE(connected);

    const auto fpath=watchDir/L"hello.txt";
    {
        std::ofstream out(fpath,std::ios::binary|std::ios::trunc);
        REQUIRE_TRUE(out.good());
        out << "Hello, sync!";
        out.close();
        REQUIRE_TRUE(out.good());
    }
    const auto expected=serverDir/fs::path(Utf8ToWide(hostKey))/L"hello.txt";
    const bool received=WaitFor([&] {
        std::error_code err;
        return fs::exists(expected,err) && fs::is_regular_file(expected,err) &&
               fs::file_size(expected,err)==uintmax_t(12);
    }, 15000);
    if (!received) {
        std::printf("  expected committed file: %ls\n",expected.c_str());
        ui.DumpLogTail();
    }
    EXPECT_TRUE(received);
    if (received) {
        std::ifstream in(expected, std::ios::binary);
        const std::string content((std::istreambuf_iterator<char>(in)),
                                  std::istreambuf_iterator<char>());
        EXPECT_EQ(content,std::string("Hello, sync!"));
    }
    cli.Stop();
    srv.Stop();
    fs::remove_all(base,ec);
}
