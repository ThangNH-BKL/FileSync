#pragma once
#include "resource.h"
#include <afxdialogex.h>
#include <shellapi.h>
#include "Callbacks.h"
#include "ServerEngine.h"
#include "AbsoluteSchedule.h"
#include "ClientPresence.h"
#include <afxdtctl.h>
#include <afxcmn.h>
#include "TransferProgress.h"
#include <chrono>
#include <memory>
#include <string>
#include <deque>
#include <vector>
#include <utility>
#include <unordered_map>
#include <mutex>
#include <atomic>

class CFServerDlg : public CDialogEx, public fsyn::IUiCallbacks {
public:
    CFServerDlg(CWnd* pParent = nullptr);
    virtual ~CFServerDlg();
    enum { IDD = IDD_FILESERVER_DIALOG };

    // IUiCallbacks (gọi từ worker thread → PostMessage sang UI)
    void OnLog(fsyn::LogLevel lv, const std::string& line) override;
    void OnConnState(fsyn::ConnStateUi state) override;
    void OnClientListChanged() override;
    void OnTreeUpdate(const std::string& relPath) override;

protected:
    virtual void DoDataExchange(CDataExchange* pDX);
    virtual BOOL OnInitDialog();
    virtual void OnCancel() override;
    DECLARE_MESSAGE_MAP()

    afx_msg void OnBnClickedBtnStart();
    afx_msg void OnBnClickedBtnStop();
    afx_msg void OnBnClickedBtnBrowse();
    afx_msg void OnBnClickedScheduleToggle();
    afx_msg void OnBnClickedFilterOption(UINT nID);
    afx_msg void OnNmDblClickClients(NMHDR* pNMHDR, LRESULT* pResult);
    afx_msg void OnNmDblClickTree(NMHDR* pNMHDR, LRESULT* pResult);
    afx_msg void OnClose();
    afx_msg void OnDestroy();
    afx_msg LRESULT OnTrayIcon(WPARAM, LPARAM);
    afx_msg LRESULT OnTrayStartup(WPARAM, LPARAM);
    afx_msg LRESULT OnTaskbarCreated(WPARAM, LPARAM);
    afx_msg LRESULT OnAppLog(WPARAM, LPARAM);
    afx_msg LRESULT OnAppState(WPARAM, LPARAM);
    afx_msg LRESULT OnAppClientList(WPARAM, LPARAM);
    afx_msg LRESULT OnAppTreeUpdate(WPARAM, LPARAM);
    afx_msg LRESULT OnAppClientFolderResult(WPARAM, LPARAM);
    afx_msg LRESULT OnAppStopComplete(WPARAM, LPARAM);
    afx_msg void OnTimer(UINT_PTR nIDEvent);
    afx_msg void OnSize(UINT nType, int cx, int cy);
    afx_msg void OnGetMinMaxInfo(MINMAXINFO* info);

private:
    void FitWindowToWorkArea();
    void LayoutDashboard();
    bool layoutReady_ = false;
    void InstallTrayIcon();
    void RemoveTrayIcon();
    void ShowMainWindow();
    void ShowTrayMenu();
    void ExitApplication();
    NOTIFYICONDATAW trayIcon_{};
    bool trayInstalled_ = false;
    bool exiting_ = false;
    UINT taskbarCreatedMsg_ = 0;
    void AppendLog(const std::string& line);
    void RefreshClientList();
    void BuildClientStatusIcons();
    void UpdateClientStatusIcons();
    void AddTreePath(const std::string& relPath);
    void RefreshTransferProgress();
    void LoadSettings();
    void SaveSettings();
    void EnableConfig(bool enabled);
    void UpdateStatus(const CString& s);
    void UpdateScheduleControls();
    void UpdateFilterControls();
    bool BuildSelectedFilter(CStringW& result, CStringW& validationMessage);
    void OpenClientDataFolder(const fsyn::ClientPresence::Entry& entry);
    struct ClientFolderProbe {
        std::mutex mu;
        bool alive = true;
        bool pending = false;
        bool ready = false;
        DWORD error = ERROR_SUCCESS;
        HWND window = nullptr;
        std::wstring target;
        std::vector<std::pair<std::wstring, DWORD>> attempts;
    };
    // Shared lifetime lets a blocked SMB check finish safely after UI closes.
    std::shared_ptr<ClientFolderProbe> folderProbe_ = std::make_shared<ClientFolderProbe>();
    void SetScheduleDefaults();
    bool ReadPickerDateTime(const CDateTimeCtrl& datePicker,
                            const CDateTimeCtrl& timePicker,
                            fsyn::AbsoluteScheduleWindow::Clock::time_point& out) const;
    bool PutPickerDateTime(CDateTimeCtrl& datePicker, CDateTimeCtrl& timePicker,
                           fsyn::AbsoluteScheduleWindow::Clock::time_point when);
    bool StartEngineNow();
    void BeginSchedule(bool autoLaunch);
    void CheckSchedule();
    void RequestStop(bool scheduledFinish);


    std::unique_ptr<fsyn::ServerEngine> engine_;
    std::wstring rootUtf16_;
    CButton scheduleCheckbox_;
    CDateTimeCtrl scheduleStartDate_;
    CDateTimeCtrl scheduleStartTime_;
    CDateTimeCtrl scheduleEndDate_;
    CDateTimeCtrl scheduleEndTime_;
    fsyn::ServerEngine::Config queuedEngineConfig_;
    fsyn::AbsoluteScheduleWindow scheduledWindow_;
    bool scheduleArmed_ = false;
    bool scheduleWaiting_ = false;
    bool scheduleStoppedAtEnd_ = false;
    CListCtrl   listClients_;
    CImageList  clientStatusIcons_;
    fsyn::ClientPresence clientPresence_;
    bool        clientIconsBright_ = true;
    ULONGLONG   clientLastBlinkMs_ = 0;
    ULONGLONG   clientLastPollMs_ = 0;
    CTreeCtrl   treeFiles_;
    CProgressCtrl progressFiles_;
    fsyn::TransferProgress lastProgress_{};
    bool progressInitialized_ = false;
    CListBox    listLog_;
    HTREEITEM   treeRoot_ = nullptr;
    bool        running_ = false;
    bool        stopping_ = false;
    std::mutex logQueueMu_;
    std::deque<std::string> logQueue_;
    std::mutex treeQueueMu_;
    std::deque<std::string> treeQueue_;
    std::unordered_map<std::wstring, HTREEITEM> treeNodeCache_;
    std::atomic<bool> treeWakePending_{false};
    static constexpr UINT_PTR kTreeTimerId = 9001;
};
