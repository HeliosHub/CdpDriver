#pragma once

#include <Windows.h>

/* 口令只放在这块缓冲里。用完必须调用 CdpSecurePasswordClear。 */
typedef struct _CDP_SECURE_PASSWORD {
	UCHAR Utf8[128];
	ULONG Bytes;
} CDP_SECURE_PASSWORD;

/* 维护对话框可以走忘记密码，或声明这台机器还没设过保护口令。 */
typedef struct _CDP_PROMPT_RESULT {
	CDP_SECURE_PASSWORD Password;
	BOOL Forgot;
	BOOL Unconfigured;
} CDP_PROMPT_RESULT;

/* 清掉 UTF-8 口令字节。 */
void CdpSecurePasswordClear(CDP_SECURE_PASSWORD *Password);

/* 把维护窗口提到安装向导前面。nsExec 后台启动时 SetForegroundWindow 经常失败。 */
void CdpBringWindowToFront(_In_ HWND Window);

/*
 * 弹出无回显口令框。MinBytes 为 0 时允许空口令。
 * 取消返回 FALSE。忘记密码或尚未设密时 Password 为空，对应标志为 TRUE。
 */
BOOL CdpPromptPassword(
	_In_opt_ HWND Owner,
	_In_z_ const wchar_t *Title,
	_In_z_ const wchar_t *Prompt,
	_In_ ULONG MinBytes,
	_In_ BOOL AllowForgot,
	_In_ BOOL AllowUnconfigured,
	_In_ BOOL Confirm,
	_Out_ CDP_PROMPT_RESULT *Result);
