#include "../../Common/DiskBaselineLookup.h"
#include "../../Common/DurableEventLog.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <cassert>
#include <thread>
#include <vector>
using namespace fsyn;namespace fs=std::filesystem;
static std::string id(unsigned n){char out[64];std::snprintf(out,sizeof(out),"%040x",n);return out;}
int main(){
 const auto root=fs::temp_directory_path()/"filesync_v8_faults";
 fs::remove_all(root);fs::create_directories(root);
 {DiskBaselineLookup idx(root);assert(idx.Reserve(L"Data\\critical",id(1)));
  std::string got;assert(idx.Find(L"Data\\critical",got)&&got==id(1));}
 fs::path idxFile,binFile;
 for(const auto& e:fs::directory_iterator(root/"baseline_v7")){
  if(e.path().extension()==".sidx")idxFile=e.path();
  if(e.path().extension()==".bin")binFile=e.path();
 }
 assert(!idxFile.empty()&&!binFile.empty());
 // Torn/corrupt derived index is reconstructed from validated V7 WAL.
 {std::fstream out(idxFile,std::ios::in|std::ios::out|std::ios::binary);
  out.seekp(27);out.put('\x7f');}
 {DiskBaselineLookup idx(root);std::string got;
  assert(idx.Find(L"Data\\critical",got)&&got==id(1));}
 // Authoritative log corruption is never silently accepted for its key.
 {std::fstream out(binFile,std::ios::in|std::ios::out|std::ios::binary);
  out.seekp(8);out.put('\x45');}
 {DiskBaselineLookup idx(root);std::string got;
  assert(!idx.Find(L"Data\\critical",got));}
 fs::remove_all(root);
 std::cout<<"PASS: rebuilt corrupt derived index, failed closed on corrupt authoritative log\n";
}
