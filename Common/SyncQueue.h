#pragma once
// SyncQueue.h — Hàng đợi sự kiện client. Thread-safe, merge theo relPath,
// 10,000 buffered events. Overflow marks reconciliation required; never silent.

#include "Protocol.h"
#include <mutex>
#include <deque>
#include <unordered_map>

namespace fsyn {

class SyncQueue {
public:
    static constexpr size_t kMax = 10000;

    // Thêm/cập nhật event. Nếu relPath đã có → ghi đè value, giữ vị trí.
    void Push(const FileEventMsg& ev);

    // Lấy toàn bộ và xoá. Trả false nếu rỗng.
    bool PopAll(std::vector<FileEventMsg>& out);
    // Drain a small batch; keep the remainder so the Client can receive ACKs
    // and Server requests between FILE_EVENT bursts.
    bool PopBatch(size_t maxCount, std::vector<FileEventMsg>& out);

    size_t Size() const;
    void   Clear();
    bool Overflowed() const;
    // Clear the prior overflow flag immediately BEFORE a fresh full snapshot.
    // An overflow during snapshot sets it again and forces another reconnect.
    bool ConsumeOverflow();
    void RequireReconciliation();

private:
    mutable std::mutex mu_;
    std::unordered_map<std::string, FileEventMsg> items_;
    std::deque<std::string>                        order_;
    bool overflowed_ = false;
};

} // namespace fsyn
