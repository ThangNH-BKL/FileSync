#pragma once
// FileSync V7 experimental on-disk baseline index, no SQLite.
// Version identities are immutable. Sharded log records have CRC32 and are
// synchronously flushed before a new first-generation reservation succeeds.
// Legacy baseline.log is retained read-only; interrupted migration can replay.
// This file is platform-neutral except for the durable append implementation.
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <list>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>
#include <algorithm>
#include "DurabilityTelemetry.h"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace fsyn {
class ShardedBaselineIndex {
public:
    static constexpr unsigned kShards = 1024;
    static constexpr size_t kMaxResidentBuckets = 8;
    // Client directory, e.g. ...\.fsync_state\192.168.1.1\abc
    explicit ShardedBaselineIndex(std::filesystem::path clientState)
        : base_(std::move(clientState)) {}
    bool Find(const std::wstring& key, std::string& out) {
        std::lock_guard<std::mutex> lock(mu_);
        if (!EnsureMigrated()) return false;
        Bucket* bucket = nullptr;
        if (!LoadBucket(Shard(key),bucket)) return false;
        auto it = bucket->records.find(key);
        out = it == bucket->records.end() ? std::string() : it->second;
        return true;
    }
    bool Reserve(const std::wstring& key, const std::string& identity) {
        if (key.empty() || identity.size() != 40) return false;
        for (char c: identity) if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
        std::lock_guard<std::mutex> lock(mu_);
        if (!EnsureMigrated()) return false;
        return InsertInternal(key, identity);
    }
    std::filesystem::path Root() const { return base_ / L"baseline_v7"; }
private:
    struct Bucket {
        std::unordered_map<std::wstring,std::string> records;
        uint64_t tick = 0;
    };
    std::filesystem::path base_;
    std::map<unsigned,Bucket> cache_;
    std::mutex mu_;
    uint64_t tick_ = 0;
    bool checkedMigration_ = false;
    static uint32_t Crc(const uint8_t* data, size_t count) {
        uint32_t crc = 0xffffffffu;
        for (size_t i=0; i<count; ++i) {
            crc ^= data[i];
            for (unsigned j=0;j<8;++j) crc=(crc>>1) ^ (0xedb88320u & (0u-(crc&1u)));
        }
        return ~crc;
    }
    static void U16(std::vector<uint8_t>& a,uint16_t v) { a.push_back(uint8_t(v));a.push_back(uint8_t(v>>8)); }
    static void U32(std::vector<uint8_t>& a,uint32_t v) { for(unsigned i=0;i<4;++i) a.push_back(uint8_t(v>>(8*i))); }
    static uint16_t R16(const uint8_t* p) { return uint16_t(p[0]) | uint16_t(p[1])<<8; }
    static uint32_t R32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1])<<8 | uint32_t(p[2])<<16 | uint32_t(p[3])<<24; }
    static std::vector<uint8_t> KeyBytes(const std::wstring& key) {
        std::vector<uint8_t> out;
        out.reserve(key.size()*2);
        for (wchar_t c:key) U16(out,static_cast<uint16_t>(c)); // UTF-16 on Windows
        return out;
    }
    static unsigned Shard(const std::wstring& key) {
        uint64_t h = 14695981039346656037ull;
        for (uint8_t b : KeyBytes(key)) { h ^= b; h *= 1099511628211ull; }
        return static_cast<unsigned>(h % kShards);
    }
    std::filesystem::path BucketPath(unsigned shard) const {
        wchar_t name[32]{};
#ifdef _WIN32
        swprintf_s(name, L"shard_%04u.bin", shard);
#else
        swprintf(name,32,L"shard_%04u.bin",shard);
#endif
        return Root() / name;
    }
    static std::vector<uint8_t> Encode(const std::wstring& key,const std::string& id) {
        auto kb=KeyBytes(key);
        if (kb.size()>8192 || kb.empty() || id.size()!=40) return {};
        std::vector<uint8_t> record;
        U32(record,0x31425346u); // FSB1
        U16(record,static_cast<uint16_t>(kb.size()));
        U16(record,40);
        record.insert(record.end(),kb.begin(),kb.end());
        record.insert(record.end(),id.begin(),id.end());
        U32(record,Crc(record.data(),record.size()));
        return record;
    }
    static bool AppendSynced(const std::filesystem::path& path, const std::vector<uint8_t>& payload) {
        if (payload.empty()) return false;
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(),ec);
        if (ec) return false;
#ifdef _WIN32
        HANDLE h=CreateFileW(path.c_str(),FILE_APPEND_DATA,FILE_SHARE_READ,nullptr,OPEN_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL,nullptr);
        if(h==INVALID_HANDLE_VALUE) return false;
        DWORD n=0;
        bool good=WriteFile(h,payload.data(),static_cast<DWORD>(payload.size()),&n,nullptr)!=FALSE &&
                  n==payload.size();
        if(good) {
            const auto started=durability::Clock::now();
            good=FlushFileBuffers(h)!=FALSE;
            durability::RecordBaseline(durability::Us(started));
        }
        CloseHandle(h);
        return good;
#else
        int fd=open(path.c_str(),O_WRONLY|O_APPEND|O_CREAT,0600);
        if(fd<0) return false;
        size_t off=0;
        while(off<payload.size()) {
            ssize_t n=write(fd,payload.data()+off,payload.size()-off);
            if(n<=0) {close(fd);return false;}
            off+=static_cast<size_t>(n);
        }
        bool good=fsync(fd)==0;
        close(fd);
        return good;
#endif
    }
    static bool PublishMarker(const std::filesystem::path& marker,const std::string& contents) {
        std::error_code ec;
        std::filesystem::create_directories(marker.parent_path(),ec);
        if(ec) return false;
        const auto temporary=marker.parent_path()/L"migration.writing";
#ifdef _WIN32
        HANDLE h=CreateFileW(temporary.c_str(),GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL,nullptr);
        if(h==INVALID_HANDLE_VALUE) return false;
        DWORD count=0;
        bool good=WriteFile(h,contents.data(),static_cast<DWORD>(contents.size()),&count,nullptr)!=FALSE &&
                  count==contents.size() && FlushFileBuffers(h)!=FALSE;
        CloseHandle(h);
        if(!good) return false;
        return MoveFileExW(temporary.c_str(),marker.c_str(),MOVEFILE_WRITE_THROUGH)!=FALSE;
#else
        int fd=open(temporary.c_str(),O_CREAT|O_TRUNC|O_WRONLY,0600);
        if(fd<0) return false;
        size_t off=0;
        while(off<contents.size()) {
            ssize_t n=write(fd,contents.data()+off,contents.size()-off);
            if(n<=0) {close(fd);return false;}off+=static_cast<size_t>(n);
        }
        bool good=fsync(fd)==0;
        close(fd);
        if(!good || std::filesystem::exists(marker,ec) || ec) return false;
        std::filesystem::rename(temporary,marker,ec);
        return !ec;
#endif
    }
    static bool ReadExact(std::ifstream& in, uint8_t* p,size_t n) {
        if (n>static_cast<size_t>((std::numeric_limits<std::streamsize>::max)())) return false;
        return bool(in.read(reinterpret_cast<char*>(p),static_cast<std::streamsize>(n)));
    }
    bool LoadBucket(unsigned shard, Bucket*& out) {
        auto cached=cache_.find(shard);
        if(cached!=cache_.end()) { cached->second.tick=++tick_;out=&cached->second;return true; }
        Bucket bucket;
        const auto file=BucketPath(shard);
        std::error_code ec;
        const bool exists=std::filesystem::exists(file,ec);
        if(ec) return false;
        if(exists) {
            std::ifstream in(file,std::ios::binary);
            if(!in) return false;
            for(;;) {
                std::array<uint8_t,8> h{};
                in.read(reinterpret_cast<char*>(h.data()),h.size());
                if(in.gcount()==0 && in.eof()) break;
                if(!in || R32(h.data())!=0x31425346u) return false;
                uint16_t len=R16(h.data()+4),vlen=R16(h.data()+6);
                if(len==0 || len>8192 || len%2 || vlen!=40) return false;
                std::vector<uint8_t> body(static_cast<size_t>(len)+vlen+4u);
                if(!ReadExact(in,body.data(),body.size())) return false;
                std::vector<uint8_t> crcBytes(h.begin(),h.end());
                crcBytes.insert(crcBytes.end(),body.begin(),body.end()-4);
                if(Crc(crcBytes.data(),crcBytes.size())!=R32(body.data()+body.size()-4)) return false;
                std::wstring key;
                for(size_t i=0;i<len;i+=2) key.push_back(static_cast<wchar_t>(R16(body.data()+i)));
                std::string id(reinterpret_cast<const char*>(body.data()+len),vlen);
                for(char c:id) if(!((c>='0'&&c<='9')||(c>='a'&&c<='f'))) return false;
                if(Shard(key)!=shard) return false;
                auto old=bucket.records.find(key);
                if(old!=bucket.records.end() && old->second!=id) return false;
                bucket.records.emplace(std::move(key),std::move(id));
            }
        }
        bucket.tick=++tick_;
        // Do not store unbounded per-client cache; only a fixed number of shards.
        if(cache_.size()>=kMaxResidentBuckets) {
            auto oldest=cache_.begin();
            for(auto i=cache_.begin();i!=cache_.end();++i)
                if(i->second.tick<oldest->second.tick) oldest=i;
            cache_.erase(oldest);
        }
        auto inserted=cache_.emplace(shard,std::move(bucket));
        out=&inserted.first->second;
        return true;
    }
    bool InsertInternal(const std::wstring& key,const std::string& id) {
        Bucket* bucket=nullptr;
        if(!LoadBucket(Shard(key),bucket)) return false;
        auto old=bucket->records.find(key);
        if(old!=bucket->records.end()) return old->second==id;
        auto bytes=Encode(key,id);
        if(!AppendSynced(BucketPath(Shard(key)),bytes)) {
            cache_.erase(Shard(key)); // Never trust cache if append had partial success.
            return false;
        }
        bucket->records.emplace(key,id);
        return true;
    }
    static bool DecodeLegacyKey(const std::string& hex,std::wstring& out) {
        if(hex.empty() || hex.size()%4 || hex.size()>32768) return false;
        out.clear();
        for(size_t i=0;i<hex.size();i+=4) {
            unsigned v=0;
            for(size_t j=0;j<4;++j) {
                char c=hex[i+j];unsigned nib=(c>='0'&&c<='9')?unsigned(c-'0'):
                    (c>='a'&&c<='f')?unsigned(c-'a'+10):16u;
                if(nib>15) return false;
                v=(v<<4)|nib;
            }
            out.push_back(static_cast<wchar_t>(v));
        }
        return true;
    }
    bool EnsureMigrated() {
        if(checkedMigration_) return true;
        const auto legacy=base_/L"baseline.log";
        const auto marker=Root()/L"migration.complete";
        std::error_code ec;
        bool legacyExists=std::filesystem::exists(legacy,ec);
        if(ec) return false;
        if(!legacyExists) { checkedMigration_=true;return true; }
        const auto size=std::filesystem::file_size(legacy,ec);
        if(ec) return false;
        const auto timestamp=std::filesystem::last_write_time(legacy,ec);
        if(ec) return false;
        const std::string fingerprint=std::to_string(size)+":"+
            std::to_string(timestamp.time_since_epoch().count())+"\n";
        if(std::filesystem::exists(marker,ec)) {
            if(ec) return false;
            std::ifstream in(marker,std::ios::binary);
            std::string line;
            if(!std::getline(in,line) || line+"\n"!=fingerprint) return false;
            checkedMigration_=true;
            return true;
        }
        if(ec) return false;
        std::ifstream in(legacy,std::ios::binary);
        if(!in) return false;
        std::string line;
        if(!std::getline(in,line) || in.eof() || line!="FSYN-BASELINE-1") return false;
        while(std::getline(in,line)) {
            // An incomplete last line was never durably acknowledged by V6.
            if(in.eof()) return false;
            size_t split=line.find(' ');
            if(split==std::string::npos || line.find(' ',split+1)!=std::string::npos) return false;
            std::wstring key;
            const std::string id=line.substr(split+1);
            if(!DecodeLegacyKey(line.substr(0,split),key) || id.size()!=40 ||
               !InsertInternal(key,id)) return false;
        }
        if(in.bad() || (size>0 && !in.eof())) return false;
        // Validate source unchanged during migration; otherwise an older tool may
        // still be writing to it. Fail closed and let administrator investigate.
        if(std::filesystem::file_size(legacy,ec)!=size || ec ||
           std::filesystem::last_write_time(legacy,ec)!=timestamp || ec) return false;
        if(!PublishMarker(marker,fingerprint)) return false;
        checkedMigration_=true;
        return true;
    }
};
} // namespace fsyn
