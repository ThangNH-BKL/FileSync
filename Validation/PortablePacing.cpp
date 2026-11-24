#include "GlobalTransferControl.h"
#include <atomic>
#include <iostream>
int main() {
    using fsyn::GlobalTransferControl;
    std::atomic<bool> stop{false};
    GlobalTransferControl gate(2,1024*1024,1024*1024);
    if(!gate.TryEnter((void*)1))return 1;
    if(!gate.TryEnter((void*)2))return 2;
    if(gate.TryEnter((void*)3))return 3;
    gate.Leave((void*)1);
    if(!gate.TryEnter((void*)3))return 4;
    if(!gate.PaceReceived(65536,stop))return 5;
    gate.Leave((void*)2); gate.Leave((void*)3);
    if(gate.Active()!=0)return 6;
    std::cout << "PASS: admission, pacing, release, metrics=" << gate.ReceivedBytes() << "\n";
    return 0;
}
