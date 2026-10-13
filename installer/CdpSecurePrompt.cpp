/*
 * Helper 自己的口令对话框。
 * 口令不进入命令行、不写注册表，窗口销毁前清掉编辑框和 UTF-8 缓冲。
 */

#include "CdpSecurePrompt.h"

#include <strsafe.h>

/* 对话框控件编号。 */
#define CDP_PROMPT_EDIT 101
#define CDP_PROMPT_CONFIRM 102
#define CDP_PROMPT_FORGOT 103

typedef struct _CDP_PROMPT_STATE {
	const wchar_t *Prompt;
	ULONG MinBytes;
	BOOL AllowForgot;
	BOOL AllowUnconfigured;
	BOOL Confirm;
	CDP_PROMPT_RESULT *Result;
	HWND Edit;
	HWND ConfirmEdit;
	BOOL Finished;
	BOOL Accepted;
} CDP_PROMPT_STATE;

/* 把宽字符口令收成最多 128 字节的 UTF-8。过长或非法字符返回 FALSE。 */
static BOOL CdpPromptToUtf8(
	_In_z_ const wchar_t *Wide,
	_In_ ULONG MinBytes,
	_Out_ CDP_SECURE_PASSWORD *Password)
{
	int bytes;

	CdpSecurePasswordClear(Password);
	if (!Wide)
		return FALSE;
	bytes = WideCharToMultiByte(
		CP_UTF8,
		WC_ERR_INVALID_CHARS,
		Wide,
		-1,
		(LPSTR)Password->Utf8,
		sizeof(Password->Utf8),
		NULL,
		NULL);
	if (bytes <= 0)
		return FALSE;
	Password->Bytes = (ULONG)(bytes - 1);
	if (Password->Bytes < MinBytes || Password->Bytes > sizeof(Password->Utf8))
	{
		CdpSecurePasswordClear(Password);
		return FALSE;
	}
	return TRUE;
}

/* 读编辑框后立刻清掉窗口里的明文。 */
static BOOL CdpPromptReadEdit(
	_In_ HWND Edit,
	_In_ ULONG MinBytes,
	_Out_ CDP_SECURE_PASSWORD *Password)
{
	wchar_t wide[129];
	BOOL ok;

	ZeroMemory(wide, sizeof(wide));
	GetWindowTextW(Edit, wide, _countof(wide));
	SetWindowTextW(Edit, L"");
	ok = CdpPromptToUtf8(wide, MinBytes, Password);
	SecureZeroMemory(wide, sizeof(wide));
	return ok;
}

/* 口令框窗口过程。确定时才把 UTF-8 交给调用方。 */
static LRESULT CALLBACK CdpPromptWindowProc(HWND Window, UINT Message, WPARAM WParam, LPARAM LParam)
{
	CDP_PROMPT_STATE *state = (CDP_PROMPT_STATE *)GetWindowLongPtrW(Window, GWLP_USERDATA);

	if (Message == WM_NCCREATE)
	{
		CREATESTRUCTW *create = (CREATESTRUCTW *)LParam;
		SetWindowLongPtrW(Window, GWLP_USERDATA, (LONG_PTR)create->lpCreateParams);
		return DefWindowProcW(Window, Message, WParam, LParam);
	}
	if (!state)
		return DefWindowProcW(Window, Message, WParam, LParam);

	switch (Message)
	{
	case WM_COMMAND:
		if (LOWORD(WParam) == IDCANCEL || LOWORD(WParam) == CDP_PROMPT_FORGOT)
		{
			if (state->Edit)
				SetWindowTextW(state->Edit, L"");
			if (state->ConfirmEdit)
				SetWindowTextW(state->ConfirmEdit, L"");
			state->Result->Forgot = LOWORD(WParam) == CDP_PROMPT_FORGOT;
			state->Result->Unconfigured = FALSE;
			state->Accepted = state->Result->Forgot;
			state->Finished = TRUE;
			DestroyWindow(Window);
			return 0;
		}
		if (LOWORD(WParam) == IDOK)
		{
			CDP_SECURE_PASSWORD confirm;

			/* 不再显示“尚未设密”按钮。确实未配置口令时，空输入按确定
			 * 仍走受驱动校验的未配置维护通道。 */
			if (state->AllowUnconfigured &&
				GetWindowTextLengthW(state->Edit) == 0)
			{
				state->Result->Unconfigured = TRUE;
				state->Accepted = TRUE;
				state->Finished = TRUE;
				DestroyWindow(Window);
				return 0;
			}
			ZeroMemory(&confirm, sizeof(confirm));
			if (!CdpPromptReadEdit(state->Edit, state->MinBytes, &state->Result->Password))
			{
				MessageBoxW(Window, L"密码长度无效。保护密码至少 6 字节，最多 128 字节。",
					L"源点恢复", MB_OK | MB_ICONWARNING);
				return 0;
			}
			if (state->Confirm)
			{
				if (!CdpPromptReadEdit(state->ConfirmEdit, state->MinBytes, &confirm) ||
					confirm.Bytes != state->Result->Password.Bytes ||
					memcmp(confirm.Utf8, state->Result->Password.Utf8, confirm.Bytes) != 0)
				{
					CdpSecurePasswordClear(&confirm);
					CdpSecurePasswordClear(&state->Result->Password);
					MessageBoxW(Window, L"两次输入的新密码不一致。", L"源点恢复", MB_OK | MB_ICONWARNING);
					return 0;
				}
				CdpSecurePasswordClear(&confirm);
			}
			state->Accepted = TRUE;
			state->Finished = TRUE;
			DestroyWindow(Window);
			return 0;
		}
		break;
	case WM_CLOSE:
		state->Accepted = FALSE;
		state->Finished = TRUE;
		DestroyWindow(Window);
		return 0;
	default:
		break;
	}
	return DefWindowProcW(Window, Message, WParam, LParam);
}

/* 清掉口令缓冲。 */
void CdpSecurePasswordClear(CDP_SECURE_PASSWORD *Password)
{
	if (!Password)
		return;
	SecureZeroMemory(Password, sizeof(*Password));
}

/* 置顶并尝试抢前台，避免口令框落在 NSIS 向导后面。 */
void CdpBringWindowToFront(HWND Window)
{
	HWND foreground;
	DWORD foregroundTid;
	DWORD thisTid;
	FLASHWINFO flash;

	if (!Window)
		return;
	ShowWindow(Window, SW_SHOWNORMAL);
	SetWindowPos(
		Window,
		HWND_TOPMOST,
		0,
		0,
		0,
		0,
		SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
	foreground = GetForegroundWindow();
	thisTid = GetCurrentThreadId();
	foregroundTid = foreground ? GetWindowThreadProcessId(foreground, NULL) : 0;
	if (foregroundTid != 0 && foregroundTid != thisTid)
		AttachThreadInput(foregroundTid, thisTid, TRUE);
	BringWindowToTop(Window);
	SetForegroundWindow(Window);
	if (foregroundTid != 0 && foregroundTid != thisTid)
		AttachThreadInput(foregroundTid, thisTid, FALSE);
	ZeroMemory(&flash, sizeof(flash));
	flash.cbSize = sizeof(flash);
	flash.hwnd = Window;
	flash.dwFlags = FLASHW_ALL | FLASHW_TIMERNOFG;
	flash.uCount = 3;
	FlashWindowEx(&flash);
}

/* 在拥有者/当前前台安装器所在显示器的工作区正中央显示。 */
static void CdpCenterPromptWindow(HWND Window, HWND Owner)
{
	HWND anchor = Owner;
	RECT windowRect;
	RECT workArea;
	MONITORINFO monitorInfo;
	HMONITOR monitor;
	int width;
	int height;
	int x;
	int y;

	if (!Window || !GetWindowRect(Window, &windowRect))
		return;
	if (!anchor || anchor == Window || !IsWindowVisible(anchor))
		anchor = GetForegroundWindow();
	if (anchor == Window || !IsWindow(anchor))
		anchor = NULL;

	monitor = anchor
		? MonitorFromWindow(anchor, MONITOR_DEFAULTTONEAREST)
		: MonitorFromWindow(Window, MONITOR_DEFAULTTOPRIMARY);
	ZeroMemory(&monitorInfo, sizeof(monitorInfo));
	monitorInfo.cbSize = sizeof(monitorInfo);
	if (monitor && GetMonitorInfoW(monitor, &monitorInfo))
		workArea = monitorInfo.rcWork;
	else
	{
		workArea.left = 0;
		workArea.top = 0;
		workArea.right = GetSystemMetrics(SM_CXSCREEN);
		workArea.bottom = GetSystemMetrics(SM_CYSCREEN);
	}

	width = windowRect.right - windowRect.left;
	height = windowRect.bottom - windowRect.top;
	x = workArea.left + ((workArea.right - workArea.left) - width) / 2;
	y = workArea.top + ((workArea.bottom - workArea.top) - height) / 2;

	if (x < workArea.left)
		x = workArea.left;
	if (y < workArea.top)
		y = workArea.top;
	if (x + width > workArea.right)
		x = workArea.right - width;
	if (y + height > workArea.bottom)
		y = workArea.bottom - height;
	SetWindowPos(Window, NULL, x, y, 0, 0,
		SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

/* 创建无回显口令窗口并等到用户关闭。 */
BOOL CdpPromptPassword(
	HWND Owner,
	const wchar_t *Title,
	const wchar_t *Prompt,
	ULONG MinBytes,
	BOOL AllowForgot,
	BOOL AllowUnconfigured,
	BOOL Confirm,
	CDP_PROMPT_RESULT *Result)
{
	WNDCLASSW windowClass = {};
	CDP_PROMPT_STATE state = {};
	HWND window;
	HWND promptText;
	HWND placementAnchor;
	HFONT font;
	MSG message;
	int y = 36;

	if (!Title || !Prompt || !Result)
		return FALSE;
	placementAnchor = Owner ? Owner : GetForegroundWindow();
	ZeroMemory(Result, sizeof(*Result));
	state.Prompt = Prompt;
	state.MinBytes = MinBytes;
	state.AllowForgot = AllowForgot;
	state.AllowUnconfigured = AllowUnconfigured;
	state.Confirm = Confirm;
	state.Result = Result;

	windowClass.lpfnWndProc = CdpPromptWindowProc;
	windowClass.hInstance = GetModuleHandleW(NULL);
	windowClass.hCursor = LoadCursorW(NULL, IDC_ARROW);
	windowClass.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
	windowClass.lpszClassName = L"CdpSecurePrompt";
	RegisterClassW(&windowClass);

	window = CreateWindowExW(
		WS_EX_DLGMODALFRAME | WS_EX_TOPMOST | WS_EX_APPWINDOW,
		windowClass.lpszClassName,
		Title,
		WS_CAPTION | WS_SYSMENU,
		CW_USEDEFAULT, CW_USEDEFAULT, 420, Confirm ? 220 : 175,
		Owner,
		NULL,
		windowClass.hInstance,
		&state);
	if (!window)
		return FALSE;
	font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
	promptText = CreateWindowExW(0, L"STATIC", Prompt, WS_CHILD | WS_VISIBLE,
		16, 12, 380, 36, window, NULL, windowClass.hInstance, NULL);
	SendMessageW(promptText, WM_SETFONT, (WPARAM)font, TRUE);
	state.Edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
		WS_CHILD | WS_VISIBLE | ES_PASSWORD | ES_AUTOHSCROLL,
		16, 52, 380, 24, window, (HMENU)CDP_PROMPT_EDIT, windowClass.hInstance, NULL);
	SendMessageW(state.Edit, WM_SETFONT, (WPARAM)font, TRUE);
	SendMessageW(state.Edit, EM_SETLIMITTEXT, 128, 0);
	y = 84;
	if (Confirm)
	{
		state.ConfirmEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
			WS_CHILD | WS_VISIBLE | ES_PASSWORD | ES_AUTOHSCROLL,
			16, 84, 380, 24, window, (HMENU)CDP_PROMPT_CONFIRM, windowClass.hInstance, NULL);
		SendMessageW(state.ConfirmEdit, WM_SETFONT, (WPARAM)font, TRUE);
		SendMessageW(state.ConfirmEdit, EM_SETLIMITTEXT, 128, 0);
		y = 116;
	}
	CreateWindowExW(0, L"BUTTON", L"确定", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
		16, y, 88, 26, window, (HMENU)IDOK, windowClass.hInstance, NULL);
	CreateWindowExW(0, L"BUTTON", L"取消", WS_CHILD | WS_VISIBLE,
		112, y, 88, 26, window, (HMENU)IDCANCEL, windowClass.hInstance, NULL);
	if (AllowForgot)
		CreateWindowExW(0, L"BUTTON", L"忘记密码", WS_CHILD | WS_VISIBLE,
			208, y, 88, 26, window, (HMENU)CDP_PROMPT_FORGOT, windowClass.hInstance, NULL);
	CdpBringWindowToFront(window);
	/* 首次 ShowWindow 之后再定位，防止 CW_USEDEFAULT 覆盖预先设置的位置。 */
	CdpCenterPromptWindow(window, placementAnchor);
	if (state.Edit)
		SetFocus(state.Edit);
	while (!state.Finished && GetMessageW(&message, NULL, 0, 0) > 0)
	{
		if (!IsDialogMessageW(window, &message))
		{
			TranslateMessage(&message);
			DispatchMessageW(&message);
		}
	}
	return state.Accepted;
}
