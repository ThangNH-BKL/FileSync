#pragma once
// FileServer GUI numeric configuration parsing (portable C++17).
// Parse before persisting or starting. Never silently truncate a value.
#include <string_view>

namespace fsyn { namespace ui_server_limits {
inline constexpr unsigned kDefaultClients = 512;
inline constexpr unsigned kDefaultTransfers = 12;
inline constexpr unsigned kMinClients = 1;
inline constexpr unsigned kMaxClients = 4096;
inline constexpr unsigned kMinTransfers = 1;
inline constexpr unsigned kMaxTransfers = 64;

inline bool Parse(std::wstring_view value, unsigned minimum,
                  unsigned maximum, unsigned& out) noexcept {
    // Accept harmless leading/trailing whitespace from copy-and-paste.
    while (!value.empty() && (value.front() == L' ' || value.front() == L'\t')) value.remove_prefix(1);
    while (!value.empty() && (value.back() == L' ' || value.back() == L'\t')) value.remove_suffix(1);
    if (value.empty() || minimum > maximum) return false;
    unsigned parsed = 0;
    for (wchar_t c : value) {
        if (c < L'0' || c > L'9') return false;
        const unsigned digit = static_cast<unsigned>(c - L'0');
        // Checked arithmetic also rejects huge decimal strings without overflow.
        if (digit > maximum || parsed > (maximum - digit) / 10) return false;
        parsed = parsed * 10 + digit;
    }
    if (parsed < minimum || parsed > maximum) return false;
    out = parsed;
    return true;
}
} } // namespace fsyn::ui_server_limits
