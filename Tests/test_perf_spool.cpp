#include "MiniTest.h"
#include "DiskSnapshotSpool.h"
#include <windows.h>
#include <filesystem>
#include <fstream>
#include <string>

using namespace fsyn;
TEST(v83_spool_reused_buffers_preserve_record_crc_and_order) {
    wchar_t tmp[MAX_PATH]{};
    REQUIRE_TRUE(GetTempPathW(MAX_PATH,tmp)!=0);
    const auto dir=std::filesystem::path(tmp)/
        (L"v83_spool_"+std::to_wstring(GetCurrentProcessId()));
    std::error_code ec;std::filesystem::remove_all(dir,ec);
    auto file=dir/L"inventory.dat";
    constexpr uint64_t kFiles=12000;
    {
        DiskSnapshotSpool spool;
        REQUIRE_TRUE(spool.Open(file));
        for(uint64_t i=0;i<kFiles;++i) {
            DiskSnapshotSpool::Entry e{
                "project/part_"+std::to_string(i)+".ini",std::string(40,'a'),
                i*31,static_cast<int64_t>(i)};
            REQUIRE_TRUE(spool.Append(e));
        }
        REQUIRE_TRUE(spool.Finish());
        uint64_t next=0;bool ended=false;
        while(!ended) {
            std::vector<DiskSnapshotSpool::Entry> batch;
            REQUIRE_TRUE(spool.ReadBatch(500,batch,ended));
            for(const auto& e:batch) {
                EXPECT_EQ(e.path,"project/part_"+std::to_string(next)+".ini");
                EXPECT_EQ(e.size,next*31);
                EXPECT_EQ(e.mtime,static_cast<int64_t>(next));
                ++next;
            }
        }
        EXPECT_EQ(next,kFiles);
    }
    EXPECT_FALSE(std::filesystem::exists(file));
    std::filesystem::remove_all(dir,ec);
}
TEST(v83_spool_rejects_corrupted_crc) {
    wchar_t tmp[MAX_PATH]{};
    REQUIRE_TRUE(GetTempPathW(MAX_PATH,tmp)!=0);
    const auto dir=std::filesystem::path(tmp)/
        (L"v83_spool_bad_"+std::to_wstring(GetCurrentProcessId()));
    std::error_code ec;std::filesystem::remove_all(dir,ec);
    auto file=dir/L"inventory.dat";
    {
        DiskSnapshotSpool spool;
        REQUIRE_TRUE(spool.Open(file));
        REQUIRE_TRUE(spool.Append({"a/test.ini",std::string(40,'a'),5,4}));
        REQUIRE_TRUE(spool.Finish());
        std::fstream f(file,std::ios::binary|std::ios::in|std::ios::out);
        REQUIRE_TRUE(bool(f));
        f.seekp(3);f.put('X');f.flush();f.close();
        std::vector<DiskSnapshotSpool::Entry> out;bool ended=false;
        EXPECT_FALSE(spool.ReadBatch(500,out,ended));
    }
    std::filesystem::remove_all(dir,ec);
}
