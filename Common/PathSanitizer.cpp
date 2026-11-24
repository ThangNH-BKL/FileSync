#include "pch.h"
#include "Protocol.h"
#include "PathSanitizer.h"
#include "Util.h"
#include <cwctype>
#include <vector>

namespace fsyn {
namespace {

bool IsDeviceName(const std::string& name) {
    // Tên thiết bị bị cấm, so sánh không phân biệt hoa/thường, bỏ phần mở rộng.
    std::string base = name;
    size_t dot = base.find('.');
    if (dot != std::string::npos) base = base.substr(0, dot);

    // Viết hoa ASCII
    for (auto& c : base) {
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
    }

    static const char* kDevices[] = {
        "CON", "PRN", "AUX", "NUL",
        "COM1","COM2","COM3","COM4","COM5","COM6","COM7","COM8","COM9",
        "LPT1","LPT2","LPT3","LPT4","LPT5","LPT6","LPT7","LPT8","LPT9",
    };
    for (const char* d : kDevices) {
        if (base == d) return true;
    }
    return false;
}

bool HasInvalidChar(char c) {
    if ((unsigned char)c < 0x20) return true;     // control char
    switch (c) {
        case '<': case '>': case '"': case '|':
        case '?': case '*': case ':':
            return true;
        default:
            return false;
    }
}

bool StartsWithDriveLetter(const std::string& s) {
    // Dạng "X:" hoặc "X:\..."
    if (s.size() < 2) return false;
    char c = s[0];
    return ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) && s[1] == ':';
}

} // namespace

std::string PathSanitizer::NormalizeSeparators(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    bool prevSep = false;
    for (char c : in) {
        char ch = (c == '/') ? '\\' : c;
        if (ch == '\\') {
            if (!prevSep) out.push_back('\\');
            prevSep = true;
        } else {
            out.push_back(ch);
            prevSep = false;
        }
    }
    return out;
}

bool PathSanitizer::NormalizeRoot(const std::wstring& rootIn, std::wstring& rootOut) {
    if (rootIn.empty()) return false;

    wchar_t buf[MAX_PATH * 4] = { 0 };
    DWORD n = GetFullPathNameW(rootIn.c_str(), (DWORD)std::size(buf), buf, nullptr);
    if (n == 0 || n >= std::size(buf)) return false;

    rootOut.assign(buf, n);
    while (!rootOut.empty() && (rootOut.back() == L'\\' || rootOut.back() == L'/')) {
        rootOut.pop_back();
    }
    return !rootOut.empty();
}

bool PathSanitizer::Validate(const std::wstring& rootUtf16,
                             const std::string& relPathUtf8,
                             std::wstring& fullPathOut,
                             std::string* reasonOut) {
    auto fail = [&](const char* r) -> bool {
        if (reasonOut) *reasonOut = r;
        return false;
    };

    if (relPathUtf8.empty()) return fail("empty relPath");
    if (relPathUtf8.size() > kMaxRelPathLen) return fail("relPath too long");

    // 1) Ký tự cấm / control / ':'
    for (char c : relPathUtf8) {
        if (HasInvalidChar(c)) return fail("invalid character in relPath");
    }

    // 2) Tuyệt đối / drive / UNC
    if (relPathUtf8[0] == '\\' || relPathUtf8[0] == '/') return fail("absolute path");
    if (StartsWithDriveLetter(relPathUtf8)) return fail("drive letter path");

    // 3) Chuẩn hóa dấu phân cách
    std::string rel = NormalizeSeparators(relPathUtf8);

    // 4) Kiểm tra từng thành phần: rỗng, ".", "..", device name
    std::vector<std::string> parts;
    {
        size_t start = 0;
        while (start <= rel.size()) {
            size_t pos = rel.find('\\', start);
            std::string part = (pos == std::string::npos)
                ? rel.substr(start)
                : rel.substr(start, pos - start);
            if (!part.empty()) parts.push_back(part);
            if (pos == std::string::npos) break;
            start = pos + 1;
        }
    }
    if (parts.empty()) return fail("no path component");

    for (const auto& p : parts) {
        if (p == "." || p == "..") return fail("'.' or '..' component");
        if (IsDeviceName(p)) return fail("device name");
        // Tên file quá dài (giới hạn NTFS 255 ký tự)
        if (p.size() > 255) return fail("component too long");
    }

    // 5) Ghép root + rel, chuẩn hóa, kiểm tra containment
    std::wstring relW = Utf8ToWide(rel);
    std::wstring joined = JoinPath(rootUtf16, relW);

    wchar_t canonBuf[MAX_PATH * 8] = { 0 };
    DWORD n = GetFullPathNameW(joined.c_str(), (DWORD)std::size(canonBuf), canonBuf, nullptr);
    if (n == 0 || n >= std::size(canonBuf)) return fail("GetFullPathName failed");
    std::wstring full(canonBuf, n);

    // Chuẩn hóa root so sánh
    std::wstring rootNorm;
    if (!NormalizeRoot(rootUtf16, rootNorm)) return fail("bad root");

    // So sánh prefix có phân cách thư mục để tránh C:\root2 khớp C:\root
    if (full.size() <= rootNorm.size()) return fail("path not under root");
    if (_wcsnicmp(full.c_str(), rootNorm.c_str(), rootNorm.size()) != 0)
        return fail("path escaped root");
    wchar_t sep = full[rootNorm.size()];
    if (sep != L'\\' && sep != L'/') return fail("path escaped root (separator)");

    fullPathOut = full;
    return true;
}

} // namespace fsyn
