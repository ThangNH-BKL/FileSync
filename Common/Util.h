#pragma once
// Util.h — helper chung: chuỗi lỗi Win32, hostKey sanitize, so sánh constant-time.

#include <string>
#include <cstdint>

namespace fsyn {

// Trả về message lỗi Win32/Winsock tương ứng mã lỗi.
std::string ErrStr(DWORD code);

// WCHAR (UTF-16) <-> UTF-8
std::string  WideToUtf8(const std::wstring& w);
std::wstring Utf8ToWide(const std::string& s);

// Lấy hostname của máy hiện tại (UTF-8); rỗng nếu lỗi.
std::string GetLocalHostname();

// Sanitize hostKey (dùng làm tên thư mục con dưới ServerRoot).
// Chỉ cho phép: [A-Za-z0-9._-], thay ký tự khác bằng '_'. Cắt còn 100 ký tự.
// Trả chuỗi rỗng nếu input rỗng (caller fallback IP).
std::string SanitizeHostKey(const std::string& raw);

// So sánh constant-time (chống timing attack).
bool ConstantTimeEquals(const std::string& a, const std::string& b);

// Đường dẫn ghép an toàn (chỉ dùng cho internal; relPath đã qua sanitizer).
std::wstring JoinPath(const std::wstring& a, const std::wstring& b);

// Bật/tắt chế độ không chặn (non-blocking) cho socket.
bool SetSocketNonBlocking(SOCKET s, bool nonblocking);

} // namespace fsyn
