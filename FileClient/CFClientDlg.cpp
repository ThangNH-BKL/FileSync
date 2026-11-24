#include "pch.h"
#include "CFClientDlg.h"
#include "resource.h"
#include "Util.h"
#include <shlobj.h>
#include <shellapi.h>
#include <vector>
#pragma comment(lib, "Shell32.lib")

static UINT WM_TASKBARCREATED = ::RegisterWindowMessageW(L"TaskbarCreated");
#define WM_TRAY_ICON (WM_APP + 500)
#define WM_TRAY_STARTUP (WM_APP + 501)
#define TRAY_CMD_OPEN 510
#define TRAY_CMD_TOGGLE 511
#define TRAY_CMD_EXIT 512

// Always resolve against FileClient.exe, never the current working directory.
static CStringW FileClientExecutableDirectory() {
    std::vector<wchar_t> buf(512);
    for (;;) {
        const DWORD count = ::GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
        if (count == 0) return {};
        if (count < static_cast<DWORD>(buf.size())) {
            std::wstring full(buf.data(), count);
            const auto slash = full.find_last_of(L"\\/");
            return slash == std::wstring::npos ? CStringW() : CStringW(full.substr(0, slash).c_str());
        }
        if (buf.size() >= 32768) return {};
        buf.resize(buf.size() * 2);
    }
}

using namespace fsyn;

#define WM_APP_LOG_MSG    (WM_APP + 201)
#define WM_APP_STATE_MSG  (WM_APP + 202)
#define WM_APP_FILEDONE   (WM_APP + 203)

CFClientDlg::CFClientDlg(CWnd* pParent)
    : CDialogEx(IDD_FILECLIENT_DIALOG, pParent) {}

CFClientDlg::~CFClientDlg() {
    if (engine_) { engine_->Stop(); engine_.reset(); }
}

void CFClientDlg::DoDataExchange(CDataExchange* pDX) {
    CDialogEx::DoDataExchange(pDX);
    DDX_Control(pDX, IDC_LIST_LOG, listLog_);
}

BEGIN_MESSAGE_MAP(CFClientDlg, CDialogEx)
    ON_BN_CLICKED(IDC_BTN_START, &CFClientDlg::OnBnClickedBtnStart)
    ON_BN_CLICKED(IDC_BTN_STOP, &CFClientDlg::OnBnClickedBtnStop)
    ON_BN_CLICKED(IDC_BTN_BROWSE, &CFClientDlg::OnBnClickedBtnBrowse)
    ON_WM_WINDOWPOSCHANGING()
    ON_WM_CLOSE()
    ON_WM_DESTROY()
    ON_WM_TIMER()
    ON_MESSAGE(WM_TRAY_ICON, &CFClientDlg::OnTrayIcon)
    ON_MESSAGE(WM_TRAY_STARTUP, &CFClientDlg::OnTrayStartup)
    ON_REGISTERED_MESSAGE(WM_TASKBARCREATED, &CFClientDlg::OnTaskbarCreated)
    ON_MESSAGE(WM_APP_LOG_MSG, &CFClientDlg::OnAppLog)
    ON_MESSAGE(WM_APP_STATE_MSG, &CFClientDlg::OnAppState)
    ON_MESSAGE(WM_APP_FILEDONE, &CFClientDlg::OnAppFileDone)
END_MESSAGE_MAP()

BOOL CFClientDlg::OnInitDialog() {
    CDialogEx::OnInitDialog();
    SetWindowText(_T("FileClient - File Sync"));
    listLog_.InitStorage(5000, 256);
    SetTimer(kLogTimerId, 250, nullptr); // Drain a bounded batch; no PostMessage storm.
    LoadSettings();
    EnableConfig(true);
    UpdateStatus(_T("Status: Idle"));
    taskbarCreatedMsg_ = WM_TASKBARCREATED;
    InstallTrayIcon();
    // Defer auto-start and hiding until the dialog has finished initializing.
    PostMessage(WM_TRAY_STARTUP, 0, 0);
    return TRUE;
}

void CFClientDlg::LoadSettings() {
    CWinApp* app = AfxGetApp();
    GetDlgItem(IDC_EDIT_IP)->SetWindowTextW(app->GetProfileString(L"Client", L"ServerIp", L"127.0.0.1"));
    GetDlgItem(IDC_EDIT_PORT)->SetWindowTextW(app->GetProfileString(L"Client", L"Port", L"9000"));
    // Explicit requirement: default to the directory containing FileClient.exe
    // every time the application starts. Browse remains an optional session override.
    CStringW exeDir = FileClientExecutableDirectory();
    GetDlgItem(IDC_EDIT_WATCH)->SetWindowTextW(exeDir.IsEmpty()
        ? app->GetProfileString(L"Client", L"WatchRoot", L"") : exeDir);
    GetDlgItem(IDC_EDIT_TOKEN)->SetWindowTextW(app->GetProfileString(L"Client", L"Token", L"secret"));
}

void CFClientDlg::SaveSettings() {
    CWinApp* app = AfxGetApp();
    CString s;
    GetDlgItem(IDC_EDIT_IP)->GetWindowTextW(s);      app->WriteProfileString(L"Client", L"ServerIp", s);
    GetDlgItem(IDC_EDIT_PORT)->GetWindowTextW(s);    app->WriteProfileString(L"Client", L"Port", s);
    GetDlgItem(IDC_EDIT_WATCH)->GetWindowTextW(s);   app->WriteProfileString(L"Client", L"WatchRoot", s);
    GetDlgItem(IDC_EDIT_TOKEN)->GetWindowTextW(s);   app->WriteProfileString(L"Client", L"Token", s);
}

void CFClientDlg::EnableConfig(bool enabled) {
    GetDlgItem(IDC_EDIT_IP)->EnableWindow(enabled);
    GetDlgItem(IDC_EDIT_PORT)->EnableWindow(enabled);
    GetDlgItem(IDC_EDIT_WATCH)->EnableWindow(enabled);
    GetDlgItem(IDC_EDIT_TOKEN)->EnableWindow(enabled);
    GetDlgItem(IDC_BTN_BROWSE)->EnableWindow(enabled);
    GetDlgItem(IDC_BTN_START)->EnableWindow(enabled);
    GetDlgItem(IDC_BTN_STOP)->EnableWindow(!enabled);
}

void CFClientDlg::UpdateStatus(const CString& s) {
    SetDlgItemText(IDC_STATIC_STATUS, s);
}

void CFClientDlg::AppendLog(const std::string& line) {
    // Remove old entries in batches instead of a costly shift per new line.
    if (listLog_.GetCount() >= 5000) {
        for (int i = 0; i < 1000; ++i) listLog_.DeleteString(0);
    }
    CStringW w(CA2W(line.c_str(), CP_UTF8));
    int idx = listLog_.AddString(w);
    listLog_.SetTopIndex(idx);
}

void CFClientDlg::OnBnClickedBtnBrowse() {
    BROWSEINFOW bi{};
    bi.lpszTitle = L"Chọn thư mục theo dõi";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
    if (pidl) {
        wchar_t path[MAX_PATH] = { 0 };
        if (SHGetPathFromIDListW(pidl, path)) {
            GetDlgItem(IDC_EDIT_WATCH)->SetWindowTextW(path);
        }
        CoTaskMemFree(pidl);
    }
}

void CFClientDlg::OnBnClickedBtnStart() {
    if (running_) return;

    CString sIp, sPort, sWatch, sToken;
    GetDlgItem(IDC_EDIT_IP)->GetWindowTextW(sIp);
    GetDlgItem(IDC_EDIT_PORT)->GetWindowTextW(sPort);
    GetDlgItem(IDC_EDIT_WATCH)->GetWindowTextW(sWatch);
    GetDlgItem(IDC_EDIT_TOKEN)->GetWindowTextW(sToken);

    if (sWatch.IsEmpty()) {
        MessageBox(L"Vui lòng chọn thư mục theo dõi.", L"Lỗi", MB_ICONERROR);
        return;
    }
    DWORD attr = GetFileAttributesW(sWatch);
    if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
        MessageBox(L"Thư mục theo dõi không tồn tại.", L"Lỗi", MB_ICONERROR);
        return;
    }
    int port = _wtoi(sPort);
    if (port <= 0 || port > 65535) {
        MessageBox(L"Port không hợp lệ.", L"Lỗi", MB_ICONERROR);
        return;
    }
    if (sToken.IsEmpty()) {
        MessageBox(L"Token không được rỗng.", L"Lỗi", MB_ICONERROR);
        return;
    }

    ClientEngine::Config cfg;
    cfg.serverIp        = std::string(CStringA(sIp).GetString());
    cfg.port            = (uint16_t)port;
    cfg.watchRootUtf16  = (LPCWSTR)sWatch;
    cfg.token           = std::string(CStringA(sToken).GetString());
    // hostname rỗng → ClientEngine tự lấy GetLocalHostname()

    SaveSettings();

    engine_ = std::make_unique<ClientEngine>(cfg, this);
    if (!engine_->Start()) {
        MessageBox(L"Không khởi động được client. Xem log.", L"Lỗi", MB_ICONERROR);
        engine_.reset();
        return;
    }

    running_ = true;
    EnableConfig(false);
    UpdateStatus(_T("Status: Connecting..."));
    AppendLog("Client started.");
}

void CFClientDlg::OnBnClickedBtnStop() {
    if (!running_) return;
    if (engine_) { engine_->Stop(); engine_.reset(); }
    running_ = false;
    EnableConfig(true);
    UpdateStatus(_T("Status: Stopped"));
    AppendLog("Client stopped.");
}

void CFClientDlg::OnWindowPosChanging(WINDOWPOS* lpwndpos) {
    // MFC DoModal may show a new dialog after WM_INITDIALOG. Block that
    // initial show while the tray icon is installed; no brief flashing window.
    if (trayInstalled_ && !allowDialogVisible_ && (lpwndpos->flags & SWP_SHOWWINDOW)) {
        lpwndpos->flags &= ~SWP_SHOWWINDOW;
        lpwndpos->flags |= SWP_HIDEWINDOW;
    }
    CDialogEx::OnWindowPosChanging(lpwndpos);
}

void CFClientDlg::OnClose() {
    if (!exiting_) {
        allowDialogVisible_ = false;
        ShowWindow(SW_HIDE);
        return;
    }
    OnBnClickedBtnStop();
    RemoveTrayIcon();
    CDialogEx::OnClose();
}

void CFClientDlg::OnDestroy() {
    KillTimer(kLogTimerId);
    RemoveTrayIcon();
    CDialogEx::OnDestroy();
}

void CFClientDlg::InstallTrayIcon() {
    if (trayInstalled_) return;
    trayIcon_ = {};
    trayIcon_.cbSize = sizeof(trayIcon_);
    trayIcon_.hWnd = GetSafeHwnd();
    trayIcon_.uID = 1;
    trayIcon_.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    trayIcon_.uCallbackMessage = WM_TRAY_ICON;
    trayIcon_.hIcon = ::LoadIconW(nullptr, IDI_INFORMATION);
    wcscpy_s(trayIcon_.szTip, _countof(trayIcon_.szTip), L"FileClient - FileSync");
    trayInstalled_ = Shell_NotifyIconW(NIM_ADD, &trayIcon_) != FALSE;
    if (trayInstalled_) {
        trayIcon_.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &trayIcon_);
    }
}

void CFClientDlg::RemoveTrayIcon() {
    if (trayInstalled_) {
        Shell_NotifyIconW(NIM_DELETE, &trayIcon_);
        trayInstalled_ = false;
    }
}

void CFClientDlg::ShowMainWindow() {
    allowDialogVisible_ = true;
    ShowWindow(SW_SHOW);
    ShowWindow(SW_RESTORE);
    SetForegroundWindow();
}

void CFClientDlg::ExitApplication() {
    exiting_ = true;
    OnClose();
}

void CFClientDlg::ShowTrayMenu() {
    CMenu menu;
    if (!menu.CreatePopupMenu()) return;
    menu.AppendMenuW(MF_STRING, TRAY_CMD_OPEN, L"Open " L"FileClient");
    menu.AppendMenuW(MF_STRING, TRAY_CMD_TOGGLE, running_ ? L"Stop sync" : L"Start sync");
    menu.AppendMenuW(MF_SEPARATOR);
    menu.AppendMenuW(MF_STRING, TRAY_CMD_EXIT, L"Exit");
    POINT pt{};
    ::GetCursorPos(&pt);
    SetForegroundWindow();
    UINT cmd = menu.TrackPopupMenu(TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY,
                                   pt.x, pt.y, this);
    if (cmd == TRAY_CMD_OPEN) ShowMainWindow();
    else if (cmd == TRAY_CMD_TOGGLE) {
        if (running_) OnBnClickedBtnStop();
        else OnBnClickedBtnStart();
    } else if (cmd == TRAY_CMD_EXIT) ExitApplication();
}

LRESULT CFClientDlg::OnTrayIcon(WPARAM, LPARAM lp) {
    // NOTIFYICON_VERSION_4 delivers the mouse event in LOWORD(lParam).
    const UINT eventCode = LOWORD(static_cast<DWORD_PTR>(lp));
    if (eventCode == WM_LBUTTONDBLCLK || eventCode == NIN_SELECT || eventCode == NIN_KEYSELECT)
        ShowMainWindow();
    else if (eventCode == WM_RBUTTONUP || eventCode == WM_CONTEXTMENU)
        ShowTrayMenu();
    return 0;
}

LRESULT CFClientDlg::OnTrayStartup(WPARAM, LPARAM) {
    // Hide the window and start synchronization using saved settings.
    if (trayInstalled_) {
        allowDialogVisible_ = false;
        ShowWindow(SW_HIDE);
    } else {
        // Don't strand a hidden process if Windows notification area failed.
        ShowMainWindow();
    }
    OnBnClickedBtnStart();
    return 0;
}

LRESULT CFClientDlg::OnTaskbarCreated(WPARAM, LPARAM) {
    // Windows Explorer can restart; recreate our icon in the new tray.
    trayInstalled_ = false;
    InstallTrayIcon();
    return 0;
}

// ---------- IUiCallbacks (worker thread) ----------
void CFClientDlg::OnLog(LogLevel, const std::string& line) {
    if (!::IsWindow(GetSafeHwnd())) return;
    std::lock_guard<std::mutex> guard(logQueueMu_);
    // Bound UI memory; Logger still records warnings/errors on disk.
    if (logQueue_.size() >= 10000) logQueue_.pop_front();
    logQueue_.push_back(line);
}

void CFClientDlg::OnTimer(UINT_PTR nIDEvent) {
    if (nIDEvent == kLogTimerId) {
        std::deque<std::string> batch;
        {
            std::lock_guard<std::mutex> guard(logQueueMu_);
            for (size_t i = 0; i < 100 && !logQueue_.empty(); ++i) {
                batch.push_back(std::move(logQueue_.front()));
                logQueue_.pop_front();
            }
        }
        for (const auto& line : batch) AppendLog(line);
        return;
    }
    CDialogEx::OnTimer(nIDEvent);
}

void CFClientDlg::OnConnState(ConnStateUi state) {
    if (!::IsWindow(GetSafeHwnd())) return;
    PostMessage(WM_APP_STATE_MSG, (WPARAM)state, 0);
}

void CFClientDlg::OnFileDone(const std::string& relPath, uint64_t /*finalSize*/) {
    if (!::IsWindow(GetSafeHwnd())) return;
    CString* p = new CString(CA2W(relPath.c_str(), CP_UTF8));
    if (!PostMessage(WM_APP_FILEDONE, 0, (LPARAM)p)) delete p;
}

// ---------- Handlers UI ----------
LRESULT CFClientDlg::OnAppLog(WPARAM, LPARAM lp) {
    CString* p = (CString*)lp;
    if (p) { AppendLog(std::string(CStringA(*p).GetString())); delete p; }
    return 0;
}

LRESULT CFClientDlg::OnAppState(WPARAM wp, LPARAM) {
    ConnStateUi s = (ConnStateUi)wp;
    CString txt;
    switch (s) {
        case ConnStateUi::Idle:          txt = L"Status: Idle"; break;
        case ConnStateUi::Connecting:    txt = L"Status: Connecting..."; break;
        case ConnStateUi::Syncing:       txt = L"Status: Syncing..."; break;
        case ConnStateUi::ConnectedIdle: txt = L"Status: Connected"; break;
        case ConnStateUi::ReconnectWait: txt = L"Status: Reconnecting (backoff)..."; break;
        case ConnStateUi::AuthFail:      txt = L"Status: Auth failed - kiểm tra token"; break;
        case ConnStateUi::Stopping:      txt = L"Status: Stopping..."; break;
    }
    UpdateStatus(txt);
    return 0;
}

LRESULT CFClientDlg::OnAppFileDone(WPARAM, LPARAM lp) {
    CString* p = (CString*)lp;
    if (p) {
        // The Logger already displays the commit; avoid duplicate UI lines.
        delete p;
    }
    return 0;
}

void CFClientDlg::OnCancel() {
    OnClose();
}
