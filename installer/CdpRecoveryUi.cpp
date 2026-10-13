/*
 * 安装器找回界面。
 * 二维码在本进程里绘制。凭据导入和重设口令使用同一个 CdpDriver 句柄。
 * 不把口令、找回 JSON 或凭据写进日志。
 */

#include "CdpRecoveryUi.h"

#include "../CdpDriver/CdpIoctl.h"

#include <commdlg.h>
#include <string.h>
#include <strsafe.h>

extern "C" {
#include "third_party/qrcodegen/qrcodegen.h"
}

#define CDP_RECOVERY_URL 201
#define CDP_RECOVERY_BUILD 202
#define CDP_RECOVERY_IMPORT 203
#define CDP_RECOVERY_CANVAS 204

typedef struct _CDP_RECOVERY_STATE {
	CDP_SECURE_PASSWORD *NewPassword;
	HANDLE Driver;
	HWND UrlEdit;
	HWND Canvas;
	HBITMAP Bitmap;
	BOOL Finished;
	BOOL Accepted;
} CDP_RECOVERY_STATE;

/* 打开 CdpDriver。找回期间一直握着这个句柄，避免 nonce 丢失。 */
static HANDLE CdpRecoveryOpenDriver(void)
{
	return CreateFileW(
		Cdp_CONTROL_SYSTEM_LINK_NAME,
		GENERIC_READ | GENERIC_WRITE,
		0,
		NULL,
		OPEN_EXISTING,
		FILE_ATTRIBUTE_NORMAL,
		NULL);
}

/* 把用户输入的服务地址补成驱动要求的扫码前缀。 */
static BOOL CdpRecoveryMakePrefix(const wchar_t *Url, char *Prefix, ULONG PrefixCap)
{
	wchar_t trimmed[256];
	char utf8[256];
	int bytes;
	size_t length;
	const char *marker;

	if (!Url || !Prefix || PrefixCap < 16)
		return FALSE;
	trimmed[0] = L'\0';
	StringCchCopyW(trimmed, _countof(trimmed), Url);
	bytes = WideCharToMultiByte(CP_UTF8, 0, trimmed, -1, utf8, sizeof(utf8), NULL, NULL);
	SecureZeroMemory(trimmed, sizeof(trimmed));
	if (bytes <= 1)
		return FALSE;
	length = (size_t)(bytes - 1);
	marker = strstr(utf8, "#c=");
	if (!marker)
		marker = strstr(utf8, "?c=");
	if (marker)
	{
		if (length + 1 > PrefixCap)
			return FALSE;
		memcpy(Prefix, utf8, length + 1);
		return TRUE;
	}
	if (utf8[length - 1] == '/')
		utf8[--length] = 0;
	if (length + strlen("/v1/recovery#c=") + 1 > PrefixCap)
		return FALSE;
	memcpy(Prefix, utf8, length);
	memcpy(Prefix + length, "/v1/recovery#c=", strlen("/v1/recovery#c=") + 1);
	return TRUE;
}

/* 用二维码模块画一张位图。画不出来时调用方仍可复制 URI。 */
static HBITMAP CdpRecoveryMakeBitmap(const char *Text)
{
	uint8_t *qr = NULL;
	uint8_t *temp = NULL;
	HBITMAP bitmap = NULL;
	BITMAPINFO info;
	void *bits = NULL;
	HDC screen;
	int size;
	int scale;
	int quiet = 4;
	int pixels;
	int y;
	BOOL encoded;

	qr = (uint8_t *)HeapAlloc(GetProcessHeap(), 0, qrcodegen_BUFFER_LEN_MAX);
	temp = (uint8_t *)HeapAlloc(GetProcessHeap(), 0, qrcodegen_BUFFER_LEN_MAX);
	if (!qr || !temp)
		goto done;
	encoded = qrcodegen_encodeText(
		Text,
		temp,
		qr,
		qrcodegen_Ecc_LOW,
		1,
		40,
		qrcodegen_Mask_AUTO,
		TRUE);
	if (!encoded)
		goto done;
	size = qrcodegen_getSize(qr);
	scale = 280 / (size + quiet * 2);
	if (scale < 2)
		scale = 2;
	pixels = (size + quiet * 2) * scale;
	ZeroMemory(&info, sizeof(info));
	info.bmiHeader.biSize = sizeof(info.bmiHeader);
	info.bmiHeader.biWidth = pixels;
	info.bmiHeader.biHeight = -pixels;
	info.bmiHeader.biPlanes = 1;
	info.bmiHeader.biBitCount = 32;
	screen = GetDC(NULL);
	bitmap = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &bits, NULL, 0);
	ReleaseDC(NULL, screen);
	if (!bitmap || !bits)
		goto done;
	for (y = 0; y < pixels; ++y)
	{
		int x;
		DWORD *row = (DWORD *)bits + y * pixels;
		int moduleY = y / scale - quiet;
		for (x = 0; x < pixels; ++x)
		{
			int moduleX = x / scale - quiet;
			BOOL dark = moduleX >= 0 && moduleY >= 0 && moduleX < size && moduleY < size &&
				qrcodegen_getModule(qr, moduleX, moduleY);
			row[x] = dark ? 0x00000000 : 0x00FFFFFF;
		}
	}
done:
	if (qr)
		HeapFree(GetProcessHeap(), 0, qr);
	if (temp)
		HeapFree(GetProcessHeap(), 0, temp);
	return bitmap;
}

/* 生成找回二维码并显示。失败时不留下半截句柄状态。 */
static BOOL CdpRecoveryBuild(CDP_RECOVERY_STATE *State, HWND Window)
{
	Cdp_BUILD_RECOVERY_QR_REQUEST request;
	Cdp_RECOVERY_QR_REPLY *reply = NULL;
	wchar_t url[256];
	char prefix[Cdp_RECOVERY_QR_PREFIX_MAX];
	DWORD returned = 0;
	BOOL ok = FALSE;
	HBITMAP bitmap;

	if (State->Driver == INVALID_HANDLE_VALUE)
	{
		State->Driver = CdpRecoveryOpenDriver();
		if (State->Driver == INVALID_HANDLE_VALUE)
		{
			MessageBoxW(Window, L"打不开 CdpDriver。请先关闭正在运行的源点恢复。",
				L"找回密码", MB_OK | MB_ICONWARNING);
			return FALSE;
		}
	}
	url[0] = L'\0';
	GetWindowTextW(State->UrlEdit, url, _countof(url));
	ZeroMemory(&request, sizeof(request));
	ZeroMemory(prefix, sizeof(prefix));
	if (!CdpRecoveryMakePrefix(url, prefix, sizeof(prefix)))
	{
		MessageBoxW(Window, L"授权服务地址无效。", L"找回密码", MB_OK | MB_ICONWARNING);
		return FALSE;
	}
	request.Size = sizeof(request);
	request.Version = Cdp_BUILD_RECOVERY_QR_REQUEST_VERSION;
	memcpy(request.QrPrefix, prefix, sizeof(prefix));
	reply = (Cdp_RECOVERY_QR_REPLY *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*reply));
	if (!reply)
		return FALSE;
	ok = DeviceIoControl(
		State->Driver,
		IOCTL_Cdp_BUILD_RECOVERY_QR,
		&request,
		sizeof(request),
		reply,
		sizeof(*reply),
		&returned,
		NULL);
	SecureZeroMemory(&request, sizeof(request));
	SecureZeroMemory(prefix, sizeof(prefix));
	if (!ok || returned < sizeof(*reply) || reply->QrPayload[0] == 0)
	{
		DWORD error = GetLastError();
		wchar_t text[128];
		StringCchPrintfW(text, _countof(text), L"生成找回二维码失败，错误码 %lu。", error);
		MessageBoxW(Window, text, L"找回密码", MB_OK | MB_ICONWARNING);
		ok = FALSE;
		goto done;
	}
	bitmap = CdpRecoveryMakeBitmap(reply->QrPayload);
	if (State->Bitmap)
		DeleteObject(State->Bitmap);
	State->Bitmap = bitmap;
	SendMessageW(State->Canvas, STM_SETIMAGE, IMAGE_BITMAP, (LPARAM)bitmap);
	if (!bitmap)
		MessageBoxW(Window, L"二维码内容过长，未能画出。请缩短服务地址后重试。",
			L"找回密码", MB_OK | MB_ICONINFORMATION);
	ok = TRUE;
done:
	if (reply)
	{
		SecureZeroMemory(reply, sizeof(*reply));
		HeapFree(GetProcessHeap(), 0, reply);
	}
	return ok;
}

/* 导入用户选中的凭据文件，并在当前句柄上改成新口令。 */
static BOOL CdpRecoveryImport(CDP_RECOVERY_STATE *State, HWND Window)
{
	OPENFILENAMEW openName;
	wchar_t path[MAX_PATH];
	HANDLE file;
	Cdp_IMPORT_RECOVERY_CREDENTIAL_REQUEST request;
	Cdp_RECOVERY_CHANGE_PASSWORD_REQUEST change;
	CDP_PROMPT_RESULT prompt;
	DWORD readBytes = 0;
	DWORD returned = 0;
	BOOL ok = FALSE;

	if (State->Driver == INVALID_HANDLE_VALUE)
	{
		MessageBoxW(Window, L"请先生成找回二维码。", L"找回密码", MB_OK | MB_ICONWARNING);
		return FALSE;
	}
	ZeroMemory(&openName, sizeof(openName));
	path[0] = L'\0';
	openName.lStructSize = sizeof(openName);
	openName.hwndOwner = Window;
	openName.lpstrFilter = L"找回凭据\0*.bin;*.cdp\0所有文件\0*.*\0";
	openName.lpstrFile = path;
	openName.nMaxFile = _countof(path);
	openName.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
	if (!GetOpenFileNameW(&openName))
		return FALSE;
	file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (file == INVALID_HANDLE_VALUE)
		return FALSE;
	ZeroMemory(&request, sizeof(request));
	if (!ReadFile(file, request.Credential, sizeof(request.Credential), &readBytes, NULL) || readBytes == 0)
	{
		CloseHandle(file);
		SecureZeroMemory(&request, sizeof(request));
		MessageBoxW(Window, L"凭据文件为空或无法读取。", L"找回密码", MB_OK | MB_ICONWARNING);
		return FALSE;
	}
	CloseHandle(file);
	request.Size = sizeof(request);
	request.Version = Cdp_IMPORT_RECOVERY_CREDENTIAL_REQUEST_VERSION;
	request.CredentialLength = readBytes;
	ok = DeviceIoControl(
		State->Driver,
		IOCTL_Cdp_IMPORT_RECOVERY_CREDENTIAL,
		&request,
		sizeof(request),
		NULL,
		0,
		&returned,
		NULL);
	SecureZeroMemory(&request, sizeof(request));
	if (!ok)
	{
		wchar_t text[128];
		StringCchPrintfW(text, _countof(text), L"导入凭据失败，错误码 %lu。", GetLastError());
		MessageBoxW(Window, text, L"找回密码", MB_OK | MB_ICONWARNING);
		return FALSE;
	}
	ZeroMemory(&prompt, sizeof(prompt));
	if (!CdpPromptPassword(
		Window,
		L"设置新密码",
		L"凭据已接受。请输入新的保护密码，至少 6 字节。",
		6,
		FALSE,
		FALSE,
		TRUE,
		&prompt))
	{
		return FALSE;
	}
	ZeroMemory(&change, sizeof(change));
	change.Size = sizeof(change);
	change.Version = Cdp_RECOVERY_CHANGE_PASSWORD_REQUEST_VERSION;
	change.PasswordLength = prompt.Password.Bytes;
	memcpy(change.Password, prompt.Password.Utf8, prompt.Password.Bytes);
	ok = DeviceIoControl(
		State->Driver,
		IOCTL_Cdp_CHANGE_PASSWORD_WITH_RECOVERY,
		&change,
		sizeof(change),
		NULL,
		0,
		&returned,
		NULL);
	SecureZeroMemory(&change, sizeof(change));
	if (!ok)
	{
		wchar_t text[160];
		StringCchPrintfW(text, _countof(text),
			L"重设密码失败，错误码 %lu。可以再次导入后重试。", GetLastError());
		CdpSecurePasswordClear(&prompt.Password);
		MessageBoxW(Window, text, L"找回密码", MB_OK | MB_ICONWARNING);
		return FALSE;
	}
	*State->NewPassword = prompt.Password;
	CdpSecurePasswordClear(&prompt.Password);
	State->Accepted = TRUE;
	State->Finished = TRUE;
	MessageBoxW(Window, L"新密码已写入全部在线 Journal。请用新密码继续。",
		L"找回密码", MB_OK | MB_ICONINFORMATION);
	DestroyWindow(Window);
	return TRUE;
}

/* 找回窗口过程。关闭时不把未完成的新口令交出去。 */
static LRESULT CALLBACK CdpRecoveryWindowProc(HWND Window, UINT Message, WPARAM WParam, LPARAM LParam)
{
	CDP_RECOVERY_STATE *state = (CDP_RECOVERY_STATE *)GetWindowLongPtrW(Window, GWLP_USERDATA);

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
		if (LOWORD(WParam) == CDP_RECOVERY_BUILD)
			CdpRecoveryBuild(state, Window);
		else if (LOWORD(WParam) == CDP_RECOVERY_IMPORT)
			CdpRecoveryImport(state, Window);
		else if (LOWORD(WParam) == IDCANCEL)
		{
			state->Finished = TRUE;
			DestroyWindow(Window);
		}
		return 0;
	case WM_CLOSE:
		state->Finished = TRUE;
		DestroyWindow(Window);
		return 0;
	default:
		break;
	}
	return DefWindowProcW(Window, Message, WParam, LParam);
}

/* 打开找回窗口。成功后 NewPassword 持有新口令。 */
BOOL CdpRunPasswordRecovery(CDP_SECURE_PASSWORD *NewPassword)
{
	WNDCLASSW windowClass = {};
	CDP_RECOVERY_STATE state = {};
	HWND window;
	HFONT font;
	MSG message;

	if (!NewPassword)
		return FALSE;
	CdpSecurePasswordClear(NewPassword);
	state.NewPassword = NewPassword;
	state.Driver = INVALID_HANDLE_VALUE;
	windowClass.lpfnWndProc = CdpRecoveryWindowProc;
	windowClass.hInstance = GetModuleHandleW(NULL);
	windowClass.hCursor = LoadCursorW(NULL, IDC_ARROW);
	windowClass.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
	windowClass.lpszClassName = L"CdpRecoveryUi";
	RegisterClassW(&windowClass);
	window = CreateWindowExW(
		WS_EX_DLGMODALFRAME | WS_EX_TOPMOST | WS_EX_APPWINDOW,
		windowClass.lpszClassName,
		L"找回保护密码",
		WS_CAPTION | WS_SYSMENU,
		CW_USEDEFAULT, CW_USEDEFAULT, 460, 520,
		NULL, NULL, windowClass.hInstance, &state);
	if (!window)
		return FALSE;
	font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
	CreateWindowExW(0, L"STATIC", L"授权服务地址", WS_CHILD | WS_VISIBLE,
		16, 12, 420, 18, window, NULL, windowClass.hInstance, NULL);
	state.UrlEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"http://127.0.0.1:8080",
		WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
		16, 32, 420, 24, window, (HMENU)CDP_RECOVERY_URL, windowClass.hInstance, NULL);
	SendMessageW(state.UrlEdit, WM_SETFONT, (WPARAM)font, TRUE);
	CreateWindowExW(0, L"BUTTON", L"生成二维码", WS_CHILD | WS_VISIBLE,
		16, 64, 120, 26, window, (HMENU)CDP_RECOVERY_BUILD, windowClass.hInstance, NULL);
	CreateWindowExW(0, L"BUTTON", L"导入凭据", WS_CHILD | WS_VISIBLE,
		144, 64, 120, 26, window, (HMENU)CDP_RECOVERY_IMPORT, windowClass.hInstance, NULL);
	CreateWindowExW(0, L"BUTTON", L"取消", WS_CHILD | WS_VISIBLE,
		272, 64, 88, 26, window, (HMENU)IDCANCEL, windowClass.hInstance, NULL);
	state.Canvas = CreateWindowExW(0, L"STATIC", NULL, WS_CHILD | WS_VISIBLE | SS_BITMAP | SS_CENTERIMAGE,
		16, 104, 420, 360, window, (HMENU)CDP_RECOVERY_CANVAS, windowClass.hInstance, NULL);
	CdpBringWindowToFront(window);
	if (state.UrlEdit)
		SetFocus(state.UrlEdit);
	while (!state.Finished && GetMessageW(&message, NULL, 0, 0) > 0)
	{
		if (!IsDialogMessageW(window, &message))
		{
			TranslateMessage(&message);
			DispatchMessageW(&message);
		}
	}
	if (state.Driver != INVALID_HANDLE_VALUE)
		CloseHandle(state.Driver);
	if (state.Bitmap)
		DeleteObject(state.Bitmap);
	if (!state.Accepted)
		CdpSecurePasswordClear(NewPassword);
	return state.Accepted;
}
