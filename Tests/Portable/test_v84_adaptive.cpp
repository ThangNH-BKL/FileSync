#include "../../Common/AdaptiveServerPolicy.h"
#include "../../Common/DurabilityTelemetry.h"
#include "../../Common/StageFlushPolicy.h"
#include "../../Common/AdaptiveSleepPolicy.h"
#include <cassert>
#include <iostream>
using namespace fsyn;
int main(){
  AdaptiveServerPolicy p(1024*1024,10ull*1024*1024,4ull*1024*1024);
  AdaptiveServerPolicy::Sample s{};s.externalFraction=.02;s.activeTransfers=2;
  for(int i=0;i<100;++i)p.Step(s);
  assert(p.Rate()==10ull*1024*1024);
  s.externalFraction=.5;
  for(int i=0;i<100;++i)p.Step(s);
  assert(p.Rate()==1024ull*1024);
  s.externalFraction=-1;assert(p.Step(s)==1024ull*1024);
  s.externalFraction=0.03;s.flushCount=10;s.flushUs=800000;s.activeTransfers=2;
  assert(p.Step(s)==1024ull*1024);
  s.flushUs=0;s.flushCount=0;s.activeTransfers=0;
  assert(p.Step(s)==1024ull*1024);
  assert(AdaptiveWaitSlice(std::chrono::milliseconds(13)).count()==13);
  assert(AdaptiveWaitSlice(std::chrono::milliseconds(90)).count()==20);
  assert(ShouldFlushStageLiteral(65536,0));
  assert(!ShouldFlushStageLiteral(65536,4ull*1024*1024));
  assert(ShouldFlushStageLiteral(4ull*1024*1024,4ull*1024*1024));
  auto before=durability::Read();
  durability::RecordBaseline(13);durability::RecordManifest(17);
  auto after=durability::Read();
  assert(after.baselineCount==before.baselineCount+1);
  assert(after.manifestCount==before.manifestCount+1);
  assert(after.baselineUs==before.baselineUs+13);
  assert(after.manifestUs==before.manifestUs+17);
  std::cout<<"PASS: v8.4 bounded AIMD + durability telemetry"<<std::endl;
}
