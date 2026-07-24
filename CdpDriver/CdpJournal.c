#ifdef Cdp_USERMODE
#include "cdp_portable.h"
#include "cdp_store.h"
#include "CdpJournal.h"
#else
#include "CdpEngineDefs.h"
#include "CdpJournal.h"
#endif

#define Cdp_CRC32C_POLY 0x82F63B78UL
#ifndef Cdp_USERMODE
#if DBG
#define Cdp_JOURNAL_DIAG(fmt, ...) \
	Cdp_LOG("[JOURNAL-APPLY] " fmt, ##__VA_ARGS__)
#else
#define Cdp_JOURNAL_DIAG(fmt, ...) ((void)0)
#endif
#else
#define Cdp_JOURNAL_DIAG(fmt, ...) ((void)0)
#endif

static ULONG g_CdpCrc32cTable[256];
static volatile LONG g_CdpCrc32cReady;

static VOID CdpStallBrief(VOID)
{
#ifdef Cdp_USERMODE
	SwitchToThread();
#else
	KeStallExecutionProcessor(1);
#endif
}

static UINT64 CdpAlignDown64(_In_ UINT64 Value, _In_ ULONG Alignment)
{
	return Value - (Value % Alignment);
}

static UINT64 CdpAlignUp64(_In_ UINT64 Value, _In_ ULONG Alignment)
{
	UINT64 remainder = Value % Alignment;
	return remainder ? Value + (Alignment - remainder) : Value;
}

static VOID CdpInitializeCrc32c(VOID)
{
	ULONG table[256];
	ULONG i;

	if (InterlockedCompareExchange(&g_CdpCrc32cReady, 1, 0) != 0)
	{
		while (InterlockedCompareExchange(&g_CdpCrc32cReady, 0, 0) != 2)
			CdpStallBrief();
		return;
	}

	for (i = 0; i < RTL_NUMBER_OF(table); ++i)
	{
		ULONG crc = i;
		ULONG bit;
		for (bit = 0; bit < 8; ++bit)
			crc = (crc & 1) ? ((crc >> 1) ^ Cdp_CRC32C_POLY) : (crc >> 1);
		table[i] = crc;
	}
	RtlCopyMemory(g_CdpCrc32cTable, table, sizeof(table));
	InterlockedExchange(&g_CdpCrc32cReady, 2);
}

static ULONG CdpCrc32c(
	_In_ ULONG InitialCrc,
	_In_reads_bytes_(Length) const VOID* Buffer,
	_In_ SIZE_T Length)
{
	const UCHAR* bytes = (const UCHAR*)Buffer;
	ULONG crc = InitialCrc ^ 0xFFFFFFFFUL;

	if (InterlockedCompareExchange(&g_CdpCrc32cReady, 0, 0) != 2)
		CdpInitializeCrc32c();

	while (Length--)
		crc = g_CdpCrc32cTable[(crc ^ *bytes++) & 0xFF] ^ (crc >> 8);
	return crc ^ 0xFFFFFFFFUL;
}

static PVOID CdpAllocateAligned(
	_In_ PCdp_JOURNAL Journal,
	_In_ SIZE_T Length,
	_Out_ PVOID* AllocationBase)
{
	SIZE_T alignment = sizeof(PVOID);
	ULONG_PTR address;

	if (Journal->SectorSize > alignment)
		alignment = Journal->SectorSize;
#ifndef Cdp_USERMODE
	if (Journal->TargetDevice)
	{
		SIZE_T deviceAlign =
			(SIZE_T)Journal->TargetDevice->AlignmentRequirement + 1;
		if (deviceAlign > alignment)
			alignment = deviceAlign;
	}
#endif
	*AllocationBase = cdpalloc(Length + alignment - 1);
	if (!*AllocationBase)
		return NULL;
	address = ((ULONG_PTR)*AllocationBase + alignment - 1) & ~(alignment - 1);
	return (PVOID)address;
}

static UINT64 CdpJournalUsableStart(_In_ PCdp_JOURNAL Journal)
{
	return Journal->SectorSize;
}

static UINT64 CdpJournalUsableEnd(_In_ PCdp_JOURNAL Journal)
{
	return Journal->PartitionSize;
}

static NTSTATUS CdpJournalRawIo(
	_In_ PCdp_JOURNAL Journal,
	_In_ UCHAR MajorFunction,
	_In_ UINT64 Offset,
	_In_ ULONG Length,
	_Inout_updates_bytes_(Length) PVOID Buffer)
{
	NTSTATUS status;

	if (!Buffer || Length == 0 ||
		(Offset % Journal->SectorSize) != 0 ||
		(Length % Journal->SectorSize) != 0 ||
		Offset > Journal->PartitionSize ||
		Length > Journal->PartitionSize - Offset)
	{
		return STATUS_INVALID_PARAMETER;
	}

	if (Journal->Store)
	{
		if (MajorFunction == IRP_MJ_READ)
			return Journal->Store->Read(Journal->Store, Offset, Length, Buffer);
		if (MajorFunction == IRP_MJ_WRITE)
			return Journal->Store->Write(
				Journal->Store,
				Offset,
				Length,
				Buffer);
		return STATUS_NOT_IMPLEMENTED;
	}

#ifndef Cdp_USERMODE
	// Prefer the volume stack below our own filter.  This keeps offsets volume-
	// relative and avoids blocking on a synchronous \\PhysicalDrive handle.
	// Retain the physical-disk backend only as a compatibility fallback when no
	// lower volume device was supplied.
	if (Journal->RawDiskHandle && !Journal->TargetDevice)
	{
		IO_STATUS_BLOCK iosb;
		LARGE_INTEGER byteOffset;

		if (MajorFunction != IRP_MJ_READ &&
			MajorFunction != IRP_MJ_WRITE)
			return STATUS_NOT_IMPLEMENTED;
		if (Journal->TargetBaseOffset >
			(UINT64)MAXLONGLONG - Offset)
			return STATUS_INTEGER_OVERFLOW;

		byteOffset.QuadPart =
			(LONGLONG)(Journal->TargetBaseOffset + Offset);
		RtlZeroMemory(&iosb, sizeof(iosb));
		Cdp_DBG("[JOURNAL-PHYSICAL] io begin handle=%p "
			"major=0x%02X partitionOffset=%llu diskOffset=%llu len=%lu\n",
			Journal->RawDiskHandle,
			MajorFunction,
			Offset,
			(UINT64)byteOffset.QuadPart,
			Length);
		if (MajorFunction == IRP_MJ_READ)
		{
			status = ZwReadFile(
				(HANDLE)Journal->RawDiskHandle,
				NULL, NULL, NULL, &iosb,
				Buffer, Length, &byteOffset, NULL);
		}
		else
		{
			status = ZwWriteFile(
				(HANDLE)Journal->RawDiskHandle,
				NULL, NULL, NULL, &iosb,
				Buffer, Length, &byteOffset, NULL);
		}
		if (NT_SUCCESS(status))
			status = iosb.Status;
		Cdp_DBG("[JOURNAL-PHYSICAL] io end "
			"status=0x%08X bytes=%Iu\n",
			status,
			iosb.Information);
		if (NT_SUCCESS(status) && iosb.Information != Length)
			return STATUS_UNEXPECTED_IO_ERROR;
		return status;
	}

	{
		KEVENT event;
		IO_STATUS_BLOCK iosb;
		LARGE_INTEGER byteOffset;
		PIRP irp;

		if (!Journal->TargetDevice)
			return STATUS_DEVICE_NOT_READY;

	byteOffset.QuadPart = (LONGLONG)Offset;
	KeInitializeEvent(&event, NotificationEvent, FALSE);
	RtlZeroMemory(&iosb, sizeof(iosb));
	irp = IoBuildSynchronousFsdRequest(
		MajorFunction,
		Journal->TargetDevice,
		Buffer,
		Length,
		&byteOffset,
		&event,
		&iosb);
	if (!irp)
		return STATUS_INSUFFICIENT_RESOURCES;

	Cdp_DBG("[JOURNAL-RAW] io begin target=%p major=0x%02X "
		"offset=%llu len=%lu irp=%p\n",
		Journal->TargetDevice,
		MajorFunction,
		Offset,
		Length,
		irp);
	status = IoCallDriver(Journal->TargetDevice, irp);
	Cdp_DBG("[JOURNAL-RAW] IoCallDriver returned irp=%p "
		"status=0x%08X iosb=0x%08X bytes=%Iu\n",
		irp,
		status,
		iosb.Status,
		iosb.Information);
	if (status == STATUS_PENDING)
	{
		Cdp_DBG("[JOURNAL-RAW] wait begin irp=%p\n",
			irp);
		KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, NULL);
		status = iosb.Status;
		Cdp_DBG("[JOURNAL-RAW] wait end irp=%p "
			"status=0x%08X bytes=%Iu\n",
			irp,
			status,
			iosb.Information);
	}
	else if (NT_SUCCESS(status))
	{
		status = iosb.Status;
	}

	if (NT_SUCCESS(status) && iosb.Information != Length)
		return STATUS_UNEXPECTED_IO_ERROR;
	Cdp_DBG("[JOURNAL-RAW] io end irp=%p status=0x%08X "
		"bytes=%Iu\n",
		irp,
		status,
		iosb.Information);
		return status;
	}
#else
	UNREFERENCED_PARAMETER(status);
	return STATUS_DEVICE_NOT_READY;
#endif
}

static NTSTATUS CdpJournalRawWriteSub(
	_In_ PCdp_JOURNAL Journal,
	_In_ UINT64 Offset,
	_In_ ULONG Length,
	_In_reads_bytes_(Length) const VOID* Data)
{
	ULONG sec = Journal->SectorSize;
	UINT64 start = (Offset / sec) * sec;
	UINT64 endB = CdpAlignUp64(Offset + Length, sec);
	ULONG span = (ULONG)(endB - start);
	PVOID allocationBase = NULL;
	PUCHAR buf;
	NTSTATUS status;

	if ((Offset % sec) == 0 && (Length % sec) == 0)
		return CdpJournalRawIo(
			Journal,
			IRP_MJ_WRITE,
			Offset,
			Length,
			(PVOID)Data);

	buf = (PUCHAR)CdpAllocateAligned(Journal, span, &allocationBase);
	if (!buf)
		return STATUS_INSUFFICIENT_RESOURCES;

	status = CdpJournalRawIo(Journal, IRP_MJ_READ, start, span, buf);
	if (NT_SUCCESS(status))
	{
		RtlCopyMemory(buf + (Offset - start), Data, Length);
		status = CdpJournalRawIo(Journal, IRP_MJ_WRITE, start, span, buf);
	}
	cdpfree(allocationBase);
	return status;
}

static NTSTATUS CdpJournalFlush(_In_ PCdp_JOURNAL Journal)
{
	if (Journal->Store)
		return STATUS_SUCCESS;

#ifndef Cdp_USERMODE
{
	KEVENT event;
	IO_STATUS_BLOCK iosb;
	PIRP irp;
	NTSTATUS status;

	if (!Journal->TargetDevice)
		return STATUS_DEVICE_NOT_READY;
	KeInitializeEvent(&event, NotificationEvent, FALSE);
	RtlZeroMemory(&iosb, sizeof(iosb));
	irp = IoBuildSynchronousFsdRequest(
		IRP_MJ_FLUSH_BUFFERS,
		Journal->TargetDevice,
		NULL,
		0,
		NULL,
		&event,
		&iosb);
	if (!irp)
		return STATUS_INSUFFICIENT_RESOURCES;
	status = IoCallDriver(Journal->TargetDevice, irp);
	if (status == STATUS_PENDING)
	{
		KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, NULL);
		status = iosb.Status;
	}
	else if (NT_SUCCESS(status))
	{
		status = iosb.Status;
	}
	return status;
	}
#else
	UNREFERENCED_PARAMETER(Journal);
	return STATUS_SUCCESS;
#endif
}

static BOOLEAN CdpJournalIsEmptyLocked(_In_ PCdp_JOURNAL Journal)
{
	return Journal->TotalRecords == 0;
}

static VOID CdpJournalAdvanceRecordGenerationLocked(
	_Inout_ PCdp_JOURNAL Journal)
{
	Journal->RecordGeneration++;
	if (Journal->RecordGeneration == 0)
		Journal->RecordGeneration = 1;
}

static NTSTATUS CdpJournalWriteSuperblockLocked(_Inout_ PCdp_JOURNAL Journal)
{
	PVOID allocationBase = NULL;
	PUCHAR sector;
	PCdp_JOURNAL_SUPERBLOCK superblock;
	NTSTATUS status;

	sector = (PUCHAR)CdpAllocateAligned(Journal,
		Journal->SectorSize,
		&allocationBase);
	if (!sector)
		return STATUS_INSUFFICIENT_RESOURCES;

	RtlZeroMemory(sector, Journal->SectorSize);
	superblock = (PCdp_JOURNAL_SUPERBLOCK)sector;
	superblock->Magic = Cdp_JOURNAL_MAGIC;
	superblock->Version = Cdp_JOURNAL_VERSION;
	superblock->SectorSize = Journal->SectorSize;
	superblock->PartitionSize = Journal->PartitionSize;
	superblock->LastHeaderRegionOff = Journal->LastHeaderRegionOff;
	superblock->SourceVolumeGuid = Journal->SourceVolumeGuid;
	superblock->Crc32c = CdpCrc32c(
		0,
		superblock,
		FIELD_OFFSET(Cdp_JOURNAL_SUPERBLOCK, Crc32c));

	status = CdpJournalRawIo(
		Journal,
		IRP_MJ_WRITE,
		0,
		Journal->SectorSize,
		sector);
	cdpfree(allocationBase);
	return status;
}

static BOOLEAN CdpJournalSuperblockValid(
	_In_ PCdp_JOURNAL Journal,
	_In_ const Cdp_JOURNAL_SUPERBLOCK* Superblock)
{
	ULONG crc;
	UINT64 usableStart;
	UINT64 usableEnd;

	if (Superblock->Magic != Cdp_JOURNAL_MAGIC ||
		Superblock->Version != Cdp_JOURNAL_VERSION ||
		Superblock->SectorSize != Journal->SectorSize ||
		Superblock->PartitionSize != Journal->PartitionSize)
	{
		return FALSE;
	}
	crc = CdpCrc32c(
		0,
		Superblock,
		FIELD_OFFSET(Cdp_JOURNAL_SUPERBLOCK, Crc32c));
	if (crc != Superblock->Crc32c)
		return FALSE;

	usableStart = Journal->SectorSize;
	usableEnd = Journal->PartitionSize;
	if (Superblock->LastHeaderRegionOff < usableStart ||
		Superblock->LastHeaderRegionOff + Cdp_JOURNAL_HEADER_REGION_SIZE > usableEnd ||
		(Superblock->LastHeaderRegionOff % Journal->SectorSize) != 0)
	{
		return FALSE;
	}
	return TRUE;
}

static NTSTATUS CdpJournalReadHeaderRegionSlice(
	_Inout_ PCdp_JOURNAL Journal,
	_In_ UINT64 RegionOff,
	_In_ ULONG RelativeOffset,
	_In_ ULONG Length,
	_Out_writes_bytes_(Length) PVOID Data)
{
	NTSTATUS status;
	ULONG chunk = Journal->HeaderRegionWriteChunk;
	PVOID allocationBase = NULL;
	PUCHAR buffer = NULL;

	if (!Data || Length == 0 ||
		RelativeOffset >= Cdp_JOURNAL_HEADER_REGION_SIZE ||
		Length > Cdp_JOURNAL_HEADER_REGION_SIZE - RelativeOffset)
	{
		return STATUS_INVALID_PARAMETER;
	}

	if (chunk == 0 || chunk > Cdp_JOURNAL_HEADER_REGION_SIZE)
		chunk = Cdp_JOURNAL_HEADER_REGION_SIZE;
	chunk = (chunk / Journal->SectorSize) * Journal->SectorSize;
	if (chunk < Journal->SectorSize)
		chunk = Journal->SectorSize;

	for (;;)
	{
		ULONG chunkOffset = (RelativeOffset / chunk) * chunk;

		buffer = (PUCHAR)CdpAllocateAligned(Journal, chunk, &allocationBase);
		if (!buffer)
			return STATUS_INSUFFICIENT_RESOURCES;

		status = CdpJournalRawIo(
			Journal,
			IRP_MJ_READ,
			RegionOff + chunkOffset,
			chunk,
			buffer);
		if (NT_SUCCESS(status))
		{
			Journal->HeaderRegionWriteChunk = chunk;
			RtlCopyMemory(
				Data,
				buffer + (RelativeOffset - chunkOffset),
				Length);
			cdpfree(allocationBase);
			return STATUS_SUCCESS;
		}

		cdpfree(allocationBase);
		allocationBase = NULL;
		buffer = NULL;
		if (chunk <= Journal->SectorSize)
			return status;
		chunk /= 2;
		chunk = (chunk / Journal->SectorSize) * Journal->SectorSize;
		if (chunk < Journal->SectorSize)
			chunk = Journal->SectorSize;
	}
}

static NTSTATUS CdpJournalReadRegionLink(
	_In_ PCdp_JOURNAL Journal,
	_In_ UINT64 RegionOff,
	_Out_ PCdp_HEADER_REGION_LINK Link)
{
	return CdpJournalReadHeaderRegionSlice(
		Journal,
		RegionOff,
		Cdp_JOURNAL_HEADER_REGION_SIZE - Cdp_JOURNAL_HEADER_LINK_SIZE,
		sizeof(*Link),
		Link);
}

static NTSTATUS CdpJournalWriteRegionLink(
	_In_ PCdp_JOURNAL Journal,
	_In_ UINT64 RegionOff,
	_In_ const Cdp_HEADER_REGION_LINK* Link)
{
	return CdpJournalRawWriteSub(
		Journal,
		RegionOff + Cdp_JOURNAL_HEADER_REGION_SIZE - Cdp_JOURNAL_HEADER_LINK_SIZE,
		sizeof(*Link),
		Link);
}

static NTSTATUS CdpJournalInitHeaderRegion(
	_In_ PCdp_JOURNAL Journal,
	_In_ UINT64 RegionOff,
	_In_ UINT64 PrevOff,
	_In_ UINT64 NextOff)
{
	PVOID allocationBase = NULL;
	PUCHAR region;
	PCdp_HEADER_REGION_LINK link;
	NTSTATUS status;
	ULONG offset;
	ULONG chunk;

	region = (PUCHAR)CdpAllocateAligned(Journal,
		Cdp_JOURNAL_HEADER_REGION_SIZE,
		&allocationBase);
	if (!region)
		return STATUS_INSUFFICIENT_RESOURCES;

	RtlZeroMemory(region, Cdp_JOURNAL_HEADER_REGION_SIZE);
	link = (PCdp_HEADER_REGION_LINK)(
		region + Cdp_JOURNAL_HEADER_REGION_SIZE - Cdp_JOURNAL_HEADER_LINK_SIZE);
	link->Marker = Cdp_JOURNAL_HEADER_LINK_MARK;
	link->PrevRegionOff = PrevOff;
	link->NextRegionOff = NextOff;
	link->Reserved = 0;

	chunk = Journal->HeaderRegionWriteChunk;
	if (chunk == 0 || chunk > Cdp_JOURNAL_HEADER_REGION_SIZE)
		chunk = Cdp_JOURNAL_HEADER_REGION_SIZE;
	chunk = (chunk / Journal->SectorSize) * Journal->SectorSize;
	if (chunk < Journal->SectorSize)
		chunk = Journal->SectorSize;

	for (offset = 0; offset < Cdp_JOURNAL_HEADER_REGION_SIZE;)
	{
		ULONG remaining = Cdp_JOURNAL_HEADER_REGION_SIZE - offset;
		ULONG transfer = chunk < remaining ? chunk : remaining;

		status = CdpJournalRawIo(
			Journal,
			IRP_MJ_WRITE,
			RegionOff + offset,
			transfer,
			region + offset);
		if (!NT_SUCCESS(status))
		{
			if (chunk <= Journal->SectorSize)
			{
				cdpfree(allocationBase);
				return status;
			}

			chunk /= 2;
			chunk = (chunk / Journal->SectorSize) * Journal->SectorSize;
			if (chunk < Journal->SectorSize)
				chunk = Journal->SectorSize;
			continue;
		}

		// Cache the largest transfer that succeeded.  Later header regions
		// start directly with this size instead of probing from 2MB again.
		Journal->HeaderRegionWriteChunk = chunk;
		offset += transfer;
	}
#ifndef Cdp_USERMODE
	Cdp_DBG("[JOURNAL] HeaderRegionWriteChunk=%lu bytes\n",
		Journal->HeaderRegionWriteChunk);
#endif
	cdpfree(allocationBase);
	return STATUS_SUCCESS;
}

static ULONG CdpJournalRegionHeaderLimit(
	_In_ PCdp_JOURNAL Journal,
	_In_ UINT64 RegionOff)
{
	if (RegionOff == Journal->LastHeaderRegionOff)
		return Journal->CurrentHeaderCount;
	return Cdp_JOURNAL_HEADERS_PER_REGION;
}

static NTSTATUS CdpJournalReadHeaderAt(
	_In_ PCdp_JOURNAL Journal,
	_In_ UINT64 RegionOff,
	_In_ ULONG Index,
	_Out_ PCdp_JOURNAL_RECORD_HEADER Header)
{
	if (Index >= Cdp_JOURNAL_HEADERS_PER_REGION)
		return STATUS_INVALID_PARAMETER;
	return CdpJournalReadHeaderRegionSlice(
		Journal,
		RegionOff,
		Index * sizeof(Cdp_JOURNAL_RECORD_HEADER),
		sizeof(*Header),
		Header);
}

static NTSTATUS CdpJournalReadHeaderRegion(
	_Inout_ PCdp_JOURNAL Journal,
	_In_ UINT64 RegionOff,
	_Out_writes_bytes_(Cdp_JOURNAL_HEADER_REGION_SIZE) PUCHAR Region)
{
	NTSTATUS status;
	ULONG offset = 0;
	ULONG chunk = Journal->HeaderRegionWriteChunk;

	if (chunk == 0 || chunk > Cdp_JOURNAL_HEADER_REGION_SIZE)
		chunk = Cdp_JOURNAL_HEADER_REGION_SIZE;
	chunk = (chunk / Journal->SectorSize) * Journal->SectorSize;
	if (chunk < Journal->SectorSize)
		chunk = Journal->SectorSize;

	while (offset < Cdp_JOURNAL_HEADER_REGION_SIZE)
	{
		ULONG remaining = Cdp_JOURNAL_HEADER_REGION_SIZE - offset;
		ULONG transfer = chunk < remaining ? chunk : remaining;

		status = CdpJournalRawIo(
			Journal,
			IRP_MJ_READ,
			RegionOff + offset,
			transfer,
			Region + offset);
		if (!NT_SUCCESS(status))
		{
			if (chunk <= Journal->SectorSize)
				return status;
			chunk /= 2;
			chunk = (chunk / Journal->SectorSize) * Journal->SectorSize;
			if (chunk < Journal->SectorSize)
				chunk = Journal->SectorSize;
			continue;
		}

		Journal->HeaderRegionWriteChunk = chunk;
		offset += transfer;
	}

	return STATUS_SUCCESS;
}

static NTSTATUS CdpJournalWriteHeaderAt(
	_In_ PCdp_JOURNAL Journal,
	_In_ UINT64 RegionOff,
	_In_ ULONG Index,
	_In_ const Cdp_JOURNAL_RECORD_HEADER* Header)
{
	if (Index >= Cdp_JOURNAL_HEADERS_PER_REGION)
		return STATUS_INVALID_PARAMETER;
	return CdpJournalRawWriteSub(
		Journal,
		RegionOff + (UINT64)Index * sizeof(Cdp_JOURNAL_RECORD_HEADER),
		sizeof(*Header),
		Header);
}

static NTSTATUS CdpJournalRefreshOldestTimeLocked(_Inout_ PCdp_JOURNAL Journal)
{
	Cdp_JOURNAL_RECORD_HEADER header;
	NTSTATUS status;

	if (CdpJournalIsEmptyLocked(Journal))
	{
		Journal->Oldest100ns = 0;
		Journal->Newest100ns = 0;
		return STATUS_SUCCESS;
	}

	status = CdpJournalReadHeaderAt(
		Journal,
		Journal->OldestHeaderRegionOff,
		Journal->OldestHeaderIndex,
		&header);
	if (!NT_SUCCESS(status))
	return status;
	Journal->Oldest100ns = header.WallClock100ns;
	return STATUS_SUCCESS;
}

// Contiguous free bytes from PayloadRegionOff without wrapping and without
// entering the oldest live header region.
static UINT64 CdpJournalContiguousFreeLocked(_In_ PCdp_JOURNAL Journal)
{
	UINT64 usableEnd = CdpJournalUsableEnd(Journal);
	UINT64 head = Journal->PayloadRegionOff;
	UINT64 tail = Journal->OldestHeaderRegionOff;

	if (CdpJournalIsEmptyLocked(Journal))
		return usableEnd - head;

	// Write cursor caught up with oldest header: no contiguous free in front.
	if (head == tail)
		return 0;

	if (head < tail)
		return tail - head;

	// head > tail: free until partition end; wrap is handled separately.
	return usableEnd - head;
}

static NTSTATUS CdpJournalDropOldestLocked(_Inout_ PCdp_JOURNAL Journal)
{
	Cdp_HEADER_REGION_LINK link;
	Cdp_JOURNAL_RECORD_HEADER header;
	UINT64 alignedSize;
	ULONG limit;
	NTSTATUS status;

	if (CdpJournalIsEmptyLocked(Journal))
		return STATUS_NOT_FOUND;

	limit = CdpJournalRegionHeaderLimit(
		Journal,
		Journal->OldestHeaderRegionOff);
	if (Journal->OldestHeaderIndex >= limit)
		return STATUS_DISK_CORRUPT_ERROR;
	status = CdpJournalReadHeaderAt(
		Journal,
		Journal->OldestHeaderRegionOff,
		Journal->OldestHeaderIndex,
		&header);
	if (!NT_SUCCESS(status))
		return status;
	if (header.DataLength == 0 ||
		header.DataLength > Cdp_JOURNAL_MAX_RECORD_DATA)
	{
		return STATUS_DISK_CORRUPT_ERROR;
	}
	alignedSize = CdpAlignUp64(header.DataLength, Journal->SectorSize);
	if (alignedSize > Journal->PayloadBytesUsed)
		return STATUS_DISK_CORRUPT_ERROR;

	Journal->OldestHeaderIndex++;
	Journal->TotalRecords--;
	Journal->PayloadBytesUsed -= alignedSize;
	CdpJournalAdvanceRecordGenerationLocked(Journal);

	if (Journal->OldestHeaderIndex < limit)
		return CdpJournalRefreshOldestTimeLocked(Journal);

	status = CdpJournalReadRegionLink(
		Journal,
		Journal->OldestHeaderRegionOff,
		&link);
	if (!NT_SUCCESS(status) || link.Marker != Cdp_JOURNAL_HEADER_LINK_MARK)
		return STATUS_DISK_CORRUPT_ERROR;

	if (Journal->OldestHeaderRegionOff == Journal->LastHeaderRegionOff &&
		Journal->TotalRecords == 0)
	{
		Journal->OldestHeaderRegionOff = Journal->LastHeaderRegionOff;
		Journal->OldestHeaderIndex = 0;
		Journal->CurrentHeaderCount = 0;
		Journal->PayloadRegionOff =
			Journal->LastHeaderRegionOff + Cdp_JOURNAL_HEADER_REGION_SIZE;
		Journal->Oldest100ns = 0;
		Journal->Newest100ns = 0;
		return STATUS_SUCCESS;
	}

	Journal->OldestHeaderRegionOff = link.NextRegionOff;
	Journal->OldestHeaderIndex = 0;
	return CdpJournalRefreshOldestTimeLocked(Journal);
}

static NTSTATUS CdpJournalEnsureContiguousLocked(
	_Inout_ PCdp_JOURNAL Journal,
	_In_ UINT64 BytesNeeded)
{
	UINT64 usableStart = CdpJournalUsableStart(Journal);
	UINT64 usableEnd = CdpJournalUsableEnd(Journal);
	ULONG guard = 0;

	if (BytesNeeded > usableEnd - usableStart)
		return STATUS_INSUFFICIENT_RESOURCES;

	for (;;)
	{
		NTSTATUS status;

		// Payload hits the end: wrap write cursor; do NOT open a new header.
		if (Journal->PayloadRegionOff + BytesNeeded > usableEnd)
			Journal->PayloadRegionOff = usableStart;

		if (CdpJournalContiguousFreeLocked(Journal) >= BytesNeeded)
			return STATUS_SUCCESS;

		status = CdpJournalDropOldestLocked(Journal);
		if (!NT_SUCCESS(status))
			return status;
		if (++guard > 1000000UL)
			return STATUS_DISK_CORRUPT_ERROR;
	}
}

// Place a new 2MB header region at the current payload cursor, then start a
// fresh payload area immediately after it: ...[Pprev][Hnew 2MB][Pnew...]
static NTSTATUS CdpJournalAllocateHeaderRegionLocked(
	_Inout_ PCdp_JOURNAL Journal,
	_Out_ PUINT64 NewRegionOff)
{
	UINT64 usableStart = CdpJournalUsableStart(Journal);
	UINT64 usableEnd = CdpJournalUsableEnd(Journal);
	UINT64 candidate;
	UINT64 oldRegion;
	Cdp_HEADER_REGION_LINK oldLink;
	Cdp_HEADER_REGION_LINK newLink;
	NTSTATUS status;
	ULONG guard = 0;

	candidate = CdpAlignUp64(Journal->PayloadRegionOff, Journal->SectorSize);
	if (candidate + Cdp_JOURNAL_HEADER_REGION_SIZE > usableEnd)
		candidate = usableStart;

	// Reclaim until [candidate, candidate+2MB) does not overlap the oldest live unit.
	while (!CdpJournalIsEmptyLocked(Journal))
	{
		UINT64 old = Journal->OldestHeaderRegionOff;
		UINT64 oldEnd = old + Cdp_JOURNAL_HEADER_REGION_SIZE;
		BOOLEAN overlaps =
			!(oldEnd <= candidate ||
				old >= candidate + Cdp_JOURNAL_HEADER_REGION_SIZE);

		if (!overlaps)
		{
			if (candidate == usableStart)
			{
				if (Journal->OldestHeaderRegionOff >=
					candidate + Cdp_JOURNAL_HEADER_REGION_SIZE)
				{
					break;
				}
			}
			else if (Journal->PayloadRegionOff <= candidate ||
				candidate == Journal->PayloadRegionOff)
			{
				break;
			}
		}

		status = CdpJournalDropOldestLocked(Journal);
		if (!NT_SUCCESS(status))
			return status;
		if (++guard > 1000000UL)
			return STATUS_DISK_CORRUPT_ERROR;
	}

	if (candidate + Cdp_JOURNAL_HEADER_REGION_SIZE > usableEnd)
		return STATUS_INSUFFICIENT_RESOURCES;

	oldRegion = Journal->LastHeaderRegionOff;
	status = CdpJournalReadRegionLink(Journal, oldRegion, &oldLink);
	if (!NT_SUCCESS(status))
		return status;

	status = CdpJournalInitHeaderRegion(
		Journal,
		candidate,
		oldRegion,
		candidate);
	if (!NT_SUCCESS(status))
		return status;

	oldLink.NextRegionOff = candidate;
	status = CdpJournalWriteRegionLink(Journal, oldRegion, &oldLink);
	if (!NT_SUCCESS(status))
		return status;

	newLink.Marker = Cdp_JOURNAL_HEADER_LINK_MARK;
	newLink.PrevRegionOff = oldRegion;
	newLink.NextRegionOff = candidate;
	newLink.Reserved = 0;
	status = CdpJournalWriteRegionLink(Journal, candidate, &newLink);
	if (!NT_SUCCESS(status))
		return status;

	Journal->LastHeaderRegionOff = candidate;
	Journal->CurrentHeaderCount = 0;
	// Payload for this header region starts immediately after it.
	Journal->PayloadRegionOff = candidate + Cdp_JOURNAL_HEADER_REGION_SIZE;

	*NewRegionOff = candidate;
	return STATUS_SUCCESS;
}

static NTSTATUS CdpJournalRebuildRuntimeLocked(_Inout_ PCdp_JOURNAL Journal)
{
	UINT64 regionOff;
	UINT64 oldestOff;
	ULONG guard = 0;
	Cdp_HEADER_REGION_LINK link;
	NTSTATUS status = STATUS_SUCCESS;
	PVOID regionAllocationBase = NULL;
	PUCHAR region = NULL;

	Journal->TotalRecords = 0;
	Journal->PayloadBytesUsed = 0;
	Journal->CurrentHeaderCount = 0;
	Journal->NextSequence = 1;
	Journal->Oldest100ns = 0;
	Journal->Newest100ns = 0;
	Journal->OldestHeaderIndex = 0;
	Journal->PayloadRegionOff =
		Journal->LastHeaderRegionOff + Cdp_JOURNAL_HEADER_REGION_SIZE;

	regionOff = Journal->LastHeaderRegionOff;
	for (;;)
	{
		status = CdpJournalReadRegionLink(Journal, regionOff, &link);
		if (!NT_SUCCESS(status) || link.Marker != Cdp_JOURNAL_HEADER_LINK_MARK)
		{
			status = STATUS_DISK_CORRUPT_ERROR;
			goto cleanup;
		}
		if (link.PrevRegionOff == regionOff)
			break;
		regionOff = link.PrevRegionOff;
		if (++guard > 100000UL)
		{
			status = STATUS_DISK_CORRUPT_ERROR;
			goto cleanup;
		}
	}
	oldestOff = regionOff;
	Journal->OldestHeaderRegionOff = oldestOff;
	region = (PUCHAR)CdpAllocateAligned(
		Journal,
		Cdp_JOURNAL_HEADER_REGION_SIZE,
		&regionAllocationBase);
	if (!region)
	{
		status = STATUS_INSUFFICIENT_RESOURCES;
		goto cleanup;
	}

	regionOff = oldestOff;
	guard = 0;
	for (;;)
	{
		ULONG index;
		BOOLEAN isLast = (regionOff == Journal->LastHeaderRegionOff);

		status = CdpJournalReadHeaderRegion(Journal, regionOff, region);
		if (!NT_SUCCESS(status))
			goto cleanup;
		RtlCopyMemory(
			&link,
			region + Cdp_JOURNAL_HEADER_REGION_SIZE -
				Cdp_JOURNAL_HEADER_LINK_SIZE,
			sizeof(link));
		if (link.Marker != Cdp_JOURNAL_HEADER_LINK_MARK)
		{
			status = STATUS_DISK_CORRUPT_ERROR;
			goto cleanup;
		}

		for (index = 0; index < Cdp_JOURNAL_HEADERS_PER_REGION; ++index)
		{
			Cdp_JOURNAL_RECORD_HEADER header;

			RtlCopyMemory(
				&header,
				region + index * sizeof(Cdp_JOURNAL_RECORD_HEADER),
				sizeof(header));

			if (header.DataLength == 0 && header.Sequence == 0)
			{
				if (isLast)
					Journal->CurrentHeaderCount = index;
				break;
			}
			if (header.DataLength == 0 ||
				header.DataLength > Cdp_JOURNAL_MAX_RECORD_DATA ||
				header.FileOffset < CdpJournalUsableStart(Journal) ||
				header.FileOffset > CdpJournalUsableEnd(Journal) ||
				header.DataLength >
					CdpJournalUsableEnd(Journal) - header.FileOffset ||
				header.VolumeOffset > MAXUINT64 - header.DataLength)
			{
				status = STATUS_DISK_CORRUPT_ERROR;
				goto cleanup;
			}

			Journal->TotalRecords++;
			Journal->PayloadBytesUsed += CdpAlignUp64(
				header.DataLength,
				Journal->SectorSize);
			if (header.Sequence >= Journal->NextSequence)
				Journal->NextSequence = header.Sequence + 1;
			if (Journal->NextSequence == 0)
				Journal->NextSequence = 1;

			if (Journal->Oldest100ns == 0 ||
				header.WallClock100ns < Journal->Oldest100ns)
			{
				Journal->Oldest100ns = header.WallClock100ns;
			}
			if (header.WallClock100ns > Journal->Newest100ns)
				Journal->Newest100ns = header.WallClock100ns;

			if (isLast)
			{
				Journal->CurrentHeaderCount = index + 1;
				Journal->PayloadRegionOff = CdpAlignUp64(
					header.FileOffset + header.DataLength,
					Journal->SectorSize);
			}
		}

		if (isLast)
			break;
		if (link.NextRegionOff == regionOff)
			break;
		regionOff = link.NextRegionOff;
		if (++guard > 100000UL)
		{
			status = STATUS_DISK_CORRUPT_ERROR;
			goto cleanup;
		}
		if (regionOff == oldestOff)
			break;
	}

cleanup:
	if (regionAllocationBase)
		cdpfree(regionAllocationBase);
	return status;
}

VOID CdpJournalInitialize(
	_Out_ PCdp_JOURNAL Journal,
	_In_opt_ PVOID TargetDevice,
	_In_opt_ PVOID RawDiskHandle,
	_In_ UINT64 TargetBaseOffset,
	_In_ UINT64 PartitionSize,
	_In_ ULONG SectorSize,
	_In_ const GUID* SourceVolumeGuid)
{
	RtlZeroMemory(Journal, sizeof(*Journal));
	Journal->TargetDevice = TargetDevice;
	Journal->RawDiskHandle = RawDiskHandle;
	Journal->TargetBaseOffset = TargetBaseOffset;
	Journal->Store = NULL;
	Journal->PartitionSize = CdpAlignDown64(PartitionSize, SectorSize);
	Journal->SectorSize = SectorSize;
	Journal->SourceVolumeGuid = *SourceVolumeGuid;
	Cdp_LOCK_INIT(&Journal->Lock);
}

VOID CdpJournalInitializeWithStore(
	_Out_ PCdp_JOURNAL Journal,
	_In_ PCdp_STORE Store,
	_In_ const GUID* SourceVolumeGuid,
	_In_opt_ Cdp_QUERY_TIME_100NS QueryTime100ns,
	_In_opt_ PVOID QueryTimeContext)
{
	RtlZeroMemory(Journal, sizeof(*Journal));
	Journal->TargetDevice = NULL;
	Journal->Store = Store;
	Journal->PartitionSize = CdpAlignDown64(Store->Size, Store->SectorSize);
	Journal->SectorSize = Store->SectorSize;
	Journal->SourceVolumeGuid = *SourceVolumeGuid;
	Journal->QueryTime100ns = QueryTime100ns;
	Journal->QueryTimeContext = QueryTimeContext;
	Cdp_LOCK_INIT(&Journal->Lock);
}

static BOOLEAN CdpJournalHasBackend(_In_ PCdp_JOURNAL Journal)
{
	return Journal->Store != NULL ||
		Journal->RawDiskHandle != NULL ||
		Journal->TargetDevice != NULL;
}

NTSTATUS CdpJournalFormat(_Inout_ PCdp_JOURNAL Journal)
{
	UINT64 usableStart;
	UINT64 usableEnd;
	UINT64 headerOff;
	UINT64 minSize;
	NTSTATUS status;
	PVOID tailAllocationBase = NULL;
	PUCHAR tailSector = NULL;

	minSize = (UINT64)Journal->SectorSize +
		Cdp_JOURNAL_HEADER_REGION_SIZE + (UINT64)Journal->SectorSize;
	if (!CdpJournalHasBackend(Journal) ||
		(Journal->SectorSize != 512 && Journal->SectorSize != 4096) ||
		Journal->PartitionSize < minSize)
	{
		return STATUS_INVALID_PARAMETER;
	}

	Cdp_LOCK_ACQUIRE(&Journal->Lock);

	usableStart = CdpJournalUsableStart(Journal);
	usableEnd = CdpJournalUsableEnd(Journal);
	headerOff = usableStart;
	if (headerOff + Cdp_JOURNAL_HEADER_REGION_SIZE >= usableEnd)
	{
		status = STATUS_INVALID_PARAMETER;
		goto cleanup;
	}

	status = CdpJournalInitHeaderRegion(
		Journal,
		headerOff,
		headerOff,
		headerOff);
	if (!NT_SUCCESS(status))
		goto cleanup;

	// v7 has no backup superblock.  Explicitly erase a stale v6 backup when
	// reformatting; the sector remains normal payload capacity afterwards.
	tailSector = (PUCHAR)CdpAllocateAligned(
		Journal,
		Journal->SectorSize,
		&tailAllocationBase);
	if (!tailSector)
	{
		status = STATUS_INSUFFICIENT_RESOURCES;
		goto cleanup;
	}
	RtlZeroMemory(tailSector, Journal->SectorSize);
	status = CdpJournalRawIo(
		Journal,
		IRP_MJ_WRITE,
		Journal->PartitionSize - Journal->SectorSize,
		Journal->SectorSize,
		tailSector);
	if (!NT_SUCCESS(status))
		goto cleanup;

	Journal->LastHeaderRegionOff = headerOff;
	// Payload area 0 starts immediately after header region 0.
	Journal->PayloadRegionOff = headerOff + Cdp_JOURNAL_HEADER_REGION_SIZE;
	Journal->OldestHeaderRegionOff = headerOff;
	Journal->OldestHeaderIndex = 0;
	Journal->CurrentHeaderCount = 0;
	Journal->NextSequence = 1;
	Journal->TotalRecords = 0;
	Journal->PayloadBytesUsed = 0;
	Journal->RecordGeneration = 1;
	Journal->Oldest100ns = 0;
	Journal->Newest100ns = 0;

		status = CdpJournalWriteSuperblockLocked(Journal);
	if (NT_SUCCESS(status))
		status = CdpJournalFlush(Journal);
	if (NT_SUCCESS(status))
		Journal->Mounted = TRUE;

cleanup:
	if (tailAllocationBase)
		cdpfree(tailAllocationBase);
	Cdp_LOCK_RELEASE(&Journal->Lock);
	return status;
}

NTSTATUS CdpJournalMount(_Inout_ PCdp_JOURNAL Journal)
{
	PVOID allocationBase = NULL;
	PUCHAR sector;
	PCdp_JOURNAL_SUPERBLOCK superblock;
	UINT64 minSize;
	NTSTATUS status;

	minSize = (UINT64)Journal->SectorSize +
		Cdp_JOURNAL_HEADER_REGION_SIZE + (UINT64)Journal->SectorSize;
	if (!CdpJournalHasBackend(Journal) ||
		(Journal->SectorSize != 512 && Journal->SectorSize != 4096) ||
		Journal->PartitionSize < minSize)
	{
		return STATUS_INVALID_PARAMETER;
	}

	Cdp_LOCK_ACQUIRE(&Journal->Lock);
	sector = (PUCHAR)CdpAllocateAligned(Journal,
		Journal->SectorSize,
		&allocationBase);
	if (!sector)
	{
		status = STATUS_INSUFFICIENT_RESOURCES;
		goto cleanup;
	}

	status = CdpJournalRawIo(
		Journal,
		IRP_MJ_READ,
		0,
		Journal->SectorSize,
		sector);
	if (!NT_SUCCESS(status))
		goto cleanup;
	superblock = (PCdp_JOURNAL_SUPERBLOCK)sector;
	if (!CdpJournalSuperblockValid(Journal, superblock))
	{
		status = STATUS_DISK_CORRUPT_ERROR;
		goto cleanup;
	}

	Journal->LastHeaderRegionOff = superblock->LastHeaderRegionOff;
	Journal->SourceVolumeGuid = superblock->SourceVolumeGuid;

	status = CdpJournalRebuildRuntimeLocked(Journal);
	if (!NT_SUCCESS(status))
		goto cleanup;

	Journal->RecordGeneration = 1;
	Journal->Mounted = TRUE;

#ifndef Cdp_USERMODE
	Cdp_DBG("[JOURNAL] mounted lastHeader=%llu "
		"payloadHead=%llu records=%llu\n",
		Journal->LastHeaderRegionOff,
		Journal->PayloadRegionOff,
		Journal->TotalRecords);
#endif
	status = STATUS_SUCCESS;

cleanup:
	if (allocationBase)
		cdpfree(allocationBase);
	if (!NT_SUCCESS(status))
		Journal->Mounted = FALSE;
	Cdp_LOCK_RELEASE(&Journal->Lock);
	return status;
}

NTSTATUS CdpJournalInvalidate(_Inout_ PCdp_JOURNAL Journal)
{
	PVOID allocationBase = NULL;
	PUCHAR sector;
	NTSTATUS status;

	if (!Journal)
		return STATUS_INVALID_PARAMETER;
	if (!CdpJournalHasBackend(Journal) ||
		(Journal->SectorSize != 512 && Journal->SectorSize != 4096))
	{
		return STATUS_INVALID_PARAMETER;
	}

	Cdp_LOCK_ACQUIRE(&Journal->Lock);
	sector = (PUCHAR)CdpAllocateAligned(Journal,
		Journal->SectorSize,
		&allocationBase);
	if (!sector)
	{
		status = STATUS_INSUFFICIENT_RESOURCES;
		goto cleanup;
	}

	RtlZeroMemory(sector, Journal->SectorSize);
	status = CdpJournalRawIo(
		Journal,
		IRP_MJ_WRITE,
		0,
		Journal->SectorSize,
		sector);
	if (NT_SUCCESS(status))
	{
		Journal->Mounted = FALSE;
#ifndef Cdp_USERMODE
		Cdp_LOG("[JOURNAL] invalidated superblock\n");
#endif
	}

cleanup:
	if (allocationBase)
		cdpfree(allocationBase);
	Cdp_LOCK_RELEASE(&Journal->Lock);
	return status;
}

NTSTATUS CdpJournalAppend(
	_Inout_ PCdp_JOURNAL Journal,
	_In_ UINT64 VolumeOffset,
	_In_ ULONG DataLength,
	_In_reads_bytes_(DataLength) const VOID* BeforeImage,
	_Out_opt_ PCdp_JOURNAL_RECORD_HEADER WrittenHeader)
{
	Cdp_JOURNAL_RECORD_HEADER header;
	UINT64 payloadOff;
	UINT64 alignedSize;
	PVOID allocationBase = NULL;
	PUCHAR payloadBuffer = NULL;
	ULONG writeSeq;
	UINT64 writeTime;
	NTSTATUS status = STATUS_SUCCESS;

	if (!Journal->Mounted || !BeforeImage ||
		DataLength == 0 || DataLength > Cdp_JOURNAL_MAX_RECORD_DATA)
	{
		return STATUS_INVALID_PARAMETER;
	}

	Cdp_LOCK_ACQUIRE(&Journal->Lock);
	if (!Journal->Mounted)
	{
		status = STATUS_DEVICE_NOT_READY;
		goto cleanup;
	}

	alignedSize = CdpAlignUp64(DataLength, Journal->SectorSize);

	// New header region only when the current 2MB header slots are exhausted.
	if (Journal->CurrentHeaderCount >= Cdp_JOURNAL_HEADERS_PER_REGION)
	{
		UINT64 newRegion = 0;
		status = CdpJournalAllocateHeaderRegionLocked(Journal, &newRegion);
		if (!NT_SUCCESS(status))
			goto cleanup;
	}

	// Payload: wrap at partition end and/or drop oldest until there is room.
	status = CdpJournalEnsureContiguousLocked(Journal, alignedSize);
	if (!NT_SUCCESS(status))
		goto cleanup;

	payloadOff = Journal->PayloadRegionOff;
	payloadBuffer = (PUCHAR)CdpAllocateAligned(Journal,
		(SIZE_T)alignedSize,
		&allocationBase);
	if (!payloadBuffer)
	{
		status = STATUS_INSUFFICIENT_RESOURCES;
		goto cleanup;
	}
	RtlZeroMemory(payloadBuffer, (SIZE_T)alignedSize);
	RtlCopyMemory(payloadBuffer, BeforeImage, DataLength);

	status = CdpJournalRawIo(
		Journal,
		IRP_MJ_WRITE,
		payloadOff,
		(ULONG)alignedSize,
		payloadBuffer);
	if (!NT_SUCCESS(status))
		goto cleanup;

	writeSeq = Journal->NextSequence;
	if (Journal->QueryTime100ns)
	{
		writeTime = Journal->QueryTime100ns(Journal->QueryTimeContext);
	}
	else
	{
#ifdef Cdp_USERMODE
		FILETIME utcFt;
		FILETIME localFt;
		ULARGE_INTEGER u;
		GetSystemTimeAsFileTime(&utcFt);
		if (!FileTimeToLocalFileTime(&utcFt, &localFt))
			localFt = utcFt;
		u.LowPart = localFt.dwLowDateTime;
		u.HighPart = localFt.dwHighDateTime;
		writeTime = u.QuadPart;
#else
		{
			LARGE_INTEGER systemTime;
			LARGE_INTEGER localTime;
			KeQuerySystemTime(&systemTime);
			ExSystemTimeToLocalTime(&systemTime, &localTime);
			writeTime = (UINT64)localTime.QuadPart;
		}
#endif
	}

	RtlZeroMemory(&header, sizeof(header));
	header.WallClock100ns = writeTime;
	header.VolumeOffset = VolumeOffset;
	header.FileOffset = payloadOff;
	header.DataLength = DataLength;
	header.Sequence = writeSeq;

	status = CdpJournalWriteHeaderAt(
		Journal,
		Journal->LastHeaderRegionOff,
		Journal->CurrentHeaderCount,
		&header);
		if (!NT_SUCCESS(status))
			goto cleanup;

	Journal->CurrentHeaderCount++;
	Journal->PayloadRegionOff = payloadOff + alignedSize;
	Journal->NextSequence = writeSeq + 1;
	if (Journal->NextSequence == 0)
		Journal->NextSequence = 1;
	if (Journal->PayloadBytesUsed > MAXUINT64 - alignedSize)
	{
		status = STATUS_DISK_CORRUPT_ERROR;
		goto cleanup;
	}
	Journal->TotalRecords++;
	Journal->PayloadBytesUsed += alignedSize;
	CdpJournalAdvanceRecordGenerationLocked(Journal);
	Journal->Newest100ns = writeTime;
	if (Journal->TotalRecords == 1)
	{
		Journal->OldestHeaderRegionOff = Journal->LastHeaderRegionOff;
		Journal->OldestHeaderIndex = Journal->CurrentHeaderCount - 1;
		Journal->Oldest100ns = writeTime;
	}

	if (WrittenHeader)
		*WrittenHeader = header;

	status = CdpJournalWriteSuperblockLocked(Journal);
	if (NT_SUCCESS(status))
		status = CdpJournalFlush(Journal);

cleanup:
	if (allocationBase)
		cdpfree(allocationBase);
	Cdp_LOCK_RELEASE(&Journal->Lock);
	return status;
}

NTSTATUS CdpJournalQueryTimeRange(
	_Inout_ PCdp_JOURNAL Journal,
	_Out_ PUINT64 OldestTime100ns,
	_Out_ PUINT64 NewestTime100ns)
{
	NTSTATUS status;

	if (!OldestTime100ns || !NewestTime100ns)
		return STATUS_INVALID_PARAMETER;
	*OldestTime100ns = 0;
	*NewestTime100ns = 0;

	Cdp_LOCK_ACQUIRE(&Journal->Lock);
	if (!Journal->Mounted)
		status = STATUS_DEVICE_NOT_READY;
	else if (CdpJournalIsEmptyLocked(Journal))
		status = STATUS_NOT_FOUND;
	else
	{
		*OldestTime100ns = Journal->Oldest100ns;
		*NewestTime100ns = Journal->Newest100ns;
		status = STATUS_SUCCESS;
	}
	Cdp_LOCK_RELEASE(&Journal->Lock);
	return status;
}

static NTSTATUS CdpJournalCountActiveHeaderRegionsLocked(
	_Inout_ PCdp_JOURNAL Journal,
	_Out_ PUINT64 HeaderRegionCount)
{
	UINT64 regionOff;
	UINT64 count;
	ULONG guard = 0;
	Cdp_HEADER_REGION_LINK link;
	NTSTATUS status;

	if (!HeaderRegionCount)
		return STATUS_INVALID_PARAMETER;

	regionOff = Journal->OldestHeaderRegionOff;
	count = 1;
	while (regionOff != Journal->LastHeaderRegionOff)
	{
		status = CdpJournalReadRegionLink(Journal, regionOff, &link);
		if (!NT_SUCCESS(status))
			return status;
		if (link.Marker != Cdp_JOURNAL_HEADER_LINK_MARK ||
			link.NextRegionOff == regionOff)
		{
			return STATUS_DISK_CORRUPT_ERROR;
		}
		regionOff = link.NextRegionOff;
		if (++count > 100001ULL || ++guard > 100000UL)
			return STATUS_DISK_CORRUPT_ERROR;
	}

	*HeaderRegionCount = count;
	return STATUS_SUCCESS;
}

NTSTATUS CdpJournalQueryUsage(
	_Inout_ PCdp_JOURNAL Journal,
	_Out_ PUINT64 PartitionBytes,
	_Out_ PUINT64 MetadataBytes,
	_Out_ PUINT64 PayloadBytesUsed,
	_Out_ PUINT64 PayloadBytesFree,
	_Out_ PUINT64 TotalRecords)
{
	UINT64 headerRegionCount;
	UINT64 metadataBytes;
	NTSTATUS status;

	if (!Journal || !PartitionBytes || !MetadataBytes ||
		!PayloadBytesUsed || !PayloadBytesFree || !TotalRecords)
	{
		return STATUS_INVALID_PARAMETER;
	}

	*PartitionBytes = 0;
	*MetadataBytes = 0;
	*PayloadBytesUsed = 0;
	*PayloadBytesFree = 0;
	*TotalRecords = 0;

	Cdp_LOCK_ACQUIRE(&Journal->Lock);
	if (!Journal->Mounted)
	{
		status = STATUS_DEVICE_NOT_READY;
		goto cleanup;
	}

	status = CdpJournalCountActiveHeaderRegionsLocked(
		Journal,
		&headerRegionCount);
	if (!NT_SUCCESS(status))
		goto cleanup;
	if (headerRegionCount >
		(MAXUINT64 - Journal->SectorSize) / Cdp_JOURNAL_HEADER_REGION_SIZE)
	{
		status = STATUS_DISK_CORRUPT_ERROR;
		goto cleanup;
	}

	metadataBytes = Journal->SectorSize +
		headerRegionCount * Cdp_JOURNAL_HEADER_REGION_SIZE;
	if (metadataBytes > Journal->PartitionSize ||
		Journal->PayloadBytesUsed > Journal->PartitionSize - metadataBytes)
	{
		status = STATUS_DISK_CORRUPT_ERROR;
		goto cleanup;
	}

	*PartitionBytes = Journal->PartitionSize;
	*MetadataBytes = metadataBytes;
	*PayloadBytesUsed = Journal->PayloadBytesUsed;
	*PayloadBytesFree =
		Journal->PartitionSize - metadataBytes - Journal->PayloadBytesUsed;
	*TotalRecords = Journal->TotalRecords;
	status = STATUS_SUCCESS;

cleanup:
	Cdp_LOCK_RELEASE(&Journal->Lock);
	return status;
}

NTSTATUS CdpJournalQueryRecordHeaders(
	_Inout_ PCdp_JOURNAL Journal,
	_In_ UINT64 StartIndex,
	_In_ UINT64 ExpectedGeneration,
	_Out_writes_to_(HeaderCapacity, *ReturnedCount) PCdp_JOURNAL_RECORD_HEADER Headers,
	_In_ ULONG HeaderCapacity,
	_Out_ PUINT64 TotalRecords,
	_Out_ PUINT64 Generation,
	_Out_ PULONG ReturnedCount)
{
	UINT64 remaining;
	UINT64 regionOff;
	ULONG headerIndex;
	ULONG returned = 0;
	ULONG wanted;
	ULONG guard = 0;
	NTSTATUS status;

	if (!Journal || !TotalRecords || !Generation || !ReturnedCount ||
		(HeaderCapacity != 0 && !Headers))
	{
		return STATUS_INVALID_PARAMETER;
	}

	*TotalRecords = 0;
	*Generation = 0;
	*ReturnedCount = 0;

	Cdp_LOCK_ACQUIRE(&Journal->Lock);
	if (!Journal->Mounted)
	{
		status = STATUS_DEVICE_NOT_READY;
		goto cleanup;
	}

	*TotalRecords = Journal->TotalRecords;
	*Generation = Journal->RecordGeneration;
	if (ExpectedGeneration != 0 &&
		ExpectedGeneration != Journal->RecordGeneration)
	{
		status = STATUS_RETRY;
		goto cleanup;
	}
	if (StartIndex > Journal->TotalRecords)
	{
		status = STATUS_INVALID_PARAMETER;
		goto cleanup;
	}
	if (StartIndex == Journal->TotalRecords || HeaderCapacity == 0)
	{
		status = STATUS_SUCCESS;
		goto cleanup;
	}

	remaining = Journal->TotalRecords - StartIndex;
	wanted = remaining < HeaderCapacity ? (ULONG)remaining : HeaderCapacity;
	regionOff = Journal->OldestHeaderRegionOff;
	headerIndex = Journal->OldestHeaderIndex;
	remaining = StartIndex;

	while (remaining != 0)
	{
		ULONG limit = CdpJournalRegionHeaderLimit(Journal, regionOff);
		ULONG available;
		Cdp_HEADER_REGION_LINK link;

		if (headerIndex >= limit)
		{
			status = STATUS_DISK_CORRUPT_ERROR;
			goto cleanup;
		}
		available = limit - headerIndex;
		if (remaining < available)
		{
			headerIndex += (ULONG)remaining;
			remaining = 0;
			break;
		}

		remaining -= available;
		if (regionOff == Journal->LastHeaderRegionOff)
		{
			status = STATUS_DISK_CORRUPT_ERROR;
			goto cleanup;
		}
		status = CdpJournalReadRegionLink(Journal, regionOff, &link);
		if (!NT_SUCCESS(status))
			goto cleanup;
		if (link.Marker != Cdp_JOURNAL_HEADER_LINK_MARK ||
			link.NextRegionOff == regionOff || ++guard > 100000UL)
		{
			status = STATUS_DISK_CORRUPT_ERROR;
			goto cleanup;
		}
		regionOff = link.NextRegionOff;
		headerIndex = 0;
	}

	while (returned < wanted)
	{
		ULONG limit = CdpJournalRegionHeaderLimit(Journal, regionOff);
		Cdp_JOURNAL_RECORD_HEADER header;

		if (headerIndex >= limit)
		{
			Cdp_HEADER_REGION_LINK link;

			if (regionOff == Journal->LastHeaderRegionOff)
			{
				status = STATUS_DISK_CORRUPT_ERROR;
				goto cleanup;
			}
			status = CdpJournalReadRegionLink(Journal, regionOff, &link);
			if (!NT_SUCCESS(status))
				goto cleanup;
			if (link.Marker != Cdp_JOURNAL_HEADER_LINK_MARK ||
				link.NextRegionOff == regionOff || ++guard > 100000UL)
			{
				status = STATUS_DISK_CORRUPT_ERROR;
				goto cleanup;
			}
			regionOff = link.NextRegionOff;
			headerIndex = 0;
			continue;
		}

		status = CdpJournalReadHeaderAt(
			Journal,
			regionOff,
			headerIndex,
			&header);
		if (!NT_SUCCESS(status))
			goto cleanup;
		if (header.DataLength == 0 ||
			header.DataLength > Cdp_JOURNAL_MAX_RECORD_DATA)
		{
			status = STATUS_DISK_CORRUPT_ERROR;
			goto cleanup;
		}

		Headers[returned++] = header;
		headerIndex++;
	}

	*ReturnedCount = returned;
	status = STATUS_SUCCESS;

cleanup:
	Cdp_LOCK_RELEASE(&Journal->Lock);
	return status;
}

VOID CdpPreviewTreeInitialize(_Out_ PCdp_PREVIEW_TREE Tree)
{
	RtlZeroMemory(Tree, sizeof(*Tree));
}

static VOID CdpPreviewTreeFreeNode(_In_opt_ PCdp_PREVIEW_TREE_NODE Node)
{
	if (!Node)
		return;
	CdpPreviewTreeFreeNode(Node->Left);
	CdpPreviewTreeFreeNode(Node->Right);
	cdpfree(Node);
}

VOID CdpPreviewTreeFree(_Inout_ PCdp_PREVIEW_TREE Tree)
{
	if (!Tree)
		return;
	CdpPreviewTreeFreeNode(Tree->Root);
	Tree->Root = NULL;
	Tree->NodeCount = 0;
}

static LONG CdpPreviewTreeNodeHeight(
	_In_opt_ PCdp_PREVIEW_TREE_NODE Node)
{
	return Node ? Node->Height : 0;
}

static VOID CdpPreviewTreeNodeUpdate(
	_Inout_ PCdp_PREVIEW_TREE_NODE Node)
{
	LONG hl = CdpPreviewTreeNodeHeight(Node->Left);
	LONG hr = CdpPreviewTreeNodeHeight(Node->Right);
	UINT64 maxEnd = Node->End;

	Node->Height = 1 + (hl > hr ? hl : hr);
	if (Node->Left && Node->Left->MaxEnd > maxEnd)
		maxEnd = Node->Left->MaxEnd;
	if (Node->Right && Node->Right->MaxEnd > maxEnd)
		maxEnd = Node->Right->MaxEnd;
	Node->MaxEnd = maxEnd;
}

static PCdp_PREVIEW_TREE_NODE CdpPreviewTreeRotateRight(
	_Inout_ PCdp_PREVIEW_TREE_NODE Y)
{
	PCdp_PREVIEW_TREE_NODE x = Y->Left;
	PCdp_PREVIEW_TREE_NODE t2 = x->Right;

	x->Right = Y;
	Y->Left = t2;
	CdpPreviewTreeNodeUpdate(Y);
	CdpPreviewTreeNodeUpdate(x);
	return x;
}

static PCdp_PREVIEW_TREE_NODE CdpPreviewTreeRotateLeft(
	_Inout_ PCdp_PREVIEW_TREE_NODE X)
{
	PCdp_PREVIEW_TREE_NODE y = X->Right;
	PCdp_PREVIEW_TREE_NODE t2 = y->Left;

	y->Left = X;
	X->Right = t2;
	CdpPreviewTreeNodeUpdate(X);
	CdpPreviewTreeNodeUpdate(y);
	return y;
}

static PCdp_PREVIEW_TREE_NODE CdpPreviewTreeAvlInsertNode(
	_In_opt_ PCdp_PREVIEW_TREE_NODE Root,
	_In_ PCdp_PREVIEW_TREE_NODE Node)
{
	LONG balance;

	if (!Root)
		return Node;

	if (Node->Start < Root->Start)
		Root->Left = CdpPreviewTreeAvlInsertNode(Root->Left, Node);
	else
		Root->Right = CdpPreviewTreeAvlInsertNode(Root->Right, Node);

	CdpPreviewTreeNodeUpdate(Root);
	balance = CdpPreviewTreeNodeHeight(Root->Left) -
		CdpPreviewTreeNodeHeight(Root->Right);

	if (balance > 1 && Node->Start < Root->Left->Start)
		return CdpPreviewTreeRotateRight(Root);
	if (balance < -1 && Node->Start >= Root->Right->Start)
		return CdpPreviewTreeRotateLeft(Root);
	if (balance > 1 && Node->Start >= Root->Left->Start)
	{
		Root->Left = CdpPreviewTreeRotateLeft(Root->Left);
		return CdpPreviewTreeRotateRight(Root);
	}
	if (balance < -1 && Node->Start < Root->Right->Start)
	{
		Root->Right = CdpPreviewTreeRotateRight(Root->Right);
		return CdpPreviewTreeRotateLeft(Root);
	}
	return Root;
}

static NTSTATUS CdpPreviewTreeInsertRaw(
	_Inout_ PCdp_PREVIEW_TREE Tree,
	_In_ const Cdp_JOURNAL_RECORD_HEADER* Header)
{
	PCdp_PREVIEW_TREE_NODE node;

	if (!Tree || !Header)
		return STATUS_INVALID_PARAMETER;
	if (Header->DataLength == 0)
		return STATUS_SUCCESS;

	node = (PCdp_PREVIEW_TREE_NODE)cdpalloc(sizeof(*node));
	if (!node)
		return STATUS_INSUFFICIENT_RESOURCES;

	RtlZeroMemory(node, sizeof(*node));
	node->Start = Header->VolumeOffset;
	node->End = Header->VolumeOffset + Header->DataLength;
	node->MaxEnd = node->End;
	node->FileOffset = Header->FileOffset;
	node->WallClock100ns = Header->WallClock100ns;
	node->DataLength = Header->DataLength;
	node->Sequence = Header->Sequence;
	node->Height = 1;
	node->Invalid = FALSE;

	Tree->Root = CdpPreviewTreeAvlInsertNode(Tree->Root, node);
	Tree->NodeCount++;
	return STATUS_SUCCESS;
}

static ULONG CdpBitmapByteCount(_In_ ULONG BitCount)
{
	return (BitCount + 7UL) / 8UL;
}

static BOOLEAN CdpBitmapTest(_In_reads_bytes_((BitCount + 7) / 8) const UCHAR* Bitmap,
	_In_ ULONG BitCount,
	_In_ ULONG Bit)
{
	UNREFERENCED_PARAMETER(BitCount);
	return (Bitmap[Bit >> 3] & (UCHAR)(1U << (Bit & 7))) != 0;
}

static VOID CdpBitmapSetRange(
	_Inout_ PUCHAR Bitmap,
	_In_ ULONG StartBit,
	_In_ ULONG BitCount)
{
	ULONG bit = StartBit;
	ULONG end = StartBit + BitCount;

	while (bit < end && (bit & 7) != 0)
	{
		Bitmap[bit >> 3] |= (UCHAR)(1U << (bit & 7));
		++bit;
	}
	if (bit + 8 <= end)
	{
		ULONG bytes = (end - bit) >> 3;
		RtlFillMemory(Bitmap + (bit >> 3), bytes, 0xFF);
		bit += bytes << 3;
	}
	while (bit < end)
	{
		Bitmap[bit >> 3] |= (UCHAR)(1U << (bit & 7));
		++bit;
	}
}

static VOID CdpBitmapClearRange(
	_Inout_ PUCHAR Bitmap,
	_In_ ULONG StartBit,
	_In_ ULONG BitCount)
{
	ULONG bit = StartBit;
	ULONG end = StartBit + BitCount;

	while (bit < end && (bit & 7) != 0)
	{
		Bitmap[bit >> 3] &= (UCHAR)~(1U << (bit & 7));
		++bit;
	}
	if (bit + 8 <= end)
	{
		ULONG bytes = (end - bit) >> 3;
		RtlZeroMemory(Bitmap + (bit >> 3), bytes);
		bit += bytes << 3;
	}
	while (bit < end)
	{
		Bitmap[bit >> 3] &= (UCHAR)~(1U << (bit & 7));
		++bit;
	}
}

// Forward: used by Insert for in-tree dedup.  Mask is one bit per byte.
static VOID CdpPreviewTreeClearMaskByTree(
	_In_ PCdp_PREVIEW_TREE Tree,
	_In_ UINT64 VolumeOffset,
	_In_ ULONG DataLength,
	_Inout_updates_bytes_((DataLength + 7) / 8) PUCHAR Mask);

NTSTATUS CdpPreviewTreeInsert(
	_Inout_ PCdp_PREVIEW_TREE Tree,
	_In_ const Cdp_JOURNAL_RECORD_HEADER* Header)
{
	PUCHAR mask = NULL;
	ULONG len;
	ULONG idx;
	ULONG runStart;
	NTSTATUS status = STATUS_SUCCESS;

	if (!Tree || !Header)
		return STATUS_INVALID_PARAMETER;
	if (Header->DataLength == 0)
		return STATUS_SUCCESS;
	if (Header->VolumeOffset > MAXUINT64 - Header->DataLength)
		return STATUS_INVALID_PARAMETER;

	// Empty tree: raw insert.
	if (!Tree->Root)
		return CdpPreviewTreeInsertRaw(Tree, Header);

	len = Header->DataLength;
	mask = (PUCHAR)cdpalloc(CdpBitmapByteCount(len));
	if (!mask)
		return STATUS_INSUFFICIENT_RESOURCES;

	RtlFillMemory(mask, CdpBitmapByteCount(len), 0xFF);
	CdpPreviewTreeClearMaskByTree(
		Tree,
		Header->VolumeOffset,
		len,
		mask);

	idx = 0;
	while (idx < len)
	{
		while (idx < len && !CdpBitmapTest(mask, len, idx))
			++idx;
		if (idx >= len)
			break;
		runStart = idx;
		while (idx < len && CdpBitmapTest(mask, len, idx))
			++idx;

		{
			Cdp_JOURNAL_RECORD_HEADER frag = *Header;
			frag.VolumeOffset = Header->VolumeOffset + runStart;
			frag.FileOffset = Header->FileOffset + runStart;
			frag.DataLength = idx - runStart;
			status = CdpPreviewTreeInsertRaw(Tree, &frag);
			if (!NT_SUCCESS(status))
				break;
		}
	}

	cdpfree(mask);
	return status;
}

static NTSTATUS CdpPreviewTreeMergeNode(
	_Inout_ PCdp_PREVIEW_TREE Dest,
	_In_opt_ PCdp_PREVIEW_TREE_NODE Node)
{
	Cdp_JOURNAL_RECORD_HEADER header;
	NTSTATUS status;

	if (!Node)
		return STATUS_SUCCESS;

	status = CdpPreviewTreeMergeNode(Dest, Node->Left);
	if (!NT_SUCCESS(status))
		return status;

	RtlZeroMemory(&header, sizeof(header));
	header.WallClock100ns = Node->WallClock100ns;
	header.VolumeOffset = Node->Start;
	header.FileOffset = Node->FileOffset;
	header.DataLength = Node->DataLength;
	header.Sequence = Node->Sequence;
	status = CdpPreviewTreeInsert(Dest, &header);
	if (!NT_SUCCESS(status))
		return status;

	return CdpPreviewTreeMergeNode(Dest, Node->Right);
}

NTSTATUS CdpPreviewTreeMergeFrom(
	_Inout_ PCdp_PREVIEW_TREE Dest,
	_Inout_ PCdp_PREVIEW_TREE Source)
{
	NTSTATUS status;

	if (!Dest || !Source)
		return STATUS_INVALID_PARAMETER;
	if (!Source->Root)
		return STATUS_SUCCESS;

	status = CdpPreviewTreeMergeNode(Dest, Source->Root);
	CdpPreviewTreeFree(Source);
	return status;
}

static NTSTATUS CdpPreviewTreePunchByNode(
	_Inout_ PCdp_PREVIEW_TREE HistoryTree,
	_In_opt_ PCdp_PREVIEW_TREE_NODE Node)
{
	NTSTATUS status;

	if (!Node)
		return STATUS_SUCCESS;

	status = CdpPreviewTreePunchByNode(HistoryTree, Node->Left);
	if (!NT_SUCCESS(status))
		return status;
	status = CdpPreviewTreePunchRange(
		HistoryTree,
		Node->Start,
		Node->DataLength);
	if (!NT_SUCCESS(status))
		return status;
	return CdpPreviewTreePunchByNode(HistoryTree, Node->Right);
}

NTSTATUS CdpPreviewTreePunchByStaging(
	_Inout_ PCdp_PREVIEW_TREE HistoryTree,
	_Inout_ PCdp_PREVIEW_TREE StagingTree)
{
	NTSTATUS status;

	if (!HistoryTree || !StagingTree)
		return STATUS_INVALID_PARAMETER;

	if (StagingTree->Root)
		status = CdpPreviewTreePunchByNode(HistoryTree, StagingTree->Root);
	else
		status = STATUS_SUCCESS;

	CdpPreviewTreeFree(StagingTree);
	return status;
}

static VOID CdpPreviewTreeInvalidateOverlaps(
	_In_opt_ PCdp_PREVIEW_TREE_NODE Node,
	_In_ UINT64 CutStart,
	_In_ UINT64 CutEnd)
{
	if (!Node)
		return;
	if (Node->MaxEnd <= CutStart)
		return;

	if (Node->Left)
		CdpPreviewTreeInvalidateOverlaps(Node->Left, CutStart, CutEnd);

	if (!Node->Invalid &&
		Node->Start < CutEnd &&
		Node->End > CutStart)
	{
		Node->Invalid = TRUE;
	}

	if (Node->Start < CutEnd && Node->Right)
		CdpPreviewTreeInvalidateOverlaps(Node->Right, CutStart, CutEnd);
}

VOID CdpPreviewTreeInvalidateRange(
	_Inout_ PCdp_PREVIEW_TREE Tree,
	_In_ UINT64 VolumeOffset,
	_In_ ULONG DataLength)
{
	if (!Tree || !Tree->Root || DataLength == 0 ||
		VolumeOffset > MAXUINT64 - DataLength)
	{
		return;
	}

	CdpPreviewTreeInvalidateOverlaps(
		Tree->Root,
		VolumeOffset,
		VolumeOffset + DataLength);
}

typedef struct _Cdp_PREVIEW_HIT
{
	Cdp_PREVIEW_TREE_NODE Node;
} Cdp_PREVIEW_HIT, *PCdp_PREVIEW_HIT;

static ULONG CdpPreviewTreeCountOverlaps(
	_In_opt_ PCdp_PREVIEW_TREE_NODE Node,
	_In_ UINT64 QueryStart,
	_In_ UINT64 QueryEnd)
{
	ULONG count = 0;

	if (!Node || Node->MaxEnd <= QueryStart)
		return 0;
	if (Node->Left)
		count += CdpPreviewTreeCountOverlaps(
			Node->Left, QueryStart, QueryEnd);
	if (!Node->Invalid && Node->Start < QueryEnd && Node->End > QueryStart)
		++count;
	if (Node->Start < QueryEnd && Node->Right)
		count += CdpPreviewTreeCountOverlaps(
			Node->Right, QueryStart, QueryEnd);
	return count;
}

static VOID CdpPreviewTreeCollectOverlaps(
	_In_opt_ PCdp_PREVIEW_TREE_NODE Node,
	_In_ UINT64 QueryStart,
	_In_ UINT64 QueryEnd,
	_Inout_updates_(HitCapacity) Cdp_PREVIEW_HIT* Hits,
	_Inout_ PULONG HitCount,
	_In_ ULONG HitCapacity)
{
	if (!Node || *HitCount >= HitCapacity)
		return;
	if (Node->MaxEnd <= QueryStart)
		return;

	if (Node->Left)
		CdpPreviewTreeCollectOverlaps(
			Node->Left,
			QueryStart,
			QueryEnd,
			Hits,
			HitCount,
			HitCapacity);

	if (!Node->Invalid &&
		Node->Start < QueryEnd &&
		Node->End > QueryStart)
	{
		if (*HitCount < HitCapacity)
		{
			Hits[*HitCount].Node = *Node;
			(*HitCount)++;
		}
	}

	if (Node->Start < QueryEnd && Node->Right)
		CdpPreviewTreeCollectOverlaps(
			Node->Right,
			QueryStart,
			QueryEnd,
			Hits,
			HitCount,
			HitCapacity);
}

static int CdpPreviewHitCompareSequence(
	_In_ const VOID* A,
	_In_ const VOID* B)
{
	const Cdp_PREVIEW_HIT* ha = (const Cdp_PREVIEW_HIT*)A;
	const Cdp_PREVIEW_HIT* hb = (const Cdp_PREVIEW_HIT*)B;
	if (ha->Node.Sequence < hb->Node.Sequence)
		return -1;
	if (ha->Node.Sequence > hb->Node.Sequence)
		return 1;
	return 0;
}

static VOID CdpPreviewSortHitsBySequence(
	_Inout_updates_(Count) PCdp_PREVIEW_HIT Hits,
	_In_ ULONG Count)
{
	ULONG start;
	ULONG end;

	if (Count < 2)
		return;

	// In-place heap sort: bounded stack usage and O(k log k) for large reads.
	for (start = Count / 2; start > 0;)
	{
		ULONG root = --start;
		Cdp_PREVIEW_HIT value = Hits[root];
		for (;;)
		{
			ULONG child = root * 2 + 1;
			if (child >= Count)
				break;
			if (child + 1 < Count &&
				CdpPreviewHitCompareSequence(&Hits[child], &Hits[child + 1]) < 0)
				++child;
			if (CdpPreviewHitCompareSequence(&value, &Hits[child]) >= 0)
				break;
			Hits[root] = Hits[child];
			root = child;
		}
		Hits[root] = value;
	}

	for (end = Count - 1; end > 0; --end)
	{
		Cdp_PREVIEW_HIT top = Hits[0];
		ULONG root = 0;
		Hits[0] = Hits[end];
		Hits[end] = top;
		for (;;)
		{
			ULONG child = root * 2 + 1;
			Cdp_PREVIEW_HIT value;
			if (child >= end)
				break;
			if (child + 1 < end &&
				CdpPreviewHitCompareSequence(&Hits[child], &Hits[child + 1]) < 0)
				++child;
			if (CdpPreviewHitCompareSequence(&Hits[root], &Hits[child]) >= 0)
				break;
			value = Hits[root];
			Hits[root] = Hits[child];
			Hits[child] = value;
			root = child;
		}
	}
}

static VOID CdpPreviewTreeCollectAllValid(
	_In_opt_ PCdp_PREVIEW_TREE_NODE Node,
	_Out_writes_(Capacity) PCdp_PREVIEW_TREE_NODE* Nodes,
	_Inout_ PULONG Count,
	_In_ ULONG Capacity)
{
	if (!Node || *Count >= Capacity)
		return;

	CdpPreviewTreeCollectAllValid(Node->Left, Nodes, Count, Capacity);
	if (!Node->Invalid && *Count < Capacity)
	{
		Nodes[*Count] = Node;
		(*Count)++;
	}
	CdpPreviewTreeCollectAllValid(Node->Right, Nodes, Count, Capacity);
}

NTSTATUS CdpPreviewTreePunchRange(
	_Inout_ PCdp_PREVIEW_TREE Tree,
	_In_ UINT64 VolumeOffset,
	_In_ ULONG DataLength)
{
	PCdp_PREVIEW_TREE_NODE* nodes = NULL;
	Cdp_JOURNAL_RECORD_HEADER* headers = NULL;
	Cdp_PREVIEW_TREE rebuilt;
	UINT64 cutEnd;
	ULONG capacity;
	ULONG count = 0;
	ULONG i;
	NTSTATUS status = STATUS_SUCCESS;

	if (!Tree || DataLength == 0 ||
		VolumeOffset > MAXUINT64 - DataLength)
	{
		return STATUS_INVALID_PARAMETER;
	}
	if (!Tree->Root || Tree->NodeCount == 0)
		return STATUS_SUCCESS;

	cutEnd = VolumeOffset + DataLength;
	capacity = Tree->NodeCount;
	nodes = (PCdp_PREVIEW_TREE_NODE*)cdpalloc(
		sizeof(PCdp_PREVIEW_TREE_NODE) * capacity);
	headers = (Cdp_JOURNAL_RECORD_HEADER*)cdpalloc(
		sizeof(Cdp_JOURNAL_RECORD_HEADER) * capacity);
	if (!nodes || !headers)
	{
		status = STATUS_INSUFFICIENT_RESOURCES;
		goto cleanup;
	}

	CdpPreviewTreeCollectAllValid(Tree->Root, nodes, &count, capacity);
	for (i = 0; i < count; ++i)
	{
		RtlZeroMemory(&headers[i], sizeof(headers[i]));
		headers[i].WallClock100ns = nodes[i]->WallClock100ns;
		headers[i].VolumeOffset = nodes[i]->Start;
		headers[i].FileOffset = nodes[i]->FileOffset;
		headers[i].DataLength = nodes[i]->DataLength;
		headers[i].Sequence = nodes[i]->Sequence;
	}

	CdpPreviewTreeInitialize(&rebuilt);
	for (i = 0; i < count; ++i)
	{
		Cdp_JOURNAL_RECORD_HEADER* header = &headers[i];
		UINT64 headerEnd = header->VolumeOffset + header->DataLength;

		if (headerEnd <= VolumeOffset || header->VolumeOffset >= cutEnd)
		{
			status = CdpPreviewTreeInsertRaw(&rebuilt, header);
		}
		else
		{
			if (header->VolumeOffset < VolumeOffset)
			{
				Cdp_JOURNAL_RECORD_HEADER left = *header;
				left.DataLength = (ULONG)(VolumeOffset - header->VolumeOffset);
				status = CdpPreviewTreeInsertRaw(&rebuilt, &left);
			}
			if (NT_SUCCESS(status) && headerEnd > cutEnd)
			{
				Cdp_JOURNAL_RECORD_HEADER right = *header;
				right.VolumeOffset = cutEnd;
				right.FileOffset = header->FileOffset +
					(cutEnd - header->VolumeOffset);
				right.DataLength = (ULONG)(headerEnd - cutEnd);
				status = CdpPreviewTreeInsertRaw(&rebuilt, &right);
			}
		}
		if (!NT_SUCCESS(status))
			break;
	}

	if (NT_SUCCESS(status))
	{
		CdpPreviewTreeFree(Tree);
		*Tree = rebuilt;
	}
	else
	{
		CdpPreviewTreeFree(&rebuilt);
	}

cleanup:
	if (nodes)
		cdpfree(nodes);
	if (headers)
		cdpfree(headers);
	return status;
}

static int CdpPreviewHeaderCompareSequence(
	_In_ const Cdp_JOURNAL_RECORD_HEADER* A,
	_In_ const Cdp_JOURNAL_RECORD_HEADER* B)
{
	if (A->Sequence < B->Sequence)
		return -1;
	if (A->Sequence > B->Sequence)
		return 1;
	return 0;
}

static VOID CdpPreviewSortHeadersBySequence(
	_Inout_updates_(Count) Cdp_JOURNAL_RECORD_HEADER* Headers,
	_In_ ULONG Count)
{
	ULONG i;
	for (i = 1; i < Count; ++i)
	{
		Cdp_JOURNAL_RECORD_HEADER key = Headers[i];
		ULONG j = i;
		while (j > 0 &&
			CdpPreviewHeaderCompareSequence(&Headers[j - 1], &key) > 0)
		{
			Headers[j] = Headers[j - 1];
			--j;
		}
		Headers[j] = key;
	}
}

static VOID CdpPreviewTreeClearMaskByNode(
	_In_opt_ PCdp_PREVIEW_TREE_NODE Node,
	_In_ UINT64 QueryStart,
	_In_ UINT64 QueryEnd,
	_Inout_ PUCHAR Mask)
{
	if (!Node || Node->MaxEnd <= QueryStart)
		return;

	if (Node->Left)
		CdpPreviewTreeClearMaskByNode(
			Node->Left, QueryStart, QueryEnd, Mask);

	if (!Node->Invalid && Node->Start < QueryEnd && Node->End > QueryStart)
	{
		UINT64 start = Node->Start > QueryStart ? Node->Start : QueryStart;
		UINT64 end = Node->End < QueryEnd ? Node->End : QueryEnd;
		CdpBitmapClearRange(
			Mask,
			(ULONG)(start - QueryStart),
			(ULONG)(end - start));
	}

	if (Node->Start < QueryEnd && Node->Right)
		CdpPreviewTreeClearMaskByNode(
			Node->Right, QueryStart, QueryEnd, Mask);
}

static VOID CdpPreviewTreeClearMaskByTree(
	_In_ PCdp_PREVIEW_TREE Tree,
	_In_ UINT64 VolumeOffset,
	_In_ ULONG DataLength,
	_Inout_updates_bytes_((DataLength + 7) / 8) PUCHAR Mask)
{
	if (!Tree || !Tree->Root || !Mask || DataLength == 0)
		return;

	CdpPreviewTreeClearMaskByNode(
		Tree->Root,
		VolumeOffset,
		VolumeOffset + DataLength,
		Mask);
}

NTSTATUS CdpPreviewTreeDedupEarliest(
	_Inout_ PCdp_PREVIEW_TREE Tree)
{
	PCdp_PREVIEW_TREE_NODE* nodes = NULL;
	Cdp_JOURNAL_RECORD_HEADER* headers = NULL;
	ULONG count = 0;
	ULONG capacity;
	ULONG i;
	NTSTATUS status = STATUS_SUCCESS;
	Cdp_PREVIEW_TREE rebuilt;

	if (!Tree)
		return STATUS_INVALID_PARAMETER;
	if (!Tree->Root || Tree->NodeCount == 0)
		return STATUS_SUCCESS;

	capacity = Tree->NodeCount;
	nodes = (PCdp_PREVIEW_TREE_NODE*)cdpalloc(
		sizeof(PCdp_PREVIEW_TREE_NODE) * capacity);
	headers = (Cdp_JOURNAL_RECORD_HEADER*)cdpalloc(
		sizeof(Cdp_JOURNAL_RECORD_HEADER) * capacity);
	if (!nodes || !headers)
		{
			status = STATUS_INSUFFICIENT_RESOURCES;
			goto cleanup;
		}

	CdpPreviewTreeCollectAllValid(Tree->Root, nodes, &count, capacity);
	for (i = 0; i < count; ++i)
	{
		RtlZeroMemory(&headers[i], sizeof(headers[i]));
		headers[i].WallClock100ns = nodes[i]->WallClock100ns;
		headers[i].VolumeOffset = nodes[i]->Start;
		headers[i].FileOffset = nodes[i]->FileOffset;
		headers[i].DataLength = nodes[i]->DataLength;
		headers[i].Sequence = nodes[i]->Sequence;
	}

	// Drop old tree (including Invalid nodes) before rebuilding.
	CdpPreviewTreeFree(Tree);
	CdpPreviewTreeInitialize(&rebuilt);

	CdpPreviewSortHeadersBySequence(headers, count);

	for (i = 0; i < count; ++i)
	{
		PUCHAR mask = NULL;
		ULONG len = headers[i].DataLength;
		ULONG idx;
		ULONG runStart;

		if (len == 0)
			continue;

		mask = (PUCHAR)cdpalloc(CdpBitmapByteCount(len));
		if (!mask)
		{
			status = STATUS_INSUFFICIENT_RESOURCES;
			CdpPreviewTreeFree(&rebuilt);
			goto cleanup;
		}
		RtlFillMemory(mask, CdpBitmapByteCount(len), 0xFF);
		CdpPreviewTreeClearMaskByTree(
			&rebuilt,
			headers[i].VolumeOffset,
			len,
			mask);

		idx = 0;
		while (idx < len)
		{
			while (idx < len && !CdpBitmapTest(mask, len, idx))
				++idx;
			if (idx >= len)
				break;
			runStart = idx;
			while (idx < len && CdpBitmapTest(mask, len, idx))
				++idx;

			{
				Cdp_JOURNAL_RECORD_HEADER frag = headers[i];
				frag.VolumeOffset =
					headers[i].VolumeOffset + runStart;
				frag.FileOffset =
					headers[i].FileOffset + runStart;
				frag.DataLength = idx - runStart;
				status = CdpPreviewTreeInsertRaw(&rebuilt, &frag);
		if (!NT_SUCCESS(status))
				{
					cdpfree(mask);
					CdpPreviewTreeFree(&rebuilt);
			goto cleanup;
				}
			}
		}
		cdpfree(mask);
	}

	*Tree = rebuilt;

	if (NT_SUCCESS(status))
		status = CdpPreviewTreeCoalesceAdjacent(Tree);

cleanup:
	if (nodes)
		cdpfree(nodes);
	if (headers)
		cdpfree(headers);
	return status;
}

NTSTATUS CdpPreviewTreeCoalesceAdjacent(
	_Inout_ PCdp_PREVIEW_TREE Tree)
{
	PCdp_PREVIEW_TREE_NODE* nodes = NULL;
	Cdp_JOURNAL_RECORD_HEADER* headers = NULL;
	ULONG count = 0;
	ULONG capacity;
	ULONG i;
	ULONG outCount = 0;
	NTSTATUS status = STATUS_SUCCESS;
	Cdp_PREVIEW_TREE rebuilt;

	if (!Tree)
		return STATUS_INVALID_PARAMETER;
	if (!Tree->Root || Tree->NodeCount == 0)
		return STATUS_SUCCESS;

	capacity = Tree->NodeCount;
	nodes = (PCdp_PREVIEW_TREE_NODE*)cdpalloc(
		sizeof(PCdp_PREVIEW_TREE_NODE) * capacity);
	headers = (Cdp_JOURNAL_RECORD_HEADER*)cdpalloc(
		sizeof(Cdp_JOURNAL_RECORD_HEADER) * capacity);
	if (!nodes || !headers)
	{
		status = STATUS_INSUFFICIENT_RESOURCES;
		goto cleanup;
	}

	CdpPreviewTreeCollectAllValid(Tree->Root, nodes, &count, capacity);
	if (count <= 1)
		goto cleanup;

	for (i = 0; i < count; ++i)
	{
		RtlZeroMemory(&headers[i], sizeof(headers[i]));
		headers[i].WallClock100ns = nodes[i]->WallClock100ns;
		headers[i].VolumeOffset = nodes[i]->Start;
		headers[i].FileOffset = nodes[i]->FileOffset;
		headers[i].DataLength = nodes[i]->DataLength;
		headers[i].Sequence = nodes[i]->Sequence;
	}

	// Sort by volume offset.
	for (i = 1; i < count; ++i)
	{
		Cdp_JOURNAL_RECORD_HEADER key = headers[i];
		ULONG j = i;
		while (j > 0 &&
			headers[j - 1].VolumeOffset > key.VolumeOffset)
		{
			headers[j] = headers[j - 1];
			--j;
		}
		headers[j] = key;
	}

	outCount = 0;
	for (i = 0; i < count; ++i)
	{
		Cdp_JOURNAL_RECORD_HEADER* cur = &headers[i];
		Cdp_JOURNAL_RECORD_HEADER* prev;

		if (cur->DataLength == 0)
			continue;
		if (outCount == 0)
		{
			headers[outCount++] = *cur;
			continue;
		}

		prev = &headers[outCount - 1];
		if (prev->VolumeOffset + prev->DataLength == cur->VolumeOffset &&
			prev->FileOffset + prev->DataLength == cur->FileOffset &&
			prev->DataLength <= MAXULONG - cur->DataLength)
		{
			// Contiguous on volume and in journal payload: merge.
			if (cur->Sequence < prev->Sequence)
			{
				prev->Sequence = cur->Sequence;
				prev->WallClock100ns = cur->WallClock100ns;
			}
			prev->DataLength += cur->DataLength;
		}
		else
		{
			headers[outCount++] = *cur;
		}
	}

	CdpPreviewTreeFree(Tree);
	CdpPreviewTreeInitialize(&rebuilt);
	for (i = 0; i < outCount; ++i)
	{
		status = CdpPreviewTreeInsertRaw(&rebuilt, &headers[i]);
		if (!NT_SUCCESS(status))
		{
			CdpPreviewTreeFree(&rebuilt);
			goto cleanup;
		}
	}
	*Tree = rebuilt;

cleanup:
	if (nodes)
		cdpfree(nodes);
	if (headers)
		cdpfree(headers);
	return status;
}

NTSTATUS CdpJournalBuildPreviewTree(
	_Inout_ PCdp_JOURNAL Journal,
	_In_ UINT64 TargetTime100ns,
	_In_ ULONG MaxSequence,
	_In_ BOOLEAN IncludeTargetTime,
	_Out_ PCdp_PREVIEW_TREE Tree)
{
	NTSTATUS status = STATUS_SUCCESS;
	UINT64 regionOff;
	ULONG guardRegions = 0;
	BOOLEAN stop = FALSE;
	Cdp_JOURNAL_RECORD_HEADER* headers = NULL;
	PVOID regionAllocationBase = NULL;
	PUCHAR region = NULL;
	ULONG headerCount = 0;
	ULONG headerCapacity = 0;
	ULONG i;

	if (!Tree)
		return STATUS_INVALID_PARAMETER;

	CdpPreviewTreeInitialize(Tree);

	Cdp_LOCK_ACQUIRE(&Journal->Lock);
	if (!Journal->Mounted)
	{
		status = STATUS_DEVICE_NOT_READY;
		goto cleanup_locked;
	}
	if (CdpJournalIsEmptyLocked(Journal) ||
		(IncludeTargetTime ?
			TargetTime100ns > Journal->Newest100ns :
			TargetTime100ns >= Journal->Newest100ns))
	{
		status = STATUS_SUCCESS;
		goto cleanup_locked;
	}
	if (TargetTime100ns < Journal->Oldest100ns)
	{
		status = STATUS_NOT_FOUND;
		goto cleanup_locked;
	}

	region = (PUCHAR)CdpAllocateAligned(
		Journal,
		Cdp_JOURNAL_HEADER_REGION_SIZE,
		&regionAllocationBase);
	if (!region)
	{
		status = STATUS_INSUFFICIENT_RESOURCES;
		goto cleanup_locked;
	}

	regionOff = Journal->LastHeaderRegionOff;
	for (;;)
	{
		Cdp_HEADER_REGION_LINK link;
		ULONG limit;
		LONG index;
		BOOLEAN isLast;
		BOOLEAN isOldest;

		if (++guardRegions > 100000UL)
		{
			status = STATUS_DISK_CORRUPT_ERROR;
			goto cleanup_locked;
		}

		status = CdpJournalReadHeaderRegion(Journal, regionOff, region);
		if (!NT_SUCCESS(status))
			goto cleanup_locked;
		RtlCopyMemory(
			&link,
			region + Cdp_JOURNAL_HEADER_REGION_SIZE -
				Cdp_JOURNAL_HEADER_LINK_SIZE,
			sizeof(link));
		if (link.Marker != Cdp_JOURNAL_HEADER_LINK_MARK)
		{
			status = STATUS_DISK_CORRUPT_ERROR;
			goto cleanup_locked;
		}

		isLast = (regionOff == Journal->LastHeaderRegionOff);
		isOldest = (regionOff == Journal->OldestHeaderRegionOff);
		limit = isLast ?
			Journal->CurrentHeaderCount : Cdp_JOURNAL_HEADERS_PER_REGION;

		for (index = (LONG)limit - 1; index >= 0; --index)
		{
			Cdp_JOURNAL_RECORD_HEADER header;
			ULONG startIndex = isOldest ? Journal->OldestHeaderIndex : 0;

			if ((ULONG)index < startIndex)
				break;

			RtlCopyMemory(
				&header,
				region + (ULONG)index *
					sizeof(Cdp_JOURNAL_RECORD_HEADER),
				sizeof(header));

			if (header.DataLength == 0 ||
				header.DataLength > Cdp_JOURNAL_MAX_RECORD_DATA ||
				header.VolumeOffset > MAXUINT64 - header.DataLength)
			{
				continue;
			}

			if (header.Sequence >= MaxSequence)
				continue;

			if (IncludeTargetTime ?
				header.WallClock100ns < TargetTime100ns :
				header.WallClock100ns <= TargetTime100ns)
			{
				stop = TRUE;
				break;
			}

			if (headerCount >= headerCapacity)
			{
				ULONG newCap = headerCapacity ? headerCapacity * 2UL : 256UL;
				Cdp_JOURNAL_RECORD_HEADER* grown;

				if (newCap < headerCount + 1)
					newCap = headerCount + 1;
				grown = (Cdp_JOURNAL_RECORD_HEADER*)cdpalloc(
					sizeof(Cdp_JOURNAL_RECORD_HEADER) * newCap);
				if (!grown)
				{
					status = STATUS_INSUFFICIENT_RESOURCES;
					goto cleanup_locked;
				}
				if (headers)
				{
	RtlCopyMemory(
						grown,
						headers,
						sizeof(Cdp_JOURNAL_RECORD_HEADER) * headerCount);
					cdpfree(headers);
				}
				headers = grown;
				headerCapacity = newCap;
			}

			headers[headerCount++] = header;
		}

		if (stop)
			break;
		if (isOldest)
			break;
		if (link.PrevRegionOff == regionOff)
			break;
		regionOff = link.PrevRegionOff;
	}

	status = STATUS_SUCCESS;

cleanup_locked:
	Cdp_LOCK_RELEASE(&Journal->Lock);
	if (!NT_SUCCESS(status))
		goto cleanup;

	// Insert oldest Sequence first; Insert skips already-covered bytes so the
	// tree never holds overlapping intervals (Preview + Recovery share this).
	if (headerCount > 0)
	{
		CdpPreviewSortHeadersBySequence(headers, headerCount);
		for (i = 0; i < headerCount; ++i)
		{
			status = CdpPreviewTreeInsert(Tree, &headers[i]);
			if (!NT_SUCCESS(status))
			{
				CdpPreviewTreeFree(Tree);
				break;
			}
		}
		if (NT_SUCCESS(status))
			status = CdpPreviewTreeCoalesceAdjacent(Tree);
		if (!NT_SUCCESS(status))
			CdpPreviewTreeFree(Tree);
	}

cleanup:
	if (regionAllocationBase)
		cdpfree(regionAllocationBase);
	if (headers)
		cdpfree(headers);
	return status;
}

NTSTATUS CdpJournalApplyPreviewTree(
	_Inout_ PCdp_JOURNAL Journal,
	_In_ PCdp_PREVIEW_TREE Tree,
	_Inout_ Cdp_LOCK* TreeLock,
	_In_ UINT64 VolumeOffset,
	_In_ ULONG DataLength,
	_Out_writes_bytes_(DataLength) PVOID Buffer,
	_Out_writes_bytes_((DataLength + 7) / 8) PUCHAR CoveredMask,
	_Out_ PULONG CoveredCount)
{
	NTSTATUS status = STATUS_SUCCESS;
	PCdp_PREVIEW_HIT hits = NULL;
	ULONG hitCount = 0;
	ULONG hitCapacity;
	ULONG covered = 0;
	ULONG i;
	BOOLEAN treeLocked = FALSE;

	if (!Tree || !TreeLock || !Buffer || !CoveredMask || !CoveredCount ||
		DataLength == 0 ||
		VolumeOffset > MAXUINT64 - DataLength)
	{
		return STATUS_INVALID_PARAMETER;
	}

	*CoveredCount = 0;
	RtlZeroMemory(CoveredMask, CdpBitmapByteCount(DataLength));

	Cdp_LOCK_ACQUIRE(TreeLock);
	treeLocked = TRUE;
	if (!Tree->Root || Tree->NodeCount == 0)
		goto cleanup;

	if (!Journal->Mounted)
	{
		status = STATUS_DEVICE_NOT_READY;
		goto cleanup;
	}

	hitCapacity = CdpPreviewTreeCountOverlaps(
		Tree->Root,
		VolumeOffset,
		VolumeOffset + DataLength);
	if (hitCapacity == 0)
		goto cleanup;
	hits = (PCdp_PREVIEW_HIT)cdpalloc(sizeof(Cdp_PREVIEW_HIT) * hitCapacity);
	if (!hits)
	{
		status = STATUS_INSUFFICIENT_RESOURCES;
		goto cleanup;
	}

	Cdp_JOURNAL_DIAG(
		"collect begin volumeOff=%llu len=%lu nodes=%lu\n",
		VolumeOffset,
		DataLength,
		Tree->NodeCount);
	CdpPreviewTreeCollectOverlaps(
		Tree->Root,
		VolumeOffset,
		VolumeOffset + DataLength,
		hits,
		&hitCount,
		hitCapacity);
	// Hits contain value copies.  The tree may now be modified or replaced
	// without keeping a mutex held across journal I/O.
	Cdp_LOCK_RELEASE(TreeLock);
	treeLocked = FALSE;
	Cdp_JOURNAL_DIAG(
		"collect end volumeOff=%llu len=%lu hits=%lu\n",
		VolumeOffset,
		DataLength,
		hitCount);
	CdpPreviewSortHitsBySequence(hits, hitCount);
	Cdp_JOURNAL_DIAG(
		"sort end volumeOff=%llu len=%lu hits=%lu\n",
		VolumeOffset,
		DataLength,
		hitCount);

	for (i = 0; i < hitCount && covered < DataLength; ++i)
	{
		PCdp_PREVIEW_TREE_NODE node = &hits[i].Node;
		PVOID payloadBase = NULL;
		PUCHAR payload;
		UINT64 alignedSize;
		UINT64 overlapStart;
		UINT64 overlapEnd;
		ULONG outputIndex;
		ULONG copyLength;

		Cdp_JOURNAL_DIAG(
			"hit begin index=%lu/%lu seq=%lu node=[%llu,%llu) "
			"dataLen=%lu fileOff=%llu invalid=%lu\n",
			i,
			hitCount,
			node->Sequence,
			node->Start,
			node->End,
			node->DataLength,
			node->FileOffset,
			node->Invalid ? 1UL : 0UL);
		alignedSize = CdpAlignUp64(node->DataLength, Journal->SectorSize);
		payload = (PUCHAR)CdpAllocateAligned(Journal,
			(SIZE_T)alignedSize,
			&payloadBase);
		if (!payload)
		{
			status = STATUS_INSUFFICIENT_RESOURCES;
			goto cleanup;
		}

		Cdp_JOURNAL_DIAG(
			"payload read begin index=%lu seq=%lu fileOff=%llu len=%lu\n",
			i,
			node->Sequence,
			node->FileOffset,
			(ULONG)alignedSize);
		status = CdpJournalRawIo(
			Journal,
			IRP_MJ_READ,
			node->FileOffset,
			(ULONG)alignedSize,
			payload);
		Cdp_JOURNAL_DIAG(
			"payload read end index=%lu seq=%lu status=0x%08X\n",
			i,
			node->Sequence,
			status);
		if (!NT_SUCCESS(status))
		{
			cdpfree(payloadBase);
			goto cleanup;
		}

		overlapStart = node->Start > VolumeOffset ? node->Start : VolumeOffset;
		overlapEnd = node->End < (VolumeOffset + DataLength) ?
			node->End : (VolumeOffset + DataLength);

		outputIndex = (ULONG)(overlapStart - VolumeOffset);
		copyLength = (ULONG)(overlapEnd - overlapStart);
		RtlCopyMemory(
			(PUCHAR)Buffer + outputIndex,
			payload + (ULONG)(overlapStart - node->Start),
			copyLength);
		CdpBitmapSetRange(CoveredMask, outputIndex, copyLength);
		covered += copyLength;
		cdpfree(payloadBase);
		Cdp_JOURNAL_DIAG(
			"hit end index=%lu seq=%lu covered=%lu\n",
			i,
			node->Sequence,
			covered);
	}

	*CoveredCount = covered;
	Cdp_JOURNAL_DIAG(
		"apply end volumeOff=%llu len=%lu hits=%lu covered=%lu "
		"status=0x%08X\n",
		VolumeOffset,
		DataLength,
		hitCount,
		covered,
		status);

cleanup:
	if (treeLocked)
		Cdp_LOCK_RELEASE(TreeLock);
	if (!NT_SUCCESS(status))
	{
		Cdp_JOURNAL_DIAG(
			"apply failed volumeOff=%llu len=%lu hits=%lu covered=%lu "
			"status=0x%08X\n",
			VolumeOffset,
			DataLength,
			hitCount,
			covered,
			status);
	}
	if (hits)
		cdpfree(hits);
	return status;
}

NTSTATUS CdpJournalReadPayload(
	_Inout_ PCdp_JOURNAL Journal,
	_In_ UINT64 FileOffset,
	_In_ ULONG DataLength,
	_Out_writes_bytes_(DataLength) PVOID Buffer)
{
	UINT64 currentOffset;
	ULONG remaining;
	PUCHAR output;
	NTSTATUS status = STATUS_SUCCESS;

	if (!Journal || !Journal->Mounted || !Buffer || DataLength == 0)
	{
		return STATUS_INVALID_PARAMETER;
	}

	// Coalesced preview/recovery nodes may span many adjacent records.  Keep
	// each raw I/O at the per-record maximum, but assemble the full logical
	// payload for the caller.
	currentOffset = FileOffset;
	remaining = DataLength;
	output = (PUCHAR)Buffer;
	while (remaining != 0)
	{
		ULONG chunk = remaining > Cdp_JOURNAL_MAX_RECORD_DATA ?
			Cdp_JOURNAL_MAX_RECORD_DATA : remaining;
		UINT64 alignedSize = CdpAlignUp64(chunk, Journal->SectorSize);
		PVOID allocationBase = NULL;
		PUCHAR payload = (PUCHAR)CdpAllocateAligned(
			Journal,
			(SIZE_T)alignedSize,
			&allocationBase);

		if (!payload)
			return STATUS_INSUFFICIENT_RESOURCES;

		status = CdpJournalRawIo(
			Journal,
			IRP_MJ_READ,
			currentOffset,
			(ULONG)alignedSize,
			payload);
		if (NT_SUCCESS(status))
			RtlCopyMemory(output, payload, chunk);
		cdpfree(allocationBase);
		if (!NT_SUCCESS(status))
			break;

		currentOffset += chunk;
		output += chunk;
		remaining -= chunk;
	}
	return status;
}

VOID CdpJournalClose(_Inout_ PCdp_JOURNAL Journal)
{
	Cdp_LOCK_ACQUIRE(&Journal->Lock);
	if (Journal->Mounted)
		(VOID)CdpJournalWriteSuperblockLocked(Journal);
	Journal->Mounted = FALSE;
	Journal->TargetDevice = NULL;
	Journal->Store = NULL;
	Cdp_LOCK_RELEASE(&Journal->Lock);
	Cdp_LOCK_DELETE(&Journal->Lock);
}
