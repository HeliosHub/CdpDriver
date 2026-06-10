#pragma once

class CQHEngineUIApp : public CWinApp
{
public:
    CQHEngineUIApp();

public:
    virtual BOOL InitInstance();
};

extern CQHEngineUIApp theApp;
