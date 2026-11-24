#define FSYNC_LSM_TEST_MEMTABLE_LIMIT 32
#include "../../Common/ServerLsmBaselineIndex.h"
#include <cassert>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
using namespace fsyn;
namespace fs=std::filesystem;
static std::string id(unsigned n){char s[64];std::snprintf(s,sizeof(s),"%040x",n);return s;}
static void FlipByte(const fs::path& p,std::streamoff n){
 std::fstream f(p,std::ios::binary|std::ios::in|std::ios::out);assert(f);
 f.seekg(n);char c=0;f.read(&c,1);assert(f);c^=0x55;f.seekp(n);f.write(&c,1);f.close();
}
int main(){
 auto root=fs::temp_directory_path()/"fsync_v85_faults";
 fs::remove_all(root);
 auto client=root/".fsync_state"/"10.0.0.1"/"exe";
 {
  ServerLsmBaselineIndex idx(root);
  for(unsigned i=0;i<256;++i)assert(idx.Reserve(client,L"data\\"+std::to_wstring(i),id(i+1)));
  assert(idx.CheckpointForTest());
 }
 auto dir=root/".fsync_state"/"baseline_lsm_v85";
 auto manifest=dir/"manifest.bin";
 auto backup=dir/"manifest.backup";
 fs::copy_file(manifest,backup,fs::copy_options::overwrite_existing);
 FlipByte(manifest,10);
 {ServerLsmBaselineIndex idx(root);std::string v;
  assert(!idx.Find(client,L"data\\5",v));}
 fs::copy_file(backup,manifest,fs::copy_options::overwrite_existing);
 {ServerLsmBaselineIndex idx(root);std::string v;
  assert(idx.Find(client,L"data\\5",v) && v==id(6));}
 // SST damage: index must reject, not return an apparently absent key.
 fs::path firstSst;
 {
   std::ifstream mf(manifest,std::ios::binary);
   assert(mf);mf.seekg(28);
   uint8_t b[12]{};mf.read(reinterpret_cast<char*>(b),12);assert(mf);
   unsigned level=unsigned(b[0])|(unsigned(b[1])<<8)|(unsigned(b[2])<<16)|(unsigned(b[3])<<24);
   uint64_t runId=0;for(int i=0;i<8;++i)runId|=uint64_t(b[i+4])<<(8*i);
   char filename[80]{};
   std::snprintf(filename,sizeof(filename),"L%u_%020llu.sst",level,
                 static_cast<unsigned long long>(runId));
   firstSst=dir/"runs"/filename;
 }
 assert(!firstSst.empty());
 auto bak=firstSst;bak+=".backup";
 fs::copy_file(firstSst,bak,fs::copy_options::overwrite_existing);
 FlipByte(firstSst,45);
 {ServerLsmBaselineIndex idx(root);std::string v;
  assert(!idx.Find(client,L"data\\5",v));}
 fs::copy_file(bak,firstSst,fs::copy_options::overwrite_existing);
 {ServerLsmBaselineIndex idx(root);std::string v;
  assert(idx.Find(client,L"data\\5",v) && v==id(6));}
 fs::remove_all(root);
 std::cout<<"PASS V8.5 CRC manifest/SST corruption rejected; restored state reopens\n";
}
