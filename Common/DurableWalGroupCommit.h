#pragma once
// V8.5 EXPERIMENTAL: bounded cross-session group flush for V7 CRC baseline journals.
// A successful Append() means its bytes were included in a successful OS flush.
// The V7 shard log is the authoritative append-only WAL; *.sst are disposable indexes.
// Do not concurrently run another FileSync process on the same ServerRoot.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <vector>
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
class DurableWalGroupCommit {
public:
    struct Result { bool ok = false; uint64_t offset = 0; };
    struct Counters { uint64_t records=0, flushes=0, totalFlushUs=0, failedBatches=0; };
    static DurableWalGroupCommit& Global() { static DurableWalGroupCommit s; return s; }
    DurableWalGroupCommit(const DurableWalGroupCommit&) = delete;
    DurableWalGroupCommit& operator=(const DurableWalGroupCommit&) = delete;
    Result Append(const std::filesystem::path& path, std::vector<uint8_t> payload) {
        if(payload.empty() || payload.size()>16384) return {};
        auto task=std::make_shared<Request>();
        task->path=path; task->bytes=std::move(payload);
        {
            std::unique_lock<std::mutex> lk(mu_);
            spaceCv_.wait(lk,[&]{return stopped_ || queue_.size()<4096;});
            if(stopped_ || badPaths_.count(path)) return {};
            queue_.push_back(task);
        }
        queueCv_.notify_one();
        std::unique_lock<std::mutex> lk(task->mu);
        task->cv.wait(lk,[&]{return task->done;});
        return {task->good,task->offset};
    }
    Counters Stats() const {
        return {records_.load(),flushes_.load(),flushUs_.load(),failed_.load()};
    }
private:
    struct Request {
        std::filesystem::path path;
        std::vector<uint8_t> bytes;
        std::mutex mu;
        std::condition_variable cv;
        bool done=false,good=false;
        uint64_t offset=0;
    };
    DurableWalGroupCommit():worker_([this]{Run();}){}
    ~DurableWalGroupCommit(){
        {std::lock_guard<std::mutex> lk(mu_);stopped_=true;}
        queueCv_.notify_all(); spaceCv_.notify_all();
        if(worker_.joinable())worker_.join();
    }
    static void Finish(const std::shared_ptr<Request>& req,bool ok) {
        {std::lock_guard<std::mutex> lk(req->mu);req->good=ok;req->done=true;}
        req->cv.notify_one();
    }
    static bool FlushFile(const std::filesystem::path& path,
                          const std::vector<std::shared_ptr<Request>>& jobs,
                          uint64_t& us) {
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(),ec);
        if(ec) return false;
#ifdef _WIN32
        HANDLE h=CreateFileW(path.c_str(),FILE_APPEND_DATA|FILE_READ_ATTRIBUTES,
                             FILE_SHARE_READ,nullptr,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(h==INVALID_HANDLE_VALUE)return false;
        LARGE_INTEGER size{};
        bool ok=GetFileSizeEx(h,&size)!=FALSE && size.QuadPart>=0;
        uint64_t position=ok?static_cast<uint64_t>(size.QuadPart):0;
        for(const auto& job:jobs) {
            if(!ok)break;
            job->offset=position;
            DWORD written=0;
            ok=WriteFile(h,job->bytes.data(),static_cast<DWORD>(job->bytes.size()),&written,nullptr)!=FALSE
                && written==job->bytes.size();
            position+=job->bytes.size();
        }
        if(ok) {
            const auto begin=std::chrono::steady_clock::now();
            ok=FlushFileBuffers(h)!=FALSE;
            us=static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now()-begin).count());
        }
        CloseHandle(h);
        return ok;
#else
        int fd=open(path.c_str(),O_WRONLY|O_APPEND|O_CREAT,0600);
        if(fd<0)return false;
        off_t end=lseek(fd,0,SEEK_END);
        bool ok=end>=0;
        uint64_t position=ok?static_cast<uint64_t>(end):0;
        for(const auto& job:jobs){
            if(!ok)break;
            job->offset=position;
            size_t off=0;
            while(off<job->bytes.size()){
                ssize_t n=write(fd,job->bytes.data()+off,job->bytes.size()-off);
                if(n<=0){ok=false;break;}
                off+=static_cast<size_t>(n);
            }
            position+=job->bytes.size();
        }
        if(ok){
            const auto begin=std::chrono::steady_clock::now();
            ok=fsync(fd)==0;
            us=static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now()-begin).count());
        }
        if(close(fd)!=0)ok=false;
        return ok;
#endif
    }
    void Run(){
        for(;;){
            std::vector<std::shared_ptr<Request>> batch;
            {
                std::unique_lock<std::mutex> lk(mu_);
                queueCv_.wait(lk,[&]{return stopped_||!queue_.empty();});
                if(stopped_ && queue_.empty())break;
                // Single-client latency matters: wait at most 1 ms for peers.
                // With >=16 requests already queued, flush immediately.
                if(!stopped_ && queue_.size()<16)
                    queueCv_.wait_for(lk,std::chrono::milliseconds(1),[&]{
                        return stopped_ || queue_.size()>=16;
                    });
                const size_t n=(std::min)(size_t(256),queue_.size());
                for(size_t i=0;i<n;++i){batch.push_back(queue_.front());queue_.pop_front();}
            }
            spaceCv_.notify_all();
            std::map<std::filesystem::path,std::vector<std::shared_ptr<Request>>> grouped;
            for(const auto& task:batch)grouped[task->path].push_back(task);
            for(auto& item:grouped){
                bool poisoned=false;
                {std::lock_guard<std::mutex> lk(mu_);poisoned=badPaths_.count(item.first)!=0;}
                uint64_t us=0;
                bool good=!poisoned && FlushFile(item.first,item.second,us);
                if(!good){
                    ++failed_;
                    // Any uncertain partial write poisons this WAL until restart/recovery.
                    std::lock_guard<std::mutex> lk(mu_);
                    badPaths_.insert(item.first);
                }else{
                    records_.fetch_add(item.second.size());++flushes_;flushUs_.fetch_add(us);
                }
                for(const auto& req:item.second)Finish(req,good);
            }
        }
    }
    mutable std::mutex mu_;
    std::condition_variable queueCv_,spaceCv_;
    std::deque<std::shared_ptr<Request>> queue_;
    std::set<std::filesystem::path> badPaths_;
    bool stopped_=false;
    std::thread worker_;
    std::atomic<uint64_t> records_{0},flushes_{0},flushUs_{0},failed_{0};
};
} // namespace fsyn
