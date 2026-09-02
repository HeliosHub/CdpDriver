#include <Windows.h>
#include <objbase.h>
#include <stdio.h>
#include <strsafe.h>
#include "..\CdpDriver\CdpIoctl.h"

#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "ole32.lib")

static const wchar_t kServiceName[] = L"CdpBootConfirm";
static const wchar_t kServiceDisplayName[] = L"CDP Boot Confirmation";
static SERVICE_STATUS_HANDLE g_ServiceStatusHandle = NULL;
static SERVICE_STATUS g_ServiceStatus = {};
static HANDLE g_StopEvent = NULL;
static const wchar_t kLogPath[] = L"C:\\Windows\\Temp\\CdpBootConfirm.log";

static int LastErrorExitCode()
{
	DWORD error = GetLastError();
	return error ? (int)error : 1;
}

/* The old one-shot service can still be running while retrying.  Stop it before
 * replacing its executable, otherwise CopyFile fails with sharing violation. */
static BOOL StopExistingServiceForUpdate()
{
	SC_HANDLE scm;
	SC_HANDLE service;
	SERVICE_STATUS_PROCESS status = {};
	DWORD bytesNeeded = 0;
	DWORD waited = 0;

	scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
	if (!scm)
		return FALSE;
	service = OpenServiceW(scm, kServiceName,
		SERVICE_QUERY_STATUS | SERVICE_STOP);
	if (!service)
	{
		DWORD error = GetLastError();
		CloseServiceHandle(scm);
		if (error == ERROR_SERVICE_DOES_NOT_EXIST)
			return TRUE;
		SetLastError(error);
		return FALSE;
	}
	if (!QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO,
		(LPBYTE)&status, sizeof(status), &bytesNeeded))
	{
		DWORD error = GetLastError();
		CloseServiceHandle(service);
		CloseServiceHandle(scm);
		SetLastError(error);
		return FALSE;
	}
	if (status.dwCurrentState != SERVICE_STOPPED)
		ControlService(service, SERVICE_CONTROL_STOP, (LPSERVICE_STATUS)&status);
	while (status.dwCurrentState != SERVICE_STOPPED && waited < 30000)
	{
		Sleep(500);
		waited += 500;
		if (!QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO,
			(LPBYTE)&status, sizeof(status), &bytesNeeded))
			break;
	}
	CloseServiceHandle(service);
	CloseServiceHandle(scm);
	if (status.dwCurrentState != SERVICE_STOPPED)
	{
		SetLastError(ERROR_SERVICE_REQUEST_TIMEOUT);
		return FALSE;
	}
	return TRUE;
}

static void ReportServiceStatus(DWORD state, DWORD win32ExitCode, DWORD waitHint)
{
	g_ServiceStatus.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
	g_ServiceStatus.dwCurrentState = state;
	g_ServiceStatus.dwWin32ExitCode = win32ExitCode;
	g_ServiceStatus.dwWaitHint = waitHint;
	g_ServiceStatus.dwControlsAccepted =
		state == SERVICE_RUNNING ? SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN : 0;
	if (state == SERVICE_START_PENDING || state == SERVICE_STOP_PENDING)
		g_ServiceStatus.dwCheckPoint++;
	else
		g_ServiceStatus.dwCheckPoint = 0;
	if (g_ServiceStatusHandle)
		SetServiceStatus(g_ServiceStatusHandle, &g_ServiceStatus);
}

static void Trace(const wchar_t* message, DWORD error)
{
	wchar_t buffer[512];
	HANDLE logFile;
	DWORD bytesWritten = 0;
	swprintf_s(buffer, _countof(buffer),
		L"CdpBootConfirm: %s (error=%lu)\n", message, error);
	OutputDebugStringW(buffer);
	logFile = CreateFileW(kLogPath, FILE_APPEND_DATA,
		FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS,
		FILE_ATTRIBUTE_NORMAL, NULL);
	if (logFile != INVALID_HANDLE_VALUE)
	{
		WriteFile(logFile, buffer, (DWORD)(wcslen(buffer) * sizeof(wchar_t)),
			&bytesWritten, NULL);
		CloseHandle(logFile);
	}
}

static BOOL GetSystemVolumeGuid(GUID* volumeGuid)
{
	wchar_t windowsDirectory[MAX_PATH];
	wchar_t root[] = L"C:\\";
	wchar_t volumeName[MAX_PATH];
	wchar_t guidText[40];
	const wchar_t* brace;

	if (!volumeGuid ||
		!GetWindowsDirectoryW(windowsDirectory, _countof(windowsDirectory)) ||
		windowsDirectory[1] != L':')
		return FALSE;
	root[0] = windowsDirectory[0];
	if (!GetVolumeNameForVolumeMountPointW(root, volumeName, _countof(volumeName)))
		return FALSE;
	brace = wcschr(volumeName, L'{');
	if (!brace || wcslen(brace) < 38)
		return FALSE;
	wcsncpy_s(guidText, brace, 38);
	return CLSIDFromString(guidText, volumeGuid) == S_OK;
}

static DWORD ConfirmSystemVolumeRestoreBoot(void)
{
	Cdp_RESTORE_BOOT_CONFIRM_REQUEST request = {};
	Cdp_RESTORE_POINT_QUERY_REQUEST query = {};
	Cdp_RESTORE_POINT_QUERY_REPLY reply = {};
	wchar_t guidText[40];
	wchar_t traceText[256];
	DWORD bytesReturned = 0;
	HANDLE control;

	if (!GetSystemVolumeGuid(&request.SourceVolumeGuid))
		return GetLastError() ? GetLastError() : ERROR_INVALID_DATA;
	query.SourceVolumeGuid = request.SourceVolumeGuid;
	StringFromGUID2(request.SourceVolumeGuid, guidText, _countof(guidText));
	swprintf_s(traceText, _countof(traceText),
		L"system source GUID=%s", guidText);
	Trace(traceText, ERROR_SUCCESS);
	control = CreateFileW(
		Cdp_CONTROL_SYSTEM_LINK_NAME,
		GENERIC_READ | GENERIC_WRITE,
		FILE_SHARE_READ | FILE_SHARE_WRITE,
		NULL,
		OPEN_EXISTING,
		FILE_ATTRIBUTE_NORMAL,
		NULL);
	if (control == INVALID_HANDLE_VALUE)
		return GetLastError();
	if (!DeviceIoControl(
		control,
		IOCTL_Cdp_QUERY_RESTORE_POINT,
		&query,
		sizeof(query),
		&reply,
		sizeof(reply),
		&bytesReturned,
		NULL))
	{
		DWORD error = GetLastError();
		CloseHandle(control);
		return error;
	}
	swprintf_s(traceText, _countof(traceText),
		L"query result IsSet=%lu PENDING=%lu target=%llu",
		reply.IsSet, reply.BootConfirmed, reply.TargetTime100ns);
	Trace(traceText, ERROR_SUCCESS);
	if (!reply.IsSet || reply.BootConfirmed)
	{
		CloseHandle(control);
		return ERROR_SUCCESS;
	}
	if (!DeviceIoControl(
		control,
		IOCTL_Cdp_CONFIRM_RESTORE_BOOT,
		&request,
		sizeof(request),
		NULL,
		0,
		&bytesReturned,
		NULL))
	{
		DWORD error = GetLastError();
		CloseHandle(control);
		return error;
	}
	CloseHandle(control);
	Trace(L"confirm IOCTL succeeded; PENDING is now 1", ERROR_SUCCESS);
	return ERROR_SUCCESS;
}

static DWORD WINAPI BootConfirmWorker(void*)
{
	DWORD error = ERROR_RETRY;
	ULONGLONG deadline = GetTickCount64() + 10ULL * 60ULL * 1000ULL;

	Trace(L"worker started; confirmation retry window is 10 minutes", ERROR_SUCCESS);

	/* SCM delayed-auto-start already places the service after early boot. Do
	 * not add a second fixed delay: an unconfirmed restore boot must be marked
	 * as soon as the service can reach the driver. */
	for (;;)
	{
		if (WaitForSingleObject(g_StopEvent, 0) == WAIT_OBJECT_0)
			return ERROR_CANCELLED;
		error = ConfirmSystemVolumeRestoreBoot();
		if (error == ERROR_SUCCESS)
		{
			Trace(L"system-volume restore boot confirmed", ERROR_SUCCESS);
			return ERROR_SUCCESS;
		}
		Trace(L"restore boot confirmation failed; retrying in 5 seconds", error);
		if (GetTickCount64() >= deadline)
		{
			Trace(L"confirmation retry window expired; leaving PENDING unchanged", error);
			return error;
		}
		if (WaitForSingleObject(g_StopEvent, 5000) == WAIT_OBJECT_0)
			return ERROR_CANCELLED;
	}
}

static DWORD WINAPI ServiceControlHandler(DWORD control, DWORD, void*, void*)
{
	if (control == SERVICE_CONTROL_STOP || control == SERVICE_CONTROL_SHUTDOWN)
	{
		ReportServiceStatus(SERVICE_STOP_PENDING, ERROR_SUCCESS, 5000);
		if (g_StopEvent)
			SetEvent(g_StopEvent);
	}
	return NO_ERROR;
}

static void WINAPI ServiceMain(DWORD, LPWSTR*)
{
	HANDLE worker;
	DWORD exitCode = ERROR_SUCCESS;

	g_ServiceStatusHandle = RegisterServiceCtrlHandlerExW(
		kServiceName, ServiceControlHandler, NULL);
	if (!g_ServiceStatusHandle)
		return;
	ReportServiceStatus(SERVICE_START_PENDING, ERROR_SUCCESS, 30000);
	g_StopEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
	if (!g_StopEvent)
	{
		ReportServiceStatus(SERVICE_STOPPED, GetLastError(), 0);
		return;
	}
	worker = CreateThread(NULL, 0, BootConfirmWorker, NULL, 0, NULL);
	if (!worker)
	{
		exitCode = GetLastError();
		CloseHandle(g_StopEvent);
		g_StopEvent = NULL;
		ReportServiceStatus(SERVICE_STOPPED, exitCode, 0);
		return;
	}
	ReportServiceStatus(SERVICE_RUNNING, ERROR_SUCCESS, 0);
	WaitForSingleObject(worker, INFINITE);
	GetExitCodeThread(worker, &exitCode);
	Trace(exitCode == ERROR_SUCCESS ?
		L"service stopped after successful confirmation/query" :
		L"service stopped without confirmation", exitCode);
	CloseHandle(worker);
	CloseHandle(g_StopEvent);
	g_StopEvent = NULL;
	ReportServiceStatus(SERVICE_STOPPED, exitCode, 0);
}

static int InstallService()
{
	wchar_t modulePath[MAX_PATH];
	wchar_t systemDirectory[MAX_PATH];
	wchar_t installPath[MAX_PATH];
	wchar_t commandLine[MAX_PATH + 4];
	SC_HANDLE scm;
	SC_HANDLE service;
	SERVICE_DELAYED_AUTO_START_INFO delayed = {};

	if (!GetModuleFileNameW(NULL, modulePath, _countof(modulePath)))
		return LastErrorExitCode();
	if (!GetSystemDirectoryW(systemDirectory, _countof(systemDirectory)) ||
		FAILED(StringCchPrintfW(
			installPath, _countof(installPath), L"%s\\CdpBootService.exe",
			systemDirectory)))
	{
		return LastErrorExitCode();
	}
	if (_wcsicmp(modulePath, installPath) != 0 &&
		(!StopExistingServiceForUpdate() ||
		 !CopyFileW(modulePath, installPath, FALSE)))
	{
		return LastErrorExitCode();
	}
	swprintf_s(commandLine, _countof(commandLine), L"\"%s\"", installPath);
	scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT | SC_MANAGER_CREATE_SERVICE);
	if (!scm)
		return LastErrorExitCode();
	service = CreateServiceW(
		scm, kServiceName, kServiceDisplayName,
		SERVICE_QUERY_STATUS | SERVICE_CHANGE_CONFIG | DELETE,
		SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START, SERVICE_ERROR_NORMAL,
		commandLine, NULL, NULL, NULL, NULL, NULL);
	if (!service && GetLastError() == ERROR_SERVICE_EXISTS)
	{
		service = OpenServiceW(scm,
			kServiceName, SERVICE_QUERY_STATUS | SERVICE_CHANGE_CONFIG | DELETE);
	}
	if (!service)
	{
		CloseServiceHandle(scm);
		return LastErrorExitCode();
	}
	if (!ChangeServiceConfigW(
		service,
		SERVICE_NO_CHANGE,
		SERVICE_AUTO_START,
		SERVICE_NO_CHANGE,
		commandLine,
		NULL,
		NULL,
		NULL,
		NULL,
		NULL,
		kServiceDisplayName))
	{
		CloseServiceHandle(service);
		CloseServiceHandle(scm);
		return LastErrorExitCode();
	}
	/* Confirmation is required for the boot that just completed.  Use normal
	 * automatic startup, not the optional delayed-auto-start queue.  If the
	 * filter is still initializing, BootConfirmWorker retries for 10 minutes. */
	delayed.fDelayedAutostart = FALSE;
	if (!ChangeServiceConfig2W(
		service, SERVICE_CONFIG_DELAYED_AUTO_START_INFO, &delayed))
	{
		CloseServiceHandle(service);
		CloseServiceHandle(scm);
		return LastErrorExitCode();
	}
	CloseServiceHandle(service);
	CloseServiceHandle(scm);
	return 0;
}

static int UninstallService()
{
	SC_HANDLE scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
	SC_HANDLE service;
	SERVICE_STATUS status;
	if (!scm)
		return LastErrorExitCode();
	service = OpenServiceW(scm, kServiceName, SERVICE_STOP | DELETE | SERVICE_QUERY_STATUS);
	if (!service)
	{
		CloseServiceHandle(scm);
		return GetLastError() == ERROR_SERVICE_DOES_NOT_EXIST ? 0 : LastErrorExitCode();
	}
	ControlService(service, SERVICE_CONTROL_STOP, &status);
	BOOL ok = DeleteService(service);
	CloseServiceHandle(service);
	CloseServiceHandle(scm);
	return ok ? 0 : LastErrorExitCode();
}

int wmain(int argc, wchar_t** argv)
{
	SERVICE_TABLE_ENTRYW serviceTable[] = {
		{ const_cast<LPWSTR>(kServiceName), ServiceMain },
		{ NULL, NULL }
	};
	if (argc == 2 && _wcsicmp(argv[1], L"--install") == 0)
		return InstallService();
	if (argc == 2 && _wcsicmp(argv[1], L"--uninstall") == 0)
		return UninstallService();
	return StartServiceCtrlDispatcherW(serviceTable) ? 0 : LastErrorExitCode();
}
