#pragma once
// A conservative NTFS USN checkpoint validator. This is intentionally NOT
// an incremental sync engine: no persisted checkpoint is advanced until the
// protocol has a durable server ACK/barrier. V8 still sends full inventory.
#include <cstdint>
#include <string>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winioctl.h>
#endif
namespace fsyn {
struct UsnState {
    uint64_t journalId=0;
    int64_t first=0, lowestValid=0, next=0;
};
struct UsnCheckpoint {uint64_t journalId=0;int64_t lastAckedUsn=0;};
inline bool IsUsnCursorUsable(const UsnState& now,const UsnCheckpoint& previous) {
    return previous.journalId!=0 && previous.journalId==now.journalId &&
       previous.lastAckedUsn>=(now.first>now.lowestValid?now.first:now.lowestValid) &&
       previous.lastAckedUsn<=now.next && now.next>=now.lowestValid;
}
#ifdef _WIN32
inline bool QueryNtfsUsnState(const std::wstring& root,UsnState& out,DWORD& err) {
    out={};err=ERROR_NOT_SUPPORTED;
    if(root.size()<3 || root[1]!=L':' || (root[2]!=L'\\'&&root[2]!=L'/')) return false;
    wchar_t drive[4]={root[0],L':',L'\\',0};
    wchar_t type[32]{};
    if(!GetVolumeInformationW(drive,nullptr,0,nullptr,nullptr,nullptr,type,32)) {
        err=GetLastError();return false;
    }
    if(_wcsicmp(type,L"NTFS")!=0) return false;
    const std::wstring device=L"\\\\.\\"+root.substr(0,2);
    HANDLE handle=CreateFileW(device.c_str(),GENERIC_READ,
        FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,0,nullptr);
    if(handle==INVALID_HANDLE_VALUE){err=GetLastError();return false;}
    USN_JOURNAL_DATA_V0 data{};DWORD actual=0;
    bool ok=DeviceIoControl(handle,FSCTL_QUERY_USN_JOURNAL,nullptr,0,
        &data,sizeof(data),&actual,nullptr)!=FALSE && actual>=sizeof(data);
    if(!ok) err=GetLastError();
    CloseHandle(handle);
    if(!ok) return false;
    out.journalId=data.UsnJournalID;
    out.first=data.FirstUsn;out.lowestValid=data.LowestValidUsn;out.next=data.NextUsn;
    err=ERROR_SUCCESS;
    return true;
}
#endif
} // namespace fsyn
