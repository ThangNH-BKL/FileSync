#pragma once
// Temporary, disk-backed inventory spool for one session. A reconnect always
// re-enumerates source; this is NOT a durable server work queue. It keeps RAM
// independent of TREE_SNAPSHOT length and validates every persisted record.
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include <system_error>

namespace fsyn {
class DiskSnapshotSpool {
public:
    struct Entry {std::string path, instance; uint64_t size=0; int64_t mtime=0;};
    bool Open(const std::filesystem::path& filename) {
        Close(true);
        std::error_code ec;
        std::filesystem::create_directories(filename.parent_path(),ec);
        if(ec || std::filesystem::exists(filename,ec) || ec) return false;
        path_=filename;
        writer_.open(path_,std::ios::binary|std::ios::trunc);
        return bool(writer_);
    }
    bool Append(const Entry& e) {
        if(!writer_ || e.path.empty() || e.path.size()>1024 || e.instance.size()!=40) return false;
        // Reuse the same scratch capacity across millions of entries.
        // Record bytes, CRC, file format, and failure semantics are unchanged.
        writeScratch_.clear();
        auto& record = writeScratch_;
        Write16(record,static_cast<uint16_t>(e.path.size()));
        record.insert(record.end(),e.path.begin(),e.path.end());
        record.insert(record.end(),e.instance.begin(),e.instance.end());
        Write64(record,e.size);Write64(record,static_cast<uint64_t>(e.mtime));
        Write32(record,Crc(record.data(),record.size()));
        writer_.write(reinterpret_cast<const char*>(record.data()),record.size());
        return bool(writer_);
    }
    bool Finish() {
        if(!writer_) return false;
        writer_.flush();
        if(!writer_) return false;
        writer_.close();
        reader_.open(path_,std::ios::binary);
        return bool(reader_);
    }
    // end==true only when all entries were read; errors return false (fail closed).
    bool ReadBatch(size_t limit, std::vector<Entry>& results, bool& end) {
        results.clear();end=false;
        if(!reader_ || !limit || limit>4096) return false;
        for(size_t i=0;i<limit;++i) {
            char probe=0;
            if(!reader_.get(probe)) {
                if(reader_.eof()) {reader_.clear();end=true;return true;}
                return false;
            }
            char probe2=0;
            if(!reader_.get(probe2)) return false;
            uint16_t len=uint8_t(probe)|(uint16_t(uint8_t(probe2))<<8);
            if(!len || len>1024) return false;
            readScratch_.resize(2+size_t(len)+40+8+8+4);
            auto& data=readScratch_;
            data[0]=uint8_t(probe);data[1]=uint8_t(probe2);
            if(!reader_.read(reinterpret_cast<char*>(data.data()+2),data.size()-2)) return false;
            if(Crc(data.data(),data.size()-4)!=Read32(data.data()+data.size()-4)) return false;
            Entry e;
            e.path.assign(reinterpret_cast<const char*>(data.data()+2),len);
            e.instance.assign(reinterpret_cast<const char*>(data.data()+2+len),40);
            for(char c:e.instance)
                if(!((c>='0'&&c<='9')||(c>='a'&&c<='f'))) return false;
            e.size=Read64(data.data()+2+len+40);
            e.mtime=static_cast<int64_t>(Read64(data.data()+2+len+48));
            results.push_back(std::move(e));
        }
        end=reader_.peek()==std::char_traits<char>::eof();
        reader_.clear();
        return true;
    }
    void Close(bool removeFile) {
        if(writer_.is_open()) writer_.close();
        if(reader_.is_open()) reader_.close();
        if(removeFile && !path_.empty()) {std::error_code ec;std::filesystem::remove(path_,ec);}
        path_.clear();
    }
    ~DiskSnapshotSpool() {Close(true);}
    DiskSnapshotSpool()=default;
    DiskSnapshotSpool(const DiskSnapshotSpool&)=delete;
    DiskSnapshotSpool& operator=(const DiskSnapshotSpool&)=delete;
private:
    std::filesystem::path path_;
    std::ofstream writer_;
    std::ifstream reader_;
    std::vector<uint8_t> writeScratch_, readScratch_;
    static void Write16(std::vector<uint8_t>& x,uint16_t v){x.push_back(uint8_t(v));x.push_back(uint8_t(v>>8));}
    static void Write32(std::vector<uint8_t>& x,uint32_t v){for(int i=0;i<4;++i)x.push_back(uint8_t(v>>(8*i)));}
    static void Write64(std::vector<uint8_t>& x,uint64_t v){for(int i=0;i<8;++i)x.push_back(uint8_t(v>>(8*i)));}
    static uint32_t Read32(const uint8_t* x){uint32_t v=0;for(int i=0;i<4;++i)v|=uint32_t(x[i])<<(i*8);return v;}
    static uint64_t Read64(const uint8_t* x){uint64_t v=0;for(int i=0;i<8;++i)v|=uint64_t(x[i])<<(i*8);return v;}
    static uint32_t Crc(const uint8_t* x,size_t n){
        uint32_t v=0xffffffffu;
        for(size_t i=0;i<n;++i){v^=x[i];for(unsigned j=0;j<8;++j)v=(v>>1)^(0xedb88320u&(0u-(v&1u)));}
        return ~v;
    }
};
} // namespace fsyn
