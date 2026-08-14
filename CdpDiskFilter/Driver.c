#include <ntddk.h>
#include <ntdddisk.h>
#include <ntddstor.h>
#include <ntstrsafe.h>
#include "CdpDiskFilterIoctl.h"

#define CDP_TAG 'kDcC'
#define CDP_DISK_SIGNATURE 'DdcC'
#define CDP_CONTROL_SIGNATURE 'CdcC'
#define CDP_MAX_IO_BYTES (4UL * 1024UL * 1024UL)
#define CDP_RECORD_REGION_BYTES (1UL * 1024UL * 1024UL)
#define CDP_RECORD_HEADER_BYTES 32UL
#define CDP_RECORD_REGION_LINK_BYTES 32UL
#define CDP_RECORDS_PER_REGION \
	((CDP_RECORD_REGION_BYTES - CDP_RECORD_REGION_LINK_BYTES) / \
	 CDP_RECORD_HEADER_BYTES)

#define CDP_LOG(...) DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, \
	"CdpDiskFilter: " __VA_ARGS__)

typedef struct _CDP_MAP_ITEM
{
	LIST_ENTRY Entry;
	ULONGLONG SourceStart;
	ULONGLONG SourceEnd;
	ULONGLONG JournalStart;
} CDP_MAP_ITEM, *PCDP_MAP_ITEM;

typedef struct _CDP_IO_ITEM
{
	LIST_ENTRY Entry;
	PIRP Irp;
} CDP_IO_ITEM, *PCDP_IO_ITEM;

#pragma pack(push, 1)
typedef struct _CDP_DISK_RECORD_HEADER
{
	ULONGLONG WallClock100ns;
	ULONGLONG VolumeOffset;
	ULONGLONG JournalOffset;
	ULONG DataLength;
	ULONG Sequence;
} CDP_DISK_RECORD_HEADER, *PCDP_DISK_RECORD_HEADER;
#pragma pack(pop)

C_ASSERT(sizeof(CDP_DISK_RECORD_HEADER) == CDP_RECORD_HEADER_BYTES);

typedef struct _CDP_DISK_EXTENSION
{
	ULONG Signature;
	PDEVICE_OBJECT FilterDevice;
	PDEVICE_OBJECT LowerDevice;
	PDEVICE_OBJECT PhysicalDevice;
	LIST_ENTRY GlobalEntry;
	ULONG DiskNumber;
	ULONG PartitionStyle;
	ULONG MbrSignature;
	GUID DiskGuid;
	ULONG SectorSize;
	ULONGLONG DiskLength;
	BOOLEAN IdentityKnown;
	BOOLEAN Enabled;
	BOOLEAN Stopping;
	ULONGLONG SourceStart;
	ULONGLONG SourceLength;
	ULONGLONG JournalStart;
	ULONGLONG JournalLength;
	ULONGLONG JournalBytesUsed;
	ULONGLONG MappingItemCount;
	PUCHAR HeaderSectorCache;
	ULONGLONG HeaderSectorCacheOffset;
	BOOLEAN HeaderSectorCacheValid;
	LIST_ENTRY MappingList;
	KMUTEX HistoryMutex;
	KSPIN_LOCK QueueLock;
	LIST_ENTRY Queue;
	KEVENT QueueEvent;
	HANDLE WorkerHandle;
} CDP_DISK_EXTENSION, *PCDP_DISK_EXTENSION;

typedef struct _CDP_CONTROL_EXTENSION
{
	ULONG Signature;
} CDP_CONTROL_EXTENSION, *PCDP_CONTROL_EXTENSION;

typedef struct _CDP_GLOBAL
{
	PDRIVER_OBJECT DriverObject;
	PDEVICE_OBJECT ControlDevice;
	UNICODE_STRING SymbolicLink;
	FAST_MUTEX DiskListMutex;
	LIST_ENTRY DiskList;
} CDP_GLOBAL;

static CDP_GLOBAL g_Cdp;

DRIVER_INITIALIZE DriverEntry;
DRIVER_ADD_DEVICE CdpAddDevice;
DRIVER_UNLOAD CdpUnload;

static NTSTATUS CdpCompleteIrp(PIRP Irp, NTSTATUS Status, ULONG_PTR Information)
{
	Irp->IoStatus.Status = Status;
	Irp->IoStatus.Information = Information;
	IoCompleteRequest(Irp, IO_NO_INCREMENT);
	return Status;
}

static NTSTATUS CdpForward(PDEVICE_OBJECT Lower, PIRP Irp)
{
	IoSkipCurrentIrpStackLocation(Irp);
	return IoCallDriver(Lower, Irp);
}

static PVOID CdpAllocate(SIZE_T Bytes)
{
	return ExAllocatePool2(POOL_FLAG_NON_PAGED, Bytes, CDP_TAG);
}

static VOID CdpFree(PVOID Buffer)
{
	if (Buffer)
		ExFreePoolWithTag(Buffer, CDP_TAG);
}

static BOOLEAN CdpRangesOverlap(
	ULONGLONG StartA, ULONGLONG LengthA,
	ULONGLONG StartB, ULONGLONG LengthB)
{
	return StartA < StartB + LengthB && StartB < StartA + LengthA;
}

static NTSTATUS CdpRawIo(
	PCDP_DISK_EXTENSION Ext,
	UCHAR MajorFunction,
	ULONGLONG Offset,
	ULONG Length,
	PVOID Buffer)
{
	KEVENT event;
	IO_STATUS_BLOCK iosb;
	LARGE_INTEGER byteOffset;
	PIRP irp;
	NTSTATUS status;

	if (!Ext || !Ext->LowerDevice || !Buffer || Length == 0 ||
		Offset > MAXULONGLONG - Length)
		return STATUS_INVALID_PARAMETER;
	KeInitializeEvent(&event, NotificationEvent, FALSE);
	RtlZeroMemory(&iosb, sizeof(iosb));
	byteOffset.QuadPart = (LONGLONG)Offset;
	irp = IoBuildSynchronousFsdRequest(
		MajorFunction, Ext->LowerDevice, Buffer, Length,
		&byteOffset, &event, &iosb);
	if (!irp)
		return STATUS_INSUFFICIENT_RESOURCES;
	status = IoCallDriver(Ext->LowerDevice, irp);
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
		status = STATUS_UNEXPECTED_IO_ERROR;
	return status;
}

static VOID CdpClearMappings(PCDP_DISK_EXTENSION Ext)
{
	while (!IsListEmpty(&Ext->MappingList))
	{
		PLIST_ENTRY entry = RemoveHeadList(&Ext->MappingList);
		PCDP_MAP_ITEM item = CONTAINING_RECORD(entry, CDP_MAP_ITEM, Entry);
		CdpFree(item);
	}
	Ext->MappingItemCount = 0;
	Ext->JournalBytesUsed = 0;
}

static NTSTATUS CdpMapOriginalBuffer(
	PIRP Irp,
	ULONG Length,
	PVOID* Buffer)
{
	PVOID mapped;
	if (!Irp->MdlAddress || Length == 0)
		return STATUS_INVALID_PARAMETER;
	mapped = MmGetSystemAddressForMdlSafe(
		Irp->MdlAddress, NormalPagePriority | MdlMappingNoExecute);
	if (!mapped || MmGetMdlByteCount(Irp->MdlAddress) < Length)
		return STATUS_INSUFFICIENT_RESOURCES;
	*Buffer = mapped;
	return STATUS_SUCCESS;
}

static NTSTATUS CdpWriteRecordHeader(
	PCDP_DISK_EXTENSION Ext,
	ULONGLONG RecordIndex,
	ULONGLONG SourceOffset,
	ULONGLONG JournalOffset,
	ULONG Length)
{
	CDP_DISK_RECORD_HEADER header;
	LARGE_INTEGER systemTime;
	LARGE_INTEGER localTime;
	ULONGLONG headerRelativeOffset;
	ULONGLONG sectorRelativeOffset;
	ULONG offsetInSector;
	NTSTATUS status;

	if (RecordIndex >= CDP_RECORDS_PER_REGION)
		return STATUS_DISK_FULL;
	headerRelativeOffset = RecordIndex * CDP_RECORD_HEADER_BYTES;
	sectorRelativeOffset = headerRelativeOffset -
		(headerRelativeOffset % Ext->SectorSize);
	offsetInSector = (ULONG)(headerRelativeOffset - sectorRelativeOffset);
	if (!Ext->HeaderSectorCache)
	{
		Ext->HeaderSectorCache = (PUCHAR)CdpAllocate(Ext->SectorSize);
		if (!Ext->HeaderSectorCache)
			return STATUS_INSUFFICIENT_RESOURCES;
	}
	if (!Ext->HeaderSectorCacheValid ||
		Ext->HeaderSectorCacheOffset != sectorRelativeOffset)
	{
		status = CdpRawIo(
			Ext, IRP_MJ_READ,
			Ext->JournalStart + sectorRelativeOffset,
			Ext->SectorSize, Ext->HeaderSectorCache);
		if (!NT_SUCCESS(status))
			return status;
		Ext->HeaderSectorCacheOffset = sectorRelativeOffset;
		Ext->HeaderSectorCacheValid = TRUE;
	}
	KeQuerySystemTimePrecise(&systemTime);
	ExSystemTimeToLocalTime(&systemTime, &localTime);
	RtlZeroMemory(&header, sizeof(header));
	header.WallClock100ns = (ULONGLONG)localTime.QuadPart;
	header.VolumeOffset = SourceOffset - Ext->SourceStart;
	header.JournalOffset = JournalOffset - Ext->JournalStart;
	header.DataLength = Length;
	header.Sequence = (ULONG)RecordIndex;
	RtlCopyMemory(
		Ext->HeaderSectorCache + offsetInSector, &header, sizeof(header));
	status = CdpRawIo(
		Ext, IRP_MJ_WRITE,
		Ext->JournalStart + sectorRelativeOffset,
		Ext->SectorSize, Ext->HeaderSectorCache);
	return status;
}

static NTSTATUS CdpProcessWrite(
	PCDP_DISK_EXTENSION Ext,
	PIRP Irp,
	ULONGLONG Offset,
	ULONG Length)
{
	PCDP_MAP_ITEM mapItem = NULL;
	PVOID original = NULL;
	PVOID snapshot = NULL;
	ULONGLONG journalOffset;
	NTSTATUS status;

	if (Length > CDP_MAX_IO_BYTES)
		return STATUS_INVALID_BUFFER_SIZE;
	status = CdpMapOriginalBuffer(Irp, Length, &original);
	if (!NT_SUCCESS(status))
		return status;
	snapshot = CdpAllocate(Length);
	mapItem = (PCDP_MAP_ITEM)CdpAllocate(sizeof(*mapItem));
	if (!snapshot || !mapItem)
	{
		CdpFree(snapshot);
		CdpFree(mapItem);
		return STATUS_INSUFFICIENT_RESOURCES;
	}
	RtlCopyMemory(snapshot, original, Length);

	if (Ext->JournalLength <= CDP_RECORD_REGION_BYTES ||
		Ext->MappingItemCount >= CDP_RECORDS_PER_REGION ||
		Ext->JournalBytesUsed >
			Ext->JournalLength - CDP_RECORD_REGION_BYTES ||
		Length > Ext->JournalLength - CDP_RECORD_REGION_BYTES -
			Ext->JournalBytesUsed)
	{
		status = STATUS_DISK_FULL;
		goto done;
	}
	journalOffset = Ext->JournalStart + CDP_RECORD_REGION_BYTES +
		Ext->JournalBytesUsed;
	status = CdpRawIo(Ext, IRP_MJ_WRITE, journalOffset, Length, snapshot);
	if (!NT_SUCCESS(status))
		goto done;
	status = CdpWriteRecordHeader(
		Ext, Ext->MappingItemCount, Offset, journalOffset, Length);
	if (!NT_SUCCESS(status))
		goto done;

	mapItem->SourceStart = Offset;
	mapItem->SourceEnd = Offset + Length;
	mapItem->JournalStart = journalOffset;
	InsertTailList(&Ext->MappingList, &mapItem->Entry);
	mapItem = NULL;
	Ext->MappingItemCount++;
	Ext->JournalBytesUsed += Length;

done:
	CdpFree(mapItem);
	CdpFree(snapshot);
	return status;
}

static BOOLEAN CdpSectorCovered(PUCHAR Bitmap, ULONG Sector)
{
	return (Bitmap[Sector >> 3] & (UCHAR)(1U << (Sector & 7))) != 0;
}

static VOID CdpSetSectorCovered(PUCHAR Bitmap, ULONG Sector)
{
	Bitmap[Sector >> 3] |= (UCHAR)(1U << (Sector & 7));
}

static NTSTATUS CdpProcessRead(
	PCDP_DISK_EXTENSION Ext,
	PIRP Irp,
	ULONGLONG Offset,
	ULONG Length)
{
	PVOID original = NULL;
	PUCHAR view = NULL;
	PUCHAR covered = NULL;
	ULONG sectorCount;
	ULONG bitmapBytes;
	PLIST_ENTRY entry;
	NTSTATUS status;

	if (Length > CDP_MAX_IO_BYTES || Length % Ext->SectorSize != 0)
		return STATUS_INVALID_BUFFER_SIZE;
	status = CdpMapOriginalBuffer(Irp, Length, &original);
	if (!NT_SUCCESS(status))
		return status;
	view = (PUCHAR)CdpAllocate(Length);
	sectorCount = Length / Ext->SectorSize;
	bitmapBytes = (sectorCount + 7UL) / 8UL;
	covered = (PUCHAR)CdpAllocate(bitmapBytes);
	if (!view || !covered)
	{
		status = STATUS_INSUFFICIENT_RESOURCES;
		goto done;
	}
	RtlZeroMemory(covered, bitmapBytes);
	status = CdpRawIo(Ext, IRP_MJ_READ, Offset, Length, view);
	if (!NT_SUCCESS(status))
		goto done;

	for (entry = Ext->MappingList.Blink;
		entry != &Ext->MappingList;
		entry = entry->Blink)
	{
		PCDP_MAP_ITEM item = CONTAINING_RECORD(entry, CDP_MAP_ITEM, Entry);
		ULONGLONG hitStart;
		ULONGLONG hitEnd;
		ULONG firstSector;
		ULONG endSector;
		ULONG cursor;

		if (item->SourceEnd <= Offset || item->SourceStart >= Offset + Length)
			continue;
		hitStart = item->SourceStart > Offset ? item->SourceStart : Offset;
		hitEnd = item->SourceEnd < Offset + Length ?
			item->SourceEnd : Offset + Length;
		firstSector = (ULONG)((hitStart - Offset) / Ext->SectorSize);
		endSector = (ULONG)((hitEnd - Offset) / Ext->SectorSize);
		cursor = firstSector;
		while (cursor < endSector)
		{
			ULONG runStart;
			ULONG runEnd;
			ULONG runLength;
			ULONGLONG sourceAtRun;
			ULONGLONG journalAtRun;

			while (cursor < endSector && CdpSectorCovered(covered, cursor))
				cursor++;
			if (cursor == endSector)
				break;
			runStart = cursor;
			while (cursor < endSector && !CdpSectorCovered(covered, cursor))
				cursor++;
			runEnd = cursor;
			runLength = (runEnd - runStart) * Ext->SectorSize;
			sourceAtRun = Offset + (ULONGLONG)runStart * Ext->SectorSize;
			journalAtRun = item->JournalStart +
				(sourceAtRun - item->SourceStart);
			status = CdpRawIo(
				Ext, IRP_MJ_READ, journalAtRun, runLength,
				view + (SIZE_T)runStart * Ext->SectorSize);
			if (!NT_SUCCESS(status))
				goto done;
			while (runStart < runEnd)
				CdpSetSectorCovered(covered, runStart++);
		}
	}
	RtlCopyMemory(original, view, Length);

done:
	CdpFree(covered);
	CdpFree(view);
	return status;
}

static VOID CdpWorker(PVOID Context)
{
	PCDP_DISK_EXTENSION ext = (PCDP_DISK_EXTENSION)Context;
	for (;;)
	{
		PLIST_ENTRY entry = NULL;
		KIRQL oldIrql;
		KeWaitForSingleObject(
			&ext->QueueEvent, Executive, KernelMode, FALSE, NULL);
		for (;;)
		{
			KeAcquireSpinLock(&ext->QueueLock, &oldIrql);
			if (!IsListEmpty(&ext->Queue))
				entry = RemoveHeadList(&ext->Queue);
			else
			{
				KeClearEvent(&ext->QueueEvent);
				entry = NULL;
			}
			KeReleaseSpinLock(&ext->QueueLock, oldIrql);
			if (!entry)
				break;
			{
				PCDP_IO_ITEM io = CONTAINING_RECORD(entry, CDP_IO_ITEM, Entry);
				PIRP irp = io->Irp;
				PIO_STACK_LOCATION irpSp = IoGetCurrentIrpStackLocation(irp);
				UCHAR major = irpSp->MajorFunction;
				ULONGLONG offset = (ULONGLONG)
					(major == IRP_MJ_READ ?
					 irpSp->Parameters.Read.ByteOffset.QuadPart :
					 irpSp->Parameters.Write.ByteOffset.QuadPart);
				ULONG length = major == IRP_MJ_READ ?
					irpSp->Parameters.Read.Length :
					irpSp->Parameters.Write.Length;
				NTSTATUS status;
				CdpFree(io);
				KeWaitForSingleObject(
					&ext->HistoryMutex, Executive, KernelMode, FALSE, NULL);
				if (!ext->Enabled || ext->Stopping)
					status = STATUS_DEVICE_NOT_READY;
				else if (major == IRP_MJ_READ)
					status = CdpProcessRead(ext, irp, offset, length);
				else
					status = CdpProcessWrite(ext, irp, offset, length);
				KeReleaseMutex(&ext->HistoryMutex, FALSE);
				CdpCompleteIrp(
					irp, status, NT_SUCCESS(status) ? length : 0);
			}
		}
		if (ext->Stopping)
			break;
	}
	PsTerminateSystemThread(STATUS_SUCCESS);
}

static NTSTATUS CdpStartWorker(PCDP_DISK_EXTENSION Ext)
{
	return PsCreateSystemThread(
		&Ext->WorkerHandle,
		THREAD_ALL_ACCESS,
		NULL, NULL, NULL,
		CdpWorker,
		Ext);
}

static VOID CdpStopWorker(PCDP_DISK_EXTENSION Ext)
{
	HANDLE handle = Ext->WorkerHandle;
	PVOID threadObject = NULL;
	if (!handle)
		return;
	(void)ObReferenceObjectByHandle(
		handle,
		SYNCHRONIZE,
		*PsThreadType,
		KernelMode,
		&threadObject,
		NULL);
	Ext->Stopping = TRUE;
	KeSetEvent(&Ext->QueueEvent, IO_NO_INCREMENT, FALSE);
	if (threadObject)
	{
		KeWaitForSingleObject(
			threadObject, Executive, KernelMode, FALSE, NULL);
		ObDereferenceObject(threadObject);
	}
	ZwClose(handle);
	Ext->WorkerHandle = NULL;
}

static PCDP_DISK_EXTENSION CdpFindDisk(
	ULONG DiskNumber,
	ULONG PartitionStyle,
	const GUID* DiskGuid,
	ULONG MbrSignature,
	ULONG SectorSize)
{
	WCHAR nameBuffer[64];
	UNICODE_STRING deviceName;
	PFILE_OBJECT fileObject = NULL;
	PDEVICE_OBJECT topDevice = NULL;
	PCDP_DISK_EXTENSION found = NULL;
	NTSTATUS status;

	UNREFERENCED_PARAMETER(PartitionStyle);
	UNREFERENCED_PARAMETER(DiskGuid);
	UNREFERENCED_PARAMETER(MbrSignature);
	UNREFERENCED_PARAMETER(SectorSize);
	status = RtlStringCchPrintfW(
		nameBuffer, RTL_NUMBER_OF(nameBuffer),
		L"\\Device\\Harddisk%lu\\DR%lu", DiskNumber, DiskNumber);
	if (!NT_SUCCESS(status))
		return NULL;
	RtlInitUnicodeString(&deviceName, nameBuffer);
	status = IoGetDeviceObjectPointer(
		&deviceName,
		FILE_READ_ATTRIBUTES,
		&fileObject,
		&topDevice);
	if (!NT_SUCCESS(status))
	{
		CDP_LOG("open disk stack failed disk=%lu status=0x%08X\n",
			DiskNumber, status);
		return NULL;
	}
	ExAcquireFastMutex(&g_Cdp.DiskListMutex);
	{
		PLIST_ENTRY entry;
		for (entry = g_Cdp.DiskList.Flink;
			entry != &g_Cdp.DiskList;
			entry = entry->Flink)
		{
			PCDP_DISK_EXTENSION ext = CONTAINING_RECORD(
				entry, CDP_DISK_EXTENSION, GlobalEntry);
			PDEVICE_OBJECT instanceTop =
				IoGetAttachedDeviceReference(ext->PhysicalDevice);
			if (instanceTop == topDevice)
			{
				found = ext;
			}
			ObDereferenceObject(instanceTop);
			if (found)
				break;
		}
	}
	ExReleaseFastMutex(&g_Cdp.DiskListMutex);
	ObDereferenceObject(fileObject);
	if (!found)
		CDP_LOG("filter instance not present in disk=%lu named stack\n",
			DiskNumber);
	return found;
}

static NTSTATUS CdpConfigureDisk(const CDP_DISK_FILTER_CONFIG* Config)
{
	PCDP_DISK_EXTENSION ext;
	ULONGLONG sourceEnd;
	ULONGLONG journalEnd;
	NTSTATUS status = STATUS_SUCCESS;

	if (!Config || Config->Version != CDP_DISK_FILTER_CONFIG_VERSION ||
		Config->SourceLength == 0 ||
		Config->JournalLength <= CDP_RECORD_REGION_BYTES ||
		Config->SourceStart > MAXULONGLONG - Config->SourceLength ||
		Config->JournalStart > MAXULONGLONG - Config->JournalLength)
		return STATUS_INVALID_PARAMETER;
	ext = CdpFindDisk(
		Config->DiskNumber,
		Config->PartitionStyle,
		&Config->DiskGuid,
		Config->MbrSignature,
		Config->SectorSize);
	if (!ext)
		return STATUS_NOT_FOUND;
	ext->DiskNumber = Config->DiskNumber;
	ext->PartitionStyle = Config->PartitionStyle;
	ext->MbrSignature = Config->MbrSignature;
	ext->DiskGuid = Config->DiskGuid;
	ext->DiskLength = Config->DiskLength;
	ext->SectorSize = Config->SectorSize;
	sourceEnd = Config->SourceStart + Config->SourceLength;
	journalEnd = Config->JournalStart + Config->JournalLength;
	if (sourceEnd > ext->DiskLength || journalEnd > ext->DiskLength ||
		CdpRangesOverlap(
			Config->SourceStart, Config->SourceLength,
			Config->JournalStart, Config->JournalLength) ||
		(ext->SectorSize != 512 && ext->SectorSize != 4096) ||
		(Config->SourceStart % ext->SectorSize) != 0 ||
		(Config->SourceLength % ext->SectorSize) != 0 ||
		(Config->JournalStart % ext->SectorSize) != 0 ||
		(Config->JournalLength % ext->SectorSize) != 0)
		return STATUS_INVALID_PARAMETER;

	KeWaitForSingleObject(&ext->HistoryMutex, Executive, KernelMode, FALSE, NULL);
	ext->Enabled = FALSE;
	CdpClearMappings(ext);
	ext->SourceStart = Config->SourceStart;
	ext->SourceLength = Config->SourceLength;
	ext->JournalStart = Config->JournalStart;
	ext->JournalLength = Config->JournalLength;
	ext->HeaderSectorCacheValid = FALSE;
	ext->Enabled = TRUE;
	KeReleaseMutex(&ext->HistoryMutex, FALSE);
	CDP_LOG("configured disk=%lu sector=%lu source=[%llu,%llu) journal=[%llu,%llu)\n",
		ext->DiskNumber, ext->SectorSize,
		ext->SourceStart, ext->SourceStart + ext->SourceLength,
		ext->JournalStart, ext->JournalStart + ext->JournalLength);
	return status;
}

static VOID CdpBuildDiagnostics(
	const CDP_DISK_FILTER_DIAGNOSTIC_REQUEST* Request,
	PCDP_DISK_FILTER_DIAGNOSTIC_REPLY Reply)
{
	PLIST_ENTRY listEntry;
	ULONG instanceIndex = 0;

	RtlZeroMemory(Reply, sizeof(*Reply));
	Reply->Version = CDP_DISK_FILTER_CONFIG_VERSION;
	Reply->RequestedDiskNumber = Request->DiskNumber;
	Reply->RequestedPartitionStyle = Request->PartitionStyle;
	Reply->RequestedMbrSignature = Request->MbrSignature;
	Reply->RequestedDiskGuid = Request->DiskGuid;
	Reply->RequestedSectorSize = Request->SectorSize;
	ExAcquireFastMutex(&g_Cdp.DiskListMutex);
	for (listEntry = g_Cdp.DiskList.Flink;
		listEntry != &g_Cdp.DiskList;
		listEntry = listEntry->Flink, ++instanceIndex)
	{
		PCDP_DISK_EXTENSION ext = CONTAINING_RECORD(
			listEntry, CDP_DISK_EXTENSION, GlobalEntry);
		PCDP_DISK_FILTER_DIAGNOSTIC_ENTRY diagnostic;
		Reply->AttachedInstanceCount++;
		if (Reply->ReturnedInstanceCount >=
			CDP_DISK_FILTER_MAX_DIAGNOSTIC_INSTANCES)
			continue;
		diagnostic = &Reply->Instances[Reply->ReturnedInstanceCount++];
		diagnostic->InstanceIndex = instanceIndex;
		diagnostic->DeviceNumberStatus = STATUS_NOT_SUPPORTED;
		diagnostic->DeviceNumber = MAXULONG;
		diagnostic->IdentityReadStatus = STATUS_NOT_SUPPORTED;
		diagnostic->LowerDeviceType = ext->LowerDevice ?
			ext->LowerDevice->DeviceType : 0;
		diagnostic->LowerStackSize = ext->LowerDevice ?
			ext->LowerDevice->StackSize : 0;
		diagnostic->FilterStackSize = ext->FilterDevice ?
			ext->FilterDevice->StackSize : 0;
	}
	ExReleaseFastMutex(&g_Cdp.DiskListMutex);
}

static PCDP_DISK_EXTENSION CdpAcquireConfiguredDiskByNumber(
	ULONG DiskNumber)
{
	PLIST_ENTRY entry;
	PCDP_DISK_EXTENSION found = NULL;

	ExAcquireFastMutex(&g_Cdp.DiskListMutex);
	for (entry = g_Cdp.DiskList.Flink;
		entry != &g_Cdp.DiskList;
		entry = entry->Flink)
	{
		PCDP_DISK_EXTENSION ext = CONTAINING_RECORD(
			entry, CDP_DISK_EXTENSION, GlobalEntry);
		if (ext->Enabled && ext->DiskNumber == DiskNumber)
		{
			KeWaitForSingleObject(
				&ext->HistoryMutex, Executive, KernelMode, FALSE, NULL);
			found = ext;
			break;
		}
	}
	ExReleaseFastMutex(&g_Cdp.DiskListMutex);
	return found;
}

static NTSTATUS CdpQueryRecords(
	const CDP_DISK_FILTER_RECORDS_REQUEST* Request,
	PCDP_DISK_FILTER_RECORDS_REPLY Reply)
{
	PCDP_DISK_EXTENSION ext;
	PUCHAR sectorBuffer = NULL;
	ULONGLONG loadedSector = MAXULONGLONG;
	ULONGLONG index;
	NTSTATUS status = STATUS_SUCCESS;

	ext = CdpAcquireConfiguredDiskByNumber(Request->DiskNumber);
	if (!ext)
		return STATUS_NOT_FOUND;
	RtlZeroMemory(Reply, sizeof(*Reply));
	Reply->Version = CDP_DISK_FILTER_CONFIG_VERSION;
	Reply->DiskNumber = Request->DiskNumber;
	Reply->TotalCount = ext->MappingItemCount;
	Reply->StartIndex = Request->StartIndex;
	sectorBuffer = (PUCHAR)CdpAllocate(ext->SectorSize);
	if (!sectorBuffer)
	{
		status = STATUS_INSUFFICIENT_RESOURCES;
		goto done;
	}
	for (index = Request->StartIndex;
		index < ext->MappingItemCount &&
		Reply->ReturnedCount < CDP_DISK_FILTER_MAX_RECORDS_PER_QUERY;
		++index)
	{
		ULONGLONG headerRelativeOffset = index * CDP_RECORD_HEADER_BYTES;
		ULONGLONG sectorRelativeOffset = headerRelativeOffset -
			(headerRelativeOffset % ext->SectorSize);
		ULONG offsetInSector =
			(ULONG)(headerRelativeOffset - sectorRelativeOffset);
		PCDP_DISK_RECORD_HEADER header;
		PCDP_DISK_FILTER_RECORD_ENTRY record;
		if (loadedSector != sectorRelativeOffset)
		{
			status = CdpRawIo(
				ext, IRP_MJ_READ,
				ext->JournalStart + sectorRelativeOffset,
				ext->SectorSize, sectorBuffer);
			if (!NT_SUCCESS(status))
				goto done;
			loadedSector = sectorRelativeOffset;
		}
		header = (PCDP_DISK_RECORD_HEADER)(sectorBuffer + offsetInSector);
		record = &Reply->Records[Reply->ReturnedCount++];
		record->Index = index;
		record->WallClock100ns = header->WallClock100ns;
		record->VolumeOffset = header->VolumeOffset;
		record->JournalOffset = header->JournalOffset;
		record->Sequence = index;
		record->DataLength = header->DataLength;
		record->Flags = header->Sequence & 0xFFFF0000UL;
	}

done:
	CdpFree(sectorBuffer);
	KeReleaseMutex(&ext->HistoryMutex, FALSE);
	return status;
}

static NTSTATUS CdpDispatchControl(PIRP Irp, PIO_STACK_LOCATION IrpSp)
{
	ULONG code = IrpSp->Parameters.DeviceIoControl.IoControlCode;
	PVOID buffer = Irp->AssociatedIrp.SystemBuffer;
	ULONG inLength = IrpSp->Parameters.DeviceIoControl.InputBufferLength;
	ULONG outLength = IrpSp->Parameters.DeviceIoControl.OutputBufferLength;
	NTSTATUS status;

	switch (code)
	{
	case IOCTL_CDP_DISK_FILTER_CONFIGURE:
		if (!buffer || inLength < sizeof(CDP_DISK_FILTER_CONFIG))
			return CdpCompleteIrp(Irp, STATUS_BUFFER_TOO_SMALL, 0);
		status = CdpConfigureDisk((PCDP_DISK_FILTER_CONFIG)buffer);
		return CdpCompleteIrp(Irp, status, 0);

	case IOCTL_CDP_DISK_FILTER_DISABLE:
		if (!buffer || inLength < sizeof(CDP_DISK_FILTER_DISABLE))
			return CdpCompleteIrp(Irp, STATUS_BUFFER_TOO_SMALL, 0);
		{
			PCDP_DISK_FILTER_DISABLE request =
				(PCDP_DISK_FILTER_DISABLE)buffer;
			PCDP_DISK_EXTENSION ext = request->Version ==
				CDP_DISK_FILTER_CONFIG_VERSION ?
				CdpAcquireConfiguredDiskByNumber(request->DiskNumber) : NULL;
			if (!ext)
				return CdpCompleteIrp(Irp, STATUS_NOT_FOUND, 0);
			ext->Enabled = FALSE;
			CdpClearMappings(ext);
			KeReleaseMutex(&ext->HistoryMutex, FALSE);
			return CdpCompleteIrp(Irp, STATUS_SUCCESS, 0);
		}

	case IOCTL_CDP_DISK_FILTER_QUERY:
		if (!buffer || inLength < sizeof(CDP_DISK_FILTER_QUERY) ||
			outLength < sizeof(CDP_DISK_FILTER_STATUS))
			return CdpCompleteIrp(Irp, STATUS_BUFFER_TOO_SMALL, 0);
		{
			CDP_DISK_FILTER_QUERY request = *(PCDP_DISK_FILTER_QUERY)buffer;
			PCDP_DISK_EXTENSION ext = request.Version ==
				CDP_DISK_FILTER_CONFIG_VERSION ?
				CdpAcquireConfiguredDiskByNumber(request.DiskNumber) : NULL;
			PCDP_DISK_FILTER_STATUS reply =
				(PCDP_DISK_FILTER_STATUS)buffer;
			if (!ext)
				return CdpCompleteIrp(Irp, STATUS_NOT_FOUND, 0);
			RtlZeroMemory(reply, sizeof(*reply));
			reply->Version = CDP_DISK_FILTER_CONFIG_VERSION;
			reply->DiskNumber = ext->DiskNumber;
			reply->PartitionStyle = ext->PartitionStyle;
			reply->MbrSignature = ext->MbrSignature;
			reply->DiskGuid = ext->DiskGuid;
			reply->Enabled = ext->Enabled ? 1UL : 0UL;
			reply->SectorSize = ext->SectorSize;
			reply->DiskLength = ext->DiskLength;
			reply->SourceStart = ext->SourceStart;
			reply->SourceLength = ext->SourceLength;
			reply->JournalStart = ext->JournalStart;
			reply->JournalLength = ext->JournalLength;
			reply->JournalBytesUsed = ext->JournalBytesUsed;
			reply->MappingItemCount = ext->MappingItemCount;
			KeReleaseMutex(&ext->HistoryMutex, FALSE);
			return CdpCompleteIrp(Irp, STATUS_SUCCESS, sizeof(*reply));
		}

	case IOCTL_CDP_DISK_FILTER_DIAGNOSE:
		if (!buffer ||
			inLength < sizeof(CDP_DISK_FILTER_DIAGNOSTIC_REQUEST) ||
			outLength < sizeof(CDP_DISK_FILTER_DIAGNOSTIC_REPLY))
			return CdpCompleteIrp(Irp, STATUS_BUFFER_TOO_SMALL, 0);
		{
			CDP_DISK_FILTER_DIAGNOSTIC_REQUEST request =
				*(PCDP_DISK_FILTER_DIAGNOSTIC_REQUEST)buffer;
			if (request.Version != CDP_DISK_FILTER_CONFIG_VERSION)
				return CdpCompleteIrp(
					Irp, STATUS_REVISION_MISMATCH, 0);
			CdpBuildDiagnostics(
				&request, (PCDP_DISK_FILTER_DIAGNOSTIC_REPLY)buffer);
			return CdpCompleteIrp(
				Irp, STATUS_SUCCESS,
				sizeof(CDP_DISK_FILTER_DIAGNOSTIC_REPLY));
		}

	case IOCTL_CDP_DISK_FILTER_RECORDS:
		if (!buffer ||
			inLength < sizeof(CDP_DISK_FILTER_RECORDS_REQUEST) ||
			outLength < sizeof(CDP_DISK_FILTER_RECORDS_REPLY))
			return CdpCompleteIrp(Irp, STATUS_BUFFER_TOO_SMALL, 0);
		{
			CDP_DISK_FILTER_RECORDS_REQUEST request =
				*(PCDP_DISK_FILTER_RECORDS_REQUEST)buffer;
			if (request.Version != CDP_DISK_FILTER_CONFIG_VERSION)
				return CdpCompleteIrp(
					Irp, STATUS_REVISION_MISMATCH, 0);
			status = CdpQueryRecords(
				&request, (PCDP_DISK_FILTER_RECORDS_REPLY)buffer);
			return CdpCompleteIrp(
				Irp, status,
				NT_SUCCESS(status) ?
				sizeof(CDP_DISK_FILTER_RECORDS_REPLY) : 0);
		}
	default:
		return CdpCompleteIrp(Irp, STATUS_INVALID_DEVICE_REQUEST, 0);
	}
}

static NTSTATUS CdpDispatchCreateClose(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
	PULONG signature = (PULONG)DeviceObject->DeviceExtension;
	if (signature && *signature == CDP_CONTROL_SIGNATURE)
		return CdpCompleteIrp(Irp, STATUS_SUCCESS, 0);
	if (signature && *signature == CDP_DISK_SIGNATURE)
		return CdpForward(
			((PCDP_DISK_EXTENSION)signature)->LowerDevice, Irp);
	return CdpCompleteIrp(Irp, STATUS_INVALID_DEVICE_REQUEST, 0);
}

static NTSTATUS CdpDispatchDeviceControl(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
	PULONG signature = (PULONG)DeviceObject->DeviceExtension;
	if (signature && *signature == CDP_CONTROL_SIGNATURE)
		return CdpDispatchControl(Irp, IoGetCurrentIrpStackLocation(Irp));
	if (signature && *signature == CDP_DISK_SIGNATURE)
		return CdpForward(
			((PCDP_DISK_EXTENSION)signature)->LowerDevice, Irp);
	return CdpCompleteIrp(Irp, STATUS_INVALID_DEVICE_REQUEST, 0);
}

static NTSTATUS CdpDispatchReadWrite(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
	PCDP_DISK_EXTENSION ext =
		(PCDP_DISK_EXTENSION)DeviceObject->DeviceExtension;
	PIO_STACK_LOCATION irpSp = IoGetCurrentIrpStackLocation(Irp);
	UCHAR major = irpSp->MajorFunction;
	LONGLONG signedOffset = major == IRP_MJ_READ ?
		irpSp->Parameters.Read.ByteOffset.QuadPart :
		irpSp->Parameters.Write.ByteOffset.QuadPart;
	ULONG length = major == IRP_MJ_READ ?
		irpSp->Parameters.Read.Length : irpSp->Parameters.Write.Length;
	ULONGLONG offset;
	PCDP_IO_ITEM item;
	KIRQL oldIrql;

	if (!ext || ext->Signature != CDP_DISK_SIGNATURE || !ext->LowerDevice)
		return CdpCompleteIrp(Irp, STATUS_INVALID_DEVICE_REQUEST, 0);
	if (!ext->Enabled || signedOffset < 0 || length == 0)
		return CdpForward(ext->LowerDevice, Irp);
	offset = (ULONGLONG)signedOffset;
	if (!CdpRangesOverlap(offset, length, ext->SourceStart, ext->SourceLength))
		return CdpForward(ext->LowerDevice, Irp);
	if (offset < ext->SourceStart ||
		offset > ext->SourceStart + ext->SourceLength ||
		length > ext->SourceStart + ext->SourceLength - offset ||
		(offset % ext->SectorSize) != 0 ||
		(length % ext->SectorSize) != 0)
	{
		CDP_LOG("blocked crossing/misaligned I/O major=%u offset=%llu len=%lu\n",
			major, offset, length);
		return CdpCompleteIrp(Irp, STATUS_INVALID_PARAMETER, 0);
	}
	item = (PCDP_IO_ITEM)CdpAllocate(sizeof(*item));
	if (!item)
		return CdpCompleteIrp(Irp, STATUS_INSUFFICIENT_RESOURCES, 0);
	item->Irp = Irp;
	IoMarkIrpPending(Irp);
	KeAcquireSpinLock(&ext->QueueLock, &oldIrql);
	if (ext->Stopping)
	{
		KeReleaseSpinLock(&ext->QueueLock, oldIrql);
		CdpFree(item);
		return CdpCompleteIrp(Irp, STATUS_DEVICE_NOT_READY, 0);
	}
	InsertTailList(&ext->Queue, &item->Entry);
	KeSetEvent(&ext->QueueEvent, IO_NO_INCREMENT, FALSE);
	KeReleaseSpinLock(&ext->QueueLock, oldIrql);
	return STATUS_PENDING;
}

static NTSTATUS CdpDispatchDefault(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
	PULONG signature = (PULONG)DeviceObject->DeviceExtension;
	if (signature && *signature == CDP_DISK_SIGNATURE)
		return CdpForward(
			((PCDP_DISK_EXTENSION)signature)->LowerDevice, Irp);
	return CdpCompleteIrp(Irp, STATUS_INVALID_DEVICE_REQUEST, 0);
}

static NTSTATUS CdpDispatchPnp(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
	PCDP_DISK_EXTENSION ext =
		(PCDP_DISK_EXTENSION)DeviceObject->DeviceExtension;
	PIO_STACK_LOCATION irpSp = IoGetCurrentIrpStackLocation(Irp);
	NTSTATUS status;

	if (!ext || ext->Signature != CDP_DISK_SIGNATURE)
		return CdpCompleteIrp(Irp, STATUS_INVALID_DEVICE_REQUEST, 0);
	if (irpSp->MinorFunction != IRP_MN_REMOVE_DEVICE)
		return CdpForward(ext->LowerDevice, Irp);

	CdpStopWorker(ext);
	KeWaitForSingleObject(&ext->HistoryMutex, Executive, KernelMode, FALSE, NULL);
	ext->Enabled = FALSE;
	CdpClearMappings(ext);
	CdpFree(ext->HeaderSectorCache);
	ext->HeaderSectorCache = NULL;
	ext->HeaderSectorCacheValid = FALSE;
	KeReleaseMutex(&ext->HistoryMutex, FALSE);
	ExAcquireFastMutex(&g_Cdp.DiskListMutex);
	RemoveEntryList(&ext->GlobalEntry);
	ExReleaseFastMutex(&g_Cdp.DiskListMutex);
	IoSkipCurrentIrpStackLocation(Irp);
	status = IoCallDriver(ext->LowerDevice, Irp);
	IoDetachDevice(ext->LowerDevice);
	IoDeleteDevice(DeviceObject);
	return status;
}

NTSTATUS CdpAddDevice(PDRIVER_OBJECT DriverObject, PDEVICE_OBJECT PhysicalDeviceObject)
{
	PDEVICE_OBJECT filterDevice = NULL;
	PCDP_DISK_EXTENSION ext;
	NTSTATUS status;

	status = IoCreateDevice(
		DriverObject,
		sizeof(CDP_DISK_EXTENSION),
		NULL,
		FILE_DEVICE_DISK,
		FILE_DEVICE_SECURE_OPEN,
		FALSE,
		&filterDevice);
	if (!NT_SUCCESS(status))
		return status;
	ext = (PCDP_DISK_EXTENSION)filterDevice->DeviceExtension;
	RtlZeroMemory(ext, sizeof(*ext));
	ext->Signature = CDP_DISK_SIGNATURE;
	ext->FilterDevice = filterDevice;
	ext->PhysicalDevice = PhysicalDeviceObject;
	InitializeListHead(&ext->MappingList);
	InitializeListHead(&ext->Queue);
	KeInitializeMutex(&ext->HistoryMutex, 0);
	KeInitializeSpinLock(&ext->QueueLock);
	KeInitializeEvent(&ext->QueueEvent, NotificationEvent, FALSE);
	status = IoAttachDeviceToDeviceStackSafe(
		filterDevice, PhysicalDeviceObject, &ext->LowerDevice);
	if (!NT_SUCCESS(status))
	{
		IoDeleteDevice(filterDevice);
		return status;
	}
	filterDevice->Flags = ext->LowerDevice->Flags;
	filterDevice->Characteristics = ext->LowerDevice->Characteristics;
	status = CdpStartWorker(ext);
	if (!NT_SUCCESS(status))
	{
		IoDetachDevice(ext->LowerDevice);
		IoDeleteDevice(filterDevice);
		return status;
	}
	ExAcquireFastMutex(&g_Cdp.DiskListMutex);
	InsertTailList(&g_Cdp.DiskList, &ext->GlobalEntry);
	ExReleaseFastMutex(&g_Cdp.DiskListMutex);
	CDP_LOG("attached physical disk device stack ext=%p lower=%p\n",
		ext, ext->LowerDevice);
	filterDevice->Flags &= ~DO_DEVICE_INITIALIZING;
	return STATUS_SUCCESS;
}

VOID CdpUnload(PDRIVER_OBJECT DriverObject)
{
	UNREFERENCED_PARAMETER(DriverObject);
	if (g_Cdp.ControlDevice)
	{
		IoDeleteSymbolicLink(&g_Cdp.SymbolicLink);
		IoDeleteDevice(g_Cdp.ControlDevice);
		g_Cdp.ControlDevice = NULL;
	}
}

NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
	UNICODE_STRING controlName;
	PCDP_CONTROL_EXTENSION controlExt;
	ULONG index;
	NTSTATUS status;

	UNREFERENCED_PARAMETER(RegistryPath);
	RtlZeroMemory(&g_Cdp, sizeof(g_Cdp));
	g_Cdp.DriverObject = DriverObject;
	ExInitializeFastMutex(&g_Cdp.DiskListMutex);
	InitializeListHead(&g_Cdp.DiskList);
	for (index = 0; index <= IRP_MJ_MAXIMUM_FUNCTION; ++index)
		DriverObject->MajorFunction[index] = CdpDispatchDefault;
	DriverObject->MajorFunction[IRP_MJ_CREATE] = CdpDispatchCreateClose;
	DriverObject->MajorFunction[IRP_MJ_CLOSE] = CdpDispatchCreateClose;
	DriverObject->MajorFunction[IRP_MJ_READ] = CdpDispatchReadWrite;
	DriverObject->MajorFunction[IRP_MJ_WRITE] = CdpDispatchReadWrite;
	DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = CdpDispatchDeviceControl;
	DriverObject->MajorFunction[IRP_MJ_PNP] = CdpDispatchPnp;
	DriverObject->DriverExtension->AddDevice = CdpAddDevice;
	DriverObject->DriverUnload = CdpUnload;

	RtlInitUnicodeString(&controlName, L"\\Device\\CdpDiskFilter");
	RtlInitUnicodeString(&g_Cdp.SymbolicLink, L"\\DosDevices\\CdpDiskFilter");
	status = IoCreateDevice(
		DriverObject,
		sizeof(CDP_CONTROL_EXTENSION),
		&controlName,
		CDP_DISK_FILTER_DEVICE_TYPE,
		FILE_DEVICE_SECURE_OPEN,
		FALSE,
		&g_Cdp.ControlDevice);
	if (!NT_SUCCESS(status))
		return status;
	controlExt = (PCDP_CONTROL_EXTENSION)g_Cdp.ControlDevice->DeviceExtension;
	controlExt->Signature = CDP_CONTROL_SIGNATURE;
	g_Cdp.ControlDevice->Flags |= DO_BUFFERED_IO;
	status = IoCreateSymbolicLink(&g_Cdp.SymbolicLink, &controlName);
	if (!NT_SUCCESS(status))
	{
		IoDeleteDevice(g_Cdp.ControlDevice);
		g_Cdp.ControlDevice = NULL;
		return status;
	}
	g_Cdp.ControlDevice->Flags &= ~DO_DEVICE_INITIALIZING;
	CDP_LOG("loaded; configure manually through \\.\\CdpDiskFilter\n");
	return STATUS_SUCCESS;
}
