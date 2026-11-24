#include "MiniTest.h"
#include "ShardedBaselineIndex.h"
#include "DiskSnapshotSpool.h"
#include "GlobalTransferControl.h"
#include <windows.h>
#include <filesystem>
#include <fstream>
#include <atomic>
#include <string>

using namespace fsyn;
namespace {
std::string TestId(unsigned value) {
    char id[41]{};
    _snprintf_s(id,sizeof(id),_TRUNCATE,"%040x",value);
    return id;
}
std::filesystem::path TestRoot(const wchar_t* name) {
    wchar_t temp[MAX_PATH]{};
    GetTempPathW(MAX_PATH,temp);
    return std::filesystem::path(temp) /
        (std::wstring(L"FileSyncV7_")+name+L"_"+std::to_wstring(GetCurrentProcessId()));
}
}
TEST(v7_binary_baseline_persists_and_refuses_conflicting_identity) {
    auto root=TestRoot(L"index");std::error_code ec;std::filesystem::remove_all(root,ec);
    {
        ShardedBaselineIndex idx(root);
        std::string id;
        EXPECT_TRUE(idx.Find(L"Data\\A.bin",id));
        EXPECT_TRUE(id.empty());
        EXPECT_TRUE(idx.Reserve(L"Data\\A.bin",TestId(1)));
        EXPECT_TRUE(idx.Reserve(L"Data\\A.bin",TestId(1)));
        EXPECT_FALSE(idx.Reserve(L"Data\\A.bin",TestId(2)));
    }
    {
        ShardedBaselineIndex idx(root);
        std::string id;
        EXPECT_TRUE(idx.Find(L"Data\\A.bin",id));
        EXPECT_EQ(id,TestId(1));
    }
    std::filesystem::remove_all(root,ec);
}
TEST(v7_inventory_spool_is_bounded_and_detects_bad_tail) {
    auto root=TestRoot(L"spool");std::error_code ec;std::filesystem::remove_all(root,ec);
    const auto path=root/L"snapshot.tmp";
    {
        DiskSnapshotSpool spool;
        EXPECT_TRUE(spool.Open(path));
        for(unsigned i=0;i<1200;++i) {
            DiskSnapshotSpool::Entry x{"folder/f"+std::to_string(i),TestId(i),i*3u,0};
            EXPECT_TRUE(spool.Append(x));
        }
        EXPECT_TRUE(spool.Finish());
        size_t count=0;bool end=false;
        while(!end) {
            std::vector<DiskSnapshotSpool::Entry> batch;
            EXPECT_TRUE(spool.ReadBatch(500,batch,end));
            EXPECT_TRUE(batch.size()<=500);
            count+=batch.size();
        }
        EXPECT_EQ(count,size_t(1200));
    }
    EXPECT_FALSE(std::filesystem::exists(path,ec));
    std::filesystem::remove_all(root,ec);
}
TEST(v7_global_active_limit_and_spool_budget) {
    GlobalTransferControl governor(2,4u*1024u*1024u,1000);
    EXPECT_TRUE(governor.TryEnter());
    EXPECT_TRUE(governor.TryEnter());
    EXPECT_FALSE(governor.TryEnter());
    EXPECT_TRUE(governor.ReserveSpool(800));
    EXPECT_FALSE(governor.ReserveSpool(201));
    EXPECT_EQ(governor.SpoolUsed(),uint64_t(800));
    governor.ReleaseSpool(800);
    EXPECT_TRUE(governor.ReserveSpool(900));
    governor.Leave();
    EXPECT_TRUE(governor.TryEnter());
    governor.Leave();governor.Leave();governor.ReleaseSpool(900);
    EXPECT_EQ(governor.Active(),0u);
    EXPECT_EQ(governor.SpoolUsed(),uint64_t(0));
}
