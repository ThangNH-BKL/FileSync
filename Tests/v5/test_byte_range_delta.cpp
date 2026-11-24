#include "ByteRangeDelta.h"
#include "V5Wire.h"
#include <algorithm>
#include <cassert>
#include <cstring>
#include <iostream>
#include <random>
#include <string>
#include <vector>
using namespace fsyn;
using Bytes = std::vector<uint8_t>;
ByteRead Reader(const Bytes& data) {
    return [&data](uint64_t p, void* b, uint32_t n) -> uint64_t {
        if (p > data.size() || n > data.size()-p) return uint64_t(-1);
        if (n) std::memcpy(b, data.data()+p, n);
        return n;
    };
}
void Trial(std::string name, const Bytes& oldData, const Bytes& newData,
           uint64_t maximumLiteral) {
    uint32_t block=SelectDeltaBlockSize(oldData.size());
    std::vector<ByteSignature> sig;
    assert(BuildByteSignatures(oldData.size(),Reader(oldData),block,sig));
    Bytes prefix(oldData.begin(),oldData.begin()+std::min(size_t(64),oldData.size()));
    std::vector<ByteOp> ops;
    assert(BuildBytePlan(newData.size(),Reader(newData),block,sig,prefix,ops));
    Bytes recreated(newData.size());
    uint64_t literal=0, copied=0;
    for(const auto& o:ops){
        assert(o.destination + o.length <= recreated.size());
        if(o.kind == ByteOp::Copy){
            assert(o.source + o.length <= oldData.size());
            std::memcpy(recreated.data()+o.destination,oldData.data()+o.source,o.length);
            copied+=o.length;
        }else{
            std::memcpy(recreated.data()+o.destination,newData.data()+o.destination,o.length);
            literal+=o.length;
        }
    }
    assert(recreated == newData);
    assert(literal + copied == newData.size());
    assert(literal <= maximumLiteral);
    std::cout<<"PASS "<<name<<" newSize="<<newData.size()
        <<" copied="<<copied<<" literal="<<literal
        <<" opCount="<<ops.size()<<"\n";
}
int main(){
    // A realistic 136 KB text file, exactly one byte inserted in the header.
    std::string header="time\tSensor_SN\tBarcode\n";
    Bytes a(header.begin(),header.end());
    std::mt19937 gen(1234);
    while(a.size()<136115) a.push_back(uint8_t(32+gen()%90));
    Bytes b=a;b.insert(b.begin()+header.find("Barcode")+7,'e');
    Trial("single-char insertion near header",a,b,1);
    Bytes changed=a;changed[9]='X';
    Trial("same-size one-byte replacement",a,changed,1);
    Bytes c=a;c.insert(c.begin()+65432,77);
    Trial("single middle insertion",a,c,128);
    Bytes gone=a;gone.erase(gone.begin()+45001);
    Trial("single middle deletion",a,gone,128);
    Trial("identical",a,a,0);
    Trial("new file",{},b,b.size());
    Trial("empty file",{},{},0);
    Trial("truncation",a,Bytes(a.begin(),a.begin()+140),64);
    Bytes binary(1024*1024+47);
    for(auto& x:binary)x=uint8_t(gen());
    Bytes binaryInserted=binary;binaryInserted.insert(binaryInserted.begin()+17,0);
    Trial("binary NUL insertion in header",binary,binaryInserted,1);
    Bytes binaryChanged=binary;binaryChanged[700001]^=0xff;
    Trial("binary bit modification",binary,binaryChanged,128);
    Bytes zeroFilled(1024*1024,0);
    Bytes zeroExtended=zeroFilled;zeroExtended.insert(zeroExtended.begin()+123456,0x17);
    Trial("duplicate-content binary insertion",zeroFilled,zeroExtended,128);

    // Protocol pack/unpack pages and data checks.
    V5SignaturePage page;page.path="x.bin";page.baseSize=a.size();page.blockSize=64;
    page.firstIndex=0;page.last=false;page.prefix.assign(a.begin(),a.begin()+64);
    std::vector<uint8_t> wire;
    assert(Pack(page,wire));
    V5SignaturePage decoded;
    assert(Unpack(wire.data(), (uint32_t)wire.size(), decoded));
    assert(decoded.path==page.path && decoded.prefix==page.prefix);
    V5Check check;check.path="x.bin";check.dest=991;check.length=17;
    assert(Pack(check,wire));V5Check checkBack;
    assert(Unpack(wire.data(),(uint32_t)wire.size(),checkBack));
    assert(checkBack.dest==991 && checkBack.length==17);
    for(uint32_t n=0; n<(uint32_t)wire.size(); ++n){
        V5Check truncated;
        assert(!Unpack(wire.data(),n,truncated));
    }
    std::cout << "PASS v5 wire roundtrips\n";
    return 0;
}
