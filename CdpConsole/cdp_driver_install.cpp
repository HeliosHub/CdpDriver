#include "cdp_driver_install.h"

#include <SetupAPI.h>
#include <Shlwapi.h>
#include <stdio.h>

#pragma comment(lib, "SetupAPI.lib")
#pragma comment(lib, "Shlwapi.lib")

static BOOL CdpFileExists(_In_ const wchar_t* path)
{
	return path && path[0] && GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
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

	if (CopyFileW(src, dst, FALSE))
		return TRUE;

	/* Loaded image may lock the file; stage replace for next reboot. */
	if (GetLastError() == ERROR_SHARING_VIOLATION ||
		GetLastError() == ERROR_ACCESS_DENIED ||
		GetLastError() == ERROR_USER_MAPPED_FILE)
	{
		wchar_t pending[MAX_PATH];
		_snwprintf_s(
			pending,
			_countof(pending),
			_TRUNCATE,
			L"%s\\System32\\drivers\\CdpDriver.sys.new",
			winDir);
		if (!CopyFileW(src, pending, FALSE))
			return CdpFileExists(dst);
		if (!MoveFileExW(pending, dst, MOVEFILE_REPLACE_EXISTING | MOVEFILE_DELAY_UNTIL_REBOOT))
			return CdpFileExists(dst);
		return TRUE;
	}

	return CdpFileExists(dst);
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

BOOL CdpRegisterVolumeUpperFilter(void)
{
	const wchar_t* volumeClassKey =
		L"SYSTEM\\CurrentControlSet\\Control\\Class\\"
		L"{71a27cdd-812a-11d0-bec7-08002be2092f}";
	wchar_t existing[4096];
	wchar_t* pEnd;
	DWORD existingSize;
	DWORD type;
	LONG result;
	HKEY hKey;
	BOOL alreadyExists = FALSE;
	const wchar_t* filterName = L"CdpDriver";

	result = RegOpenKeyExW(
		HKEY_LOCAL_MACHINE,
		volumeClassKey,
		0,
		KEY_READ | KEY_SET_VALUE,
		&hKey);
	if (result != ERROR_SUCCESS)
		return FALSE;

	existing[0] = L'\0';
	existing[1] = L'\0';
	existingSize = (DWORD)(sizeof(existing) - sizeof(wchar_t));
	type = REG_MULTI_SZ;
	result = RegQueryValueExW(
		hKey,
		L"UpperFilters",
		NULL,
		&type,
		(LPBYTE)existing,
		&existingSize);
	if (result == ERROR_SUCCESS && type == REG_MULTI_SZ)
	{
		const wchar_t* p = existing;
		while (*p)
		{
			if (_wcsicmp(p, filterName) == 0)
			{
				alreadyExists = TRUE;
				break;
			}
			p += wcslen(p) + 1;
		}
	}

	if (!alreadyExists)
	{
		pEnd = existing;
		if (result != ERROR_SUCCESS || type != REG_MULTI_SZ)
		{
			existing[0] = L'\0';
			existing[1] = L'\0';
			pEnd = existing;
		}
		else
		{
			while (*pEnd)
				pEnd += wcslen(pEnd) + 1;
		}

		if ((size_t)(pEnd - existing) + wcslen(filterName) + 2 >= _countof(existing))
		{
			RegCloseKey(hKey);
			return FALSE;
		}

		wcscpy_s(pEnd, _countof(existing) - (pEnd - existing), filterName);
		pEnd += wcslen(filterName) + 1;
		*pEnd = L'\0';

		result = RegSetValueExW(
			hKey,
			L"UpperFilters",
			0,
			REG_MULTI_SZ,
			(const BYTE*)existing,
			(DWORD)((pEnd - existing + 1) * sizeof(wchar_t)));
		if (result != ERROR_SUCCESS)
		{
			RegCloseKey(hKey);
			return FALSE;
		}
	}

	RegCloseKey(hKey);
	return TRUE;
}

BOOL CdpInstallDriverPackage(void)
{
	wchar_t infPath[MAX_PATH];

	if (!CdpResolveDriverInfPath(infPath, _countof(infPath)))
		return FALSE;

	if (!CdpInstallDriverFromInf(infPath))
		return FALSE;

	if (!CdpRegisterVolumeUpperFilter())
		return FALSE;

	/* System Settings Change reboot dialog (same family as INF AddFilter). */
	(void)SetupPromptReboot(NULL, NULL, FALSE);
	return TRUE;
}
