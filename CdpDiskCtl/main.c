#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winioctl.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include "CdpDiskFilterIoctl.h"

#define CDP_DISK_CLASS_KEY \
	L"SYSTEM\\CurrentControlSet\\Control\\Class\\" \
	L"{4d36e967-e325-11ce-bfc1-08002be10318}"

static HANDLE OpenControl(void)
{
	return CreateFileW(
		L"\\\\.\\CdpDiskFilter",
		GENERIC_READ | GENERIC_WRITE,
		0, NULL, OPEN_EXISTING, 0, NULL);
}

static BOOL QueryUpperFilterInstalled(void)
{
	HKEY key;
	WCHAR filters[4096];
	DWORD type = 0;
	DWORD bytes = sizeof(filters);
	const WCHAR* current;
	LONG result;

	result = RegOpenKeyExW(
		HKEY_LOCAL_MACHINE, CDP_DISK_CLASS_KEY, 0, KEY_QUERY_VALUE, &key);
	if (result != ERROR_SUCCESS)
		return FALSE;
	ZeroMemory(filters, sizeof(filters));
	result = RegQueryValueExW(
		key, L"UpperFilters", NULL, &type, (LPBYTE)filters, &bytes);
	RegCloseKey(key);
	if (result != ERROR_SUCCESS || type != REG_MULTI_SZ)
		return FALSE;
	for (current = filters; *current; current += wcslen(current) + 1)
	{
		if (_wcsicmp(current, L"CdpDiskFilter") == 0)
			return TRUE;
	}
	return FALSE;
}

static int InstallDriver(const WCHAR* explicitInfPath)
{
	WCHAR infPath[MAX_PATH];
	WCHAR systemDirectory[MAX_PATH];
	WCHAR pnputilPath[MAX_PATH];
	WCHAR commandLine[MAX_PATH * 3];
	WCHAR moduleDirectory[MAX_PATH];
	STARTUPINFOW startupInfo;
	PROCESS_INFORMATION processInfo;
	DWORD exitCode = ERROR_GEN_FAILURE;
	DWORD attributes;
	WCHAR* slash;

	if (explicitInfPath && *explicitInfPath)
	{
		if (!GetFullPathNameW(
				explicitInfPath, _countof(infPath), infPath, NULL))
		{
			fwprintf(stderr, L"Resolve INF path failed: %lu\n", GetLastError());
			return 2;
		}
	}
	else
	{
		if (!GetModuleFileNameW(
				NULL, moduleDirectory, _countof(moduleDirectory)))
			return 2;
		slash = wcsrchr(moduleDirectory, L'\\');
		if (!slash)
			return 2;
		*(slash + 1) = L'\0';
		if (wcscpy_s(infPath, _countof(infPath), moduleDirectory) != 0 ||
			wcscat_s(
				infPath, _countof(infPath),
				L"CdpDiskFilter\\CdpDiskFilter.inf") != 0)
			return 2;
		if (GetFileAttributesW(infPath) == INVALID_FILE_ATTRIBUTES)
		{
			if (wcscpy_s(
					infPath, _countof(infPath), moduleDirectory) != 0 ||
				wcscat_s(
					infPath, _countof(infPath), L"CdpDiskFilter.inf") != 0)
				return 2;
		}
	}
	attributes = GetFileAttributesW(infPath);
	if (attributes == INVALID_FILE_ATTRIBUTES ||
		(attributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
	{
		fwprintf(stderr, L"INF not found: %ls\n", infPath);
		return 2;
	}
	if (!GetSystemDirectoryW(systemDirectory, _countof(systemDirectory)) ||
		swprintf_s(
			pnputilPath, _countof(pnputilPath),
			L"%ls\\pnputil.exe", systemDirectory) < 0 ||
		swprintf_s(
			commandLine, _countof(commandLine),
			L"\"%ls\" /add-driver \"%ls\" /install",
			pnputilPath, infPath) < 0)
		return 2;
	ZeroMemory(&startupInfo, sizeof(startupInfo));
	ZeroMemory(&processInfo, sizeof(processInfo));
	startupInfo.cb = sizeof(startupInfo);
	wprintf(L"Installing: %ls\n", infPath);
	if (!CreateProcessW(
			pnputilPath, commandLine, NULL, NULL, TRUE, 0,
			NULL, NULL, &startupInfo, &processInfo))
	{
		fwprintf(stderr, L"Start pnputil failed: %lu\n", GetLastError());
		return 3;
	}
	WaitForSingleObject(processInfo.hProcess, INFINITE);
	(void)GetExitCodeProcess(processInfo.hProcess, &exitCode);
	CloseHandle(processInfo.hThread);
	CloseHandle(processInfo.hProcess);
	if (exitCode != 0)
	{
		fwprintf(stderr,
			L"pnputil failed: exit=%lu. Run this command as Administrator.\n",
			exitCode);
		return 4;
	}
	wprintf(
		L"pnputil succeeded. DiskDrive UpperFilters contains "
		L"CdpDiskFilter: %ls\n",
		QueryUpperFilterInstalled() ? L"YES" : L"NO");
	wprintf(L"Reboot Windows before configure/diagnose.\n");
	return 0;
}

static void PrintGuid(const GUID* guid)
{
	wprintf(
		L"{%08lX-%04hX-%04hX-%02hhX%02hhX-%02hhX%02hhX%02hhX%02hhX%02hhX%02hhX}",
		guid->Data1, guid->Data2, guid->Data3,
		guid->Data4[0], guid->Data4[1], guid->Data4[2], guid->Data4[3],
		guid->Data4[4], guid->Data4[5], guid->Data4[6], guid->Data4[7]);
}

static void PrintDiagnostics(
	HANDLE control,
	ULONG diskNumber,
	ULONG partitionStyle,
	ULONG mbrSignature,
	const GUID* diskGuid,
	ULONG sectorSize)
{
	CDP_DISK_FILTER_DIAGNOSTIC_REQUEST request;
	CDP_DISK_FILTER_DIAGNOSTIC_REPLY reply;
	DWORD bytes = 0;
	ULONG index;

	ZeroMemory(&request, sizeof(request));
	request.Version = CDP_DISK_FILTER_CONFIG_VERSION;
	request.DiskNumber = diskNumber;
	request.PartitionStyle = partitionStyle;
	request.MbrSignature = mbrSignature;
	request.DiskGuid = *diskGuid;
	request.SectorSize = sectorSize;
	ZeroMemory(&reply, sizeof(reply));
	if (!DeviceIoControl(
			control, (DWORD)IOCTL_CDP_DISK_FILTER_DIAGNOSE,
			&request, (DWORD)sizeof(request),
			&reply, (DWORD)sizeof(reply), &bytes, NULL))
	{
		fwprintf(stderr, L"Diagnostic IOCTL failed: %lu\n", GetLastError());
		return;
	}
	wprintf(
		L"DIAG request: disk=%lu style=%s sector=%lu mbr=0x%08lX guid=",
		diskNumber,
		partitionStyle == PARTITION_STYLE_GPT ? L"GPT" :
		partitionStyle == PARTITION_STYLE_MBR ? L"MBR" : L"UNKNOWN",
		sectorSize, mbrSignature);
	PrintGuid(diskGuid);
	wprintf(L"\nDIAG attached=%lu returned=%lu bytes=%lu\n",
		reply.AttachedInstanceCount, reply.ReturnedInstanceCount, bytes);
	for (index = 0; index < reply.ReturnedInstanceCount; ++index)
	{
		PCDP_DISK_FILTER_DIAGNOSTIC_ENTRY entry = &reply.Instances[index];
		wprintf(
			L"  instance[%lu]: deviceNumberStatus=0x%08lX deviceNumber=%lu "
			L"identityReadStatus=0x%08lX identityMatch=%lu\n"
			L"    lowerType=0x%08lX lowerStack=%lu filterStack=%lu "
			L"actualMbr=0x%08lX actualGpt=",
			entry->InstanceIndex,
			(ULONG)entry->DeviceNumberStatus,
			entry->DeviceNumber,
			(ULONG)entry->IdentityReadStatus,
			entry->IdentityMatches,
			entry->LowerDeviceType,
			entry->LowerStackSize,
			entry->FilterStackSize,
			entry->ActualMbrSignature);
		PrintGuid(&entry->ActualGptDiskGuid);
		wprintf(L"\n");
	}
}

static BOOL ReadDiskInfo(
	ULONG diskNumber,
	ULONG* partitionStyle,
	ULONG* mbrSignature,
	GUID* diskGuid,
	ULONG* sectorSize,
	ULONGLONG* diskLength,
	ULONG sourcePartitionNumber,
	ULONGLONG* sourceStart,
	ULONGLONG* sourceLength,
	ULONG journalPartitionNumber,
	ULONGLONG* journalStart,
	ULONGLONG* journalLength)
{
	WCHAR path[64];
	HANDLE disk;
	BYTE* buffer;
	DWORD bytes;
	BOOL ok;
	PDRIVE_LAYOUT_INFORMATION_EX layout;
	DISK_GEOMETRY geometry;
	GET_LENGTH_INFORMATION lengthInfo;
	ULONG index;
	BOOL sourceFound = sourceStart == NULL;
	BOOL journalFound = journalStart == NULL;

	(void)swprintf_s(path, _countof(path), L"\\\\.\\PhysicalDrive%lu", diskNumber);
	disk = CreateFileW(
		path, GENERIC_READ,
		FILE_SHARE_READ | FILE_SHARE_WRITE,
		NULL, OPEN_EXISTING, 0, NULL);
	if (disk == INVALID_HANDLE_VALUE)
		return FALSE;
	buffer = (BYTE*)calloc(1, 1024 * 1024);
	if (!buffer)
	{
		CloseHandle(disk);
		return FALSE;
	}
	ok = DeviceIoControl(
		disk, IOCTL_DISK_GET_DRIVE_LAYOUT_EX,
		NULL, 0, buffer, 1024 * 1024, &bytes, NULL);
	if (!ok)
	{
		CloseHandle(disk);
		free(buffer);
		return FALSE;
	}
	layout = (PDRIVE_LAYOUT_INFORMATION_EX)buffer;
	if (layout->PartitionStyle != PARTITION_STYLE_GPT &&
		layout->PartitionStyle != PARTITION_STYLE_MBR)
	{
		CloseHandle(disk);
		free(buffer);
		SetLastError(ERROR_NOT_SUPPORTED);
		return FALSE;
	}
	if (layout->PartitionStyle == PARTITION_STYLE_MBR &&
		layout->Mbr.Signature == 0)
	{
		CloseHandle(disk);
		free(buffer);
		SetLastError(ERROR_NOT_SUPPORTED);
		return FALSE;
	}
	if (!DeviceIoControl(
			disk, IOCTL_DISK_GET_DRIVE_GEOMETRY,
			NULL, 0, &geometry, (DWORD)sizeof(geometry), &bytes, NULL) ||
		!DeviceIoControl(
			disk, IOCTL_DISK_GET_LENGTH_INFO,
			NULL, 0, &lengthInfo, (DWORD)sizeof(lengthInfo), &bytes, NULL))
	{
		CloseHandle(disk);
		free(buffer);
		return FALSE;
	}
	if (partitionStyle)
		*partitionStyle = (ULONG)layout->PartitionStyle;
	if (mbrSignature)
		*mbrSignature = layout->PartitionStyle == PARTITION_STYLE_MBR ?
			layout->Mbr.Signature : 0;
	if (diskGuid)
	{
		ZeroMemory(diskGuid, sizeof(*diskGuid));
		if (layout->PartitionStyle == PARTITION_STYLE_GPT)
			*diskGuid = layout->Gpt.DiskId;
	}
	if (sectorSize)
		*sectorSize = geometry.BytesPerSector;
	if (diskLength)
		*diskLength = (ULONGLONG)lengthInfo.Length.QuadPart;
	for (index = 0; index < layout->PartitionCount; ++index)
	{
		PPARTITION_INFORMATION_EX part = &layout->PartitionEntry[index];
		if (!sourceFound &&
			part->PartitionNumber == sourcePartitionNumber &&
			part->PartitionLength.QuadPart > 0)
		{
			*sourceStart = (ULONGLONG)part->StartingOffset.QuadPart;
			*sourceLength = (ULONGLONG)part->PartitionLength.QuadPart;
			sourceFound = TRUE;
		}
		if (!journalFound &&
			part->PartitionNumber == journalPartitionNumber &&
			part->PartitionLength.QuadPart > 0)
		{
			*journalStart = (ULONGLONG)part->StartingOffset.QuadPart;
			*journalLength = (ULONGLONG)part->PartitionLength.QuadPart;
			journalFound = TRUE;
		}
	}
	free(buffer);
	CloseHandle(disk);
	if (!sourceFound || !journalFound)
		SetLastError(ERROR_NOT_FOUND);
	return sourceFound && journalFound;
}

static int Configure(ULONG disk, ULONG sourcePart, ULONG journalPart)
{
	CDP_DISK_FILTER_CONFIG request;
	HANDLE control;
	DWORD bytes;

	ZeroMemory(&request, sizeof(request));
	request.Version = CDP_DISK_FILTER_CONFIG_VERSION;
	request.DiskNumber = disk;
	if (!ReadDiskInfo(
			disk,
			&request.PartitionStyle,
			&request.MbrSignature,
			&request.DiskGuid,
			&request.SectorSize,
			&request.DiskLength,
			sourcePart, &request.SourceStart, &request.SourceLength,
			journalPart, &request.JournalStart, &request.JournalLength))
	{
		fwprintf(stderr, L"Cannot resolve source/journal partition. error=%lu\n",
			GetLastError());
		return 2;
	}
	control = OpenControl();
	if (control == INVALID_HANDLE_VALUE)
	{
		fwprintf(stderr, L"Open CdpDiskFilter failed: %lu\n", GetLastError());
		return 3;
	}
	if (!DeviceIoControl(
			control, (DWORD)IOCTL_CDP_DISK_FILTER_CONFIGURE,
			&request, (DWORD)sizeof(request), NULL, 0, &bytes, NULL))
	{
		fwprintf(stderr, L"Configure failed: %lu\n", GetLastError());
		PrintDiagnostics(
			control,
			request.DiskNumber,
			request.PartitionStyle,
			request.MbrSignature,
			&request.DiskGuid,
			request.SectorSize);
		CloseHandle(control);
		return 4;
	}
	CloseHandle(control);
	wprintf(L"Configured disk %lu: source partition %lu [%llu,%llu), journal partition %lu [%llu,%llu)\n",
		disk, sourcePart,
		request.SourceStart, request.SourceStart + request.SourceLength,
		journalPart,
		request.JournalStart, request.JournalStart + request.JournalLength);
	return 0;
}

static int Diagnose(ULONG disk)
{
	ULONG partitionStyle = 0;
	ULONG mbrSignature = 0;
	GUID diskGuid;
	ULONG sectorSize = 0;
	HANDLE control;

	ZeroMemory(&diskGuid, sizeof(diskGuid));
	if (!ReadDiskInfo(
			disk, &partitionStyle, &mbrSignature, &diskGuid, &sectorSize, NULL,
			0, NULL, NULL, 0, NULL, NULL))
	{
		fwprintf(stderr, L"Cannot read disk identity: %lu\n", GetLastError());
		return 2;
	}
	control = OpenControl();
	if (control == INVALID_HANDLE_VALUE)
	{
		fwprintf(stderr, L"Open CdpDiskFilter failed: %lu\n", GetLastError());
		return 3;
	}
	PrintDiagnostics(
		control, disk, partitionStyle, mbrSignature, &diskGuid, sectorSize);
	CloseHandle(control);
	return 0;
}

static int Disable(ULONG disk)
{
	CDP_DISK_FILTER_DISABLE request;
	HANDLE control = OpenControl();
	DWORD bytes;
	if (control == INVALID_HANDLE_VALUE)
		return 3;
	ZeroMemory(&request, sizeof(request));
	request.Version = CDP_DISK_FILTER_CONFIG_VERSION;
	request.DiskNumber = disk;
	if (!ReadDiskInfo(
			disk, &request.PartitionStyle, &request.MbrSignature,
			&request.DiskGuid, &request.SectorSize, NULL,
			0, NULL, NULL, 0, NULL, NULL))
	{
		fwprintf(stderr, L"Cannot read disk identity: %lu\n", GetLastError());
		CloseHandle(control);
		return 2;
	}
	if (!DeviceIoControl(
			control, (DWORD)IOCTL_CDP_DISK_FILTER_DISABLE,
			&request, (DWORD)sizeof(request), NULL, 0, &bytes, NULL))
	{
		fwprintf(stderr, L"Disable failed: %lu\n", GetLastError());
		CloseHandle(control);
		return 4;
	}
	CloseHandle(control);
	wprintf(L"Disabled disk %lu\n", disk);
	return 0;
}

static int Query(ULONG disk)
{
	CDP_DISK_FILTER_QUERY request;
	CDP_DISK_FILTER_STATUS reply;
	HANDLE control = OpenControl();
	DWORD bytes;
	if (control == INVALID_HANDLE_VALUE)
		return 3;
	ZeroMemory(&request, sizeof(request));
	request.Version = CDP_DISK_FILTER_CONFIG_VERSION;
	request.DiskNumber = disk;
	if (!ReadDiskInfo(
			disk, &request.PartitionStyle, &request.MbrSignature,
			&request.DiskGuid, &request.SectorSize, NULL,
			0, NULL, NULL, 0, NULL, NULL))
	{
		fwprintf(stderr, L"Cannot read disk identity: %lu\n", GetLastError());
		CloseHandle(control);
		return 2;
	}
	ZeroMemory(&reply, sizeof(reply));
	if (!DeviceIoControl(
			control, (DWORD)IOCTL_CDP_DISK_FILTER_QUERY,
			&request, (DWORD)sizeof(request),
			&reply, (DWORD)sizeof(reply), &bytes, NULL))
	{
		fwprintf(stderr, L"Query failed: %lu\n", GetLastError());
		CloseHandle(control);
		return 4;
	}
	CloseHandle(control);
	wprintf(L"disk=%lu enabled=%lu sector=%lu diskBytes=%llu\nsource=[%llu,%llu) journal=[%llu,%llu) used=%llu items=%llu\n",
		reply.DiskNumber, reply.Enabled, reply.SectorSize, reply.DiskLength,
		reply.SourceStart, reply.SourceStart + reply.SourceLength,
		reply.JournalStart, reply.JournalStart + reply.JournalLength,
		reply.JournalBytesUsed, reply.MappingItemCount);
	return 0;
}

static int Records(ULONG disk)
{
	CDP_DISK_FILTER_RECORDS_REQUEST request;
	CDP_DISK_FILTER_RECORDS_REPLY reply;
	HANDLE control = OpenControl();
	DWORD bytes;
	ULONGLONG nextIndex = 0;
	ULONGLONG totalCount = 0;
	BOOL firstPage = TRUE;

	if (control == INVALID_HANDLE_VALUE)
	{
		fwprintf(stderr, L"Open CdpDiskFilter failed: %lu\n", GetLastError());
		return 3;
	}
	for (;;)
	{
		ULONG index;
		ZeroMemory(&request, sizeof(request));
		request.Version = CDP_DISK_FILTER_CONFIG_VERSION;
		request.DiskNumber = disk;
		request.StartIndex = nextIndex;
		ZeroMemory(&reply, sizeof(reply));
		if (!DeviceIoControl(
				control, (DWORD)IOCTL_CDP_DISK_FILTER_RECORDS,
				&request, (DWORD)sizeof(request),
				&reply, (DWORD)sizeof(reply), &bytes, NULL))
		{
			fwprintf(stderr,
				L"Read records failed: %lu (protection may be disabled)\n",
				GetLastError());
			CloseHandle(control);
			return 4;
		}
		if (firstPage)
		{
			totalCount = reply.TotalCount;
			wprintf(
				L"Journal record list: %llu record(s), oldest first\n",
				totalCount);
			wprintf(
				L"Index  WallClock100ns       VolumeOffset         JournalOffset        Length    Sequence  Flags\n");
			firstPage = FALSE;
		}
		for (index = 0; index < reply.ReturnedCount; ++index)
		{
			PCDP_DISK_FILTER_RECORD_ENTRY record = &reply.Records[index];
			wprintf(
				L"%-6llu %-20llu 0x%016llX 0x%016llX %-9lu %-9llu 0x%08lX\n",
				record->Index,
				record->WallClock100ns,
				record->VolumeOffset,
				record->JournalOffset,
				record->DataLength,
				record->Sequence,
				record->Flags);
		}
		if (reply.ReturnedCount == 0)
			break;
		nextIndex = reply.Records[reply.ReturnedCount - 1].Index + 1;
		if (nextIndex >= totalCount)
			break;
	}
	CloseHandle(control);
	return 0;
}

static void PrintUsage(void)
{
	wprintf(
		L"Commands:\n"
		L"  install [CdpDiskFilter.inf path]\n"
		L"  configure <disk> <source-partition> <journal-partition>\n"
		L"  disable <disk>\n"
		L"  records <disk>\n"
		L"  query <disk>\n"
		L"  diagnose <disk>\n"
		L"  help\n"
		L"  exit\n");
}

static int ExecuteCommand(int argc, wchar_t** argv, BOOL interactive)
{
	if (argc == 1 && _wcsicmp(argv[0], L"install") == 0)
		return InstallDriver(NULL);
	if (argc == 2 && _wcsicmp(argv[0], L"install") == 0)
		return InstallDriver(argv[1]);
	if (argc == 4 && _wcsicmp(argv[0], L"configure") == 0)
		return Configure(
			wcstoul(argv[1], NULL, 0),
			wcstoul(argv[2], NULL, 0),
			wcstoul(argv[3], NULL, 0));
	if (argc == 2 && _wcsicmp(argv[0], L"disable") == 0)
		return Disable(wcstoul(argv[1], NULL, 0));
	if (argc == 2 && _wcsicmp(argv[0], L"records") == 0)
		return Records(wcstoul(argv[1], NULL, 0));
	if (argc == 2 && _wcsicmp(argv[0], L"query") == 0)
		return Query(wcstoul(argv[1], NULL, 0));
	if (argc == 2 && _wcsicmp(argv[0], L"diagnose") == 0)
		return Diagnose(wcstoul(argv[1], NULL, 0));
	if (argc == 1 && (_wcsicmp(argv[0], L"help") == 0 ||
		_wcsicmp(argv[0], L"?") == 0))
	{
		PrintUsage();
		return 0;
	}
	if (interactive && argc == 1 &&
		(_wcsicmp(argv[0], L"exit") == 0 ||
		 _wcsicmp(argv[0], L"quit") == 0))
		return -1;
	fwprintf(stderr, L"Invalid command. Type 'help'.\n");
	return 1;
}

int wmain(int argc, wchar_t** argv)
{
	if (argc > 1)
		return ExecuteCommand(argc - 1, argv + 1, FALSE);
	PrintUsage();
	for (;;)
	{
		WCHAR line[2048];
		LPWSTR* commandArgs;
		int commandArgc;
		int result;
		wprintf(L"cdpdisk> ");
		fflush(stdout);
		if (!fgetws(line, _countof(line), stdin))
			break;
		if (line[0] == L'\r' || line[0] == L'\n' || line[0] == L'\0')
			continue;
		commandArgs = CommandLineToArgvW(line, &commandArgc);
		if (!commandArgs)
		{
			fwprintf(stderr, L"Parse command failed: %lu\n", GetLastError());
			continue;
		}
		result = ExecuteCommand(commandArgc, commandArgs, TRUE);
		LocalFree(commandArgs);
		if (result == -1)
			break;
	}
	return 0;
}
