#include "../../Common/FilterSelection.h"
#include "../../Common/UiNetworkPath.h"
#include "../../Common/ClientPresence.h"
#include "../../Common/UiServerTreePath.h"
#include <cassert>
#include <iostream>

int main() {
    using namespace fsyn;
    auto all = ui_filter::Parse("*");
    assert(all.all);
    // V8.6.4: ALL disables but does not hide the other controls.
    auto disabled = ui_filter::GetControlState(true, true, true);
    assert(!disabled.extensionOptionsEnabled && !disabled.customEditorEnabled);
    auto editable = ui_filter::GetControlState(true, false, true);
    assert(editable.extensionOptionsEnabled && editable.customEditorEnabled);
    editable = ui_filter::GetControlState(true, false, false);
    assert(editable.extensionOptionsEnabled && !editable.customEditorEnabled);
    disabled = ui_filter::GetControlState(false, false, true);
    assert(!disabled.extensionOptionsEnabled && !disabled.customEditorEnabled);
    std::string out, err;
    assert(ui_filter::Compose(all,out,err) && out=="*");
    auto specific=ui_filter::Parse("*.XML,*.JSON,*.raw,.ini,*.RTF,*.log");
    assert(specific.xml && specific.json && specific.raw && specific.ini && specific.rtf && specific.other);
    assert(ui_filter::Compose(specific,out,err) &&
           out=="*.xml,*.json,*.raw,*.ini,*.rtf,*.log");
    // ALL wins over any checked (disabled) extension option; choices survive toggle.
    const auto original = specific;
    specific.all = true;
    assert(ui_filter::Compose(specific,out,err) && out=="*");
    const auto restored = ui_filter::DisplaySelection(specific, original);
    assert(restored.xml && restored.json && restored.raw && restored.ini && restored.rtf);
    assert(restored.other && restored.custom=="*.log");
    specific.all=false; specific.other=true; specific.custom="";
    assert(!ui_filter::Compose(specific,out,err));
    specific.custom="*.pdf,*.txt";
    assert(ui_filter::Compose(specific,out,err));
    specific.custom="*";
    assert(!ui_filter::Compose(specific,out,err));
    assert(ui_path::StandardSharePath("192.168.0.104", L"D:\\Project_Codex") ==
           L"\\\\192.168.0.104\\D\\Project_Codex");
    assert((ui_path::CandidateSharePaths("192.168.0.104", L"D:\\Project_Codex") ==
            std::vector<std::wstring>{L"\\\\192.168.0.104\\D\\Project_Codex",
                                      L"\\\\192.168.0.104\\D$\\Project_Codex"}));
    assert(ui_path::CandidateSharePaths("10.1.2.3", L"D:\\A\\..\\B").empty());
    assert(ui_path::AdministrativeSharePath("192.168.1.33", L"D:\\Data\\Test") ==
           L"\\\\192.168.1.33\\D$\\Data\\Test");
    assert(ui_path::AdministrativeSharePath("192.168.1.33", L"c:\\") ==
           L"\\\\192.168.1.33\\C$");
    assert(ui_path::AdministrativeSharePath("192.168.1.33", L"D:\\Data\\..\\Private").empty());
    assert(ui_path::AdministrativeSharePath("192.168.1.33", L"\\\\nas\\share").empty());
    assert(ui_path::AdministrativeSharePath("300.1.1.1", L"D:\\Data").empty());
    // V8.6.1: a real folder target can contain spaces and Unicode.
    assert(ui_path::AdministrativeSharePath("10.1.2.3", L"D:\\Test Data\\Th\u1eed") ==
           L"\\\\10.1.2.3\\D$\\Test Data\\Th\u1eed");
    assert(ui_path::AdministrativeSharePath("10.1.2.3", L"D:\\Test\\.\\Unsafe").empty());
    assert(ui_path::AdministrativeSharePath("10.1.2.3", L"D:\\Invalid|Name").empty());
    assert(ui_path::AdministrativeSharePath("10.1.2.3", L"D:Relative").empty());
    ClientPresence history;
    history.ReconcileDetailed({{"192.168.1.33\\Demo", "192.168.1.33", "D:\\Data"}});
    assert(history.Entries().size()==1 && history.Entries()[0].online);
    history.ReconcileDetailed({});
    assert(!history.Entries()[0].online && history.Entries()[0].watchRoot=="D:\\Data");
    history.ReconcileDetailed({{"192.168.1.33\\Demo", "192.168.1.33", "D:\\Data2"}});
    assert(history.Entries().size()==1 && history.Entries()[0].online && history.Entries()[0].watchRoot=="D:\\Data2");
    std::wstring treeRel;
    assert(ui_tree::BuildServerRelativePath({L"192.168.0.104"}, treeRel));
    assert(treeRel == L"192.168.0.104");
    assert(ui_tree::BuildServerRelativePath({L"192.168.0.104", L"Project_Codex", L"xml"}, treeRel));
    assert(treeRel == L"192.168.0.104\\Project_Codex\\xml");
    assert(!ui_tree::BuildServerRelativePath({}, treeRel));
    assert(!ui_tree::BuildServerRelativePath({L"192.168.0.104", L".."}, treeRel));
    assert(!ui_tree::BuildServerRelativePath({L"192.168.0.104", L"C:\\Danger"}, treeRel));
    assert(!ui_tree::BuildServerRelativePath({L"192.168.0.104", L"folder/evil"}, treeRel));
    assert(!ui_tree::BuildServerRelativePath({L"192.168.0.104", L"trailing."}, treeRel));
    assert(ui_tree::BuildServerRelativePath({L"192.168.0.104", L"Dữ liệu"}, treeRel));
    std::cout << "PASS: V8.6.4 UI Server Tree + ALL disable/preserve + HostKey SMB" << std::endl;
}
