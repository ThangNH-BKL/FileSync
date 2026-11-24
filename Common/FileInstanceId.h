#pragma once
// File identity is stable across normal edits and changes when a file is
// deleted/recreated (including when it has the same pathname and contents).
// NTFS/ReFS: volume serial + file index + creation FILETIME.
#include <windows.h>
#include <string>
#include <cstdio>
namespace fsyn {
inline std::string GetFileInstanceId(const std::wstring& path) {
    HANDLE h = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return {};
    BY_HANDLE_FILE_INFORMATION info{};
    const bool ok = GetFileInformationByHandle(h, &info) != FALSE;
    CloseHandle(h);
    if (!ok || (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) return {};
    const unsigned long long index =
        (static_cast<unsigned long long>(info.nFileIndexHigh) << 32) | info.nFileIndexLow;
    const unsigned long long created =
        (static_cast<unsigned long long>(info.ftCreationTime.dwHighDateTime) << 32) |
        info.ftCreationTime.dwLowDateTime;
    if (index == 0 && created == 0) return {}; // no reliable instance identity
    char out[64]{};
    sprintf_s(out, sizeof(out), "%08x%016llx%016llx",
        static_cast<unsigned int>(info.dwVolumeSerialNumber), index, created);
    return out;
}
inline bool IsValidFileInstanceId(const std::string& id) {
    if (id.size() != 40) return false;
    for (char c : id) if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    return true;
}
}
