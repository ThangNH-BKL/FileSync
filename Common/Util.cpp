#include "pch.h"
#include "Util.h"
#include <vector>

namespace fsyn {

std::string ErrStr(DWORD code) {
    if (code == 0) return "OK";
    LPWSTR buf = nullptr;
    DWORD n = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        (LPWSTR)&buf, 0, nullptr);
    std::wstring w;
    if (n && buf) { w.assign(buf, n); LocalFree(buf); }
    // Cắt newline cuối
    while (!w.empty() && (w.back() == L'\r' || w.back() == L'\n')) w.pop_back();
    return WideToUtf8(w);
}

std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(),
                                nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string s;
    s.resize(n);
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(),
                        s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    if (n <= 0) return {};
    std::wstring w;
    w.resize(n);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

std::string GetLocalHostname() {
    wchar_t buf[256] = { 0 };
    DWORD n = (DWORD)std::size(buf);
    if (!GetComputerNameW(buf, &n)) return {};
    return WideToUtf8(std::wstring(buf, n));
}

std::string SanitizeHostKey(const std::string& raw) {
    std::string out;
    out.reserve(raw.size());
    for (char c : raw) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-') {
            out.push_back(c);
        } else {
            out.push_back('_');
        }
        if (out.size() >= 100) break;
    }
    return out;
}

bool ConstantTimeEquals(const std::string& a, const std::string& b) {
    // Đảm bảo không lộ độ dài qua timing: luôn so sánh trên độ dài max + 1.
    size_t la = a.size(), lb = b.size();
    size_t n  = la > lb ? la : lb;
    unsigned diff = (unsigned)(la ^ lb);
    for (size_t i = 0; i < n; ++i) {
        unsigned ca = i < la ? (unsigned char)a[i] : 0;
        unsigned cb = i < lb ? (unsigned char)b[i] : 0;
        diff |= (ca ^ cb);
    }
    return diff == 0;
}

std::wstring JoinPath(const std::wstring& a, const std::wstring& b) {
    if (a.empty()) return b;
    if (b.empty()) return a;
    std::wstring r = a;
    if (r.back() != L'\\' && r.back() != L'/') r.push_back(L'\\');
    // Bỏ dấu \ đầu của b
    size_t start = 0;
    while (start < b.size() && (b[start] == L'\\' || b[start] == L'/')) ++start;
    r.append(b, start, std::wstring::npos);
    return r;
}

bool SetSocketNonBlocking(SOCKET s, bool nonblocking) {
    u_long mode = nonblocking ? 1u : 0u;
    return ::ioctlsocket(s, FIONBIO, &mode) == 0;
}

} // namespace fsyn
