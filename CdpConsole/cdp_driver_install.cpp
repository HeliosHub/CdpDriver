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

BOOL CdpInstallDriverFromInf(_In_ const wchar_t* infPath)
{
	wchar_t cmdLine[MAX_PATH * 2];

	if (!infPath || !infPath[0])
		return FALSE;

	_snwprintf_s(
		cmdLine,
		_countof(cmdLine),
		_TRUNCATE,
		L"DefaultInstall 132 %s",
		infPath);
	InstallHinfSectionW(NULL, NULL, cmdLine, 0);
	return CdpIsDriverServiceInstalled();
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

	return CdpRegisterVolumeUpperFilter();
}
