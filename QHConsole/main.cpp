#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <Windows.h>
#include <objbase.h>
#include "..\SysRestoreDriver\QHIoctl.h"
#include "qh_driver_install.h"

#pragma comment(lib, "ole32.lib")

static UINT64 g_VolumeHandle = 0;
static UINT64 g_PreviewHandle = 0;

static void ConOut(const wchar_t* text)
{
	DWORD written = 0;
	HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
	if (!hOut || hOut == INVALID_HANDLE_VALUE)
		return;
	WriteConsoleW(hOut, text, (DWORD)wcslen(text), &written, NULL);
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
	if (!ReadConsoleW(hIn, buf, cch - 1, &n, NULL))
		return FALSE;
	while (n > 0 && (buf[n - 1] == L'\n' || buf[n - 1] == L'\r'))
		--n;
	buf[n] = L'\0';
	return TRUE;
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
		QH_CONTROL_SYSTEM_LINK_NAME,
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

static BOOL SendCmdBuffered(HANDLE hDevice, const void* req, DWORD reqSize)
{
	QH_COMMAND_REPLY reply = { 0 };
	DWORD bytesReturned = 0;

	if (!DeviceIoControl(hDevice, IOCTL_QH_SEND_COMMAND,
		(LPVOID)req, reqSize, &reply, sizeof(reply), &bytesReturned, NULL))
	{
		ConOutFmt(L"DeviceIoControl failed (err=%lu)\n", GetLastError());
		return FALSE;
	}

	ConOutFmt(L"Reply: Command=%lu Result=%lu Handle=%llu Message=%s\n",
		reply.Command, reply.Result, reply.VolumeHandle, reply.Message);

	if ((reply.Command == QH_CMD_1 || reply.Command == QH_CMD_4) &&
		reply.Result == 0)
		g_VolumeHandle = reply.VolumeHandle;

	if ((reply.Command == QH_CMD_2 || reply.Command == QH_CMD_5) &&
		reply.Result == 0)
		g_VolumeHandle = 0;

	return TRUE;
}

static BOOL DoCommand1(HANDLE hDevice)
{
	QH_CMD1_REQUEST req = { 0 };
	wchar_t line[16];
	req.Code = QH_CMD_1;
	ListVolumes();
	if (!PromptGuid(L"Protected source volume GUID: ", &req.PartitionGuid1))
		return FALSE;
	if (!PromptGuid(L"Dedicated journal partition GUID: ", &req.PartitionGuid2))
		return FALSE;
	ConOut(L"FORMAT the journal partition? This destroys its contents [y/N]: ");
	if (!ReadLine(line, _countof(line)))
		return FALSE;
	req.FormatJournal = (line[0] == L'y' || line[0] == L'Y') ? 1 : 0;
	return SendCmdBuffered(hDevice, &req, sizeof(req));
}

static BOOL DoCommand2(HANDLE hDevice)
{
	QH_CMD2_REQUEST req = { 0 };
	req.Code = QH_CMD_2;
	return SendCmdBuffered(hDevice, &req, sizeof(req));
}

static BOOL DoCommand4(HANDLE hDevice)
{
	QH_CMD4_REQUEST req = { 0 };
	req.Code = QH_CMD_4;
	ListVolumes();
	if (!PromptGuid(L"Volume GUID to open: ", &req.PartitionGuid))
		return FALSE;
	ConOut(L"Opening volume (CMD4)...\n");
	return SendCmdBuffered(hDevice, &req, sizeof(req));
}

static BOOL DoCommand5(HANDLE hDevice)
{
	QH_CMD5_REQUEST req = { 0 };
	wchar_t line[64];

	req.Code = QH_CMD_5;
	if (g_VolumeHandle != 0)
	{
		ConOutFmt(L"Current handle=%llu. Press Enter to close it, or type another id: ", g_VolumeHandle);
		if (!ReadLine(line, _countof(line)))
			return FALSE;
		if (line[0] == L'\0')
			req.VolumeHandle = g_VolumeHandle;
		else
			req.VolumeHandle = _wcstoui64(line, NULL, 0);
	}
	else
	{
		ConOut(L"VolumeHandle: ");
		if (!ReadLine(line, _countof(line)))
			return FALSE;
		req.VolumeHandle = _wcstoui64(line, NULL, 0);
	}

	ConOut(L"Closing volume (CMD5)...\n");
	return SendCmdBuffered(hDevice, &req, sizeof(req));
}

static void PrintMaxReadHint(void)
{
	ConOutFmt(
		L"  (CMD3 / Preview read: max %u bytes per request, 2 MiB)\n",
		QH_CMD3_MAX_READ_BYTES);
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

static BOOL DoCommand3(HANDLE hDevice)
{
	QH_CMD3_REQUEST req = { 0 };
	BYTE* buf = NULL;
	DWORD bytesReturned = 0;
	wchar_t line[64];
	ULONG i;
	ULONG dump;

	req.Code = QH_CMD_3;

	if (g_VolumeHandle == 0)
	{
		ConOut(L"No open handle. Run command 4 first.\n");
		return FALSE;
	}

	req.VolumeHandle = g_VolumeHandle;
	ConOutFmt(L"Using handle=%llu\n", req.VolumeHandle);

	ConOut(L"ByteOffset (e.g. 0): ");
	if (!ReadLine(line, _countof(line)))
		return FALSE;
	req.ByteOffset = _wcstoui64(line, NULL, 0);

	ConOutFmt(L"ByteLength (512-byte aligned, 1..%u): ", QH_CMD3_MAX_READ_BYTES);
	if (!ReadLine(line, _countof(line)))
		return FALSE;
	req.ByteLength = (ULONG)wcstoul(line, NULL, 0);

	if (req.ByteLength == 0 || req.ByteLength > QH_CMD3_MAX_READ_BYTES)
	{
		ConOutFmt(L"Length must be 1..%u\n", QH_CMD3_MAX_READ_BYTES);
		return FALSE;
	}
	if ((req.ByteOffset % QH_SECTOR_SIZE_DEFAULT) != 0 ||
		(req.ByteLength % QH_SECTOR_SIZE_DEFAULT) != 0)
	{
		ConOut(L"Offset/Length must be multiples of 512.\n");
		return FALSE;
	}

	buf = (BYTE*)VirtualAlloc(NULL, req.ByteLength, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
	if (!buf)
	{
		ConOut(L"VirtualAlloc failed.\n");
		return FALSE;
	}

	ConOutFmt(L"Reading handle=%llu offset=%llu len=%lu ...\n",
		req.VolumeHandle, req.ByteOffset, req.ByteLength);

	if (!DeviceIoControl(hDevice, IOCTL_QH_READ_SECTORS,
		&req, sizeof(req), buf, req.ByteLength, &bytesReturned, NULL))
	{
		ConOutFmt(L"Read failed (err=%lu)\n", GetLastError());
		VirtualFree(buf, 0, MEM_RELEASE);
		return FALSE;
	}

	ConOutFmt(L"Got %lu bytes. Hex (first 64):\n", bytesReturned);
	dump = min(bytesReturned, 64ul);
	for (i = 0; i < dump; ++i)
	{
		ConOutFmt(L"%02X ", buf[i]);
		if ((i + 1) % 16 == 0)
			ConOut(L"\n");
	}
	if (dump % 16)
		ConOut(L"\n");

	VirtualFree(buf, 0, MEM_RELEASE);
	return TRUE;
}

static BOOL DoPreviewBegin(HANDLE hDevice)
{
	QH_PREVIEW_BEGIN_REQUEST req = { 0 };
	QH_PREVIEW_BEGIN_REPLY reply = { 0 };
	DWORD bytesReturned = 0;
	wchar_t line[64];

	ListVolumes();
	if (!PromptGuid(L"Protected source volume GUID: ", &req.SourceVolumeGuid))
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

	if (!DeviceIoControl(hDevice, IOCTL_QH_BEGIN_PREVIEW,
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
	QH_PREVIEW_READ_REQUEST req = { 0 };
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
	ConOutFmt(L"Length (1..%u): ", QH_CMD3_MAX_READ_BYTES);
	if (!ReadLine(line, _countof(line)))
		return FALSE;
	req.ByteLength = (ULONG)wcstoul(line, NULL, 0);
	if (!req.ByteLength || req.ByteLength > QH_CMD3_MAX_READ_BYTES)
	{
		ConOutFmt(L"Length must be 1..%u\n", QH_CMD3_MAX_READ_BYTES);
		return FALSE;
	}

	buffer = (BYTE*)VirtualAlloc(
		NULL,
		req.ByteLength,
		MEM_COMMIT | MEM_RESERVE,
		PAGE_READWRITE);
	if (!buffer)
		return FALSE;

	if (!DeviceIoControl(hDevice, IOCTL_QH_READ_PREVIEW,
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
	QH_PREVIEW_END_REQUEST req = { 0 };
	DWORD bytesReturned = 0;

	if (!g_PreviewHandle)
	{
		ConOut(L"No preview session.\n");
		return FALSE;
	}
	req.PreviewHandle = g_PreviewHandle;
	if (!DeviceIoControl(hDevice, IOCTL_QH_END_PREVIEW,
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
	QH_RECOVERY_BEGIN_REQUEST req = { 0 };
	QH_RECOVERY_BEGIN_REPLY reply = { 0 };
	DWORD bytesReturned = 0;
	wchar_t line[64];

	ListVolumes();
	if (!PromptGuid(L"Protected source volume GUID: ", &req.SourceVolumeGuid))
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

	if (!DeviceIoControl(
			hDevice,
			IOCTL_QH_BEGIN_RECOVERY,
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

	ConOutFmt(
		L"Recovery prepared. Phase=%lu target=%llu range=[%llu, %llu]\n",
		reply.Phase,
		reply.TargetTime100ns,
		reply.OldestRecoverable100ns,
		reply.NewestRecoverable100ns);
	ConOut(L"No writeback has occurred. Bring the source online, then use r to commit.\n");
	return TRUE;
}

static BOOL DoRecoveryCommit(HANDLE hDevice)
{
	QH_RECOVERY_CONTROL_REQUEST req = { 0 };
	QH_RECOVERY_COMMIT_REPLY reply = { 0 };
	DWORD bytesReturned = 0;

	ListVolumes();
	if (!PromptGuid(L"Prepared source volume GUID: ", &req.SourceVolumeGuid))
		return FALSE;

	if (!DeviceIoControl(
			hDevice,
			IOCTL_QH_COMMIT_RECOVERY,
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
	QH_RECOVERY_CONTROL_REQUEST req = { 0 };
	DWORD bytesReturned = 0;

	ListVolumes();
	if (!PromptGuid(L"Prepared source volume GUID: ", &req.SourceVolumeGuid))
		return FALSE;

	if (!DeviceIoControl(
			hDevice,
			IOCTL_QH_CANCEL_RECOVERY,
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
	QH_PHASE_QUERY_REQUEST req = { 0 };
	QH_PHASE_QUERY_REPLY reply = { 0 };
	DWORD bytesReturned = 0;
	GUID zeroGuid = { 0 };

	ListVolumes();
	if (!PromptGuid(L"Source volume GUID: ", &req.SourceVolumeGuid))
		return FALSE;

	if (!DeviceIoControl(
			hDevice,
			IOCTL_QH_QUERY_PHASE,
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
	case QH_STATUS_UNPROTECTED:
		ConOut(L"(unprotected)\n");
		break;
	case (LONG)QH_PHASE_GENERAL:
		ConOut(L"(general)\n");
		break;
	case (LONG)QH_PHASE_PREVIEW:
		ConOut(L"(preview)\n");
		break;
	case (LONG)QH_PHASE_RECOVERY:
		ConOut(L"(recovery)\n");
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
	QH_TIME_RANGE_QUERY_REQUEST req = { 0 };
	QH_TIME_RANGE_QUERY_REPLY reply = { 0 };
	DWORD bytesReturned = 0;

	ListVolumes();
	if (!PromptGuid(L"Protected source volume GUID: ", &req.SourceVolumeGuid))
		return FALSE;

	if (!DeviceIoControl(
			hDevice,
			IOCTL_QH_QUERY_TIME_RANGE,
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

static BOOL DoInstallDriver(void)
{
	wchar_t infPath[MAX_PATH];

	if (QhIsDriverServiceInstalled())
		ConOut(L"SysRestoreDriver service already installed.\n");
	else
		ConOut(L"Installing SysRestoreDriver from INF...\n");

	if (!QhInstallDriverPackage())
	{
		if (!QhResolveDriverInfPath(infPath, _countof(infPath)))
		{
			ConOut(L"Install failed: SysRestoreDriver.inf/.sys not found.\n");
			ConOut(L"Expected locations (relative to QHConsole.exe):\n");
			ConOut(L"  .\\driver\\SysRestoreDriver.inf\n");
			ConOut(L"  .\\SysRestoreDriver.inf\n");
		}
		else
		{
			ConOutFmt(L"Install failed for: %s\n", infPath);
			ConOut(L"Run as Administrator and ensure test-signing is enabled.\n");
		}
		return FALSE;
	}

	if (QhResolveDriverInfPath(infPath, _countof(infPath)))
		ConOutFmt(L"Driver package: %s\n", infPath);
	ConOut(L"Driver installed and UpperFilters registered.\n");
	ConOut(L"Reboot may be required before the filter attaches to volumes.\n");
	return TRUE;
}

static void PrintHelp(void)
{
	ConOut(L"\nCommands:\n");
	ConOut(L"  i  - install/register SysRestoreDriver (INF + UpperFilters)\n");
	ConOut(L"  1  - configure capture: source GUID + dedicated journal GUID\n");
	ConOut(L"  2  - stop capture and close the journal\n");
	ConOut(L"  4  - CMD4: open volume by GUID -> handle\n");
	ConOutFmt(
		L"  3  - CMD3: read sectors by handle (need 4 first; max %u bytes)\n",
		QH_CMD3_MAX_READ_BYTES);
	ConOut(L"  5  - CMD5: close handle\n");
	ConOut(L"  6  - begin point-in-time preview (source GUID + time)\n");
	ConOutFmt(
		L"  7  - read preview data by volume offset and length (max %u bytes)\n",
		QH_CMD3_MAX_READ_BYTES);
	ConOut(L"  8  - end preview session\n");
	ConOut(L"  9  - query journal oldest/newest record time (source GUID)\n");
	ConOut(L"  s  - query protect status (source GUID -> status + journal GUID)\n");
	ConOut(L"  e  - enter prepared recovery (source GUID + time; no writeback)\n");
	ConOut(L"  r  - commit prepared recovery synchronously (writeback to source)\n");
	ConOut(L"  c  - cancel prepared recovery without writeback\n");
	ConOut(L"  v  - list volumes\n");
	ConOut(L"  h  - help\n");
	ConOut(L"  q  - quit console (does not stop CDP)\n");
	PrintMaxReadHint();
	ConOut(L"\n");
	if (g_VolumeHandle)
		ConOutFmt(L"Current VolumeHandle=%llu\n\n", g_VolumeHandle);
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
		ConOutFmt(L"Connected: %s\n", QH_CONTROL_SYSTEM_LINK_NAME);
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
					ConOutFmt(L"Connected: %s\n", QH_CONTROL_SYSTEM_LINK_NAME);
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
		case L'3':
			hDevice = EnsureControlDevice(hDevice);
			if (hDevice != INVALID_HANDLE_VALUE)
				DoCommand3(hDevice);
			break;
		case L'4':
			hDevice = EnsureControlDevice(hDevice);
			if (hDevice != INVALID_HANDLE_VALUE)
				DoCommand4(hDevice);
			break;
		case L'5':
			hDevice = EnsureControlDevice(hDevice);
			if (hDevice != INVALID_HANDLE_VALUE)
				DoCommand5(hDevice);
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
		case L'v':
		case L'V':
			ListVolumes();
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
