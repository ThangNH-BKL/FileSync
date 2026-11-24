#pragma once
// V8.4: bounded AIMD flow control policy; portable, deterministic, no Win32.
// This policy only chooses a receive pacing rate. It does NOT promise wire QoS.
#include <algorithm>
#include <cstdint>

namespace fsyn {
class AdaptiveServerPolicy {
public:
    struct Sample {
        // Best-effort external receive traffic / configured LAN capacity.
        // Negative means telemetry unavailable; fail conservative (hold rate).
        double externalFraction = -1.0;
        uint64_t flushUs = 0;
        uint64_t flushCount = 0;
        unsigned activeTransfers = 0;
    };
    AdaptiveServerPolicy(uint64_t low, uint64_t high, uint64_t initial)
        : low_((std::max)(uint64_t(256*1024),low)), high_((std::max)(low_,high)),
          rate_((std::max)(low_,(std::min)(high_,initial))) {}
    uint64_t Rate() const {return rate_;}
    uint64_t Floor() const {return low_;}
    uint64_t Ceiling() const {return high_;}
    uint64_t Step(const Sample& s) {
        const bool congestedNetwork=s.externalFraction>=0.45;
        // A very slow HDD flush is an indication, not a disk utilization proof.
        const bool congestedDisk=s.flushCount>=4 && s.activeTransfers>=2 &&
            (static_cast<double>(s.flushUs)/s.flushCount)>=40000.0;
        if(congestedNetwork || congestedDisk) {
            rate_=(std::max)(low_, rate_*7/10); // controlled multiplicative reduction
        } else if(s.externalFraction>=0.0 && s.externalFraction<0.18 && s.activeTransfers>0) {
            const uint64_t add=(std::max)(uint64_t(256*1024),high_/20);
            rate_=(std::min)(high_,rate_ > high_-add ? high_ : rate_+add);
        }
        // Traffic sampling unavailable / medium load: hold rate, no unbounded ramp.
        return rate_;
    }
private:
    uint64_t low_,high_,rate_;
};
} // namespace fsyn
