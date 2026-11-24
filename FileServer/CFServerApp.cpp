#include "pch.h"
#include "CFServerApp.h"
#include "CFServerDlg.h"

#ifdef _DEBUG
#define new DEBUG_NEW
#endif

BEGIN_MESSAGE_MAP(CFServerApp, CWinApp)
END_MESSAGE_MAP()

CFServerApp::CFServerApp() {}

CFServerApp theApp;

BOOL CFServerApp::InitInstance() {
    INITCOMMONCONTROLSEX initControls{};
    initControls.dwSize = sizeof(initControls);
    initControls.dwICC = ICC_WIN95_CLASSES | ICC_DATE_CLASSES;
    InitCommonControlsEx(&initControls);
    CWinApp::InitInstance();
    SetRegistryKey(_T("FileSync"));
    CFServerDlg dlg;
    m_pMainWnd = &dlg;
    dlg.DoModal();
    return FALSE;
}
