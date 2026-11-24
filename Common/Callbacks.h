#pragma once
// Callbacks.h — Interface UI ↔ engine.
// Engine KHÔNG gọi trực tiếp MFC; chỉ gọi callback do UI inject.
// Tests inject callback ghi ra stdout.

#include <string>
#include <cstdint>
#include "Logger.h"

namespace fsyn {

// Trạng thái kết nối hiển thị trên UI
enum class ConnStateUi {
    Idle = 0,
    Connecting,
    Syncing,
    ConnectedIdle,
    ReconnectWait,
    AuthFail,
    Stopping,
};

// Callback từ Engine → UI (mọi hàm chạy trên thread của engine).
class IUiCallbacks {
public:
    virtual ~IUiCallbacks() = default;

    // Ghi log (đã có timestamp) — UI chỉ append.
    virtual void OnLog(LogLevel lv, const std::string& line) = 0;

    // Cập nhật trạng thái kết nối.
    virtual void OnConnState(ConnStateUi state) = 0;

    // Client: 1 file đã gửi xong.
    virtual void OnFileDone(const std::string&, uint64_t) {}

    // Server: danh sách client thay đổi (chỉ gọi khi có client connect/disconnect).
    virtual void OnClientListChanged() {}

    // Server: cây thư mục cập nhật (thêm 1 relPath).
    virtual void OnTreeUpdate(const std::string&) {}

    // Tiến trình (tùy chọn).
    virtual void OnProgress(uint64_t, uint64_t) {}
};

// Logger sink: chuyển tiếp từ Logger vào IUiCallbacks::OnLog.
void LoggerSinkToUi(void* ctx, LogLevel lv, const std::string& line);

} // namespace fsyn
