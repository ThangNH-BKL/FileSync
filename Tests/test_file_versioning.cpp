#include "MiniTest.h"
#include "VersionedFileNames.h"
#include "FileVersionStore.h"
#include "FileInstanceId.h"
#include "FileIo.h"
#include <windows.h>
#include <filesystem>
#include <string>
#include <vector>

using namespace fsyn;
namespace {
const std::string kId0 = "0000000100000000000000010000000000000001";
const std::string kId1 = "0000000100000000000000020000000000000002";
const std::string kId2 = "0000000100000000000000030000000000000003";

std::wstring TempRoot(const wchar_t* tag) {
    wchar_t temp[MAX_PATH]{};
    GetTempPathW(MAX_PATH,temp);
    return std::wstring(temp) + L"FileSyncV62_" + tag + L"_" +
           std::to_wstring(GetCurrentProcessId());
}
void Clean(const std::wstring& root) {
    std::error_code ec;
    std::filesystem::remove_all(std::filesystem::path(root),ec);
}
std::vector<std::wstring> Manifests(const std::wstring& root) {
    std::vector<std::wstring> found;
    const auto state = std::filesystem::path(root) / L".fsync_state";
    std::error_code ec;
    if (!std::filesystem::exists(state,ec)) return found;
    for (auto it=std::filesystem::recursive_directory_iterator(state,ec);
         !ec && it!=std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
        if (it->is_regular_file(ec) && it->path().extension() == L".vmap")
            found.push_back(it->path().wstring());
    }
    return found;
}
}

TEST(file_version_suffix_before_extension) {
    EXPECT_EQ(NumberedFilePath(L"C:\\Data\\A.bin",0), L"C:\\Data\\A.bin");
    EXPECT_EQ(NumberedFilePath(L"C:\\Data\\A.bin",1), L"C:\\Data\\A (1).bin");
    EXPECT_EQ(NumberedFilePath(L"C:\\Data\\A.bin",2), L"C:\\Data\\A (2).bin");
    EXPECT_EQ(NumberedFilePath(L"C:\\Data\\readme",1), L"C:\\Data\\readme (1)");
    EXPECT_EQ(NumberedFilePath(L"C:\\Data\\config.json",3), L"C:\\Data\\config (3).json");
}

TEST(v62_all_version_metadata_is_private_and_sparse) {
    const auto root=TempRoot(L"sparse");
    Clean(root);
    const auto client=root+L"\\192.168.1.101\\ABC";
    const auto logical=client+L"\\Data\\A.bin";
    const auto oldSidecar=logical+L".fsync.versions";
    const auto baseline=root+L"\\.fsync_state\\192.168.1.101\\abc\\baseline.log";
    REQUIRE_TRUE(FileIo::EnsureParentDir(logical));
    VersionPlan a,b,c,d,e;
    EXPECT_TRUE(FileVersionStore::Resolve(root,client,logical,kId0,a));
    EXPECT_EQ(a.destination,logical);
    EXPECT_FALSE(a.destinationExisted);
    EXPECT_EQ(GetFileAttributesW(oldSidecar.c_str()),INVALID_FILE_ATTRIBUTES);
    EXPECT_EQ(Manifests(root).size(),size_t(0));
    REQUIRE_TRUE(FileIo::WriteAll(logical,"old",3));
    EXPECT_TRUE(FileVersionStore::Resolve(root,client,logical,kId0,b));
    EXPECT_EQ(b.destination,logical);
    EXPECT_TRUE(b.destinationExisted);
    EXPECT_EQ(Manifests(root).size(),size_t(0));

    // FileClient deleted and recreated A.bin while FileServer was offline.
    EXPECT_TRUE(FileVersionStore::Resolve(root,client,logical,kId1,c));
    EXPECT_EQ(c.destination,NumberedFilePath(logical,1));
    EXPECT_EQ(c.base,logical);
    EXPECT_EQ(GetFileAttributesW(oldSidecar.c_str()),INVALID_FILE_ATTRIBUTES);
    REQUIRE_EQ(Manifests(root).size(),size_t(1));
    EXPECT_TRUE(FileVersionStore::Resolve(root,client,logical,kId1,d));
    EXPECT_EQ(d.destination,c.destination); // reconnect does not make (2)
    EXPECT_TRUE(FileIo::WriteAll(c.destination,"new",3));
    EXPECT_TRUE(FileVersionStore::Resolve(root,client,logical,kId2,e));
    EXPECT_EQ(e.destination,NumberedFilePath(logical,2));
    EXPECT_EQ(e.base,c.destination);
    REQUIRE_EQ(Manifests(root).size(),size_t(1));
    // V7 stores first-generation IDs in binary shards; legacy baseline.log
    // is not created for a new ServerRoot.
    EXPECT_EQ(GetFileAttributesW(baseline.c_str()),INVALID_FILE_ATTRIBUTES);
    size_t shards=0;
    const auto binaryRoot=std::filesystem::path(root)/L".fsync_state"/
        L"192.168.1.101"/L"abc"/L"baseline_v7";
    REQUIRE_TRUE(std::filesystem::exists(binaryRoot));
    for(auto it=std::filesystem::directory_iterator(binaryRoot);it!=std::filesystem::directory_iterator();++it)
        if(it->path().extension()==L".bin") ++shards;
    EXPECT_TRUE(shards>=1);
    const DWORD stateAttr=GetFileAttributesW((root+L"\\.fsync_state").c_str());
    EXPECT_TRUE(stateAttr!=INVALID_FILE_ATTRIBUTES);
    EXPECT_TRUE((stateAttr & FILE_ATTRIBUTE_HIDDEN)!=0);
    Clean(root);
}

TEST(fileio_writeall_requires_explicit_parent_creation) {
    const auto root=TempRoot(L"fileio_parent");
    Clean(root);
    const auto file=root+L"\\nested\\file.bin";
    // WriteAll is deliberately not a directory creator. Production callers
    // must validate the destination and create its parent explicitly.
    EXPECT_FALSE(FileIo::WriteAll(file,"abc",3));
    REQUIRE_TRUE(FileIo::EnsureParentDir(file));
    REQUIRE_TRUE(FileIo::WriteAll(file,"abc",3));
    std::vector<uint8_t> bytes;
    REQUIRE_TRUE(FileIo::ReadAll(file,bytes));
    EXPECT_EQ(std::string(bytes.begin(),bytes.end()),std::string("abc"));
    Clean(root);
}

TEST(v62_migrate_existing_v61_sidecar_to_private_store) {
    const auto root=TempRoot(L"migrate");
    Clean(root);
    const auto client=root+L"\\192.168.1.102\\ABC";
    const auto logical=client+L"\\Data\\A.bin";
    const auto sidecar=logical+L".fsync.versions";
    REQUIRE_TRUE(FileIo::EnsureParentDir(logical));
    REQUIRE_TRUE(FileIo::WriteAll(logical,"old",3));
    const std::string oldManifest="FSYN-VERSION-1\n"+kId0+" 0\n"+kId1+" 1\n";
    REQUIRE_TRUE(FileIo::WriteAll(sidecar,oldManifest.data(),oldManifest.size()));
    VersionPlan plan;
    EXPECT_TRUE(FileVersionStore::Resolve(root,client,logical,kId1,plan));
    EXPECT_EQ(plan.destination,NumberedFilePath(logical,1));
    EXPECT_EQ(GetFileAttributesW(sidecar.c_str()),INVALID_FILE_ATTRIBUTES);
    REQUIRE_EQ(Manifests(root).size(),size_t(1));
    EXPECT_TRUE(FileVersionStore::Resolve(root,client,logical,kId1,plan));
    EXPECT_EQ(plan.destination,NumberedFilePath(logical,1));
    Clean(root);
}

TEST(v62_migrate_single_generation_sidecar_without_private_vmap) {
    const auto root=TempRoot(L"single");
    Clean(root);
    const auto client=root+L"\\192.168.1.103\\ABC";
    const auto logical=client+L"\\Data\\A.bin";
    const auto sidecar=logical+L".fsync.versions";
    const std::string oldManifest="FSYN-VERSION-1\n"+kId0+" 0\n";
    REQUIRE_TRUE(FileIo::EnsureParentDir(logical));
    REQUIRE_TRUE(FileIo::WriteAll(logical,"same",4));
    REQUIRE_TRUE(FileIo::WriteAll(sidecar,oldManifest.data(),oldManifest.size()));
    VersionPlan plan;
    EXPECT_TRUE(FileVersionStore::Resolve(root,client,logical,kId0,plan));
    EXPECT_EQ(plan.destination,logical);
    EXPECT_EQ(GetFileAttributesW(sidecar.c_str()),INVALID_FILE_ATTRIBUTES);
    EXPECT_EQ(Manifests(root).size(),size_t(0));
    Clean(root);
}

TEST(v62_legacy_v5_identical_adoption_survives_crash_before_cleanup) {
    const auto root=TempRoot(L"adopt");
    Clean(root);
    const auto client=root+L"\\192.168.1.104\\ABC";
    const auto logical=client+L"\\Data\\A.bin";
    REQUIRE_TRUE(FileIo::EnsureParentDir(logical));
    REQUIRE_TRUE(FileIo::WriteAll(logical,"same",4));
    VersionPlan plan;
    EXPECT_TRUE(FileVersionStore::Resolve(root,client,logical,kId1,plan));
    EXPECT_TRUE(plan.legacyAdoption);
    auto manifests=Manifests(root);
    REQUIRE_EQ(manifests.size(),size_t(1));
    // Block DeleteFileW to simulate a crash after durable baseline append.
    HANDLE locked=CreateFileW(manifests[0].c_str(),GENERIC_READ,FILE_SHARE_READ,
                              nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    REQUIRE_TRUE(locked!=INVALID_HANDLE_VALUE);
    EXPECT_FALSE(FileVersionStore::AdoptIdenticalLegacy(root,client,logical,kId1));
    if (locked!=INVALID_HANDLE_VALUE) CloseHandle(locked);
    EXPECT_TRUE(FileVersionStore::Resolve(root,client,logical,kId1,plan));
    EXPECT_EQ(plan.destination,logical);
    EXPECT_EQ(Manifests(root).size(),size_t(0));
    EXPECT_TRUE(FileVersionStore::Resolve(root,client,logical,kId2,plan));
    EXPECT_EQ(plan.destination,NumberedFilePath(logical,1));
    Clean(root);
}

TEST(v62_corrupted_private_metadata_refuses_version_reservation) {
    const auto root=TempRoot(L"corrupt");
    Clean(root);
    const auto client=root+L"\\192.168.1.105\\ABC";
    const auto logical=client+L"\\A.bin";
    REQUIRE_TRUE(FileIo::EnsureParentDir(logical));
    VersionPlan plan;
    EXPECT_TRUE(FileVersionStore::Resolve(root,client,logical,kId0,plan));
    REQUIRE_TRUE(FileIo::WriteAll(logical,"old",3));
    EXPECT_TRUE(FileVersionStore::Resolve(root,client,logical,kId1,plan));
    auto manifests=Manifests(root);
    REQUIRE_EQ(manifests.size(),size_t(1));
    REQUIRE_TRUE(FileIo::WriteAll(manifests[0],"BROKEN",6));
    EXPECT_FALSE(FileVersionStore::Resolve(root,client,logical,kId2,plan));
    Clean(root);
}

TEST(v62_legacy_journal_is_located_only_in_private_state) {
    const auto root=TempRoot(L"journal");
    const auto client=root+L"\\192.168.1.106\\ABC";
    const auto logical=client+L"\\Data\\A.bin";
    std::wstring journal;
    EXPECT_TRUE(FileVersionStore::JournalPath(root,client,logical,journal));
    EXPECT_TRUE(journal.find(L"\\.fsync_state\\")!=std::wstring::npos);
    EXPECT_TRUE(journal.find(L"\\journals\\")!=std::wstring::npos);
    EXPECT_TRUE(journal.find(L".blocks")!=std::wstring::npos);
    EXPECT_FALSE(journal==logical+L".fsync.blocks");
}

TEST(file_instance_id_is_stable_for_normal_edit) {
    wchar_t temp[MAX_PATH]{};
    EXPECT_TRUE(GetTempPathW(MAX_PATH,temp)!=0);
    const std::wstring path=std::wstring(temp)+L"FileSyncIdentityTest_"+
        std::to_wstring(GetCurrentProcessId())+L".txt";
    EXPECT_TRUE(FileIo::WriteAll(path,"abc",3));
    auto a=GetFileInstanceId(path);
    EXPECT_TRUE(IsValidFileInstanceId(a));
    EXPECT_TRUE(FileIo::WriteAll(path,"abcd",4));
    EXPECT_EQ(a,GetFileInstanceId(path));
    EXPECT_TRUE(DeleteFileW(path.c_str())!=FALSE);
    Sleep(50);
    EXPECT_TRUE(FileIo::WriteAll(path,"other",5));
    const auto b=GetFileInstanceId(path);
    EXPECT_TRUE(IsValidFileInstanceId(b));
    EXPECT_NE(a,b);
    DeleteFileW(path.c_str());
}
