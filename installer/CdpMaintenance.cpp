/*
 * 维护授权。
 * Helper 收集口令后调用唯一的 CdpProtect IOCTL，并把 NSIS 父进程作为 TargetPid。
 */

#include "CdpMaintenance.h"

#include "../../cdp-protect-drv/include/CdpProtectIoctl.h"

#include <string.h>
#include <strsafe.h>

typedef LONG NTSTATUS;
typedef NTSTATUS (NTAPI *NT_QUERY_INFORMATION_PROCESS)(
	HANDLE Process,
	ULONG ProcessInformationClass,
	PVOID ProcessInformation,
	ULONG ProcessInformationLength,
	PULONG ReturnLength);

typedef struct _CDP_PROCESS_BASIC_INFORMATION {
	PVOID Reserved1;
	PVOID PebBaseAddress;
	PVOID Reserved2[2];
	ULONG_PTR UniqueProcessId;
	ULONG_PTR InheritedFromUniqueProcessId;
} CDP_PROCESS_BASIC_INFORMATION;

/* 打开 CdpProtect 控制设备。 */
static HANDLE CdpOpenProtectDevice(void)
{
	return CreateFileW(
		CDPP_CONTROL_USER_NAME,
		GENERIC_READ | GENERIC_WRITE,
		FILE_SHARE_READ | FILE_SHARE_WRITE,
		NULL,
		OPEN_EXISTING,
		FILE_ATTRIBUTE_NORMAL,
		NULL);
}

/* 取父进程 PID。失败时返回 0，调用方就只授权自己。 */
static DWORD CdpQueryParentProcessId(void)
{
	HMODULE ntdll;
	NT_QUERY_INFORMATION_PROCESS query;
	CDP_PROCESS_BASIC_INFORMATION info;
	ULONG returned = 0;

	ntdll = GetModuleHandleW(L"ntdll.dll");
	if (!ntdll)
		return 0;
	query = (NT_QUERY_INFORMATION_PROCESS)GetProcAddress(
		ntdll, "NtQueryInformationProcess");
	if (!query)
		return 0;
	ZeroMemory(&info, sizeof(info));
	if (query(GetCurrentProcess(), 0, &info, sizeof(info), &returned) < 0)
		return 0;
	if (info.InheritedFromUniqueProcessId > MAXDWORD)
		return 0;
	return (DWORD)info.InheritedFromUniqueProcessId;
}

/* 发布包名为 RecoverySetup-x64-主.次.构建.exe；只接受三段纯数字版本，
 * 不能把任意带同一前缀的程序纳入维护会话。 */
static BOOL CdpIsVersionedSetupName(const wchar_t* Name)
{
	static const wchar_t prefix[] = L"RecoverySetup-x64-";
	static const wchar_t extension[] = L".exe";
	const size_t prefixChars = _countof(prefix) - 1;
	const size_t extensionChars = _countof(extension) - 1;
	size_t nameChars;
	size_t versionChars;
	size_t index;
	ULONG dots = 0;
	BOOL previousWasDigit = FALSE;

	if (!Name || _wcsnicmp(Name, prefix, prefixChars) != 0)
		return FALSE;
	nameChars = wcslen(Name);
	if (nameChars <= prefixChars + extensionChars ||
		_wcsicmp(Name + nameChars - extensionChars, extension) != 0)
		return FALSE;
	versionChars = nameChars - prefixChars - extensionChars;
	for (index = 0; index < versionChars; ++index)
	{
		wchar_t ch = Name[prefixChars + index];
		if (ch >= L'0' && ch <= L'9')
		{
			previousWasDigit = TRUE;
			continue;
		}
		if (ch != L'.' || !previousWasDigit)
			return FALSE;
		dots++;
		previousWasDigit = FALSE;
	}
	return previousWasDigit && dots == 2;
}

/* 父进程路径是否像正式安装包或卸载器。 */
static BOOL CdpParentIsInstaller(DWORD ParentPid)
{
	HANDLE process;
	wchar_t path[MAX_PATH];
	DWORD size = _countof(path);
	BOOL match = FALSE;

	if (!ParentPid)
		return FALSE;
	process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, ParentPid);
	if (!process)
		return FALSE;
	path[0] = L'\0';
	if (QueryFullProcessImageNameW(process, 0, path, &size))
	{
		size_t length = wcslen(path);
		const wchar_t *setup = L"\\RecoverySetup-x64.exe";
		const wchar_t *uninstall = L"\\Uninstall.exe";
		size_t setupLen = wcslen(setup);
		size_t uninstallLen = wcslen(uninstall);
		if (length >= setupLen && _wcsicmp(path + length - setupLen, setup) == 0)
			match = TRUE;
		if (!match)
		{
			const wchar_t* fileName = wcsrchr(path, L'\\');
			match = CdpIsVersionedSetupName(fileName ? fileName + 1 : path);
		}
		if (length >= uninstallLen && _wcsicmp(path + length - uninstallLen, uninstall) == 0)
			match = TRUE;
	}
	CloseHandle(process);
	return match;
}

/* 发送口令。请求缓冲在返回前清零，调用方的 Password 不动。 */
static BOOL CdpSendPassphrase(
	ULONG Flags,
	const CDP_SECURE_PASSWORD *Password,
	UINT64 TargetPid,
	DWORD *Win32Error)
{
	CDPP_SET_PASSPHRASE_REQUEST request;
	HANDLE device;
	DWORD returned = 0;
	BOOL ok;

	if (Win32Error)
		*Win32Error = ERROR_SUCCESS;
	ZeroMemory(&request, sizeof(request));
	request.Size = sizeof(request);
	request.Version = CDPP_AUTH_REQUEST_VERSION;
	request.Flags = Flags;
	request.PasswordBytes = Password ? Password->Bytes : 0;
	request.TargetProcessId = TargetPid;
	if (Password && Password->Bytes)
		memcpy(request.Password, Password->Utf8, Password->Bytes);
	device = CdpOpenProtectDevice();
	if (device == INVALID_HANDLE_VALUE)
	{
		SecureZeroMemory(&request, sizeof(request));
		if (Win32Error)
			*Win32Error = GetLastError();
		return FALSE;
	}
	ok = DeviceIoControl(
		device,
		IOCTL_CDPP_SET_PASSPHRASE,
		&request,
		sizeof(request),
		NULL,
		0,
		&returned,
		NULL);
	if (!ok && Win32Error)
		*Win32Error = GetLastError();
	SecureZeroMemory(&request, sizeof(request));
	CloseHandle(device);
	return ok;
}

/* 安装器父进程存在时授权它，否则只授权 Helper。 */
BOOL CdpAuthorizeMaintenance(
	ULONG Flags,
	const CDP_SECURE_PASSWORD *Password,
	DWORD *Win32Error)
{
	DWORD parent = CdpQueryParentProcessId();
	UINT64 target = 0;

	if (CdpParentIsInstaller(parent))
		target = parent;
	return CdpSendPassphrase(Flags, Password, target, Win32Error);
}

/* 先问口令。尚未设密走空口令的 ALLOW_UNCONFIGURED。 */
BOOL CdpPromptAndAuthorize(
	ULONG Flags,
	BOOL AllowForgot,
	BOOL AllowUnconfigured,
	CDP_PROMPT_RESULT *Result,
	DWORD *Win32Error)
{
	ULONG minBytes = (Flags & CDPP_AUTH_ALLOW_UNCONFIGURED) ? 0 : 6;

	if (!CdpPromptPassword(
		NULL,
		L"源点恢复维护",
		L"请输入当前保护密码。密码不会出现在命令行或日志中。",
		minBytes,
		AllowForgot,
		AllowUnconfigured,
		FALSE,
		Result))
	{
		return FALSE;
	}
	if (Result->Forgot)
		return TRUE;
	if (Result->Unconfigured)
	{
		CdpSecurePasswordClear(&Result->Password);
		return CdpAuthorizeMaintenance(
			CDPP_AUTH_MAINTENANCE | CDPP_AUTH_ALLOW_UNCONFIGURED,
			&Result->Password,
			Win32Error);
	}
	return CdpAuthorizeMaintenance(Flags, &Result->Password, Win32Error);
}
