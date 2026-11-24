#pragma once
// FileSync V8 experimental immutable sorted-run lookup over V7 shard logs.
// V7 *.bin remains authoritative and is never rewritten/deleted.
// *.sidx is a rebuildable sorted hash->log-offset index, not a database.
// This reduces cold RAM use and random lookups; group commit is NOT included.
// This adapter must not run concurrently with V7 on the same ServerRoot.
#include "ShardedBaselineIndex.h"
#include "DurabilityTelemetry.h"
#include <atomic>
#include <array>
#include <vector>
#include <map>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <mutex>
#include <limits>
#include <system_error>
namespace fsyn {
class DiskBaselineLookup {
public:
    explicit DiskBaselineLookup(std::filesystem::path base)
        :base_(std::move(base)),migrator_(base_) {}
    ~DiskBaselineLookup() {
        for(const auto& pair:cache_) totalRecent_.fetch_sub(pair.second.delta.size());
    }
    bool Find(const std::wstring& key,std::string& out) {
        std::lock_guard<std::mutex> g(mu_);
        if(!EnsureMigration()) return false;
        return Lookup(key,out);
    }
    bool Reserve(const std::wstring& key,const std::string& id) {
        if(key.empty() || id.size()!=40) return false;
        for(char c:id) if(!((c>='0'&&c<='9')||(c>='a'&&c<='f'))) return false;
        std::lock_guard<std::mutex> g(mu_);
        if(!EnsureMigration()) return false;
        std::string old;
        if(!Lookup(key,old)) return false;
        if(!old.empty()) return old==id;
        const auto shard=Shard(key);
        auto bytes=Encode(key,id);
        if(bytes.empty() || !AppendSynced(LogPath(shard),bytes)) {
            cache_.clear();return false;
        }
        auto it=cache_.find(shard);
        if(it==cache_.end()) return false; // fail-closed (should be impossible)
        auto& s=it->second;
        s.logSize+=bytes.size();
        s.delta.emplace(key,id);
        ++totalRecent_;
        // Bounded cache even with many Client roots. Never ACK until
        // authoritative log was flushed. Rebuildable index is not required
        // for ACK durability.
        if(s.delta.size()>=2048 || totalRecent_.load()>150000) {
            if(!Rebuild(shard,s)) return false;
        }
        return true;
    }
    std::filesystem::path Root() const {return base_/L"baseline_v7";}
private:
    struct Ref {uint64_t hash=0,offset=0;};
    struct State {uint64_t indexedSize=0,logSize=0,tick=0;
                  std::map<std::wstring,std::string> delta;};
    std::filesystem::path base_;
    ShardedBaselineIndex migrator_;
    std::map<unsigned,State> cache_;
    std::mutex mu_;
    bool migrated_=false;
    uint64_t tick_=0;
    static std::atomic<size_t> totalRecent_;
    static uint32_t Crc(const uint8_t* p,size_t n) {
        uint32_t crc=0xffffffffu;
        for(size_t i=0;i<n;++i){crc^=p[i];for(int j=0;j<8;++j)crc=(crc>>1)^(0xedb88320u&(0u-(crc&1u)));}
        return ~crc;
    }
    static void U16(std::vector<uint8_t>& a,uint16_t n){a.push_back(uint8_t(n));a.push_back(uint8_t(n>>8));}
    static void U32(std::vector<uint8_t>& a,uint32_t n){for(int j=0;j<4;++j)a.push_back(uint8_t(n>>(8*j)));}
    static void U64(std::vector<uint8_t>& a,uint64_t n){for(int j=0;j<8;++j)a.push_back(uint8_t(n>>(8*j)));}
    static uint16_t R16(const uint8_t* p){return uint16_t(p[0])|(uint16_t(p[1])<<8);}
    static uint32_t R32(const uint8_t* p){uint32_t n=0;for(int j=0;j<4;++j)n|=uint32_t(p[j])<<(8*j);return n;}
    static uint64_t R64(const uint8_t* p){uint64_t n=0;for(int j=0;j<8;++j)n|=uint64_t(p[j])<<(8*j);return n;}
    static uint64_t Hash(const std::wstring& key) {
        uint64_t h=14695981039346656037ull;
        for(wchar_t wc:key){auto c=uint16_t(wc);
            h^=uint8_t(c);h*=1099511628211ull;
            h^=uint8_t(c>>8);h*=1099511628211ull;}
        return h;
    }
    static unsigned Shard(const std::wstring& key){return unsigned(Hash(key)%1024);}
    static std::vector<uint8_t> Encode(const std::wstring& key,const std::string& id) {
        if(key.empty() || key.size()>4096 || id.size()!=40) return {};
        std::vector<uint8_t> a;
        U32(a,0x31425346u);U16(a,uint16_t(key.size()*2));U16(a,40);
        for(wchar_t wc:key)U16(a,uint16_t(wc));
        a.insert(a.end(),id.begin(),id.end());U32(a,Crc(a.data(),a.size()));
        return a;
    }
    std::filesystem::path LogPath(unsigned shard) const {
        wchar_t name[32]{};
#ifdef _WIN32
        swprintf_s(name,L"shard_%04u.bin",shard);
#else
        swprintf(name,32,L"shard_%04u.bin",shard);
#endif
        return Root()/name;
    }
    std::filesystem::path IdxPath(unsigned shard) const {
        auto name=LogPath(shard);name.replace_extension(L".sidx");return name;
    }
    bool EnsureMigration() {
        if(migrated_) return true;
        std::string unused;
        // This invokes the V7 idempotent legacy baseline.log migration.
        if(!migrator_.Find(L"__v8_migration_probe__",unused)) return false;
        migrated_=true;return true;
    }
    static bool AppendSynced(const std::filesystem::path& path,const std::vector<uint8_t>& bytes){
        std::error_code ec;std::filesystem::create_directories(path.parent_path(),ec);
        if(ec)return false;
#ifdef _WIN32
        HANDLE h=CreateFileW(path.c_str(),FILE_APPEND_DATA,FILE_SHARE_READ,nullptr,
            OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(h==INVALID_HANDLE_VALUE)return false;
        DWORD n=0;bool ok=WriteFile(h,bytes.data(),DWORD(bytes.size()),&n,nullptr)!=FALSE &&
            n==bytes.size();
        if(ok) {
            const auto started=durability::Clock::now();
            ok=FlushFileBuffers(h)!=FALSE;
            durability::RecordBaseline(durability::Us(started));
        }
        CloseHandle(h);return ok;
#else
        int fd=open(path.c_str(),O_WRONLY|O_APPEND|O_CREAT,0600);if(fd<0)return false;
        size_t off=0;bool ok=true;
        while(off<bytes.size()) {auto n=write(fd,bytes.data()+off,bytes.size()-off);
            if(n<=0){ok=false;break;}off+=size_t(n);}
        if(ok) ok=fsync(fd)==0;
        close(fd);return ok;
#endif
    }
    // Read one V7 record at an exact byte offset and verify its CRC.
    static bool ReadRecord(std::ifstream& in,uint64_t offset,std::wstring& key,std::string& id,uint64_t& next){
        in.clear();in.seekg(std::streamoff(offset));if(!in)return false;
        uint8_t hdr[8]{};if(!in.read(reinterpret_cast<char*>(hdr),8))return false;
        const uint16_t n=R16(hdr+4),v=R16(hdr+6);
        if(R32(hdr)!=0x31425346u || !n || n>8192 || n%2 || v!=40)return false;
        std::vector<uint8_t> body(size_t(n)+44);
        if(!in.read(reinterpret_cast<char*>(body.data()),std::streamsize(body.size())))return false;
        std::vector<uint8_t> check(hdr,hdr+8);
        check.insert(check.end(),body.begin(),body.end()-4);
        if(Crc(check.data(),check.size())!=R32(body.data()+body.size()-4))return false;
        key.clear();key.reserve(n/2);
        for(size_t i=0;i<n;i+=2)key.push_back(wchar_t(R16(body.data()+i)));
        id.assign(reinterpret_cast<const char*>(body.data()+n),40);
        for(char c:id)if(!((c>='0'&&c<='9')||(c>='a'&&c<='f')))return false;
        next=offset+8+body.size();return true;
    }
    static bool AtomicWrite(const std::filesystem::path& final,const std::vector<uint8_t>& data){
        std::error_code ec;auto tmp=final;tmp+=L".writing";
        std::filesystem::create_directories(final.parent_path(),ec);if(ec)return false;
#ifdef _WIN32
        HANDLE h=CreateFileW(tmp.c_str(),GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(h==INVALID_HANDLE_VALUE)return false;
        bool ok=true;size_t off=0;
        while(off<data.size()) {DWORD n=0;DWORD count=DWORD((std::min)(data.size()-off,size_t(1<<20)));
            if(!WriteFile(h,data.data()+off,count,&n,nullptr)||n!=count){ok=false;break;}off+=count;}
        if(ok)ok=FlushFileBuffers(h)!=FALSE;
        CloseHandle(h);
        if(!ok)return false;
        return MoveFileExW(tmp.c_str(),final.c_str(),
            MOVEFILE_WRITE_THROUGH|MOVEFILE_REPLACE_EXISTING)!=FALSE;
#else
        int fd=open(tmp.c_str(),O_WRONLY|O_CREAT|O_TRUNC,0600);if(fd<0)return false;
        size_t off=0;bool ok=true;
        while(off<data.size()){ssize_t n=write(fd,data.data()+off,data.size()-off);
            if(n<=0){ok=false;break;}off+=size_t(n);}
        if(ok) ok=fsync(fd)==0;
        close(fd);
        if(!ok)return false;
        std::filesystem::rename(tmp,final,ec);return !ec;
#endif
    }
    // *.sidx: magic[8], logSize[8], count[8], entriesCrc[4], headerCrc[4],
    // then count (hash[8], V7 byte offset[8]) pairs sorted by hash,offset.
    bool Build(unsigned shard,uint64_t size) {
        std::vector<Ref> refs;
        std::ifstream log(LogPath(shard),std::ios::binary);
        if(size && !log)return false;
        uint64_t pos=0;
        while(pos<size) {
            if(refs.size()>=5000000) return false; // hard RAM safety bound / shard
            std::wstring key;std::string id;uint64_t next=0;
            if(!ReadRecord(log,pos,key,id,next)||next>size||Shard(key)!=shard) return false;
            refs.push_back({Hash(key),pos});pos=next;
        }
        if(pos!=size)return false;
        std::sort(refs.begin(),refs.end(),[](const Ref&a,const Ref&b){
            return a.hash<b.hash || (a.hash==b.hash && a.offset<b.offset);});
        std::vector<uint8_t> out;out.reserve(32+refs.size()*16);
        const char magic[]="FSBIDX08";out.insert(out.end(),magic,magic+8);
        U64(out,size);U64(out,refs.size());
        std::vector<uint8_t> body;body.reserve(refs.size()*16);
        for(const auto& r:refs){U64(body,r.hash);U64(body,r.offset);}
        U32(out,Crc(body.data(),body.size()));
        U32(out,Crc(out.data(),out.size()));
        out.insert(out.end(),body.begin(),body.end());
        return AtomicWrite(IdxPath(shard),out);
    }
    static bool ValidIndex(const std::filesystem::path& file,uint64_t logSize) {
        std::error_code ec;
        if(!std::filesystem::exists(file,ec)||ec)return false;
        const uint64_t actual=std::filesystem::file_size(file,ec);
        if(ec || actual<32)return false;
        std::ifstream in(file,std::ios::binary);if(!in)return false;
        uint8_t h[32]{};if(!in.read(reinterpret_cast<char*>(h),32))return false;
        if(std::string(reinterpret_cast<char*>(h),8)!="FSBIDX08" ||
           R64(h+8)!=logSize || R32(h+28)!=Crc(h,28))return false;
        uint64_t count=R64(h+16);
        if(count>((std::numeric_limits<uint64_t>::max)()-32ull)/16ull ||
           actual!=32+count*16)return false;
        uint32_t checksum=0xffffffffu;std::array<uint8_t,65536> buf{};uint64_t remain=count*16;
        while(remain){auto n=size_t((std::min)(remain,uint64_t(buf.size())));
            if(!in.read(reinterpret_cast<char*>(buf.data()),std::streamsize(n)))return false;
            for(size_t i=0;i<n;++i){checksum^=buf[i];for(int j=0;j<8;++j)
                checksum=(checksum>>1)^(0xedb88320u&(0u-(checksum&1u)));}
            remain-=n;
        }
        return ~checksum==R32(h+24);
    }
    bool Prepare(unsigned shard,State*& out){
        auto it=cache_.find(shard);
        std::error_code ec;
        uint64_t sz=0;
        const auto path=LogPath(shard);
        if(std::filesystem::exists(path,ec)) {sz=std::filesystem::file_size(path,ec);}
        if(ec)return false;
        if(it!=cache_.end()) {
            if(sz==it->second.logSize){it->second.tick=++tick_;out=&it->second;return true;}
            totalRecent_.fetch_sub(it->second.delta.size());cache_.erase(it);
        }
        const auto idx=IdxPath(shard);
        if(!ValidIndex(idx,sz) && !Build(shard,sz))return false;
        if(cache_.size()>=2){auto old=cache_.begin();
            for(auto i=cache_.begin();i!=cache_.end();++i)if(i->second.tick<old->second.tick)old=i;
            totalRecent_.fetch_sub(old->second.delta.size());cache_.erase(old);
        }
        State s{};s.indexedSize=sz;s.logSize=sz;s.tick=++tick_;
        out=&cache_.emplace(shard,std::move(s)).first->second;
        return true;
    }
    bool Rebuild(unsigned shard,State& state) {
        if(!Build(shard,state.logSize))return false;
        totalRecent_.fetch_sub(state.delta.size());state.delta.clear();
        state.indexedSize=state.logSize;return true;
    }
    bool Lookup(const std::wstring& key,std::string& out){
        out.clear();if(key.empty()||key.size()>4096)return false;
        const unsigned shard=Shard(key);
        State* s=nullptr;if(!Prepare(shard,s))return false;
        auto it=s->delta.find(key);
        if(it!=s->delta.end()){out=it->second;return true;}
        auto idx=IdxPath(shard);
        std::ifstream in(idx,std::ios::binary);
        if(!in)return false;
        uint8_t head[32]{};if(!in.read(reinterpret_cast<char*>(head),32))return false;
        uint64_t count=R64(head+16),wanted=Hash(key);
        auto get=[&](uint64_t k,Ref& r)->bool{
            uint8_t bytes[16]{};
            in.clear();in.seekg(std::streamoff(32+k*16));
            if(!in.read(reinterpret_cast<char*>(bytes),16))return false;
            r={R64(bytes),R64(bytes+8)};return true;
        };
        uint64_t l=0,r=count;
        while(l<r){uint64_t mid=l+(r-l)/2;Ref e{};
            if(!get(mid,e))return false;
            if(e.hash<wanted)l=mid+1;else r=mid;
        }
        std::ifstream log(LogPath(shard),std::ios::binary);
        while(l<count){Ref e{};if(!get(l++,e))return false;
            if(e.hash!=wanted)break;
            std::wstring entry;std::string id;uint64_t next=0;
            if(e.offset>=s->indexedSize ||
               !ReadRecord(log,e.offset,entry,id,next) || next>s->indexedSize)return false;
            if(entry==key){out=id;return true;}
        }
        return true;
    }
};
inline std::atomic<size_t> DiskBaselineLookup::totalRecent_{0};
} // namespace fsyn
