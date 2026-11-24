#pragma once
// FileSync V7 preview: ALL persistent file-generation metadata lives under
// State folder: <ServerRoot>\.fsync_state\<ClientIP>\<ExeFolder> (no data sidecars)
//   baseline_v7/shard_XXXX.bin     : indexed first-generation identities
//   baseline.log                   : read-only legacy migration source
//   versions\<hash-prefix>\<SHA256>.vmap : generation reservations (only on recreate)
// NEVER create <logical>.fsync.versions beside a committed data file.
// Legacy V6/V6.1 sidecars are migrated atomically on first access. A partial
// migration fails closed and preserves both identity and older data.
// Existing V6 wire protocol and SHA-256 commit checks are unchanged.
#include "VersionedFileNames.h"
#include "FileInstanceId.h"
#include "FileIo.h"
#include "Crypto.h"
#include "ShardedBaselineIndex.h"
#include "DiskBaselineLookup.h"
#include <windows.h>
#include <vector>
#include <utility>
#include <mutex>
#include <sstream>
#include <limits>
#include <algorithm>
#include <cstdlib>
#include <unordered_map>
#include <map>
#include <memory>
#include <array>
#include <cwctype>
namespace fsyn {
struct VersionPlan {
    std::wstring destination;
    std::wstring base;
    std::string identity;
    uint32_t index = 0;
    bool destinationExisted = false;
    bool legacyAdoption = false;
};
class FileVersionStore {
    using Entries = std::vector<std::pair<std::string, uint32_t>>;
    static std::mutex& PathLock(const std::wstring& root, const std::wstring& client,
                               const std::wstring& logical) {
        static std::array<std::mutex,257> locks;
        return locks[std::hash<std::wstring>{}(root+L"|"+client+L"|"+logical)%locks.size()];
    }
    class BaselineFacade {
    public:
        explicit BaselineFacade(const std::wstring& clientState) {
            char mode[8]{};
            if(GetEnvironmentVariableA("FILESYNC_EXPERIMENTAL_DISK_INDEX",mode,sizeof(mode))==1 &&
               mode[0]=='1')
                v8_=std::make_unique<DiskBaselineLookup>(clientState);
            else
                v7_=std::make_unique<ShardedBaselineIndex>(clientState);
        }
        bool Find(const std::wstring& key,std::string& out) {
            return v8_?v8_->Find(key,out):v7_->Find(key,out);
        }
        bool Reserve(const std::wstring& key,const std::string& id) {
            return v8_?v8_->Reserve(key,id):v7_->Reserve(key,id);
        }
    private:
        std::unique_ptr<DiskBaselineLookup> v8_;
        std::unique_ptr<ShardedBaselineIndex> v7_;
    };
    static BaselineFacade& Index(const std::wstring& indexPath) {
        static std::mutex poolMu;
        static std::map<std::wstring,std::unique_ptr<BaselineFacade>> pool;
        std::lock_guard<std::mutex> guard(poolMu);
        auto& store=pool[indexPath];
        if (!store) {
            auto pos=indexPath.find_last_of(L"\\/");
            store=std::make_unique<BaselineFacade>(indexPath.substr(0,pos));
        }
        return *store;
    }
    static std::string LegacyId() { return std::string(40, '0'); }
    static bool Exists(const std::wstring& p) {
        DWORD a = GetFileAttributesW(p.c_str());
        return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
    }
    static bool IsMissing(const std::wstring& p) {
        if (GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES) return false;
        DWORD e = GetLastError();
        return e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND;
    }
    // V2 manifests bind the reservations to the canonical relative pathname.
    // A SHA-256 filename collision (or a mis-copied vmap) must fail closed.
    // Legacy sidecars use V1 format and are READ-ONLY inputs for migration.
    static bool Load(const std::wstring& manifest, Entries& entries,
                     const std::string& expectedKey, bool allowV1) {
        if (IsMissing(manifest)) return true;
        FileIo file;
        if (!file.OpenForRead(manifest) || file.Size() > 256u*1024u) return false;
        std::vector<uint8_t> bytes;
        if (!FileIo::ReadAll(manifest, bytes)) return false;
        const std::string content(bytes.begin(), bytes.end());
        if (content.empty() || content.back() != '\n') return false;
        std::istringstream lines(content);
        std::string header;
        if (!std::getline(lines,header)) return false;
        if (header == "FSYN-VERSION-2") {
            std::string boundKey;
            if (!std::getline(lines,boundKey) || boundKey != expectedKey) return false;
        } else if (!allowV1 || header != "FSYN-VERSION-1") return false;
        std::string line;
        while (std::getline(lines,line)) {
            std::istringstream entry(line);
            std::string id, extra;
            unsigned long long n = 0;
            if (line.empty() || !(entry >> id >> n) || (entry >> extra) ||
                !IsValidFileInstanceId(id) || n > 1000000ull) return false;
            for (const auto& prior : entries)
                if (prior.first == id || prior.second == n) return false;
            entries.emplace_back(id, static_cast<uint32_t>(n));
        }
        return !entries.empty();
    }
    static bool Save(const std::wstring& manifest, const Entries& entries,
                     const std::string& canonicalKey) {
        std::ostringstream text;
        text << "FSYN-VERSION-2\n" << canonicalKey << "\n";
        for (const auto& e : entries) text << e.first << ' ' << e.second << '\n';
        const std::string data = text.str();
        const std::wstring tmp = manifest + L".writing";
        if (!FileIo::EnsureParentDir(tmp)) return false;
        HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return false;
        DWORD written = 0;
        bool ok = WriteFile(h, data.data(), static_cast<DWORD>(data.size()), &written, nullptr) &&
                  written == data.size() && FlushFileBuffers(h);
        CloseHandle(h);
        if (!ok) { DeleteFileW(tmp.c_str()); return false; }
        ok = MoveFileExW(tmp.c_str(), manifest.c_str(),
                          MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
        if (!ok) DeleteFileW(tmp.c_str());
        return ok;
    }
    static std::wstring Normalized(const std::wstring& s) {
        std::wstring t(s);
        for (auto& c : t) {
            if (c == L'/') c = L'\\';
            c = static_cast<wchar_t>(std::towlower(c));
        }
        return t;
    }
    static bool Paths(const std::wstring& serverRoot, const std::wstring& clientRoot,
                      const std::wstring& logical, std::wstring& indexPath,
                      std::wstring& key) {
        // Root/subtree checks prevent metadata escaping the designated ServerRoot.
        auto root = Normalized(serverRoot), client = Normalized(clientRoot), path = Normalized(logical);
        while (root.size() > 1 && root.back() == L'\\') root.pop_back();
        while (client.size() > 3 && client.back() == L'\\') client.pop_back();
        if (client.size() <= root.size() || client.compare(0, root.size(), root) ||
            client[root.size()] != L'\\' ||
            path.size() <= client.size() || path.compare(0, client.size(), client) ||
            path[client.size()] != L'\\') return false;
        const std::wstring relativeClient = client.substr(root.size() + 1);
        key = path.substr(client.size() + 1);
        if (relativeClient.empty() || key.empty()) return false;
        size_t cursor = 0;
        while (cursor < relativeClient.size()) {
            auto next = relativeClient.find(L'\\', cursor);
            auto part = relativeClient.substr(cursor,
                next == std::wstring::npos ? next : next - cursor);
            if (part.empty() || part == L"." || part == L"..") return false;
            if (next == std::wstring::npos) break;
            cursor = next + 1;
        }
        indexPath = serverRoot;
        if (!indexPath.empty() && indexPath.back() != L'\\' && indexPath.back() != L'/')
            indexPath += L'\\';
        indexPath += L".fsync_state\\" + relativeClient + L"\\baseline.log";
        return true;
    }
    static std::string HexKey(const std::wstring& key) {
        static const char* digits = "0123456789abcdef";
        std::string out;
        out.reserve(key.size() * 4);
        for (wchar_t wc : key) {
            unsigned n = static_cast<unsigned>(wc) & 0xffffu; // Windows UTF-16
            out.push_back(digits[(n >> 12) & 15]); out.push_back(digits[(n >> 8) & 15]);
            out.push_back(digits[(n >> 4) & 15]); out.push_back(digits[n & 15]);
        }
        return out;
    }
    static bool StateManifestPath(const std::wstring& indexPath,
                                  const std::wstring& key,
                                  std::wstring& manifest, std::string& canonicalKey) {
        canonicalKey = HexKey(key);
        uint8_t sha[32]{};
        if (!Sha256::HashBuffer(canonicalKey.data(),canonicalKey.size(),sha)) return false;
        const std::string hash=Sha256ToHex(sha);
        const auto slash=indexPath.find_last_of(L"\\/");
        if (slash==std::wstring::npos) return false;
        manifest=indexPath.substr(0,slash) + L"\\versions\\" +
                 Utf8Ascii(hash.substr(0,2)) + L"\\" + Utf8Ascii(hash) + L".vmap";
        return true;
    }
    static std::wstring Utf8Ascii(const std::string& s) {
        return std::wstring(s.begin(),s.end()); // only lowercase hex digits
    }
    static bool HideStateRoot(const std::wstring& serverRoot) {
        std::wstring state=serverRoot;
        if (!state.empty() && state.back()!=L'\\' && state.back()!=L'/') state+=L'\\';
        state+=L".fsync_state";
        DWORD attr=GetFileAttributesW(state.c_str());
        return attr!=INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) &&
               ((attr & FILE_ATTRIBUTE_HIDDEN) ||
                SetFileAttributesW(state.c_str(),attr|FILE_ATTRIBUTE_HIDDEN)!=FALSE);
    }
    static bool IsContained(const Entries& oldEntries, const Entries& newer) {
        for (const auto& old : oldEntries) {
            bool matched=false;
            for (const auto& current : newer)
                if (old==current) {matched=true;break;}
            if (!matched) return false;
        }
        return true;
    }
    static bool DecodeKey(const std::string& hex, std::wstring& out) {
        if (hex.empty() || hex.size() % 4 != 0 || hex.size() > 32768u) return false;
        out.clear();
        for (size_t i = 0; i < hex.size(); i += 4) {
            unsigned n = 0;
            for (size_t j = i; j < i + 4; ++j) {
                char c = hex[j];
                unsigned v = c >= '0' && c <= '9' ? c - '0' :
                    c >= 'a' && c <= 'f' ? c - 'a' + 10 : 16;
                if (v == 16) return false;
                n = (n << 4) | v;
            }
            out.push_back(static_cast<wchar_t>(n));
        }
        return true;
    }
    static bool ReadBaseline(const std::wstring& path,const std::wstring& key,std::string& id) {
        return Index(path).Find(key,id);
    }
    static bool RecordBaseline(const std::wstring& path,const std::wstring& key,const std::string& id) {
        return Index(path).Reserve(key,id);
    }
public:
    // The legacy V4 chunk journal is metadata, not user data. Keep it in the
    // same private metadata hierarchy if the legacy handler is ever invoked.
    static bool JournalPath(const std::wstring& serverRoot,
                            const std::wstring& clientRoot,
                            const std::wstring& logical,
                            std::wstring& out) {
        std::wstring indexPath,key,manifest;
        std::string keyHex;
        if (!Paths(serverRoot,clientRoot,logical,indexPath,key) ||
            !StateManifestPath(indexPath,key,manifest,keyHex)) return false;
        const auto slash=manifest.find_last_of(L"\\/");
        if (slash==std::wstring::npos) return false;
        const auto hashName=manifest.substr(slash+1);
        const auto prefix=hashName.substr(0,2);
        const auto clientState=indexPath.substr(0,indexPath.find_last_of(L"\\/"));
        out=clientState + L"\\journals\\" + prefix + L"\\" +
            hashName.substr(0,hashName.size()-5) + L".blocks";
        return true;
    }
    // Reservations are serialized by per-logical-path striped locks.
    // ServerRoot and clientRoot must be trusted paths determined by handshake.
    static bool Resolve(const std::wstring& serverRoot, const std::wstring& clientRoot,
                        const std::wstring& logical, const std::string& identity,
                        VersionPlan& out) {
        if (!IsValidFileInstanceId(identity)) return false;
        std::lock_guard<std::mutex> guard(PathLock(serverRoot,clientRoot,logical));
        std::wstring indexPath,key,manifest;
        std::string keyHex;
        if (!Paths(serverRoot,clientRoot,logical,indexPath,key) ||
            !StateManifestPath(indexPath,key,manifest,keyHex)) return false;
        const std::wstring legacySidecar = logical + L".fsync.versions";
        std::string baselineId;
        if (!ReadBaseline(indexPath,key,baselineId)) return false;
        bool hasBaseline=!baselineId.empty();
        Entries entries;
        if (!Load(manifest,entries,keyHex,false)) return false;

        // Migration from pre-V6.2. New metadata is written and flushed BEFORE
        // removing any old sidecar, preventing lost reservations on a crash.
        if (!IsMissing(legacySidecar)) {
            Entries old;
            if (!Load(legacySidecar,old,keyHex,true)) return false;
            if (entries.empty() && old.size()==1 && old[0].second==0) {
                // Normal file: one baseline record, no per-file manifest.
                if (hasBaseline && baselineId!=old[0].first)
                    return false;
                if (!hasBaseline) {
                    if (!RecordBaseline(indexPath,key,old[0].first) || !HideStateRoot(serverRoot)) return false;
                    baselineId=old[0].first; hasBaseline=true;
                }
            } else if (entries.empty()) {
                if (!Save(manifest,old,keyHex) || !HideStateRoot(serverRoot)) return false;
                entries=old;
            } else if (!IsContained(old,entries)) {
                // Keep the old sidecar for manual recovery rather than guess.
                return false;
            }
            if (!DeleteFileW(legacySidecar.c_str()) && !IsMissing(legacySidecar))
                return false;
        }

        // A V5 legacy file may have been adopted (after checking SHA-256) just
        // before a crash; the baseline append succeeded but the manifest remove
        // didn't. Complete the clean-up idempotently on the next Resolve().
        if (entries.size()==2 && entries[0].first==LegacyId() && entries[0].second==0 &&
            entries[1].second!=0 && hasBaseline &&
            baselineId==entries[1].first && Exists(logical) &&
            !Exists(NumberedFilePath(logical,entries[1].second))) {
            if (!DeleteFileW(manifest.c_str()) && !IsMissing(manifest)) return false;
            entries.clear();
        }

        // Generation 0 from an old manifest can be collapsed to baseline.
        if (entries.size()==1 && entries[0].second==0) {
            if (!hasBaseline) {
                if (!RecordBaseline(indexPath,key,entries[0].first) || !HideStateRoot(serverRoot)) return false;
                baselineId=entries[0].first; hasBaseline=true;
            }
            if (baselineId!=entries[0].first) return false;
            if (!DeleteFileW(manifest.c_str()) && !IsMissing(manifest)) return false;
            entries.clear();
        }

        if (entries.empty()) {
            /* baseline remains loaded */
            if (hasBaseline) {
                if (baselineId==identity) {
                    out=VersionPlan{};
                    out.identity=identity;
                    out.destination=logical;
                    out.destinationExisted=Exists(logical);
                    if (out.destinationExisted) out.base=logical;
                    return true;
                }
                // Client recreated the file while either peer was offline.
                entries.emplace_back(baselineId,0u);
            } else if (!Exists(logical)) {
                if (!RecordBaseline(indexPath,key,identity) || !HideStateRoot(serverRoot)) return false;
                out=VersionPlan{};
                out.identity=identity;
                out.destination=logical;
                return true;
            } else {
                // V5 data has no identity: reserve an alternative until SHA-256
                // verifies whether the existing file can be adopted safely.
                entries.emplace_back(LegacyId(),0u);
            }
        }

        uint32_t index=0;
        bool found=false;
        for (const auto& e : entries) if (e.first==identity) {
            index=e.second;found=true;break;
        }
        if (!found) {
            for (const auto& e : entries) index=(std::max)(index,e.second);
            if (index==UINT32_MAX) return false;
            ++index;
            while (index<1000000u && Exists(NumberedFilePath(logical,index))) ++index;
            if (index>=1000000u) return false;
            entries.emplace_back(identity,index);
            if (!Save(manifest,entries,keyHex) || !HideStateRoot(serverRoot)) return false;
        }
        out=VersionPlan{};
        out.identity=identity;
        out.index=index;
        out.destination=NumberedFilePath(logical,index);
        out.destinationExisted=Exists(out.destination);
        out.legacyAdoption=index!=0 && !out.destinationExisted &&
            entries.size()==2 && entries[0].first==LegacyId() &&
            entries[0].second==0 && entries[1].first==identity &&
            entries[1].second==index;
        if (out.destinationExisted) out.base=out.destination;
        else {
            uint32_t latest=0;
            bool have=false;
            for (const auto& e : entries) {
                if (e.first==identity) continue;
                const auto candidate=NumberedFilePath(logical,e.second);
                if (Exists(candidate) && (!have || e.second>latest)) {
                    latest=e.second;have=true;
                }
            }
            if (have) out.base=NumberedFilePath(logical,latest);
            else if (index && Exists(logical)) out.base=logical;
        }
        return true;
    }
    // Called ONLY after Server has verified the existing V5 file's SHA-256
    // equals the Client's identical source (never on a hash mismatch).
    static bool AdoptIdenticalLegacy(const std::wstring& serverRoot,
                                     const std::wstring& clientRoot,
                                     const std::wstring& logical,
                                     const std::string& identity) {
        std::lock_guard<std::mutex> guard(PathLock(serverRoot,clientRoot,logical));
        std::wstring indexPath,key,manifest;
        std::string keyHex;
        if (!Paths(serverRoot,clientRoot,logical,indexPath,key) ||
            !StateManifestPath(indexPath,key,manifest,keyHex)) return false;
        Entries entries;
        if (!Load(manifest,entries,keyHex,false) || entries.size()!=2 ||
            entries[0].first!=LegacyId() || entries[0].second!=0 ||
            entries[1].first!=identity || entries[1].second==0 ||
            !Exists(logical) || Exists(NumberedFilePath(logical,entries[1].second)))
            return false;
        if (!RecordBaseline(indexPath,key,identity) || !HideStateRoot(serverRoot)) return false;
        // A crash here is safe: Resolve() detects the adopted ID in baseline
        // and discards the stale V5 reservation without making A (1).
        return DeleteFileW(manifest.c_str())!=FALSE || IsMissing(manifest);
    }
};
}
