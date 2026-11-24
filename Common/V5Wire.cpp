#include "V5Wire.h"
#include <cstring>
namespace fsyn {
namespace {
struct W {
    std::vector<uint8_t>& b;
    void u8(uint8_t x) { b.push_back(x); }
    void u32(uint32_t x) { for (int i=0;i<4;++i) u8((uint8_t)(x>>(i*8))); }
    void u64(uint64_t x) { for (int i=0;i<8;++i) u8((uint8_t)(x>>(i*8))); }
    bool str(const std::string& x) {
        if (x.size()>kMaxRelPathLen || x.size()>UINT16_MAX) return false;
        u8((uint8_t)x.size()); u8((uint8_t)(x.size()>>8));
        b.insert(b.end(),x.begin(),x.end()); return true;
    }
    void sha(const std::array<uint8_t,32>& x){ b.insert(b.end(),x.begin(),x.end()); }
};
struct R {
    const uint8_t* p; uint32_t n; uint32_t pos=0; bool ok=true;
    bool need(uint64_t len) { if (len>n || pos>n-len){ok=false;return false;} return true; }
    uint8_t u8(){ if(!need(1))return 0;return p[pos++]; }
    uint32_t u32(){uint32_t x=0;for(int i=0;i<4;++i)x|=(uint32_t)u8()<<(8*i);return x;}
    uint64_t u64(){uint64_t x=0;for(int i=0;i<8;++i)x|=(uint64_t)u8()<<(8*i);return x;}
    std::string str(){uint16_t l=u8();l|=(uint16_t)u8()<<8;
        if(l>kMaxRelPathLen||!need(l))return {};
        std::string x((const char*)p+pos,l);pos+=l;return x;}
    void sha(std::array<uint8_t,32>& x){if(need(32)){std::memcpy(x.data(),p+pos,32);pos+=32;}}
    bool done()const{return ok&&pos==n;}
};
void begin(std::vector<uint8_t>& b){b.clear();}
bool valid(const std::vector<uint8_t>& b){return b.size()<=kMaxPayload;}
}
bool Pack(const V5SignaturePage& x,std::vector<uint8_t>& b){begin(b);W w{b};
    if(!w.str(x.path)||!w.str(x.expectedInstanceId)||x.prefix.size()>64||x.items.size()>10000)return false;
    w.u64(x.baseSize);w.u32(x.blockSize);w.sha(x.baseSha);
    w.u8((uint8_t)x.prefix.size());b.insert(b.end(),x.prefix.begin(),x.prefix.end());
    w.u32(x.firstIndex);w.u8(x.last?1:0);w.u32((uint32_t)x.items.size());
    for(const auto& s:x.items){w.u64(s.sourceOffset);w.u32(s.length);w.u32(s.weak);w.u64(s.strong);}
    return valid(b);
}
bool Unpack(const uint8_t* p,uint32_t n,V5SignaturePage& x){R r{p,n};x.path=r.str();
    x.expectedInstanceId=r.str();
    x.baseSize=r.u64();x.blockSize=r.u32();r.sha(x.baseSha);uint8_t prefix=r.u8();
    if(prefix>64||!r.need(prefix))return false;
    x.prefix.assign(p+r.pos,p+r.pos+prefix);r.pos+=prefix;
    x.firstIndex=r.u32();uint8_t last=r.u8();if(last>1)return false;x.last=(last==1);
    uint32_t count=r.u32();if(count>10000||!r.need(uint64_t(count)*24))return false;
    x.items.clear();x.items.reserve(count);
    for(uint32_t i=0;i<count;++i){ByteSignature s;s.sourceOffset=r.u64();s.length=r.u32();s.weak=r.u32();s.strong=r.u64();x.items.push_back(s);}
    return r.done() && !x.path.empty();}
bool Pack(const V5Begin& x,std::vector<uint8_t>& b){begin(b);W w{b};if(!w.str(x.path))return false;
    w.u64(x.size);w.sha(x.targetSha);w.sha(x.baseSha);return valid(b);}
bool Unpack(const uint8_t* p,uint32_t n,V5Begin& x){R r{p,n};x.path=r.str();x.size=r.u64();r.sha(x.targetSha);r.sha(x.baseSha);return r.done()&&!x.path.empty();}
bool Pack(const V5Copy& x,std::vector<uint8_t>& b){begin(b);W w{b};if(!w.str(x.path))return false;
    w.u64(x.dest);w.u64(x.source);w.u32(x.length);return valid(b);}
bool Unpack(const uint8_t* p,uint32_t n,V5Copy& x){R r{p,n};x.path=r.str();x.dest=r.u64();x.source=r.u64();x.length=r.u32();return r.done()&&!x.path.empty();}
bool Pack(const V5Check& x,std::vector<uint8_t>& b){begin(b);W w{b};if(!w.str(x.path))return false;
    w.u64(x.dest);w.u32(x.length);w.sha(x.sha);return valid(b);}
bool Unpack(const uint8_t* p,uint32_t n,V5Check& x){R r{p,n};x.path=r.str();x.dest=r.u64();x.length=r.u32();r.sha(x.sha);return r.done()&&!x.path.empty();}
bool Pack(const V5Need& x,std::vector<uint8_t>& b){begin(b);W w{b};if(!w.str(x.path))return false;
    w.u64(x.dest);w.u8(x.need?1:0);return valid(b);}
bool Unpack(const uint8_t* p,uint32_t n,V5Need& x){R r{p,n};x.path=r.str();x.dest=r.u64();uint8_t v=r.u8();if(v>1)return false;x.need=(v==1);return r.done()&&!x.path.empty();}
bool Pack(const V5Bytes& x,std::vector<uint8_t>& b){begin(b);W w{b};if(!w.str(x.path)||x.data.size()>kMaxChunkData)return false;
    w.u64(x.dest);w.u32((uint32_t)x.data.size());b.insert(b.end(),x.data.begin(),x.data.end());return valid(b);}
bool Unpack(const uint8_t* p,uint32_t n,V5Bytes& x){R r{p,n};x.path=r.str();x.dest=r.u64();uint32_t l=r.u32();
    if(l>kMaxChunkData||!r.need(l))return false;
    x.data.assign(p+r.pos,p+r.pos+l);r.pos+=l;
    return r.done()&&!x.path.empty();}
} // namespace fsyn
