#pragma once

#define WINVER       0x0601
#define _WIN32_WINNT 0x0601

#ifndef VC_EXTRALEAN
#define VC_EXTRALEAN
#endif

#include <afxwin.h>         // MFC 核心组件
#include <afxext.h>         // MFC 扩展

#ifndef _AFX_NO_OLE_SUPPORT
#include <afxdtctl.h>
#endif

#ifndef _AFX_NO_AFXCMN_SUPPORT
#include <afxcmn.h>
#endif

#include <afxcontrolbars.h>

#include <winsvc.h>
#include <winioctl.h>
#include "resource.h"
