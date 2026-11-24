#include "MiniTest.h"
#include "DurableEventLog.h"
#include "DiskBaselineLookup.h"
#include "UsnJournalGuard.h"
#include <filesystem>
#include <fstream>
#include <windows.h>
using namespace fsyn;
TEST(v8_durable_event_log_survives_restart_and_fails_closed) {
    wchar_t tmp[MAX_PATH]{};GetTempPathW(MAX_PATH,tmp);
    auto dir=std::filesystem::path(tmp)/(L"v8_durable_"+std::to_wstring(GetCurrentProcessId()));
    std::error_code ec;std::filesystem::remove_all(dir,ec);
    auto path=dir/L"queue.wal";
    FileEventMsg ev{};ev.action=FileAction::Modified;ev.relPath="Data/A.bin";ev.size=5;
    {DurableEventLog q(path);EXPECT_TRUE(q.Open());EXPECT_TRUE(q.Push(ev));}
    {DurableEventLog q(path);EXPECT_TRUE(q.Open());auto b=q.UnsentBatch(2);
     EXPECT_EQ(b.size(),size_t(1));if(!b.empty()){q.MarkSent(b[0].seq);
       EXPECT_TRUE(q.AckSentPath("Data/A.bin"));}
     EXPECT_EQ(q.Size(),size_t(0));EXPECT_TRUE(q.CompactIfEmpty());}
    {DurableEventLog q(path);EXPECT_TRUE(q.Open());EXPECT_EQ(q.Size(),size_t(0));}
    std::filesystem::remove_all(dir,ec);
}
TEST(v8_disk_lookup_retains_v7_log_and_conflict_detection) {
    wchar_t tmp[MAX_PATH]{};GetTempPathW(MAX_PATH,tmp);
    auto root=std::filesystem::path(tmp)/(L"v8_index_"+std::to_wstring(GetCurrentProcessId()));
    std::error_code ec;std::filesystem::remove_all(root,ec);
    const std::string first(40,'1'),second(40,'2');
    {ShardedBaselineIndex v7(root);EXPECT_TRUE(v7.Reserve(L"Data\\A.bin",first));}
    {DiskBaselineLookup v8(root);std::string got;
      EXPECT_TRUE(v8.Find(L"Data\\A.bin",got));EXPECT_EQ(got,first);
      EXPECT_TRUE(v8.Reserve(L"Data\\B.bin",second));
      EXPECT_FALSE(v8.Reserve(L"Data\\A.bin",second));}
    {DiskBaselineLookup after(root);std::string got;
      EXPECT_TRUE(after.Find(L"Data\\B.bin",got));EXPECT_EQ(got,second);}
    std::filesystem::remove_all(root,ec);
}
TEST(v8_usn_checkpoint_rejects_journal_wrap_or_replacement) {
    UsnState now{99,100,130,500};
    EXPECT_TRUE(IsUsnCursorUsable(now,{99,350}));
    EXPECT_FALSE(IsUsnCursorUsable(now,{99,129}));
    EXPECT_FALSE(IsUsnCursorUsable(now,{98,350}));
    EXPECT_FALSE(IsUsnCursorUsable(now,{99,501}));
}
