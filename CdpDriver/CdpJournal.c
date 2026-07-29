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

// Caller holds Journal->Lock. A journal has at most one header scan in
// progress, so one aligned region buffer can be reused for its lifetime.
static NTSTATUS CdpJournalGetHeaderScanBufferLocked(
	_Inout_ PCdp_JOURNAL Journal,
	_Outptr_ PUCHAR* Buffer)
{
	if (!Journal || !Buffer)
		return STATUS_INVALID_PARAMETER;
	if (!Journal->HeaderScanBuffer)
	{
		Journal->HeaderScanBuffer = (PUCHAR)CdpAllocateAligned(
			Journal,
			Cdp_JOURNAL_HEADER_REGION_SIZE,
			&Journal->HeaderScanAllocationBase);
		if (!Journal->HeaderScanBuffer)
			return STATUS_INSUFFICIENT_RESOURCES;
	}
	*Buffer = Journal->HeaderScanBuffer;
	return STATUS_SUCCESS;
}

static UINT64 CdpJournalUsableStart(_In_ PCdp_JOURNAL Journal)
{
	return Journal->SectorSize;
}

static UINT64 CdpJournalUsableEnd(_In_ PCdp_JOURNAL Journal)
{
	return Journal->PartitionSize;
}

static NTSTATUS CdpJournalRingDistance(
	_In_ PCdp_JOURNAL Journal,
	_In_ UINT64 Start,
	_In_ UINT64 End,
	_Out_ PUINT64 Distance)
{
	UINT64 usableStart;
	UINT64 usableEnd;

	if (!Journal || !Distance)
		return STATUS_INVALID_PARAMETER;
	usableStart = CdpJournalUsableStart(Journal);
	usableEnd = CdpJournalUsableEnd(Journal);
	if (Start < usableStart || Start > usableEnd ||
		End < usableStart || End > usableEnd)
	{
		return STATUS_DISK_CORRUPT_ERROR;
	}
	if (End >= Start)
		*Distance = End - Start;
	else
		*Distance = (usableEnd - Start) + (End - usableStart);
	return STATUS_SUCCESS;
}

static BOOLEAN CdpJournalHeaderRegionOffsetValid(
	_In_ PCdp_JOURNAL Journal,
	_In_ UINT64 RegionOff)
{
	return RegionOff >= CdpJournalUsableStart(Journal) &&
		RegionOff <= CdpJournalUsableEnd(Journal) -
			Cdp_JOURNAL_HEADER_REGION_SIZE &&
		(RegionOff % Journal->SectorSize) == 0;
}

static BOOLEAN CdpJournalRegionLinkValid(
	_In_ PCdp_JOURNAL Journal,
	_In_ const Cdp_HEADER_REGION_LINK* Link)
{
	return Link && Link->Reserved == 0 && Link->StartSequence != 0 &&
		CdpJournalHeaderRegionOffsetValid(Journal, Link->PrevRegionOff) &&
		CdpJournalHeaderRegionOffsetValid(Journal, Link->NextRegionOff);
}

static NTSTATUS CdpJournalDecodeRecord(
	_In_ const Cdp_HEADER_REGION_LINK* Link,
	_In_ const Cdp_JOURNAL_RECORD_HEADER* Header,
	_Out_ PCdp_JOURNAL_RECORD Record)
{
	if (!Link || !Header || !Record ||
		Link->StartSequence > MAXUINT64 - Header->Sequence)
	{
		return STATUS_INTEGER_OVERFLOW;
	}
	RtlZeroMemory(Record, sizeof(*Record));
	Record->WallClock100ns = Header->WallClock100ns;
	Record->VolumeOffset = Header->VolumeOffset;
	Record->FileOffset = Header->FileOffset;
	Record->Sequence = Link->StartSequence + Header->Sequence;
	Record->DataLength = Header->DataLength;
	return STATUS_SUCCESS;
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
	superblock->Flags = Journal->RecoveryPending ?
		Cdp_JOURNAL_FLAG_RECOVERY_PENDING : 0;
	superblock->PartitionSize = Journal->PartitionSize;
	superblock->LastHeaderRegionOff = Journal->LastHeaderRegionOff;
	superblock->SourceVolumeGuid = Journal->SourceVolumeGuid;
	superblock->RecoveryTargetTime100ns = Journal->RecoveryTargetTime100ns;
	superblock->Crc32c = CdpCrc32c(
		0,
		superblock,
		FIELD_OFFSET(Cdp_JOURNAL_SUPERBLOCK, Crc32c));
	superblock->RecoveryCrc32c = CdpCrc32c(
		0,
		superblock,
		FIELD_OFFSET(Cdp_JOURNAL_SUPERBLOCK, RecoveryCrc32c));

	status = CdpJournalRawIo(
		Journal,
		IRP_MJ_WRITE,
		0,
		Journal->SectorSize,
		sector);
	cdpfree(allocationBase);
	if (NT_SUCCESS(status))
		Journal->SuperblockDirty = FALSE;
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
	if ((Superblock->Flags & Cdp_JOURNAL_FLAG_RECOVERY_PENDING) != 0 &&
		(Superblock->RecoveryTargetTime100ns == 0 ||
		 CdpCrc32c(0, Superblock,
			FIELD_OFFSET(Cdp_JOURNAL_SUPERBLOCK, RecoveryCrc32c)) !=
			Superblock->RecoveryCrc32c))
	{
		return FALSE;
	}

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

static NTSTATUS CdpJournalReadRegionLink(
	_In_ PCdp_JOURNAL Journal,
	_In_ UINT64 RegionOff,
	_Out_ PCdp_HEADER_REGION_LINK Link)
{
	PVOID allocationBase = NULL;
	PUCHAR sector;
	NTSTATUS status;

	if (!Journal || !Link || Journal->SectorSize < sizeof(*Link))
		return STATUS_INVALID_PARAMETER;
	sector = (PUCHAR)CdpAllocateAligned(
		Journal,
		Journal->SectorSize,
		&allocationBase);
	if (!sector)
		return STATUS_INSUFFICIENT_RESOURCES;

	status = CdpJournalRawIo(
		Journal,
		IRP_MJ_READ,
		RegionOff + Cdp_JOURNAL_HEADER_REGION_SIZE - Journal->SectorSize,
		Journal->SectorSize,
		sector);
	if (NT_SUCCESS(status))
	{
		RtlCopyMemory(
			Link,
			sector + Journal->SectorSize - sizeof(*Link),
			sizeof(*Link));
	}
	cdpfree(allocationBase);
	return status;
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
	_In_ UINT64 NextOff,
	_In_ UINT64 StartSequence)
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
	link->PrevRegionOff = PrevOff;
	link->NextRegionOff = NextOff;
	link->StartSequence = StartSequence;
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
	PVOID allocationBase = NULL;
	PUCHAR sector;
	UINT64 relativeOffset;
	UINT64 sectorOffset;
	NTSTATUS status;

	if (Index >= Cdp_JOURNAL_HEADERS_PER_REGION)
		return STATUS_INVALID_PARAMETER;
	relativeOffset = (UINT64)Index * sizeof(*Header);
	sectorOffset = (relativeOffset / Journal->SectorSize) * Journal->SectorSize;
	sector = (PUCHAR)CdpAllocateAligned(
		Journal,
		Journal->SectorSize,
		&allocationBase);
	if (!sector)
		return STATUS_INSUFFICIENT_RESOURCES;
	status = CdpJournalRawIo(
		Journal,
		IRP_MJ_READ,
		RegionOff + sectorOffset,
		Journal->SectorSize,
		sector);
	if (NT_SUCCESS(status))
	{
		RtlCopyMemory(
			Header,
			sector + (relativeOffset - sectorOffset),
			sizeof(*Header));
	}
	cdpfree(allocationBase);
	return status;
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

static NTSTATUS CdpJournalDropOldestRegionLocked(
	_Inout_ PCdp_JOURNAL Journal)
{
	Cdp_HEADER_REGION_LINK link;
	Cdp_HEADER_REGION_LINK nextLink;
	UINT64 regionOff;
	UINT64 reclaimedBytes = 0;
	UINT64 reclaimedRecords = 0;
	ULONG limit;
	NTSTATUS status;

	if (CdpJournalIsEmptyLocked(Journal))
		return STATUS_NOT_FOUND;

	regionOff = Journal->OldestHeaderRegionOff;
	limit = CdpJournalRegionHeaderLimit(
		Journal,
		regionOff);
	if (Journal->OldestHeaderIndex >= limit)
		return STATUS_DISK_CORRUPT_ERROR;

	status = CdpJournalReadRegionLink(Journal, regionOff, &link);
	if (!NT_SUCCESS(status) || !CdpJournalRegionLinkValid(Journal, &link))
		return STATUS_DISK_CORRUPT_ERROR;

	if (regionOff == Journal->LastHeaderRegionOff)
	{
		reclaimedRecords = limit - Journal->OldestHeaderIndex;
		reclaimedBytes = Journal->PayloadBytesUsed;
		if (reclaimedRecords != Journal->TotalRecords)
			return STATUS_DISK_CORRUPT_ERROR;
	}
	else
	{
		UINT64 firstSequence;

		if (link.NextRegionOff == regionOff ||
			link.StartSequence > MAXUINT64 - Journal->OldestHeaderIndex)
		{
			return STATUS_DISK_CORRUPT_ERROR;
		}
		status = CdpJournalReadRegionLink(
			Journal,
			link.NextRegionOff,
			&nextLink);
		if (!NT_SUCCESS(status) ||
			!CdpJournalRegionLinkValid(Journal, &nextLink) ||
			nextLink.PrevRegionOff != regionOff)
		{
			return STATUS_DISK_CORRUPT_ERROR;
		}
		firstSequence = link.StartSequence + Journal->OldestHeaderIndex;
		if (nextLink.StartSequence <= firstSequence)
			return STATUS_DISK_CORRUPT_ERROR;
		reclaimedRecords = nextLink.StartSequence - firstSequence;
		if (reclaimedRecords != limit - Journal->OldestHeaderIndex)
			return STATUS_DISK_CORRUPT_ERROR;
		status = CdpJournalRingDistance(
			Journal,
			regionOff + Cdp_JOURNAL_HEADER_REGION_SIZE,
			link.NextRegionOff,
			&reclaimedBytes);
		if (!NT_SUCCESS(status))
			return status;
	}

	if (reclaimedRecords > Journal->TotalRecords ||
		reclaimedBytes > Journal->PayloadBytesUsed)
		return STATUS_DISK_CORRUPT_ERROR;

	Journal->TotalRecords -= reclaimedRecords;
	Journal->PayloadBytesUsed -= reclaimedBytes;
	CdpJournalAdvanceRecordGenerationLocked(Journal);

	if (regionOff == Journal->LastHeaderRegionOff)
	{
		if (Journal->TotalRecords != 0 || Journal->PayloadBytesUsed != 0)
			return STATUS_DISK_CORRUPT_ERROR;
		// The active region is about to be reused from header[0]. Clear stale
		// headers and start a new local-sequence epoch at the next global id.
		status = CdpJournalInitHeaderRegion(
			Journal,
			Journal->LastHeaderRegionOff,
			Journal->LastHeaderRegionOff,
			Journal->LastHeaderRegionOff,
			Journal->NextSequence);
		if (!NT_SUCCESS(status))
			return status;
		Journal->OldestHeaderRegionOff = Journal->LastHeaderRegionOff;
		Journal->CurrentHeaderRegionStartSequence = Journal->NextSequence;
		Journal->OldestHeaderIndex = 0;
		Journal->CurrentHeaderCount = 0;
		Journal->PayloadRegionOff =
			Journal->LastHeaderRegionOff + Cdp_JOURNAL_HEADER_REGION_SIZE;
		Journal->Oldest100ns = 0;
		Journal->Newest100ns = 0;
		return STATUS_SUCCESS;
	}

	// Persist the logical deletion: Mount walking backwards from the newest
	// region must stop at the new oldest region, not rediscover stale headers.
	nextLink.PrevRegionOff = link.NextRegionOff;
	status = CdpJournalWriteRegionLink(
		Journal,
		link.NextRegionOff,
		&nextLink);
	if (!NT_SUCCESS(status))
		return status;

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
		{
			UINT64 skipped = usableEnd - Journal->PayloadRegionOff;
			if (Journal->PayloadBytesUsed > MAXUINT64 - skipped)
				return STATUS_INTEGER_OVERFLOW;
			Journal->PayloadBytesUsed += skipped;
			Journal->PayloadRegionOff = usableStart;
		}

		if (CdpJournalContiguousFreeLocked(Journal) >= BytesNeeded)
			return STATUS_SUCCESS;

		status = CdpJournalDropOldestRegionLocked(Journal);
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

		status = CdpJournalDropOldestRegionLocked(Journal);
		if (!NT_SUCCESS(status))
			return status;
		if (++guard > 1000000UL)
			return STATUS_DISK_CORRUPT_ERROR;
	}

	if (candidate + Cdp_JOURNAL_HEADER_REGION_SIZE > usableEnd)
		return STATUS_INSUFFICIENT_RESOURCES;

	oldRegion = Journal->LastHeaderRegionOff;
	status = CdpJournalReadRegionLink(Journal, oldRegion, &oldLink);
	if (!NT_SUCCESS(status) || !CdpJournalRegionLinkValid(Journal, &oldLink))
		return STATUS_DISK_CORRUPT_ERROR;

	status = CdpJournalInitHeaderRegion(
		Journal,
		candidate,
		oldRegion,
		candidate,
		Journal->NextSequence);
	if (!NT_SUCCESS(status))
		return status;

	oldLink.NextRegionOff = candidate;
	status = CdpJournalWriteRegionLink(Journal, oldRegion, &oldLink);
	if (!NT_SUCCESS(status))
		return status;

	newLink.PrevRegionOff = oldRegion;
	newLink.NextRegionOff = candidate;
	newLink.StartSequence = Journal->NextSequence;
	newLink.Reserved = 0;
	status = CdpJournalWriteRegionLink(Journal, candidate, &newLink);
	if (!NT_SUCCESS(status))
		return status;

	Journal->LastHeaderRegionOff = candidate;
	Journal->CurrentHeaderRegionStartSequence = Journal->NextSequence;
	Journal->SuperblockDirty = TRUE;
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
	UINT64 expectedSequence = 0;
	BOOLEAN haveExpectedSequence = FALSE;
	NTSTATUS status = STATUS_SUCCESS;
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
		if (!NT_SUCCESS(status) || !CdpJournalRegionLinkValid(Journal, &link))
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
	status = CdpJournalGetHeaderScanBufferLocked(Journal, &region);
	if (!NT_SUCCESS(status))
	{
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
		if (!CdpJournalRegionLinkValid(Journal, &link))
		{
			status = STATUS_DISK_CORRUPT_ERROR;
			goto cleanup;
		}
		if (haveExpectedSequence && link.StartSequence != expectedSequence)
		{
			status = STATUS_DISK_CORRUPT_ERROR;
			goto cleanup;
		}
		if (isLast)
		{
			Journal->CurrentHeaderRegionStartSequence = link.StartSequence;
			Journal->NextSequence = link.StartSequence;
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
				if (!isLast)
				{
					status = STATUS_DISK_CORRUPT_ERROR;
					goto cleanup;
				}
				Journal->CurrentHeaderCount = index;
				break;
			}
			if (header.DataLength == 0 ||
				header.Sequence != index ||
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
			if (link.StartSequence > MAXUINT64 - header.Sequence ||
				link.StartSequence + header.Sequence == MAXUINT64)
			{
				status = STATUS_INTEGER_OVERFLOW;
				goto cleanup;
			}
			expectedSequence = link.StartSequence + header.Sequence + 1;
			haveExpectedSequence = TRUE;
			if (isLast)
				Journal->NextSequence = expectedSequence;

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

		{
			UINT64 payloadBytes;
			UINT64 payloadEnd = isLast ?
				Journal->PayloadRegionOff : link.NextRegionOff;

			status = CdpJournalRingDistance(
				Journal,
				regionOff + Cdp_JOURNAL_HEADER_REGION_SIZE,
				payloadEnd,
				&payloadBytes);
			if (!NT_SUCCESS(status) ||
				Journal->PayloadBytesUsed > MAXUINT64 - payloadBytes)
			{
				status = STATUS_DISK_CORRUPT_ERROR;
				goto cleanup;
			}
			Journal->PayloadBytesUsed += payloadBytes;
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
		headerOff,
		1);
	if (!NT_SUCCESS(status))
		goto cleanup;

	// v8 has no backup superblock. Explicitly erase a stale legacy backup when
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
	Journal->CurrentHeaderRegionStartSequence = 1;
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

	Journal->SuperblockDirty = TRUE;
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
	Journal->RecoveryPending =
		(superblock->Flags & Cdp_JOURNAL_FLAG_RECOVERY_PENDING) != 0;
	Journal->RecoveryTargetTime100ns = Journal->RecoveryPending ?
		superblock->RecoveryTargetTime100ns : 0;
	Journal->SuperblockDirty = FALSE;

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

NTSTATUS CdpJournalSetRecoveryIntent(
	_Inout_ PCdp_JOURNAL Journal,
	_In_ UINT64 TargetTime100ns)
{
	NTSTATUS status;

	if (!Journal || TargetTime100ns == 0)
		return STATUS_INVALID_PARAMETER;
	Cdp_LOCK_ACQUIRE(&Journal->Lock);
	if (!Journal->Mounted)
	{
		status = STATUS_DEVICE_NOT_READY;
		goto done;
	}
	Journal->RecoveryPending = TRUE;
	Journal->RecoveryTargetTime100ns = TargetTime100ns;
	Journal->SuperblockDirty = TRUE;
	status = CdpJournalWriteSuperblockLocked(Journal);
	if (NT_SUCCESS(status))
		status = CdpJournalFlush(Journal);
done:
	Cdp_LOCK_RELEASE(&Journal->Lock);
	return status;
}

NTSTATUS CdpJournalClearRecoveryIntent(_Inout_ PCdp_JOURNAL Journal)
{
	NTSTATUS status;

	if (!Journal)
		return STATUS_INVALID_PARAMETER;
	Cdp_LOCK_ACQUIRE(&Journal->Lock);
	if (!Journal->Mounted)
	{
		status = STATUS_DEVICE_NOT_READY;
		goto done;
	}
	Journal->RecoveryPending = FALSE;
	Journal->RecoveryTargetTime100ns = 0;
	Journal->SuperblockDirty = TRUE;
	status = CdpJournalWriteSuperblockLocked(Journal);
	if (NT_SUCCESS(status))
		status = CdpJournalFlush(Journal);
done:
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
	_Out_opt_ PCdp_JOURNAL_RECORD WrittenRecord)
{
	Cdp_JOURNAL_RECORD_HEADER header;
	Cdp_JOURNAL_RECORD record;
	UINT64 payloadOff;
	UINT64 alignedSize;
	PVOID allocationBase = NULL;
	PUCHAR payloadBuffer = NULL;
	UINT64 writeSeq;
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
	if (Journal->NextSequence == MAXUINT64 ||
		Journal->CurrentHeaderRegionStartSequence >
			MAXUINT64 - Journal->CurrentHeaderCount ||
		Journal->CurrentHeaderRegionStartSequence +
			Journal->CurrentHeaderCount != Journal->NextSequence)
	{
		status = STATUS_INTEGER_OVERFLOW;
		goto cleanup;
	}

	// Payload: wrap at partition end and/or drop complete oldest header
	// regions until there is room.
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
	header.Sequence = Journal->CurrentHeaderCount;

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

	if (WrittenRecord)
	{
		RtlZeroMemory(&record, sizeof(record));
		record.WallClock100ns = header.WallClock100ns;
		record.VolumeOffset = header.VolumeOffset;
		record.FileOffset = header.FileOffset;
		record.Sequence = writeSeq;
		record.DataLength = header.DataLength;
		*WrittenRecord = record;
	}

	if (Journal->SuperblockDirty)
	{
		// Make the new region, link, payload and record header durable before
		// publishing the region through the superblock.
		status = CdpJournalFlush(Journal);
		if (NT_SUCCESS(status))
			status = CdpJournalWriteSuperblockLocked(Journal);
		if (NT_SUCCESS(status))
			status = CdpJournalFlush(Journal);
	}
	else
	{
		status = CdpJournalFlush(Journal);
	}

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
		if (!CdpJournalRegionLinkValid(Journal, &link) ||
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
	_Out_writes_to_(RecordCapacity, *ReturnedCount) PCdp_JOURNAL_RECORD Records,
	_In_ ULONG RecordCapacity,
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
	Cdp_HEADER_REGION_LINK link;
	NTSTATUS status;

	if (!Journal || !TotalRecords || !Generation || !ReturnedCount ||
		(RecordCapacity != 0 && !Records))
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
	if (StartIndex == Journal->TotalRecords || RecordCapacity == 0)
	{
		status = STATUS_SUCCESS;
		goto cleanup;
	}

	remaining = Journal->TotalRecords - StartIndex;
	wanted = remaining < RecordCapacity ? (ULONG)remaining : RecordCapacity;
	regionOff = Journal->OldestHeaderRegionOff;
	headerIndex = Journal->OldestHeaderIndex;
	remaining = StartIndex;
	status = CdpJournalReadRegionLink(Journal, regionOff, &link);
	if (!NT_SUCCESS(status) || !CdpJournalRegionLinkValid(Journal, &link))
	{
		status = STATUS_DISK_CORRUPT_ERROR;
		goto cleanup;
	}

	while (remaining != 0)
	{
		ULONG limit = CdpJournalRegionHeaderLimit(Journal, regionOff);
		ULONG available;
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
		if (!CdpJournalRegionLinkValid(Journal, &link) ||
			link.NextRegionOff == regionOff || ++guard > 100000UL)
		{
			status = STATUS_DISK_CORRUPT_ERROR;
			goto cleanup;
		}
		regionOff = link.NextRegionOff;
		headerIndex = 0;
		status = CdpJournalReadRegionLink(Journal, regionOff, &link);
		if (!NT_SUCCESS(status) || !CdpJournalRegionLinkValid(Journal, &link))
		{
			status = STATUS_DISK_CORRUPT_ERROR;
			goto cleanup;
		}
	}

	while (returned < wanted)
	{
		ULONG limit = CdpJournalRegionHeaderLimit(Journal, regionOff);
		Cdp_JOURNAL_RECORD_HEADER header;
		Cdp_JOURNAL_RECORD record;

		if (headerIndex >= limit)
		{
			if (regionOff == Journal->LastHeaderRegionOff)
			{
				status = STATUS_DISK_CORRUPT_ERROR;
				goto cleanup;
			}
			status = CdpJournalReadRegionLink(Journal, regionOff, &link);
			if (!NT_SUCCESS(status))
				goto cleanup;
			if (!CdpJournalRegionLinkValid(Journal, &link) ||
				link.NextRegionOff == regionOff || ++guard > 100000UL)
			{
				status = STATUS_DISK_CORRUPT_ERROR;
				goto cleanup;
			}
			regionOff = link.NextRegionOff;
			headerIndex = 0;
			status = CdpJournalReadRegionLink(Journal, regionOff, &link);
			if (!NT_SUCCESS(status) || !CdpJournalRegionLinkValid(Journal, &link))
			{
				status = STATUS_DISK_CORRUPT_ERROR;
				goto cleanup;
			}
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
			header.Sequence != headerIndex ||
			header.DataLength > Cdp_JOURNAL_MAX_RECORD_DATA)
		{
			status = STATUS_DISK_CORRUPT_ERROR;
			goto cleanup;
		}

		status = CdpJournalDecodeRecord(&link, &header, &record);
		if (!NT_SUCCESS(status))
			goto cleanup;
		Records[returned++] = record;
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
	UINT64 minValidSequence = Node->Invalid ? MAXUINT64 : Node->Sequence;

	Node->Height = 1 + (hl > hr ? hl : hr);
	if (Node->Left && Node->Left->MaxEnd > maxEnd)
		maxEnd = Node->Left->MaxEnd;
	if (Node->Right && Node->Right->MaxEnd > maxEnd)
		maxEnd = Node->Right->MaxEnd;
	Node->MaxEnd = maxEnd;
	if (Node->Left && Node->Left->MinValidSequence < minValidSequence)
		minValidSequence = Node->Left->MinValidSequence;
	if (Node->Right && Node->Right->MinValidSequence < minValidSequence)
		minValidSequence = Node->Right->MinValidSequence;
	Node->MinValidSequence = minValidSequence;
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
	_In_ const Cdp_JOURNAL_RECORD* Record)
{
	PCdp_PREVIEW_TREE_NODE node;

	if (!Tree || !Record)
		return STATUS_INVALID_PARAMETER;
	if (Record->DataLength == 0)
		return STATUS_SUCCESS;

	node = (PCdp_PREVIEW_TREE_NODE)cdpalloc(sizeof(*node));
	if (!node)
		return STATUS_INSUFFICIENT_RESOURCES;

	RtlZeroMemory(node, sizeof(*node));
	node->Start = Record->VolumeOffset;
	node->End = Record->VolumeOffset + Record->DataLength;
	node->MaxEnd = node->End;
	node->FileOffset = Record->FileOffset;
	node->WallClock100ns = Record->WallClock100ns;
	node->DataLength = Record->DataLength;
	node->Sequence = Record->Sequence;
	node->MinValidSequence = Record->Sequence;
	node->Height = 1;
	node->Invalid = FALSE;

	Tree->Root = CdpPreviewTreeAvlInsertNode(Tree->Root, node);
	Tree->NodeCount++;
	return STATUS_SUCCESS;
}

static PCdp_PREVIEW_TREE_NODE CdpPreviewTreeAvlRebalance(
	_Inout_ PCdp_PREVIEW_TREE_NODE Root)
{
	LONG balance;

	if (!Root)
		return NULL;
	CdpPreviewTreeNodeUpdate(Root);
	balance = CdpPreviewTreeNodeHeight(Root->Left) -
		CdpPreviewTreeNodeHeight(Root->Right);
	if (balance > 1)
	{
		if (CdpPreviewTreeNodeHeight(Root->Left->Left) <
			CdpPreviewTreeNodeHeight(Root->Left->Right))
		{
			Root->Left = CdpPreviewTreeRotateLeft(Root->Left);
		}
		return CdpPreviewTreeRotateRight(Root);
	}
	if (balance < -1)
	{
		if (CdpPreviewTreeNodeHeight(Root->Right->Right) <
			CdpPreviewTreeNodeHeight(Root->Right->Left))
		{
			Root->Right = CdpPreviewTreeRotateRight(Root->Right);
		}
		return CdpPreviewTreeRotateLeft(Root);
	}
	return Root;
}

static PCdp_PREVIEW_TREE_NODE CdpPreviewTreeAvlMinimum(
	_In_ PCdp_PREVIEW_TREE_NODE Root)
{
	while (Root->Left)
		Root = Root->Left;
	return Root;
}

static VOID CdpPreviewTreeCopyNodeData(
	_Inout_ PCdp_PREVIEW_TREE_NODE Dest,
	_In_ const PCdp_PREVIEW_TREE_NODE Source)
{
	Dest->Start = Source->Start;
	Dest->End = Source->End;
	Dest->FileOffset = Source->FileOffset;
	Dest->WallClock100ns = Source->WallClock100ns;
	Dest->DataLength = Source->DataLength;
	Dest->Sequence = Source->Sequence;
	Dest->Invalid = Source->Invalid;
}

static PCdp_PREVIEW_TREE_NODE CdpPreviewTreeAvlDeleteByStart(
	_In_opt_ PCdp_PREVIEW_TREE_NODE Root,
	_In_ UINT64 Start,
	_Out_ PBOOLEAN Removed)
{
	if (!Root)
		return NULL;
	if (Start < Root->Start)
	{
		Root->Left = CdpPreviewTreeAvlDeleteByStart(
			Root->Left, Start, Removed);
	}
	else if (Start > Root->Start)
	{
		Root->Right = CdpPreviewTreeAvlDeleteByStart(
			Root->Right, Start, Removed);
	}
	else if (!Root->Left || !Root->Right)
	{
		PCdp_PREVIEW_TREE_NODE child = Root->Left ? Root->Left : Root->Right;
		cdpfree(Root);
		*Removed = TRUE;
		return child;
	}
	else
	{
		PCdp_PREVIEW_TREE_NODE successor =
			CdpPreviewTreeAvlMinimum(Root->Right);
		UINT64 successorStart = successor->Start;

		CdpPreviewTreeCopyNodeData(Root, successor);
		Root->Right = CdpPreviewTreeAvlDeleteByStart(
			Root->Right, successorStart, Removed);
	}
	return CdpPreviewTreeAvlRebalance(Root);
}

static PCdp_PREVIEW_TREE_NODE CdpPreviewTreeFindFirstOverlap(
	_In_opt_ PCdp_PREVIEW_TREE_NODE Node,
	_In_ UINT64 QueryStart,
	_In_ UINT64 QueryEnd)
{
	PCdp_PREVIEW_TREE_NODE hit;

	if (!Node || Node->MaxEnd <= QueryStart)
		return NULL;
	if (Node->Left)
	{
		hit = CdpPreviewTreeFindFirstOverlap(
			Node->Left, QueryStart, QueryEnd);
		if (hit)
			return hit;
	}
	if (!Node->Invalid && Node->Start < QueryEnd && Node->End > QueryStart)
		return Node;
	if (Node->Start < QueryEnd && Node->Right)
		return CdpPreviewTreeFindFirstOverlap(
			Node->Right, QueryStart, QueryEnd);
	return NULL;
}

// Remove a byte range without rebuilding the tree.  Only overlapping nodes
// are deleted; their unaffected left/right fragments are reinserted with the
// original payload offsets.  The caller serializes access to Tree.
static NTSTATUS CdpPreviewTreeRemoveRangeInPlace(
	_Inout_ PCdp_PREVIEW_TREE Tree,
	_In_ UINT64 CutStart,
	_In_ UINT64 CutEnd)
{
	NTSTATUS status;

	if (!Tree || CutStart >= CutEnd)
		return STATUS_INVALID_PARAMETER;

	for (;;)
	{
		PCdp_PREVIEW_TREE_NODE overlap = CdpPreviewTreeFindFirstOverlap(
			Tree->Root, CutStart, CutEnd);
		Cdp_JOURNAL_RECORD saved;
		BOOLEAN removed = FALSE;

		if (!overlap)
			break;
		RtlZeroMemory(&saved, sizeof(saved));
		saved.WallClock100ns = overlap->WallClock100ns;
		saved.VolumeOffset = overlap->Start;
		saved.FileOffset = overlap->FileOffset;
		saved.DataLength = overlap->DataLength;
		saved.Sequence = overlap->Sequence;

		Tree->Root = CdpPreviewTreeAvlDeleteByStart(
			Tree->Root, overlap->Start, &removed);
		if (!removed)
			return STATUS_DISK_CORRUPT_ERROR;
		Tree->NodeCount--;

		if (saved.VolumeOffset < CutStart)
		{
			Cdp_JOURNAL_RECORD left = saved;
			left.DataLength = (ULONG)(CutStart - saved.VolumeOffset);
			status = CdpPreviewTreeInsertRaw(Tree, &left);
			if (!NT_SUCCESS(status))
				return status;
		}
		if (saved.VolumeOffset + saved.DataLength > CutEnd)
		{
			Cdp_JOURNAL_RECORD right = saved;
			right.VolumeOffset = CutEnd;
			right.FileOffset = saved.FileOffset +
				(CutEnd - saved.VolumeOffset);
			right.DataLength = (ULONG)(saved.VolumeOffset +
				saved.DataLength - CutEnd);
			status = CdpPreviewTreeInsertRaw(Tree, &right);
			if (!NT_SUCCESS(status))
				return status;
		}
	}
	return STATUS_SUCCESS;
}

// Build scans journal headers newest-to-oldest.  A newly scanned header is
// therefore an earlier before-image and must replace newer overlapping bytes.
// Remove overlaps in-place, preserve their non-overlapping fragments, then
// insert the complete earlier header.  No header array or tree-wide rebuild.
static NTSTATUS CdpPreviewTreeOverlayEarlier(
	_Inout_ PCdp_PREVIEW_TREE Tree,
	_In_ const Cdp_JOURNAL_RECORD* Record)
{
	NTSTATUS status;
	UINT64 cutEnd;

	if (!Tree || !Record || Record->DataLength == 0 ||
		Record->VolumeOffset > MAXUINT64 - Record->DataLength)
	{
		return STATUS_INVALID_PARAMETER;
	}
	cutEnd = Record->VolumeOffset + Record->DataLength;
	status = CdpPreviewTreeRemoveRangeInPlace(
		Tree, Record->VolumeOffset, cutEnd);
	if (!NT_SUCCESS(status))
		return status;

	return CdpPreviewTreeInsertRaw(Tree, Record);
}

static ULONG CdpBitmapByteCount(_In_ ULONG BitCount)
{
	return (BitCount + 7UL) / 8UL;
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

NTSTATUS CdpPreviewTreeInsert(
	_Inout_ PCdp_PREVIEW_TREE Tree,
	_In_ const Cdp_JOURNAL_RECORD* Record)
{
	UINT64 cursor;
	UINT64 end;
	NTSTATUS status = STATUS_SUCCESS;

	if (!Tree || !Record)
		return STATUS_INVALID_PARAMETER;
	if (Record->DataLength == 0)
		return STATUS_SUCCESS;
	if (Record->VolumeOffset > MAXUINT64 - Record->DataLength)
		return STATUS_INVALID_PARAMETER;

	// Empty tree: raw insert.
	if (!Tree->Root)
		return CdpPreviewTreeInsertRaw(Tree, Record);

	cursor = Record->VolumeOffset;
	end = cursor + Record->DataLength;
	while (cursor < end)
	{
		PCdp_PREVIEW_TREE_NODE overlap = CdpPreviewTreeFindFirstOverlap(
			Tree->Root, cursor, end);
		UINT64 overlapStart;
		UINT64 overlapEnd;

		if (!overlap)
		{
			Cdp_JOURNAL_RECORD frag = *Record;
			frag.VolumeOffset = cursor;
			frag.FileOffset = Record->FileOffset +
				(cursor - Record->VolumeOffset);
			frag.DataLength = (ULONG)(end - cursor);
			status = CdpPreviewTreeInsertRaw(Tree, &frag);
			break;
		}

		// Save scalar values before insertion because AVL rotations may move
		// the overlap node. Existing valid nodes retain priority over Header.
		overlapStart = overlap->Start;
		overlapEnd = overlap->End;
		if (overlapStart > cursor)
		{
			Cdp_JOURNAL_RECORD frag = *Record;
			UINT64 gapEnd = overlapStart < end ? overlapStart : end;
			frag.VolumeOffset = cursor;
			frag.FileOffset = Record->FileOffset +
				(cursor - Record->VolumeOffset);
			frag.DataLength = (ULONG)(gapEnd - cursor);
			status = CdpPreviewTreeInsertRaw(Tree, &frag);
			if (!NT_SUCCESS(status))
				break;
		}
		if (overlapEnd <= cursor)
			return STATUS_DISK_CORRUPT_ERROR;
		cursor = overlapEnd < end ? overlapEnd : end;
	}
	return status;
}

static NTSTATUS CdpPreviewTreeMergeNode(
	_Inout_ PCdp_PREVIEW_TREE Dest,
	_In_opt_ PCdp_PREVIEW_TREE_NODE Node)
{
	Cdp_JOURNAL_RECORD record;
	NTSTATUS status;

	if (!Node)
		return STATUS_SUCCESS;

	status = CdpPreviewTreeMergeNode(Dest, Node->Left);
	if (!NT_SUCCESS(status))
		return status;

	RtlZeroMemory(&record, sizeof(record));
	record.WallClock100ns = Node->WallClock100ns;
	record.VolumeOffset = Node->Start;
	record.FileOffset = Node->FileOffset;
	record.DataLength = Node->DataLength;
	record.Sequence = Node->Sequence;
	status = CdpPreviewTreeInsert(Dest, &record);
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

static PCdp_PREVIEW_TREE_NODE CdpPreviewTreeMarkInvalidNodeByStart(
	_Inout_opt_ PCdp_PREVIEW_TREE_NODE Node,
	_In_ UINT64 VolumeOffset,
	_Out_ PBOOLEAN Found)
{
	if (!Node)
		return NULL;

	if (VolumeOffset < Node->Start)
	{
		Node->Left = CdpPreviewTreeMarkInvalidNodeByStart(
			Node->Left, VolumeOffset, Found);
	}
	else if (VolumeOffset > Node->Start)
	{
		Node->Right = CdpPreviewTreeMarkInvalidNodeByStart(
			Node->Right, VolumeOffset, Found);
	}
	else
	{
		Node->Invalid = TRUE;
		*Found = TRUE;
	}

	CdpPreviewTreeNodeUpdate(Node);
	return Node;
}

NTSTATUS CdpPreviewTreeMarkInvalidByStart(
	_Inout_ PCdp_PREVIEW_TREE Tree,
	_In_ UINT64 VolumeOffset)
{
	BOOLEAN found = FALSE;

	if (!Tree)
		return STATUS_INVALID_PARAMETER;
	Tree->Root = CdpPreviewTreeMarkInvalidNodeByStart(
		Tree->Root, VolumeOffset, &found);
	return found ? STATUS_SUCCESS : STATUS_NOT_FOUND;
}

typedef struct _Cdp_PREVIEW_HIT
{
	UINT64 Start;
	UINT64 End;
	UINT64 FileOffset;
	ULONG DataLength;
	UINT64 Sequence;
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
			Hits[*HitCount].Start = Node->Start;
			Hits[*HitCount].End = Node->End;
			Hits[*HitCount].FileOffset = Node->FileOffset;
			Hits[*HitCount].DataLength = Node->DataLength;
			Hits[*HitCount].Sequence = Node->Sequence;
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

NTSTATUS CdpPreviewTreePunchRange(
	_Inout_ PCdp_PREVIEW_TREE Tree,
	_In_ UINT64 VolumeOffset,
	_In_ ULONG DataLength)
{
	UINT64 cutEnd;

	if (!Tree || DataLength == 0 ||
		VolumeOffset > MAXUINT64 - DataLength)
	{
		return STATUS_INVALID_PARAMETER;
	}
	if (!Tree->Root || Tree->NodeCount == 0)
		return STATUS_SUCCESS;

	cutEnd = VolumeOffset + DataLength;
	return CdpPreviewTreeRemoveRangeInPlace(Tree, VolumeOffset, cutEnd);
}

NTSTATUS CdpJournalBuildPreviewTree(
	_Inout_ PCdp_JOURNAL Journal,
	_In_ UINT64 TargetTime100ns,
	_In_ UINT64 MaxSequence,
	_In_ BOOLEAN IncludeTargetTime,
	_Out_ PCdp_PREVIEW_TREE Tree)
{
	NTSTATUS status = STATUS_SUCCESS;
	UINT64 regionOff;
	ULONG guardRegions = 0;
	BOOLEAN stop = FALSE;
	PUCHAR region = NULL;

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

	status = CdpJournalGetHeaderScanBufferLocked(Journal, &region);
	if (!NT_SUCCESS(status))
	{
		goto cleanup_locked;
	}

	// Single pass, newest-to-oldest.  Each header is immediately overlaid into
	// the tree; the earlier before-image replaces only overlapping bytes from
	// newer records.  No record-header array and no second region read.
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
		if (!CdpJournalRegionLinkValid(Journal, &link))
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
			Cdp_JOURNAL_RECORD record;
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
				header.VolumeOffset > MAXUINT64 - header.DataLength ||
				header.Sequence != (ULONG)index)
			{
				continue;
			}
			status = CdpJournalDecodeRecord(&link, &header, &record);
			if (!NT_SUCCESS(status))
				goto cleanup_locked;

			if (record.Sequence >= MaxSequence)
				continue;

			if (IncludeTargetTime ?
				header.WallClock100ns < TargetTime100ns :
				header.WallClock100ns <= TargetTime100ns)
			{
				stop = TRUE;
				break;
			}

			status = CdpPreviewTreeOverlayEarlier(Tree, &record);
			if (!NT_SUCCESS(status))
				goto cleanup_locked;
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
		CdpPreviewTreeFree(Tree);
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
	for (i = 0; i < hitCount && covered < DataLength; ++i)
	{
		PCdp_PREVIEW_HIT node = &hits[i];
		PVOID payloadBase = NULL;
		PUCHAR payload;
		UINT64 alignedSize;
		UINT64 overlapStart;
		UINT64 overlapEnd;
		ULONG outputIndex;
		ULONG copyLength;

		Cdp_JOURNAL_DIAG(
			"hit begin index=%lu/%lu seq=%llu node=[%llu,%llu) "
			"dataLen=%lu fileOff=%llu\n",
			i,
			hitCount,
			node->Sequence,
			node->Start,
			node->End,
			node->DataLength,
			node->FileOffset);
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
			"payload read begin index=%lu seq=%llu fileOff=%llu len=%lu\n",
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
			"payload read end index=%lu seq=%llu status=0x%08X\n",
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
			"hit end index=%lu seq=%llu covered=%lu\n",
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
	if (Journal->HeaderScanAllocationBase)
		cdpfree(Journal->HeaderScanAllocationBase);
	Journal->HeaderScanAllocationBase = NULL;
	Journal->HeaderScanBuffer = NULL;
	Cdp_LOCK_RELEASE(&Journal->Lock);
	Cdp_LOCK_DELETE(&Journal->Lock);
}
