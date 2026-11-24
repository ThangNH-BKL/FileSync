#include "../../Common/ShardedBaselineIndex.h"
#include "../../Common/GlobalTransferControl.h"
#include "../../Common/DiskSnapshotSpool.h"
#include <filesystem>
#include <iostream>
#include <string>
#include <atomic>
#include <cassert>
#include <fstream>
#include <vector>
using namespace fsyn;
namespace fs=std::filesystem;
static std::string id(unsigned n){char c[64];snprintf(c,sizeof(c),"%040x",n);return c;}
static std::string hexkey(const std::wstring& s){
  const char* d="0123456789abcdef";std::string x;
  for(wchar_t c:s){unsigned n=unsigned(c)&0xffff;x+=d[(n>>12)&15];x+=d[(n>>8)&15];x+=d[(n>>4)&15];x+=d[n&15];}
  return x;
}
int main(){
  auto home=fs::temp_directory_path()/"filesync_v7_native_test";
  fs::remove_all(home);fs::create_directories(home);
  {
    ShardedBaselineIndex index(home/"newclient");
    std::string r;
    assert(index.Find(L"a\\b.bin",r)&&r.empty());
    assert(index.Reserve(L"a\\b.bin",id(1)));
    assert(index.Reserve(L"a\\b.bin",id(1)));
    assert(!index.Reserve(L"a\\b.bin",id(2)));
    assert(index.Find(L"a\\b.bin",r)&&r==id(1));
    for(unsigned i=0;i<25000;++i) {
      std::wstring key=L"data\\file_"+std::to_wstring(i)+L".bin";
      if(!index.Reserve(key,id(i+10))) {std::cerr<<"reserve failed "<<i<<"\n";return 2;}
    }
  }
  {ShardedBaselineIndex index(home/"newclient");std::string r;
    for(unsigned i=0;i<25000;i+=97){
      auto key=L"data\\file_"+std::to_wstring(i)+L".bin";
      assert(index.Find(key,r)&&r==id(i+10));
    }
  }
  {
    auto legacy=home/"legacyclient";fs::create_directories(legacy);
    std::ofstream file(legacy/"baseline.log",std::ios::binary);
    file<<"FSYN-BASELINE-1\n"<<hexkey(L"a\\older.bin")<<" "<<id(22)<<"\n";
    file.close();
    ShardedBaselineIndex index(legacy);std::string got;
    assert(index.Find(L"a\\older.bin",got)&&got==id(22));
    assert(index.Reserve(L"a\\newer.bin",id(23)));
    assert(fs::exists(legacy/"baseline.log"));
    assert(fs::exists(legacy/"baseline_v7"/"migration.complete"));
  }
  {
    auto file=home/"corruptclient";
    ShardedBaselineIndex index(file);
    assert(index.Reserve(L"test",id(3)));
    auto dir=file/"baseline_v7";
    for(const auto& entry:fs::directory_iterator(dir)){
      if(entry.path().extension()==".bin"){
        std::ofstream f(entry.path(),std::ios::binary|std::ios::app);
        f<<"BROKEN";break;
      }
    }
    ShardedBaselineIndex again(file);std::string result;
    assert(!again.Find(L"test",result));
  }
  {
    DiskSnapshotSpool spool;
    assert(spool.Open(home/"spools"/"sample.tmp"));
    for(int i=0;i<12500;++i){
      DiskSnapshotSpool::Entry e{"d/file_"+std::to_string(i),id(i),uint64_t(i*20),int64_t(i)};
      assert(spool.Append(e));
    }
    assert(spool.Finish());
    size_t total=0;bool end=false;
    while(!end){
      std::vector<DiskSnapshotSpool::Entry> batch;
      assert(spool.ReadBatch(500,batch,end));
      assert(batch.size()<=500);
      for(auto& e:batch){auto i=total++;assert(e.path=="d/file_"+std::to_string(i));assert(e.size==i*20);}
    }
    assert(total==12500);
    spool.Close(true);
    assert(!fs::exists(home/"spools"/"sample.tmp"));
  }
  {
    GlobalTransferControl gate(3,1024*1024);
    assert(gate.TryEnter()&&gate.TryEnter()&&gate.TryEnter());
    assert(!gate.TryEnter());
    assert(gate.Active()==3);
    gate.Leave();assert(gate.TryEnter());
    gate.Leave();gate.Leave();gate.Leave();
    std::atomic<bool> stopped{false};
    assert(gate.PaceReceived(64*1024,stopped));
  }
  { // Round-robin FIFO fairness among session identities.
    GlobalTransferControl gate(1,1024*1024);
    int a=1,b=2,c=3;
    assert(gate.TryEnter(&a));
    assert(!gate.TryEnter(&b));
    assert(!gate.TryEnter(&c));
    gate.Leave(&a);
    assert(!gate.TryEnter(&c));
    assert(gate.TryEnter(&b));
    gate.Leave(&b);
    assert(gate.TryEnter(&c));
    gate.Cancel(&c);
    assert(gate.Active()==0);
  }
  { // Incomplete legacy baseline is rejected before any migration marker.
    auto dir=home/"partial_legacy";fs::create_directories(dir);
    std::ofstream f(dir/"baseline.log",std::ios::binary);
    f<<"FSYN-BASELINE-1\n"<<hexkey(L"a\\b")<<" "<<id(5);f.close();
    ShardedBaselineIndex idx(dir);std::string x;
    assert(!idx.Find(L"a\\b",x));
    assert(!fs::exists(dir/"baseline_v7"/"migration.complete"));
  }
  std::cout<<"PASS: binary index 25k, legacy migration, corruption fails closed, "
           <<"snapshot spool 12.5k, global gate\n";
  fs::remove_all(home);
  return 0;
}
