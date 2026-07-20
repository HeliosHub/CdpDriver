#include "QHEngineDefs.h"
#include "QHJournal.h"

#define QH_CRC32C_POLY 0x82F63B78UL

static ULONG g_QhCrc32cTable[256];
static volatile LONG g_QhCrc32cReady;

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
			KeStallExecutionProcessor(1);
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
	_In_ PDEVICE_OBJECT Device,
	_In_ SIZE_T Length,
	_Out_ PVOID* AllocationBase)
{
	SIZE_T alignment = (SIZE_T)Device->AlignmentRequirement + 1;
	ULONG_PTR address;

	if (alignment < sizeof(PVOID))
		alignment = sizeof(PVOID);
	*AllocationBase = qhalloc(Length + alignment - 1);
	if (!*AllocationBase)
		return NULL;
	address = ((ULONG_PTR)*AllocationBase + alignment - 1) & ~(alignment - 1);
	return (PVOID)address;
}

static NTSTATUS QHJournalRawIo(
	_In_ PQH_JOURNAL Journal,
	_In_ UCHAR MajorFunction,
	_In_ UINT64 Offset,
	_In_ ULONG Length,
	_Inout_updates_bytes_(Length) PVOID Buffer)
{
	KEVENT event;
	IO_STATUS_BLOCK iosb;
	LARGE_INTEGER byteOffset;
	PIRP irp;
	NTSTATUS status;

	if (!Journal->TargetDevice)
		return STATUS_DEVICE_NOT_READY;
	if (!Buffer || Length == 0 ||
		(Offset % Journal->SectorSize) != 0 ||
		(Length % Journal->SectorSize) != 0 ||
		Offset > Journal->PartitionSize ||
		Length > Journal->PartitionSize - Offset)
	{
		return STATUS_INVALID_PARAMETER;
	}

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

	if (NT_SUCCESS(status) && iosb.Information != Length)
		return STATUS_UNEXPECTED_IO_ERROR;
	return status;
}

// RMW for unaligned / sub-sector writes (anchors are 32 bytes).
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
		return QHJournalRawIo(Journal, IRP_MJ_WRITE, Offset, Length, (PVOID)Data);

	buf = (PUCHAR)QHAllocateAligned(Journal->TargetDevice, span, &allocationBase);
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

static NTSTATUS QHJournalFlush(_In_ PQH_JOURNAL Journal)
{
	KEVENT event;
	IO_STATUS_BLOCK iosb;
	PIRP irp;
	NTSTATUS status;

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

static VOID QHJournalComputeLayout(_Inout_ PQH_JOURNAL Journal)
{
	ULONG sec = Journal->SectorSize;

	Journal->IndexRegionOff = sec;
	Journal->IndexRegionSize = (UINT64)QH_JOURNAL_INDEX_SECTORS * sec;
	Journal->LogOffset = Journal->IndexRegionOff + Journal->IndexRegionSize;
	Journal->LogSize = QHAlignDown64(
		Journal->PartitionSize - sec - Journal->LogOffset,
		sec);
}

static NTSTATUS QHJournalWriteSuperblockLocked(_Inout_ PQH_JOURNAL Journal)
{
	PVOID allocationBase = NULL;
	PUCHAR sector;
	PQH_JOURNAL_SUPERBLOCK superblock;
	NTSTATUS status;

	sector = (PUCHAR)QHAllocateAligned(
		Journal->TargetDevice,
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
	superblock->IndexRegionOff = Journal->IndexRegionOff;
	superblock->IndexRegionSize = Journal->IndexRegionSize;
	superblock->LogOffset = Journal->LogOffset;
	superblock->LogSize = Journal->LogSize;
	superblock->LogHead = Journal->LogHead;
	superblock->LogTail = Journal->LogTail;
	superblock->IndexHead = Journal->IndexHead;
	superblock->IndexTail = Journal->IndexTail;
	superblock->IndexStride = Journal->IndexStride;
	superblock->NextSequence = Journal->NextSequence;
	superblock->DroppedRecords = Journal->DroppedRecords;
	superblock->OldestRecoverable100ns = Journal->Oldest100ns;
	superblock->NewestRecoverable100ns = Journal->Newest100ns;
	superblock->SourceVolumeGuid = Journal->SourceVolumeGuid;
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
	if (crc != Superblock->Crc32c ||
		RtlCompareMemory(
			&Superblock->SourceVolumeGuid,
			&Journal->SourceVolumeGuid,
			sizeof(GUID)) != sizeof(GUID))
	{
		return FALSE;
	}
	if (Superblock->IndexRegionOff < Journal->SectorSize ||
		(Superblock->IndexRegionOff % Journal->SectorSize) != 0 ||
		(Superblock->IndexRegionSize % Journal->SectorSize) != 0 ||
		Superblock->IndexRegionSize < Journal->SectorSize ||
		Superblock->LogOffset < Superblock->IndexRegionOff + Superblock->IndexRegionSize ||
		(Superblock->LogOffset % Journal->SectorSize) != 0 ||
		(Superblock->LogSize % Journal->SectorSize) != 0 ||
		Superblock->LogSize < Journal->SectorSize * 2ULL ||
		Superblock->LogOffset > Journal->PartitionSize ||
		Superblock->LogSize >
			Journal->PartitionSize - Journal->SectorSize - Superblock->LogOffset ||
		Superblock->LogHead >= Superblock->LogSize ||
		Superblock->LogTail >= Superblock->LogSize)
	{
		return FALSE;
	}
	return TRUE;
}

static UINT64 QHJournalUsedLocked(_In_ PQH_JOURNAL Journal)
{
	if (Journal->LogHead >= Journal->LogTail)
		return Journal->LogHead - Journal->LogTail;
	return Journal->LogSize - (Journal->LogTail - Journal->LogHead);
}

static UINT64 QHJournalFreeLocked(_In_ PQH_JOURNAL Journal)
{
	return Journal->LogSize - QHJournalUsedLocked(Journal) - Journal->SectorSize;
}

static NTSTATUS QHIndexPushBack(
	_Inout_ PQH_JOURNAL Journal,
	_In_ UINT64 Sequence,
	_In_ UINT64 Time,
	_In_ UINT64 LogOffset,
	_In_ UINT64 VolumeOffset,
	_In_ ULONG DataLength)
{
	if (Journal->IndexStart + Journal->IndexCount >= Journal->IndexCapacity)
	{
		if (Journal->IndexStart > 0)
		{
			RtlMoveMemory(
				Journal->IndexArray,
				Journal->IndexArray + Journal->IndexStart,
				Journal->IndexCount * sizeof(QH_INDEX_MEM_ENTRY));
			Journal->IndexStart = 0;
		}
		else
		{
			ULONG newCap = (Journal->IndexCapacity == 0) ?
				4096UL : Journal->IndexCapacity * 2;
			PQH_INDEX_MEM_ENTRY na = (PQH_INDEX_MEM_ENTRY)qhalloc(
				(SIZE_T)newCap * sizeof(QH_INDEX_MEM_ENTRY));
			if (!na)
				return STATUS_INSUFFICIENT_RESOURCES;
			if (Journal->IndexArray)
			{
				RtlCopyMemory(
					na,
					Journal->IndexArray + Journal->IndexStart,
					Journal->IndexCount * sizeof(QH_INDEX_MEM_ENTRY));
				qhfree(Journal->IndexArray);
			}
			Journal->IndexArray = na;
			Journal->IndexStart = 0;
			Journal->IndexCapacity = newCap;
		}
	}

	{
		PQH_INDEX_MEM_ENTRY e =
			&Journal->IndexArray[Journal->IndexStart + Journal->IndexCount];
		e->Sequence = Sequence;
		e->WallClock100ns = Time;
		e->LogRingOffset = LogOffset;
		e->VolumeOffset = VolumeOffset;
		e->DataLength = DataLength;
		e->Pad = 0;
		Journal->IndexCount++;
	}
	return STATUS_SUCCESS;
}

static VOID QHIndexPopFront(_Inout_ PQH_JOURNAL Journal)
{
	if (Journal->IndexCount > 0)
	{
		Journal->IndexStart++;
		Journal->IndexCount--;
		if (Journal->IndexCount == 0)
			Journal->IndexStart = 0;
	}
}

static VOID QHIndexFree(_Inout_ PQH_JOURNAL Journal)
{
	if (Journal->IndexArray)
	{
		qhfree(Journal->IndexArray);
		Journal->IndexArray = NULL;
	}
	Journal->IndexCapacity = 0;
	Journal->IndexCount = 0;
	Journal->IndexStart = 0;
}

static VOID QHIndexRefreshTimeBounds(_Inout_ PQH_JOURNAL Journal)
{
	if (Journal->IndexCount == 0)
	{
		Journal->Oldest100ns = 0;
		Journal->Newest100ns = 0;
		return;
	}
	Journal->Oldest100ns =
		Journal->IndexArray[Journal->IndexStart].WallClock100ns;
	Journal->Newest100ns =
		Journal->IndexArray[Journal->IndexStart + Journal->IndexCount - 1]
			.WallClock100ns;
}

static NTSTATUS QHJournalScanRebuildLocked(_Inout_ PQH_JOURNAL Journal)
{
	NTSTATUS status = STATUS_SUCCESS;
	PVOID allocationBase = NULL;
	PUCHAR headerSector;
	UINT64 position;
	ULONG guard = 0;

	headerSector = (PUCHAR)QHAllocateAligned(
		Journal->TargetDevice,
		Journal->SectorSize,
		&allocationBase);
	if (!headerSector)
		return STATUS_INSUFFICIENT_RESOURCES;

	Journal->IndexStart = 0;
	Journal->IndexCount = 0;
	Journal->RecsSinceAnchor = 0;

	position = Journal->LogTail;
	while (position != Journal->LogHead)
	{
		PQH_JOURNAL_RECORD_HEADER header;

		if (++guard > (ULONG)(Journal->LogSize / Journal->SectorSize) + 4)
		{
			status = STATUS_DISK_CORRUPT_ERROR;
			break;
		}

		status = QHJournalRawIo(
			Journal,
			IRP_MJ_READ,
			Journal->LogOffset + position,
			Journal->SectorSize,
			headerSector);
		if (!NT_SUCCESS(status))
			break;

		header = (PQH_JOURNAL_RECORD_HEADER)headerSector;
		if (header->Magic != QH_JOURNAL_RECORD_MAGIC ||
			header->RecordSize == 0 ||
			(header->RecordSize % Journal->SectorSize) != 0 ||
			header->RecordSize > Journal->LogSize - position)
		{
			Journal->LogHead = position;
			status = STATUS_SUCCESS;
			break;
		}
		if (QHCrc32c(
				0,
				header,
				FIELD_OFFSET(QH_JOURNAL_RECORD_HEADER, HeaderCrc32c)) !=
			header->HeaderCrc32c)
		{
			Journal->LogHead = position;
			status = STATUS_SUCCESS;
			break;
		}

		if (header->Flags & QH_JOURNAL_FLAG_WRAP)
		{
			position = 0;
			continue;
		}

		status = QHIndexPushBack(
			Journal,
			header->Sequence,
			header->WallClock100ns,
			position,
			header->VolumeOffset,
			header->DataLength);
		if (!NT_SUCCESS(status))
			break;
		if (header->Sequence >= Journal->NextSequence)
			Journal->NextSequence = header->Sequence + 1;

		Journal->RecsSinceAnchor++;
		if (Journal->RecsSinceAnchor >= Journal->IndexStride)
			Journal->RecsSinceAnchor = 0;

		position += header->RecordSize;
		if (position >= Journal->LogSize)
			position = 0;
	}

	QHIndexRefreshTimeBounds(Journal);
	qhfree(allocationBase);
	return status;
}

static NTSTATUS QHJournalDropOldestLocked(_Inout_ PQH_JOURNAL Journal)
{
	PVOID allocationBase = NULL;
	PUCHAR sector;
	PQH_JOURNAL_RECORD_HEADER header;
	ULONG crc;
	NTSTATUS status;

	if (Journal->LogTail == Journal->LogHead)
		return STATUS_NOT_FOUND;
	sector = (PUCHAR)QHAllocateAligned(
		Journal->TargetDevice,
		Journal->SectorSize,
		&allocationBase);
	if (!sector)
		return STATUS_INSUFFICIENT_RESOURCES;

	status = QHJournalRawIo(
		Journal,
		IRP_MJ_READ,
		Journal->LogOffset + Journal->LogTail,
		Journal->SectorSize,
		sector);
	header = (PQH_JOURNAL_RECORD_HEADER)sector;
	if (!NT_SUCCESS(status) ||
		header->Magic != QH_JOURNAL_RECORD_MAGIC ||
		header->RecordSize == 0 ||
		(header->RecordSize % Journal->SectorSize) != 0 ||
		header->RecordSize > Journal->LogSize - Journal->LogTail)
	{
		status = STATUS_DISK_CORRUPT_ERROR;
		goto cleanup;
	}
	crc = QHCrc32c(
		0,
		header,
		FIELD_OFFSET(QH_JOURNAL_RECORD_HEADER, HeaderCrc32c));
	if (crc != header->HeaderCrc32c)
	{
		status = STATUS_CRC_ERROR;
		goto cleanup;
	}

	if (header->Flags & QH_JOURNAL_FLAG_WRAP)
		Journal->LogTail = 0;
	else
	{
		Journal->LogTail += header->RecordSize;
		if (Journal->LogTail >= Journal->LogSize)
			Journal->LogTail = 0;
		Journal->DroppedRecords++;
		QHIndexPopFront(Journal);
		QHIndexRefreshTimeBounds(Journal);
	}
	status = STATUS_SUCCESS;

cleanup:
	qhfree(allocationBase);
	return status;
}

static NTSTATUS QHJournalWriteDiskAnchorLocked(
	_Inout_ PQH_JOURNAL Journal,
	_In_ UINT64 Sequence,
	_In_ UINT64 WallClock100ns,
	_In_ UINT64 LogRingOffset)
{
	QH_INDEX_ANCHOR anchor;
	UINT64 slots;
	UINT64 anchorSlot;

	if (Journal->IndexRegionSize < sizeof(QH_INDEX_ANCHOR))
		return STATUS_INVALID_PARAMETER;

	slots = Journal->IndexRegionSize / sizeof(QH_INDEX_ANCHOR);
	if (slots == 0)
		return STATUS_INVALID_PARAMETER;

	RtlZeroMemory(&anchor, sizeof(anchor));
	anchor.Sequence = Sequence;
	anchor.WallClock100ns = WallClock100ns;
	anchor.LogRingOffset = LogRingOffset;
	anchor.Crc32c = QHCrc32c(
		0,
		&anchor,
		FIELD_OFFSET(QH_INDEX_ANCHOR, Crc32c));

	anchorSlot = Journal->IndexHead % slots;
	return QHJournalRawWriteSub(
		Journal,
		Journal->IndexRegionOff + anchorSlot * sizeof(QH_INDEX_ANCHOR),
		sizeof(anchor),
		&anchor);
}

VOID QHJournalInitialize(
	_Out_ PQH_JOURNAL Journal,
	_In_ PDEVICE_OBJECT TargetDevice,
	_In_ UINT64 PartitionSize,
	_In_ ULONG SectorSize,
	_In_ const GUID* SourceVolumeGuid)
{
	RtlZeroMemory(Journal, sizeof(*Journal));
	Journal->TargetDevice = TargetDevice;
	Journal->PartitionSize = QHAlignDown64(PartitionSize, SectorSize);
	Journal->SectorSize = SectorSize;
	Journal->SourceVolumeGuid = *SourceVolumeGuid;
	Journal->IndexStride = QH_JOURNAL_DEFAULT_STRIDE;
	KeInitializeMutex(&Journal->Lock, 0);
}

NTSTATUS QHJournalFormat(_Inout_ PQH_JOURNAL Journal)
{
	PVOID allocationBase = NULL;
	PUCHAR zeroSector;
	NTSTATUS status;
	UINT64 minSize;

	minSize = (UINT64)Journal->SectorSize *
		(1ULL + QH_JOURNAL_INDEX_SECTORS + 2ULL);
	if (!Journal->TargetDevice ||
		(Journal->SectorSize != 512 && Journal->SectorSize != 4096) ||
		Journal->PartitionSize < minSize)
	{
		return STATUS_INVALID_PARAMETER;
	}

	KeWaitForSingleObject(&Journal->Lock, Executive, KernelMode, FALSE, NULL);
	QHIndexFree(Journal);
	QHJournalComputeLayout(Journal);
	if (Journal->LogSize < Journal->SectorSize * 2ULL)
	{
		status = STATUS_INVALID_PARAMETER;
		goto cleanup;
	}

	Journal->LogHead = 0;
	Journal->LogTail = 0;
	Journal->IndexHead = 0;
	Journal->IndexTail = 0;
	Journal->IndexStride = QH_JOURNAL_DEFAULT_STRIDE;
	Journal->RecsSinceAnchor = 0;
	Journal->NextSequence = 1;
	Journal->DroppedRecords = 0;
	Journal->Oldest100ns = 0;
	Journal->Newest100ns = 0;

	zeroSector = (PUCHAR)QHAllocateAligned(
		Journal->TargetDevice,
		Journal->SectorSize,
		&allocationBase);
	if (!zeroSector)
	{
		status = STATUS_INSUFFICIENT_RESOURCES;
		goto cleanup;
	}
	RtlZeroMemory(zeroSector, Journal->SectorSize);
	status = QHJournalRawIo(
		Journal,
		IRP_MJ_WRITE,
		Journal->LogOffset,
		Journal->SectorSize,
		zeroSector);
	if (NT_SUCCESS(status))
		status = QHJournalWriteSuperblockLocked(Journal);
	if (NT_SUCCESS(status))
		status = QHJournalFlush(Journal);
	if (NT_SUCCESS(status))
		Journal->Mounted = TRUE;

cleanup:
	if (allocationBase)
		qhfree(allocationBase);
	KeReleaseMutex(&Journal->Lock, FALSE);
	return status;
}

NTSTATUS QHJournalMount(_Inout_ PQH_JOURNAL Journal)
{
	PVOID allocationBase = NULL;
	PUCHAR sector;
	PQH_JOURNAL_SUPERBLOCK superblock;
	NTSTATUS status;
	UINT64 minSize;

	minSize = (UINT64)Journal->SectorSize *
		(1ULL + QH_JOURNAL_INDEX_SECTORS + 2ULL);
	if (!Journal->TargetDevice ||
		(Journal->SectorSize != 512 && Journal->SectorSize != 4096) ||
		Journal->PartitionSize < minSize)
	{
		return STATUS_INVALID_PARAMETER;
	}

	KeWaitForSingleObject(&Journal->Lock, Executive, KernelMode, FALSE, NULL);
	QHIndexFree(Journal);
	sector = (PUCHAR)QHAllocateAligned(
		Journal->TargetDevice,
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

	Journal->IndexRegionOff = superblock->IndexRegionOff;
	Journal->IndexRegionSize = superblock->IndexRegionSize;
	Journal->LogOffset = superblock->LogOffset;
	Journal->LogSize = superblock->LogSize;
	Journal->LogHead = superblock->LogHead;
	Journal->LogTail = superblock->LogTail;
	Journal->IndexHead = superblock->IndexHead;
	Journal->IndexTail = superblock->IndexTail;
	Journal->IndexStride = superblock->IndexStride ?
		superblock->IndexStride : QH_JOURNAL_DEFAULT_STRIDE;
	Journal->NextSequence = superblock->NextSequence;
	Journal->DroppedRecords = superblock->DroppedRecords;
	Journal->Oldest100ns = superblock->OldestRecoverable100ns;
	Journal->Newest100ns = superblock->NewestRecoverable100ns;

	status = QHJournalScanRebuildLocked(Journal);
	if (!NT_SUCCESS(status))
		goto cleanup;

	status = QHJournalWriteSuperblockLocked(Journal);
	if (NT_SUCCESS(status))
		Journal->Mounted = TRUE;

cleanup:
	if (allocationBase)
		qhfree(allocationBase);
	if (!NT_SUCCESS(status))
	{
		QHIndexFree(Journal);
		Journal->Mounted = FALSE;
	}
	KeReleaseMutex(&Journal->Lock, FALSE);
	return status;
}

NTSTATUS QHJournalAppend(
	_Inout_ PQH_JOURNAL Journal,
	_In_ UINT64 VolumeOffset,
	_In_ ULONG DataLength,
	_In_reads_bytes_(DataLength) const VOID* BeforeImage)
{
	PVOID allocationBase = NULL;
	PUCHAR recordBuffer;
	PQH_JOURNAL_RECORD_HEADER header;
	UINT64 recordSize64;
	ULONG recordSize;
	ULONG fillerSize = 0;
	UINT64 recordOffset;
	UINT64 writeSeq;
	UINT64 writeTime;
	LARGE_INTEGER wallClock;
	LARGE_INTEGER performanceCounter;
	NTSTATUS status = STATUS_SUCCESS;

	if (!Journal->Mounted || !BeforeImage ||
		DataLength == 0 || DataLength > QH_JOURNAL_MAX_RECORD_DATA)
	{
		return STATUS_INVALID_PARAMETER;
	}
	recordSize64 = QHAlignUp64(
		sizeof(QH_JOURNAL_RECORD_HEADER) + (UINT64)DataLength,
		Journal->SectorSize);
	if (recordSize64 > MAXULONG)
		return STATUS_INTEGER_OVERFLOW;
	recordSize = (ULONG)recordSize64;

	KeWaitForSingleObject(&Journal->Lock, Executive, KernelMode, FALSE, NULL);
	if (!Journal->Mounted)
	{
		status = STATUS_DEVICE_NOT_READY;
		goto cleanup;
	}
	if ((UINT64)recordSize + Journal->SectorSize >= Journal->LogSize)
	{
		status = STATUS_INSUFFICIENT_RESOURCES;
		goto cleanup;
	}
	if (Journal->LogHead + recordSize > Journal->LogSize)
		fillerSize = (ULONG)(Journal->LogSize - Journal->LogHead);

	while (QHJournalFreeLocked(Journal) < (UINT64)fillerSize + recordSize)
	{
		status = QHJournalDropOldestLocked(Journal);
		if (!NT_SUCCESS(status))
			goto cleanup;
	}

	if (fillerSize)
	{
		PVOID fillerBase = NULL;
		PUCHAR filler = (PUCHAR)QHAllocateAligned(
			Journal->TargetDevice,
			Journal->SectorSize,
			&fillerBase);
		if (!filler)
		{
			status = STATUS_INSUFFICIENT_RESOURCES;
			goto cleanup;
		}
		RtlZeroMemory(filler, Journal->SectorSize);
		header = (PQH_JOURNAL_RECORD_HEADER)filler;
		header->Magic = QH_JOURNAL_RECORD_MAGIC;
		header->RecordSize = fillerSize;
		header->Flags = QH_JOURNAL_FLAG_WRAP;
		header->HeaderCrc32c = QHCrc32c(
			0,
			header,
			FIELD_OFFSET(QH_JOURNAL_RECORD_HEADER, HeaderCrc32c));
		status = QHJournalRawIo(
			Journal,
			IRP_MJ_WRITE,
			Journal->LogOffset + Journal->LogHead,
			Journal->SectorSize,
			filler);
		qhfree(fillerBase);
		if (!NT_SUCCESS(status))
			goto cleanup;
		Journal->LogHead = 0;
	}

	recordBuffer = (PUCHAR)QHAllocateAligned(
		Journal->TargetDevice,
		recordSize,
		&allocationBase);
	if (!recordBuffer)
	{
		status = STATUS_INSUFFICIENT_RESOURCES;
		goto cleanup;
	}
	recordOffset = Journal->LogHead;
	writeSeq = Journal->NextSequence;
	KeQuerySystemTime(&wallClock);
	performanceCounter = KeQueryPerformanceCounter(NULL);
	writeTime = (UINT64)wallClock.QuadPart;

	RtlZeroMemory(recordBuffer, recordSize);
	header = (PQH_JOURNAL_RECORD_HEADER)recordBuffer;
	header->Magic = QH_JOURNAL_RECORD_MAGIC;
	header->RecordSize = recordSize;
	header->Sequence = writeSeq;
	header->WallClock100ns = writeTime;
	header->PerformanceCounter = (UINT64)performanceCounter.QuadPart;
	header->SourceVolumeGuid = Journal->SourceVolumeGuid;
	header->VolumeOffset = VolumeOffset;
	header->DataLength = DataLength;
	RtlCopyMemory(
		recordBuffer + sizeof(QH_JOURNAL_RECORD_HEADER),
		BeforeImage,
		DataLength);
	header->DataCrc32c = QHCrc32c(
		0,
		recordBuffer + sizeof(QH_JOURNAL_RECORD_HEADER),
		DataLength);
	header->HeaderCrc32c = QHCrc32c(
		0,
		header,
		FIELD_OFFSET(QH_JOURNAL_RECORD_HEADER, HeaderCrc32c));

	status = QHJournalRawIo(
		Journal,
		IRP_MJ_WRITE,
		Journal->LogOffset + recordOffset,
		recordSize,
		recordBuffer);
	if (!NT_SUCCESS(status))
		goto cleanup;

	status = QHIndexPushBack(
		Journal,
		writeSeq,
		writeTime,
		recordOffset,
		VolumeOffset,
		DataLength);
	if (!NT_SUCCESS(status))
		goto cleanup;

	Journal->NextSequence = writeSeq + 1;
	Journal->Newest100ns = writeTime;
	if (Journal->IndexCount == 1)
		Journal->Oldest100ns = writeTime;

	Journal->LogHead = recordOffset + recordSize;
	if (Journal->LogHead >= Journal->LogSize)
		Journal->LogHead = 0;

	Journal->RecsSinceAnchor++;
	if (Journal->RecsSinceAnchor >= Journal->IndexStride)
	{
		(VOID)QHJournalWriteDiskAnchorLocked(
			Journal,
			writeSeq,
			writeTime,
			recordOffset);
		Journal->IndexHead++;
		Journal->RecsSinceAnchor = 0;
	}

	status = QHJournalWriteSuperblockLocked(Journal);
	if (NT_SUCCESS(status))
		status = QHJournalFlush(Journal);

cleanup:
	if (allocationBase)
		qhfree(allocationBase);
	KeReleaseMutex(&Journal->Lock, FALSE);
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

	KeWaitForSingleObject(&Journal->Lock, Executive, KernelMode, FALSE, NULL);
	if (!Journal->Mounted)
		status = STATUS_DEVICE_NOT_READY;
	else if (Journal->IndexCount == 0)
		status = STATUS_NOT_FOUND;
	else
	{
		*OldestTime100ns = Journal->Oldest100ns;
		*NewestTime100ns = Journal->Newest100ns;
		status = STATUS_SUCCESS;
	}
	KeReleaseMutex(&Journal->Lock, FALSE);
	return status;
}

ULONG QHJournalFindFirstAfter(
	_In_ PQH_JOURNAL Journal,
	_In_ UINT64 TargetTime100ns)
{
	ULONG lo = 0;
	ULONG hi = Journal->IndexCount;
	PQH_INDEX_MEM_ENTRY base;

	if (Journal->IndexCount == 0 || !Journal->IndexArray)
		return 0;

	base = Journal->IndexArray + Journal->IndexStart;
	while (lo < hi)
	{
		ULONG mid = lo + (hi - lo) / 2;
		if (base[mid].WallClock100ns > TargetTime100ns)
			hi = mid;
		else
			lo = mid + 1;
	}
	return lo;
}

NTSTATUS QHJournalBuildPreview(
	_Inout_ PQH_JOURNAL Journal,
	_In_ UINT64 TargetTime100ns,
	_In_ UINT64 VolumeOffset,
	_In_ ULONG DataLength,
	_Out_writes_bytes_(DataLength) PVOID Buffer,
	_Out_writes_bytes_(DataLength) PUCHAR CoveredMask,
	_Out_ PULONG CoveredCount)
{
	NTSTATUS status = STATUS_SUCCESS;
	ULONG covered = 0;
	ULONG slot;
	ULONG startSlot;

	if (!Buffer || !CoveredMask || !CoveredCount || DataLength == 0 ||
		VolumeOffset > MAXUINT64 - DataLength)
	{
		return STATUS_INVALID_PARAMETER;
	}

	*CoveredCount = 0;
	RtlZeroMemory(CoveredMask, DataLength);

	KeWaitForSingleObject(&Journal->Lock, Executive, KernelMode, FALSE, NULL);
	if (!Journal->Mounted)
	{
		status = STATUS_DEVICE_NOT_READY;
		goto cleanup;
	}
	if (Journal->IndexCount == 0)
	{
		// Empty journal: caller fills everything from the live volume.
		status = STATUS_SUCCESS;
		goto cleanup;
	}
	if (TargetTime100ns < Journal->Oldest100ns)
	{
		status = STATUS_NOT_FOUND;
		goto cleanup;
	}
	if (TargetTime100ns >= Journal->Newest100ns)
	{
		// No write after T: caller fills everything from the live volume.
		status = STATUS_SUCCESS;
		goto cleanup;
	}

	// Binary search to the first record after T, then scan forward.
	// VolumeOffset/DataLength in the memory index skip non-overlapping
	// records without touching the journal disk.
	startSlot = QHJournalFindFirstAfter(Journal, TargetTime100ns);
	for (slot = startSlot;
		slot < Journal->IndexCount && covered < DataLength;
		++slot)
	{
		PQH_INDEX_MEM_ENTRY entry =
			&Journal->IndexArray[Journal->IndexStart + slot];
		PVOID recordBase = NULL;
		PUCHAR record;
		PUCHAR beforeImage;
		PQH_JOURNAL_RECORD_HEADER header;
		UINT64 overlapStart;
		UINT64 overlapEnd;
		UINT64 byteOffset;
		ULONG recordSize;

		if (entry->DataLength == 0 ||
			entry->DataLength > QH_JOURNAL_MAX_RECORD_DATA ||
			entry->VolumeOffset > MAXUINT64 - entry->DataLength ||
			entry->VolumeOffset >= VolumeOffset + DataLength ||
			VolumeOffset >= entry->VolumeOffset + entry->DataLength)
		{
			continue;
		}

		recordSize = (ULONG)QHAlignUp64(
			sizeof(QH_JOURNAL_RECORD_HEADER) + (UINT64)entry->DataLength,
			Journal->SectorSize);
		record = (PUCHAR)QHAllocateAligned(
			Journal->TargetDevice,
			recordSize,
			&recordBase);
		if (!record)
		{
			status = STATUS_INSUFFICIENT_RESOURCES;
			goto cleanup;
		}

		status = QHJournalRawIo(
			Journal,
			IRP_MJ_READ,
			Journal->LogOffset + entry->LogRingOffset,
			recordSize,
			record);
		if (!NT_SUCCESS(status))
		{
			qhfree(recordBase);
			goto cleanup;
		}

		header = (PQH_JOURNAL_RECORD_HEADER)record;
		beforeImage = record + sizeof(*header);
		if (header->Magic != QH_JOURNAL_RECORD_MAGIC ||
			header->Sequence != entry->Sequence ||
			header->DataLength != entry->DataLength ||
			header->VolumeOffset != entry->VolumeOffset ||
			QHCrc32c(
				0,
				header,
				FIELD_OFFSET(QH_JOURNAL_RECORD_HEADER, HeaderCrc32c)) !=
				header->HeaderCrc32c ||
			QHCrc32c(0, beforeImage, header->DataLength) !=
				header->DataCrc32c)
		{
			qhfree(recordBase);
			status = STATUS_DISK_CORRUPT_ERROR;
			goto cleanup;
		}

		overlapStart = header->VolumeOffset > VolumeOffset ?
			header->VolumeOffset : VolumeOffset;
		overlapEnd =
			(header->VolumeOffset + header->DataLength) <
			(VolumeOffset + DataLength) ?
			(header->VolumeOffset + header->DataLength) :
			(VolumeOffset + DataLength);

		for (byteOffset = overlapStart; byteOffset < overlapEnd; ++byteOffset)
		{
			ULONG outputIndex = (ULONG)(byteOffset - VolumeOffset);
			if (!CoveredMask[outputIndex])
			{
				((PUCHAR)Buffer)[outputIndex] =
					beforeImage[byteOffset - header->VolumeOffset];
				CoveredMask[outputIndex] = 1;
				covered++;
			}
		}
		qhfree(recordBase);
	}

	*CoveredCount = covered;

cleanup:
	KeReleaseMutex(&Journal->Lock, FALSE);
	return status;
}

VOID QHJournalClose(_Inout_ PQH_JOURNAL Journal)
{
	KeWaitForSingleObject(&Journal->Lock, Executive, KernelMode, FALSE, NULL);
	if (Journal->Mounted)
		(VOID)QHJournalWriteSuperblockLocked(Journal);
	Journal->Mounted = FALSE;
	Journal->TargetDevice = NULL;
	QHIndexFree(Journal);
	KeReleaseMutex(&Journal->Lock, FALSE);
}
