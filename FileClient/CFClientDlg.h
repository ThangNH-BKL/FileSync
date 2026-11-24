#pragma once
#include "resource.h"
#include <afxdialogex.h>
#include <shellapi.h>
#include "Callbacks.h"
#include "ClientEngine.h"
#include <memory>
#include <string>
#include <deque>
#include <mutex>

class CFClientDlg : public CDialogEx, public fsyn::IUiCallbacks {
public:
    CFClientDlg(CWnd* pParent = nullptr);
    virtual ~CFClientDlg();
    enum { IDD = IDD_FILECLIENT_DIALOG };

    // IUiCallbacks (worker thread)
    void OnLog(fsyn::LogLevel lv, const std::string& line) override;
    void OnConnState(fsyn::ConnStateUi state) override;
    void OnFileDone(const std::string& relPath, uint64_t finalSize) override;

protected:
    virtual void DoDataExchange(CDataExchange* pDX);
    virtual BOOL OnInitDialog();
    virtual void OnCancel() override;
    DECLARE_MESSAGE_MAP()

    afx_msg void OnBnClickedBtnStart();
    afx_msg void OnBnClickedBtnStop();
    afx_msg void OnBnClickedBtnBrowse();
    afx_msg void OnClose();
    afx_msg void OnDestroy();
    afx_msg void OnTimer(UINT_PTR nIDEvent);
    afx_msg LRESULT OnTrayIcon(WPARAM, LPARAM);
    afx_msg LRESULT OnTrayStartup(WPARAM, LPARAM);
    afx_msg LRESULT OnTaskbarCreated(WPARAM, LPARAM);
    afx_msg LRESULT OnAppLog(WPARAM, LPARAM);
    afx_msg LRESULT OnAppState(WPARAM, LPARAM);
    afx_msg LRESULT OnAppFileDone(WPARAM, LPARAM);

private:
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
    void LoadSettings();
    void SaveSettings();
    void EnableConfig(bool enabled);
    void UpdateStatus(const CString& s);

    std::unique_ptr<fsyn::ClientEngine> engine_;
    CListBox listLog_;
    bool running_ = false;
    // Logger writes to disk independently; only the UI queue is bounded.
    std::mutex logQueueMu_;
    std::deque<std::string> logQueue_;
    static constexpr UINT_PTR kLogTimerId = 9201;
};
