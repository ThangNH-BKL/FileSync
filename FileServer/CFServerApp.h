#pragma once
#ifndef __AFXWIN_H__
#error "include pch.h"
#endif
#include "resource.h"

class CFServerApp : public CWinApp {
public:
    CFServerApp();
    virtual BOOL InitInstance();
    DECLARE_MESSAGE_MAP()
};

extern CFServerApp theApp;
