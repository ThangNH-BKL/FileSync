#pragma once
// FileSync V8.5 EXPERIMENTAL, NO SQLITE.
// Shared ServerRoot LSM index: one durable append-only CRC WAL, bounded memtable,
// immutable sorted SST runs, tiered streaming compaction and crash replay.
// WAL is authoritative; SST files are rebuildable acceleration artifacts.
// Returns success from Reserve ONLY after group WAL fsync/FlushFileBuffers.
// This index does not modify prior baseline_v7; FileVersionStore calls the
// existing DiskBaselineLookup for compatibility. That adapter may build .sidx
// and migrate legacy baseline.log into baseline_v7 during first access.
// NOT YET VALIDATED for real Windows power loss, 100M files or multi-process use.
#include "DurableWalGroupCommit.h"
#include <algorithm>
#include <array>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <queue>
#include <set>
#include <string>
#include <system_error>
#include <vector>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#include <sys/file.h>
#endif
namespace fsyn {
class ServerLsmBaselineIndex {
public:
#ifdef FSYNC_LSM_TEST_MEMTABLE_LIMIT
    static constexpr size_t kMemtableLimit = FSYNC_LSM_TEST_MEMTABLE_LIMIT;
#else
    static constexpr size_t kMemtableLimit = 16384;
#endif
    explicit ServerLsmBaselineIndex(std::filesystem::path serverRoot)
      :root_(std::move(serverRoot)),dir_(root_/L".fsync_state"/L"baseline_lsm_v85"),
       wal_(dir_/L"baseline.wal"),manifest_(dir_/L"manifest.bin") {}
    ~ServerLsmBaselineIndex(){ ReleaseWriterLock(); }
    ServerLsmBaselineIndex(const ServerLsmBaselineIndex&)=delete;
    ServerLsmBaselineIndex& operator=(const ServerLsmBaselineIndex&)=delete;
    bool Find(const std::filesystem::path& clientState,const std::wstring& key,std::string& out){
        const auto k=FullKey(clientState,key);
        std::unique_lock<std::mutex> g(mu_);
        if(k.empty() || !Open() || unhealthy_)return false;
        cv_.wait(g,[&]{return unhealthy_ || !pending_.count(k);});
        return !unhealthy_ && Lookup(k,out);
    }
    bool Reserve(const std::filesystem::path& clientState,const std::wstring& key,const std::string& id){
        if(id.size()!=40)return false;
        for(char c:id)if(!((c>='0'&&c<='9')||(c>='a'&&c<='f')))return false;
        const auto k=FullKey(clientState,key);
        if(k.empty())return false;
        std::vector<uint8_t> record=Encode(k,id);
        if(record.empty())return false;
        {
            std::unique_lock<std::mutex> g(mu_);
            if(!Open()||unhealthy_)return false;
            cv_.wait(g,[&]{return unhealthy_ || (!checkpointing_ && !pending_.count(k));});
            if(unhealthy_)return false;
            std::string old;
            if(!Lookup(k,old))return false;
            if(!old.empty())return old==id;
            pending_.insert(k);
        }
        // WAL flush is outside the index lock, allowing concurrent sessions
        // to group records into ONE shared ServerRoot file flush.
        auto result=DurableWalGroupCommit::Global().Append(wal_,std::move(record));
        std::unique_lock<std::mutex> g(mu_);
        if(!result.ok){unhealthy_=true;pending_.erase(k);cv_.notify_all();return false;}
        // Append offsets are assigned by the WAL writer; each successful record
        // is durable even if the process dies before this in-RAM publish.
        mem_[k]={id,result.offset};
        const uint64_t end=result.offset+8+static_cast<uint64_t>(k.size())*2+40+4;
        liveEnd_=(std::max)(liveEnd_,end);
        pending_.erase(k);
        if(mem_.size()>=kMemtableLimit)checkpointing_=true;
        if(checkpointing_ && pending_.empty()){
            if(!Checkpoint())unhealthy_=true;
            checkpointing_=false;
        }
        cv_.notify_all();
        // Even if the derived index checkpoint failed, this reservation was
        // already durably WAL-committed, but further operations fail closed.
        return !unhealthy_;
    }
    bool CheckpointForTest(){
        std::lock_guard<std::mutex> g(mu_);
        return Open() && pending_.empty() && Checkpoint();
    }
    struct Stats {uint64_t walBytes=0, runs=0, memEntries=0, covered=0;};
    Stats GetStats(){
        std::lock_guard<std::mutex> g(mu_);
        if(!Open())return {};
        return {liveEnd_,runs_.size(),mem_.size(),covered_};
    }
private:
    struct MemEntry{std::string id;uint64_t offset=0;};
    struct Ref{uint64_t hash=0,offset=0;};
    struct Run{uint32_t level=0;uint64_t id=0,count=0,minHash=0,maxHash=0;uint32_t crc=0;};
    std::filesystem::path root_,dir_,wal_,manifest_;
    std::mutex mu_;
    std::condition_variable cv_;
    std::map<std::wstring,MemEntry> mem_;
    std::set<std::wstring> pending_;
    std::vector<Run> runs_;
    uint64_t covered_=8,liveEnd_=8,nextId_=1;
    bool opened_=false,unhealthy_=false,checkpointing_=false;
#ifdef _WIN32
    HANDLE lockFile_=INVALID_HANDLE_VALUE;
#else
    int lockFd_=-1;
#endif
    static uint32_t CrcUpdate(uint32_t crc,const uint8_t* p,size_t n){
        for(size_t i=0;i<n;++i){crc^=p[i];for(unsigned j=0;j<8;++j)crc=(crc>>1)^(0xedb88320u&(0u-(crc&1u)));}
        return crc;
    }
    static uint32_t Crc(const uint8_t* p,size_t n){return ~CrcUpdate(0xffffffffu,p,n);}
    static void U16(std::vector<uint8_t>& a,uint16_t n){a.push_back(uint8_t(n));a.push_back(uint8_t(n>>8));}
    static void U32(std::vector<uint8_t>& a,uint32_t n){for(unsigned j=0;j<4;++j)a.push_back(uint8_t(n>>(8*j)));}
    static void U64(std::vector<uint8_t>& a,uint64_t n){for(unsigned j=0;j<8;++j)a.push_back(uint8_t(n>>(8*j)));}
    static uint16_t R16(const uint8_t*p){return uint16_t(p[0])|(uint16_t(p[1])<<8);}
    static uint32_t R32(const uint8_t*p){uint32_t n=0;for(unsigned j=0;j<4;++j)n|=uint32_t(p[j])<<(8*j);return n;}
    static uint64_t R64(const uint8_t*p){uint64_t n=0;for(unsigned j=0;j<8;++j)n|=uint64_t(p[j])<<(8*j);return n;}
    static uint64_t Hash(const std::wstring& s){
        uint64_t h=14695981039346656037ull;
        for(wchar_t w:s){uint16_t u=static_cast<uint16_t>(w);
            h^=uint8_t(u);h*=1099511628211ull;
            h^=uint8_t(u>>8);h*=1099511628211ull;}
        return h;
    }
    static std::wstring FullKey(const std::filesystem::path& client,const std::wstring& key){
        if(key.empty()||key.size()>3000)return {};
        const auto exe=client.filename().wstring(),ip=client.parent_path().filename().wstring();
        if(exe.empty()||ip.empty()||exe==L"."||ip==L".")return {};
        const auto full=ip+L"/"+exe+L"/"+key;
        return full.size()<=4096?full:std::wstring{};
    }
    static std::vector<uint8_t> Encode(const std::wstring& key,const std::string& id){
        if(key.empty()||key.size()>4096||id.size()!=40)return {};
        std::vector<uint8_t> a;
        a.reserve(52+key.size()*2);
        U32(a,0x35385742u);U16(a,static_cast<uint16_t>(key.size()*2));U16(a,40);
        for(wchar_t w:key)U16(a,static_cast<uint16_t>(w));
        a.insert(a.end(),id.begin(),id.end());U32(a,Crc(a.data(),a.size()));
        return a;
    }
    static bool ReadWal(std::ifstream& in,uint64_t pos,std::wstring& key,std::string& id,uint64_t& end){
        in.clear();in.seekg(static_cast<std::streamoff>(pos));if(!in)return false;
        uint8_t h[8]{};if(!in.read(reinterpret_cast<char*>(h),8))return false;
        auto bytes=R16(h+4),len=R16(h+6);
        if(R32(h)!=0x35385742u||bytes==0||bytes>8192||bytes%2||len!=40)return false;
        std::vector<uint8_t> body(size_t(bytes)+44);
        if(!in.read(reinterpret_cast<char*>(body.data()),static_cast<std::streamsize>(body.size())))return false;
        std::vector<uint8_t> check(h,h+8);
        check.insert(check.end(),body.begin(),body.end()-4);
        if(Crc(check.data(),check.size())!=R32(body.data()+body.size()-4))return false;
        key.clear();key.reserve(bytes/2);
        for(size_t i=0;i<bytes;i+=2)key.push_back(static_cast<wchar_t>(R16(body.data()+i)));
        id.assign(reinterpret_cast<const char*>(body.data()+bytes),40);
        for(char c:id)if(!((c>='0'&&c<='9')||(c>='a'&&c<='f')))return false;
        end=pos+8+body.size();return true;
    }
    std::filesystem::path RunPath(const Run& r)const{
        wchar_t name[72]{};
#ifdef _WIN32
        swprintf_s(name,L"L%u_%020llu.sst",r.level,static_cast<unsigned long long>(r.id));
#else
        swprintf(name,72,L"L%u_%020llu.sst",r.level,static_cast<unsigned long long>(r.id));
#endif
        return dir_/L"runs"/name;
    }
    bool AcquireWriterLock(){
        const auto path=dir_/L"writer.lock";
#ifdef _WIN32
        lockFile_=CreateFileW(path.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,
                              OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
        return lockFile_!=INVALID_HANDLE_VALUE;
#else
        lockFd_=open(path.c_str(),O_RDWR|O_CREAT,0600);
        if(lockFd_<0)return false;
        if(flock(lockFd_,LOCK_EX|LOCK_NB)!=0){close(lockFd_);lockFd_=-1;return false;}
        return true;
#endif
    }
    void ReleaseWriterLock(){
#ifdef _WIN32
        if(lockFile_!=INVALID_HANDLE_VALUE){CloseHandle(lockFile_);lockFile_=INVALID_HANDLE_VALUE;}
#else
        if(lockFd_>=0){flock(lockFd_,LOCK_UN);close(lockFd_);lockFd_=-1;}
#endif
    }
    static bool SyncFile(const std::filesystem::path& path){
#ifdef _WIN32
        HANDLE h=CreateFileW(path.c_str(),GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ,
                             nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(h==INVALID_HANDLE_VALUE)return false;
        bool ok=FlushFileBuffers(h)!=FALSE;
        CloseHandle(h);return ok;
#else
        int fd=open(path.c_str(),O_RDONLY);if(fd<0)return false;
        bool ok=fsync(fd)==0;
        if(close(fd)!=0)ok=false;
        return ok;
#endif
    }
    static bool Publish(const std::filesystem::path& tmp,const std::filesystem::path& dest){
        if(!SyncFile(tmp))return false;
#ifdef _WIN32
        return MoveFileExW(tmp.c_str(),dest.c_str(),
                   MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=FALSE;
#else
        std::error_code ec;std::filesystem::rename(tmp,dest,ec);if(ec)return false;
        int fd=open(dest.parent_path().c_str(),O_RDONLY|O_DIRECTORY);
        if(fd<0)return false;
        bool ok=fsync(fd)==0;close(fd);return ok;
#endif
    }
    bool InitializeActiveMarker(){
        const auto marker=dir_/L"ACTIVE_V85";
        std::error_code ec;
        if(std::filesystem::exists(marker,ec))return !ec;
        if(ec)return false;
        std::ofstream out(marker,std::ios::binary|std::ios::trunc);
        if(!out)return false;
        out << "FSYNC-V85-LSM-WAL ACTIVE: do not run V7/V8 baseline mode without recovery.\n";
        out.flush();if(!out)return false;out.close();if(!out)return false;
        return SyncFile(marker);
    }
    bool InitializeWal(){
        std::error_code ec;
        if(std::filesystem::exists(wal_,ec))return !ec;
        if(ec)return false;
        std::ofstream out(wal_,std::ios::binary|std::ios::trunc);
        if(!out)return false;
        out.write("FSWAL085",8);out.flush();if(!out)return false;
        out.close();if(!out)return false;
        if(!SyncFile(wal_))return false;
#ifndef _WIN32
        int fd=open(dir_.c_str(),O_RDONLY|O_DIRECTORY);
        if(fd<0)return false;
        bool ok=fsync(fd)==0;close(fd);if(!ok)return false;
#endif
        return true;
    }
    static bool ParseHeader(std::ifstream& f,Run& r){
        uint8_t b[40]{};
        if(!f.read(reinterpret_cast<char*>(b),40))return false;
        if(std::string(reinterpret_cast<char*>(b),8)!="FSLM0851")return false;
        if(R32(b+36)!=Crc(b,36))return false;
        r.count=R64(b+8);r.minHash=R64(b+16);r.maxHash=R64(b+24);r.crc=R32(b+32);
        return r.count>0 && r.count<=(std::numeric_limits<uint64_t>::max)()/16 &&
               r.minHash<=r.maxHash;
    }
    bool ValidateRun(const Run& expected)const{
        const auto path=RunPath(expected);
        std::error_code ec;
        uint64_t size=std::filesystem::file_size(path,ec);
        if(ec || size!=40+expected.count*16)return false;
        std::ifstream in(path,std::ios::binary);if(!in)return false;
        Run actual;
        if(!ParseHeader(in,actual)||actual.count!=expected.count||
           actual.minHash!=expected.minHash||actual.maxHash!=expected.maxHash||
           actual.crc!=expected.crc)return false;
        uint32_t crc=0xffffffffu;
        std::array<uint8_t,65536> buf{};
        uint64_t remaining=expected.count*16;
        while(remaining){size_t n=static_cast<size_t>((std::min)(remaining,uint64_t(buf.size())));
            if(!in.read(reinterpret_cast<char*>(buf.data()),static_cast<std::streamsize>(n)))return false;
            crc=CrcUpdate(crc,buf.data(),n);remaining-=n;
        }
        return ~crc==expected.crc;
    }
    bool LoadManifest(){
        std::error_code ec;
        if(!std::filesystem::exists(manifest_,ec))return !ec;
        if(ec)return false;
        auto n=std::filesystem::file_size(manifest_,ec);
        if(ec||n<32||n>32ull+100000ull*40ull)return false;
        std::ifstream in(manifest_,std::ios::binary);if(!in)return false;
        std::vector<uint8_t> a(static_cast<size_t>(n));
        if(!in.read(reinterpret_cast<char*>(a.data()),static_cast<std::streamsize>(n)))return false;
        if(std::string(reinterpret_cast<const char*>(a.data()),8)!="FSML0851" ||
           R32(a.data()+n-4)!=Crc(a.data(),size_t(n-4)))return false;
        uint32_t count=R32(a.data()+24);
        if(n!=32ull+uint64_t(count)*40)return false;
        covered_=R64(a.data()+8);nextId_=R64(a.data()+16);
        if(covered_<8||nextId_<1)return false;
        runs_.clear();runs_.reserve(count);
        size_t pos=28;
        for(uint32_t i=0;i<count;++i){
            Run r{};
            r.level=R32(a.data()+pos);pos+=4;
            r.id=R64(a.data()+pos);pos+=8;
            r.count=R64(a.data()+pos);pos+=8;
            r.minHash=R64(a.data()+pos);pos+=8;
            r.maxHash=R64(a.data()+pos);pos+=8;
            r.crc=R32(a.data()+pos);pos+=4;
            if(r.level>16||r.id>=nextId_||!ValidateRun(r))return false;
            runs_.push_back(r);
        }
        return true;
    }
    bool SaveManifest(){
        if(runs_.size()>100000)return false;
        std::vector<uint8_t> a;
        a.insert(a.end(),{'F','S','M','L','0','8','5','1'});
        U64(a,covered_);U64(a,nextId_);U32(a,static_cast<uint32_t>(runs_.size()));
        for(const auto& r:runs_){
            U32(a,r.level);U64(a,r.id);U64(a,r.count);
            U64(a,r.minHash);U64(a,r.maxHash);U32(a,r.crc);
        }
        U32(a,Crc(a.data(),a.size()));
        auto tmp=manifest_;tmp+=L".writing";
        {std::ofstream out(tmp,std::ios::binary|std::ios::trunc);
            if(!out)return false;
            out.write(reinterpret_cast<const char*>(a.data()),static_cast<std::streamsize>(a.size()));
            out.flush();if(!out)return false;
            out.close();if(!out)return false;
        }
        return Publish(tmp,manifest_);
    }
    // Write sorted references. The actual keys and values stay in the WAL.
    template<class Next>
    bool WriteRun(uint32_t level,Next next,Run& result){
        result=Run{};result.level=level;result.id=nextId_++;
        auto path=RunPath(result);auto tmp=path;tmp+=L".writing";
        std::error_code ec;std::filesystem::create_directories(path.parent_path(),ec);if(ec)return false;
        uint32_t crc=0xffffffffu;
        std::ofstream out(tmp,std::ios::binary|std::ios::trunc);
        if(!out)return false;
        std::array<char,40> blank{};out.write(blank.data(),blank.size());
        Ref ref{};bool first=true;uint64_t previous=0;
        while(next(ref)){
            if(!first && (ref.hash<previous))return false;
            uint8_t b[16]{};
            for(int i=0;i<8;++i){b[i]=uint8_t(ref.hash>>(8*i));b[i+8]=uint8_t(ref.offset>>(8*i));}
            out.write(reinterpret_cast<const char*>(b),16);if(!out)return false;
            crc=CrcUpdate(crc,b,16);
            if(first){result.minHash=ref.hash;first=false;}
            previous=ref.hash;result.maxHash=ref.hash;++result.count;
        }
        if(first)return false;
        result.crc=~crc;
        std::vector<uint8_t> h;h.insert(h.end(),{'F','S','L','M','0','8','5','1'});
        U64(h,result.count);U64(h,result.minHash);U64(h,result.maxHash);
        U32(h,result.crc);U32(h,Crc(h.data(),h.size()));
        if(h.size()!=40)return false;
        out.seekp(0);out.write(reinterpret_cast<const char*>(h.data()),40);
        out.flush();if(!out)return false;
        out.close();if(!out)return false;
        return Publish(tmp,path);
    }
    bool Checkpoint(){
        if(mem_.empty())return true;
        if(!pending_.empty())return false;
        std::vector<Ref> refs;refs.reserve(mem_.size());
        for(const auto& e:mem_)refs.push_back({Hash(e.first),e.second.offset});
        std::sort(refs.begin(),refs.end(),[](const Ref&a,const Ref&b){
            return a.hash<b.hash || (a.hash==b.hash && a.offset<b.offset);
        });
        size_t p=0;Run run;
        if(!WriteRun(0,[&](Ref& r){if(p==refs.size())return false;r=refs[p++];return true;},run))return false;
        runs_.push_back(run);
        covered_=liveEnd_;
        if(!SaveManifest())return false;
        mem_.clear();
        return Compact();
    }
    bool Compact(){
        // Tiered LSM: merge 8 same-level SSTs using O(8) merge memory.
        // The WAL remains authoritative; older SSTs are deliberately retained
        // until offline GC/crash validation rather than deleted eagerly.
        for(uint32_t level=0;level<16;++level){
            std::vector<Run> chosen;
            for(const auto& r:runs_)if(r.level==level && chosen.size()<8)chosen.push_back(r);
            if(chosen.size()<8)continue;
            struct Cursor{std::ifstream file;uint64_t left=0;uint32_t crc=0xffffffffu;
                          uint32_t expected=0;};
            std::vector<std::unique_ptr<Cursor>> cursors;
            struct Node{Ref r;size_t source;};
            auto greater=[](const Node&a,const Node&b){
                return a.r.hash>b.r.hash || (a.r.hash==b.r.hash && a.r.offset>b.r.offset);
            };
            std::priority_queue<Node,std::vector<Node>,decltype(greater)> heap(greater);
            auto advance=[&](size_t i)->bool{
                auto& c=*cursors[i];if(!c.left)return true;
                uint8_t b[16]{};if(!c.file.read(reinterpret_cast<char*>(b),16))return false;
                c.crc=CrcUpdate(c.crc,b,16);
                heap.push({{R64(b),R64(b+8)},i});--c.left;
                return c.left!=0 || ~c.crc==c.expected;
            };
            for(const auto& r:chosen){
                auto c=std::make_unique<Cursor>();c->file.open(RunPath(r),std::ios::binary);
                if(!c->file)return false;
                c->file.seekg(40);c->left=r.count;c->expected=r.crc;
                cursors.push_back(std::move(c));
                if(!advance(cursors.size()-1))return false;
            }
            bool readOk=true;Run merged;
            if(!WriteRun(level+1,[&](Ref& r){
                if(heap.empty()||!readOk)return false;
                auto node=heap.top();heap.pop();r=node.r;
                if(!advance(node.source))readOk=false;
                return true;
            },merged)||!readOk)return false;
            std::set<uint64_t> eraseIds;
            for(const auto& r:chosen)eraseIds.insert(r.id);
            auto next=runs_;
            next.erase(std::remove_if(next.begin(),next.end(),[&](const Run& r){
                return eraseIds.count(r.id)>0;
            }),next.end());
            next.push_back(merged);
            auto old=std::move(runs_);runs_=std::move(next);
            if(!SaveManifest()){runs_=std::move(old);return false;}
            // Keep old *.sst as safely disposable orphan artifacts.
            level=static_cast<uint32_t>(-1); // restart from L0 after a merge
        }
        return true;
    }
    bool Lookup(const std::wstring& key,std::string& out){
        out.clear();
        auto m=mem_.find(key);
        if(m!=mem_.end()){out=m->second.id;return true;}
        uint64_t hash=Hash(key);
        std::ifstream wal(wal_,std::ios::binary);if(!wal)return false;
        // Newest run first. Keys are immutable; any duplicate must have same id.
        for(auto i=runs_.rbegin();i!=runs_.rend();++i){
            if(hash<i->minHash||hash>i->maxHash)continue;
            std::ifstream idx(RunPath(*i),std::ios::binary);if(!idx)return false;
            auto get=[&](uint64_t n,Ref& r)->bool{
                idx.clear();idx.seekg(static_cast<std::streamoff>(40+n*16));if(!idx)return false;
                uint8_t b[16]{};if(!idx.read(reinterpret_cast<char*>(b),16))return false;
                r={R64(b),R64(b+8)};return true;
            };
            uint64_t l=0,r=i->count;
            while(l<r){uint64_t mid=l+(r-l)/2;Ref e{};
                if(!get(mid,e))return false;
                if(e.hash<hash)l=mid+1;else r=mid;
            }
            while(l<i->count){Ref e{};if(!get(l++,e))return false;
                if(e.hash!=hash)break;
                std::wstring found;std::string id;uint64_t end=0;
                if(e.offset<8||e.offset>=liveEnd_||!ReadWal(wal,e.offset,found,id,end)||
                   end>liveEnd_)return false;
                if(found==key){out=id;return true;}
            }
        }
        return true;
    }
    bool Open(){
        if(opened_)return !unhealthy_;
        std::error_code ec;std::filesystem::create_directories(dir_,ec);
        if(ec||!AcquireWriterLock())return false;
        if(!InitializeWal()||!LoadManifest()||!InitializeActiveMarker()){
            unhealthy_=true;return false;
        }
        const auto size=std::filesystem::file_size(wal_,ec);
        if(ec||size<8||covered_>size){unhealthy_=true;return false;}
        std::ifstream in(wal_,std::ios::binary);char sig[8]{};
        if(!in.read(sig,8)||std::string(sig,8)!="FSWAL085"){
            unhealthy_=true;return false;
        }
        liveEnd_=covered_;
        // CRC-verified replay of all records since last durable manifest.
        uint64_t p=covered_;
        while(p<size){
            std::wstring key;std::string id;uint64_t next=0;
            if(!ReadWal(in,p,key,id,next)||next>size){unhealthy_=true;return false;}
            std::string old;
            if(!Lookup(key,old)||(!old.empty()&&old!=id)){
                unhealthy_=true;return false;
            }
            if(old.empty())mem_[key]={id,p};
            p=next;
            liveEnd_=p;
            if(mem_.size()>=kMemtableLimit){
                if(!Checkpoint()){unhealthy_=true;return false;}
            }
        }
        liveEnd_=size;
        opened_=true;return true;
    }
};
} // namespace fsyn
