#pragma once
// Pure C++17 version naming, independently testable on Windows or Linux.
#include <string>
#include <cstdint>
namespace fsyn {
inline std::wstring NumberedFilePath(const std::wstring& fullPath, uint32_t index) {
    if (!index) return fullPath;
    const auto slash = fullPath.find_last_of(L"\\/");
    const auto dot = fullPath.find_last_of(L'.');
    const bool extension = dot != std::wstring::npos &&
        (slash == std::wstring::npos || dot > slash + 1);
    const auto pos = extension ? dot : fullPath.size();
    return fullPath.substr(0, pos) + L" (" + std::to_wstring(index) + L")" + fullPath.substr(pos);
}
}
