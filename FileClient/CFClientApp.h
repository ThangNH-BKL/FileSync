#pragma once
#ifndef __AFXWIN_H__
#error "include pch.h"
#endif
#include "resource.h"

class CFClientApp : public CWinApp {
public:
    CFClientApp();
    virtual BOOL InitInstance();
    DECLARE_MESSAGE_MAP()
};

extern CFClientApp theApp;
