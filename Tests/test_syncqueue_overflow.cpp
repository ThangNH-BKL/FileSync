#include "MiniTest.h"
#include "SyncQueue.h"
#include <string>

TEST(syncqueue_overflow_requires_reconciliation) {
    fsyn::SyncQueue q;
    fsyn::FileEventMsg ev;
    ev.action = fsyn::FileAction::Created;
    for (size_t i = 0; i < fsyn::SyncQueue::kMax + 5; ++i) {
        ev.relPath = "event" + std::to_string(i);
        q.Push(ev);
    }
    EXPECT_TRUE(q.Overflowed());
    EXPECT_EQ(q.Size(), fsyn::SyncQueue::kMax);
    EXPECT_TRUE(q.ConsumeOverflow());
    EXPECT_FALSE(q.Overflowed());
    EXPECT_FALSE(q.ConsumeOverflow());
}
