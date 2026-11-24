#include "ByteRangeDelta.h"
#include <cassert>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <random>
#include <vector>
using namespace fsyn;
using Bytes=std::vector<uint8_t>;
ByteRead R(const Bytes& b) {return [&b](uint64_t off,void*dst,uint32_t len)->uint64_t {
    if(off>b.size()||len>b.size()-off)return uint64_t(-1);
    if(len)std::memcpy(dst,b.data()+off,len);
    return len;};}
int main(){
    constexpr size_t N=6*1024*1024+33;
    Bytes original(N),updated(N);
    std::mt19937 g(20261119);
    for(auto& byte:updated)byte=(uint8_t)g();
    // Fresh file: no committed base; interruptions only preserve stage literals.
    std::vector<ByteSignature> empty;
    std::vector<ByteOp> ops;
    assert(BuildBytePlan(updated.size(),R(updated),64,empty,{},ops));
    for(int percent:{20,50,80,99}){
        Bytes stage(updated.size(),0);
        std::vector<bool> valid(ops.size(),false);
        size_t budget=updated.size()*size_t(percent)/100;
        size_t accepted=0;
        // First run: successful, flushed literals before a simulated crash.
        for(size_t i=0;i<ops.size();++i){
            const auto& op=ops[i];
            if(accepted+op.length>budget)break;
            std::memcpy(stage.data()+op.destination,updated.data()+op.destination,op.length);
            valid[i]=true;accepted+=op.length;
        }
        // Restart: the file stage survives. Every range must be checked again
        // (a journal alone is insufficient), and only missing ranges resend.
        size_t sent=0,reused=0;
        for(size_t i=0;i<ops.size();++i){
            const auto& op=ops[i];
            bool matches=valid[i]&&std::memcmp(stage.data()+op.destination,
                         updated.data()+op.destination,op.length)==0;
            if(matches)reused+=op.length;
            else{
                std::memcpy(stage.data()+op.destination,updated.data()+op.destination,op.length);
                sent+=op.length;
            }
        }
        assert(stage==updated);
        assert(sent+reused==updated.size());
        assert(sent < updated.size());
        std::cout<<"PASS simulated connection loss at "<<percent<<"%"
                 <<" staged_reused="<<reused<<" transferred_after_reconnect="<<sent<<"\n";
    }
}
