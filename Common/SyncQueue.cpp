#include "pch.h"
#include "SyncQueue.h"

namespace fsyn {

void SyncQueue::Push(const FileEventMsg& ev) {
    std::lock_guard<std::mutex> g(mu_);
    auto it = items_.find(ev.relPath);
    if (it != items_.end()) {
        it->second = ev;   // merge: giữ vị trí, cập nhật payload
        return;
    }
    items_[ev.relPath] = ev;
    order_.push_back(ev.relPath);
    while (order_.size() > kMax) {
        overflowed_ = true;  // A fresh snapshot is now mandatory.
        items_.erase(order_.front());
        order_.pop_front();
    }
}

bool SyncQueue::PopAll(std::vector<FileEventMsg>& out) {
    return PopBatch(kMax, out);
}

bool SyncQueue::PopBatch(size_t maxCount, std::vector<FileEventMsg>& out) {
    std::lock_guard<std::mutex> g(mu_);
    out.clear();
    if (maxCount == 0 || order_.empty()) return false;
    out.reserve((std::min)(maxCount, order_.size()));
    while (!order_.empty() && out.size() < maxCount) {
        std::string key = std::move(order_.front());
        order_.pop_front();
        auto it = items_.find(key);
        if (it == items_.end()) continue;
        out.push_back(std::move(it->second));
        items_.erase(it);
    }
    return !out.empty();
}

size_t SyncQueue::Size() const {
    std::lock_guard<std::mutex> g(mu_);
    return items_.size();
}

void SyncQueue::RequireReconciliation() {
    std::lock_guard<std::mutex> g(mu_);
    overflowed_ = true;
}

bool SyncQueue::Overflowed() const {
    std::lock_guard<std::mutex> g(mu_);
    return overflowed_;
}

bool SyncQueue::ConsumeOverflow() {
    std::lock_guard<std::mutex> g(mu_);
    const bool was = overflowed_;
    overflowed_ = false;
    return was;
}

void SyncQueue::Clear() {
    std::lock_guard<std::mutex> g(mu_);
    items_.clear();
    order_.clear();
    overflowed_ = false;
}

} // namespace fsyn
