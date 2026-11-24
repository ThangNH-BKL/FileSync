#include "pch.h"
#include "CFServerDlg.h"
#include "resource.h"
#include "Util.h"
#include <shlobj.h>
#include <shellapi.h>
#include <thread>
#include <ctime>
#include <cwchar>
#include <chrono>
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
    ON_WM_TIMER()
END_MESSAGE_MAP()

BOOL CFServerDlg::OnInitDialog() {
    CDialogEx::OnInitDialog();
    SetWindowText(_T("FileServer - File Sync"));

    // The status icon is shown beside HostKey; the final column also shows
    // Online/Offline in text (useful when the user cannot distinguish colors).
    listClients_.InsertColumn(0, _T("HostKey"), LVCFMT_LEFT, 118);
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
    GetDlgItem(IDC_EDIT_ROOT)->EnableWindow(enabled);
    GetDlgItem(IDC_BTN_BROWSE)->EnableWindow(enabled);
    GetDlgItem(IDC_EDIT_FILTER)->EnableWindow(enabled);
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
    CString root = app->GetProfileString(L"Server", L"Root", L"");
    GetDlgItem(IDC_EDIT_ROOT)->SetWindowTextW(root);
    rootUtf16_ = (LPCWSTR)root;
    GetDlgItem(IDC_EDIT_FILTER)->SetWindowTextW(app->GetProfileString(L"Server", L"Filter", L"*"));
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
    GetDlgItem(IDC_EDIT_ROOT)->GetWindowTextW(s);        app->WriteProfileString(L"Server", L"Root", s);
    GetDlgItem(IDC_EDIT_FILTER)->GetWindowTextW(s);      app->WriteProfileString(L"Server", L"Filter", s);
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

    CString sPort, sToken, sRoot, sFilter;
    GetDlgItem(IDC_EDIT_PORT)->GetWindowTextW(sPort);
    GetDlgItem(IDC_EDIT_TOKEN)->GetWindowTextW(sToken);
    GetDlgItem(IDC_EDIT_ROOT)->GetWindowTextW(sRoot);
    GetDlgItem(IDC_EDIT_FILTER)->GetWindowTextW(sFilter);
    if (sRoot.IsEmpty() || _wtoi(sPort) <= 0 || _wtoi(sPort) > 65535 || sToken.IsEmpty()) {
        if (!autoLaunch) MessageBox(L"Please configure a valid port, token and root folder.", L"FileServer", MB_ICONERROR);
        else UpdateStatus(L"Status: Configure Server before starting");
        return;
    }

    // The old recurring Window/Cycle scheduler is no longer selected from the UI.
    // When the engine is running, its internal gate must be open 24/7;
    // absolute start/stop is enforced by the UI timer around ServerEngine lifetime.
    fsyn::ServerEngine::Config cfg;
    cfg.port = static_cast<uint16_t>(_wtoi(sPort));
    cfg.token = CStringA(sToken);
    cfg.serverRootUtf16 = static_cast<LPCWSTR>(sRoot);
    cfg.extFilterSpec = CStringA(sFilter);
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
    wcscpy_s(trayIcon_.szTip, _countof(trayIcon_.szTip), L"FileServer - FileSync");
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
    const std::vector<std::pair<std::string, std::string>> live =
        engine_ ? engine_->ClientList() : std::vector<std::pair<std::string, std::string>>{};
    clientPresence_.Reconcile(live);
    const auto& entries = clientPresence_.Entries();
    // Never DeleteAllItems() here: disconnected IPs must remain visible in red.
    for (size_t i = 0; i < entries.size(); ++i) {
        const auto& entry = entries[i];
        const int row = static_cast<int>(i);
        if (row >= listClients_.GetItemCount()) {
            LVITEMW item{};
            CStringW host(entry.hostKey.c_str());
            item.mask = LVIF_TEXT | LVIF_IMAGE;
            item.iItem = row;
            item.iSubItem = 0;
            item.pszText = host.GetBuffer();
            item.iImage = fsyn::ClientPresence::IconIndex(entry.online, clientIconsBright_);
            listClients_.InsertItem(&item);
            host.ReleaseBuffer();
            listClients_.SetItemText(row, 1, CStringW(entry.ip.c_str()));
            listClients_.SetItemText(row, 2, entry.online ? L"Online" : L"Offline");
        } else {
            const CString expected = entry.online ? L"Online" : L"Offline";
            if (listClients_.GetItemText(row, 2) != expected) {
                listClients_.SetItemText(row, 2, expected);
            }
        }
    }
    UpdateClientStatusIcons();
}

void CFServerDlg::AddTreePath(const std::string& relPath) {
    if (!treeRoot_) return;
    CStringW full(relPath.c_str());
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

void CFServerDlg::OnCancel() {
    OnClose();
}
