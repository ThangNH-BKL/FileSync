#include "MiniTest.h"
#include "SequentialTransferQueue.h"
#include "TransferProgress.h"

TEST(file_progress_counts_verified_not_transmitted_bytes) {
    fsyn::SequentialTransferQueue q;
    q.Enqueue("a.txt", 100, 1);
    q.Enqueue("b.bin", 200, 1);
    q.Enqueue("c.json", 300, 1);
    fsyn::TransferProgress p;
    p.pending = static_cast<uint64_t>(q.PendingCount());
    EXPECT_EQ(p.Total(), uint64_t(3));
    EXPECT_EQ(p.Percent(), 0u);
    fsyn::SequentialTransferQueue::File f;
    EXPECT_TRUE(q.BeginNext(f));
    p.pending = static_cast<uint64_t>(q.PendingCount());
    p.active = q.Active() ? 1 : 0;
    EXPECT_EQ(p.Total(), uint64_t(3));
    EXPECT_EQ(p.Percent(), 0u); // Starting does not count as verified.
    EXPECT_TRUE(q.Commit(f.path));
    ++p.completed;
    ++p.updated;
    p.active = 0;
    p.pending = static_cast<uint64_t>(q.PendingCount());
    EXPECT_EQ(p.Total(), uint64_t(3));
    EXPECT_EQ(p.Percent(), 33u);
    EXPECT_EQ(p.Permille(), 333u);
    EXPECT_TRUE(q.BeginNext(f));
    EXPECT_TRUE(q.Commit(f.path)); // Verified unchanged still counts as processed.
    ++p.completed;
    ++p.unchanged;
    p.pending = static_cast<uint64_t>(q.PendingCount());
    EXPECT_EQ(p.Percent(), 66u);
    EXPECT_EQ(p.updated, uint64_t(1));
    EXPECT_EQ(p.unchanged, uint64_t(1));
    EXPECT_TRUE(q.BeginNext(f));
    EXPECT_TRUE(q.Commit(f.path));
    ++p.completed;
    ++p.updated;
    p.active = 0;
    p.pending = static_cast<uint64_t>(q.PendingCount());
    EXPECT_EQ(p.Percent(), 100u);
}

TEST(file_progress_deferred_does_not_advance_completion) {
    fsyn::SequentialTransferQueue q;
    q.Enqueue("unstable.bin", 9, 10);
    fsyn::SequentialTransferQueue::File f;
    EXPECT_TRUE(q.BeginNext(f));
    fsyn::TransferProgress p;
    p.active = 1;
    EXPECT_EQ(p.Percent(), 0u);
    EXPECT_TRUE(q.Defer(f.path, fsyn::SequentialTransferQueue::Clock::now()));
    p.active = 0;
    p.pending = static_cast<uint64_t>(q.PendingCount());
    EXPECT_EQ(p.Total(), uint64_t(1));
    EXPECT_EQ(p.Percent(), 0u);
}

TEST(file_progress_multi_client_aggregates) {
    fsyn::TransferProgress left, right;
    left.completed=8; left.updated=2; left.unchanged=6; left.pending=2;
    right.completed=1; right.updated=1; right.active=1; right.pending=3;
    left.Accumulate(right);
    EXPECT_EQ(left.Total(), uint64_t(15));
    EXPECT_EQ(left.completed, uint64_t(9));
    EXPECT_EQ(left.Percent(), 60u);
    EXPECT_EQ(left.active, uint64_t(1));
    EXPECT_EQ(left.updated, uint64_t(3));
    EXPECT_EQ(left.unchanged, uint64_t(6));
}
