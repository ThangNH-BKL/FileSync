#pragma once
// Logger.h — log UTF-8, có timestamp ms, thread-safe, flush ngay, xoay khi > 5MB.
// Dùng ở cả Client/Server (mỗi app 1 Logger riêng) và trong Tests.

#include <string>
#include <mutex>
#include <cstdio>

namespace fsyn {

enum class LogLevel { Info = 0, Warn = 1, Error = 2 };

class Logger {
public:
    Logger() = default;
    ~Logger();
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    // Mở file (append). Trả false nếu không mở được.
    bool Open(const std::wstring& pathUtf16);

    // Ghi 1 dòng. Nếu file chưa Open → ghi ra stderr.
    void Log(LogLevel lv, const std::string& msg);

    // Log lỗi Win32 kèm mã lỗi.
    void LogWin32(LogLevel lv, const std::string& msg, DWORD err);

    void Close();

    // Callback tuỳ chọn: nhận mỗi dòng log (đã có timestamp) để UI hiển thị.
    using Sink = void(*)(void* ctx, LogLevel lv, const std::string& line);
    void SetSink(Sink sink, void* ctx) { sink_ = sink; sinkCtx_ = ctx; }

private:
    void RotateIfNeeded_();
    void WriteLine_(LogLevel lv, const std::string& msg);

    std::mutex  mu_;
    FILE*       fp_        = nullptr;
    std::wstring path_;
    uint64_t    size_      = 0;
    Sink        sink_      = nullptr;
    void*       sinkCtx_   = nullptr;
};

} // namespace fsyn
