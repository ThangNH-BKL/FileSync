#pragma once
// Maps a FileServer Tree node (IP / exe-folder / relative data) to a SAFE
// relative path under the configured ServerRoot. Does not open network SMB.
// Portable C++17 helper: the Windows caller additionally validates path
// containment with PathSanitizer before opening anything.
#include <string>
#include <vector>
#include <cwctype>

namespace fsyn { namespace ui_tree {
inline bool IsSafeComponent(const std::wstring& component) {
    if (component.empty() || component == L"." || component == L".." ||
        component.size() > 255 || component.back() == L'.' || component.back() == L' ')
        return false;
    for (const wchar_t ch : component) {
        if (ch < 32 || ch == L'\\' || ch == L'/' || ch == L':' ||
            ch == L'<' || ch == L'>' || ch == L'"' || ch == L'|' ||
            ch == L'?' || ch == L'*') return false;
    }
    return true;
}
inline bool BuildServerRelativePath(const std::vector<std::wstring>& segments,
                                    std::wstring& relative) {
    relative.clear();
    if (segments.empty()) return false;
    for (const auto& part : segments) {
        if (!IsSafeComponent(part)) { relative.clear(); return false; }
        if (!relative.empty()) relative += L'\\';
        relative += part;
    }
    return true;
}
}} // namespace fsyn::ui_tree
