#include "stdafx.h"
#include "QHEngineUI.h"
#include "QHEngineUIDlg.h"

CQHEngineUIApp theApp;

CQHEngineUIApp::CQHEngineUIApp()
{
}

BOOL CQHEngineUIApp::InitInstance()
{
    CWinApp::InitInstance();

    // 创建主对话框
    CQHEngineUIDlg dlg;
    m_pMainWnd = &dlg;
    dlg.DoModal();

    return FALSE;
}
