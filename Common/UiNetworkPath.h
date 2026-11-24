#pragma once
// Display and Windows SMB navigation helpers. UI-only: NEVER use a client-reported
// path as a ServerRoot or change the persistent hostKey/storage layout.
#include <string>
#include <vector>
#include <utility>
#include <cstdint>

namespace fsyn {
namespace ui_path {

inline bool IsIpv4(const std::string& ip) {
    if (ip.empty()) return false;
    unsigned octets = 0;
    size_t start = 0;
    while (start < ip.size()) {
        const size_t end = ip.find('.', start);
        const size_t last = end == std::string::npos ? ip.size() : end;
        if (last == start || last - start > 3) return false;
        unsigned value = 0;
        for (size_t k = start; k < last; ++k) {
            if (ip[k] < '0' || ip[k] > '9') return false;
            value = value * 10 + static_cast<unsigned>(ip[k] - '0');
        }
        if (value > 255) return false;
        ++octets;
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return octets == 4;
}

// Convert a local drive path (D:\folder) into a network share path. This is
// UI-only; it never influences the server's data storage key or root.
// Prefer ordinary, explicitly shared drive names (\\IP\D\folder), since
// administrative shares (D$) may be disabled or require extra privileges.
// Returns empty for untrusted/non-local paths (including UNC and traversal).
inline std::wstring DriveSharePath(const std::string& peerIp,
                                   const std::wstring& clientWatchRoot,
                                   bool administrative) {
    if (!IsIpv4(peerIp) || clientWatchRoot.size() < 3) return {};
    wchar_t drive = clientWatchRoot[0];
    if (drive >= L'a' && drive <= L'z') drive -= (L'a' - L'A');
    if (drive < L'A' || drive > L'Z' || clientWatchRoot[1] != L':' ||
        (clientWatchRoot[2] != L'\\' && clientWatchRoot[2] != L'/')) return {};
    std::wstring root = clientWatchRoot.substr(3);
    for (wchar_t& c : root) {
        if (c == L'/') c = L'\\';
        // Refuse shell-significant and invalid Windows path characters.
        if (c == L'\"' || c == L'<' || c == L'>' || c == L'|' || c == L'?' ||
            c == L'*' || c == L':' || c < 32) return {};
    }
    size_t start = 0;
    while (start < root.size()) {
        const size_t end = root.find(L'\\', start);
        const size_t last = end == std::wstring::npos ? root.size() : end;
        const std::wstring component = root.substr(start, last - start);
        if (component == L"." || component == L"..") return {};
        if (end == std::wstring::npos) break;
        start = end + 1;
    }
    std::wstring unc = L"\\\\";
    unc.append(peerIp.begin(), peerIp.end());
    unc += L"\\";
    unc.push_back(drive);
    if (administrative) unc += L"$";
    if (!root.empty()) unc += L"\\" + root;
    return unc;
}

// The default mapping is to a normal SMB share named like the drive letter.
inline std::wstring StandardSharePath(const std::string& peerIp,
                                      const std::wstring& clientWatchRoot) {
    return DriveSharePath(peerIp, clientWatchRoot, false);
}

// The optional admin-share fallback is only for machines with D$/C$ enabled.
inline std::wstring AdministrativeSharePath(const std::string& peerIp,
                                             const std::wstring& clientWatchRoot) {
    return DriveSharePath(peerIp, clientWatchRoot, true);
}

inline std::vector<std::wstring> CandidateSharePaths(const std::string& peerIp,
                                                      const std::wstring& clientWatchRoot) {
    std::vector<std::wstring> result;
    auto standard = StandardSharePath(peerIp, clientWatchRoot);
    if (standard.empty()) return result;
    result.push_back(std::move(standard));
    auto admin = AdministrativeSharePath(peerIp, clientWatchRoot);
    if (!admin.empty() && admin != result.front()) result.push_back(std::move(admin));
    return result;
}
} // namespace ui_path
} // namespace fsyn
