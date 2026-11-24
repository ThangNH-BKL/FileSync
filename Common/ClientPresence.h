#pragma once
// ClientPresence.h - UI-only history of authenticated FileSync client sessions.
// No socket operations are performed here. Retain disconnected clients so that
// their offline state remains visible in the Server's Clients table.

#include <cstddef>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace fsyn {

class ClientPresence {
public:
    struct Entry {
        std::string hostKey;
        std::string ip;
        bool online = false;
    };

    // The caller passes only authenticated, currently live sessions. Entries
    // absent from the snapshot become offline, but remain in the table.
    // The same IP may host several FileClient.exe instances with different
    // executable-folder keys, so identity includes both fields.
    void Reconcile(const std::vector<std::pair<std::string, std::string>>& live) {
        for (auto& entry : entries_) entry.online = false;
        for (const auto& item : live) {
            if (item.first.empty() || item.second.empty()) continue;
            const auto key = Key(item.first, item.second);
            const auto found = positions_.find(key);
            if (found != positions_.end()) {
                entries_[found->second].online = true;
            } else {
                positions_.emplace(key, entries_.size());
                entries_.push_back({item.first, item.second, true});
            }
        }
    }

    const std::vector<Entry>& Entries() const { return entries_; }

    // Indexes in the four-image status icon strip created by CFServerDlg.
    // Keep two brightness levels for each color to create a visible blink.
    static int IconIndex(bool online, bool bright) {
        return online ? (bright ? 0 : 1) : (bright ? 2 : 3);
    }

private:
    static std::string Key(const std::string& hostKey, const std::string& ip) {
        // Length prefix avoids collisions even with delimiters in hostKey.
        return std::to_string(hostKey.size()) + ":" + hostKey + ip;
    }

    std::vector<Entry> entries_;
    std::unordered_map<std::string, std::size_t> positions_;
};

} // namespace fsyn
