#pragma once
// Server-side, per-session file *verification* progress (not a byte counter).
// Pending includes candidate files in the initial snapshot and watcher events.
// A file counts as completed only after a successful FILE_COMMITTED ACK and
// queue commit; unchanged candidates also count because they were verified.
// Pure C++17 to allow testing without Windows/MFC.
#include <cstdint>

namespace fsyn {
struct TransferProgress {
    uint64_t completed = 0; // Server-verified and ACKed, including unchanged
    uint64_t updated = 0;   // Server-committed changed files
    uint64_t unchanged = 0; // Server-verified unchanged files
    uint64_t failed = 0;    // Rejected invalid queue entries; not completed
    uint64_t active = 0;    // In flight (0/1 per ClientSession)
    uint64_t pending = 0;   // Waiting, including deferred candidates

    uint64_t Total() const { return completed + failed + active + pending; }
    unsigned Percent() const {
        const uint64_t total = Total();
        return total ? static_cast<unsigned>((100.0L * completed) / total) : 0u;
    }
    unsigned Permille() const {
        const uint64_t total = Total();
        return total ? static_cast<unsigned>((1000.0L * completed) / total) : 0u;
    }
    void Accumulate(const TransferProgress& other) {
        completed += other.completed;
        updated += other.updated;
        unchanged += other.unchanged;
        failed += other.failed;
        active += other.active;
        pending += other.pending;
    }
    bool operator==(const TransferProgress& other) const {
        return completed == other.completed && updated == other.updated &&
               unchanged == other.unchanged && failed == other.failed &&
               active == other.active && pending == other.pending;
    }
    bool operator!=(const TransferProgress& other) const { return !(*this == other); }
};
} // namespace fsyn
