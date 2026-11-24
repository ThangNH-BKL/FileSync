#include "framework.h"
#include "MiniTest.h"
#include "DeltaSync.h"
#include "Protocol.h"
#include "Crypto.h"
#include <vector>
#include <cstring>

using namespace fsyn;

namespace {
// In-memory reader cho test (offset 64-bit giả lập)
class MemReader : public IBlockReader {
public:
    std::vector<uint8_t> data;
    uint64_t Size() const override { return data.size(); }
    uint64_t ReadAt(uint64_t off, void* buf, uint32_t len) override {
        if (off >= data.size()) return 0;
        uint64_t remain = data.size() - off;
        uint32_t toRead = (uint32_t)(remain < len ? remain : len);
        std::memcpy(buf, data.data() + off, toRead);
        return toRead;
    }
};

void FillSeq(std::vector<uint8_t>& v, size_t n) {
    v.resize(n);
    for (size_t i = 0; i < n; ++i) v[i] = (uint8_t)(i & 0xFF);
}
}

TEST(delta_append) {
    MemReader srv, cli;
    FillSeq(srv.data, 100);
    FillSeq(cli.data, 200);  // client = 100 đầu giống + 100 thêm

    std::vector<BlockEntry> blocks;
    EXPECT_TRUE(ComputeBlockHashes(cli, blocks));
    EXPECT_EQ(blocks.size(), (size_t)1);  // 1 block < 64KB

    bool trunc = false;
    std::vector<uint64_t> need;
    EXPECT_TRUE(DecideNeedOffsets(blocks, &srv, srv.Size(), cli.Size(), trunc, need));
    EXPECT_FALSE(trunc);
    EXPECT_EQ(need.size(), (size_t)1);   // Cần gửi vì CRC khác (100 < 200)
    EXPECT_EQ(need[0], (uint64_t)0);
}

TEST(delta_identical) {
    MemReader srv, cli;
    FillSeq(srv.data, 5000);
    FillSeq(cli.data, 5000);

    std::vector<BlockEntry> blocks;
    EXPECT_TRUE(ComputeBlockHashes(cli, blocks));
    bool trunc = false;
    std::vector<uint64_t> need;
    EXPECT_TRUE(DecideNeedOffsets(blocks, &srv, srv.Size(), cli.Size(), trunc, need));
    EXPECT_FALSE(trunc);
    EXPECT_EQ(need.size(), (size_t)0);   // Không gửi gì
}

TEST(delta_truncate) {
    MemReader srv, cli;
    FillSeq(srv.data, 500);
    FillSeq(cli.data, 200);

    std::vector<BlockEntry> blocks;
    EXPECT_TRUE(ComputeBlockHashes(cli, blocks));
    bool trunc = false;
    std::vector<uint64_t> need;
    EXPECT_TRUE(DecideNeedOffsets(blocks, &srv, srv.Size(), cli.Size(), trunc, need));
    EXPECT_TRUE(trunc);
    EXPECT_EQ(need.size(), (size_t)1);  // block 0 khác (200 vs 500)
}

TEST(delta_empty) {
    MemReader srv, cli;
    FillSeq(srv.data, 0);
    FillSeq(cli.data, 0);
    std::vector<BlockEntry> blocks;
    EXPECT_TRUE(ComputeBlockHashes(cli, blocks));
    bool trunc = false;
    std::vector<uint64_t> need;
    EXPECT_TRUE(DecideNeedOffsets(blocks, &srv, 0, 0, trunc, need));
    EXPECT_FALSE(trunc);
    EXPECT_EQ(need.size(), (size_t)0);
}

TEST(delta_large_simulated) {
    // Mô phỏng file >4GB bằng offset 64-bit (không cần tạo file thật)
    MemReader srv, cli;
    // Chỉ giữ hash, không cần data thật
    std::vector<BlockEntry> blocks;
    // 70000 block × 64KB ≈ 4.4GB
    for (uint64_t i = 0; i < 70000; ++i) {
        BlockEntry b;
        b.offset = i * kBlockSize;
        b.size = kBlockSize;
        b.crc32 = (uint32_t)(i * 0x12345678ull);
        blocks.push_back(b);
    }
    bool trunc = false;
    std::vector<uint64_t> need;
    EXPECT_TRUE(DecideNeedOffsets(blocks, nullptr, 0, 70000ull * kBlockSize, trunc, need));
    EXPECT_FALSE(trunc);
    EXPECT_EQ(need.size(), (size_t)70000);
    EXPECT_EQ(need.back(), (uint64_t)(69999ull * kBlockSize));
}

TEST(delta_sha256_matches_crc32_of_known) {
    // SHA-256 của "abc" = ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad
    MemReader r;
    r.data = { 'a', 'b', 'c' };
    uint8_t sha[32];
    EXPECT_TRUE(ComputeFileSha256(r, sha));
    const char* expectHex = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
    std::string got = Sha256ToHex(sha);
    EXPECT_EQ(got, std::string(expectHex));
}
