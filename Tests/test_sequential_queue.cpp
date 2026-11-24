#include "MiniTest.h"
#include "SequentialTransferQueue.h"
#include <chrono>
#include <string>

using fsyn::SequentialTransferQueue;

TEST(queue_smallest_file_first) {
    SequentialTransferQueue q;
    q.Enqueue("big.bin", 1000, 100);
    q.Enqueue("tiny.json", 8, 100);
    q.Enqueue("middle.txt", 200, 100);
    SequentialTransferQueue::File f;
    EXPECT_TRUE(q.BeginNext(f));
    EXPECT_EQ(f.path, std::string("tiny.json"));
    EXPECT_FALSE(q.BeginNext(f)); // Must wait for commit
    EXPECT_TRUE(q.Commit("tiny.json"));
    EXPECT_TRUE(q.BeginNext(f));
    EXPECT_EQ(f.path, std::string("middle.txt"));
    EXPECT_TRUE(q.Commit("middle.txt"));
    EXPECT_TRUE(q.BeginNext(f));
    EXPECT_EQ(f.path, std::string("big.bin"));
}

TEST(queue_equal_size_newest_first) {
    SequentialTransferQueue q;
    q.Enqueue("older.bin", 99, 100);
    q.Enqueue("newer.bin", 99, 900);
    q.Enqueue("newest.bin", 99, 1000);
    SequentialTransferQueue::File f;
    EXPECT_TRUE(q.BeginNext(f));
    EXPECT_EQ(f.path, std::string("newest.bin"));
    EXPECT_FALSE(q.Commit("older.bin"));
    EXPECT_FALSE(q.BeginNext(f));
    EXPECT_TRUE(q.Commit("newest.bin"));
    EXPECT_TRUE(q.BeginNext(f));
    EXPECT_EQ(f.path, std::string("newer.bin"));
}

TEST(queue_deduplicates_updated_file) {
    SequentialTransferQueue q;
    q.Enqueue("one.bin", 500, 100);
    q.Enqueue("one.bin", 5, 200);
    q.Enqueue("another.bin", 10, 10);
    EXPECT_EQ(q.PendingCount(), size_t(2));
    SequentialTransferQueue::File f;
    EXPECT_TRUE(q.BeginNext(f));
    EXPECT_EQ(f.path, std::string("one.bin"));
    EXPECT_EQ(f.size, uint64_t(5));
    q.Enqueue("one.bin", 11, 300); // Modified while in-flight
    EXPECT_TRUE(q.Commit("one.bin"));
    EXPECT_TRUE(q.BeginNext(f));
    EXPECT_EQ(f.path, std::string("another.bin"));
    EXPECT_TRUE(q.Commit("another.bin"));
    EXPECT_TRUE(q.BeginNext(f));
    EXPECT_EQ(f.path, std::string("one.bin"));
    EXPECT_EQ(f.size, uint64_t(11));
}

TEST(queue_deferred_allows_other_ready_file) {
    SequentialTransferQueue q;
    using Clock = SequentialTransferQueue::Clock;
    const auto now = Clock::now();
    q.Enqueue("unstable.bin", 1, 100);
    q.Enqueue("ready.bin", 2, 100);
    SequentialTransferQueue::File f;
    EXPECT_TRUE(q.BeginNext(f, now));
    EXPECT_EQ(f.path, std::string("unstable.bin"));
    EXPECT_TRUE(q.Defer("unstable.bin", now + std::chrono::seconds(5)));
    EXPECT_TRUE(q.BeginNext(f, now));
    EXPECT_EQ(f.path, std::string("ready.bin"));
    EXPECT_TRUE(q.Commit("ready.bin"));
    EXPECT_FALSE(q.BeginNext(f, now));
    EXPECT_TRUE(q.BeginNext(f, now + std::chrono::seconds(6)));
    EXPECT_EQ(f.path, std::string("unstable.bin"));
}

TEST(queue_10k_files_order_and_no_overlap) {
    SequentialTransferQueue q;
    for (int i = 9999; i >= 0; --i)
        q.Enqueue("file" + std::to_string(i), uint64_t(i), int64_t(i));
    EXPECT_EQ(q.PendingCount(), size_t(10000));
    SequentialTransferQueue::File f;
    for (int i = 0; i < 10000; ++i) {
        EXPECT_TRUE(q.BeginNext(f));
        EXPECT_EQ(f.size, uint64_t(i));
        EXPECT_FALSE(q.BeginNext(f));
        EXPECT_TRUE(q.Commit(f.path));
    }
    EXPECT_EQ(q.PendingCount(), size_t(0));
}
