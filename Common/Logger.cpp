#include "pch.h"
#include "Logger.h"
#include "Util.h"
#include <vector>
#include <chrono>

namespace fsyn {
namespace {

constexpr uint64_t kRotateBytes = 5ull * 1024ull * 1024ull; // 5 MB
constexpr int      kKeepOld     = 1;

std::string TimestampMs() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%04u-%02u-%02u %02u:%02u:%02u.%03u",
                  st.wYear, st.wMonth, st.wDay,
                  st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    return buf;
}

const char* LvStr(LogLevel lv) {
    switch (lv) {
        case LogLevel::Info:  return "INFO";
        case LogLevel::Warn:  return "WARN";
        case LogLevel::Error: return "ERR ";
    }
    return "???";
}

} // namespace

Logger::~Logger() { Close(); }

bool Logger::Open(const std::wstring& pathUtf16) {
    std::lock_guard<std::mutex> g(mu_);
    if (fp_) { std::fclose(fp_); fp_ = nullptr; }
    path_ = pathUtf16;
    errno_t e = _wfopen_s(&fp_, pathUtf16.c_str(), L"ab");
    if (e != 0 || !fp_) { fp_ = nullptr; return false; }
    // Đo kích thước hiện tại
    _fseeki64(fp_, 0, SEEK_END);
    size_ = (uint64_t)_ftelli64(fp_);
    return true;
}

void Logger::RotateIfNeeded_() {
    if (!fp_ || size_ < kRotateBytes) return;

    std::fclose(fp_);
    fp_ = nullptr;

    // Đổi tên path -> path.old (xoay vòng đơn giản, giữ 1 file cũ)
    std::wstring oldPath = path_ + L".old";
    DeleteFileW(oldPath.c_str());
    MoveFileW(path_.c_str(), oldPath.c_str());

    errno_t e = _wfopen_s(&fp_, path_.c_str(), L"ab");
    if (e != 0 || !fp_) { fp_ = nullptr; return; }
    size_ = 0;
}

void Logger::WriteLine_(LogLevel lv, const std::string& msg) {
    std::string line;
    line.reserve(msg.size() + 32);
    line += TimestampMs();
    line += " [";
    line += LvStr(lv);
    line += "] ";
    line += msg;
    line += "\n";

    if (fp_) {
        std::fwrite(line.data(), 1, line.size(), fp_);
        std::fflush(fp_);
        size_ += line.size();
        RotateIfNeeded_();
    } else {
        std::fwrite(line.data(), 1, line.size(), stderr);
        std::fflush(stderr);
    }

    if (sink_) sink_(sinkCtx_, lv, line);
}

void Logger::Log(LogLevel lv, const std::string& msg) {
    std::lock_guard<std::mutex> g(mu_);
    WriteLine_(lv, msg);
}

void Logger::LogWin32(LogLevel lv, const std::string& msg, DWORD err) {
    std::string m = msg;
    m += " (err=";
    m += std::to_string(err);
    m += " \"";
    m += ErrStr(err);
    m += "\")";
    Log(lv, m);
}

void Logger::Close() {
    std::lock_guard<std::mutex> g(mu_);
    if (fp_) { std::fclose(fp_); fp_ = nullptr; }
}

} // namespace fsyn
