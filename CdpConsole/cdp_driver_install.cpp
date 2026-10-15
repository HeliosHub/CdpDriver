#include "cdp_driver_install.h"

#include <SetupAPI.h>
#include <Shlwapi.h>
#include <strsafe.h>
#include <stdio.h>
#include <string.h>

#pragma comment(lib, "SetupAPI.lib")
#pragma comment(lib, "Shlwapi.lib")

static const wchar_t* g_CdpInstallFailureStage = L"unknown";

const wchar_t* CdpGetInstallFailureStage(void)
{
	return g_CdpInstallFailureStage;
}

static BOOL CdpFileExists(_In_ const wchar_t* path)
{
	return path && path[0] && GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
}

static BOOL CdpPathEndsWithInsensitive(
	_In_ const wchar_t* path,
	_In_ const wchar_t* suffix)
{
	size_t pathChars;
	size_t suffixChars;

	if (!path || !suffix)
		return FALSE;
	while (*path == L'!')
		++path;
	pathChars = wcslen(path);
	suffixChars = wcslen(suffix);
	return pathChars >= suffixChars &&
		_wcsicmp(path + pathChars - suffixChars, suffix) == 0;
}

/* 旧安装包曾用 MoveFileEx(..., DELAY_UNTIL_REBOOT) 更新启动驱动。
 * 该操作由“删除旧文件 + .new 改名”两步组成，自保护若挡住第二步会让
 * 正式驱动路径消失。升级前必须撤销所有这类遗留任务。 */
static BOOL CdpRemoveLegacyDriverPendingRename(void)
{
	static const wchar_t keyPath[] =
		L"SYSTEM\\CurrentControlSet\\Control\\Session Manager";
	static const wchar_t valueName[] = L"PendingFileRenameOperations";
	static const wchar_t driverSuffix[] = L"\\System32\\drivers\\CdpDriver.sys";
	static const wchar_t pendingSuffix[] = L"\\System32\\drivers\\CdpDriver.sys.new";
	HKEY key = NULL;
	DWORD type = 0;
	DWORD bytes = 0;
	LONG result;
	wchar_t* values = NULL;
	wchar_t* read;
	wchar_t* write;
	size_t remaining;
	BOOL removed = FALSE;

	result = RegOpenKeyExW(
		HKEY_LOCAL_MACHINE, keyPath, 0, KEY_QUERY_VALUE | KEY_SET_VALUE, &key);
	if (result != ERROR_SUCCESS)
	{
		SetLastError((DWORD)result);
		return FALSE;
	}
	result = RegQueryValueExW(key, valueName, NULL, &type, NULL, &bytes);
	if (result == ERROR_FILE_NOT_FOUND)
	{
		RegCloseKey(key);
		return TRUE;
	}
	if (result != ERROR_SUCCESS || type != REG_MULTI_SZ || bytes < sizeof(wchar_t))
	{
		RegCloseKey(key);
		SetLastError(result == ERROR_SUCCESS ? ERROR_INVALID_DATA : (DWORD)result);
		return FALSE;
	}
	values = (wchar_t*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
		bytes + 2 * sizeof(wchar_t));
	if (!values)
	{
		RegCloseKey(key);
		SetLastError(ERROR_OUTOFMEMORY);
		return FALSE;
	}
	result = RegQueryValueExW(key, valueName, NULL, &type, (BYTE*)values, &bytes);
	if (result != ERROR_SUCCESS)
	{
		HeapFree(GetProcessHeap(), 0, values);
		RegCloseKey(key);
		SetLastError((DWORD)result);
		return FALSE;
	}

	read = values;
	write = values;
	remaining = bytes / sizeof(wchar_t);
	while (remaining > 1 && *read != L'\0')
	{
		size_t sourceChars = wcsnlen_s(read, remaining);
		wchar_t* source = read;
		wchar_t* destination;
		size_t destinationChars;
		BOOL discard;

		if (sourceChars >= remaining)
		{
			SetLastError(ERROR_INVALID_DATA);
			goto cleanup_failed;
		}
		read += sourceChars + 1;
		remaining -= sourceChars + 1;
		if (remaining == 0)
		{
			SetLastError(ERROR_INVALID_DATA);
			goto cleanup_failed;
		}
		destination = read;
		destinationChars = wcsnlen_s(read, remaining);
		if (destinationChars >= remaining)
		{
			SetLastError(ERROR_INVALID_DATA);
			goto cleanup_failed;
		}
		read += destinationChars + 1;
		remaining -= destinationChars + 1;

		discard =
			CdpPathEndsWithInsensitive(source, driverSuffix) ||
			CdpPathEndsWithInsensitive(source, pendingSuffix) ||
			CdpPathEndsWithInsensitive(destination, driverSuffix) ||
			CdpPathEndsWithInsensitive(destination, pendingSuffix);
		if (discard)
		{
			removed = TRUE;
			continue;
		}
		memmove(write, source, (sourceChars + 1) * sizeof(wchar_t));
		write += sourceChars + 1;
		memmove(write, destination, (destinationChars + 1) * sizeof(wchar_t));
		write += destinationChars + 1;
	}
	*write++ = L'\0';
	if (removed)
	{
		if (write == values + 1)
			result = RegDeleteValueW(key, valueName);
		else
			result = RegSetValueExW(
				key, valueName, 0, REG_MULTI_SZ, (const BYTE*)values,
				(DWORD)((write - values) * sizeof(wchar_t)));
		if (result != ERROR_SUCCESS && result != ERROR_FILE_NOT_FOUND)
		{
			SetLastError((DWORD)result);
			goto cleanup_failed;
		}
	}
	HeapFree(GetProcessHeap(), 0, values);
	RegCloseKey(key);
	return TRUE;

cleanup_failed:
	HeapFree(GetProcessHeap(), 0, values);
	RegCloseKey(key);
	return FALSE;
}

static void CdpGetExeDirectory(_Out_writes_(cchDir) wchar_t* dir, _In_ size_t cchDir)
{
	DWORD len;
	wchar_t* slash;

	if (!dir || cchDir == 0)
		return;

	dir[0] = L'\0';
	len = GetModuleFileNameW(NULL, dir, (DWORD)cchDir);
	if (len == 0 || len >= cchDir)
		return;

	slash = wcsrchr(dir, L'\\');
	if (slash)
		*slash = L'\0';
}

static BOOL CdpTryInfCandidate(
	_In_ const wchar_t* infPath,
	_Out_writes_(cchOutInf) wchar_t* outInf,
	_In_ size_t cchOutInf)
{
	wchar_t fullInf[MAX_PATH];
	wchar_t dir[MAX_PATH];
	wchar_t sysPath[MAX_PATH];
	wchar_t* slash;

	if (!CdpFileExists(infPath))
		return FALSE;

	if (!GetFullPathNameW(infPath, MAX_PATH, fullInf, NULL))
		return FALSE;

	wcsncpy_s(outInf, cchOutInf, fullInf, _TRUNCATE);

	wcsncpy_s(dir, fullInf, _TRUNCATE);
	slash = wcsrchr(dir, L'\\');
	if (!slash)
		return FALSE;
	*slash = L'\0';

	_snwprintf_s(
		sysPath,
		_countof(sysPath),
		_TRUNCATE,
		L"%s\\CdpDriver.sys",
		dir);
	return CdpFileExists(sysPath);
}

BOOL CdpResolveDriverInfPath(
	_Out_writes_(cchInfPath) wchar_t* infPath,
	_In_ size_t cchInfPath)
{
	wchar_t exeDir[MAX_PATH];
	wchar_t candidate[MAX_PATH];
	const wchar_t* suffixes[] = {
		L"\\driver\\CdpDriver.inf",
		L"\\CdpDriver.inf",
		L"\\..\\CdpDriver\\x64\\Release\\CdpDriver.inf",
		L"\\..\\CdpDriver\\x64\\Debug\\CdpDriver.inf",
	};
	size_t i;

	if (!infPath || cchInfPath == 0)
		return FALSE;

	infPath[0] = L'\0';
	CdpGetExeDirectory(exeDir, _countof(exeDir));
	if (exeDir[0] == L'\0')
		return FALSE;

	for (i = 0; i < _countof(suffixes); ++i)
	{
		_snwprintf_s(candidate, _countof(candidate), _TRUNCATE, L"%s%s", exeDir, suffixes[i]);
		if (CdpTryInfCandidate(candidate, infPath, cchInfPath))
			return TRUE;
	}

	return FALSE;
}

BOOL CdpIsDriverServiceInstalled(void)
{
	SC_HANDLE scm;
	SC_HANDLE svc;
	BOOL installed = FALSE;

	scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
	if (!scm)
		return FALSE;

	svc = OpenServiceW(scm, L"CdpDriver", SERVICE_QUERY_STATUS);
	if (svc)
	{
		installed = TRUE;
		CloseServiceHandle(svc);
	}

	CloseServiceHandle(scm);
	return installed;
}

/* Always refresh System32\drivers\CdpDriver.sys from the package next to the INF.
   Skipping this when the service already exists leaves an old image loaded, so
   new IOCTLs (e.g. QUERY_VERSION) fail until reboot with a stale binary. */
static BOOL CdpUpdateDriverBinary(_In_ const wchar_t* infPath)
{
	wchar_t fullInf[MAX_PATH];
	wchar_t dir[MAX_PATH];
	wchar_t src[MAX_PATH];
	wchar_t dst[MAX_PATH];
	wchar_t staged[MAX_PATH];
	wchar_t legacyPending[MAX_PATH];
	wchar_t winDir[MAX_PATH];
	wchar_t* slash;

	if (!GetFullPathNameW(infPath, MAX_PATH, fullInf, NULL))
		return FALSE;

	wcsncpy_s(dir, fullInf, _TRUNCATE);
	slash = wcsrchr(dir, L'\\');
	if (!slash)
		return FALSE;
	*slash = L'\0';

	_snwprintf_s(src, _countof(src), _TRUNCATE, L"%s\\CdpDriver.sys", dir);
	if (!CdpFileExists(src))
		return FALSE;

	if (!GetWindowsDirectoryW(winDir, MAX_PATH))
		return FALSE;

	_snwprintf_s(
		dst,
		_countof(dst),
		_TRUNCATE,
		L"%s\\System32\\drivers\\CdpDriver.sys",
		winDir);
	_snwprintf_s(
		staged,
		_countof(staged),
		_TRUNCATE,
		L"%s\\System32\\drivers\\CdpDriver.sys.upgrade.%lu",
		winDir,
		GetCurrentProcessId());
	_snwprintf_s(
		legacyPending,
		_countof(legacyPending),
		_TRUNCATE,
		L"%s\\System32\\drivers\\CdpDriver.sys.new",
		winDir);

	/* 先撤销旧包留下的两阶段重启替换，避免下次启动只删除正式驱动。 */
	if (!CdpRemoveLegacyDriverPendingRename())
		return FALSE;
	DeleteFileW(legacyPending);
	DeleteFileW(staged);

	if (CopyFileW(src, dst, FALSE))
		return TRUE;

	/* 已加载的驱动可能拒绝 CopyFile 原地写入。新版先完整落到同目录，再做
	 * 单次原子替换；失败时旧正式文件完全不动。禁止 DELAY_UNTIL_REBOOT。 */
	if (!CopyFileW(src, staged, FALSE))
		return FALSE;
	if (!MoveFileExW(staged, dst, MOVEFILE_REPLACE_EXISTING))
	{
		DWORD error = GetLastError();
		DeleteFileW(staged);
		SetLastError(error);
		return FALSE;
	}
	return TRUE;
}

static BOOL CdpEnsureDriverService(void)
{
	wchar_t winDir[MAX_PATH];
	wchar_t sysDst[MAX_PATH];
	SC_HANDLE scm;
	SC_HANDLE svc;
	BOOL ok = FALSE;

	if (CdpIsDriverServiceInstalled())
		return TRUE;

	if (!GetWindowsDirectoryW(winDir, MAX_PATH))
		return FALSE;

	_snwprintf_s(
		sysDst,
		_countof(sysDst),
		_TRUNCATE,
		L"%s\\System32\\drivers\\CdpDriver.sys",
		winDir);
	if (!CdpFileExists(sysDst))
		return FALSE;

	scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT | SC_MANAGER_CREATE_SERVICE);
	if (!scm)
		return FALSE;

	svc = CreateServiceW(
		scm,
		L"CdpDriver",
		L"CdpDriver",
		SERVICE_QUERY_STATUS,
		SERVICE_KERNEL_DRIVER,
		SERVICE_BOOT_START,
		SERVICE_ERROR_NORMAL,
		sysDst,
		L"Storage Volume Filters",
		NULL,
		NULL,
		NULL,
		NULL);
	if (svc)
	{
		ok = TRUE;
		CloseServiceHandle(svc);
	}

	CloseServiceHandle(scm);
	return ok && CdpIsDriverServiceInstalled();
}

BOOL CdpInstallDriverFromInf(_In_ const wchar_t* infPath)
{
	if (!infPath || !infPath[0])
		return FALSE;

	/* Do not call InstallHinfSection/DefaultInstall here.
	   On a fresh machine AddFilter+CatalogFile pops a SetupAPI "install failed"
	   dialog under testsigning, even though SCM fallback would succeed afterward.
	   Install path: copy .sys -> CreateService -> UpperFilters -> SetupPromptReboot. */
	if (!CdpUpdateDriverBinary(infPath))
		return FALSE;

	return CdpEnsureDriverService();
}

static BOOL CdpAppendUpperFilterValue(
	_Out_writes_(capacity) wchar_t* values,
	_In_ size_t capacity,
	_Inout_ wchar_t** end,
	_In_ const wchar_t* value)
{
	size_t length = wcslen(value);
	if ((size_t)(*end - values) + length + 2 > capacity)
	{
		SetLastError(ERROR_BUFFER_OVERFLOW);
		return FALSE;
	}
	wcscpy_s(*end, capacity - (*end - values), value);
	*end += length + 1;
	**end = L'\0';
	return TRUE;
}

static BOOL CdpRegisterClassUpperFilter(
	_In_ const wchar_t* classKey,
	_In_opt_ const wchar_t* insertBefore)
{
	wchar_t existing[4096] = {};
	wchar_t updated[4096] = {};
	wchar_t* out = updated;
	DWORD existingSize;
	DWORD type;
	LONG result;
	HKEY hKey;
	BOOL inserted = FALSE;
	const wchar_t* filterName = L"CdpDriver";

	result = RegOpenKeyExW(
		HKEY_LOCAL_MACHINE,
		classKey,
		0,
		KEY_READ | KEY_SET_VALUE,
		&hKey);
	if (result != ERROR_SUCCESS)
	{
		SetLastError((DWORD)result);
		return FALSE;
	}

	existingSize = sizeof(existing);
	type = REG_MULTI_SZ;
	result = RegQueryValueExW(
		hKey,
		L"UpperFilters",
		NULL,
		&type,
		(LPBYTE)existing,
		&existingSize);
	if (result == ERROR_FILE_NOT_FOUND)
	{
		existing[0] = L'\0';
		existing[1] = L'\0';
	}
	else if (result != ERROR_SUCCESS || type != REG_MULTI_SZ)
	{
		RegCloseKey(hKey);
		SetLastError(result == ERROR_SUCCESS ? ERROR_INVALID_DATATYPE : (DWORD)result);
		return FALSE;
	}
	existing[_countof(existing) - 1] = L'\0';
	existing[_countof(existing) - 2] = L'\0';

	for (const wchar_t* value = existing; *value; value += wcslen(value) + 1)
	{
		if (_wcsicmp(value, filterName) == 0)
			continue;
		if (!inserted && insertBefore &&
			_wcsicmp(value, insertBefore) == 0)
		{
			if (!CdpAppendUpperFilterValue(
					updated, _countof(updated), &out, filterName))
			{
				RegCloseKey(hKey);
				return FALSE;
			}
			inserted = TRUE;
		}
		if (!CdpAppendUpperFilterValue(
				updated, _countof(updated), &out, value))
		{
			RegCloseKey(hKey);
			return FALSE;
		}
	}
	if (!inserted && !CdpAppendUpperFilterValue(
			updated, _countof(updated), &out, filterName))
	{
		RegCloseKey(hKey);
		return FALSE;
	}

	result = RegSetValueExW(
		hKey,
		L"UpperFilters",
		0,
		REG_MULTI_SZ,
		(const BYTE*)updated,
		(DWORD)((out - updated + 1) * sizeof(wchar_t)));
	if (result != ERROR_SUCCESS)
	{
		RegCloseKey(hKey);
		SetLastError((DWORD)result);
		return FALSE;
	}

	RegCloseKey(hKey);
	return TRUE;
}

BOOL CdpRegisterVolumeUpperFilter(void)
{
	/* UpperFilters are ordered bottom-up.  Keep CdpDriver immediately below
	 * volsnap so reads that volsnap reissues to its lower device still cross
	 * the protected-volume view. */
	return CdpRegisterClassUpperFilter(
		L"SYSTEM\\CurrentControlSet\\Control\\Class\\"
		L"{71a27cdd-812a-11d0-bec7-08002be2092f}",
		L"volsnap");
}

static BOOL CdpUnregisterClassUpperFilter(_In_ const wchar_t* classKey);

BOOL CdpRemoveLegacyDiskUpperFilter(void)
{
	/* Upgrades from the dual-layer development build must detach CdpDriver
	 * from DiskDrive after reboot while preserving PartMgr and other filters. */
	return CdpUnregisterClassUpperFilter(
		L"SYSTEM\\CurrentControlSet\\Control\\Class\\"
		L"{4d36e967-e325-11ce-bfc1-08002be10318}");
}

static BOOL CdpUnregisterClassUpperFilter(_In_ const wchar_t* classKey)
{
	wchar_t existing[4096] = {};
	wchar_t updated[4096] = {};
	wchar_t* out = updated;
	DWORD existingSize = sizeof(existing);
	DWORD type = 0;
	LONG result;
	HKEY hKey;
	BOOL removed = FALSE;
	const wchar_t* filterName = L"CdpDriver";

	result = RegOpenKeyExW(
		HKEY_LOCAL_MACHINE, classKey, 0, KEY_READ | KEY_SET_VALUE, &hKey);
	if (result != ERROR_SUCCESS)
	{
		SetLastError((DWORD)result);
		return FALSE;
	}

	result = RegQueryValueExW(
		hKey, L"UpperFilters", NULL, &type, (LPBYTE)existing, &existingSize);
	if (result == ERROR_FILE_NOT_FOUND)
	{
		RegCloseKey(hKey);
		return TRUE;
	}
	if (result != ERROR_SUCCESS || type != REG_MULTI_SZ)
	{
		RegCloseKey(hKey);
		SetLastError(result == ERROR_SUCCESS ? ERROR_INVALID_DATATYPE : (DWORD)result);
		return FALSE;
	}

	existing[_countof(existing) - 1] = L'\0';
	existing[_countof(existing) - 2] = L'\0';
	for (const wchar_t* value = existing; *value; value += wcslen(value) + 1)
	{
		size_t length = wcslen(value);
		if (_wcsicmp(value, filterName) == 0)
		{
			removed = TRUE;
			continue;
		}
		if ((size_t)(out - updated) + length + 2 > _countof(updated))
		{
			RegCloseKey(hKey);
			SetLastError(ERROR_BUFFER_OVERFLOW);
			return FALSE;
		}
		wcscpy_s(out, _countof(updated) - (out - updated), value);
		out += length + 1;
	}
	*out = L'\0';

	if (!removed)
	{
		RegCloseKey(hKey);
		return TRUE;
	}

	if (out == updated)
		result = RegDeleteValueW(hKey, L"UpperFilters");
	else
		result = RegSetValueExW(
			hKey, L"UpperFilters", 0, REG_MULTI_SZ, (const BYTE*)updated,
			(DWORD)((out - updated + 1) * sizeof(wchar_t)));

	RegCloseKey(hKey);
	if (result != ERROR_SUCCESS && result != ERROR_FILE_NOT_FOUND)
	{
		SetLastError((DWORD)result);
		return FALSE;
	}
	return TRUE;
}

static BOOL CdpDeleteDriverService(void)
{
	SC_HANDLE scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
	if (!scm)
		return FALSE;

	SC_HANDLE svc = OpenServiceW(
		scm, L"CdpDriver", DELETE | SERVICE_STOP | SERVICE_QUERY_STATUS);
	if (!svc)
	{
		DWORD error = GetLastError();
		CloseServiceHandle(scm);
		if (error == ERROR_SERVICE_DOES_NOT_EXIST)
			return TRUE;
		SetLastError(error);
		return FALSE;
	}

	SERVICE_STATUS status = {};
	(void)ControlService(svc, SERVICE_CONTROL_STOP, &status);
	BOOL deleted = DeleteService(svc);
	DWORD error = deleted ? ERROR_SUCCESS : GetLastError();
	CloseServiceHandle(svc);
	CloseServiceHandle(scm);
	if (!deleted && error != ERROR_SERVICE_MARKED_FOR_DELETE)
	{
		SetLastError(error);
		return FALSE;
	}
	return TRUE;
}

BOOL CdpUninstallDriverPackage(void)
{
	g_CdpInstallFailureStage = L"removing Volume UpperFilter";
	if (!CdpUnregisterClassUpperFilter(
		L"SYSTEM\\CurrentControlSet\\Control\\Class\\"
		L"{71a27cdd-812a-11d0-bec7-08002be2092f}"))
		return FALSE;

	g_CdpInstallFailureStage = L"removing DiskDrive UpperFilter";
	if (!CdpUnregisterClassUpperFilter(
		L"SYSTEM\\CurrentControlSet\\Control\\Class\\"
		L"{4d36e967-e325-11ce-bfc1-08002be10318}"))
		return FALSE;

	g_CdpInstallFailureStage = L"deleting CdpDriver service";
	if (!CdpDeleteDriverService())
		return FALSE;

	wchar_t winDir[MAX_PATH];
	wchar_t driverPath[MAX_PATH];
	if (!GetWindowsDirectoryW(winDir, _countof(winDir)) ||
		FAILED(StringCchPrintfW(
			driverPath, _countof(driverPath), L"%s\\System32\\drivers\\CdpDriver.sys", winDir)))
	{
		SetLastError(ERROR_FILENAME_EXCED_RANGE);
		return FALSE;
	}
	if (CdpFileExists(driverPath) &&
		!MoveFileExW(driverPath, NULL, MOVEFILE_DELAY_UNTIL_REBOOT))
		return FALSE;

	g_CdpInstallFailureStage = L"none";
	return TRUE;
}

BOOL CdpInstallBootConfirmService(void)
{
	wchar_t exeDir[MAX_PATH];
	wchar_t sourcePath[MAX_PATH];
	wchar_t systemDirectory[MAX_PATH];
	wchar_t servicePath[MAX_PATH];
	wchar_t stagedPath[MAX_PATH];
	wchar_t oldPath[MAX_PATH];
	wchar_t commandLine[MAX_PATH + 4];
	SC_HANDLE scm = NULL;
	SC_HANDLE service = NULL;
	DWORD error = ERROR_SUCCESS;
	const wchar_t* operation = L"initializing";
	SERVICE_DELAYED_AUTO_START_INFO delayed = {};
	SC_ACTION restart[3];
	SERVICE_FAILURE_ACTIONSW failureActions = {};
	int index;

	CdpGetExeDirectory(exeDir, _countof(exeDir));
	if (!exeDir[0] ||
		FAILED(StringCchPrintfW(
			sourcePath, _countof(sourcePath), L"%s\\CdpBootService.exe", exeDir)) ||
		!CdpFileExists(sourcePath))
	{
		SetLastError(ERROR_FILE_NOT_FOUND);
		return FALSE;
	}
	if (!GetSystemDirectoryW(systemDirectory, _countof(systemDirectory)) ||
		FAILED(StringCchPrintfW(
			servicePath, _countof(servicePath), L"%s\\CdpBootService.exe",
			systemDirectory)) ||
		FAILED(StringCchPrintfW(
			stagedPath, _countof(stagedPath), L"%s\\CdpBootService.exe.upgrade.%lu",
			systemDirectory, GetCurrentProcessId())) ||
		FAILED(StringCchPrintfW(
			oldPath, _countof(oldPath), L"%s\\CdpBootService.exe.old.%lu",
			systemDirectory, GetCurrentProcessId())) ||
		FAILED(StringCchPrintfW(
			commandLine, _countof(commandLine), L"\"%s\"", servicePath)))
	{
		SetLastError(ERROR_FILENAME_EXCED_RANGE);
		return FALSE;
	}

	/* 旧 CdpBootConfirm 在 Windows 11 上可能仍处于等待驱动就绪的运行状态，
	 * 而它有意不接受 SERVICE_CONTROL_STOP。升级不能依赖强杀进程：把旧映像
	 * 改名后在原路径写入新版，当前进程继续运行旧映像，重启后自然加载新版。 */
	operation = L"staging CdpBootService.exe";
	DeleteFileW(stagedPath);
	DeleteFileW(oldPath);
	if (!CopyFileW(sourcePath, stagedPath, FALSE))
	{
		error = GetLastError();
		goto failed;
	}
	if (CdpFileExists(servicePath))
	{
		operation = L"renaming the running CdpBootService.exe";
		if (!MoveFileExW(servicePath, oldPath, MOVEFILE_REPLACE_EXISTING))
		{
			error = GetLastError();
			DeleteFileW(stagedPath);
			goto failed;
		}
	}
	operation = L"publishing the new CdpBootService.exe";
	if (!MoveFileExW(stagedPath, servicePath, MOVEFILE_REPLACE_EXISTING))
	{
		error = GetLastError();
		if (CdpFileExists(oldPath))
			(void)MoveFileExW(oldPath, servicePath, MOVEFILE_REPLACE_EXISTING);
		DeleteFileW(stagedPath);
		goto failed;
	}
	if (CdpFileExists(oldPath) && !DeleteFileW(oldPath))
		(void)MoveFileExW(oldPath, NULL, MOVEFILE_DELAY_UNTIL_REBOOT);

	/* 由已持有维护票的 Helper 直接更新 SCM，避免再派生二级安装进程。 */
	operation = L"opening the Service Control Manager";
	scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT | SC_MANAGER_CREATE_SERVICE);
	if (!scm)
	{
		error = GetLastError();
		goto failed;
	}
	operation = L"opening CdpBootConfirm";
	service = OpenServiceW(
		scm, L"CdpBootConfirm",
		SERVICE_CHANGE_CONFIG | SERVICE_START);
	if (!service && GetLastError() != ERROR_SERVICE_DOES_NOT_EXIST)
	{
		error = GetLastError();
		goto failed;
	}

	if (!service)
	{
		operation = L"creating CdpBootConfirm";
		service = CreateServiceW(
			scm, L"CdpBootConfirm", L"CDP Boot Confirmation",
			SERVICE_CHANGE_CONFIG | SERVICE_START,
			SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START, SERVICE_ERROR_NORMAL,
			commandLine, NULL, NULL, NULL, NULL, NULL);
		if (!service)
		{
			error = GetLastError();
			goto failed;
		}
	}
	operation = L"updating CdpBootConfirm configuration";
	if (!ChangeServiceConfigW(
		service, SERVICE_NO_CHANGE, SERVICE_AUTO_START, SERVICE_NO_CHANGE,
		commandLine, NULL, NULL, NULL, NULL, NULL, L"CDP Boot Confirmation"))
	{
		error = GetLastError();
		goto failed;
	}
	delayed.fDelayedAutostart = FALSE;
	operation = L"disabling delayed auto-start";
	if (!ChangeServiceConfig2W(
		service, SERVICE_CONFIG_DELAYED_AUTO_START_INFO, &delayed))
	{
		error = GetLastError();
		goto failed;
	}
	for (index = 0; index < 3; ++index)
	{
		restart[index].Type = SC_ACTION_RESTART;
		restart[index].Delay = 5000;
	}
	failureActions.dwResetPeriod = 86400;
	failureActions.cActions = _countof(restart);
	failureActions.lpsaActions = restart;
	operation = L"updating CdpBootConfirm failure actions";
	if (!ChangeServiceConfig2W(
		service, SERVICE_CONFIG_FAILURE_ACTIONS, &failureActions))
	{
		error = GetLastError();
		goto failed;
	}
	CloseServiceHandle(service);
	CloseServiceHandle(scm);
	return TRUE;

failed:
	fwprintf(stderr, L"CdpBootConfirm: %s failed (Win32=%lu).\n", operation,
		error ? error : ERROR_GEN_FAILURE);
	if (service) CloseServiceHandle(service);
	if (scm) CloseServiceHandle(scm);
	SetLastError(error ? error : ERROR_GEN_FAILURE);
	return FALSE;
}

BOOL CdpInstallDriverPackage(void)
{
	wchar_t infPath[MAX_PATH];

	g_CdpInstallFailureStage = L"resolving driver package";
	if (!CdpResolveDriverInfPath(infPath, _countof(infPath)))
		return FALSE;

	g_CdpInstallFailureStage = L"updating CdpDriver.sys / driver service";
	if (!CdpInstallDriverFromInf(infPath))
		return FALSE;

	g_CdpInstallFailureStage = L"registering Volume UpperFilter";
	if (!CdpRegisterVolumeUpperFilter())
		return FALSE;
	g_CdpInstallFailureStage = L"removing legacy DiskDrive UpperFilter";
	if (!CdpRemoveLegacyDiskUpperFilter())
		return FALSE;
	g_CdpInstallFailureStage = L"installing CdpBootConfirm service";
	if (!CdpInstallBootConfirmService())
		return FALSE;
	g_CdpInstallFailureStage = L"none";

	/* System Settings Change reboot dialog (same family as INF AddFilter). */
	(void)SetupPromptReboot(NULL, NULL, FALSE);
	return TRUE;
}
