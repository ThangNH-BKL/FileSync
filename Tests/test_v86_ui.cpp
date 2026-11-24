#include "framework.h"
#include "MiniTest.h"
#include "FilterSelection.h"
#include "UiNetworkPath.h"
#include "ClientPresence.h"
#include "UiServerTreePath.h"

TEST(v86_filter_existing_all) {
    const auto s = fsyn::ui_filter::Parse("*");
    EXPECT_TRUE(s.all);
    EXPECT_FALSE(s.xml);
    std::string out, error;
    EXPECT_TRUE(fsyn::ui_filter::Compose(s, out, error));
    EXPECT_EQ(out, std::string("*"));
}
TEST(v864_filter_all_disables_presets_and_preserves_choices) {
    using namespace fsyn::ui_filter;
    auto s = Parse("*.xml,*.json,*.raw,*.ini,*.rtf,*.log");
    EXPECT_TRUE(GetControlState(true, false, true).extensionOptionsEnabled);
    EXPECT_TRUE(GetControlState(true, false, true).customEditorEnabled);
    EXPECT_FALSE(GetControlState(true, true, true).extensionOptionsEnabled);
    EXPECT_FALSE(GetControlState(true, true, true).customEditorEnabled);
    EXPECT_FALSE(GetControlState(false, false, true).extensionOptionsEnabled);
    EXPECT_FALSE(GetControlState(true, false, false).customEditorEnabled);
    const auto prior = s;
    s.all = true;
    std::string out, err;
    EXPECT_TRUE(Compose(s, out, err));
    EXPECT_EQ(out, std::string("*"));
    const auto restored = DisplaySelection(s, prior);
    EXPECT_TRUE(restored.xml && restored.json && restored.raw);
    EXPECT_TRUE(restored.ini && restored.rtf && restored.other);
    EXPECT_EQ(restored.custom, std::string("*.log"));
}
TEST(v86_filter_presets_and_legacy_custom) {
    const auto s = fsyn::ui_filter::Parse("*.XML,*.json,.raw,*.ini,*.RTF,*.csv");
    EXPECT_TRUE(s.xml); EXPECT_TRUE(s.json); EXPECT_TRUE(s.raw);
    EXPECT_TRUE(s.ini); EXPECT_TRUE(s.rtf); EXPECT_TRUE(s.other);
    EXPECT_EQ(s.custom, std::string("*.csv"));
    std::string out, error;
    EXPECT_TRUE(fsyn::ui_filter::Compose(s, out, error));
    EXPECT_EQ(out, std::string("*.xml,*.json,*.raw,*.ini,*.rtf,*.csv"));
}
TEST(v86_filter_empty_not_accidental_all) {
    fsyn::ui_filter::Selection s;
    std::string out, error;
    EXPECT_FALSE(fsyn::ui_filter::Compose(s, out, error));
    EXPECT_FALSE(error.empty());
    s.other = true;
    EXPECT_FALSE(fsyn::ui_filter::Compose(s, out, error));
    s.custom = "*.csv,*.exe";
    EXPECT_TRUE(fsyn::ui_filter::Compose(s, out, error));
    EXPECT_EQ(out, std::string("*.csv,*.exe"));
    s.custom = "*";
    EXPECT_FALSE(fsyn::ui_filter::Compose(s, out, error));
}
TEST(v862_prefer_normal_share_then_optional_admin) {
    using fsyn::ui_path::StandardSharePath;
    using fsyn::ui_path::CandidateSharePaths;
    EXPECT_EQ(StandardSharePath("192.168.0.104", L"D:\\Project_Codex"),
              std::wstring(L"\\\\192.168.0.104\\D\\Project_Codex"));
    const auto candidates = CandidateSharePaths("192.168.0.104", L"D:\\Project_Codex");
    EXPECT_EQ(candidates.size(), size_t(2));
    if (candidates.size() == 2) {
        EXPECT_EQ(candidates[0], std::wstring(L"\\\\192.168.0.104\\D\\Project_Codex"));
        EXPECT_EQ(candidates[1], std::wstring(L"\\\\192.168.0.104\\D$\\Project_Codex"));
    }
    EXPECT_TRUE(CandidateSharePaths("192.168.0.104", L"D:\\A\\..\\B").empty());
    EXPECT_TRUE(CandidateSharePaths("999.0.0.1", L"D:\\Data").empty());
}
TEST(v86_share_path) {
    using fsyn::ui_path::AdministrativeSharePath;
    EXPECT_EQ(AdministrativeSharePath("192.168.1.15", L"D:\\Data\\Project"),
              std::wstring(L"\\\\192.168.1.15\\D$\\Data\\Project"));
    EXPECT_EQ(AdministrativeSharePath("10.0.0.5", L"c:\\"),
              std::wstring(L"\\\\10.0.0.5\\C$"));
    EXPECT_TRUE(AdministrativeSharePath("10.0.0.5", L"..\\Data").empty());
    EXPECT_TRUE(AdministrativeSharePath("10.0.0.5", L"D:\\A\\..\\B").empty());
    EXPECT_TRUE(AdministrativeSharePath("10.0.0.5", L"\\\\nas\\share").empty());
    EXPECT_TRUE(AdministrativeSharePath("10.0.0.5", L"D:\\x\" /foo").empty());
    EXPECT_TRUE(AdministrativeSharePath("999.0.0.5", L"C:\\Data").empty());
    // Regression checks: use the full directory as the shell target.
    EXPECT_EQ(AdministrativeSharePath("10.1.2.3", L"D:\\Test Data\\Th\u1eed"),
              std::wstring(L"\\\\10.1.2.3\\D$\\Test Data\\Th\u1eed"));
    EXPECT_TRUE(AdministrativeSharePath("10.1.2.3", L"D:\\Test\\.\\Unsafe").empty());
    EXPECT_TRUE(AdministrativeSharePath("10.1.2.3", L"D:\\Invalid|Name").empty());
}
TEST(v86_client_watchroot_preserved_after_offline) {
    fsyn::ClientPresence statuses;
    statuses.ReconcileDetailed({{"192.168.1.15\\A", "192.168.1.15", "D:\\Data"}});
    EXPECT_TRUE(statuses.Entries()[0].online);
    EXPECT_EQ(statuses.Entries()[0].watchRoot, std::string("D:\\Data"));
    statuses.ReconcileDetailed({});
    EXPECT_FALSE(statuses.Entries()[0].online);
    EXPECT_EQ(statuses.Entries()[0].watchRoot, std::string("D:\\Data"));
    statuses.ReconcileDetailed({{"192.168.1.15\\A", "192.168.1.15", "D:\\Data2"}});
    EXPECT_EQ(statuses.Entries()[0].watchRoot, std::string("D:\\Data2"));
}

TEST(v863_tree_ip_targets_server_relative_folder) {
    std::wstring rel;
    EXPECT_TRUE(fsyn::ui_tree::BuildServerRelativePath({L"192.168.0.104"}, rel));
    EXPECT_EQ(rel, std::wstring(L"192.168.0.104"));
    EXPECT_TRUE(fsyn::ui_tree::BuildServerRelativePath(
        {L"192.168.0.104", L"Project_Codex", L"xml", L"0001"}, rel));
    EXPECT_EQ(rel, std::wstring(L"192.168.0.104\\Project_Codex\\xml\\0001"));
}
TEST(v863_tree_rejects_unsafe_components) {
    std::wstring rel;
    EXPECT_FALSE(fsyn::ui_tree::BuildServerRelativePath({}, rel));
    EXPECT_FALSE(fsyn::ui_tree::BuildServerRelativePath({L"192.168.0.104", L".."}, rel));
    EXPECT_FALSE(fsyn::ui_tree::BuildServerRelativePath({L"192.168.0.104", L"D:\\x"}, rel));
    EXPECT_FALSE(fsyn::ui_tree::BuildServerRelativePath({L"192.168.0.104", L"data/evil"}, rel));
    EXPECT_FALSE(fsyn::ui_tree::BuildServerRelativePath({L"192.168.0.104", L"data."}, rel));
    EXPECT_TRUE(fsyn::ui_tree::BuildServerRelativePath({L"192.168.0.104", L"Dữ liệu"}, rel));
}
