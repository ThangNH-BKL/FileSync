#define FSYNC_LSM_TEST_MEMTABLE_LIMIT 64
#include "../../Common/ServerLsmBaselineIndex.h"
#include <cassert>
#include <iostream>
#include <filesystem>
#include <thread>
#include <atomic>
#include <cstdio>
using namespace fsyn;
namespace fs=std::filesystem;
static std::string id(unsigned n){char s[64];std::snprintf(s,sizeof(s),"%040x",n);return s;}
int main(){
 const auto p=fs::temp_directory_path()/"fsync_v85_lsm_native";
 fs::remove_all(p);fs::create_directories(p);
 const auto c1=p/".fsync_state"/"192.168.1.1"/"exeA";
 const auto c2=p/".fsync_state"/"192.168.1.2"/"exeB";
 {
   ServerLsmBaselineIndex idx(p);std::string out;
   assert(idx.Find(c1,L"a\\hello.ini",out)&&out.empty());
   assert(idx.Reserve(c1,L"a\\hello.ini",id(1)));
   assert(idx.Find(c1,L"a\\hello.ini",out)&&out==id(1));
   assert(!idx.Reserve(c1,L"a\\hello.ini",id(4)));
   assert(idx.Reserve(c1,L"a\\hello.ini",id(1)));
   assert(idx.Reserve(c2,L"a\\hello.ini",id(3)));
   assert(idx.Find(c2,L"a\\hello.ini",out)&&out==id(3));
   // Two racing reservations for the same key cannot install conflicting IDs.
   std::atomic<int> winners{0};
   std::thread ra([&]{if(idx.Reserve(c1,L"racing\\same.ini",id(901)))++winners;});
   std::thread rb([&]{if(idx.Reserve(c1,L"racing\\same.ini",id(902)))++winners;});
   ra.join();rb.join();
   assert(winners.load()==1);
   // An independent object cannot acquire the same ServerRoot writer lock.
   {ServerLsmBaselineIndex duplicate(p);std::string x;
    assert(!duplicate.Find(c1,L"a\\hello.ini",x));}

   std::atomic<bool> ok{true};
   std::vector<std::thread> workers;
   for(int t=0;t<8;++t)workers.emplace_back([&,t]{
     for(int i=0;i<250;++i){
       const auto& client=(t%2)?c2:c1;
       std::wstring key=L"d\\"+std::to_wstring(t)+L"_"+std::to_wstring(i);
       if(!idx.Reserve(client,key,id(10+t*250+i)))ok=false;
     }
   });
   for(auto& w:workers)w.join();
   assert(ok.load());
   const auto commits=DurableWalGroupCommit::Global().Stats();
   assert(commits.records>=2003 && commits.flushes>0);
   assert(commits.flushes<commits.records);
   std::cout<<"Group flushes="<<commits.flushes<<" for "<<commits.records<<" WAL records\n";
   auto stats=idx.GetStats();
   assert(stats.runs>0 && stats.covered>8 && stats.walBytes>stats.covered);
   assert(idx.CheckpointForTest());
   stats=idx.GetStats();assert(stats.memEntries==0 && stats.runs>0);
   for(int t=0;t<8;++t)for(int i=0;i<250;i+=31){
     const auto& client=(t%2)?c2:c1;
     std::wstring key=L"d\\"+std::to_wstring(t)+L"_"+std::to_wstring(i);
     assert(idx.Find(client,key,out)&&out==id(10+t*250+i));
   }
 }
 {
   ServerLsmBaselineIndex idx(p);std::string out;
   for(int t=0;t<8;++t)for(int i=0;i<250;i+=17){
     const auto& client=(t%2)?c2:c1;
     std::wstring key=L"d\\"+std::to_wstring(t)+L"_"+std::to_wstring(i);
     if(!idx.Find(client,key,out)||out!=id(10+t*250+i)){
       std::cerr<<"restart missing key: "<<t<<"/"<<i<<"\n";return 2;
     }
   }
   assert(idx.Reserve(c1,L"b\\after_restart.ini",id(54321)));
 }
 {
   ServerLsmBaselineIndex idx(p);std::string out;
   assert(idx.Find(c1,L"b\\after_restart.ini",out)&&out==id(54321));
   assert(idx.Find(c1,L"missing",out)&&out.empty());
 }
 // Corrupted WAL tails are never silently skipped or acknowledged.
 {
   auto wal=p/".fsync_state"/"baseline_lsm_v85"/"baseline.wal";
   std::ofstream bad(wal,std::ios::binary|std::ios::app);
   const char fragment[3]={'B','W','8'};
   bad.write(fragment,3);bad.close();
   ServerLsmBaselineIndex idx(p);std::string out;
   assert(!idx.Find(c1,L"a\\hello.ini",out));
   fs::resize_file(wal,fs::file_size(wal)-3);
 }
 {
   ServerLsmBaselineIndex idx(p);std::string out;
   assert(idx.Find(c1,L"a\\hello.ini",out)&&out==id(1));
 }
 std::cout<<"PASS V8.5 shared WAL, grouped fsync, tiered LSM, restart, corrupted-tail fail-closed\n";
 fs::remove_all(p);
}
