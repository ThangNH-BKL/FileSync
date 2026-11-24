// Standalone Linux/Clang/GCC validation for the disk spool format.
#include "DiskSnapshotSpool.h"
#include <filesystem>
#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <chrono>
#include <cstdlib>
int main() {
    using fsyn::DiskSnapshotSpool;
    const auto root=std::filesystem::temp_directory_path()/
        ("fs_v83_portable_"+std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    std::error_code ec;
    const auto file=root/"spool.bin";
    DiskSnapshotSpool spool;
    if(!spool.Open(file))return 1;
    constexpr size_t kCount=50000;
    for(size_t i=0;i<kCount;++i) {
        if(!spool.Append({"x/"+std::to_string(i)+".ini",std::string(40,'a'),i,static_cast<int64_t>(i)}))return 2;
    }
    if(!spool.Finish())return 3;
    size_t cursor=0;bool end=false;
    while(!end) {
        std::vector<DiskSnapshotSpool::Entry> batch;
        if(!spool.ReadBatch(500,batch,end))return 4;
        for(auto& e:batch) {
            if(e.path!="x/"+std::to_string(cursor)+".ini" || e.size!=cursor || e.mtime!=static_cast<int64_t>(cursor)) return 5;
            ++cursor;
        }
    }
    if(cursor!=kCount) return 6;
    spool.Close(true);
    if(std::filesystem::exists(file)) return 7;
    const auto bad=root/"bad.bin";
    if(!spool.Open(bad) || !spool.Append({"a.ini",std::string(40,'b'),12,1}) || !spool.Finish()) return 8;
    {std::fstream f(bad,std::ios::in|std::ios::out|std::ios::binary);if(!f)return 9;f.seekp(4);f.put('z');f.flush();}
    std::vector<DiskSnapshotSpool::Entry> batch;
    if(spool.ReadBatch(500,batch,end))return 10;
    spool.Close(true);
    std::filesystem::remove_all(root,ec);
    std::cout<<"PASS: 50000 records, byte order, CRC rejection, cleanup"<<std::endl;
    return 0;
}
