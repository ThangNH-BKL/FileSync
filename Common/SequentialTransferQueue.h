#pragma once
// Single-flight, server-driven file order: smallest size first; for equal sizes,
// newest modification time first; stable lexical path for exact ties.
// Pure C++17 to enable native unit testing without Windows/MFC.
#include <chrono>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>

namespace fsyn {
class SequentialTransferQueue {
public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;
    struct File {
        std::string path;
        uint64_t size = 0;
        int64_t modifiedNs = 0;
        TimePoint notBefore{};
    };

    // Update one file's priority when the watcher reports a change.
    // It may be queued again while an older version of that file is active.
    void Enqueue(const std::string& path, uint64_t size, int64_t modifiedNs,
                 TimePoint notBefore = TimePoint{}) {
        if (path.empty()) return;
        auto old = files_.find(path);
        if (old != files_.end()) {
            order_.erase(old->second);
            files_.erase(old);
        }
        File item{path, size, modifiedNs, notBefore};
        files_.emplace(path, item);
        order_.insert(std::move(item));
    }

    bool BeginNext(File& out, TimePoint now = Clock::now()) {
        if (active_) return false;  // Exactly one file may be in flight.
        for (auto it = order_.begin(); it != order_.end(); ++it) {
            if (it->notBefore > now) continue;
            out = *it;
            active_ = out;
            files_.erase(it->path);
            order_.erase(it);
            return true;
        }
        return false;
    }

    const File* Active() const { return active_ ? &*active_ : nullptr; }
    size_t PendingCount() const { return files_.size(); }
    bool HasEligible(TimePoint now=Clock::now()) const {
        for(const auto& e:order_) if(e.notBefore<=now) return true;
        return false;
    }
    bool HasPending(const std::string& path) const { return files_.find(path)!=files_.end(); }

    bool Commit(const std::string& path) {
        if (!active_ || active_->path != path) return false;
        active_.reset();
        return true;
    }

    // Source changed / temporarily inaccessible: preserve staging data and
    // retry later, allowing other ready files to proceed in the meantime.
    bool Defer(const std::string& path, TimePoint eligibleAt) {
        if (!active_ || active_->path != path) return false;
        File prior = *active_;
        active_.reset();
        auto pending = files_.find(path);
        if (pending == files_.end()) {
            Enqueue(path, prior.size, prior.modifiedNs, eligibleAt);
        } else {
            File updated = pending->second;
            if (updated.notBefore < eligibleAt) updated.notBefore = eligibleAt;
            Enqueue(updated.path, updated.size, updated.modifiedNs, updated.notBefore);
        }
        return true;
    }

private:
    struct Order {
        bool operator()(const File& a, const File& b) const {
            if (a.size != b.size) return a.size < b.size;
            if (a.modifiedNs != b.modifiedNs) return a.modifiedNs > b.modifiedNs;
            return a.path < b.path;
        }
    };
    std::set<File, Order> order_;
    std::map<std::string, File> files_;
    std::optional<File> active_;
};
} // namespace fsyn
