#pragma once
// FileSync V8 experimental AcceptEx/IOCP acceptor. DOES NOT replace the
// blocking ClientSession protocol reader or its heartbeat thread.
// Never enable for production before Windows MSVC + teardown fault tests.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <mswsock.h>
#include <windows.h>
#include <array>
#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>
#include <chrono>
#include <thread>
namespace fsyn {
class IocpAcceptLoop {
    struct Slot {
        OVERLAPPED overlapped{}; // must remain at offset zero
        SOCKET client=INVALID_SOCKET;
        std::array<char,2*(sizeof(sockaddr_in)+16)> address{};
        bool pending=false;
    };
    static_assert(offsetof(Slot,overlapped)==0,"IOCP overlapped layout");
    static constexpr size_t kDepth=8;
public:
    using Handler=std::function<void(SOCKET,const sockaddr_in&)>;
    // Runs on ServerEngine's dedicated accept thread.
    bool Run(SOCKET listenSock,const std::atomic<bool>& stop,const Handler& received){
        GUID guid=WSAID_ACCEPTEX;
        LPFN_ACCEPTEX acceptEx=nullptr;DWORD bytes=0;
        if(WSAIoctl(listenSock,SIO_GET_EXTENSION_FUNCTION_POINTER,&guid,sizeof(guid),
                    &acceptEx,sizeof(acceptEx),&bytes,nullptr,nullptr)!=0 || !acceptEx)return false;
        HANDLE cp=CreateIoCompletionPort(reinterpret_cast<HANDLE>(listenSock),nullptr,1,1);
        if(!cp)return false;
        // Heap lifetime permits intentional fail-closed quarantine on an OS
        // teardown anomaly (never free a still-pending OVERLAPPED buffer).
        auto slotsOwner=std::make_unique<std::array<Slot,kDepth>>();
        auto& slots=*slotsOwner;
        size_t active=0;
        auto post=[&](Slot& slot)->bool {
            if(stop.load())return false;
            slot.overlapped={};slot.client=WSASocketW(AF_INET,SOCK_STREAM,IPPROTO_TCP,
                                       nullptr,0,WSA_FLAG_OVERLAPPED);
            if(slot.client==INVALID_SOCKET)return false;
            DWORD receivedBytes=0;
            BOOL ok=acceptEx(listenSock,slot.client,slot.address.data(),0,
                   DWORD(sizeof(sockaddr_in)+16),DWORD(sizeof(sockaddr_in)+16),
                   &receivedBytes,&slot.overlapped);
            if(!ok && WSAGetLastError()!=ERROR_IO_PENDING){
                closesocket(slot.client);slot.client=INVALID_SOCKET;return false;
            }
            slot.pending=true;++active;return true;
        };
        for(auto& s:slots)post(s);
        if(!active){CloseHandle(cp);return false;}
        while(!stop.load()){
            DWORD n=0;ULONG_PTR key=0;LPOVERLAPPED ov=nullptr;
            BOOL ok=GetQueuedCompletionStatus(cp,&n,&key,&ov,250);
            if(!ov) {
                if(stop.load())break;
                // Retry slot submissions after temporary resource exhaustion.
                for(auto& slot:slots)if(!slot.pending)post(slot);
                continue;
            }
            Slot* slot=reinterpret_cast<Slot*>(ov);
            bool known=false;
            for(auto& s:slots) if(&s==slot) {known=true;break;}
            if(!known || !slot->pending)continue;
            slot->pending=false;--active;
            SOCKET client=slot->client;slot->client=INVALID_SOCKET;
            if(ok && !stop.load()) {
                // SO_UPDATE_ACCEPT_CONTEXT is mandatory for standard socket APIs.
                if(setsockopt(client,SOL_SOCKET,SO_UPDATE_ACCEPT_CONTEXT,
                    reinterpret_cast<const char*>(&listenSock),sizeof(listenSock))==0){
                    sockaddr_in peer{};int len=sizeof(peer);
                    if(getpeername(client,reinterpret_cast<sockaddr*>(&peer),&len)==0 &&
                       len>=int(sizeof(sockaddr_in))) {
                        // Re-arm before potentially slow admission/session work.
                        post(*slot);
                        received(client,peer);
                        continue;
                    }
                }
            }
            closesocket(client);
            if(!stop.load())post(*slot);
        }
        // ServerEngine::Stop() closes the listen socket before joining this
        // thread, cancelling outstanding AcceptEx requests. Drain completions
        // BEFORE destroying the OVERLAPPED storage or closing the IOCP.
        for(auto& slot:slots)if(slot.pending) {
            CancelIoEx(reinterpret_cast<HANDLE>(listenSock),&slot.overlapped);
            if(slot.client!=INVALID_SOCKET)closesocket(slot.client);
        }
        // No UAF: wait for completion notifications of every pending slot.
        // If cancellation cannot be observed, retain the slots until process
        // shutdown would be preferable to freeing live OVERLAPPED storage.
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(15);
        while(active && std::chrono::steady_clock::now()<deadline){
            DWORD n=0;ULONG_PTR key=0;LPOVERLAPPED ov=nullptr;
            GetQueuedCompletionStatus(cp,&n,&key,&ov,250);
            if(!ov)continue;
            Slot* slot=reinterpret_cast<Slot*>(ov);
            for(auto& s:slots)if(&s==slot && s.pending){s.pending=false;--active;break;}
        }
        // Failure to drain implies uncertain OVERLAPPED lifetimes; never
        // silently treat this as a successful graceful shutdown.
        const bool drained=active==0;
        if(!drained) {
            // A shutdown anomaly: quarantine I/O buffers and completion port
            // until process exit rather than introduce use-after-free.
            (void)slotsOwner.release();
            return false;
        }
        CloseHandle(cp);
        return true;
    }
};
} // namespace fsyn
