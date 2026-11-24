#include "pch.h"
#include "CFServerDlg.h"
#include "resource.h"
#include "Util.h"
#include "FilterSelection.h"
#include "ServerLimitSettings.h"
#include "UiNetworkPath.h"
#include "UiServerTreePath.h"
#include "PathSanitizer.h"
#include <shlobj.h>
#include <shellapi.h>
#include <thread>
#include <ctime>
#include <cwchar>
#include <chrono>
#include <algorithm>
#include <system_error>
#pragma comment(lib, "Shell32.lib")

static UINT WM_TASKBARCREATED = ::RegisterWindowMessageW(L"TaskbarCreated");
#define WM_TRAY_ICON (WM_APP + 400)
#define WM_TRAY_STARTUP (WM_APP + 401)
#define TRAY_CMD_OPEN 410
#define TRAY_CMD_TOGGLE 411
#define TRAY_CMD_EXIT 412

using namespace fsyn;

#define WM_APP_LOG_MSG       (WM_APP + 101)
#define WM_APP_STATE_MSG     (WM_APP + 102)
#define WM_APP_CLIENTLIST    (WM_APP + 103)
#define WM_APP_TREEUPD       (WM_APP + 104)
#define WM_APP_STOP_COMPLETE (WM_APP + 105)
#define WM_APP_CLIENT_FOLDER_RESULT (WM_APP + 106)

CFServerDlg::CFServerDlg(CWnd* pParent)
    : CDialogEx(IDD_FILESERVER_DIALOG, pParent) {}

CFServerDlg::~CFServerDlg() {
    if (engine_) { engine_->Stop(); engine_.reset(); }
}

void CFServerDlg::DoDataExchange(CDataExchange* pDX) {
    CDialogEx::DoDataExchange(pDX);
    DDX_Control(pDX, IDC_CHECK_SCHEDULE, scheduleCheckbox_);
    DDX_Control(pDX, IDC_DTP_START_DATE, scheduleStartDate_);
    DDX_Control(pDX, IDC_DTP_START_TIME, scheduleStartTime_);
    DDX_Control(pDX, IDC_DTP_END_DATE, scheduleEndDate_);
    DDX_Control(pDX, IDC_DTP_END_TIME, scheduleEndTime_);
    DDX_Control(pDX, IDC_LIST_CLIENTS, listClients_);
    DDX_Control(pDX, IDC_TREE_FILES, treeFiles_);
    DDX_Control(pDX, IDC_PROGRESS_FILES, progressFiles_);
    DDX_Control(pDX, IDC_LIST_LOG, listLog_);
}

BEGIN_MESSAGE_MAP(CFServerDlg, CDialogEx)
    ON_BN_CLICKED(IDC_BTN_START, &CFServerDlg::OnBnClickedBtnStart)
    ON_BN_CLICKED(IDC_BTN_STOP, &CFServerDlg::OnBnClickedBtnStop)
    ON_BN_CLICKED(IDC_BTN_BROWSE, &CFServerDlg::OnBnClickedBtnBrowse)
    ON_BN_CLICKED(IDC_CHECK_SCHEDULE, &CFServerDlg::OnBnClickedScheduleToggle)
    ON_CONTROL_RANGE(BN_CLICKED, IDC_CHECK_FILTER_XML, IDC_CHECK_FILTER_ALL, &CFServerDlg::OnBnClickedFilterOption)
    ON_NOTIFY(NM_DBLCLK, IDC_LIST_CLIENTS, &CFServerDlg::OnNmDblClickClients)
    ON_NOTIFY(NM_DBLCLK, IDC_TREE_FILES, &CFServerDlg::OnNmDblClickTree)
    ON_WM_CLOSE()
    ON_WM_DESTROY()
    ON_MESSAGE(WM_TRAY_ICON, &CFServerDlg::OnTrayIcon)
    ON_MESSAGE(WM_TRAY_STARTUP, &CFServerDlg::OnTrayStartup)
    ON_REGISTERED_MESSAGE(WM_TASKBARCREATED, &CFServerDlg::OnTaskbarCreated)
    ON_MESSAGE(WM_APP_LOG_MSG, &CFServerDlg::OnAppLog)
    ON_MESSAGE(WM_APP_STATE_MSG, &CFServerDlg::OnAppState)
    ON_MESSAGE(WM_APP_CLIENTLIST, &CFServerDlg::OnAppClientList)
    ON_MESSAGE(WM_APP_TREEUPD, &CFServerDlg::OnAppTreeUpdate)
    ON_MESSAGE(WM_APP_STOP_COMPLETE, &CFServerDlg::OnAppStopComplete)
    ON_MESSAGE(WM_APP_CLIENT_FOLDER_RESULT, &CFServerDlg::OnAppClientFolderResult)
    ON_WM_TIMER()
END_MESSAGE_MAP()

BOOL CFServerDlg::OnInitDialog() {
    CDialogEx::OnInitDialog();
    SetWindowText(_T("FileServer V8.6.5 - File Sync"));

    // The status icon is shown beside HostKey; the final column also shows
    // Online/Offline in text (useful when the user cannot distinguish colors).
    listClients_.InsertColumn(0, _T("HostKey (IP | client full path)"), LVCFMT_LEFT, 390);
    listClients_.InsertColumn(1, _T("IP"), LVCFMT_LEFT, 88);
    listClients_.InsertColumn(2, _T("Status"), LVCFMT_LEFT, 66);
    listClients_.SetExtendedStyle(listClients_.GetExtendedStyle() |
        LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
    BuildClientStatusIcons();
    clientLastBlinkMs_ = ::GetTickCount64();
    clientLastPollMs_ = clientLastBlinkMs_;

    scheduleStartDate_.SetFormat(L"dd/MM/yyyy");
    scheduleEndDate_.SetFormat(L"dd/MM/yyyy");
    scheduleStartTime_.SetFormat(L"HH:mm:ss");
    scheduleEndTime_.SetFormat(L"HH:mm:ss");
    SetScheduleDefaults();

    // Tree root
    treeRoot_ = treeFiles_.InsertItem(_T("Clients"));
    treeFiles_.SetItemData(treeRoot_, 0);
    treeNodeCache_.clear();
    progressFiles_.SetRange32(0, 1000);
    progressFiles_.SetPos(0);
    RefreshTransferProgress();
    SetTimer(kTreeTimerId, 250, nullptr);

    // Giới hạn log
    listLog_.InitStorage(5000, 256);

    // Numeric fields use explicit bounds in addition to ES_NUMBER, not _wtoi.
    SendDlgItemMessage(IDC_EDIT_MAX_CLIENTS, EM_LIMITTEXT, 4, 0);
    SendDlgItemMessage(IDC_EDIT_ACTIVE_TRANSFERS, EM_LIMITTEXT, 2, 0);
    LoadSettings();
    EnableConfig(true);
    UpdateStatus(_T("Status: Idle"));
    taskbarCreatedMsg_ = WM_TASKBARCREATED;
    InstallTrayIcon();
    // Defer auto-start and hiding until the dialog has finished initializing.
    PostMessage(WM_TRAY_STARTUP, 0, 0);
    return TRUE;
}

void CFServerDlg::EnableConfig(bool enabled) {
    GetDlgItem(IDC_EDIT_PORT)->EnableWindow(enabled);
    GetDlgItem(IDC_EDIT_TOKEN)->EnableWindow(enabled);
    GetDlgItem(IDC_EDIT_MAX_CLIENTS)->EnableWindow(enabled);
    GetDlgItem(IDC_EDIT_ACTIVE_TRANSFERS)->EnableWindow(enabled);
    GetDlgItem(IDC_EDIT_ROOT)->EnableWindow(enabled);
    GetDlgItem(IDC_BTN_BROWSE)->EnableWindow(enabled);
    const UINT filterIds[] = { IDC_CHECK_FILTER_XML, IDC_CHECK_FILTER_JSON,
        IDC_CHECK_FILTER_RAW, IDC_CHECK_FILTER_INI, IDC_CHECK_FILTER_RTF,
        IDC_CHECK_FILTER_OTHER, IDC_CHECK_FILTER_ALL };
    for (const UINT id : filterIds) GetDlgItem(id)->EnableWindow(enabled);
    UpdateFilterControls();
    scheduleCheckbox_.EnableWindow(enabled);
    const bool dateEnabled = enabled && scheduleCheckbox_.GetCheck() == BST_CHECKED;
    scheduleStartDate_.EnableWindow(dateEnabled);
    scheduleStartTime_.EnableWindow(dateEnabled);
    scheduleEndDate_.EnableWindow(dateEnabled);
    scheduleEndTime_.EnableWindow(dateEnabled);
    GetDlgItem(IDC_BTN_START)->EnableWindow(enabled);
    GetDlgItem(IDC_BTN_STOP)->EnableWindow(!enabled);
}

void CFServerDlg::LoadSettings() {
    CWinApp* app = AfxGetApp();
    GetDlgItem(IDC_EDIT_PORT)->SetWindowTextW(app->GetProfileString(L"Server", L"Port", L"9000"));
    GetDlgItem(IDC_EDIT_TOKEN)->SetWindowTextW(app->GetProfileString(L"Server", L"Token", L"secret"));
    // Values saved by older versions are absent: restore the tested defaults.
    const auto readLimit = [&](LPCWSTR key, LPCWSTR fallback, unsigned lo, unsigned hi, UINT id) {
        CStringW stored = app->GetProfileString(L"Server", key, fallback);
        unsigned value = 0;
        if (!fsyn::ui_server_limits::Parse(static_cast<LPCWSTR>(stored), lo, hi, value))
            stored = fallback;
        GetDlgItem(id)->SetWindowTextW(stored);
    };
    readLimit(L"MaxClients", L"512", fsyn::ui_server_limits::kMinClients,
              fsyn::ui_server_limits::kMaxClients, IDC_EDIT_MAX_CLIENTS);
    readLimit(L"MaxActiveTransfers", L"12", fsyn::ui_server_limits::kMinTransfers,
              fsyn::ui_server_limits::kMaxTransfers, IDC_EDIT_ACTIVE_TRANSFERS);
    CString root = app->GetProfileString(L"Server", L"Root", L"");
    GetDlgItem(IDC_EDIT_ROOT)->SetWindowTextW(root);
    rootUtf16_ = (LPCWSTR)root;
    const CStringW savedFilter = app->GetProfileString(L"Server", L"Filter", L"*");
    const auto parsed = fsyn::ui_filter::Parse(fsyn::WideToUtf8(static_cast<LPCWSTR>(savedFilter)));
    // Keep the previous extension choices separately from the active ALL filter.
    // A saved '*' must not destroy the user's XML/JSON/RAW/... selections.
    const CStringW savedDraft = app->GetProfileString(L"Server", L"FilterLastSelection", L"");
    const auto prior = fsyn::ui_filter::Parse(fsyn::WideToUtf8(static_cast<LPCWSTR>(savedDraft)));
    const auto displayed = savedDraft.IsEmpty() ? parsed : fsyn::ui_filter::DisplaySelection(parsed, prior);
    CheckDlgButton(IDC_CHECK_FILTER_XML, displayed.xml ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(IDC_CHECK_FILTER_JSON, displayed.json ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(IDC_CHECK_FILTER_RAW, displayed.raw ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(IDC_CHECK_FILTER_INI, displayed.ini ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(IDC_CHECK_FILTER_RTF, displayed.rtf ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(IDC_CHECK_FILTER_OTHER, displayed.other ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(IDC_CHECK_FILTER_ALL, parsed.all ? BST_CHECKED : BST_UNCHECKED);
    GetDlgItem(IDC_EDIT_FILTER)->SetWindowTextW(fsyn::Utf8ToWide(displayed.custom).c_str());
    UpdateFilterControls();
    // New one-shot schedule settings. Legacy Window/Cycle settings are intentionally ignored.
    scheduleCheckbox_.SetCheck(app->GetProfileInt(L"ScheduleOnce", L"Enabled", 0) ? BST_CHECKED : BST_UNCHECKED);
    auto loadTime = [&](LPCWSTR key, CDateTimeCtrl& date, CDateTimeCtrl& time) {
        CString saved = app->GetProfileString(L"ScheduleOnce", key, L"");
        int year=0, month=0, day=0, hour=0, minute=0, second=0;
        if (swscanf_s(static_cast<LPCWSTR>(saved), L"%d-%d-%d %d:%d:%d", &year, &month, &day, &hour, &minute, &second) != 6)
            return;
        std::tm local{};
        local.tm_year = year - 1900; local.tm_mon = month - 1; local.tm_mday = day;
        local.tm_hour = hour; local.tm_min = minute; local.tm_sec = second;
        local.tm_isdst = -1;
        std::time_t epoch = std::mktime(&local);
        if (epoch == static_cast<std::time_t>(-1)) return;
        std::tm verify{};
        if (localtime_s(&verify, &epoch) != 0 || verify.tm_year != year-1900 ||
            verify.tm_mon != month-1 || verify.tm_mday != day ||
            verify.tm_hour != hour || verify.tm_min != minute || verify.tm_sec != second) return;
        PutPickerDateTime(date, time, std::chrono::system_clock::from_time_t(epoch));
    };
    loadTime(L"StartLocal", scheduleStartDate_, scheduleStartTime_);
    loadTime(L"EndLocal", scheduleEndDate_, scheduleEndTime_);
    UpdateScheduleControls();
}

void CFServerDlg::SaveSettings() {
    CWinApp* app = AfxGetApp();
    CString s;
    GetDlgItem(IDC_EDIT_PORT)->GetWindowTextW(s);        app->WriteProfileString(L"Server", L"Port", s);
    GetDlgItem(IDC_EDIT_TOKEN)->GetWindowTextW(s);       app->WriteProfileString(L"Server", L"Token", s);
    GetDlgItem(IDC_EDIT_MAX_CLIENTS)->GetWindowTextW(s); app->WriteProfileString(L"Server", L"MaxClients", s);
    GetDlgItem(IDC_EDIT_ACTIVE_TRANSFERS)->GetWindowTextW(s); app->WriteProfileString(L"Server", L"MaxActiveTransfers", s);
    GetDlgItem(IDC_EDIT_ROOT)->GetWindowTextW(s);        app->WriteProfileString(L"Server", L"Root", s);
    CStringW filter, error;
    if (BuildSelectedFilter(filter, error)) app->WriteProfileString(L"Server", L"Filter", filter);
    // Persist the previous explicit choices even when '*' is currently active.
    fsyn::ui_filter::Selection draft;
    draft.xml = IsDlgButtonChecked(IDC_CHECK_FILTER_XML) == BST_CHECKED;
    draft.json = IsDlgButtonChecked(IDC_CHECK_FILTER_JSON) == BST_CHECKED;
    draft.raw = IsDlgButtonChecked(IDC_CHECK_FILTER_RAW) == BST_CHECKED;
    draft.ini = IsDlgButtonChecked(IDC_CHECK_FILTER_INI) == BST_CHECKED;
    draft.rtf = IsDlgButtonChecked(IDC_CHECK_FILTER_RTF) == BST_CHECKED;
    draft.other = IsDlgButtonChecked(IDC_CHECK_FILTER_OTHER) == BST_CHECKED;
    CStringW custom;
    GetDlgItem(IDC_EDIT_FILTER)->GetWindowTextW(custom);
    draft.custom = fsyn::WideToUtf8(static_cast<LPCWSTR>(custom));
    std::string draftSpec, draftError;
    if (fsyn::ui_filter::Compose(draft, draftSpec, draftError)) {
        app->WriteProfileString(L"Server", L"FilterLastSelection", fsyn::Utf8ToWide(draftSpec).c_str());
    }
    app->WriteProfileInt(L"ScheduleOnce", L"Enabled", scheduleCheckbox_.GetCheck() == BST_CHECKED ? 1 : 0);
    auto saveTime = [&](LPCWSTR key, const CDateTimeCtrl& date, const CDateTimeCtrl& time) {
        SYSTEMTIME d{}, t{};
        if (date.GetTime(&d) != GDT_VALID || time.GetTime(&t) != GDT_VALID) return;
        CString value;
        value.Format(L"%04u-%02u-%02u %02u:%02u:%02u",
            static_cast<unsigned>(d.wYear), static_cast<unsigned>(d.wMonth),
            static_cast<unsigned>(d.wDay), static_cast<unsigned>(t.wHour),
            static_cast<unsigned>(t.wMinute), static_cast<unsigned>(t.wSecond));
        app->WriteProfileString(L"ScheduleOnce", key, value);
    };
    saveTime(L"StartLocal", scheduleStartDate_, scheduleStartTime_);
    saveTime(L"EndLocal", scheduleEndDate_, scheduleEndTime_);
}

void CFServerDlg::UpdateFilterControls() {
    const bool enabled = !running_ && !stopping_ && !scheduleWaiting_;
    const bool other = IsDlgButtonChecked(IDC_CHECK_FILTER_OTHER) == BST_CHECKED;
    const bool all = IsDlgButtonChecked(IDC_CHECK_FILTER_ALL) == BST_CHECKED;
    const auto controlState = fsyn::ui_filter::GetControlState(enabled, all, other);
    // ALL (*) keeps all options visible, but locks them for both mouse and keyboard.
    // Do not erase checked states; they are restored as soon as ALL is unchecked.
    const UINT extensionIds[] = { IDC_CHECK_FILTER_XML, IDC_CHECK_FILTER_JSON,
        IDC_CHECK_FILTER_RAW, IDC_CHECK_FILTER_INI, IDC_CHECK_FILTER_RTF,
        IDC_CHECK_FILTER_OTHER };
    for (const UINT id : extensionIds) {
        if (auto* control = GetDlgItem(id)) {
            control->ShowWindow(SW_SHOW);
            control->EnableWindow(controlState.extensionOptionsEnabled);
        }
    }
    if (auto* edit = GetDlgItem(IDC_EDIT_FILTER)) {
        edit->ShowWindow(SW_SHOW);
        edit->EnableWindow(controlState.customEditorEnabled);
    }
}

void CFServerDlg::OnBnClickedFilterOption(UINT nID) {
    // Other options cannot be clicked when ALL is checked, so no need to
    // forcibly clear their state. Keep them for the next time ALL is off.
    if (nID != IDC_CHECK_FILTER_ALL) {
        CheckDlgButton(IDC_CHECK_FILTER_ALL, BST_UNCHECKED);
    }
    UpdateFilterControls();
}

bool CFServerDlg::BuildSelectedFilter(CStringW& result, CStringW& validationMessage) {
    fsyn::ui_filter::Selection selected;
    selected.all = IsDlgButtonChecked(IDC_CHECK_FILTER_ALL) == BST_CHECKED;
    selected.xml = IsDlgButtonChecked(IDC_CHECK_FILTER_XML) == BST_CHECKED;
    selected.json = IsDlgButtonChecked(IDC_CHECK_FILTER_JSON) == BST_CHECKED;
    selected.raw = IsDlgButtonChecked(IDC_CHECK_FILTER_RAW) == BST_CHECKED;
    selected.ini = IsDlgButtonChecked(IDC_CHECK_FILTER_INI) == BST_CHECKED;
    selected.rtf = IsDlgButtonChecked(IDC_CHECK_FILTER_RTF) == BST_CHECKED;
    selected.other = IsDlgButtonChecked(IDC_CHECK_FILTER_OTHER) == BST_CHECKED;
    CStringW custom;
    GetDlgItem(IDC_EDIT_FILTER)->GetWindowTextW(custom);
    selected.custom = fsyn::WideToUtf8(static_cast<LPCWSTR>(custom));
    std::string out, error;
    if (!fsyn::ui_filter::Compose(selected, out, error)) {
        validationMessage = fsyn::Utf8ToWide(error).c_str();
        return false;
    }
    result = fsyn::Utf8ToWide(out).c_str();
    return true;
}

void CFServerDlg::OnBnClickedBtnBrowse() {
    BROWSEINFOW bi{};
    bi.lpszTitle = L"Chọn thư mục gốc lưu file";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
    if (pidl) {
        wchar_t path[MAX_PATH] = { 0 };
        if (SHGetPathFromIDListW(pidl, path)) {
            GetDlgItem(IDC_EDIT_ROOT)->SetWindowTextW(path);
            rootUtf16_ = path;
        }
        CoTaskMemFree(pidl);
    }
}

void CFServerDlg::OnBnClickedScheduleToggle() {
    UpdateScheduleControls();
}

void CFServerDlg::UpdateScheduleControls() {
    const bool active = !running_ && !stopping_ && !scheduleWaiting_;
    const bool enabled = active && scheduleCheckbox_.GetCheck() == BST_CHECKED;
    scheduleStartDate_.EnableWindow(enabled);
    scheduleStartTime_.EnableWindow(enabled);
    scheduleEndDate_.EnableWindow(enabled);
    scheduleEndTime_.EnableWindow(enabled);
}

void CFServerDlg::SetScheduleDefaults() {
    auto now = std::chrono::system_clock::now();
    PutPickerDateTime(scheduleStartDate_, scheduleStartTime_, now + std::chrono::minutes(1));
    PutPickerDateTime(scheduleEndDate_, scheduleEndTime_, now + std::chrono::hours(1) + std::chrono::minutes(1));
}

bool CFServerDlg::PutPickerDateTime(CDateTimeCtrl& datePicker, CDateTimeCtrl& timePicker,
    fsyn::AbsoluteScheduleWindow::Clock::time_point when) {
    std::time_t raw = std::chrono::system_clock::to_time_t(when);
    std::tm local{};
    if (localtime_s(&local, &raw) != 0) return false;
    SYSTEMTIME data{};
    data.wYear   = static_cast<WORD>(local.tm_year + 1900);
    data.wMonth  = static_cast<WORD>(local.tm_mon + 1);
    data.wDay    = static_cast<WORD>(local.tm_mday);
    data.wHour   = static_cast<WORD>(local.tm_hour);
    data.wMinute = static_cast<WORD>(local.tm_min);
    data.wSecond = static_cast<WORD>(local.tm_sec);
    return datePicker.SetTime(&data) && timePicker.SetTime(&data);
}

bool CFServerDlg::ReadPickerDateTime(const CDateTimeCtrl& datePicker,
    const CDateTimeCtrl& timePicker,
    fsyn::AbsoluteScheduleWindow::Clock::time_point& out) const {
    SYSTEMTIME date{}, time{};
    if (datePicker.GetTime(&date) != GDT_VALID || timePicker.GetTime(&time) != GDT_VALID)
        return false;
    std::tm local{};
    local.tm_year = static_cast<int>(date.wYear) - 1900;
    local.tm_mon = static_cast<int>(date.wMonth) - 1;
    local.tm_mday = static_cast<int>(date.wDay);
    local.tm_hour = static_cast<int>(time.wHour);
    local.tm_min = static_cast<int>(time.wMinute);
    local.tm_sec = static_cast<int>(time.wSecond);
    local.tm_isdst = -1;
    std::time_t t = std::mktime(&local);
    if (t == static_cast<std::time_t>(-1)) return false;
    std::tm back{};
    if (localtime_s(&back, &t) != 0) return false;
    // Reject nonexistent local clock times (e.g. a DST jump).
    if (back.tm_year != static_cast<int>(date.wYear)-1900 ||
        back.tm_mon != static_cast<int>(date.wMonth)-1 || back.tm_mday != date.wDay ||
        back.tm_hour != time.wHour || back.tm_min != time.wMinute || back.tm_sec != time.wSecond)
        return false;
    out = std::chrono::system_clock::from_time_t(t);
    return true;
}

void CFServerDlg::OnBnClickedBtnStart() {
    BeginSchedule(false);
}

void CFServerDlg::BeginSchedule(bool autoLaunch) {
    if (running_ || scheduleWaiting_ || stopping_ || exiting_) return;

    CString sPort, sToken, sRoot;
    CStringW sFilter, filterError;
    GetDlgItem(IDC_EDIT_PORT)->GetWindowTextW(sPort);
    GetDlgItem(IDC_EDIT_TOKEN)->GetWindowTextW(sToken);
    GetDlgItem(IDC_EDIT_ROOT)->GetWindowTextW(sRoot);
    if (!BuildSelectedFilter(sFilter, filterError)) {
        if (!autoLaunch) MessageBox(filterError, L"File type filter", MB_ICONERROR);
        else UpdateStatus(L"Status: Invalid file extension filter");
        return;
    }
    if (sRoot.IsEmpty() || _wtoi(sPort) <= 0 || _wtoi(sPort) > 65535 || sToken.IsEmpty()) {
        if (!autoLaunch) MessageBox(L"Please configure a valid port, token and root folder.", L"FileServer", MB_ICONERROR);
        else UpdateStatus(L"Status: Configure Server before starting");
        return;
    }
    CStringW clientLimitText, transferLimitText;
    GetDlgItem(IDC_EDIT_MAX_CLIENTS)->GetWindowTextW(clientLimitText);
    GetDlgItem(IDC_EDIT_ACTIVE_TRANSFERS)->GetWindowTextW(transferLimitText);
    unsigned maxClients = 0, maxTransfers = 0;
    if (!fsyn::ui_server_limits::Parse(static_cast<LPCWSTR>(clientLimitText),
                                      fsyn::ui_server_limits::kMinClients,
                                      fsyn::ui_server_limits::kMaxClients, maxClients) ||
        !fsyn::ui_server_limits::Parse(static_cast<LPCWSTR>(transferLimitText),
                                      fsyn::ui_server_limits::kMinTransfers,
                                      fsyn::ui_server_limits::kMaxTransfers, maxTransfers)) {
        if (!autoLaunch) MessageBox(L"Max clients must be 1-4096 and Active files must be 1-64.",
                                    L"Invalid server limits", MB_ICONERROR);
        else UpdateStatus(L"Status: Invalid client/active-file limits in Server settings");
        return;
    }

    // The old recurring Window/Cycle scheduler is no longer selected from the UI.
    // When the engine is running, its internal gate must be open 24/7;
    // absolute start/stop is enforced by the UI timer around ServerEngine lifetime.
    fsyn::ServerEngine::Config cfg;
    cfg.port = static_cast<uint16_t>(_wtoi(sPort));
    cfg.token = CStringA(sToken);
    cfg.serverRootUtf16 = static_cast<LPCWSTR>(sRoot);
    cfg.extFilterSpec = fsyn::WideToUtf8(static_cast<LPCWSTR>(sFilter));
    cfg.maxClients = maxClients;
    cfg.maxActiveTransfers = maxTransfers;
    cfg.limitsFromDialog = true; // GUI values win over legacy FILESYNC_MAX_CLIENTS / ACTIVE_TRANSFERS
    cfg.sched.mode = fsyn::SchedMode::Window;
    cfg.sched.windowStartMin = 0;
    cfg.sched.windowEndMin = 0;

    fsyn::AbsoluteScheduleWindow window{};
    const bool enableSchedule = scheduleCheckbox_.GetCheck() == BST_CHECKED;
    if (enableSchedule) {
        if (!ReadPickerDateTime(scheduleStartDate_, scheduleStartTime_, window.start) ||
            !ReadPickerDateTime(scheduleEndDate_, scheduleEndTime_, window.end) ||
            !window.IsValid()) {
            if (!autoLaunch) MessageBox(L"End date/time must be later than Start date/time.", L"Schedule error", MB_ICONERROR);
            else UpdateStatus(L"Status: Invalid schedule");
            return;
        }
        if (window.PhaseAt(std::chrono::system_clock::now()) == fsyn::AbsoluteSchedulePhase::Finished) {
            UpdateStatus(L"Status: Schedule expired - select new date/time");
            if (!autoLaunch) MessageBox(L"The selected end date/time is already past.", L"Schedule expired", MB_ICONWARNING);
            return;
        }
    }

    queuedEngineConfig_ = std::move(cfg);
    rootUtf16_ = static_cast<LPCWSTR>(sRoot);
    scheduledWindow_ = window;
    scheduleArmed_ = enableSchedule;
    scheduleWaiting_ = false;
    scheduleStoppedAtEnd_ = false;
    SaveSettings();

    if (enableSchedule && window.PhaseAt(std::chrono::system_clock::now()) == fsyn::AbsoluteSchedulePhase::Waiting) {
        scheduleWaiting_ = true;
        EnableConfig(false);
        UpdateStatus(L"Status: Scheduled (waiting for start time)");
        AppendLog("Schedule armed: waiting for start date and time");
        return;
    }
    if (!StartEngineNow()) {
        scheduleArmed_ = false;
        EnableConfig(true);
    }
}

bool CFServerDlg::StartEngineNow() {
    if (running_ || stopping_ || exiting_) return false;
    engine_ = std::make_unique<fsyn::ServerEngine>(queuedEngineConfig_, this);
    if (!engine_->Start()) {
        engine_.reset();
        UpdateStatus(L"Status: Start failed");
        AppendLog("Server failed to start; check port and server log");
        return false;
    }
    running_ = true;
    scheduleWaiting_ = false;
    progressInitialized_ = false;
    RefreshTransferProgress();
    EnableConfig(false);
    UpdateStatus(scheduleArmed_ ? L"Status: Running (scheduled)" : L"Status: Running");
    AppendLog("Server started.");
    return true;
}

void CFServerDlg::CheckSchedule() {
    if (!scheduleArmed_ || exiting_ || stopping_) return;
    const auto phase = scheduledWindow_.PhaseAt(std::chrono::system_clock::now());
    if (phase == fsyn::AbsoluteSchedulePhase::Finished) {
        if (scheduleWaiting_) {
            scheduleWaiting_ = false;
            scheduleArmed_ = false;
            EnableConfig(true);
            UpdateStatus(L"Status: Schedule finished without starting");
            AppendLog("Schedule expired before Server started.");
        } else if (running_) {
            RequestStop(true);
        }
        return;
    }
    if (scheduleWaiting_ && phase == fsyn::AbsoluteSchedulePhase::Running) {
        scheduleWaiting_ = false;
        if (!StartEngineNow()) {
            scheduleArmed_ = false;
            EnableConfig(true);
        }
    }
}

void CFServerDlg::OnBnClickedBtnStop() {
    RequestStop(false);
}

void CFServerDlg::RequestStop(bool scheduledFinish) {
    if (scheduleWaiting_) {
        scheduleWaiting_ = false;
        scheduleArmed_ = false;
        EnableConfig(true);
        UpdateStatus(L"Status: Schedule cancelled");
        AppendLog("Schedule cancelled.");
        return;
    }
    if (stopping_ || !running_) return;
    scheduleArmed_ = false; // no auto restart after Stop, whether manual or scheduled
    scheduleStoppedAtEnd_ = scheduledFinish;
    stopping_ = true;
    running_ = false;
    GetDlgItem(IDC_BTN_STOP)->EnableWindow(FALSE);
    UpdateStatus(scheduledFinish ? L"Status: Scheduled end (stopping...)" : L"Status: Stopping...");
    auto oldEngine = std::move(engine_);
    HWND hwnd = GetSafeHwnd();
    std::thread([engine = std::move(oldEngine), hwnd]() mutable {
        if (engine) { engine->Stop(); engine.reset(); }
        ::PostMessage(hwnd, WM_APP_STOP_COMPLETE, 0, 0);
    }).detach();
}

LRESULT CFServerDlg::OnAppStopComplete(WPARAM, LPARAM) {
    stopping_ = false;
    EnableConfig(true);
    UpdateStatus(scheduleStoppedAtEnd_ ? L"Status: Completed (scheduled end)" : L"Status: Stopped");
    AppendLog(scheduleStoppedAtEnd_ ? "Server stopped at scheduled end." : "Server stopped.");
    RefreshClientList(); // Keep known clients visible, now with red indicators.
    progressInitialized_ = false;
    RefreshTransferProgress();
    scheduleStoppedAtEnd_ = false;
    if (exiting_) {
        RemoveTrayIcon();
        CDialogEx::OnClose();
    }
    return 0;
}

void CFServerDlg::OnClose() {
    if (!exiting_) {
        ShowWindow(SW_HIDE);
        return;
    }
    if (running_ || scheduleWaiting_) OnBnClickedBtnStop();
    if (stopping_) return;
    RemoveTrayIcon();
    CDialogEx::OnClose();
}

void CFServerDlg::OnDestroy() {
    // A pending SMB probe may still be blocked by an offline host. Never let
    // its background thread access the dialog or post to a destroyed window.
    {
        std::lock_guard<std::mutex> guard(folderProbe_->mu);
        folderProbe_->alive = false;
        folderProbe_->window = nullptr;
    }
    KillTimer(kTreeTimerId);
    RemoveTrayIcon();
    // The ListView does not own its small image list.
    listClients_.SetImageList(nullptr, LVSIL_SMALL);
    clientStatusIcons_.DeleteImageList();
    CDialogEx::OnDestroy();
}

void CFServerDlg::InstallTrayIcon() {
    if (trayInstalled_) return;
    trayIcon_ = {};
    trayIcon_.cbSize = sizeof(trayIcon_);
    trayIcon_.hWnd = GetSafeHwnd();
    trayIcon_.uID = 1;
    trayIcon_.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    trayIcon_.uCallbackMessage = WM_TRAY_ICON;
    trayIcon_.hIcon = ::LoadIconW(nullptr, IDI_APPLICATION);
    wcscpy_s(trayIcon_.szTip, _countof(trayIcon_.szTip), L"FileServer V8.6.5 - FileSync");
    trayInstalled_ = Shell_NotifyIconW(NIM_ADD, &trayIcon_) != FALSE;
    if (trayInstalled_) {
        trayIcon_.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &trayIcon_);
    }
}

void CFServerDlg::RemoveTrayIcon() {
    if (trayInstalled_) {
        Shell_NotifyIconW(NIM_DELETE, &trayIcon_);
        trayInstalled_ = false;
    }
}

void CFServerDlg::ShowMainWindow() {
    ShowWindow(SW_SHOW);
    ShowWindow(SW_RESTORE);
    SetForegroundWindow();
}

void CFServerDlg::ExitApplication() {
    exiting_ = true;
    OnClose();
}

void CFServerDlg::ShowTrayMenu() {
    CMenu menu;
    if (!menu.CreatePopupMenu()) return;
    menu.AppendMenuW(MF_STRING, TRAY_CMD_OPEN, L"Open " L"FileServer");
    menu.AppendMenuW(MF_STRING, TRAY_CMD_TOGGLE,
        running_ ? L"Stop sync" : (scheduleWaiting_ ? L"Cancel scheduled start" : L"Start sync"));
    menu.AppendMenuW(MF_SEPARATOR);
    menu.AppendMenuW(MF_STRING, TRAY_CMD_EXIT, L"Exit");
    POINT pt{};
    ::GetCursorPos(&pt);
    SetForegroundWindow();
    UINT cmd = menu.TrackPopupMenu(TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY,
                                   pt.x, pt.y, this);
    if (cmd == TRAY_CMD_OPEN) ShowMainWindow();
    else if (cmd == TRAY_CMD_TOGGLE) {
        if (running_ || scheduleWaiting_) OnBnClickedBtnStop();
        else OnBnClickedBtnStart();
    } else if (cmd == TRAY_CMD_EXIT) ExitApplication();
}

LRESULT CFServerDlg::OnTrayIcon(WPARAM, LPARAM lp) {
    // NOTIFYICON_VERSION_4 delivers the mouse event in LOWORD(lParam).
    const UINT eventCode = LOWORD(static_cast<DWORD_PTR>(lp));
    if (eventCode == WM_LBUTTONDBLCLK || eventCode == NIN_SELECT || eventCode == NIN_KEYSELECT)
        ShowMainWindow();
    else if (eventCode == WM_RBUTTONUP || eventCode == WM_CONTEXTMENU)
        ShowTrayMenu();
    return 0;
}

LRESULT CFServerDlg::OnTrayStartup(WPARAM, LPARAM) {
    // Hide the window and start synchronization using saved settings.
    if (trayInstalled_) ShowWindow(SW_HIDE);
    BeginSchedule(true);
    return 0;
}

LRESULT CFServerDlg::OnTaskbarCreated(WPARAM, LPARAM) {
    // Windows Explorer can restart; recreate our icon in the new tray.
    trayInstalled_ = false;
    InstallTrayIcon();
    return 0;
}

void CFServerDlg::AppendLog(const std::string& line) {
    if (listLog_.GetCount() > 5000) listLog_.DeleteString(0);
    CStringW w(line.c_str());
    int idx = listLog_.AddString(w);
    listLog_.SetTopIndex(idx);
}

void CFServerDlg::UpdateStatus(const CString& s) {
    SetDlgItemText(IDC_STATIC_STATUS, s);
}

// ---------- IUiCallbacks (worker thread) ----------
void CFServerDlg::OnLog(LogLevel, const std::string& line) {
    std::lock_guard<std::mutex> g(logQueueMu_);
    if (logQueue_.size() >= 10000) logQueue_.pop_front();
    logQueue_.push_back(line);
}
void CFServerDlg::OnConnState(ConnStateUi state) {
    PostMessage(WM_APP_STATE_MSG, (WPARAM)state, 0);
}
void CFServerDlg::OnClientListChanged() {
    PostMessage(WM_APP_CLIENTLIST, 0, 0);
}
void CFServerDlg::OnTreeUpdate(const std::string& relPath) {
    // Worker threads enqueue paths; never flood the MFC message queue.
    std::lock_guard<std::mutex> g(treeQueueMu_);
    if (treeQueue_.size() < 20000) treeQueue_.push_back(relPath);
    // Timer on the UI thread drains a bounded batch every 250 ms.
}

// ---------- Handlers UI ----------
LRESULT CFServerDlg::OnAppLog(WPARAM, LPARAM lp) {
    CString* p = (CString*)lp;
    if (p) {
        AppendLog(std::string(CStringA(*p).GetString()));
        delete p;
    }
    return 0;
}
LRESULT CFServerDlg::OnAppState(WPARAM, LPARAM) { return 0; }
LRESULT CFServerDlg::OnAppClientList(WPARAM, LPARAM) {
    RefreshClientList();
    return 0;
}
LRESULT CFServerDlg::OnAppTreeUpdate(WPARAM, LPARAM) { return 0; }

void CFServerDlg::OnTimer(UINT_PTR nIDEvent) {
    if (nIDEvent == kTreeTimerId) {
        CheckSchedule();
        RefreshTransferProgress(); // Bounded, read-only snapshot; no per-file messages.
        // Poll periodically because a session can end without an explicit
        // OnClientListChanged callback. The timer already serves UI batching.
        const ULONGLONG nowMs = ::GetTickCount64();
        if (nowMs - clientLastPollMs_ >= 1000) {
            clientLastPollMs_ = nowMs;
            RefreshClientList();
        }
        if (nowMs - clientLastBlinkMs_ >= 500) {
            clientLastBlinkMs_ = nowMs;
            clientIconsBright_ = !clientIconsBright_;
            UpdateClientStatusIcons();
        }
        std::deque<std::string> logs;
        {
            std::lock_guard<std::mutex> g(logQueueMu_);
            for (size_t i = 0; i < 100 && !logQueue_.empty(); ++i) {
                logs.push_back(std::move(logQueue_.front()));
                logQueue_.pop_front();
            }
        }
        for (const auto& line : logs) AppendLog(line);
        std::deque<std::string> batch;
        {
            std::lock_guard<std::mutex> g(treeQueueMu_);
            for (size_t i = 0; i < 200 && !treeQueue_.empty(); ++i) {
                batch.push_back(std::move(treeQueue_.front()));
                treeQueue_.pop_front();
            }
        }
        if (!batch.empty()) {
            treeFiles_.SetRedraw(FALSE);
            for (const auto& path : batch) AddTreePath(path);
            treeFiles_.SetRedraw(TRUE);
            treeFiles_.Invalidate(FALSE);
        }
        return;
    }
    CDialogEx::OnTimer(nIDEvent);
}

void CFServerDlg::RefreshTransferProgress() {
    const fsyn::TransferProgress current = engine_
        ? engine_->GetProgress() : fsyn::TransferProgress{};
    if (progressInitialized_ && current == lastProgress_) return;
    progressInitialized_ = true;
    lastProgress_ = current;
    progressFiles_.SetPos(static_cast<int>(current.Permille()));

    CStringW headline;
    headline.Format(L"Processed: %llu / %llu files (%u%%)",
        static_cast<unsigned long long>(current.completed),
        static_cast<unsigned long long>(current.Total()), current.Percent());
    SetDlgItemText(IDC_STATIC_PROGRESS, headline);

    CStringW breakdown;
    breakdown.Format(L"Updated: %llu   Unchanged: %llu   Active: %llu   Pending: %llu   Failed: %llu",
        static_cast<unsigned long long>(current.updated),
        static_cast<unsigned long long>(current.unchanged),
        static_cast<unsigned long long>(current.active),
        static_cast<unsigned long long>(current.pending),
        static_cast<unsigned long long>(current.failed));
    SetDlgItemText(IDC_STATIC_PROGRESS_DETAIL, breakdown);
}

void CFServerDlg::BuildClientStatusIcons() {
    // Index: 0 green bright, 1 green dim, 2 red bright, 3 red dim.
    if (!clientStatusIcons_.Create(16, 16, ILC_COLOR24 | ILC_MASK, 4, 1))
        return;
    CClientDC dc(this);
    CDC surface;
    if (!surface.CreateCompatibleDC(&dc)) return;
    constexpr COLORREF kTransparent = RGB(255, 0, 255);
    const COLORREF colors[] = {
        RGB(30, 210, 65), RGB(12, 82, 29),
        RGB(238, 48, 48), RGB(105, 23, 23)
    };
    for (const auto color : colors) {
        CBitmap bitmap;
        if (!bitmap.CreateCompatibleBitmap(&dc, 16, 16)) break;
        CBitmap* oldBitmap = surface.SelectObject(&bitmap);
        surface.FillSolidRect(0, 0, 16, 16, kTransparent);
        CPen outline(PS_SOLID, 1, RGB(48, 48, 48));
        CBrush fill(color);
        CPen* oldPen = surface.SelectObject(&outline);
        CBrush* oldBrush = surface.SelectObject(&fill);
        surface.Ellipse(2, 2, 14, 14);
        surface.SelectObject(oldBrush);
        surface.SelectObject(oldPen);
        clientStatusIcons_.Add(&bitmap, kTransparent);
        surface.SelectObject(oldBitmap);
    }
    listClients_.SetImageList(&clientStatusIcons_, LVSIL_SMALL);
}

void CFServerDlg::UpdateClientStatusIcons() {
    const auto& entries = clientPresence_.Entries();
    // Updating the image alone preserves row selection, scroll position and
    // text. No log entries or network operations are generated by blinking.
    for (size_t i = 0; i < entries.size(); ++i) {
        LVITEMW item{};
        item.mask = LVIF_IMAGE;
        item.iItem = static_cast<int>(i);
        item.iSubItem = 0;
        item.iImage = fsyn::ClientPresence::IconIndex(entries[i].online, clientIconsBright_);
        listClients_.SetItem(&item);
    }
}

void CFServerDlg::RefreshClientList() {
    const std::vector<fsyn::ClientConnectionInfo> live =
        engine_ ? engine_->ClientListDetails() : std::vector<fsyn::ClientConnectionInfo>{};
    clientPresence_.ReconcileDetailed(live);
    const auto& entries = clientPresence_.Entries();
    // Never DeleteAllItems() here: disconnected IPs must remain visible in red.
    for (size_t i = 0; i < entries.size(); ++i) {
        const auto& entry = entries[i];
        const int row = static_cast<int>(i);
        const CStringW ip = fsyn::Utf8ToWide(entry.ip).c_str();
        const CStringW watchRoot = fsyn::Utf8ToWide(entry.watchRoot).c_str();
        CStringW hostLabel;
        if (!watchRoot.IsEmpty()) hostLabel.Format(L"%s | %s", static_cast<LPCWSTR>(ip), static_cast<LPCWSTR>(watchRoot));
        else hostLabel.Format(L"%s | (WatchRoot unavailable)", static_cast<LPCWSTR>(ip));
        if (row >= listClients_.GetItemCount()) {
            // Show IP/executable-folder in the Tree immediately on connection,
            // even before the first data file has been transferred.
            AddTreePath(entry.hostKey);
            listClients_.InsertItem(row, hostLabel);
            listClients_.SetItemText(row, 1, ip);
            listClients_.SetItemText(row, 2, entry.online ? L"Online" : L"Offline");
        } else {
            if (listClients_.GetItemText(row, 0) != hostLabel) listClients_.SetItemText(row, 0, hostLabel);
            if (listClients_.GetItemText(row, 1) != ip) listClients_.SetItemText(row, 1, ip);
            const CString expected = entry.online ? L"Online" : L"Offline";
            if (listClients_.GetItemText(row, 2) != expected) listClients_.SetItemText(row, 2, expected);
        }
    }
    UpdateClientStatusIcons();
}

void CFServerDlg::AddTreePath(const std::string& relPath) {
    if (!treeRoot_) return;
    CStringW full(fsyn::Utf8ToWide(relPath).c_str());
    int start = 0;
    HTREEITEM parent = treeRoot_;
    std::wstring key;
    while (start < full.GetLength()) {
        int slash = full.Find(L'\\', start);
        CStringW seg = slash < 0 ? full.Mid(start) : full.Mid(start, slash - start);
        if (seg.IsEmpty()) break;
        if (!key.empty()) key += L"\\";
        key += static_cast<LPCWSTR>(seg);
        auto it = treeNodeCache_.find(key);
        if (it != treeNodeCache_.end()) parent = it->second;
        else {
            parent = treeFiles_.InsertItem(seg, parent);
            treeNodeCache_.emplace(key, parent);
        }
        if (slash < 0) break;
        start = slash + 1;
    }
}

namespace {
std::wstring ExplainNetworkError(DWORD code) {
    wchar_t* systemMessage = nullptr;
    const DWORD length = ::FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, 0, reinterpret_cast<LPWSTR>(&systemMessage), 0, nullptr);
    std::wstring result;
    if (length && systemMessage) result.assign(systemMessage, length);
    if (systemMessage) ::LocalFree(systemMessage);
    while (!result.empty() && (result.back() == L'\r' || result.back() == L'\n'))
        result.pop_back();
    return result;
}
}

void CFServerDlg::OpenClientDataFolder(const fsyn::ClientPresence::Entry& entry) {
    // WatchRoot is informational only; it must never change ServerRoot or the
    // persistent hostKey/storage layout. The IP is the authenticated TCP peer.
    const std::wstring localRoot = fsyn::Utf8ToWide(entry.watchRoot);
    const auto paths = fsyn::ui_path::CandidateSharePaths(entry.ip, localRoot);
    if (paths.empty()) {
        CStringW msg;
        msg.Format(L"Cannot construct a safe SMB folder path.\n\n"
                   L"Client IP: %s\nClient WatchRoot: %s\n\n"
                   L"Only absolute drive paths are supported (e.g. D:\\Project_Codex). "
                   L"A normal SMB share (\\\\IP\\D\\...) is tried first. "
                   L"No Server folder will be opened as fallback.",
                   fsyn::Utf8ToWide(entry.ip).c_str(), localRoot.c_str());
        MessageBox(msg, L"Cannot open Client folder", MB_ICONWARNING);
        return;
    }

    // An Explorer process may start successfully and still open Documents if
    // passed a nonexistent UNC argument. Probe the *actual directory* on a
    // worker, then ShellExecuteEx the UNC itself only on success.
    const auto state = folderProbe_;
    bool busy = false;
    {
        std::lock_guard<std::mutex> guard(state->mu);
        busy = state->pending;
        if (!busy) {
            state->pending = true;
            state->ready = false;
            state->target = paths.front();
            state->attempts.clear();
            state->window = GetSafeHwnd();
        }
    }
    if (busy) {
        MessageBox(L"FileSync is still checking the previous Client network folder. "
                   L"Please retry after that check completes.",
                   L"Checking Client folder", MB_ICONINFORMATION);
        return;
    }

    try {
        std::thread([state, paths]() {
            std::wstring selected = paths.front();
            DWORD lastError = ERROR_PATH_NOT_FOUND;
            std::vector<std::pair<std::wstring, DWORD>> attempts;
            for (size_t i = 0; i < paths.size(); ++i) {
                const auto& path = paths[i];
                const DWORD attrs = ::GetFileAttributesW(path.c_str());
                const DWORD error = (attrs == INVALID_FILE_ATTRIBUTES)
                    ? ::GetLastError()
                    : ((attrs & FILE_ATTRIBUTE_DIRECTORY) ? ERROR_SUCCESS : ERROR_DIRECTORY);
                attempts.emplace_back(path, error);
                selected = path;
                lastError = error;
                if (error == ERROR_SUCCESS) break;
                // Only try D$ if the ordinary share seems to be absent.
                // Do NOT silently escalate/share-hop after ACCESS_DENIED or
                // when the target is a non-directory or has invalid syntax.
                if (i == 0 && error != ERROR_BAD_NET_NAME &&
                    error != ERROR_BAD_NETPATH && error != ERROR_PATH_NOT_FOUND &&
                    error != ERROR_FILE_NOT_FOUND) break;
            }
            {
                std::lock_guard<std::mutex> guard(state->mu);
                if (!state->alive) return;
                state->target = std::move(selected);
                state->attempts = std::move(attempts);
                state->error = lastError;
                state->ready = true;
                // Post while locked so OnDestroy cannot invalidate the HWND.
                if (!state->window ||
                    !::PostMessageW(state->window, WM_APP_CLIENT_FOLDER_RESULT, 0, 0)) {
                    state->pending = false;
                    state->ready = false;
                }
            }
        }).detach();
    } catch (const std::system_error&) {
        {
            std::lock_guard<std::mutex> guard(state->mu);
            state->pending = false;
            state->ready = false;
        }
        MessageBox(L"Could not start the background SMB directory check.",
                   L"Open Client folder", MB_ICONERROR);
    }
}

LRESULT CFServerDlg::OnAppClientFolderResult(WPARAM, LPARAM) {
    std::wstring unc;
    DWORD error = ERROR_SUCCESS;
    std::vector<std::pair<std::wstring, DWORD>> attempts;
    {
        std::lock_guard<std::mutex> guard(folderProbe_->mu);
        if (!folderProbe_->ready || !folderProbe_->alive) return 0;
        unc = folderProbe_->target;
        error = folderProbe_->error;
        attempts = std::move(folderProbe_->attempts);
        folderProbe_->ready = false;
        folderProbe_->pending = false;
    }
    if (error != ERROR_SUCCESS) {
        CStringW msg(L"Cannot access the Client's shared folder.\n\n");
        for (const auto& attempt : attempts) {
            CStringW line;
            line.Format(L"%s\nWindows error %lu: %s\n\n",
                        attempt.first.c_str(),
                        static_cast<unsigned long>(attempt.second),
                        ExplainNetworkError(attempt.second).c_str());
            msg += line;
        }
        msg += L"Check the Client's actual SMB share name, permissions, "
               L"network access and Windows firewall. FileSync will not "
               L"open Documents as fallback.";
        MessageBox(msg, L"Cannot access Client folder", MB_ICONWARNING);
        return 0;
    }

    // Open the folder ITSELF as the shell target. Do not invoke explorer.exe
    // with a command-line argument: that is the V8.6 Documents-fallback bug.
    SHELLEXECUTEINFOW launch{};
    launch.cbSize = sizeof(launch);
    launch.fMask = SEE_MASK_FLAG_NO_UI;
    launch.hwnd = GetSafeHwnd();
    launch.lpVerb = L"open";
    launch.lpFile = unc.c_str();
    launch.nShow = SW_SHOWNORMAL;
    if (!::ShellExecuteExW(&launch)) {
        const DWORD launchError = ::GetLastError();
        CStringW msg;
        msg.Format(L"Windows could not open this Client folder:\n\n%s\n\n"
                   L"Windows error %lu: %s",
                   unc.c_str(), static_cast<unsigned long>(launchError),
                   ExplainNetworkError(launchError).c_str());
        MessageBox(msg, L"Cannot open Client folder", MB_ICONWARNING);
    }
    return 0;
}

void CFServerDlg::OnNmDblClickClients(NMHDR* pNMHDR, LRESULT* pResult) {
    *pResult = 0;
    const auto* activation = reinterpret_cast<const NMITEMACTIVATE*>(pNMHDR);
    const int row = activation ? activation->iItem : -1;
    const auto& entries = clientPresence_.Entries();
    if (row < 0 || static_cast<size_t>(row) >= entries.size()) return;
    OpenClientDataFolder(entries[static_cast<size_t>(row)]);
}

void CFServerDlg::OnNmDblClickTree(NMHDR*, LRESULT* pResult) {
    *pResult = 0;
    POINT cursor{};
    ::GetCursorPos(&cursor);
    treeFiles_.ScreenToClient(&cursor);
    UINT flags = 0;
    HTREEITEM node = treeFiles_.HitTest(CPoint(cursor), &flags);
    if (!node || node == treeRoot_ || !(flags & TVHT_ONITEM)) return;

    // Tree shows ServerRoot\IP\ExeFolder\... (NOT the remote Client's
    // WatchRoot). A double-click on an IP must open ServerRoot\IP locally.
    // The HostKey list retains its separate remote SMB action.
    std::vector<std::wstring> segments;
    for (HTREEITEM walk = node; walk && walk != treeRoot_; walk = treeFiles_.GetParentItem(walk))
        segments.push_back(static_cast<LPCWSTR>(treeFiles_.GetItemText(walk)));
    std::reverse(segments.begin(), segments.end());

    std::wstring relative;
    if (!fsyn::ui_tree::BuildServerRelativePath(segments, relative)) {
        MessageBox(L"Invalid Server Tree path; folder was not opened.",
                   L"Open Server data folder", MB_ICONWARNING);
        return;
    }

    // The engine root is captured at Start. Never concatenate an untrusted
    // node directly with a drive path without canonical containment checks.
    // When stopped, use the currently configured root for browsing old data.
    CStringW configuredRoot;
    GetDlgItem(IDC_EDIT_ROOT)->GetWindowTextW(configuredRoot);
    const std::wstring serverRoot = running_ || scheduleWaiting_
        ? rootUtf16_ : std::wstring(static_cast<LPCWSTR>(configuredRoot));
    std::wstring serverPath;
    std::string reason;
    if (!fsyn::PathSanitizer::Validate(serverRoot, fsyn::WideToUtf8(relative),
                                       serverPath, &reason)) {
        CStringW message;
        message.Format(L"Invalid or unsafe path under ServerRoot.\n\nServerRoot: %s\nRelative: %s",
                       serverRoot.c_str(), relative.c_str());
        MessageBox(message, L"Open Server data folder", MB_ICONWARNING);
        return;
    }

    DWORD attrs = ::GetFileAttributesW(serverPath.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        const DWORD code = ::GetLastError();
        CStringW message;
        message.Format(L"The data folder does not exist on the Server, or access was denied.\n\n"
                       L"Path: %s\nWindows error %lu: %s",
                       serverPath.c_str(), static_cast<unsigned long>(code),
                       ExplainNetworkError(code).c_str());
        MessageBox(message, L"Cannot open Server data folder", MB_ICONWARNING);
        return;
    }
    if (!(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
        // Tree can also contain file leaves. Open the containing directory,
        // never execute a data file merely because it was double-clicked.
        const auto lastSlash = serverPath.find_last_of(L"\\/");
        if (lastSlash == std::wstring::npos) return;
        serverPath.resize(lastSlash);
    }

    SHELLEXECUTEINFOW launch{};
    launch.cbSize = sizeof(launch);
    launch.fMask = SEE_MASK_FLAG_NO_UI;
    launch.hwnd = GetSafeHwnd();
    launch.lpVerb = L"open";
    launch.lpFile = serverPath.c_str();
    launch.nShow = SW_SHOWNORMAL;
    if (!::ShellExecuteExW(&launch)) {
        const DWORD code = ::GetLastError();
        CStringW message;
        message.Format(L"Cannot open Server data folder.\n\nPath: %s\nWindows error %lu: %s",
                       serverPath.c_str(), static_cast<unsigned long>(code),
                       ExplainNetworkError(code).c_str());
        MessageBox(message, L"Cannot open Server data folder", MB_ICONWARNING);
    }
}

void CFServerDlg::OnCancel() {
    OnClose();
}
