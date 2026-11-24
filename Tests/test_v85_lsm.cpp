#include "MiniTest.h"
#include "ServerLsmBaselineIndex.h"
#include "DurableWalGroupCommit.h"
#include <filesystem>
#include <atomic>
#include <thread>
#include <vector>
#include <windows.h>
using namespace fsyn;
TEST(v85_shared_serverroot_lsm_group_wal_and_restart) {
    wchar_t tmp[MAX_PATH]{};
    REQUIRE_TRUE(GetTempPathW(MAX_PATH,tmp)>0);
    const auto root=std::filesystem::path(tmp)/
        (L"FileSyncV85_native_"+std::to_wstring(GetCurrentProcessId()));
    std::error_code ec;std::filesystem::remove_all(root,ec);
    const auto a=root/L".fsync_state"/L"10.1.1.1"/L"exeA";
    const auto b=root/L".fsync_state"/L"10.1.1.2"/L"exeB";
    const std::string one(40,'1'),two(40,'2');
    {
        ServerLsmBaselineIndex store(root);
        std::string old;
        REQUIRE_TRUE(store.Find(a,L"X\\first.ini",old));
        EXPECT_TRUE(old.empty());
        REQUIRE_TRUE(store.Reserve(a,L"X\\first.ini",one));
        REQUIRE_TRUE(store.Reserve(b,L"X\\first.ini",two));
        EXPECT_FALSE(store.Reserve(a,L"X\\first.ini",two));
        EXPECT_TRUE(store.Find(a,L"X\\first.ini",old));
        EXPECT_EQ(old,one);
        std::atomic<bool> ok{true};
        std::vector<std::thread> threads;
        for(unsigned t=0;t<4;++t)threads.emplace_back([&,t]{
            const auto& client=(t%2)?b:a;
            for(unsigned i=0;i<80;++i){
                const auto key=L"data\\"+std::to_wstring(t)+L"_"+std::to_wstring(i);
                if(!store.Reserve(client,key,one))ok.store(false);
            }
        });
        for(auto& t:threads)t.join();
        REQUIRE_TRUE(ok.load());
        REQUIRE_TRUE(store.CheckpointForTest());
    }
    {
        ServerLsmBaselineIndex store(root);std::string out;
        REQUIRE_TRUE(store.Find(a,L"X\\first.ini",out));
        EXPECT_EQ(out,one);
        REQUIRE_TRUE(store.Find(b,L"X\\first.ini",out));
        EXPECT_EQ(out,two);
        REQUIRE_TRUE(store.Find(a,L"data\\0_12",out));
        EXPECT_EQ(out,one);
    }
    std::filesystem::remove_all(root,ec);
}
