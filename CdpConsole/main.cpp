#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <Windows.h>
#include <objbase.h>
#include "..\CdpDriver\CdpIoctl.h"
#include "cdp_driver_install.h"

#pragma comment(lib, "ole32.lib")

static UINT64 g_PreviewHandle = 0;

static void ConOut(const wchar_t* text)
{
	DWORD written = 0;
	HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
	DWORD chars;

	if (!text || !hOut || hOut == INVALID_HANDLE_VALUE)
		return;

	chars = (DWORD)wcslen(text);
	if (WriteConsoleW(hOut, text, chars, &written, NULL))
		return;

	/* Redirected stdout (pipe/file): WriteConsole fails; write ACP bytes. */
	{
		char stackBuf[1024];
		char* ansi = stackBuf;
		BOOL heap = FALSE;
		int bytes = WideCharToMultiByte(CP_ACP, 0, text, (int)chars, NULL, 0, NULL, NULL);
		if (bytes <= 0)
			return;
		if (bytes > (int)sizeof(stackBuf))
		{
			ansi = (char*)malloc((size_t)bytes);
			if (!ansi)
				return;
			heap = TRUE;
		}
		WideCharToMultiByte(CP_ACP, 0, text, (int)chars, ansi, bytes, NULL, NULL);
		WriteFile(hOut, ansi, (DWORD)bytes, &written, NULL);
		if (heap)
			free(ansi);
	}
}

static void ConOutFmt(const wchar_t* fmt, ...)
{
	wchar_t buf[512];
	va_list args;
	va_start(args, fmt);
	_vsnwprintf_s(buf, _countof(buf), _TRUNCATE, fmt, args);
	va_end(args);
	ConOut(buf);
}

// Same layout as `hexdump -C`.
static void HexdumpC(
	_In_ UINT64 BaseOffset,
	_In_reads_bytes_(Length) const BYTE* Data,
	_In_ DWORD Length)
{
	DWORD i;

	for (i = 0; i < Length; i += 16)
	{
		wchar_t line[96];
		size_t pos = 0;
		DWORD j;
		DWORD lineLen = (Length - i > 16) ? 16 : (Length - i);

		pos += (size_t)swprintf_s(
			line + pos,
			_countof(line) - pos,
			L"%08llx  ",
			(unsigned long long)(BaseOffset + i));

		for (j = 0; j < 16; ++j)
		{
			if (j == 8)
				pos += (size_t)swprintf_s(line + pos, _countof(line) - pos, L" ");
			if (j < lineLen)
			{
				pos += (size_t)swprintf_s(
					line + pos,
					_countof(line) - pos,
					L"%02x ",
					Data[i + j]);
			}
			else
			{
				pos += (size_t)swprintf_s(line + pos, _countof(line) - pos, L"   ");
			}
		}

		pos += (size_t)swprintf_s(line + pos, _countof(line) - pos, L" |");
		for (j = 0; j < lineLen; ++j)
		{
			BYTE c = Data[i + j];
			pos += (size_t)swprintf_s(
				line + pos,
				_countof(line) - pos,
				L"%c",
				(c >= 0x20 && c <= 0x7e) ? (int)c : (int)'.');
		}
		swprintf_s(line + pos, _countof(line) - pos, L"|\n");
		ConOut(line);
	}
}

static BOOL ReadLine(wchar_t* buf, DWORD cch)
{
	DWORD n = 0;
	HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);

	if (!buf || cch == 0)
		return FALSE;

	if (ReadConsoleW(hIn, buf, cch - 1, &n, NULL))
	{
		while (n > 0 && (buf[n - 1] == L'\n' || buf[n - 1] == L'\r'))
			--n;
		buf[n] = L'\0';
		return TRUE;
	}

	/* Redirected stdin: read ACP bytes until newline. */
	{
		char ansi[512];
		DWORD total = 0;
		DWORD got = 0;
		for (;;)
		{
			if (total >= sizeof(ansi) - 1)
				break;
			if (!ReadFile(hIn, ansi + total, 1, &got, NULL) || got == 0)
			{
				if (total == 0)
					return FALSE;
				break;
			}
			if (ansi[total] == '\n')
				break;
			if (ansi[total] != '\r')
				++total;
		}
		ansi[total] = '\0';
		if (!MultiByteToWideChar(CP_ACP, 0, ansi, -1, buf, (int)cch))
			return FALSE;
		return TRUE;
	}
}

static BOOL ParseGuid(const wchar_t* text, GUID* out)
{
	wchar_t tmp[80];
	size_t len;

	if (!text || !out)
		return FALSE;

	while (*text == L' ' || *text == L'\t')
		++text;

	len = wcslen(text);
	if (len == 0 || len >= _countof(tmp) - 2)
		return FALSE;

	if (text[0] != L'{')
	{
		tmp[0] = L'{';
		wcsncpy_s(tmp + 1, _countof(tmp) - 1, text, _TRUNCATE);
		len = wcslen(tmp);
		if (tmp[len - 1] != L'}')
		{
			if (len + 1 >= _countof(tmp))
				return FALSE;
			tmp[len] = L'}';
			tmp[len + 1] = L'\0';
		}
		return SUCCEEDED(CLSIDFromString(tmp, (LPCLSID)out));
	}

	return SUCCEEDED(CLSIDFromString(text, (LPCLSID)out));
}

static void ListVolumes(void)
{
	wchar_t name[MAX_PATH];
	HANDLE find = FindFirstVolumeW(name, MAX_PATH);
	if (find == INVALID_HANDLE_VALUE)
	{
		ConOut(L"FindFirstVolume failed.\n");
		return;
	}

	ConOut(L"\nVolumes:\n");
	do
	{
		wchar_t paths[MAX_PATH] = { 0 };
		DWORD needed = 0;
		BOOL hasPath = GetVolumePathNamesForVolumeNameW(name, paths, MAX_PATH, &needed);
		ConOutFmt(L"  %s", name);
		if (hasPath && paths[0])
			ConOutFmt(L"  -> %s", paths);
		ConOut(L"\n");
	} while (FindNextVolumeW(find, name, MAX_PATH));
	FindVolumeClose(find);
	ConOut(L"\n");
}

static HANDLE OpenControlDevice(void)
{
	HANDLE hDevice = CreateFileW(
		Cdp_CONTROL_SYSTEM_LINK_NAME,
		GENERIC_READ | GENERIC_WRITE,
		0,
		NULL,
		OPEN_EXISTING,
		FILE_ATTRIBUTE_NORMAL,
		NULL);

	if (hDevice == INVALID_HANDLE_VALUE)
		ConOutFmt(L"Open control device failed (err=%lu).\n", GetLastError());
	return hDevice;
}

static HANDLE EnsureControlDevice(_In_opt_ HANDLE hDevice)
{
	if (hDevice != INVALID_HANDLE_VALUE && hDevice != NULL)
		return hDevice;

	ConOut(L"Reconnecting control device...\n");
	return OpenControlDevice();
}

static BOOL PromptGuid(const wchar_t* prompt, GUID* out)
{
	wchar_t line[128];
	ConOut(prompt);
	if (!ReadLine(line, _countof(line)))
		return FALSE;
	if (!ParseGuid(line, out))
	{
		ConOut(L"Invalid GUID.\n");
		return FALSE;
	}
	return TRUE;
}

static BOOL PromptPasswordUtf8(
	_In_ const wchar_t* prompt,
	_Out_writes_bytes_(Cdp_PASSWORD_MAX_UTF8_BYTES) UCHAR* password,
	_Out_ PULONG passwordLength)
{
	wchar_t wide[Cdp_PASSWORD_MAX_UTF8_BYTES] = { 0 };
	HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);
	DWORD oldMode = 0;
	BOOL hasMode = GetConsoleMode(hIn, &oldMode);
	int bytes;

	*passwordLength = 0;
	SecureZeroMemory(password, Cdp_PASSWORD_MAX_UTF8_BYTES);
	ConOut(prompt);
	if (hasMode)
		SetConsoleMode(hIn, oldMode & ~ENABLE_ECHO_INPUT);
	if (!ReadLine(wide, _countof(wide)))
	{
		if (hasMode) SetConsoleMode(hIn, oldMode);
		return FALSE;
	}
	if (hasMode)
	{
		SetConsoleMode(hIn, oldMode);
		ConOut(L"\n");
	}
	bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide, -1,
		(char*)password, Cdp_PASSWORD_MAX_UTF8_BYTES, NULL, NULL);
	SecureZeroMemory(wide, sizeof(wide));
	if (bytes <= 1)
		return FALSE;
	*passwordLength = (ULONG)(bytes - 1);
	return TRUE;
}

static BOOL QueryCredentialStatus(
	_In_ HANDLE hDevice,
	_Out_ PCdp_CREDENTIAL_STATUS_REPLY reply)
{
	DWORD returned = 0;
	ZeroMemory(reply, sizeof(*reply));
	return DeviceIoControl(hDevice, IOCTL_Cdp_QUERY_CREDENTIAL,
		NULL, 0, reply, sizeof(*reply), &returned, NULL) &&
		returned >= sizeof(*reply);
}

static BOOL AuthenticatePassword(HANDLE hDevice)
{
	Cdp_AUTH_REQUEST request = { 0 };
	DWORD returned = 0;
	BOOL ok;
	if (!PromptPasswordUtf8(L"Protection password: ", request.Password,
		&request.PasswordLength))
	{
		ConOut(L"Password cannot be empty.\n");
		return FALSE;
	}
	ok = DeviceIoControl(hDevice, IOCTL_Cdp_AUTHENTICATE,
		&request, sizeof(request), NULL, 0, &returned, NULL);
	SecureZeroMemory(&request, sizeof(request));
	if (!ok)
		ConOutFmt(L"Password authentication failed (err=%lu).\n", GetLastError());
	return ok;
}

static BOOL SendCmdBuffered(HANDLE hDevice, const void* req, DWORD reqSize)
{
	Cdp_COMMAND_REPLY reply = { 0 };
	DWORD bytesReturned = 0;

	if (!DeviceIoControl(hDevice, IOCTL_Cdp_SEND_COMMAND,
		(LPVOID)req, reqSize, &reply, sizeof(reply), &bytesReturned, NULL))
	{
		ConOutFmt(L"DeviceIoControl failed (err=%lu)\n", GetLastError());
		return FALSE;
	}

	ConOutFmt(L"Reply: Command=%lu Result=%lu Handle=%llu Message=%s\n",
		reply.Command, reply.Result, reply.VolumeHandle, reply.Message);

	return TRUE;
}

static BOOL DoCommand1(HANDLE hDevice)
{
	Cdp_CMD1_REQUEST_V2 req = { 0 };
	Cdp_CREDENTIAL_STATUS_REPLY credential = { 0 };
	wchar_t line[16];
	req.Code = Cdp_CMD_1;
	ListVolumes();
	if (!PromptGuid(L"Protected source volume GUID: ", &req.PartitionGuid1))
		return FALSE;
	if (!PromptGuid(L"Dedicated journal partition GUID: ", &req.PartitionGuid2))
		return FALSE;
	ConOut(L"FORMAT the journal partition? This destroys its contents [y/N]: ");
	if (!ReadLine(line, _countof(line)))
		return FALSE;
	req.FormatJournal = (line[0] == L'y' || line[0] == L'Y') ? 1 : 0;
	if (req.FormatJournal &&
		(!QueryCredentialStatus(hDevice, &credential) || !credential.Configured))
	{
		UCHAR confirmation[Cdp_PASSWORD_MAX_UTF8_BYTES] = { 0 };
		ULONG confirmationLength = 0;
		if (!PromptPasswordUtf8(L"Create global protection password: ",
			req.Password, &req.PasswordLength) || req.PasswordLength < 6)
		{
			ConOut(L"Password must contain at least 6 UTF-8 bytes.\n");
			SecureZeroMemory(&req, sizeof(req));
			return FALSE;
		}
		if (!PromptPasswordUtf8(L"Confirm protection password: ",
			confirmation, &confirmationLength) ||
			confirmationLength != req.PasswordLength ||
			memcmp(confirmation, req.Password, req.PasswordLength) != 0)
		{
			ConOut(L"Passwords do not match.\n");
			SecureZeroMemory(confirmation, sizeof(confirmation));
			SecureZeroMemory(&req, sizeof(req));
			return FALSE;
		}
		SecureZeroMemory(confirmation, sizeof(confirmation));
	}
	{
		BOOL ok = SendCmdBuffered(hDevice, &req, sizeof(req));
		SecureZeroMemory(&req, sizeof(req));
		return ok;
	}
}

static BOOL DoCommand2(HANDLE hDevice)
{
	Cdp_CMD2_REQUEST req = { 0 };

	req.Code = Cdp_CMD_2;
	ListVolumes();
	if (!PromptGuid(L"Protected source volume GUID to stop: ", &req.SourceVolumeGuid))
		return FALSE;
	if (!AuthenticatePassword(hDevice))
		return FALSE;
	ConOut(L"Stopping capture for source...\n");
	return SendCmdBuffered(hDevice, &req, sizeof(req));
}

static BOOL DoPasswordManagement(HANDLE hDevice)
{
	Cdp_CREDENTIAL_STATUS_REPLY status = { 0 };
	wchar_t line[32] = { 0 };
	if (!QueryCredentialStatus(hDevice, &status))
	{
		ConOutFmt(L"Query credential status failed (err=%lu).\n", GetLastError());
		return FALSE;
	}
	ConOutFmt(L"Credential: %s, journals=%lu, epoch=%llu\n",
		status.Configured ? L"configured" : L"not configured",
		status.JournalCount, status.AuthEpoch);
	ConOut(L"Password action [s=set first journal, v=verify, c=change, Enter=status]: ");
	if (!ReadLine(line, _countof(line)) || line[0] == L'\0')
		return TRUE;
	if (line[0] == L's' || line[0] == L'S')
	{
		if (status.Configured)
		{
			ConOut(L"A global password already exists. Use change instead.\n");
			return FALSE;
		}
		ConOut(L"The first password is set atomically while formatting the first journal.\n");
		return DoCommand1(hDevice);
	}
	if (line[0] == L'v' || line[0] == L'V')
		return AuthenticatePassword(hDevice);
	if (line[0] == L'c' || line[0] == L'C')
	{
		Cdp_CHANGE_PASSWORD_REQUEST request = { 0 };
		UCHAR confirmation[Cdp_PASSWORD_MAX_UTF8_BYTES] = { 0 };
		ULONG confirmationLength = 0;
		DWORD returned = 0;
		BOOL ok;
		if (!status.Configured || !AuthenticatePassword(hDevice))
			return FALSE;
		if (!PromptPasswordUtf8(L"New protection password: ",
			request.Password, &request.PasswordLength) || request.PasswordLength < 6 ||
			!PromptPasswordUtf8(L"Confirm new password: ",
				confirmation, &confirmationLength) ||
			confirmationLength != request.PasswordLength ||
			memcmp(confirmation, request.Password, request.PasswordLength) != 0)
		{
			ConOut(L"New passwords are invalid or do not match.\n");
			SecureZeroMemory(&request, sizeof(request));
			SecureZeroMemory(confirmation, sizeof(confirmation));
			return FALSE;
		}
		ok = DeviceIoControl(hDevice, IOCTL_Cdp_CHANGE_PASSWORD,
			&request, sizeof(request), NULL, 0, &returned, NULL);
		SecureZeroMemory(&request, sizeof(request));
		SecureZeroMemory(confirmation, sizeof(confirmation));
		if (!ok)
			ConOutFmt(L"Change password failed (err=%lu).\n", GetLastError());
		else
			ConOut(L"Global protection password changed.\n");
		return ok;
	}
	ConOut(L"Unknown password action.\n");
	return FALSE;
}

static void PrintMaxReadHint(void)
{
	ConOutFmt(
		L"  (Preview read: max %u bytes per request, 2 MiB)\n",
		Cdp_CMD3_MAX_READ_BYTES);
}

static void PrintTime100nsLabel(_In_ const wchar_t* label, _In_ UINT64 time100ns)
{
	FILETIME ft;
	SYSTEMTIME st;

	ConOutFmt(L"%s: %llu", label, time100ns);
	if (time100ns == 0)
	{
		ConOut(L" (n/a)\n");
		return;
	}

	ft.dwLowDateTime = (DWORD)(time100ns & 0xFFFFFFFFUL);
	ft.dwHighDateTime = (DWORD)(time100ns >> 32);
	if (FileTimeToSystemTime(&ft, &st))
	{
		ConOutFmt(
			L"  (%04u-%02u-%02u %02u:%02u:%02u local)\n",
			st.wYear,
			st.wMonth,
			st.wDay,
			st.wHour,
			st.wMinute,
			st.wSecond);
	}
	else
	{
		ConOut(L"\n");
	}
}

static BOOL DoConvertLocalTimeTo100ns(void)
{
	wchar_t line[128];
	wchar_t extra = L'\0';
	SYSTEMTIME st = { 0 };
	SYSTEMTIME verify = { 0 };
	FILETIME ft = { 0 };
	ULARGE_INTEGER value;
	unsigned year;
	unsigned month;
	unsigned day;
	unsigned hour;
	unsigned minute;
	unsigned second;
	int fields;

	ConOut(L"Local time (year-month-day hour:minute:second): ");
	if (!ReadLine(line, _countof(line)))
		return FALSE;

	fields = swscanf_s(
		line,
		L" %u-%u-%u %u:%u:%u %c",
		&year,
		&month,
		&day,
		&hour,
		&minute,
		&second,
		&extra,
		(unsigned)1);
	if (fields != 6 || year > 0xFFFFU || month > 0xFFFFU ||
		day > 0xFFFFU || hour > 0xFFFFU || minute > 0xFFFFU ||
		second > 0xFFFFU)
	{
		ConOut(L"Invalid time. Expected: year-month-day hour:minute:second\n");
		return FALSE;
	}
	st.wYear = (WORD)year;
	st.wMonth = (WORD)month;
	st.wDay = (WORD)day;
	st.wHour = (WORD)hour;
	st.wMinute = (WORD)minute;
	st.wSecond = (WORD)second;
	if (!SystemTimeToFileTime(&st, &ft) ||
		!FileTimeToSystemTime(&ft, &verify) ||
		verify.wYear != st.wYear ||
		verify.wMonth != st.wMonth ||
		verify.wDay != st.wDay ||
		verify.wHour != st.wHour ||
		verify.wMinute != st.wMinute ||
		verify.wSecond != st.wSecond)
	{
		ConOut(L"Invalid time. Expected: year-month-day hour:minute:second\n");
		return FALSE;
	}

	value.LowPart = ft.dwLowDateTime;
	value.HighPart = ft.dwHighDateTime;
	ConOutFmt(L"WallClock100ns: %llu\n", value.QuadPart);
	return TRUE;
}

static BOOL DoPreviewBegin(HANDLE hDevice)
{
	Cdp_PREVIEW_BEGIN_REQUEST req = { 0 };
	Cdp_PREVIEW_BEGIN_REPLY reply = { 0 };
	DWORD bytesReturned = 0;
	wchar_t line[64];

	ListVolumes();
	if (!PromptGuid(L"Protected source volume GUID: ", &req.SourceVolumeGuid))
		return FALSE;
	if (!AuthenticatePassword(hDevice))
		return FALSE;
	ConOut(L"TargetTime100ns (local wall-clock FILETIME value): ");
	if (!ReadLine(line, _countof(line)))
		return FALSE;
	req.TargetTime100ns = _wcstoui64(line, NULL, 10);
	if (!req.TargetTime100ns)
	{
		ConOut(L"Invalid target time.\n");
		return FALSE;
	}
	if (!DeviceIoControl(hDevice, IOCTL_Cdp_BEGIN_PREVIEW,
		&req, sizeof(req), &reply, sizeof(reply), &bytesReturned, NULL))
	{
		ConOutFmt(L"Begin preview failed (err=%lu).\n", GetLastError());
		return FALSE;
	}
	g_PreviewHandle = reply.PreviewHandle;
	ConOutFmt(L"PreviewHandle=%llu target=%llu range=[%llu, %llu]\n",
		reply.PreviewHandle,
		reply.TargetTime100ns,
		reply.OldestRecoverable100ns,
		reply.NewestRecoverable100ns);
	return TRUE;
}

static BOOL DoPreviewRead(HANDLE hDevice)
{
	Cdp_PREVIEW_READ_REQUEST req = { 0 };
	BYTE* buffer;
	DWORD bytesReturned = 0;
	wchar_t line[64];

	if (!g_PreviewHandle)
	{
		ConOut(L"No preview session. Run command 6 first.\n");
		return FALSE;
	}
	req.PreviewHandle = g_PreviewHandle;
	ConOut(L"Volume byte offset: ");
	if (!ReadLine(line, _countof(line)))
		return FALSE;
	req.ByteOffset = _wcstoui64(line, NULL, 0);
	ConOutFmt(L"Length (1..%u): ", Cdp_CMD3_MAX_READ_BYTES);
	if (!ReadLine(line, _countof(line)))
		return FALSE;
	req.ByteLength = (ULONG)wcstoul(line, NULL, 0);
	if (!req.ByteLength || req.ByteLength > Cdp_CMD3_MAX_READ_BYTES)
	{
		ConOutFmt(L"Length must be 1..%u\n", Cdp_CMD3_MAX_READ_BYTES);
		return FALSE;
	}

	buffer = (BYTE*)VirtualAlloc(
		NULL,
		req.ByteLength,
		MEM_COMMIT | MEM_RESERVE,
		PAGE_READWRITE);
	if (!buffer)
		return FALSE;

	if (!DeviceIoControl(hDevice, IOCTL_Cdp_READ_PREVIEW,
		&req, sizeof(req), buffer, req.ByteLength, &bytesReturned, NULL))
	{
		ConOutFmt(L"Preview read failed (err=%lu).\n", GetLastError());
		VirtualFree(buffer, 0, MEM_RELEASE);
		return FALSE;
	}

	ConOutFmt(L"Preview returned %lu bytes:\n", bytesReturned);
	HexdumpC(req.ByteOffset, buffer, bytesReturned);
	VirtualFree(buffer, 0, MEM_RELEASE);
	return TRUE;
}

static BOOL DoPreviewEnd(HANDLE hDevice)
{
	Cdp_PREVIEW_END_REQUEST req = { 0 };
	DWORD bytesReturned = 0;

	if (!g_PreviewHandle)
	{
		ConOut(L"No preview session.\n");
		return FALSE;
	}
	req.PreviewHandle = g_PreviewHandle;
	if (!DeviceIoControl(hDevice, IOCTL_Cdp_END_PREVIEW,
		&req, sizeof(req), NULL, 0, &bytesReturned, NULL))
	{
		ConOutFmt(L"End preview failed (err=%lu).\n", GetLastError());
		return FALSE;
	}
	g_PreviewHandle = 0;
	ConOut(L"Preview session closed.\n");
	return TRUE;
}

static BOOL DoRecoveryBegin(HANDLE hDevice)
{
	Cdp_RECOVERY_BEGIN_REQUEST req = { 0 };
	Cdp_RECOVERY_BEGIN_REPLY reply = { 0 };
	DWORD bytesReturned = 0;
	wchar_t line[64];

	ListVolumes();
	if (!PromptGuid(L"Protected source volume GUID: ", &req.SourceVolumeGuid))
		return FALSE;
	if (!AuthenticatePassword(hDevice))
		return FALSE;
	ConOut(L"TargetTime100ns (local wall-clock FILETIME value): ");
	if (!ReadLine(line, _countof(line)))
		return FALSE;
	req.TargetTime100ns = _wcstoui64(line, NULL, 10);
	if (!req.TargetTime100ns)
	{
		ConOut(L"Invalid target time.\n");
		return FALSE;
	}
	ConOut(L"Persist this recovery for automatic begin+commit after reboot? (y/N): ");
	if (!ReadLine(line, _countof(line)))
		return FALSE;
	if (line[0] == L'y' || line[0] == L'Y')
		req.Flags |= Cdp_RECOVERY_BEGIN_FLAG_ON_REBOOT;

	if (!DeviceIoControl(
			hDevice,
			IOCTL_Cdp_BEGIN_RECOVERY,
			&req,
			sizeof(req),
			&reply,
			sizeof(reply),
			&bytesReturned,
			NULL))
	{
		ConOutFmt(L"Begin recovery failed (err=%lu).\n", GetLastError());
		return FALSE;
	}

	if ((req.Flags & Cdp_RECOVERY_BEGIN_FLAG_ON_REBOOT) != 0)
	{
		ConOutFmt(L"Reboot recovery intent saved. target=%llu range=[%llu, %llu]\n",
			reply.TargetTime100ns,
			reply.OldestRecoverable100ns,
			reply.NewestRecoverable100ns);
		ConOut(L"No recovery begin was run. After reboot it will automatically begin and commit.\n");
	}
	else
	{
		ConOutFmt(
			L"Recovery prepared. Phase=%lu target=%llu range=[%llu, %llu]\n",
			reply.Phase,
			reply.TargetTime100ns,
			reply.OldestRecoverable100ns,
			reply.NewestRecoverable100ns);
		ConOut(L"No writeback has occurred. Bring the source online, then use r to commit.\n");
	}
	return TRUE;
}

static BOOL DoRecoveryCommit(HANDLE hDevice)
{
	Cdp_RECOVERY_CONTROL_REQUEST req = { 0 };
	Cdp_RECOVERY_COMMIT_REPLY reply = { 0 };
	DWORD bytesReturned = 0;

	ListVolumes();
	if (!PromptGuid(L"Prepared source volume GUID: ", &req.SourceVolumeGuid))
		return FALSE;
	if (!AuthenticatePassword(hDevice))
		return FALSE;

	if (!DeviceIoControl(
			hDevice,
			IOCTL_Cdp_COMMIT_RECOVERY,
			&req,
			sizeof(req),
			&reply,
			sizeof(reply),
			&bytesReturned,
			NULL))
	{
		ConOutFmt(L"Recovery commit failed (err=%lu).\n", GetLastError());
		return FALSE;
	}

	ConOutFmt(
		L"Recovery committed. Phase=%lu target=%llu\n",
		reply.Phase,
		reply.TargetTime100ns);
	ConOut(L"Writeback completed and the source volume is back in General phase.\n");
	return TRUE;
}

static BOOL DoRecoveryCancel(HANDLE hDevice)
{
	Cdp_RECOVERY_CONTROL_REQUEST req = { 0 };
	DWORD bytesReturned = 0;

	ListVolumes();
	if (!PromptGuid(L"Prepared source volume GUID: ", &req.SourceVolumeGuid))
		return FALSE;
	if (!AuthenticatePassword(hDevice))
		return FALSE;

	if (!DeviceIoControl(
			hDevice,
			IOCTL_Cdp_CANCEL_RECOVERY,
			&req,
			sizeof(req),
			NULL,
			0,
			&bytesReturned,
			NULL))
	{
		ConOutFmt(L"Recovery cancel failed (err=%lu).\n", GetLastError());
		return FALSE;
	}

	ConOut(L"Prepared recovery cancelled; source volume returned to General phase.\n");
	return TRUE;
}

static void PrintGuidLine(_In_ const wchar_t* label, _In_ const GUID* Guid)
{
	ConOutFmt(
		L"%s{%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}\n",
		label,
		Guid->Data1,
		Guid->Data2,
		Guid->Data3,
		Guid->Data4[0], Guid->Data4[1],
		Guid->Data4[2], Guid->Data4[3],
		Guid->Data4[4], Guid->Data4[5],
		Guid->Data4[6], Guid->Data4[7]);
}

static BOOL DoQueryStatus(HANDLE hDevice)
{
	Cdp_PHASE_QUERY_REQUEST req = { 0 };
	Cdp_PHASE_QUERY_REPLY reply = { 0 };
	DWORD bytesReturned = 0;
	GUID zeroGuid = { 0 };

	ListVolumes();
	if (!PromptGuid(L"Source volume GUID: ", &req.SourceVolumeGuid))
		return FALSE;

	if (!DeviceIoControl(
			hDevice,
			IOCTL_Cdp_QUERY_PHASE,
			&req,
			sizeof(req),
			&reply,
			sizeof(reply),
			&bytesReturned,
			NULL))
	{
		ConOutFmt(L"Query status failed (err=%lu).\n", GetLastError());
		return FALSE;
	}

	ConOutFmt(L"Status=%ld ", reply.Status);
	switch (reply.Status)
	{
	case Cdp_STATUS_UNPROTECTED:
		ConOut(L"(unprotected)\n");
		break;
	case (LONG)Cdp_PHASE_GENERAL:
		ConOut(L"(general)\n");
		break;
	case (LONG)Cdp_PHASE_PREVIEW:
		ConOut(L"(preview)\n");
		break;
	case (LONG)Cdp_PHASE_RECOVERY:
		ConOut(L"(recovery)\n");
		break;
	case (LONG)Cdp_PHASE_DRAINING:
		ConOut(L"(draining to source)\n");
		break;
	default:
		ConOut(L"(unknown)\n");
		break;
	}

	if (reply.Status >= 0 &&
		memcmp(&reply.JournalPartitionGuid, &zeroGuid, sizeof(GUID)) != 0)
	{
		PrintGuidLine(L"Journal GUID: ", &reply.JournalPartitionGuid);
	}
	return TRUE;
}

static BOOL DoQueryTimeRange(HANDLE hDevice)
{
	Cdp_TIME_RANGE_QUERY_REQUEST req = { 0 };
	Cdp_TIME_RANGE_QUERY_REPLY reply = { 0 };
	DWORD bytesReturned = 0;

	ListVolumes();
	if (!PromptGuid(L"Protected source volume GUID: ", &req.SourceVolumeGuid))
		return FALSE;

	if (!DeviceIoControl(
			hDevice,
			IOCTL_Cdp_QUERY_TIME_RANGE,
			&req,
			sizeof(req),
			&reply,
			sizeof(reply),
			&bytesReturned,
			NULL))
	{
		ConOutFmt(L"Query time range failed (err=%lu).\n", GetLastError());
		return FALSE;
	}

	if (!reply.HasRecords)
	{
		ConOut(L"Journal has no COW records yet.\n");
		return TRUE;
	}

	ConOut(L"Journal record time range (WallClock100ns / local):\n");
	PrintTime100nsLabel(L"  Oldest", reply.OldestRecord100ns);
	PrintTime100nsLabel(L"  Newest", reply.NewestRecord100ns);
	return TRUE;
}

static void PrintByteCount(_In_ const wchar_t* label, _In_ UINT64 bytes)
{
	ConOutFmt(
		L"  %s: %llu bytes (%llu MiB)\n",
		label,
		bytes,
		bytes / (1024ULL * 1024ULL));
}

static BOOL DoQueryJournalUsage(HANDLE hDevice)
{
	Cdp_JOURNAL_USAGE_QUERY_REQUEST req = { 0 };
	Cdp_JOURNAL_USAGE_QUERY_REPLY reply = { 0 };
	DWORD bytesReturned = 0;

	ListVolumes();
	if (!PromptGuid(L"Protected source volume GUID: ", &req.SourceVolumeGuid))
		return FALSE;

	if (!DeviceIoControl(
			hDevice,
			IOCTL_Cdp_QUERY_JOURNAL_USAGE,
			&req,
			sizeof(req),
			&reply,
			sizeof(reply),
			&bytesReturned,
			NULL))
	{
		ConOutFmt(L"Query journal usage failed (err=%lu).\n", GetLastError());
		return FALSE;
	}

	ConOutFmt(L"Journal records: %llu\n", reply.TotalRecords);
	PrintByteCount(L"Journal partition", reply.JournalPartitionBytes);
	PrintByteCount(L"Journal metadata", reply.JournalMetadataBytes);
	PrintByteCount(L"Record payload used", reply.RecordPayloadBytesUsed);
	PrintByteCount(L"Record payload free", reply.RecordPayloadBytesFree);
	return TRUE;
}

static BOOL DoListJournalRecords(HANDLE hDevice)
{
	const ULONG batchSize = Cdp_JOURNAL_RECORD_QUERY_MAX_PER_CALL;
	const SIZE_T bufferSize = sizeof(Cdp_JOURNAL_RECORD_QUERY_REPLY) +
		(SIZE_T)batchSize * sizeof(Cdp_JOURNAL_RECORD_INFO);
	Cdp_JOURNAL_RECORD_QUERY_REQUEST req = { 0 };
	BYTE* buffer;
	UINT64 nextIndex = 0;
	UINT64 generation = 0;
	UINT64 totalRecords = 0;
	DWORD bytesReturned;
	BOOL firstPage = TRUE;

	ListVolumes();
	if (!PromptGuid(L"Protected source volume GUID: ", &req.SourceVolumeGuid))
		return FALSE;
	if (!AuthenticatePassword(hDevice))
		return FALSE;

	buffer = (BYTE*)malloc(bufferSize);
	if (!buffer)
	{
		ConOut(L"Unable to allocate record-list buffer.\n");
		return FALSE;
	}

	for (;;)
	{
		PCdp_JOURNAL_RECORD_QUERY_REPLY reply;
		PCdp_JOURNAL_RECORD_INFO records;
		ULONG i;

		req.StartIndex = nextIndex;
		req.ExpectedGeneration = generation;
		req.MaxRecords = batchSize;
		req.Reserved = 0;
		bytesReturned = 0;
		if (!DeviceIoControl(
				hDevice,
				IOCTL_Cdp_QUERY_JOURNAL_RECORDS,
				&req,
				sizeof(req),
				buffer,
				(DWORD)bufferSize,
				&bytesReturned,
				NULL))
		{
			DWORD err = GetLastError();
			if (err == ERROR_RETRY)
			{
				ConOut(L"Journal changed while listing records; no complete snapshot was produced. Run l again.\n");
			}
			else
			{
				ConOutFmt(L"List journal records failed (err=%lu).\n", err);
			}
			free(buffer);
			return FALSE;
		}

		reply = (PCdp_JOURNAL_RECORD_QUERY_REPLY)buffer;
		if (bytesReturned < sizeof(*reply) ||
			reply->RecordCount > batchSize ||
			bytesReturned < sizeof(*reply) +
				reply->RecordCount * sizeof(Cdp_JOURNAL_RECORD_INFO))
		{
			ConOut(L"Driver returned an invalid record-list reply.\n");
			free(buffer);
			return FALSE;
		}
		if (firstPage)
		{
			totalRecords = reply->TotalRecords;
			generation = reply->Generation;
			ConOutFmt(
				L"Journal record list: %llu record(s), oldest first (metadata only)\n",
				totalRecords);
			ConOut(L"Index  WallClock100ns  VolumeOffset  JournalOffset  Length  Sequence  Flags\n");
			firstPage = FALSE;
		}
		else if (reply->TotalRecords != totalRecords ||
			reply->Generation != generation)
		{
			ConOut(L"Journal changed while listing records; no complete snapshot was produced. Run l again.\n");
			free(buffer);
			return FALSE;
		}

		records = (PCdp_JOURNAL_RECORD_INFO)(reply + 1);
		for (i = 0; i < reply->RecordCount; ++i)
		{
			ConOutFmt(
				L"%5llu  %llu  %llu  %llu  %lu  %llu  0x%08lX%s\n",
				nextIndex + i,
				records[i].WallClock100ns,
				records[i].VolumeOffset,
				records[i].FileOffset,
				records[i].DataLength,
				records[i].Sequence,
				records[i].Flags,
				(records[i].Flags & Cdp_RECORD_FLAG_BRANCH) != 0 ?
					L" (branch)" : L"");
		}

		nextIndex += reply->RecordCount;
		if (nextIndex >= totalRecords)
			break;
		if (reply->RecordCount == 0)
		{
			ConOut(L"Driver returned an incomplete record-list page.\n");
			free(buffer);
			return FALSE;
		}
	}

	free(buffer);
	return TRUE;
}

typedef struct _Cdp_CONSOLE_BRANCH_ENTRY
{
	Cdp_JOURNAL_BRANCH_INFO Info;
	BOOL Printed;
} Cdp_CONSOLE_BRANCH_ENTRY, *PCdp_CONSOLE_BRANCH_ENTRY;

static BOOL HasLaterUnprintedBranchSibling(
	_In_reads_(Count) const Cdp_CONSOLE_BRANCH_ENTRY* Entries,
	_In_ ULONG Count,
	_In_ LONG ParentBranchNumber,
	_In_ ULONG Index)
{
	for (ULONG i = Index + 1; i < Count; ++i)
	{
		if (!Entries[i].Printed &&
			Entries[i].Info.ParentBranchNumber == ParentBranchNumber)
		{
			return TRUE;
		}
	}
	return FALSE;
}

static void PrintBranchEntry(
	_In_ const Cdp_CONSOLE_BRANCH_ENTRY* Entry,
	_In_ ULONG Depth,
	_In_ BOOL IsLast,
	_In_reads_(Depth) const BOOL* AncestorHasMore)
{
	wchar_t prefix[256];
	ULONG chars = 0;
	ULONG level;

	for (level = 0; level < Depth && chars + 2 < _countof(prefix); ++level)
	{
		prefix[chars++] = AncestorHasMore[level] ? L'|' : L' ';
		prefix[chars++] = L' ';
	}
	if (chars + 3 < _countof(prefix))
	{
		prefix[chars++] = IsLast ? L'\\' : L'+';
		prefix[chars++] = L'-';
		prefix[chars++] = L'-';
	}
	prefix[chars] = L'\0';

	ConOutFmt(L"%s Branch %ld%s%s",
		prefix,
		Entry->Info.BranchNumber,
		(Entry->Info.Flags & Cdp_BRANCH_INFO_FLAG_CURRENT) != 0 ?
			L" [CURRENT]" : L"",
		(Entry->Info.Flags & Cdp_BRANCH_INFO_FLAG_SYNTHETIC) != 0 ?
			L" [RETAINED-BASE]" : L"");
	if (Entry->Info.ParentBranchNumber == 0)
	{
		ConOutFmt(L"  root | created=%llu | retained seq=%llu..%llu\n",
			Entry->Info.CreatedWallClock100ns,
			Entry->Info.StartSequence,
			Entry->Info.EndSequence);
	}
	else
	{
		ConOutFmt(
			L"  <- Branch %ld @ inherit record #%llu | created=%llu | retained seq=%llu..%llu\n",
			Entry->Info.ParentBranchNumber,
			Entry->Info.InheritedRecordSequence,
			Entry->Info.CreatedWallClock100ns,
			Entry->Info.StartSequence,
			Entry->Info.EndSequence);
	}
}

static void PrintBranchChildren(
	_Inout_updates_(Count) PCdp_CONSOLE_BRANCH_ENTRY Entries,
	_In_ ULONG Count,
	_In_ LONG ParentBranchNumber,
	_In_ ULONG Depth,
	_Inout_updates_(64) BOOL* AncestorHasMore)
{
	ULONG i;

	if (Depth > Count || Depth >= 64)
		return;
	for (i = 0; i < Count; ++i)
	{
		if (Entries[i].Printed ||
			Entries[i].Info.ParentBranchNumber != ParentBranchNumber)
		{
			continue;
		}
		BOOL hasLaterSibling = HasLaterUnprintedBranchSibling(
			Entries, Count, ParentBranchNumber, i);
		Entries[i].Printed = TRUE;
		PrintBranchEntry(&Entries[i], Depth, !hasLaterSibling,
			AncestorHasMore);
		AncestorHasMore[Depth] = hasLaterSibling;
		PrintBranchChildren(
			Entries,
			Count,
			Entries[i].Info.BranchNumber,
			Depth + 1,
			AncestorHasMore);
	}
}

static BOOL DoListJournalBranches(HANDLE hDevice)
{
	const ULONG batchSize = Cdp_JOURNAL_BRANCH_QUERY_MAX_PER_CALL;
	const SIZE_T bufferSize = sizeof(Cdp_JOURNAL_BRANCH_QUERY_REPLY) +
		(SIZE_T)batchSize * sizeof(Cdp_JOURNAL_BRANCH_INFO);
	Cdp_JOURNAL_BRANCH_QUERY_REQUEST req = { 0 };
	BYTE* buffer = NULL;
	PCdp_CONSOLE_BRANCH_ENTRY entries = NULL;
	UINT64 nextIndex = 0;
	UINT64 generation = 0;
	ULONG totalBranches = 0;
	LONG currentBranch = 0;
	ULONG copied = 0;
	BOOL firstPage = TRUE;
	DWORD bytesReturned;

	ListVolumes();
	if (!PromptGuid(L"Protected source volume GUID: ", &req.SourceVolumeGuid))
		return FALSE;
	if (!AuthenticatePassword(hDevice))
		return FALSE;

	buffer = (BYTE*)malloc(bufferSize);
	if (!buffer)
	{
		ConOut(L"Unable to allocate branch-query buffer.\n");
		return FALSE;
	}

	for (;;)
	{
		PCdp_JOURNAL_BRANCH_QUERY_REPLY reply;
		PCdp_JOURNAL_BRANCH_INFO branches;

		req.StartIndex = nextIndex;
		req.ExpectedGeneration = generation;
		req.MaxBranches = batchSize;
		req.Reserved = 0;
		bytesReturned = 0;
		if (!DeviceIoControl(
				hDevice,
				IOCTL_Cdp_QUERY_JOURNAL_BRANCHES,
				&req,
				sizeof(req),
				buffer,
				(DWORD)bufferSize,
				&bytesReturned,
				NULL))
		{
			DWORD err = GetLastError();
			ConOutFmt(err == ERROR_RETRY ?
				L"Journal changed while listing branches; run b again.\n" :
				L"List journal branches failed (err=%lu).\n",
				err);
			free(entries);
			free(buffer);
			return FALSE;
		}

		reply = (PCdp_JOURNAL_BRANCH_QUERY_REPLY)buffer;
		if (bytesReturned < sizeof(*reply) ||
			reply->BranchCount > batchSize ||
			bytesReturned < sizeof(*reply) +
				reply->BranchCount * sizeof(Cdp_JOURNAL_BRANCH_INFO))
		{
			ConOut(L"Driver returned an invalid branch-list reply.\n");
			free(entries);
			free(buffer);
			return FALSE;
		}
		if (firstPage)
		{
			totalBranches = reply->TotalBranches;
			generation = reply->Generation;
			currentBranch = reply->CurrentBranchNumber;
			if (totalBranches != 0)
			{
				entries = (PCdp_CONSOLE_BRANCH_ENTRY)calloc(
					totalBranches, sizeof(*entries));
				if (!entries)
				{
					ConOut(L"Unable to allocate branch topology.\n");
					free(buffer);
					return FALSE;
				}
			}
			firstPage = FALSE;
		}
		else if (reply->TotalBranches != totalBranches ||
			reply->Generation != generation ||
			reply->CurrentBranchNumber != currentBranch)
		{
			ConOut(L"Journal changed while listing branches; run b again.\n");
			free(entries);
			free(buffer);
			return FALSE;
		}

		if (reply->BranchCount > totalBranches - copied)
		{
			ConOut(L"Driver returned an inconsistent branch-list page.\n");
			free(entries);
			free(buffer);
			return FALSE;
		}
		branches = (PCdp_JOURNAL_BRANCH_INFO)(reply + 1);
		for (ULONG i = 0; i < reply->BranchCount; ++i)
			entries[copied + i].Info = branches[i];
		copied += reply->BranchCount;
		nextIndex += reply->BranchCount;
		if (nextIndex >= totalBranches)
			break;
		if (reply->BranchCount == 0)
		{
			ConOut(L"Driver returned an incomplete branch-list page.\n");
			free(entries);
			free(buffer);
			return FALSE;
		}
	}

	ConOutFmt(L"Journal branch topology: %lu retained branch(es); current=Branch %ld\n",
		totalBranches, currentBranch);
	if (totalBranches == 0)
	{
		ConOut(L"  (no retained branches)\n");
	}
	else
	{
		BOOL ancestorHasMore[64] = { FALSE };
		ConOut(L"Legend: `<- Branch N @ inherit record #S` means this branch inherits parent state through record S.\n\n");
		PrintBranchChildren(entries, totalBranches, 0, 0, ancestorHasMore);
		for (ULONG i = 0; i < totalBranches; ++i)
		{
			if (!entries[i].Printed)
			{
				ConOut(L"Orphaned retained branch (parent was compacted/pruned):\n");
				entries[i].Printed = TRUE;
				PrintBranchEntry(&entries[i], 0, TRUE, ancestorHasMore);
				PrintBranchChildren(
					entries,
					totalBranches,
					entries[i].Info.BranchNumber,
					1,
					ancestorHasMore);
			}
		}
	}

	free(entries);
	free(buffer);
	return TRUE;
}

static BOOL DoQueryVersion(HANDLE hDevice)
{
	Cdp_VERSION_REPLY reply = { 0 };
	DWORD bytesReturned = 0;
	DWORD err;

	if (!DeviceIoControl(
			hDevice,
			IOCTL_Cdp_QUERY_VERSION,
			NULL,
			0,
			&reply,
			sizeof(reply),
			&bytesReturned,
			NULL))
	{
		err = GetLastError();
		ConOutFmt(L"Query driver version failed (err=%lu).\n", err);
		if (err == ERROR_INVALID_FUNCTION || err == ERROR_NOT_SUPPORTED)
		{
			ConOut(L"Loaded driver is older than this console (missing QUERY_VERSION).\n");
			ConOut(L"Run 'i' to update CdpDriver.sys, then reboot and try 'd' again.\n");
		}
		return FALSE;
	}

	ConOutFmt(L"Driver version: %hs\n", reply.Version);
	ConOutFmt(L"Build:          %hs\n", reply.Build);
	ConOutFmt(L"Journal:        v%lu\n", reply.JournalVersion);
	return TRUE;
}

static BOOL DoInstallDriver(void)
{
	wchar_t infPath[MAX_PATH];

	if (CdpIsDriverServiceInstalled())
		ConOut(L"CdpDriver service already installed.\n");
	else
		ConOut(L"Installing CdpDriver from INF...\n");

	if (!CdpInstallDriverPackage())
	{
		if (!CdpResolveDriverInfPath(infPath, _countof(infPath)))
		{
			ConOut(L"Install failed: CdpDriver.inf/.sys not found.\n");
			ConOut(L"Expected locations (relative to CdpConsole.exe):\n");
			ConOut(L"  .\\driver\\CdpDriver.inf\n");
			ConOut(L"  .\\CdpDriver.inf\n");
		}
		else
		{
			ConOutFmt(L"Install failed for: %s\n", infPath);
			ConOut(L"Run as Administrator and ensure test-signing is enabled.\n");
			ConOut(L"Also use the matching build folder (same tree as CdpConsole.exe).\n");
		}
		return FALSE;
	}

	if (CdpResolveDriverInfPath(infPath, _countof(infPath)))
		ConOutFmt(L"Driver package: %s\n", infPath);
	ConOut(L"Driver installed; Volume and DiskDrive UpperFilters registered.\n");
	ConOut(L"Reboot is required before both filter layers attach.\n");
	return TRUE;
}

static void PrintHelp(void)
{
	ConOut(L"\nCommands:\n");
	ConOut(L"  i  - install/register CdpDriver (Volume + DiskDrive UpperFilters)\n");
	ConOut(L"  1  - configure capture: source GUID + dedicated journal GUID\n");
	ConOut(L"  2  - stop capture for a source GUID (invalidate its journal)\n");
	ConOut(L"  6  - begin point-in-time preview (source GUID + time)\n");
	ConOutFmt(
		L"  7  - read preview data by volume offset and length (max %u bytes)\n",
		Cdp_CMD3_MAX_READ_BYTES);
	ConOut(L"  8  - end preview session\n");
	ConOut(L"  9  - query journal oldest/newest record time (source GUID)\n");
	ConOut(L"  u  - query journal record payload usage/free space (source GUID)\n");
	ConOut(L"  l  - list current journal record metadata (source GUID; no payload)\n");
	ConOut(L"  b  - print retained journal branch topology and inheritance points (source GUID)\n");
	ConOut(L"  s  - query protect status (source GUID -> status + journal GUID)\n");
	ConOut(L"  e  - enter prepared recovery (source GUID + time; no writeback)\n");
	ConOut(L"  r  - commit prepared recovery synchronously (writeback to source)\n");
	ConOut(L"  c  - cancel prepared recovery without writeback\n");
	ConOut(L"  p  - set / verify / change the shared protection password\n");
	ConOut(L"  v  - list volumes\n");
	ConOut(L"  d  - query driver version / build / journal version\n");
	ConOut(L"  t  - convert local time (year-month-day hour:minute:second) to WallClock100ns\n");
	ConOut(L"  h  - help\n");
	ConOut(L"  q  - quit console (does not stop CDP)\n");
	PrintMaxReadHint();
	ConOut(L"\n");
	if (g_PreviewHandle)
		ConOutFmt(L"Current PreviewHandle=%llu\n\n", g_PreviewHandle);
}

static wchar_t FirstCommandChar(const wchar_t* line)
{
	while (*line == L' ' || *line == L'\t')
		++line;
	return *line;
}

int wmain(void)
{
	HANDLE hDevice = INVALID_HANDLE_VALUE;
	wchar_t line[128];

	hDevice = OpenControlDevice();
	if (hDevice == INVALID_HANDLE_VALUE)
	{
		ConOut(L"Control device not available yet.\n");
		ConOut(L"Use command 'i' to install/register the driver first.\n");
	}
	else
	{
		ConOutFmt(L"Connected: %s\n", Cdp_CONTROL_SYSTEM_LINK_NAME);
	}
	PrintHelp();

	for (;;)
	{
		ConOut(L"> ");
		if (!ReadLine(line, _countof(line)))
			break;

		switch (FirstCommandChar(line))
		{
		case L'i':
		case L'I':
			DoInstallDriver();
			if (hDevice == INVALID_HANDLE_VALUE)
			{
				hDevice = OpenControlDevice();
				if (hDevice != INVALID_HANDLE_VALUE)
					ConOutFmt(L"Connected: %s\n", Cdp_CONTROL_SYSTEM_LINK_NAME);
			}
			break;
		case L'1':
			hDevice = EnsureControlDevice(hDevice);
			if (hDevice != INVALID_HANDLE_VALUE)
				DoCommand1(hDevice);
			break;
		case L'2':
			hDevice = EnsureControlDevice(hDevice);
			if (hDevice != INVALID_HANDLE_VALUE)
				DoCommand2(hDevice);
			break;
		case L'6':
			hDevice = EnsureControlDevice(hDevice);
			if (hDevice != INVALID_HANDLE_VALUE)
				DoPreviewBegin(hDevice);
			break;
		case L'7':
			hDevice = EnsureControlDevice(hDevice);
			if (hDevice != INVALID_HANDLE_VALUE)
				DoPreviewRead(hDevice);
			break;
		case L'8':
			hDevice = EnsureControlDevice(hDevice);
			if (hDevice != INVALID_HANDLE_VALUE)
				DoPreviewEnd(hDevice);
			break;
		case L'9':
			hDevice = EnsureControlDevice(hDevice);
			if (hDevice != INVALID_HANDLE_VALUE)
				DoQueryTimeRange(hDevice);
			break;
		case L'u':
		case L'U':
			hDevice = EnsureControlDevice(hDevice);
			if (hDevice != INVALID_HANDLE_VALUE)
				DoQueryJournalUsage(hDevice);
			break;
		case L'l':
		case L'L':
			hDevice = EnsureControlDevice(hDevice);
			if (hDevice != INVALID_HANDLE_VALUE)
				DoListJournalRecords(hDevice);
			break;
		case L'b':
		case L'B':
			hDevice = EnsureControlDevice(hDevice);
			if (hDevice != INVALID_HANDLE_VALUE)
				DoListJournalBranches(hDevice);
			break;
		case L's':
		case L'S':
			hDevice = EnsureControlDevice(hDevice);
			if (hDevice != INVALID_HANDLE_VALUE)
				DoQueryStatus(hDevice);
			break;
		case L'r':
		case L'R':
			hDevice = EnsureControlDevice(hDevice);
			if (hDevice != INVALID_HANDLE_VALUE)
				DoRecoveryCommit(hDevice);
			break;
		case L'e':
		case L'E':
			hDevice = EnsureControlDevice(hDevice);
			if (hDevice != INVALID_HANDLE_VALUE)
				DoRecoveryBegin(hDevice);
			break;
		case L'c':
		case L'C':
			hDevice = EnsureControlDevice(hDevice);
			if (hDevice != INVALID_HANDLE_VALUE)
				DoRecoveryCancel(hDevice);
			break;
		case L'p':
		case L'P':
			hDevice = EnsureControlDevice(hDevice);
			if (hDevice != INVALID_HANDLE_VALUE)
				DoPasswordManagement(hDevice);
			break;
		case L'v':
		case L'V':
			ListVolumes();
			break;
		case L'd':
		case L'D':
			hDevice = EnsureControlDevice(hDevice);
			if (hDevice != INVALID_HANDLE_VALUE)
				DoQueryVersion(hDevice);
			break;
		case L't':
		case L'T':
			DoConvertLocalTimeTo100ns();
			break;
		case L'h':
		case L'H':
		case L'?':
			PrintHelp();
			break;
		case L'q':
		case L'Q':
			goto done;
		case L'\0':
			break;
		default:
			ConOut(L"Unknown. Type h for help.\n");
			break;
		}
	}

done:
	if (hDevice != INVALID_HANDLE_VALUE)
	{
		// Only tear down console-local preview; leave CDP / volume handles alone.
		if (g_PreviewHandle != 0)
			DoPreviewEnd(hDevice);
		CloseHandle(hDevice);
	}
	ConOut(L"Bye.\n");
	return 0;
}
