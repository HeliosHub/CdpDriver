#include <Windows.h>
#include <objbase.h>
#include <stdio.h>
#include <strsafe.h>
#include <WtsApi32.h>
#include "..\CdpDriver\CdpIoctl.h"

#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "Wtsapi32.lib")

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

static HANDLE OpenRestoreSpaceAlertChannel()
{
	return CreateFileW(
		Cdp_CONTROL_SYSTEM_LINK_NAME,
		GENERIC_READ | GENERIC_WRITE,
		FILE_SHARE_READ | FILE_SHARE_WRITE,
		NULL,
		OPEN_EXISTING,
		FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED,
		NULL);
}

static DWORD BeginRestoreSpaceAlertWait(
	HANDLE control,
	const GUID& sourceVolumeGuid,
	UINT64 lastSeenGeneration,
	OVERLAPPED* overlapped,
	Cdp_RESTORE_SPACE_ALERT_NOTIFICATION* notification)
{
	Cdp_RESTORE_SPACE_ALERT_WAIT_REQUEST request = {};
	DWORD bytesReturned = 0;

	if (control == INVALID_HANDLE_VALUE || !overlapped || !notification)
		return ERROR_INVALID_PARAMETER;
	request.SourceVolumeGuid = sourceVolumeGuid;
	request.LastSeenGeneration = lastSeenGeneration;
	ZeroMemory(notification, sizeof(*notification));
	ResetEvent(overlapped->hEvent);
	if (DeviceIoControl(
		control,
		IOCTL_Cdp_WAIT_RESTORE_SPACE_ALERT,
		&request,
		sizeof(request),
		notification,
		sizeof(*notification),
		&bytesReturned,
		overlapped))
	{
		/* The driver may return the initial/current state synchronously. */
		SetEvent(overlapped->hEvent);
		return ERROR_SUCCESS;
	}
	{
		DWORD error = GetLastError();
		return error == ERROR_IO_PENDING ? ERROR_SUCCESS : error;
	}
}

static BOOL SendRestoreSpaceWarningToActiveSessions(
	const Cdp_RESTORE_SPACE_ALERT_NOTIFICATION& reply,
	DWORD* deliveryError)
{
	PWTS_SESSION_INFOW sessions = NULL;
	DWORD sessionCount = 0;
	DWORD index;
	BOOL delivered = FALSE;
	DWORD activeSessions = 0;
	wchar_t message[768];
	const wchar_t* reason;
	double freePercent = 0.0;
	UINT64 capacity = reply.RecordPayloadBytesUsed +
		reply.RecordPayloadBytesFree;

	if (capacity != 0)
	{
		freePercent = (double)reply.RecordPayloadBytesFree * 100.0 /
			(double)capacity;
	}
	switch (reply.AlertReason)
	{
	case Cdp_RESTORE_SPACE_ALERT_NO_COMPACTABLE_RR:
		reason = L"当前没有可继续合并的历史区域。";
		break;
	case Cdp_RESTORE_SPACE_ALERT_RESERVE_FAILED:
		reason = L"合并所需的精确预留空间不足。";
		break;
	default:
		reason = L"历史合并失败，当前无法继续释放空间。";
		break;
	}
	swprintf_s(
		message,
		_countof(message),
		L"已设置系统还原点，但保护存储空间即将不足。\n\n"
		L"%s\n"
		L"剩余空间：%.2f GB（%.1f%%）\n"
		L"已用空间：%.2f GB\n"
		L"合并状态：0x%08X\n\n"
		L"如果空间耗尽，新的保护写入将失败。请尽快重启系统。",
		reason,
		(double)reply.RecordPayloadBytesFree / (1024.0 * 1024.0 * 1024.0),
		freePercent,
		(double)reply.RecordPayloadBytesUsed / (1024.0 * 1024.0 * 1024.0),
		(ULONG)reply.MergeStatus);

	if (deliveryError)
		*deliveryError = ERROR_SUCCESS;
	if (!WTSEnumerateSessionsW(
		WTS_CURRENT_SERVER_HANDLE, 0, 1, &sessions, &sessionCount))
	{
		if (deliveryError)
			*deliveryError = GetLastError();
		return FALSE;
	}
	for (index = 0; index < sessionCount; ++index)
	{
		DWORD response = 0;
		if (sessions[index].State != WTSActive)
			continue;
		activeSessions++;
		if (WTSSendMessageW(
			WTS_CURRENT_SERVER_HANDLE,
			sessions[index].SessionId,
			const_cast<LPWSTR>(L"还原点空间警告"),
			(DWORD)(wcslen(L"还原点空间警告") * sizeof(wchar_t)),
			message,
			(DWORD)(wcslen(message) * sizeof(wchar_t)),
			MB_OK | MB_ICONWARNING | MB_SYSTEMMODAL,
			0,
			&response,
			FALSE))
		{
			delivered = TRUE;
		}
		else if (deliveryError)
		{
			*deliveryError = GetLastError();
		}
	}
	WTSFreeMemory(sessions);
	if (!delivered && activeSessions == 0 && deliveryError)
		*deliveryError = ERROR_NO_SUCH_LOGON_SESSION;
	return delivered;
}

static DWORD WINAPI BootConfirmWorker(void*)
{
	DWORD error = ERROR_RETRY;
	DWORD lastMonitorError = ERROR_SUCCESS;
	ULONGLONG deadline = GetTickCount64() + 10ULL * 60ULL * 1000ULL;
	ULONGLONG nextConfirmAttempt = 0;
	ULONGLONG nextMonitorConnect = 0;
	ULONGLONG nextDeliveryAttempt = 0;
	BOOL confirmationFinished = FALSE;
	BOOL alertDelivered = FALSE;
	BOOL alertWaitPending = FALSE;
	/* Zero GUID subscribes to the product's protected restore-point source.
	 * Boot confirmation still resolves the Windows volume independently. */
	GUID alertSourceVolumeGuid = {};
	HANDLE alertControl = INVALID_HANDLE_VALUE;
	HANDLE alertEvent = NULL;
	OVERLAPPED alertOverlapped = {};
	UINT64 alertGeneration = 0;
	Cdp_RESTORE_SPACE_ALERT_NOTIFICATION alert = {};
	Cdp_RESTORE_SPACE_ALERT_NOTIFICATION pushedAlert = {};
	BOOL haveAlertState = FALSE;
	DWORD lastDeliveryError = ERROR_SUCCESS;
	DWORD result = ERROR_SUCCESS;

	Trace(L"worker started; boot confirmation retry is 10 minutes; space alerts use driver push", ERROR_SUCCESS);
	alertEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
	if (!alertEvent)
		return GetLastError();
	alertOverlapped.hEvent = alertEvent;

	/* Normal automatic startup places the service early in boot. Do not add a
	 * fixed delay: confirmation and the pushed alert channel start as soon as the
	 * driver control device becomes reachable. */
	for (;;)
	{
		HANDLE waits[2] = { g_StopEvent, alertEvent };
		DWORD waitCount;
		DWORD waitResult;
		ULONGLONG now = GetTickCount64();

		if (!confirmationFinished && now >= nextConfirmAttempt)
		{
			error = ConfirmSystemVolumeRestoreBoot();
			if (error == ERROR_SUCCESS)
			{
				Trace(L"system-volume restore boot confirmed", ERROR_SUCCESS);
				confirmationFinished = TRUE;
			}
			else if (GetTickCount64() >= deadline)
			{
				Trace(L"confirmation retry window expired; leaving PENDING unchanged", error);
				confirmationFinished = TRUE;
			}
			else
			{
				Trace(L"restore boot confirmation failed; retrying in 5 seconds", error);
				nextConfirmAttempt = now + 5000;
			}
		}

		if (!alertWaitPending && now >= nextMonitorConnect)
		{
			DWORD monitorError;
			if (alertControl == INVALID_HANDLE_VALUE)
				alertControl = OpenRestoreSpaceAlertChannel();
			if (alertControl == INVALID_HANDLE_VALUE)
				monitorError = GetLastError();
			else
			{
				monitorError = BeginRestoreSpaceAlertWait(
					alertControl,
					alertSourceVolumeGuid,
					alertGeneration,
					&alertOverlapped,
					&pushedAlert);
				if (monitorError == ERROR_SUCCESS)
					alertWaitPending = TRUE;
			}
			if (monitorError == ERROR_SUCCESS)
				lastMonitorError = ERROR_SUCCESS;
			else
			{
				if (monitorError != lastMonitorError)
				{
					Trace(L"restore-point alert channel failed; reconnecting", monitorError);
					lastMonitorError = monitorError;
				}
				if (alertControl != INVALID_HANDLE_VALUE)
				{
					CloseHandle(alertControl);
					alertControl = INVALID_HANDLE_VALUE;
				}
				nextMonitorConnect = now + 5000;
			}
		}

		if (haveAlertState && alert.RestorePointSet && alert.AlertActive &&
			!alertDelivered && now >= nextDeliveryAttempt)
		{
			DWORD deliveryError = ERROR_SUCCESS;
			alertDelivered = SendRestoreSpaceWarningToActiveSessions(
				alert, &deliveryError);
			if (alertDelivered)
			{
				Trace(L"restore-point space warning delivered to active session", ERROR_SUCCESS);
				lastDeliveryError = ERROR_SUCCESS;
			}
			else
			{
				if (deliveryError != lastDeliveryError)
					Trace(L"restore-point space warning delivery failed; retrying", deliveryError);
				lastDeliveryError = deliveryError;
				nextDeliveryAttempt = now + 5000;
			}
		}

		waitCount = alertWaitPending ? 2 : 1;
		waitResult = WaitForMultipleObjects(waitCount, waits, FALSE, 5000);
		if (waitResult == WAIT_OBJECT_0)
			break;
		if (alertWaitPending && waitResult == WAIT_OBJECT_0 + 1)
		{
			DWORD bytesReturned = 0;
			if (GetOverlappedResult(
					alertControl, &alertOverlapped, &bytesReturned, FALSE) &&
				bytesReturned >= sizeof(alert))
			{
				alert = pushedAlert;
				alertGeneration = alert.Generation;
				haveAlertState = TRUE;
				alertDelivered = FALSE;
				nextDeliveryAttempt = 0;
				lastMonitorError = ERROR_SUCCESS;
				{
					wchar_t traceText[256];
					wchar_t guidText[40];
					StringFromGUID2(alert.SourceVolumeGuid, guidText,
						_countof(guidText));
					swprintf_s(traceText, _countof(traceText),
						L"space alert received source=%s generation=%llu active=%lu reason=%lu status=0x%08X",
						guidText, alert.Generation, alert.AlertActive,
						alert.AlertReason, (ULONG)alert.MergeStatus);
					Trace(traceText, ERROR_SUCCESS);
				}
				if (!alert.RestorePointSet || !alert.AlertActive)
					Trace(L"restore-point space alert state cleared", ERROR_SUCCESS);
			}
			else
			{
				DWORD monitorError = GetLastError();
				if (monitorError == ERROR_SUCCESS)
					monitorError = ERROR_INVALID_DATA;
				if (monitorError != lastMonitorError)
					Trace(L"restore-point pushed alert completion failed", monitorError);
				lastMonitorError = monitorError;
				CloseHandle(alertControl);
				alertControl = INVALID_HANDLE_VALUE;
				nextMonitorConnect = GetTickCount64() + 5000;
			}
			alertWaitPending = FALSE;
		}
	}

	if (alertControl != INVALID_HANDLE_VALUE)
	{
		if (alertWaitPending)
		{
			CancelIoEx(alertControl, &alertOverlapped);
			(void)GetOverlappedResult(
				alertControl, &alertOverlapped, &result, TRUE);
		}
		CloseHandle(alertControl);
	}
	CloseHandle(alertEvent);
	return ERROR_SUCCESS;
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
	Trace(L"service worker stopped", exitCode);
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
