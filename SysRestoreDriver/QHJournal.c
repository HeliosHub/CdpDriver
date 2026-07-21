#ifdef QH_USERMODE
#include "qh_portable.h"
#include "qh_store.h"
#include "QHJournal.h"
#else
#include "QHEngineDefs.h"
#include "QHJournal.h"
#endif

#define QH_CRC32C_POLY 0x82F63B78UL
#ifndef QH_USERMODE
#define QH_JOURNAL_DIAG(fmt, ...) \
	DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, \
		"SysRestoreDriver: [JOURNAL-APPLY] " fmt, ##__VA_ARGS__)
#else
#define QH_JOURNAL_DIAG(fmt, ...) ((void)0)
#endif

static ULONG g_QhCrc32cTable[256];
static volatile LONG g_QhCrc32cReady;

static VOID QHStallBrief(VOID)
{
#ifdef QH_USERMODE
	SwitchToThread();
#else
	KeStallExecutionProcessor(1);
#endif
}

static UINT64 QHAlignDown64(_In_ UINT64 Value, _In_ ULONG Alignment)
{
	return Value - (Value % Alignment);
}

static UINT64 QHAlignUp64(_In_ UINT64 Value, _In_ ULONG Alignment)
{
	UINT64 remainder = Value % Alignment;
	return remainder ? Value + (Alignment - remainder) : Value;
}

static VOID QHInitializeCrc32c(VOID)
{
	ULONG table[256];
	ULONG i;

	if (InterlockedCompareExchange(&g_QhCrc32cReady, 1, 0) != 0)
	{
		while (InterlockedCompareExchange(&g_QhCrc32cReady, 0, 0) != 2)
			QHStallBrief();
		return;
	}

	for (i = 0; i < RTL_NUMBER_OF(table); ++i)
	{
		ULONG crc = i;
		ULONG bit;
		for (bit = 0; bit < 8; ++bit)
			crc = (crc & 1) ? ((crc >> 1) ^ QH_CRC32C_POLY) : (crc >> 1);
		table[i] = crc;
	}
	RtlCopyMemory(g_QhCrc32cTable, table, sizeof(table));
	InterlockedExchange(&g_QhCrc32cReady, 2);
}

static ULONG QHCrc32c(
	_In_ ULONG InitialCrc,
	_In_reads_bytes_(Length) const VOID* Buffer,
	_In_ SIZE_T Length)
{
	const UCHAR* bytes = (const UCHAR*)Buffer;
	ULONG crc = InitialCrc ^ 0xFFFFFFFFUL;

	if (InterlockedCompareExchange(&g_QhCrc32cReady, 0, 0) != 2)
		QHInitializeCrc32c();

	while (Length--)
		crc = g_QhCrc32cTable[(crc ^ *bytes++) & 0xFF] ^ (crc >> 8);
	return crc ^ 0xFFFFFFFFUL;
}

static PVOID QHAllocateAligned(
	_In_ PQH_JOURNAL Journal,
	_In_ SIZE_T Length,
	_Out_ PVOID* AllocationBase)
{
	SIZE_T alignment = sizeof(PVOID);
	ULONG_PTR address;

	if (Journal->SectorSize > alignment)
		alignment = Journal->SectorSize;
#ifndef QH_USERMODE
	if (Journal->TargetDevice)
	{
		SIZE_T deviceAlign =
			(SIZE_T)Journal->TargetDevice->AlignmentRequirement + 1;
		if (deviceAlign > alignment)
			alignment = deviceAlign;
	}
#endif
	*AllocationBase = qhalloc(Length + alignment - 1);
	if (!*AllocationBase)
		return NULL;
	address = ((ULONG_PTR)*AllocationBase + alignment - 1) & ~(alignment - 1);
	return (PVOID)address;
}

static UINT64 QHJournalUsableStart(_In_ PQH_JOURNAL Journal)
{
	return Journal->SectorSize;
}

static UINT64 QHJournalUsableEnd(_In_ PQH_JOURNAL Journal)
{
	return Journal->PartitionSize - Journal->SectorSize;
}

static NTSTATUS QHJournalRawIo(
	_In_ PQH_JOURNAL Journal,
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

#ifndef QH_USERMODE
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
		DbgPrintEx(
			DPFLTR_IHVDRIVER_ID,
			DPFLTR_ERROR_LEVEL,
			"SysRestoreDriver: [JOURNAL-PHYSICAL] io begin handle=%p "
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
		DbgPrintEx(
			DPFLTR_IHVDRIVER_ID,
			DPFLTR_ERROR_LEVEL,
			"SysRestoreDriver: [JOURNAL-PHYSICAL] io end "
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

	DbgPrintEx(
		DPFLTR_IHVDRIVER_ID,
		DPFLTR_ERROR_LEVEL,
		"SysRestoreDriver: [JOURNAL-RAW] io begin target=%p major=0x%02X "
		"offset=%llu len=%lu irp=%p\n",
		Journal->TargetDevice,
		MajorFunction,
		Offset,
		Length,
		irp);
	status = IoCallDriver(Journal->TargetDevice, irp);
	DbgPrintEx(
		DPFLTR_IHVDRIVER_ID,
		DPFLTR_ERROR_LEVEL,
		"SysRestoreDriver: [JOURNAL-RAW] IoCallDriver returned irp=%p "
		"status=0x%08X iosb=0x%08X bytes=%Iu\n",
		irp,
		status,
		iosb.Status,
		iosb.Information);
	if (status == STATUS_PENDING)
	{
		DbgPrintEx(
			DPFLTR_IHVDRIVER_ID,
			DPFLTR_ERROR_LEVEL,
			"SysRestoreDriver: [JOURNAL-RAW] wait begin irp=%p\n",
			irp);
		KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, NULL);
		status = iosb.Status;
		DbgPrintEx(
			DPFLTR_IHVDRIVER_ID,
			DPFLTR_ERROR_LEVEL,
			"SysRestoreDriver: [JOURNAL-RAW] wait end irp=%p "
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
	DbgPrintEx(
		DPFLTR_IHVDRIVER_ID,
		DPFLTR_ERROR_LEVEL,
		"SysRestoreDriver: [JOURNAL-RAW] io end irp=%p status=0x%08X "
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

static NTSTATUS QHJournalRawWriteSub(
	_In_ PQH_JOURNAL Journal,
	_In_ UINT64 Offset,
	_In_ ULONG Length,
	_In_reads_bytes_(Length) const VOID* Data)
{
	ULONG sec = Journal->SectorSize;
	UINT64 start = (Offset / sec) * sec;
	UINT64 endB = QHAlignUp64(Offset + Length, sec);
	ULONG span = (ULONG)(endB - start);
	PVOID allocationBase = NULL;
	PUCHAR buf;
	NTSTATUS status;

	if ((Offset % sec) == 0 && (Length % sec) == 0)
		return QHJournalRawIo(
			Journal,
			IRP_MJ_WRITE,
			Offset,
			Length,
			(PVOID)Data);

	buf = (PUCHAR)QHAllocateAligned(Journal, span, &allocationBase);
	if (!buf)
		return STATUS_INSUFFICIENT_RESOURCES;

	status = QHJournalRawIo(Journal, IRP_MJ_READ, start, span, buf);
	if (NT_SUCCESS(status))
	{
		RtlCopyMemory(buf + (Offset - start), Data, Length);
		status = QHJournalRawIo(Journal, IRP_MJ_WRITE, start, span, buf);
	}
	qhfree(allocationBase);
	return status;
}

static NTSTATUS QHJournalRawReadSub(
	_In_ PQH_JOURNAL Journal,
	_In_ UINT64 Offset,
	_In_ ULONG Length,
	_Out_writes_bytes_(Length) PVOID Data)
{
	ULONG sec = Journal->SectorSize;
	UINT64 start = (Offset / sec) * sec;
	UINT64 endB = QHAlignUp64(Offset + Length, sec);
	ULONG span = (ULONG)(endB - start);
	PVOID allocationBase = NULL;
	PUCHAR buf;
	NTSTATUS status;

	if ((Offset % sec) == 0 && (Length % sec) == 0)
		return QHJournalRawIo(Journal, IRP_MJ_READ, Offset, Length, Data);

	buf = (PUCHAR)QHAllocateAligned(Journal, span, &allocationBase);
	if (!buf)
		return STATUS_INSUFFICIENT_RESOURCES;

	status = QHJournalRawIo(Journal, IRP_MJ_READ, start, span, buf);
	if (NT_SUCCESS(status))
		RtlCopyMemory(Data, buf + (Offset - start), Length);
	qhfree(allocationBase);
	return status;
}

static NTSTATUS QHJournalFlush(_In_ PQH_JOURNAL Journal)
{
	if (Journal->Store)
		return STATUS_SUCCESS;

#ifndef QH_USERMODE
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

static BOOLEAN QHJournalIsEmptyLocked(_In_ PQH_JOURNAL Journal)
{
	return Journal->TotalRecords == 0;
}

static NTSTATUS QHJournalWriteSuperblockLocked(_Inout_ PQH_JOURNAL Journal)
{
	PVOID allocationBase = NULL;
	PUCHAR sector;
	PQH_JOURNAL_SUPERBLOCK superblock;
	NTSTATUS status;

	sector = (PUCHAR)QHAllocateAligned(Journal,
		Journal->SectorSize,
		&allocationBase);
	if (!sector)
		return STATUS_INSUFFICIENT_RESOURCES;

	RtlZeroMemory(sector, Journal->SectorSize);
	superblock = (PQH_JOURNAL_SUPERBLOCK)sector;
	superblock->Magic = QH_JOURNAL_MAGIC;
	superblock->Version = QH_JOURNAL_VERSION;
	superblock->SectorSize = Journal->SectorSize;
	superblock->PartitionSize = Journal->PartitionSize;
	superblock->LastHeaderRegionOff = Journal->LastHeaderRegionOff;
	superblock->PayloadRegionOff = Journal->PayloadRegionOff;
	superblock->Crc32c = QHCrc32c(
		0,
		superblock,
		FIELD_OFFSET(QH_JOURNAL_SUPERBLOCK, Crc32c));

	status = QHJournalRawIo(
		Journal,
		IRP_MJ_WRITE,
		0,
		Journal->SectorSize,
		sector);
	if (NT_SUCCESS(status))
	{
		status = QHJournalRawIo(
			Journal,
			IRP_MJ_WRITE,
			Journal->PartitionSize - Journal->SectorSize,
			Journal->SectorSize,
			sector);
	}
	qhfree(allocationBase);
	return status;
}

static BOOLEAN QHJournalSuperblockValid(
	_In_ PQH_JOURNAL Journal,
	_In_ const QH_JOURNAL_SUPERBLOCK* Superblock)
{
	ULONG crc;
	UINT64 usableStart;
	UINT64 usableEnd;

	if (Superblock->Magic != QH_JOURNAL_MAGIC ||
		Superblock->Version != QH_JOURNAL_VERSION ||
		Superblock->SectorSize != Journal->SectorSize ||
		Superblock->PartitionSize != Journal->PartitionSize)
	{
		return FALSE;
	}
	crc = QHCrc32c(
		0,
		Superblock,
		FIELD_OFFSET(QH_JOURNAL_SUPERBLOCK, Crc32c));
	if (crc != Superblock->Crc32c)
		return FALSE;

	usableStart = Journal->SectorSize;
	usableEnd = Journal->PartitionSize - Journal->SectorSize;
	if (Superblock->LastHeaderRegionOff < usableStart ||
		Superblock->LastHeaderRegionOff + QH_JOURNAL_HEADER_REGION_SIZE > usableEnd ||
		(Superblock->LastHeaderRegionOff % Journal->SectorSize) != 0 ||
		Superblock->PayloadRegionOff <
			Superblock->LastHeaderRegionOff + QH_JOURNAL_HEADER_REGION_SIZE ||
		Superblock->PayloadRegionOff > usableEnd ||
		(Superblock->PayloadRegionOff % Journal->SectorSize) != 0)
	{
		return FALSE;
	}
	return TRUE;
}

static NTSTATUS QHJournalReadRegionLink(
	_In_ PQH_JOURNAL Journal,
	_In_ UINT64 RegionOff,
	_Out_ PQH_HEADER_REGION_LINK Link)
{
	return QHJournalRawReadSub(
		Journal,
		RegionOff + QH_JOURNAL_HEADER_REGION_SIZE - QH_JOURNAL_HEADER_LINK_SIZE,
		sizeof(*Link),
		Link);
}

static NTSTATUS QHJournalWriteRegionLink(
	_In_ PQH_JOURNAL Journal,
	_In_ UINT64 RegionOff,
	_In_ const QH_HEADER_REGION_LINK* Link)
{
	return QHJournalRawWriteSub(
		Journal,
		RegionOff + QH_JOURNAL_HEADER_REGION_SIZE - QH_JOURNAL_HEADER_LINK_SIZE,
		sizeof(*Link),
		Link);
}

static NTSTATUS QHJournalInitHeaderRegion(
	_In_ PQH_JOURNAL Journal,
	_In_ UINT64 RegionOff,
	_In_ UINT64 PrevOff,
	_In_ UINT64 NextOff)
{
	PVOID allocationBase = NULL;
	PUCHAR region;
	PQH_HEADER_REGION_LINK link;
	NTSTATUS status;
	ULONG offset;
	ULONG chunk;

	region = (PUCHAR)QHAllocateAligned(Journal,
		QH_JOURNAL_HEADER_REGION_SIZE,
		&allocationBase);
	if (!region)
		return STATUS_INSUFFICIENT_RESOURCES;

	RtlZeroMemory(region, QH_JOURNAL_HEADER_REGION_SIZE);
	link = (PQH_HEADER_REGION_LINK)(
		region + QH_JOURNAL_HEADER_REGION_SIZE - QH_JOURNAL_HEADER_LINK_SIZE);
	link->Marker = QH_JOURNAL_HEADER_LINK_MARK;
	link->PrevRegionOff = PrevOff;
	link->NextRegionOff = NextOff;
	link->Reserved = 0;

	chunk = Journal->HeaderRegionWriteChunk;
	if (chunk == 0 || chunk > QH_JOURNAL_HEADER_REGION_SIZE)
		chunk = QH_JOURNAL_HEADER_REGION_SIZE;
	chunk = (chunk / Journal->SectorSize) * Journal->SectorSize;
	if (chunk < Journal->SectorSize)
		chunk = Journal->SectorSize;

	for (offset = 0; offset < QH_JOURNAL_HEADER_REGION_SIZE;)
	{
		ULONG remaining = QH_JOURNAL_HEADER_REGION_SIZE - offset;
		ULONG transfer = chunk < remaining ? chunk : remaining;

		status = QHJournalRawIo(
			Journal,
			IRP_MJ_WRITE,
			RegionOff + offset,
			transfer,
			region + offset);
		if (!NT_SUCCESS(status))
		{
			if (chunk <= Journal->SectorSize)
			{
				qhfree(allocationBase);
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
	qhfree(allocationBase);
	return STATUS_SUCCESS;
}

static ULONG QHJournalRegionHeaderLimit(
	_In_ PQH_JOURNAL Journal,
	_In_ UINT64 RegionOff)
{
	if (RegionOff == Journal->LastHeaderRegionOff)
		return Journal->CurrentHeaderCount;
	return QH_JOURNAL_HEADERS_PER_REGION;
}

static NTSTATUS QHJournalReadHeaderAt(
	_In_ PQH_JOURNAL Journal,
	_In_ UINT64 RegionOff,
	_In_ ULONG Index,
	_Out_ PQH_JOURNAL_RECORD_HEADER Header)
{
	if (Index >= QH_JOURNAL_HEADERS_PER_REGION)
		return STATUS_INVALID_PARAMETER;
	return QHJournalRawReadSub(
		Journal,
		RegionOff + (UINT64)Index * sizeof(QH_JOURNAL_RECORD_HEADER),
		sizeof(*Header),
		Header);
}

static NTSTATUS QHJournalWriteHeaderAt(
	_In_ PQH_JOURNAL Journal,
	_In_ UINT64 RegionOff,
	_In_ ULONG Index,
	_In_ const QH_JOURNAL_RECORD_HEADER* Header)
{
	if (Index >= QH_JOURNAL_HEADERS_PER_REGION)
		return STATUS_INVALID_PARAMETER;
	return QHJournalRawWriteSub(
		Journal,
		RegionOff + (UINT64)Index * sizeof(QH_JOURNAL_RECORD_HEADER),
		sizeof(*Header),
		Header);
}

static NTSTATUS QHJournalRefreshOldestTimeLocked(_Inout_ PQH_JOURNAL Journal)
{
	QH_JOURNAL_RECORD_HEADER header;
	NTSTATUS status;

	if (QHJournalIsEmptyLocked(Journal))
	{
		Journal->Oldest100ns = 0;
		Journal->Newest100ns = 0;
		return STATUS_SUCCESS;
	}

	status = QHJournalReadHeaderAt(
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
static UINT64 QHJournalContiguousFreeLocked(_In_ PQH_JOURNAL Journal)
{
	UINT64 usableEnd = QHJournalUsableEnd(Journal);
	UINT64 head = Journal->PayloadRegionOff;
	UINT64 tail = Journal->OldestHeaderRegionOff;

	if (QHJournalIsEmptyLocked(Journal))
		return usableEnd - head;

	// Write cursor caught up with oldest header: no contiguous free in front.
	if (head == tail)
		return 0;

	if (head < tail)
		return tail - head;

	// head > tail: free until partition end; wrap is handled separately.
	return usableEnd - head;
}

static NTSTATUS QHJournalDropOldestLocked(_Inout_ PQH_JOURNAL Journal)
{
	QH_HEADER_REGION_LINK link;
	ULONG limit;
	NTSTATUS status;

	if (QHJournalIsEmptyLocked(Journal))
		return STATUS_NOT_FOUND;

	limit = QHJournalRegionHeaderLimit(
		Journal,
		Journal->OldestHeaderRegionOff);
	if (Journal->OldestHeaderIndex >= limit)
		return STATUS_DISK_CORRUPT_ERROR;

	Journal->OldestHeaderIndex++;
	Journal->TotalRecords--;

	if (Journal->OldestHeaderIndex < limit)
		return QHJournalRefreshOldestTimeLocked(Journal);

	status = QHJournalReadRegionLink(
		Journal,
		Journal->OldestHeaderRegionOff,
		&link);
	if (!NT_SUCCESS(status) || link.Marker != QH_JOURNAL_HEADER_LINK_MARK)
		return STATUS_DISK_CORRUPT_ERROR;

	if (Journal->OldestHeaderRegionOff == Journal->LastHeaderRegionOff &&
		Journal->TotalRecords == 0)
	{
		Journal->OldestHeaderRegionOff = Journal->LastHeaderRegionOff;
		Journal->OldestHeaderIndex = 0;
		Journal->CurrentHeaderCount = 0;
		Journal->PayloadRegionOff =
			Journal->LastHeaderRegionOff + QH_JOURNAL_HEADER_REGION_SIZE;
		Journal->Oldest100ns = 0;
		Journal->Newest100ns = 0;
		return STATUS_SUCCESS;
	}

	Journal->OldestHeaderRegionOff = link.NextRegionOff;
	Journal->OldestHeaderIndex = 0;
	return QHJournalRefreshOldestTimeLocked(Journal);
}

static NTSTATUS QHJournalEnsureContiguousLocked(
	_Inout_ PQH_JOURNAL Journal,
	_In_ UINT64 BytesNeeded)
{
	UINT64 usableStart = QHJournalUsableStart(Journal);
	UINT64 usableEnd = QHJournalUsableEnd(Journal);
	ULONG guard = 0;

	if (BytesNeeded > usableEnd - usableStart)
		return STATUS_INSUFFICIENT_RESOURCES;

	for (;;)
	{
		NTSTATUS status;

		// Payload hits the end: wrap write cursor; do NOT open a new header.
		if (Journal->PayloadRegionOff + BytesNeeded > usableEnd)
			Journal->PayloadRegionOff = usableStart;

		if (QHJournalContiguousFreeLocked(Journal) >= BytesNeeded)
			return STATUS_SUCCESS;

		status = QHJournalDropOldestLocked(Journal);
		if (!NT_SUCCESS(status))
			return status;
		if (++guard > 1000000UL)
			return STATUS_DISK_CORRUPT_ERROR;
	}
}

// Place a new 2MB header region at the current payload cursor, then start a
// fresh payload area immediately after it: ...[Pprev][Hnew 2MB][Pnew...]
static NTSTATUS QHJournalAllocateHeaderRegionLocked(
	_Inout_ PQH_JOURNAL Journal,
	_Out_ PUINT64 NewRegionOff)
{
	UINT64 usableStart = QHJournalUsableStart(Journal);
	UINT64 usableEnd = QHJournalUsableEnd(Journal);
	UINT64 candidate;
	UINT64 oldRegion;
	QH_HEADER_REGION_LINK oldLink;
	QH_HEADER_REGION_LINK newLink;
	NTSTATUS status;
	ULONG guard = 0;

	candidate = QHAlignUp64(Journal->PayloadRegionOff, Journal->SectorSize);
	if (candidate + QH_JOURNAL_HEADER_REGION_SIZE > usableEnd)
		candidate = usableStart;

	// Reclaim until [candidate, candidate+2MB) does not overlap the oldest live unit.
	while (!QHJournalIsEmptyLocked(Journal))
	{
		UINT64 old = Journal->OldestHeaderRegionOff;
		UINT64 oldEnd = old + QH_JOURNAL_HEADER_REGION_SIZE;
		BOOLEAN overlaps =
			!(oldEnd <= candidate ||
				old >= candidate + QH_JOURNAL_HEADER_REGION_SIZE);

		if (!overlaps)
		{
			if (candidate == usableStart)
			{
				if (Journal->OldestHeaderRegionOff >=
					candidate + QH_JOURNAL_HEADER_REGION_SIZE)
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

		status = QHJournalDropOldestLocked(Journal);
		if (!NT_SUCCESS(status))
			return status;
		if (++guard > 1000000UL)
			return STATUS_DISK_CORRUPT_ERROR;
	}

	if (candidate + QH_JOURNAL_HEADER_REGION_SIZE > usableEnd)
		return STATUS_INSUFFICIENT_RESOURCES;

	oldRegion = Journal->LastHeaderRegionOff;
	status = QHJournalReadRegionLink(Journal, oldRegion, &oldLink);
	if (!NT_SUCCESS(status))
		return status;

	status = QHJournalInitHeaderRegion(
		Journal,
		candidate,
		oldRegion,
		candidate);
	if (!NT_SUCCESS(status))
		return status;

	oldLink.NextRegionOff = candidate;
	status = QHJournalWriteRegionLink(Journal, oldRegion, &oldLink);
	if (!NT_SUCCESS(status))
		return status;

	newLink.Marker = QH_JOURNAL_HEADER_LINK_MARK;
	newLink.PrevRegionOff = oldRegion;
	newLink.NextRegionOff = candidate;
	newLink.Reserved = 0;
	status = QHJournalWriteRegionLink(Journal, candidate, &newLink);
	if (!NT_SUCCESS(status))
		return status;

	Journal->LastHeaderRegionOff = candidate;
	Journal->CurrentHeaderCount = 0;
	// Payload for this header region starts immediately after it.
	Journal->PayloadRegionOff = candidate + QH_JOURNAL_HEADER_REGION_SIZE;

	*NewRegionOff = candidate;
	return STATUS_SUCCESS;
}

static NTSTATUS QHJournalRebuildRuntimeLocked(_Inout_ PQH_JOURNAL Journal)
{
	UINT64 regionOff;
	UINT64 oldestOff;
	ULONG guard = 0;
	QH_HEADER_REGION_LINK link;
	NTSTATUS status;

	Journal->TotalRecords = 0;
	Journal->CurrentHeaderCount = 0;
	Journal->NextSequence = 1;
	Journal->Oldest100ns = 0;
	Journal->Newest100ns = 0;
	Journal->OldestHeaderIndex = 0;

	regionOff = Journal->LastHeaderRegionOff;
	for (;;)
	{
		status = QHJournalReadRegionLink(Journal, regionOff, &link);
		if (!NT_SUCCESS(status) || link.Marker != QH_JOURNAL_HEADER_LINK_MARK)
			return STATUS_DISK_CORRUPT_ERROR;
		if (link.PrevRegionOff == regionOff)
			break;
		regionOff = link.PrevRegionOff;
		if (++guard > 100000UL)
			return STATUS_DISK_CORRUPT_ERROR;
	}
	oldestOff = regionOff;
	Journal->OldestHeaderRegionOff = oldestOff;

	regionOff = oldestOff;
	guard = 0;
	for (;;)
	{
		ULONG index;
		BOOLEAN isLast = (regionOff == Journal->LastHeaderRegionOff);

		status = QHJournalReadRegionLink(Journal, regionOff, &link);
		if (!NT_SUCCESS(status) || link.Marker != QH_JOURNAL_HEADER_LINK_MARK)
			return STATUS_DISK_CORRUPT_ERROR;

		for (index = 0; index < QH_JOURNAL_HEADERS_PER_REGION; ++index)
		{
			QH_JOURNAL_RECORD_HEADER header;

			status = QHJournalReadHeaderAt(
				Journal,
				regionOff,
				index,
				&header);
			if (!NT_SUCCESS(status))
				return status;

			if (header.DataLength == 0 && header.Sequence == 0)
			{
				if (isLast)
					Journal->CurrentHeaderCount = index;
				break;
			}

			Journal->TotalRecords++;
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
				Journal->CurrentHeaderCount = index + 1;
		}

		if (isLast)
			break;
		if (link.NextRegionOff == regionOff)
			break;
		regionOff = link.NextRegionOff;
		if (++guard > 100000UL)
			return STATUS_DISK_CORRUPT_ERROR;
		if (regionOff == oldestOff)
			break;
	}

	return STATUS_SUCCESS;
}

VOID QHJournalInitialize(
	_Out_ PQH_JOURNAL Journal,
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
	Journal->PartitionSize = QHAlignDown64(PartitionSize, SectorSize);
	Journal->SectorSize = SectorSize;
	Journal->SourceVolumeGuid = *SourceVolumeGuid;
	QH_LOCK_INIT(&Journal->Lock);
}

VOID QHJournalInitializeWithStore(
	_Out_ PQH_JOURNAL Journal,
	_In_ PQH_STORE Store,
	_In_ const GUID* SourceVolumeGuid,
	_In_opt_ QH_QUERY_TIME_100NS QueryTime100ns,
	_In_opt_ PVOID QueryTimeContext)
{
	RtlZeroMemory(Journal, sizeof(*Journal));
	Journal->TargetDevice = NULL;
	Journal->Store = Store;
	Journal->PartitionSize = QHAlignDown64(Store->Size, Store->SectorSize);
	Journal->SectorSize = Store->SectorSize;
	Journal->SourceVolumeGuid = *SourceVolumeGuid;
	Journal->QueryTime100ns = QueryTime100ns;
	Journal->QueryTimeContext = QueryTimeContext;
	QH_LOCK_INIT(&Journal->Lock);
}

static BOOLEAN QHJournalHasBackend(_In_ PQH_JOURNAL Journal)
{
	return Journal->Store != NULL ||
		Journal->RawDiskHandle != NULL ||
		Journal->TargetDevice != NULL;
}

NTSTATUS QHJournalFormat(_Inout_ PQH_JOURNAL Journal)
{
	UINT64 usableStart;
	UINT64 usableEnd;
	UINT64 headerOff;
	UINT64 minSize;
	NTSTATUS status;

	minSize = (UINT64)Journal->SectorSize * 2ULL +
		QH_JOURNAL_HEADER_REGION_SIZE + (UINT64)Journal->SectorSize;
	if (!QHJournalHasBackend(Journal) ||
		(Journal->SectorSize != 512 && Journal->SectorSize != 4096) ||
		Journal->PartitionSize < minSize)
	{
		return STATUS_INVALID_PARAMETER;
	}

	QH_LOCK_ACQUIRE(&Journal->Lock);

	usableStart = QHJournalUsableStart(Journal);
	usableEnd = QHJournalUsableEnd(Journal);
	headerOff = usableStart;
	if (headerOff + QH_JOURNAL_HEADER_REGION_SIZE >= usableEnd)
	{
		status = STATUS_INVALID_PARAMETER;
		goto cleanup;
	}

	status = QHJournalInitHeaderRegion(
		Journal,
		headerOff,
		headerOff,
		headerOff);
	if (!NT_SUCCESS(status))
		goto cleanup;

	Journal->LastHeaderRegionOff = headerOff;
	// Payload area 0 starts immediately after header region 0.
	Journal->PayloadRegionOff = headerOff + QH_JOURNAL_HEADER_REGION_SIZE;
	Journal->OldestHeaderRegionOff = headerOff;
	Journal->OldestHeaderIndex = 0;
	Journal->CurrentHeaderCount = 0;
	Journal->NextSequence = 1;
	Journal->TotalRecords = 0;
	Journal->Oldest100ns = 0;
	Journal->Newest100ns = 0;

		status = QHJournalWriteSuperblockLocked(Journal);
	if (NT_SUCCESS(status))
		status = QHJournalFlush(Journal);
	if (NT_SUCCESS(status))
		Journal->Mounted = TRUE;

cleanup:
	QH_LOCK_RELEASE(&Journal->Lock);
	return status;
}

NTSTATUS QHJournalMount(_Inout_ PQH_JOURNAL Journal)
{
	PVOID allocationBase = NULL;
	PUCHAR sector;
	PQH_JOURNAL_SUPERBLOCK superblock;
	UINT64 minSize;
	NTSTATUS status;

	minSize = (UINT64)Journal->SectorSize * 2ULL +
		QH_JOURNAL_HEADER_REGION_SIZE + (UINT64)Journal->SectorSize;
	if (!QHJournalHasBackend(Journal) ||
		(Journal->SectorSize != 512 && Journal->SectorSize != 4096) ||
		Journal->PartitionSize < minSize)
	{
		return STATUS_INVALID_PARAMETER;
	}

	QH_LOCK_ACQUIRE(&Journal->Lock);
	sector = (PUCHAR)QHAllocateAligned(Journal,
		Journal->SectorSize,
		&allocationBase);
	if (!sector)
	{
		status = STATUS_INSUFFICIENT_RESOURCES;
		goto cleanup;
	}

	status = QHJournalRawIo(
		Journal,
		IRP_MJ_READ,
		0,
		Journal->SectorSize,
		sector);
	superblock = (PQH_JOURNAL_SUPERBLOCK)sector;
	if (!NT_SUCCESS(status) || !QHJournalSuperblockValid(Journal, superblock))
	{
		status = QHJournalRawIo(
			Journal,
			IRP_MJ_READ,
			Journal->PartitionSize - Journal->SectorSize,
			Journal->SectorSize,
			sector);
		if (!NT_SUCCESS(status) || !QHJournalSuperblockValid(Journal, superblock))
		{
			status = STATUS_DISK_CORRUPT_ERROR;
			goto cleanup;
		}
	}

	Journal->LastHeaderRegionOff = superblock->LastHeaderRegionOff;
	Journal->PayloadRegionOff = superblock->PayloadRegionOff;

	status = QHJournalRebuildRuntimeLocked(Journal);
	if (!NT_SUCCESS(status))
		goto cleanup;

	Journal->Mounted = TRUE;
	status = STATUS_SUCCESS;

cleanup:
	if (allocationBase)
		qhfree(allocationBase);
	if (!NT_SUCCESS(status))
		Journal->Mounted = FALSE;
	QH_LOCK_RELEASE(&Journal->Lock);
	return status;
}

NTSTATUS QHJournalAppend(
	_Inout_ PQH_JOURNAL Journal,
	_In_ UINT64 VolumeOffset,
	_In_ ULONG DataLength,
	_In_reads_bytes_(DataLength) const VOID* BeforeImage,
	_Out_opt_ PQH_JOURNAL_RECORD_HEADER WrittenHeader)
{
	QH_JOURNAL_RECORD_HEADER header;
	UINT64 payloadOff;
	UINT64 alignedSize;
	PVOID allocationBase = NULL;
	PUCHAR payloadBuffer = NULL;
	ULONG writeSeq;
	UINT64 writeTime;
	NTSTATUS status = STATUS_SUCCESS;

	if (!Journal->Mounted || !BeforeImage ||
		DataLength == 0 || DataLength > QH_JOURNAL_MAX_RECORD_DATA)
	{
		return STATUS_INVALID_PARAMETER;
	}

	QH_LOCK_ACQUIRE(&Journal->Lock);
	if (!Journal->Mounted)
	{
		status = STATUS_DEVICE_NOT_READY;
		goto cleanup;
	}

	alignedSize = QHAlignUp64(DataLength, Journal->SectorSize);

	// New header region only when the current 2MB header slots are exhausted.
	if (Journal->CurrentHeaderCount >= QH_JOURNAL_HEADERS_PER_REGION)
	{
		UINT64 newRegion = 0;
		status = QHJournalAllocateHeaderRegionLocked(Journal, &newRegion);
		if (!NT_SUCCESS(status))
			goto cleanup;
	}

	// Payload: wrap at partition end and/or drop oldest until there is room.
	status = QHJournalEnsureContiguousLocked(Journal, alignedSize);
	if (!NT_SUCCESS(status))
		goto cleanup;

	payloadOff = Journal->PayloadRegionOff;
	payloadBuffer = (PUCHAR)QHAllocateAligned(Journal,
		(SIZE_T)alignedSize,
		&allocationBase);
	if (!payloadBuffer)
	{
		status = STATUS_INSUFFICIENT_RESOURCES;
		goto cleanup;
	}
	RtlZeroMemory(payloadBuffer, (SIZE_T)alignedSize);
	RtlCopyMemory(payloadBuffer, BeforeImage, DataLength);

	status = QHJournalRawIo(
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
#ifdef QH_USERMODE
		FILETIME ft;
		ULARGE_INTEGER u;
		GetSystemTimeAsFileTime(&ft);
		u.LowPart = ft.dwLowDateTime;
		u.HighPart = ft.dwHighDateTime;
		writeTime = u.QuadPart;
#else
		{
			LARGE_INTEGER wallClock;
			KeQuerySystemTime(&wallClock);
			writeTime = (UINT64)wallClock.QuadPart;
		}
#endif
	}

	RtlZeroMemory(&header, sizeof(header));
	header.WallClock100ns = writeTime;
	header.VolumeOffset = VolumeOffset;
	header.FileOffset = payloadOff;
	header.DataLength = DataLength;
	header.Sequence = writeSeq;

	status = QHJournalWriteHeaderAt(
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
	Journal->TotalRecords++;
	Journal->Newest100ns = writeTime;
	if (Journal->TotalRecords == 1)
	{
		Journal->OldestHeaderRegionOff = Journal->LastHeaderRegionOff;
		Journal->OldestHeaderIndex = Journal->CurrentHeaderCount - 1;
		Journal->Oldest100ns = writeTime;
	}

	if (WrittenHeader)
		*WrittenHeader = header;

	status = QHJournalWriteSuperblockLocked(Journal);
	if (NT_SUCCESS(status))
		status = QHJournalFlush(Journal);

cleanup:
	if (allocationBase)
		qhfree(allocationBase);
	QH_LOCK_RELEASE(&Journal->Lock);
	return status;
}

NTSTATUS QHJournalQueryTimeRange(
	_Inout_ PQH_JOURNAL Journal,
	_Out_ PUINT64 OldestTime100ns,
	_Out_ PUINT64 NewestTime100ns)
{
	NTSTATUS status;

	if (!OldestTime100ns || !NewestTime100ns)
		return STATUS_INVALID_PARAMETER;
	*OldestTime100ns = 0;
	*NewestTime100ns = 0;

	QH_LOCK_ACQUIRE(&Journal->Lock);
	if (!Journal->Mounted)
		status = STATUS_DEVICE_NOT_READY;
	else if (QHJournalIsEmptyLocked(Journal))
		status = STATUS_NOT_FOUND;
	else
	{
		*OldestTime100ns = Journal->Oldest100ns;
		*NewestTime100ns = Journal->Newest100ns;
		status = STATUS_SUCCESS;
	}
	QH_LOCK_RELEASE(&Journal->Lock);
	return status;
}

VOID QHPreviewTreeInitialize(_Out_ PQH_PREVIEW_TREE Tree)
{
	RtlZeroMemory(Tree, sizeof(*Tree));
}

static VOID QHPreviewTreeFreeNode(_In_opt_ PQH_PREVIEW_TREE_NODE Node)
{
	if (!Node)
		return;
	QHPreviewTreeFreeNode(Node->Left);
	QHPreviewTreeFreeNode(Node->Right);
	qhfree(Node);
}

VOID QHPreviewTreeFree(_Inout_ PQH_PREVIEW_TREE Tree)
{
	if (!Tree)
		return;
	QHPreviewTreeFreeNode(Tree->Root);
	Tree->Root = NULL;
	Tree->NodeCount = 0;
}

static LONG QHPreviewTreeNodeHeight(
	_In_opt_ PQH_PREVIEW_TREE_NODE Node)
{
	return Node ? Node->Height : 0;
}

static VOID QHPreviewTreeNodeUpdate(
	_Inout_ PQH_PREVIEW_TREE_NODE Node)
{
	LONG hl = QHPreviewTreeNodeHeight(Node->Left);
	LONG hr = QHPreviewTreeNodeHeight(Node->Right);
	UINT64 maxEnd = Node->End;

	Node->Height = 1 + (hl > hr ? hl : hr);
	if (Node->Left && Node->Left->MaxEnd > maxEnd)
		maxEnd = Node->Left->MaxEnd;
	if (Node->Right && Node->Right->MaxEnd > maxEnd)
		maxEnd = Node->Right->MaxEnd;
	Node->MaxEnd = maxEnd;
}

static PQH_PREVIEW_TREE_NODE QHPreviewTreeRotateRight(
	_Inout_ PQH_PREVIEW_TREE_NODE Y)
{
	PQH_PREVIEW_TREE_NODE x = Y->Left;
	PQH_PREVIEW_TREE_NODE t2 = x->Right;

	x->Right = Y;
	Y->Left = t2;
	QHPreviewTreeNodeUpdate(Y);
	QHPreviewTreeNodeUpdate(x);
	return x;
}

static PQH_PREVIEW_TREE_NODE QHPreviewTreeRotateLeft(
	_Inout_ PQH_PREVIEW_TREE_NODE X)
{
	PQH_PREVIEW_TREE_NODE y = X->Right;
	PQH_PREVIEW_TREE_NODE t2 = y->Left;

	y->Left = X;
	X->Right = t2;
	QHPreviewTreeNodeUpdate(X);
	QHPreviewTreeNodeUpdate(y);
	return y;
}

static PQH_PREVIEW_TREE_NODE QHPreviewTreeAvlInsertNode(
	_In_opt_ PQH_PREVIEW_TREE_NODE Root,
	_In_ PQH_PREVIEW_TREE_NODE Node)
{
	LONG balance;

	if (!Root)
		return Node;

	if (Node->Start < Root->Start)
		Root->Left = QHPreviewTreeAvlInsertNode(Root->Left, Node);
	else
		Root->Right = QHPreviewTreeAvlInsertNode(Root->Right, Node);

	QHPreviewTreeNodeUpdate(Root);
	balance = QHPreviewTreeNodeHeight(Root->Left) -
		QHPreviewTreeNodeHeight(Root->Right);

	if (balance > 1 && Node->Start < Root->Left->Start)
		return QHPreviewTreeRotateRight(Root);
	if (balance < -1 && Node->Start >= Root->Right->Start)
		return QHPreviewTreeRotateLeft(Root);
	if (balance > 1 && Node->Start >= Root->Left->Start)
	{
		Root->Left = QHPreviewTreeRotateLeft(Root->Left);
		return QHPreviewTreeRotateRight(Root);
	}
	if (balance < -1 && Node->Start < Root->Right->Start)
	{
		Root->Right = QHPreviewTreeRotateRight(Root->Right);
		return QHPreviewTreeRotateLeft(Root);
	}
	return Root;
}

static NTSTATUS QHPreviewTreeInsertRaw(
	_Inout_ PQH_PREVIEW_TREE Tree,
	_In_ const QH_JOURNAL_RECORD_HEADER* Header)
{
	PQH_PREVIEW_TREE_NODE node;

	if (!Tree || !Header)
		return STATUS_INVALID_PARAMETER;
	if (Header->DataLength == 0)
		return STATUS_SUCCESS;

	node = (PQH_PREVIEW_TREE_NODE)qhalloc(sizeof(*node));
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

	Tree->Root = QHPreviewTreeAvlInsertNode(Tree->Root, node);
	Tree->NodeCount++;
	return STATUS_SUCCESS;
}

// Forward: defined with CollectOverlaps; used by Insert for in-tree dedup.
static VOID QHPreviewTreeClearMaskByTree(
	_In_ PQH_PREVIEW_TREE Tree,
	_In_ UINT64 VolumeOffset,
	_In_ ULONG DataLength,
	_Inout_updates_(DataLength) PUCHAR Mask);

NTSTATUS QHPreviewTreeInsert(
	_Inout_ PQH_PREVIEW_TREE Tree,
	_In_ const QH_JOURNAL_RECORD_HEADER* Header)
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
		return QHPreviewTreeInsertRaw(Tree, Header);

	len = Header->DataLength;
	mask = (PUCHAR)qhalloc(len);
	if (!mask)
		return STATUS_INSUFFICIENT_RESOURCES;

	RtlFillMemory(mask, len, 1);
	QHPreviewTreeClearMaskByTree(
		Tree,
		Header->VolumeOffset,
		len,
		mask);

	idx = 0;
	while (idx < len)
	{
		while (idx < len && mask[idx] == 0)
			++idx;
		if (idx >= len)
			break;
		runStart = idx;
		while (idx < len && mask[idx] != 0)
			++idx;

		{
			QH_JOURNAL_RECORD_HEADER frag = *Header;
			frag.VolumeOffset = Header->VolumeOffset + runStart;
			frag.FileOffset = Header->FileOffset + runStart;
			frag.DataLength = idx - runStart;
			status = QHPreviewTreeInsertRaw(Tree, &frag);
			if (!NT_SUCCESS(status))
				break;
		}
	}

	qhfree(mask);
	return status;
}

static NTSTATUS QHPreviewTreeMergeNode(
	_Inout_ PQH_PREVIEW_TREE Dest,
	_In_opt_ PQH_PREVIEW_TREE_NODE Node)
{
	QH_JOURNAL_RECORD_HEADER header;
	NTSTATUS status;

	if (!Node)
		return STATUS_SUCCESS;

	status = QHPreviewTreeMergeNode(Dest, Node->Left);
	if (!NT_SUCCESS(status))
		return status;

	RtlZeroMemory(&header, sizeof(header));
	header.WallClock100ns = Node->WallClock100ns;
	header.VolumeOffset = Node->Start;
	header.FileOffset = Node->FileOffset;
	header.DataLength = Node->DataLength;
	header.Sequence = Node->Sequence;
	status = QHPreviewTreeInsert(Dest, &header);
	if (!NT_SUCCESS(status))
		return status;

	return QHPreviewTreeMergeNode(Dest, Node->Right);
}

NTSTATUS QHPreviewTreeMergeFrom(
	_Inout_ PQH_PREVIEW_TREE Dest,
	_Inout_ PQH_PREVIEW_TREE Source)
{
	NTSTATUS status;

	if (!Dest || !Source)
		return STATUS_INVALID_PARAMETER;
	if (!Source->Root)
		return STATUS_SUCCESS;

	status = QHPreviewTreeMergeNode(Dest, Source->Root);
	QHPreviewTreeFree(Source);
	return status;
}

static VOID QHPreviewTreePunchByNode(
	_Inout_ PQH_PREVIEW_TREE HistoryTree,
	_In_opt_ PQH_PREVIEW_TREE_NODE Node)
{
	if (!Node)
		return;

	QHPreviewTreePunchByNode(HistoryTree, Node->Left);
	QHPreviewTreeInvalidateRange(
		HistoryTree,
		Node->Start,
		Node->DataLength);
	QHPreviewTreePunchByNode(HistoryTree, Node->Right);
}

VOID QHPreviewTreePunchByStaging(
	_Inout_ PQH_PREVIEW_TREE HistoryTree,
	_Inout_ PQH_PREVIEW_TREE StagingTree)
{
	if (!HistoryTree || !StagingTree)
		return;

	if (StagingTree->Root)
		QHPreviewTreePunchByNode(HistoryTree, StagingTree->Root);

	QHPreviewTreeFree(StagingTree);
}

static VOID QHPreviewTreeInvalidateOverlaps(
	_In_opt_ PQH_PREVIEW_TREE_NODE Node,
	_In_ UINT64 CutStart,
	_In_ UINT64 CutEnd)
{
	if (!Node)
		return;
	if (Node->MaxEnd <= CutStart)
		return;

	if (Node->Left)
		QHPreviewTreeInvalidateOverlaps(Node->Left, CutStart, CutEnd);

	if (!Node->Invalid &&
		Node->Start < CutEnd &&
		Node->End > CutStart)
	{
		Node->Invalid = TRUE;
	}

	if (Node->Start < CutEnd && Node->Right)
		QHPreviewTreeInvalidateOverlaps(Node->Right, CutStart, CutEnd);
}

VOID QHPreviewTreeInvalidateRange(
	_Inout_ PQH_PREVIEW_TREE Tree,
	_In_ UINT64 VolumeOffset,
	_In_ ULONG DataLength)
{
	if (!Tree || !Tree->Root || DataLength == 0 ||
		VolumeOffset > MAXUINT64 - DataLength)
	{
		return;
	}

	QHPreviewTreeInvalidateOverlaps(
		Tree->Root,
		VolumeOffset,
		VolumeOffset + DataLength);
}

typedef struct _QH_PREVIEW_HIT
{
	PQH_PREVIEW_TREE_NODE Node;
} QH_PREVIEW_HIT, *PQH_PREVIEW_HIT;

static VOID QHPreviewTreeCollectOverlaps(
	_In_opt_ PQH_PREVIEW_TREE_NODE Node,
	_In_ UINT64 QueryStart,
	_In_ UINT64 QueryEnd,
	_Inout_updates_(HitCapacity) QH_PREVIEW_HIT* Hits,
	_Inout_ PULONG HitCount,
	_In_ ULONG HitCapacity)
{
	if (!Node || *HitCount >= HitCapacity)
		return;
	if (Node->MaxEnd <= QueryStart)
		return;

	if (Node->Left)
		QHPreviewTreeCollectOverlaps(
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
			Hits[*HitCount].Node = Node;
			(*HitCount)++;
		}
	}

	if (Node->Start < QueryEnd && Node->Right)
		QHPreviewTreeCollectOverlaps(
			Node->Right,
			QueryStart,
			QueryEnd,
			Hits,
			HitCount,
			HitCapacity);
}

static int QHPreviewHitCompareSequence(
	_In_ const VOID* A,
	_In_ const VOID* B)
{
	const QH_PREVIEW_HIT* ha = (const QH_PREVIEW_HIT*)A;
	const QH_PREVIEW_HIT* hb = (const QH_PREVIEW_HIT*)B;
	if (ha->Node->Sequence < hb->Node->Sequence)
		return -1;
	if (ha->Node->Sequence > hb->Node->Sequence)
		return 1;
	return 0;
}

static VOID QHPreviewSortHitsBySequence(
	_Inout_updates_(Count) PQH_PREVIEW_HIT Hits,
	_In_ ULONG Count)
{
	ULONG i;
	for (i = 1; i < Count; ++i)
	{
		QH_PREVIEW_HIT key = Hits[i];
		ULONG j = i;
		while (j > 0 &&
			QHPreviewHitCompareSequence(&Hits[j - 1], &key) > 0)
		{
			Hits[j] = Hits[j - 1];
			j--;
		}
		Hits[j] = key;
	}
}

static VOID QHPreviewTreeCollectAllValid(
	_In_opt_ PQH_PREVIEW_TREE_NODE Node,
	_Out_writes_(Capacity) PQH_PREVIEW_TREE_NODE* Nodes,
	_Inout_ PULONG Count,
	_In_ ULONG Capacity)
{
	if (!Node || *Count >= Capacity)
		return;

	QHPreviewTreeCollectAllValid(Node->Left, Nodes, Count, Capacity);
	if (!Node->Invalid && *Count < Capacity)
	{
		Nodes[*Count] = Node;
		(*Count)++;
	}
	QHPreviewTreeCollectAllValid(Node->Right, Nodes, Count, Capacity);
}

static int QHPreviewHeaderCompareSequence(
	_In_ const QH_JOURNAL_RECORD_HEADER* A,
	_In_ const QH_JOURNAL_RECORD_HEADER* B)
{
	if (A->Sequence < B->Sequence)
		return -1;
	if (A->Sequence > B->Sequence)
		return 1;
	return 0;
}

static VOID QHPreviewSortHeadersBySequence(
	_Inout_updates_(Count) QH_JOURNAL_RECORD_HEADER* Headers,
	_In_ ULONG Count)
{
	ULONG i;
	for (i = 1; i < Count; ++i)
	{
		QH_JOURNAL_RECORD_HEADER key = Headers[i];
		ULONG j = i;
		while (j > 0 &&
			QHPreviewHeaderCompareSequence(&Headers[j - 1], &key) > 0)
		{
			Headers[j] = Headers[j - 1];
			--j;
		}
		Headers[j] = key;
	}
}

static VOID QHPreviewTreeClearMaskByTree(
	_In_ PQH_PREVIEW_TREE Tree,
	_In_ UINT64 VolumeOffset,
	_In_ ULONG DataLength,
	_Inout_updates_(DataLength) PUCHAR Mask)
{
	QH_PREVIEW_HIT* hits = NULL;
	ULONG hitCount = 0;
	ULONG hitCapacity;
	ULONG i;

	if (!Tree || !Tree->Root || !Mask || DataLength == 0)
		return;

	hitCapacity = Tree->NodeCount ? Tree->NodeCount : 1;
	hits = (QH_PREVIEW_HIT*)qhalloc(sizeof(QH_PREVIEW_HIT) * hitCapacity);
	if (!hits)
		return;

	QHPreviewTreeCollectOverlaps(
		Tree->Root,
		VolumeOffset,
		VolumeOffset + DataLength,
		hits,
		&hitCount,
		hitCapacity);

	for (i = 0; i < hitCount; ++i)
	{
		PQH_PREVIEW_TREE_NODE node = hits[i].Node;
		UINT64 o0 = node->Start > VolumeOffset ? node->Start : VolumeOffset;
		UINT64 o1 = node->End < (VolumeOffset + DataLength) ?
			node->End : (VolumeOffset + DataLength);
		UINT64 b;
		for (b = o0; b < o1; ++b)
			Mask[(ULONG)(b - VolumeOffset)] = 0;
	}

	qhfree(hits);
}

NTSTATUS QHPreviewTreeDedupEarliest(
	_Inout_ PQH_PREVIEW_TREE Tree)
{
	PQH_PREVIEW_TREE_NODE* nodes = NULL;
	QH_JOURNAL_RECORD_HEADER* headers = NULL;
	ULONG count = 0;
	ULONG capacity;
	ULONG i;
	NTSTATUS status = STATUS_SUCCESS;
	QH_PREVIEW_TREE rebuilt;

	if (!Tree)
		return STATUS_INVALID_PARAMETER;
	if (!Tree->Root || Tree->NodeCount == 0)
		return STATUS_SUCCESS;

	capacity = Tree->NodeCount;
	nodes = (PQH_PREVIEW_TREE_NODE*)qhalloc(
		sizeof(PQH_PREVIEW_TREE_NODE) * capacity);
	headers = (QH_JOURNAL_RECORD_HEADER*)qhalloc(
		sizeof(QH_JOURNAL_RECORD_HEADER) * capacity);
	if (!nodes || !headers)
		{
			status = STATUS_INSUFFICIENT_RESOURCES;
			goto cleanup;
		}

	QHPreviewTreeCollectAllValid(Tree->Root, nodes, &count, capacity);
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
	QHPreviewTreeFree(Tree);
	QHPreviewTreeInitialize(&rebuilt);

	QHPreviewSortHeadersBySequence(headers, count);

	for (i = 0; i < count; ++i)
	{
		PUCHAR mask = NULL;
		ULONG len = headers[i].DataLength;
		ULONG idx;
		ULONG runStart;

		if (len == 0)
			continue;

		mask = (PUCHAR)qhalloc(len);
		if (!mask)
		{
			status = STATUS_INSUFFICIENT_RESOURCES;
			QHPreviewTreeFree(&rebuilt);
			goto cleanup;
		}
		RtlFillMemory(mask, len, 1);
		QHPreviewTreeClearMaskByTree(
			&rebuilt,
			headers[i].VolumeOffset,
			len,
			mask);

		idx = 0;
		while (idx < len)
		{
			while (idx < len && mask[idx] == 0)
				++idx;
			if (idx >= len)
				break;
			runStart = idx;
			while (idx < len && mask[idx] != 0)
				++idx;

			{
				QH_JOURNAL_RECORD_HEADER frag = headers[i];
				frag.VolumeOffset =
					headers[i].VolumeOffset + runStart;
				frag.FileOffset =
					headers[i].FileOffset + runStart;
				frag.DataLength = idx - runStart;
				status = QHPreviewTreeInsertRaw(&rebuilt, &frag);
		if (!NT_SUCCESS(status))
				{
					qhfree(mask);
					QHPreviewTreeFree(&rebuilt);
			goto cleanup;
				}
			}
		}
		qhfree(mask);
	}

	*Tree = rebuilt;

	if (NT_SUCCESS(status))
		status = QHPreviewTreeCoalesceAdjacent(Tree);

cleanup:
	if (nodes)
		qhfree(nodes);
	if (headers)
		qhfree(headers);
	return status;
}

NTSTATUS QHPreviewTreeCoalesceAdjacent(
	_Inout_ PQH_PREVIEW_TREE Tree)
{
	PQH_PREVIEW_TREE_NODE* nodes = NULL;
	QH_JOURNAL_RECORD_HEADER* headers = NULL;
	ULONG count = 0;
	ULONG capacity;
	ULONG i;
	ULONG outCount = 0;
	NTSTATUS status = STATUS_SUCCESS;
	QH_PREVIEW_TREE rebuilt;

	if (!Tree)
		return STATUS_INVALID_PARAMETER;
	if (!Tree->Root || Tree->NodeCount == 0)
		return STATUS_SUCCESS;

	capacity = Tree->NodeCount;
	nodes = (PQH_PREVIEW_TREE_NODE*)qhalloc(
		sizeof(PQH_PREVIEW_TREE_NODE) * capacity);
	headers = (QH_JOURNAL_RECORD_HEADER*)qhalloc(
		sizeof(QH_JOURNAL_RECORD_HEADER) * capacity);
	if (!nodes || !headers)
	{
		status = STATUS_INSUFFICIENT_RESOURCES;
		goto cleanup;
	}

	QHPreviewTreeCollectAllValid(Tree->Root, nodes, &count, capacity);
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
		QH_JOURNAL_RECORD_HEADER key = headers[i];
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
		QH_JOURNAL_RECORD_HEADER* cur = &headers[i];
		QH_JOURNAL_RECORD_HEADER* prev;

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

	QHPreviewTreeFree(Tree);
	QHPreviewTreeInitialize(&rebuilt);
	for (i = 0; i < outCount; ++i)
	{
		status = QHPreviewTreeInsertRaw(&rebuilt, &headers[i]);
		if (!NT_SUCCESS(status))
		{
			QHPreviewTreeFree(&rebuilt);
			goto cleanup;
		}
	}
	*Tree = rebuilt;

cleanup:
	if (nodes)
		qhfree(nodes);
	if (headers)
		qhfree(headers);
	return status;
}

NTSTATUS QHJournalBuildPreviewTree(
	_Inout_ PQH_JOURNAL Journal,
	_In_ UINT64 TargetTime100ns,
	_In_ ULONG MaxSequence,
	_In_ BOOLEAN IncludeTargetTime,
	_Out_ PQH_PREVIEW_TREE Tree)
{
	NTSTATUS status = STATUS_SUCCESS;
	UINT64 regionOff;
	ULONG guardRegions = 0;
	BOOLEAN stop = FALSE;
	QH_JOURNAL_RECORD_HEADER* headers = NULL;
	ULONG headerCount = 0;
	ULONG headerCapacity = 0;
	ULONG i;

	if (!Tree)
		return STATUS_INVALID_PARAMETER;

	QHPreviewTreeInitialize(Tree);

	QH_LOCK_ACQUIRE(&Journal->Lock);
	if (!Journal->Mounted)
	{
		status = STATUS_DEVICE_NOT_READY;
		goto cleanup_locked;
	}
	if (QHJournalIsEmptyLocked(Journal) ||
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

	regionOff = Journal->LastHeaderRegionOff;
	for (;;)
	{
		QH_HEADER_REGION_LINK link;
		ULONG limit;
		LONG index;
		BOOLEAN isLast;
		BOOLEAN isOldest;

		if (++guardRegions > 100000UL)
		{
			status = STATUS_DISK_CORRUPT_ERROR;
			goto cleanup_locked;
		}

		status = QHJournalReadRegionLink(Journal, regionOff, &link);
		if (!NT_SUCCESS(status) || link.Marker != QH_JOURNAL_HEADER_LINK_MARK)
		{
			status = STATUS_DISK_CORRUPT_ERROR;
			goto cleanup_locked;
		}

		isLast = (regionOff == Journal->LastHeaderRegionOff);
		isOldest = (regionOff == Journal->OldestHeaderRegionOff);
		limit = isLast ?
			Journal->CurrentHeaderCount : QH_JOURNAL_HEADERS_PER_REGION;

		for (index = (LONG)limit - 1; index >= 0; --index)
		{
			QH_JOURNAL_RECORD_HEADER header;
			ULONG startIndex = isOldest ? Journal->OldestHeaderIndex : 0;

			if ((ULONG)index < startIndex)
				break;

			status = QHJournalReadHeaderAt(
				Journal,
				regionOff,
				(ULONG)index,
				&header);
			if (!NT_SUCCESS(status))
				goto cleanup_locked;

			if (header.DataLength == 0 ||
				header.DataLength > QH_JOURNAL_MAX_RECORD_DATA ||
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
				QH_JOURNAL_RECORD_HEADER* grown;

				if (newCap < headerCount + 1)
					newCap = headerCount + 1;
				grown = (QH_JOURNAL_RECORD_HEADER*)qhalloc(
					sizeof(QH_JOURNAL_RECORD_HEADER) * newCap);
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
						sizeof(QH_JOURNAL_RECORD_HEADER) * headerCount);
					qhfree(headers);
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
	QH_LOCK_RELEASE(&Journal->Lock);
	if (!NT_SUCCESS(status))
		goto cleanup;

	// Insert oldest Sequence first; Insert skips already-covered bytes so the
	// tree never holds overlapping intervals (Preview + Recovery share this).
	if (headerCount > 0)
	{
		QHPreviewSortHeadersBySequence(headers, headerCount);
		for (i = 0; i < headerCount; ++i)
		{
			status = QHPreviewTreeInsert(Tree, &headers[i]);
			if (!NT_SUCCESS(status))
			{
				QHPreviewTreeFree(Tree);
				break;
			}
		}
		if (NT_SUCCESS(status))
			status = QHPreviewTreeCoalesceAdjacent(Tree);
		if (!NT_SUCCESS(status))
			QHPreviewTreeFree(Tree);
	}

cleanup:
	if (headers)
		qhfree(headers);
	return status;
}

NTSTATUS QHJournalApplyPreviewTree(
	_Inout_ PQH_JOURNAL Journal,
	_In_ PQH_PREVIEW_TREE Tree,
	_In_ UINT64 VolumeOffset,
	_In_ ULONG DataLength,
	_Out_writes_bytes_(DataLength) PVOID Buffer,
	_Out_writes_bytes_(DataLength) PUCHAR CoveredMask,
	_Out_ PULONG CoveredCount)
{
	NTSTATUS status = STATUS_SUCCESS;
	PQH_PREVIEW_HIT hits = NULL;
	ULONG hitCount = 0;
	ULONG hitCapacity;
	ULONG covered = 0;
	ULONG i;

	if (!Tree || !Buffer || !CoveredMask || !CoveredCount || DataLength == 0 ||
		VolumeOffset > MAXUINT64 - DataLength)
	{
		return STATUS_INVALID_PARAMETER;
	}

	*CoveredCount = 0;
	RtlZeroMemory(CoveredMask, DataLength);

	if (!Tree->Root || Tree->NodeCount == 0)
		return STATUS_SUCCESS;

	hitCapacity = Tree->NodeCount;
	hits = (PQH_PREVIEW_HIT)qhalloc(sizeof(QH_PREVIEW_HIT) * hitCapacity);
	if (!hits)
		return STATUS_INSUFFICIENT_RESOURCES;

	if (!Journal->Mounted)
	{
		status = STATUS_DEVICE_NOT_READY;
		goto cleanup;
	}

	QH_JOURNAL_DIAG(
		"collect begin volumeOff=%llu len=%lu nodes=%lu\n",
		VolumeOffset,
		DataLength,
		Tree->NodeCount);
	QHPreviewTreeCollectOverlaps(
		Tree->Root,
		VolumeOffset,
		VolumeOffset + DataLength,
		hits,
		&hitCount,
		hitCapacity);
	QH_JOURNAL_DIAG(
		"collect end volumeOff=%llu len=%lu hits=%lu\n",
		VolumeOffset,
		DataLength,
		hitCount);
	QHPreviewSortHitsBySequence(hits, hitCount);
	QH_JOURNAL_DIAG(
		"sort end volumeOff=%llu len=%lu hits=%lu\n",
		VolumeOffset,
		DataLength,
		hitCount);

	for (i = 0; i < hitCount && covered < DataLength; ++i)
	{
		PQH_PREVIEW_TREE_NODE node = hits[i].Node;
		PVOID payloadBase = NULL;
		PUCHAR payload;
		UINT64 alignedSize;
		UINT64 overlapStart;
		UINT64 overlapEnd;
		UINT64 byteOffset;

		QH_JOURNAL_DIAG(
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
		alignedSize = QHAlignUp64(node->DataLength, Journal->SectorSize);
		payload = (PUCHAR)QHAllocateAligned(Journal,
			(SIZE_T)alignedSize,
			&payloadBase);
		if (!payload)
		{
			status = STATUS_INSUFFICIENT_RESOURCES;
			goto cleanup;
		}

		QH_JOURNAL_DIAG(
			"payload read begin index=%lu seq=%lu fileOff=%llu len=%lu\n",
			i,
			node->Sequence,
			node->FileOffset,
			(ULONG)alignedSize);
		status = QHJournalRawIo(
			Journal,
			IRP_MJ_READ,
			node->FileOffset,
			(ULONG)alignedSize,
			payload);
		QH_JOURNAL_DIAG(
			"payload read end index=%lu seq=%lu status=0x%08X\n",
			i,
			node->Sequence,
			status);
		if (!NT_SUCCESS(status))
		{
			qhfree(payloadBase);
			goto cleanup;
		}

		overlapStart = node->Start > VolumeOffset ? node->Start : VolumeOffset;
		overlapEnd = node->End < (VolumeOffset + DataLength) ?
			node->End : (VolumeOffset + DataLength);

		for (byteOffset = overlapStart; byteOffset < overlapEnd; ++byteOffset)
		{
			ULONG outputIndex = (ULONG)(byteOffset - VolumeOffset);
			if (!CoveredMask[outputIndex])
			{
				((PUCHAR)Buffer)[outputIndex] =
					payload[byteOffset - node->Start];
				CoveredMask[outputIndex] = 1;
				covered++;
			}
		}
		qhfree(payloadBase);
		QH_JOURNAL_DIAG(
			"hit end index=%lu seq=%lu covered=%lu\n",
			i,
			node->Sequence,
			covered);
	}

	*CoveredCount = covered;
	QH_JOURNAL_DIAG(
		"apply end volumeOff=%llu len=%lu hits=%lu covered=%lu "
		"status=0x%08X\n",
		VolumeOffset,
		DataLength,
		hitCount,
		covered,
		status);

cleanup:
	if (!NT_SUCCESS(status))
	{
		QH_JOURNAL_DIAG(
			"apply failed volumeOff=%llu len=%lu hits=%lu covered=%lu "
			"status=0x%08X\n",
			VolumeOffset,
			DataLength,
			hitCount,
			covered,
			status);
	}
	if (hits)
		qhfree(hits);
	return status;
}

NTSTATUS QHJournalReadPayload(
	_Inout_ PQH_JOURNAL Journal,
	_In_ UINT64 FileOffset,
	_In_ ULONG DataLength,
	_Out_writes_bytes_(DataLength) PVOID Buffer)
{
	UINT64 alignedSize;
	PVOID allocationBase = NULL;
	PUCHAR payload;
	NTSTATUS status;

	if (!Journal || !Journal->Mounted || !Buffer ||
		DataLength == 0 || DataLength > QH_JOURNAL_MAX_RECORD_DATA)
	{
		return STATUS_INVALID_PARAMETER;
	}

	alignedSize = QHAlignUp64(DataLength, Journal->SectorSize);
	payload = (PUCHAR)QHAllocateAligned(Journal,
		(SIZE_T)alignedSize,
		&allocationBase);
	if (!payload)
		return STATUS_INSUFFICIENT_RESOURCES;

	status = QHJournalRawIo(
		Journal,
		IRP_MJ_READ,
		FileOffset,
		(ULONG)alignedSize,
		payload);
	if (NT_SUCCESS(status))
		RtlCopyMemory(Buffer, payload, DataLength);

		qhfree(allocationBase);
	return status;
}

VOID QHJournalClose(_Inout_ PQH_JOURNAL Journal)
{
	QH_LOCK_ACQUIRE(&Journal->Lock);
	if (Journal->Mounted)
		(VOID)QHJournalWriteSuperblockLocked(Journal);
	Journal->Mounted = FALSE;
	Journal->TargetDevice = NULL;
	Journal->Store = NULL;
	QH_LOCK_RELEASE(&Journal->Lock);
	QH_LOCK_DELETE(&Journal->Lock);
}
