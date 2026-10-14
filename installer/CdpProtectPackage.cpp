/*
 * 安装或卸载随包的 CdpProtect。
 * INF 位于 Helper 旁的 driver\protect\CdpProtect.inf。
 */

#include <Windows.h>
#include <wincrypt.h>
#include <stdio.h>
#include <string.h>
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

/* 计算文件 SHA-256；安装后以此确认真正由内核加载的系统副本已更新。 */
static BOOL CdpHashFileSha256(
	_In_z_ const wchar_t *Path,
	_Out_writes_(32) BYTE Digest[32])
{
	HANDLE file = INVALID_HANDLE_VALUE;
	HCRYPTPROV provider = 0;
	HCRYPTHASH hash = 0;
	BYTE buffer[64 * 1024];
	DWORD read = 0;
	DWORD digestBytes = 32;
	BOOL ok = FALSE;
	DWORD error = ERROR_SUCCESS;

	file = CreateFileW(Path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
		FILE_ATTRIBUTE_NORMAL, NULL);
	if (file == INVALID_HANDLE_VALUE)
		return FALSE;
	if (!CryptAcquireContextW(&provider, NULL, NULL, PROV_RSA_AES, CRYPT_VERIFYCONTEXT) ||
		!CryptCreateHash(provider, CALG_SHA_256, 0, 0, &hash))
	{
		error = GetLastError();
		goto done;
	}
	for (;;)
	{
		if (!ReadFile(file, buffer, sizeof(buffer), &read, NULL))
		{
			error = GetLastError();
			goto done;
		}
		if (read == 0)
			break;
		if (!CryptHashData(hash, buffer, read, 0))
		{
			error = GetLastError();
			goto done;
		}
	}
	if (!CryptGetHashParam(hash, HP_HASHVAL, Digest, &digestBytes, 0) ||
		digestBytes != 32)
	{
		error = GetLastError();
		goto done;
	}
	ok = TRUE;
done:
	if (hash)
		CryptDestroyHash(hash);
	if (provider)
		CryptReleaseContext(provider, 0);
	CloseHandle(file);
	if (!ok)
		SetLastError(error ? error : ERROR_GEN_FAILURE);
	return ok;
}

/* 校验 INF payload 与 Windows 实际加载路径中的驱动字节完全相同。 */
static BOOL CdpVerifyProtectSystemDriver(_In_z_ const wchar_t *InfPath)
{
	wchar_t sourcePath[MAX_PATH];
	wchar_t systemPath[MAX_PATH];
	wchar_t *slash;
	BYTE sourceDigest[32];
	BYTE systemDigest[32];
	DWORD length;

	if (FAILED(StringCchCopyW(sourcePath, _countof(sourcePath), InfPath)))
		return FALSE;
	slash = wcsrchr(sourcePath, L'\\');
	if (!slash)
	{
		SetLastError(ERROR_INVALID_NAME);
		return FALSE;
	}
	*(slash + 1) = L'\0';
	if (FAILED(StringCchCatW(sourcePath, _countof(sourcePath), L"CdpProtect.sys")))
		return FALSE;
	length = GetSystemDirectoryW(systemPath, _countof(systemPath));
	if (!length || length >= _countof(systemPath) ||
		FAILED(StringCchCatW(systemPath, _countof(systemPath), L"\\drivers\\CdpProtect.sys")))
	{
		if (GetLastError() == ERROR_SUCCESS)
			SetLastError(ERROR_FILENAME_EXCED_RANGE);
		return FALSE;
	}
	if (!CdpHashFileSha256(sourcePath, sourceDigest) ||
		!CdpHashFileSha256(systemPath, systemDigest))
		return FALSE;
	if (memcmp(sourceDigest, systemDigest, sizeof(sourceDigest)) != 0)
	{
		SetLastError(ERROR_CRC);
		return FALSE;
	}
	return TRUE;
}

static BOOL CdpProtectServiceExists(void)
{
	SC_HANDLE manager = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
	SC_HANDLE service;
	DWORD error;

	if (!manager)
		return FALSE;
	service = OpenServiceW(manager, L"CdpProtect", SERVICE_QUERY_STATUS);
	if (!service)
	{
		error = GetLastError();
		CloseServiceHandle(manager);
		SetLastError(error);
		return FALSE;
	}
	CloseServiceHandle(service);
	CloseServiceHandle(manager);
	return TRUE;
}

static BOOL CdpReadProtectAltitude(
	_In_z_ const wchar_t *InfPath,
	_Out_writes_(CchAltitude) wchar_t *Altitude,
	_In_ DWORD CchAltitude)
{
	wchar_t value[64];
	DWORD length = GetPrivateProfileStringW(
		L"Strings", L"Instance1.Altitude", L"", value,
		_countof(value), InfPath);

	if (!length || length >= _countof(value))
	{
		SetLastError(ERROR_INVALID_DATA);
		return FALSE;
	}
	if (value[0] == L'"' && length >= 2 && value[length - 1] == L'"')
	{
		value[length - 1] = L'\0';
		return SUCCEEDED(StringCchCopyW(Altitude, CchAltitude, value + 1));
	}
	return SUCCEEDED(StringCchCopyW(Altitude, CchAltitude, value));
}

static BOOL CdpSetRegString(HKEY Key, const wchar_t *Name, const wchar_t *Value)
{
	LONG result = RegSetValueExW(
		Key, Name, 0, REG_SZ, (const BYTE *)Value,
		(DWORD)((wcslen(Value) + 1) * sizeof(wchar_t)));
	if (result != ERROR_SUCCESS)
	{
		SetLastError((DWORD)result);
		return FALSE;
	}
	return TRUE;
}

/* MiniFilter 首装也沿用 CdpDriver 的手工服务方式，避免 SetupAPI 已完成复制
 * 却返回失败。Instances 是 MiniFilter 启动所必需，Altitude 从随包 INF 读取。 */
static BOOL CdpEnsureProtectService(_In_z_ const wchar_t *InfPath)
{
	wchar_t systemDirectory[MAX_PATH];
	wchar_t driverPath[MAX_PATH];
	wchar_t altitude[32];
	SC_HANDLE manager = NULL;
	SC_HANDLE service = NULL;
	HKEY parameters = NULL;
	HKEY instances = NULL;
	HKEY instance = NULL;
	DWORD disposition;
	DWORD zero = 0;
	DWORD type;
	DWORD bytes;
	LONG result;
	BOOL ok = FALSE;

	if (!GetSystemDirectoryW(systemDirectory, _countof(systemDirectory)) ||
		FAILED(StringCchPrintfW(driverPath, _countof(driverPath),
			L"%s\\drivers\\CdpProtect.sys", systemDirectory)) ||
		!CdpReadProtectAltitude(InfPath, altitude, _countof(altitude)))
		return FALSE;

	manager = OpenSCManagerW(NULL, NULL,
		SC_MANAGER_CONNECT | SC_MANAGER_CREATE_SERVICE);
	if (!manager)
		goto done;
	service = CreateServiceW(
		manager, L"CdpProtect", L"CdpProtect",
		SERVICE_QUERY_STATUS | SERVICE_CHANGE_CONFIG,
		SERVICE_FILE_SYSTEM_DRIVER, SERVICE_BOOT_START, SERVICE_ERROR_NORMAL,
		driverPath, L"FSFilter Activity Monitor", NULL, L"FltMgr\0",
		NULL, NULL);
	if (!service && GetLastError() == ERROR_SERVICE_EXISTS)
		service = OpenServiceW(manager, L"CdpProtect",
			SERVICE_QUERY_STATUS | SERVICE_CHANGE_CONFIG);
	if (!service)
		goto done;
	if (!ChangeServiceConfigW(
		service, SERVICE_FILE_SYSTEM_DRIVER, SERVICE_BOOT_START,
		SERVICE_ERROR_NORMAL, driverPath, L"FSFilter Activity Monitor",
		NULL, L"FltMgr\0", NULL, NULL, L"CdpProtect"))
		goto done;

	result = RegCreateKeyExW(HKEY_LOCAL_MACHINE,
		L"SYSTEM\\CurrentControlSet\\Services\\CdpProtect\\Parameters",
		0, NULL, 0, KEY_QUERY_VALUE | KEY_SET_VALUE, NULL, &parameters,
		&disposition);
	if (result != ERROR_SUCCESS)
	{
		SetLastError((DWORD)result);
		goto done;
	}
	bytes = sizeof(zero);
	type = REG_DWORD;
	result = RegQueryValueExW(parameters, L"AuthInitialized", NULL,
		&type, (BYTE *)&zero, &bytes);
	if (result == ERROR_FILE_NOT_FOUND)
	{
		zero = 0;
		result = RegSetValueExW(parameters, L"AuthInitialized", 0,
			REG_DWORD, (const BYTE *)&zero, sizeof(zero));
	}
	if (result != ERROR_SUCCESS)
	{
		SetLastError((DWORD)result);
		goto done;
	}
	zero = 0;
	result = RegSetValueExW(parameters, L"DebugFlags", 0, REG_DWORD,
		(const BYTE *)&zero, sizeof(zero));
	if (result != ERROR_SUCCESS)
	{
		SetLastError((DWORD)result);
		goto done;
	}

	result = RegCreateKeyExW(HKEY_LOCAL_MACHINE,
		L"SYSTEM\\CurrentControlSet\\Services\\CdpProtect\\Instances",
		0, NULL, 0, KEY_SET_VALUE, NULL, &instances, &disposition);
	if (result != ERROR_SUCCESS ||
		!CdpSetRegString(instances, L"DefaultInstance", L"CdpProtect Instance"))
	{
		if (result != ERROR_SUCCESS)
			SetLastError((DWORD)result);
		goto done;
	}
	result = RegCreateKeyExW(instances, L"CdpProtect Instance", 0, NULL, 0,
		KEY_SET_VALUE, NULL, &instance, &disposition);
	if (result != ERROR_SUCCESS ||
		!CdpSetRegString(instance, L"Altitude", altitude))
	{
		if (result != ERROR_SUCCESS)
			SetLastError((DWORD)result);
		goto done;
	}
	zero = 0;
	result = RegSetValueExW(instance, L"Flags", 0, REG_DWORD,
		(const BYTE *)&zero, sizeof(zero));
	if (result != ERROR_SUCCESS)
	{
		SetLastError((DWORD)result);
		goto done;
	}
	ok = TRUE;
done:
	if (instance)
		RegCloseKey(instance);
	if (instances)
		RegCloseKey(instances);
	if (parameters)
		RegCloseKey(parameters);
	if (service)
		CloseServiceHandle(service);
	if (manager)
		CloseServiceHandle(manager);
	return ok;
}

/* 已有服务升级不再调用 SetupAPI。安装器父进程持有维护会话并负责强制
 * 覆盖；Helper 在这里确认字节已经一致。直接运行 Helper 时再尝试覆盖一次。 */
static BOOL CdpUpdateProtectBinary(_In_z_ const wchar_t *InfPath)
{
	wchar_t sourcePath[MAX_PATH];
	wchar_t systemPath[MAX_PATH];
	wchar_t *slash;
	BYTE sourceDigest[32];
	BYTE systemDigest[32];
	DWORD length;

	if (FAILED(StringCchCopyW(sourcePath, _countof(sourcePath), InfPath)))
		return FALSE;
	slash = wcsrchr(sourcePath, L'\\');
	if (!slash)
	{
		SetLastError(ERROR_INVALID_NAME);
		return FALSE;
	}
	*(slash + 1) = L'\0';
	if (FAILED(StringCchCatW(sourcePath, _countof(sourcePath), L"CdpProtect.sys")))
		return FALSE;
	length = GetSystemDirectoryW(systemPath, _countof(systemPath));
	if (!length || length >= _countof(systemPath) ||
		FAILED(StringCchCatW(systemPath, _countof(systemPath), L"\\drivers\\CdpProtect.sys")))
	{
		SetLastError(ERROR_FILENAME_EXCED_RANGE);
		return FALSE;
	}

	/* 安装器已经覆盖成功时不要再次以 Helper 身份打开受保护目标。 */
	if (CdpHashFileSha256(sourcePath, sourceDigest) &&
		CdpHashFileSha256(systemPath, systemDigest) &&
		memcmp(sourceDigest, systemDigest, sizeof(sourceDigest)) == 0)
		return TRUE;

	if (!CopyFileW(sourcePath, systemPath, FALSE))
		return FALSE;
	return CdpVerifyProtectSystemDriver(InfPath);
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

	/* 首装和升级统一使用 CdpDriver 的方式：直接更新二进制、手工确保服务。 */
	if (!CdpUpdateProtectBinary(infPath))
	{
		fwprintf(stderr, L"CdpProtect binary update failed (Win32=%lu).\n",
			GetLastError());
		return FALSE;
	}
	if (!CdpEnsureProtectService(infPath))
	{
		fwprintf(stderr, L"CdpProtect service configuration failed (Win32=%lu).\n",
			GetLastError());
		return FALSE;
	}
	wprintf(L"CdpProtect binary and MiniFilter service configuration completed.\n");
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
