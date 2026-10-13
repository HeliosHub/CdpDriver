/*
 * 安装或卸载随包的 CdpProtect。
 * INF 位于 Helper 旁的 driver\protect\CdpProtect.inf。
 */

#include <Windows.h>
#include <stdio.h>
#include <strsafe.h>

#define CDP_DIIRFLAG_FORCE_INF 0x00000002u

typedef BOOL (WINAPI *CDP_DI_INSTALL_DRIVER_W)(
	HWND Window,
	LPCWSTR InfPath,
	DWORD Flags,
	PBOOL NeedReboot);

typedef BOOL (WINAPI *CDP_DI_UNINSTALL_DRIVER_W)(
	HWND Window,
	LPCWSTR InfPath,
	DWORD Flags,
	PBOOL NeedReboot);

/* 取 Helper 所在目录。 */
static BOOL CdpPackageExeDir(wchar_t *Dir, size_t CchDir)
{
	DWORD length;
	wchar_t *slash;

	Dir[0] = L'\0';
	length = GetModuleFileNameW(NULL, Dir, (DWORD)CchDir);
	if (!length || length >= CchDir)
		return FALSE;
	slash = wcsrchr(Dir, L'\\');
	if (!slash)
		return FALSE;
	*slash = L'\0';
	return TRUE;
}

/* 调用 SetupAPI 执行 INF 节。节名不带 .NTamd64，64 位系统会自行加上。 */
static BOOL CdpRunInfSection(const wchar_t *InfPath, const wchar_t *Section)
{
	HMODULE setup;
	void (WINAPI *install)(HWND, HINSTANCE, PCWSTR, INT);
	wchar_t command[MAX_PATH + 64];

	setup = LoadLibraryW(L"setupapi.dll");
	if (!setup)
		return FALSE;
	install = (void (WINAPI *)(HWND, HINSTANCE, PCWSTR, INT))
		GetProcAddress(setup, "InstallHinfSectionW");
	if (!install)
	{
		FreeLibrary(setup);
		return FALSE;
	}
	/* InstallHinfSectionW 把第三项作为命令行解析；安装目录含
	 * “Program Files” 时必须引用 INF 路径，否则会被截为 C:\\Program。 */
	if (FAILED(StringCchPrintfW(
		command, _countof(command), L"%s 132 \"%s\"", Section, InfPath)))
	{
		FreeLibrary(setup);
		SetLastError(ERROR_FILENAME_EXCED_RANGE);
		return FALSE;
	}
	install(NULL, NULL, command, 0);
	FreeLibrary(setup);
	return TRUE;
}

/* Primitive MiniFilter 驱动包必须通过驱动包安装接口部署。旧的
 * InstallHinfSection 在不同 Windows 版本上的失败返回不可靠。 */
static BOOL CdpInstallInfPackage(const wchar_t *InfPath)
{
	HMODULE newdev;
	CDP_DI_INSTALL_DRIVER_W install;
	BOOL needReboot = FALSE;
	BOOL ok;
	DWORD error;

	newdev = LoadLibraryW(L"newdev.dll");
	if (!newdev)
		return FALSE;
	install = (CDP_DI_INSTALL_DRIVER_W)GetProcAddress(newdev, "DiInstallDriverW");
	if (!install)
	{
		error = GetLastError();
		FreeLibrary(newdev);
		SetLastError(error);
		return FALSE;
	}
	ok = install(NULL, InfPath, CDP_DIIRFLAG_FORCE_INF, &needReboot);
	error = ok ? ERROR_SUCCESS : GetLastError();
	FreeLibrary(newdev);
	if (!ok)
		SetLastError(error ? error : ERROR_GEN_FAILURE);
	return ok;
}

/* 与安装对应地移除 Primitive 驱动包，并把真实 Win32 错误返回给安装器。 */
static BOOL CdpUninstallInfPackage(const wchar_t *InfPath)
{
	HMODULE newdev;
	CDP_DI_UNINSTALL_DRIVER_W uninstall;
	BOOL needReboot = FALSE;
	BOOL ok;
	DWORD error;

	newdev = LoadLibraryW(L"newdev.dll");
	if (!newdev)
		return FALSE;
	uninstall = (CDP_DI_UNINSTALL_DRIVER_W)GetProcAddress(
		newdev, "DiUninstallDriverW");
	if (!uninstall)
	{
		error = GetLastError();
		FreeLibrary(newdev);
		SetLastError(error);
		return FALSE;
	}
	ok = uninstall(NULL, InfPath, 0, &needReboot);
	error = ok ? ERROR_SUCCESS : GetLastError();
	FreeLibrary(newdev);
	if (!ok)
		SetLastError(error ? error : ERROR_GEN_FAILURE);
	return ok;
}

/* 解析 protect 包里的 INF。 */
static BOOL CdpResolveProtectInf(wchar_t *InfPath, size_t CchInf)
{
	wchar_t dir[MAX_PATH];

	if (!CdpPackageExeDir(dir, _countof(dir)))
		return FALSE;
	if (FAILED(StringCchPrintfW(InfPath, CchInf, L"%s\\driver\\protect\\CdpProtect.inf", dir)))
		return FALSE;
	return GetFileAttributesW(InfPath) != INVALID_FILE_ATTRIBUTES;
}

/* 安装 CdpProtect MiniFilter。 */
BOOL CdpInstallProtectPackage(void)
{
	wchar_t infPath[MAX_PATH];

	if (!CdpResolveProtectInf(infPath, _countof(infPath)))
	{
		fwprintf(stderr, L"CdpProtect.inf was not found.\n");
		SetLastError(ERROR_FILE_NOT_FOUND);
		return FALSE;
	}
	if (!CdpInstallInfPackage(infPath))
	{
		fwprintf(stderr, L"CdpProtect INF install failed (Win32=%lu): %s\n",
			GetLastError(), infPath);
		return FALSE;
	}
	return TRUE;
}

/* 卸载 CdpProtect。调用前维护会话必须已经武装卸载窗口。 */
BOOL CdpUninstallProtectPackage(void)
{
	wchar_t infPath[MAX_PATH];

	if (!CdpResolveProtectInf(infPath, _countof(infPath)))
		return TRUE;
	return CdpUninstallInfPackage(infPath);
}
