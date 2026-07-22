#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

static void PrintUsage(void)
{
	wprintf(L"Usage:\n");
	wprintf(L"  VolHexdump <volume-guid>\n");
	wprintf(L"\n");
	wprintf(L"Accepted guid forms:\n");
	wprintf(L"  {xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx}\n");
	wprintf(L"  xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx\n");
	wprintf(L"  \\\\?\\Volume{xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx}\n");
	wprintf(L"\n");
	wprintf(L"Then enter: <offset> <size>\n");
	wprintf(L"  offset/size accept decimal or 0x-prefixed hex\n");
	wprintf(L"  q  quit\n");
}

static BOOL BuildVolumePath(
	_In_ PCWSTR Input,
	_Out_writes_(PathChars) PWSTR Path,
	_In_ size_t PathChars)
{
	const WCHAR* guidStart = Input;
	size_t len;

	if (!Input || !Input[0] || !Path || PathChars < 64)
		return FALSE;

	if (_wcsnicmp(Input, L"\\\\?\\Volume{", 11) == 0 ||
		_wcsnicmp(Input, L"\\\\.\\Volume{", 11) == 0)
	{
		guidStart = Input + 10; // points at '{'
	}
	else if (Input[0] != L'{')
	{
		if (swprintf_s(Path, PathChars, L"\\\\.\\Volume{%s}", Input) < 0)
			return FALSE;
		return TRUE;
	}

	len = wcslen(guidStart);
	if (len < 38 || guidStart[0] != L'{' || guidStart[len - 1] != L'}')
		return FALSE;

	if (swprintf_s(Path, PathChars, L"\\\\.\\Volume%s", guidStart) < 0)
		return FALSE;
	return TRUE;
}

static BOOL ParseU64(_In_ PCWSTR Text, _Out_ PUINT64 Value)
{
	WCHAR* end = NULL;
	UINT64 v;

	if (!Text || !Text[0] || !Value)
		return FALSE;

	if (Text[0] == L'0' && (Text[1] == L'x' || Text[1] == L'X'))
		v = wcstoull(Text + 2, &end, 16);
	else
		v = wcstoull(Text, &end, 0);

	if (!end || end == Text || *end != L'\0')
		return FALSE;

	*Value = v;
	return TRUE;
}

static void HexdumpC(
	_In_ UINT64 BaseOffset,
	_In_reads_bytes_(Length) const BYTE* Data,
	_In_ DWORD Length)
{
	DWORD i;

	for (i = 0; i < Length; i += 16)
	{
		DWORD j;
		DWORD lineLen = (Length - i > 16) ? 16 : (Length - i);

		wprintf(L"%08llx  ", (unsigned long long)(BaseOffset + i));

		for (j = 0; j < 16; ++j)
		{
			if (j == 8)
				wprintf(L" ");
			if (j < lineLen)
				wprintf(L"%02x ", Data[i + j]);
			else
				wprintf(L"   ");
		}

		wprintf(L" |");
		for (j = 0; j < lineLen; ++j)
		{
			BYTE c = Data[i + j];
			wprintf(L"%c", (c >= 0x20 && c <= 0x7e) ? (WCHAR)c : L'.');
		}
		wprintf(L"|\n");
	}
}

static BOOL QuerySectorSize(_In_ HANDLE Volume, _Out_ PDWORD SectorSize)
{
	DISK_GEOMETRY geometry;
	DWORD bytes = 0;

	*SectorSize = 512;
	if (!DeviceIoControl(
		Volume,
		IOCTL_DISK_GET_DRIVE_GEOMETRY,
		NULL,
		0,
		&geometry,
		sizeof(geometry),
		&bytes,
		NULL))
	{
		return FALSE;
	}

	if (geometry.BytesPerSector != 512 && geometry.BytesPerSector != 4096)
		return FALSE;

	*SectorSize = geometry.BytesPerSector;
	return TRUE;
}

static BOOL ReadVolumeRange(
	_In_ HANDLE Volume,
	_In_ DWORD SectorSize,
	_In_ UINT64 Offset,
	_In_ DWORD Length,
	_Out_writes_bytes_(Length) BYTE* OutBuffer,
	_Out_ PDWORD BytesRead)
{
	UINT64 alignedOffset;
	UINT64 alignedEnd;
	DWORD alignedLength;
	DWORD readBytes = 0;
	BYTE* alignedBuf = NULL;
	LARGE_INTEGER li;
	OVERLAPPED ov;
	BOOL ok;

	*BytesRead = 0;
	if (Length == 0)
		return TRUE;

	alignedOffset = (Offset / SectorSize) * SectorSize;
	alignedEnd = ((Offset + Length + SectorSize - 1) / SectorSize) * SectorSize;
	if (alignedEnd < alignedOffset || (alignedEnd - alignedOffset) > MAXDWORD)
	{
		SetLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	alignedLength = (DWORD)(alignedEnd - alignedOffset);

	alignedBuf = (BYTE*)_aligned_malloc(alignedLength, SectorSize);
	if (!alignedBuf)
	{
		SetLastError(ERROR_NOT_ENOUGH_MEMORY);
		return FALSE;
	}

	ZeroMemory(&ov, sizeof(ov));
	li.QuadPart = (LONGLONG)alignedOffset;
	ov.Offset = li.LowPart;
	ov.OffsetHigh = li.HighPart;

	ok = ReadFile(Volume, alignedBuf, alignedLength, &readBytes, &ov);
	if (!ok)
	{
		DWORD err = GetLastError();
		_aligned_free(alignedBuf);
		SetLastError(err);
		return FALSE;
	}
	if (readBytes < (DWORD)((Offset - alignedOffset) + Length))
	{
		_aligned_free(alignedBuf);
		SetLastError(ERROR_HANDLE_EOF);
		return FALSE;
	}

	CopyMemory(OutBuffer, alignedBuf + (Offset - alignedOffset), Length);
	_aligned_free(alignedBuf);
	*BytesRead = Length;
	return TRUE;
}

int wmain(int argc, wchar_t** argv)
{
	WCHAR path[128];
	HANDLE volume = INVALID_HANDLE_VALUE;
	DWORD sectorSize = 512;
	WCHAR line[256];

	if (argc != 2 ||
		wcscmp(argv[1], L"-h") == 0 ||
		wcscmp(argv[1], L"/?") == 0 ||
		wcscmp(argv[1], L"--help") == 0)
	{
		PrintUsage();
		return (argc == 2) ? 0 : 1;
	}

	if (!BuildVolumePath(argv[1], path, _countof(path)))
	{
		fwprintf(stderr, L"Invalid volume guid: %s\n", argv[1]);
		PrintUsage();
		return 1;
	}

	volume = CreateFileW(
		path,
		GENERIC_READ,
		FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
		NULL,
		OPEN_EXISTING,
		FILE_FLAG_NO_BUFFERING | FILE_FLAG_RANDOM_ACCESS,
		NULL);
	if (volume == INVALID_HANDLE_VALUE)
	{
		fwprintf(stderr, L"Open failed path=%s err=%lu\n", path, GetLastError());
		return 1;
	}

	if (!QuerySectorSize(volume, &sectorSize))
	{
		fwprintf(stderr,
			L"Query sector size failed err=%lu; defaulting to 512\n",
			GetLastError());
		sectorSize = 512;
	}

	wprintf(L"Opened read-only: %s\n", path);
	wprintf(L"SectorSize=%lu\n", sectorSize);
	wprintf(L"Enter '<offset> <size>' or 'q' to quit.\n");

	for (;;)
	{
		WCHAR offsetText[64];
		WCHAR sizeText[64];
		UINT64 offset = 0;
		UINT64 size64 = 0;
		DWORD size = 0;
		DWORD bytesRead = 0;
		BYTE* buffer = NULL;
		int n;

		wprintf(L"> ");
		fflush(stdout);
		if (!fgetws(line, _countof(line), stdin))
			break;

		{
			size_t lineLen = wcslen(line);
			while (lineLen > 0 &&
				(line[lineLen - 1] == L'\n' || line[lineLen - 1] == L'\r'))
			{
				line[--lineLen] = L'\0';
			}
		}

		if (line[0] == L'\0')
			continue;
		if (_wcsicmp(line, L"q") == 0 || _wcsicmp(line, L"quit") == 0)
			break;

		n = swscanf_s(line, L"%63s %63s", offsetText, (unsigned)_countof(offsetText),
			sizeText, (unsigned)_countof(sizeText));
		if (n != 2 ||
			!ParseU64(offsetText, &offset) ||
			!ParseU64(sizeText, &size64) ||
			size64 == 0 ||
			size64 > (16ULL * 1024ULL * 1024ULL))
		{
			wprintf(L"Invalid input. Example: 0 512   or   0x1000 0x100\n");
			wprintf(L"Size must be 1..16777216\n");
			continue;
		}
		size = (DWORD)size64;

		buffer = (BYTE*)malloc(size);
		if (!buffer)
		{
			wprintf(L"Out of memory\n");
			continue;
		}

		if (!ReadVolumeRange(volume, sectorSize, offset, size, buffer, &bytesRead))
		{
			wprintf(L"Read failed offset=%llu size=%lu err=%lu\n",
				(unsigned long long)offset,
				size,
				GetLastError());
			free(buffer);
			continue;
		}

		HexdumpC(offset, buffer, bytesRead);
		free(buffer);
	}

	CloseHandle(volume);
	return 0;
}
