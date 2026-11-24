#include "../../Common/DiskBaselineLookup.h"
#include <filesystem>
#include <iostream>
#include <cassert>
#include <fstream>
using namespace fsyn;namespace fs=std::filesystem;
static std::string id(unsigned n){char c[64];std::snprintf(c,sizeof(c),"%040x",n);return c;}
int main(){
 auto p=fs::temp_directory_path()/"filesync_v8_disk_index";
 fs::remove_all(p);fs::create_directories(p);
 {ShardedBaselineIndex old(p);assert(old.Reserve(L"a\\old.bin",id(1)));}
 {DiskBaselineLookup s(p);std::string r;
  assert(s.Find(L"a\\old.bin",r) && r==id(1));
  assert(s.Reserve(L"a\\new.bin",id(2)));
  assert(s.Find(L"a\\new.bin",r)&&r==id(2));
  assert(!s.Reserve(L"a\\old.bin",id(3)));
  for(unsigned i=0;i<6000;++i){auto key=L"d\\"+std::to_wstring(i);
   if(!s.Reserve(key,id(i+100))) {std::cerr<<"reserve failed "<<i<<"\n";return 2;}}
 }
 {DiskBaselineLookup s(p);std::string r;
  for(unsigned i=0;i<6000;i+=19) assert(s.Find(L"d\\"+std::to_wstring(i),r)&&r==id(i+100));
  assert(s.Find(L"a\\old.bin",r) && r==id(1));}
 std::cout<<"PASS: immutable sorted disk lookup over V7 log, updates, restart, migration\n";
 fs::remove_all(p);
}
