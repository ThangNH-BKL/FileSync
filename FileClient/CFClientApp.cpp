#include "pch.h"
#include "CFClientApp.h"
#include "CFClientDlg.h"

#ifdef _DEBUG
#define new DEBUG_NEW
#endif

BEGIN_MESSAGE_MAP(CFClientApp, CWinApp)
END_MESSAGE_MAP()

CFClientApp::CFClientApp() {}
CFClientApp theApp;

BOOL CFClientApp::InitInstance() {
    CWinApp::InitInstance();
    SetRegistryKey(_T("FileSync"));
    CFClientDlg dlg;
    m_pMainWnd = &dlg;
    dlg.DoModal();
    return FALSE;
}
