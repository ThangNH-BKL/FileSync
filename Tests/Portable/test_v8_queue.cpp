#include "../../Common/DurableEventLog.h"
#include <filesystem>
#include <iostream>
#include <fstream>
#include <cassert>
using namespace fsyn;
namespace fs=std::filesystem;
int main() {
 auto p=fs::temp_directory_path()/"filesync_v8_queue_test";
 fs::remove_all(p);fs::create_directories(p);
 auto file=p/"events.wal";
 FileEventMsg x{};x.action=FileAction::Modified;x.relPath="data/test.txt";x.size=123;x.mtimeNs=42;
 {DurableEventLog q(file);assert(q.Open());assert(q.Push(x));x.relPath="data/second";assert(q.Push(x));
  auto a=q.UnsentBatch(10);assert(a.size()==2 && a[0].seq==1 && a[1].seq==2);
  q.MarkSent(a[0].seq);assert(q.AckSentPath("data/test.txt"));assert(q.Size()==1);}
 {DurableEventLog q(file);assert(q.Open());auto a=q.UnsentBatch(10);
  assert(a.size()==1 && a[0].seq==2 && a[0].event.relPath=="data/second");
  q.MarkSent(a[0].seq);assert(q.AckSentPath("data/second"));assert(q.Size()==0);
  assert(q.CompactIfEmpty());}
 {DurableEventLog q(file);assert(q.Open());assert(q.Size()==0);assert(q.Push(x));}
 {std::ofstream out(file,std::ios::binary|std::ios::app);out.put('\x01');}
 {DurableEventLog q(file);assert(!q.Open());}
 fs::remove_all(p);
 std::cout<<"PASS: V8 durable event queue replay, ack, compaction, torn-tail rejection\n";
}
