#pragma once

#include "CdpSecurePrompt.h"

/* 把父进程 PID 交给 CdpProtect。父进程不是安装器时只授权 Helper 自己。 */
BOOL CdpAuthorizeMaintenance(
	_In_ ULONG Flags,
	_In_opt_ const CDP_SECURE_PASSWORD *Password,
	_Out_opt_ DWORD *Win32Error);

/* 弹出维护口令框并完成授权。AllowForgot 为 TRUE 时忘记密码会把 Forgot 置位。 */
BOOL CdpPromptAndAuthorize(
	_In_ ULONG Flags,
	_In_ BOOL AllowForgot,
	_In_ BOOL AllowUnconfigured,
	_Out_ CDP_PROMPT_RESULT *Result,
	_Out_opt_ DWORD *Win32Error);
