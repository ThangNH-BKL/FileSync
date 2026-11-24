#pragma once
// Server-side *advisory* sampler. Not traffic accounting or router QoS.
// One server interface can have background traffic, multiple interfaces make
// the estimate conservative. Prefer explicit LAN ceiling for shared 100Mbps.
#include <windows.h>
#include <iphlpapi.h>
#include <netioapi.h>
#include <unordered_map>
#include <cstdint>
#include <algorithm>
#pragma comment(lib,"Iphlpapi.lib")
namespace fsyn {
class ServerNetworkSampler {
public:
    double MeasureOtherFraction(uint64_t ourReceivedTotal, uint64_t configuredCapacityBytesPerSec) {
        PMIB_IF_TABLE2 table=nullptr;
        if(GetIfTable2(&table)!=NO_ERROR || !table) return -1.0;
        const ULONGLONG now=GetTickCount64();
        double observed=0.0;
        double elapsed=(previousTick_ && now>previousTick_) ? double(now-previousTick_)/1000.0 : 0.0;
        for(ULONG i=0;i<table->NumEntries;++i) {
            const auto& nic=table->Table[i];
            if(nic.OperStatus!=IfOperStatusUp || nic.Type==IF_TYPE_SOFTWARE_LOOPBACK ||
               nic.ReceiveLinkSpeed==0) continue;
            auto it=previousRx_.find(nic.InterfaceLuid.Value);
            if(elapsed>0.0 && it!=previousRx_.end() && nic.InOctets>=it->second) {
                const double rate=double(nic.InOctets-it->second)/elapsed;
                observed=(std::max)(observed,rate); // busiest usable interface
            }
            previousRx_[nic.InterfaceLuid.Value]=nic.InOctets;
        }
        FreeMibTable(table);
        const uint64_t delta=ourReceivedTotal>=previousOwn_?ourReceivedTotal-previousOwn_:0;
        previousOwn_=ourReceivedTotal; previousTick_=now;
        if(elapsed<=0.0 || configuredCapacityBytesPerSec==0) return -1.0;
        const double other=(std::max)(0.0,observed-double(delta)/elapsed);
        return (std::min)(2.0,other/static_cast<double>(configuredCapacityBytesPerSec));
    }
private:
    ULONGLONG previousTick_=0;
    uint64_t previousOwn_=0;
    std::unordered_map<ULONG64,ULONGLONG> previousRx_;
};
} // namespace fsyn
