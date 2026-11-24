#pragma once
// FileSync V8 hardening: append-only, CRC-protected client change intent log.
// This is a recovery queue, NOT an NTFS USN-based exactly-once protocol.
// A change is removed only after FILE_COMMITTED for an event actually sent.
// Never store within the watched directory. A full inventory is still sent
// on every reconnect: no checkpoint-skipping assumption is made here.
#include "Protocol.h"
#include <filesystem>
#include <fstream>
#include <deque>
#include <vector>
#include <set>
#include <mutex>
#include <string>
#include <algorithm>
#include <cstdint>
#include <system_error>
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
class DurableEventLog {
public:
    struct Item { uint64_t seq=0; FileEventMsg event{}; bool sent=false; };
    static constexpr size_t kMaxPending=10000;
    static constexpr uint64_t kMaxLogBytes=128ull*1024*1024;
    explicit DurableEventLog(std::filesystem::path path):path_(std::move(path)) {}
    bool Open() {
        std::lock_guard<std::mutex> lock(mu_);
        std::error_code ec;
        std::filesystem::create_directories(path_.parent_path(),ec);
        if(ec) return false;
        if(!std::filesystem::exists(path_,ec)) {
            if(ec || !WriteInitial(path_)) return false;
        }
        if(ec) return false;
        std::ifstream in(path_,std::ios::binary); if(!in) return false;
        char header[8]{};
        if(!in.read(header,8) || std::string(header,8)!="FSYNQ008") return false;
        pending_.clear(); sent_.clear(); nextSeq_=1;
        // Fail CLOSED on incomplete or corrupted WAL, including a torn tail.
        while(in.peek()!=std::char_traits<char>::eof()) {
            uint8_t head[17]{};
            if(!in.read(reinterpret_cast<char*>(head),17)) return false;
            const char op=char(head[0]);const uint64_t seq=Read64(head+1);
            const uint32_t length=Read32(head+9),checksum=Read32(head+13);
            if((op!='E' && op!='A') || !seq || length>8192 || (op=='A' && length!=0)) return false;
            std::vector<uint8_t> body(length);
            if(length && !in.read(reinterpret_cast<char*>(body.data()),length)) return false;
            std::vector<uint8_t> check(head,head+13);
            check.insert(check.end(),body.begin(),body.end());
            if(Crc(check.data(),check.size())!=checksum) return false;
            nextSeq_=(std::max)(nextSeq_,seq+1);
            if(op=='E') {
                FileEventMsg ev;
                if(!Decode(body,ev)) return false;
                if(pending_.size()>=kMaxPending) return false;
                if(!pending_.empty() && seq<=pending_.back().seq) return false;
                pending_.push_back({seq,std::move(ev),false});
            } else {
                auto it=std::find_if(pending_.begin(),pending_.end(),
                    [seq](const Item& item){return item.seq==seq;});
                if(it==pending_.end()) return false;
                pending_.erase(it);
            }
        }
        opened_=true;
        return true;
    }
    bool Push(const FileEventMsg& ev) {
        std::lock_guard<std::mutex> lock(mu_);
        if(!opened_ || pending_.size()>=kMaxPending) return false;
        auto body=Encode(ev);
        if(body.empty()) return false;
        if(!Append('E',nextSeq_,body)) return false;
        pending_.push_back({nextSeq_++,ev,false});
        return true;
    }
    std::vector<Item> UnsentBatch(size_t maxCount) const {
        std::lock_guard<std::mutex> lock(mu_);
        std::vector<Item> out;
        out.reserve((std::min)(maxCount,pending_.size()));
        for(auto& e:pending_) {
            if(!e.sent) out.push_back(e);
            if(out.size()>=maxCount) break;
        }
        return out;
    }
    void MarkSent(uint64_t seq) {
        std::lock_guard<std::mutex> lock(mu_);
        for(auto& e:pending_) if(e.seq==seq) { e.sent=true;sent_.insert(seq);break; }
    }
    void ResetInflight() {
        std::lock_guard<std::mutex> lock(mu_);
        for(auto& e:pending_) e.sent=false;
        sent_.clear();
    }
    // Only acknowledge a change that was actually submitted on THIS connection.
    // FILE_COMMITTED carries no event sequence; therefore acknowledge the
    // oldest sent change per matching path. Unacked duplicates are safe to retry.
    bool AckSentPath(const std::string& path) {
        std::lock_guard<std::mutex> lock(mu_);
        if(!opened_) return false;
        for(auto it=pending_.begin();it!=pending_.end();) {
            const auto& ev=it->event;
            const std::string& target=(ev.action==FileAction::Renamed && !ev.newRelPath.empty())
                                        ?ev.newRelPath:ev.relPath;
            if(it->sent && target==path) {
                if(!Append('A',it->seq,{})) return false;
                sent_.erase(it->seq);it=pending_.erase(it);
            } else ++it;
        }
        return true; // a snapshot-origin commit does not ACK a different event
    }
    // Source vanished AFTER the complete inventory was emitted. Under V7's
    // no-remote-delete semantics, this event cannot transfer any bytes.
    bool AckMissing(uint64_t seq) {
        std::lock_guard<std::mutex> lock(mu_);
        if(!opened_) return false;
        for(auto it=pending_.begin();it!=pending_.end();++it) if(it->seq==seq) {
            if(!Append('A',seq,{})) return false;
            sent_.erase(seq);pending_.erase(it);return true;
        }
        return false;
    }
    bool CompactIfEmpty() {
        std::lock_guard<std::mutex> lock(mu_);
        if(!opened_ || !pending_.empty()) return false;
        // Only compact an empty queue. A failed publication leaves old WAL safe.
        const auto tmp=path_.wstring()+L".writing";
        if(!WriteInitial(tmp)) return false;
        std::error_code ec;
#ifdef _WIN32
        if(!MoveFileExW(tmp.c_str(),path_.c_str(),
             MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)) return false;
#else
        std::filesystem::rename(tmp,path_,ec);if(ec) return false;
#endif
        nextSeq_=1;
        return true;
    }
    size_t Size() const {std::lock_guard<std::mutex> lock(mu_);return pending_.size();}
private:
    static uint32_t Crc(const uint8_t* p,size_t n) {
        uint32_t v=0xffffffffu;
        for(size_t i=0;i<n;++i) {v^=p[i];for(int k=0;k<8;++k)v=(v>>1)^(0xedb88320u&(0u-(v&1u)));}
        return ~v;
    }
    static void Put16(std::vector<uint8_t>& a,uint16_t x){a.push_back(uint8_t(x));a.push_back(uint8_t(x>>8));}
    static void Put32(std::vector<uint8_t>& a,uint32_t x){for(int i=0;i<4;++i)a.push_back(uint8_t(x>>(i*8)));}
    static void Put64(std::vector<uint8_t>& a,uint64_t x){for(int i=0;i<8;++i)a.push_back(uint8_t(x>>(i*8)));}
    static uint16_t Read16(const uint8_t*p){return uint16_t(p[0])|(uint16_t(p[1])<<8);}
    static uint32_t Read32(const uint8_t*p){uint32_t x=0;for(int i=0;i<4;++i)x|=uint32_t(p[i])<<(i*8);return x;}
    static uint64_t Read64(const uint8_t*p){uint64_t x=0;for(int i=0;i<8;++i)x|=uint64_t(p[i])<<(i*8);return x;}
    static std::vector<uint8_t> Encode(const FileEventMsg& ev) {
        if(ev.relPath.empty()||ev.relPath.size()>1024||ev.newRelPath.size()>1024||ev.instanceId.size()>40) return {};
        std::vector<uint8_t> a;a.push_back(uint8_t(ev.action));
        Put16(a,uint16_t(ev.relPath.size()));Put16(a,uint16_t(ev.newRelPath.size()));
        Put16(a,uint16_t(ev.instanceId.size()));
        Put64(a,ev.size);Put64(a,uint64_t(ev.mtimeNs));
        a.insert(a.end(),ev.relPath.begin(),ev.relPath.end());
        a.insert(a.end(),ev.newRelPath.begin(),ev.newRelPath.end());
        a.insert(a.end(),ev.instanceId.begin(),ev.instanceId.end());
        return a;
    }
    static bool Decode(const std::vector<uint8_t>& a,FileEventMsg& ev) {
        if(a.size()<23) return false;
        if(a[0]<1||a[0]>4) return false;
        auto p=Read16(a.data()+1),n=Read16(a.data()+3),id=Read16(a.data()+5);
        if(!p||p>1024||n>1024||id>40||a.size()!=23ull+p+n+id) return false;
        ev.action=FileAction(a[0]);ev.size=Read64(a.data()+7);
        ev.mtimeNs=int64_t(Read64(a.data()+15));
        ev.relPath.assign(reinterpret_cast<const char*>(a.data()+23),p);
        ev.newRelPath.assign(reinterpret_cast<const char*>(a.data()+23+p),n);
        ev.instanceId.assign(reinterpret_cast<const char*>(a.data()+23+p+n),id);
        return true;
    }
    static bool WriteSynced(const std::filesystem::path& path,const std::vector<uint8_t>& bytes,bool append) {
#ifdef _WIN32
        HANDLE h=CreateFileW(path.c_str(),append?FILE_APPEND_DATA:GENERIC_WRITE,
            FILE_SHARE_READ,nullptr,append?OPEN_ALWAYS:CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(h==INVALID_HANDLE_VALUE) return false;
        DWORD n=0;bool ok=WriteFile(h,bytes.data(),DWORD(bytes.size()),&n,nullptr)!=FALSE &&
                      n==bytes.size() && FlushFileBuffers(h)!=FALSE;
        CloseHandle(h);return ok;
#else
        int fd=open(path.c_str(),O_WRONLY|O_CREAT|(append?O_APPEND:O_TRUNC),0600);
        if(fd<0) return false;
        size_t off=0;bool ok=true;
        while(off<bytes.size()){ssize_t n=write(fd,bytes.data()+off,bytes.size()-off);
            if(n<=0){ok=false;break;}off+=size_t(n);}
        if(ok) ok=fsync(fd)==0;
        close(fd);return ok;
#endif
    }
    static bool WriteInitial(const std::filesystem::path& p) {
        const char sig[]="FSYNQ008";
        return WriteSynced(p,std::vector<uint8_t>(sig,sig+8),false);
    }
    bool Append(char op,uint64_t seq,const std::vector<uint8_t>& body) {
        std::error_code ec;auto sz=std::filesystem::file_size(path_,ec);
        if(ec || sz>kMaxLogBytes || body.size()>8192 || sz+17+body.size()>kMaxLogBytes) return false;
        std::vector<uint8_t> a;
        a.push_back(uint8_t(op));Put64(a,seq);Put32(a,uint32_t(body.size()));
        std::vector<uint8_t> check=a;
        check.insert(check.end(),body.begin(),body.end());
        Put32(a,Crc(check.data(),check.size()));
        a.insert(a.end(),body.begin(),body.end());
        return WriteSynced(path_,a,true);
    }
    std::filesystem::path path_;
    mutable std::mutex mu_;
    std::deque<Item> pending_;
    std::set<uint64_t> sent_;
    uint64_t nextSeq_=1;
    bool opened_=false;
};
} // namespace fsyn
