#include "QHIrpDispatchs.h"
#include "..\SysRestoreCore\include\qh_core.h"
#include "..\SysRestoreCore\include\qh_dev_store.h"
#include <ntdddisk.h>
#include <ntstrsafe.h>

static VOID QHDisableAllCaptureSources(_In_ PQH_DRIVER_EXTENSION DriverExt);

static VOID QHFillReply(
	_Out_ PQH_COMMAND_REPLY Reply,
	_In_ ULONG Command,
	_In_ ULONG Result,
	_In_ UINT64 VolumeHandle,
	_In_ PCWSTR Message)
{
	RtlZeroMemory(Reply, sizeof(*Reply));
	Reply->Command = Command;
	Reply->Result = Result;
	Reply->VolumeHandle = VolumeHandle;
	RtlStringCbCopyW(Reply->Message, sizeof(Reply->Message), Message);
}

static VOID QHDbgGuid(_In_ PCSTR Tag, _In_ const GUID* G)
{
#if DBG
	QH_DBG("%s {%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}\n",
		Tag,
		G->Data1, G->Data2, G->Data3,
		G->Data4[0], G->Data4[1], G->Data4[2], G->Data4[3],
		G->Data4[4], G->Data4[5], G->Data4[6], G->Data4[7]);
#else
	UNREFERENCED_PARAMETER(Tag);
	UNREFERENCED_PARAMETER(G);
#endif
}

static NTSTATUS QHFormatVolumeNtPath(
	_In_ const GUID* VolumeGuid,
	_Out_writes_bytes_(PathBytes) PWCHAR PathBuffer,
	_In_ SIZE_T PathBytes)
{
	return RtlStringCbPrintfW(
		PathBuffer,
		PathBytes,
		L"\\??\\Volume{%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
		VolumeGuid->Data1,
		VolumeGuid->Data2,
		VolumeGuid->Data3,
		VolumeGuid->Data4[0], VolumeGuid->Data4[1],
		VolumeGuid->Data4[2], VolumeGuid->Data4[3],
		VolumeGuid->Data4[4], VolumeGuid->Data4[5],
		VolumeGuid->Data4[6], VolumeGuid->Data4[7]);
}

static NTSTATUS QHQueryVolumeGeometry(
	_In_ HANDLE FileHandle,
	_Out_ PUINT64 PartitionSize,
	_Out_ PULONG SectorSize)
{
	GET_LENGTH_INFORMATION lengthInfo;
	DISK_GEOMETRY geometry;
	IO_STATUS_BLOCK iosb;
	NTSTATUS status;

	RtlZeroMemory(&lengthInfo, sizeof(lengthInfo));
	RtlZeroMemory(&geometry, sizeof(geometry));
	status = ZwDeviceIoControlFile(
		FileHandle, NULL, NULL, NULL, &iosb,
		IOCTL_DISK_GET_LENGTH_INFO,
		NULL, 0, &lengthInfo, sizeof(lengthInfo));
	if (!NT_SUCCESS(status))
		return status;
	status = ZwDeviceIoControlFile(
		FileHandle, NULL, NULL, NULL, &iosb,
		IOCTL_DISK_GET_DRIVE_GEOMETRY,
		NULL, 0, &geometry, sizeof(geometry));
	if (!NT_SUCCESS(status))
		return status;
	if (lengthInfo.Length.QuadPart <= 0 ||
		(geometry.BytesPerSector != 512 && geometry.BytesPerSector != 4096))
	{
		return STATUS_NOT_SUPPORTED;
	}
	*PartitionSize = (UINT64)lengthInfo.Length.QuadPart;
	*SectorSize = geometry.BytesPerSector;
	return STATUS_SUCCESS;
}

static NTSTATUS QHSendDeviceControlSynchronously(
	_In_ PDEVICE_OBJECT Device,
	_In_ ULONG IoControlCode,
	_Out_writes_bytes_(OutputLength) PVOID Output,
	_In_ ULONG OutputLength)
{
	KEVENT event;
	IO_STATUS_BLOCK iosb;
	PIRP irp;
	NTSTATUS status;

	KeInitializeEvent(&event, NotificationEvent, FALSE);
	RtlZeroMemory(&iosb, sizeof(iosb));
	irp = IoBuildDeviceIoControlRequest(
		IoControlCode,
		Device,
		NULL,
		0,
		Output,
		OutputLength,
		FALSE,
		&event,
		&iosb);
	if (!irp)
		return STATUS_INSUFFICIENT_RESOURCES;
	status = IoCallDriver(Device, irp);
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

static NTSTATUS QHQueryDeviceGeometry(
	_In_ PDEVICE_OBJECT Device,
	_Out_ PUINT64 PartitionSize,
	_Out_ PULONG SectorSize)
{
	GET_LENGTH_INFORMATION lengthInfo;
	DISK_GEOMETRY geometry;
	NTSTATUS status;

	RtlZeroMemory(&lengthInfo, sizeof(lengthInfo));
	RtlZeroMemory(&geometry, sizeof(geometry));
	status = QHSendDeviceControlSynchronously(
		Device,
		IOCTL_DISK_GET_LENGTH_INFO,
		&lengthInfo,
		sizeof(lengthInfo));
	if (!NT_SUCCESS(status))
		return status;
	status = QHSendDeviceControlSynchronously(
		Device,
		IOCTL_DISK_GET_DRIVE_GEOMETRY,
		&geometry,
		sizeof(geometry));
	if (!NT_SUCCESS(status))
		return status;
	if (lengthInfo.Length.QuadPart <= 0 ||
		(geometry.BytesPerSector != 512 && geometry.BytesPerSector != 4096))
	{
		return STATUS_NOT_SUPPORTED;
	}
	*PartitionSize = (UINT64)lengthInfo.Length.QuadPart;
	*SectorSize = geometry.BytesPerSector;
	return STATUS_SUCCESS;
}

static PQH_VOLUME_HANDLE_ENTRY QHLookupVolumeHandleLocked(
	_In_ PQH_DRIVER_EXTENSION DriverExt,
	_In_ UINT64 HandleId)
{
	PLIST_ENTRY entry = DriverExt->VolumeHandleList.Flink;
	while (entry != &DriverExt->VolumeHandleList)
	{
		PQH_VOLUME_HANDLE_ENTRY item = CONTAINING_RECORD(entry, QH_VOLUME_HANDLE_ENTRY, Entry);
		if (item->HandleId == HandleId)
			return item;
		entry = entry->Flink;
	}
	return NULL;
}

// Find our filter's LowerDeviceObject for a volume PDO / stack member.
static PDEVICE_OBJECT QHFindTargetLowerDevice(
	_In_ PQH_DRIVER_EXTENSION DriverExt,
	_In_ PDEVICE_OBJECT VolumeDevice)
{
	KIRQL oldIrql;
	PDEVICE_OBJECT lower = NULL;
	PDEVICE_OBJECT walk;

	if (!VolumeDevice)
		return NULL;

	// Prefer matching against devices we attached to in AddDevice.
	KeAcquireSpinLock(&DriverExt->DeviceObjectListLock, &oldIrql);
	{
		PLIST_ENTRY entry = DriverExt->DeviceObjectListHead.Flink;
		while (entry != &DriverExt->DeviceObjectListHead)
		{
			PQH_DEVICE_LIST_NODE node = CONTAINING_RECORD(entry, QH_DEVICE_LIST_NODE, Entry);
			PQH_DEVICE_EXTENSION volExt = (PQH_DEVICE_EXTENSION)node->DeviceObject->DeviceExtension;
			if (volExt &&
				(volExt->PhysicalDeviceObject == VolumeDevice ||
				 volExt->LowerDeviceObject == VolumeDevice ||
				 node->DeviceObject == VolumeDevice))
			{
				lower = volExt->LowerDeviceObject;
				break;
			}
			entry = entry->Flink;
		}
	}
	KeReleaseSpinLock(&DriverExt->DeviceObjectListLock, oldIrql);
	if (lower)
		return lower;

	// Fallback: walk attachments above the volume until our filter appears.
	for (walk = VolumeDevice; walk; walk = walk->AttachedDevice)
	{
		if (walk->DriverObject == g_DriverObject)
		{
			PQH_DEVICE_EXTENSION volExt = (PQH_DEVICE_EXTENSION)walk->DeviceExtension;
			if (volExt)
				return volExt->LowerDeviceObject;
		}
	}
	return NULL;
}

static NTSTATUS QHResolveTargetLowerDevice(
	_In_ PQH_DRIVER_EXTENSION DriverExt,
	_In_ HANDLE VolumeFileHandle,
	_Out_ PDEVICE_OBJECT* OutLowerDevice)
{
	NTSTATUS status;
	PFILE_OBJECT fileObject = NULL;
	PDEVICE_OBJECT volumeDevice = NULL;

	*OutLowerDevice = NULL;

	status = ObReferenceObjectByHandle(
		VolumeFileHandle,
		0,
		*IoFileObjectType,
		KernelMode,
		(PVOID*)&fileObject,
		NULL);
	if (!NT_SUCCESS(status))
		return status;

	if (fileObject->Vpb && fileObject->Vpb->RealDevice)
		volumeDevice = fileObject->Vpb->RealDevice;
	else
		volumeDevice = fileObject->DeviceObject;

	*OutLowerDevice = QHFindTargetLowerDevice(DriverExt, volumeDevice);
	ObDereferenceObject(fileObject);

	if (!*OutLowerDevice)
		return STATUS_DEVICE_DOES_NOT_EXIST;
	return STATUS_SUCCESS;
}

static PQH_VOLUME_HANDLE_ENTRY QHAcquireCaptureTarget(
	_In_ PQH_DRIVER_EXTENSION DriverExt)
{
	PQH_VOLUME_HANDLE_ENTRY item = NULL;

	ExAcquireFastMutex(&DriverExt->VolumeHandleMutex);
	if (DriverExt->CaptureTargetHandleId != 0)
	{
		item = QHLookupVolumeHandleLocked(DriverExt, DriverExt->CaptureTargetHandleId);
		if (item && !item->Closing)
			InterlockedIncrement(&item->ReferenceCount);
		else
			item = NULL;
	}
	ExReleaseFastMutex(&DriverExt->VolumeHandleMutex);
	return item;
}

static VOID QHReleaseVolumeHandleEntry(_In_ PQH_VOLUME_HANDLE_ENTRY Item)
{
	if (InterlockedDecrement(&Item->ReferenceCount) == 0)
		KeSetEvent(&Item->NoReferences, IO_NO_INCREMENT, FALSE);
}

static VOID QHCloseVolumeHandleEntry(_In_ PQH_VOLUME_HANDLE_ENTRY Item)
{
	QHReleaseVolumeHandleEntry(Item); // Drop the list ownership reference.
	KeWaitForSingleObject(&Item->NoReferences, Executive, KernelMode, FALSE, NULL);
	if (Item->Journal.Mounted)
		QHJournalClose(&Item->Journal);
	if (Item->FileHandle)
		ZwClose(Item->FileHandle);
	qhfree(Item);
}

VOID QHCloseAllVolumeHandles(_In_ PQH_DRIVER_EXTENSION DriverExt)
{
	QHDisableAllCaptureSources(DriverExt);
	ExAcquireFastMutex(&DriverExt->VolumeHandleMutex);
	DriverExt->CaptureTargetHandleId = 0;
	while (!IsListEmpty(&DriverExt->VolumeHandleList))
	{
		PLIST_ENTRY entry = RemoveHeadList(&DriverExt->VolumeHandleList);
		PQH_VOLUME_HANDLE_ENTRY item = CONTAINING_RECORD(entry, QH_VOLUME_HANDLE_ENTRY, Entry);
		item->Closing = TRUE;
		QHCloseVolumeHandleEntry(item);
	}
	ExReleaseFastMutex(&DriverExt->VolumeHandleMutex);
}

static NTSTATUS QHOpenVolumeHandle(
	_In_ PQH_DRIVER_EXTENSION DriverExt,
	_In_ const GUID* VolumeGuid,
	_Out_ PUINT64 OutHandleId)
{
	NTSTATUS Status;
	WCHAR path[96];
	UNICODE_STRING pathStr;
	OBJECT_ATTRIBUTES oa;
	IO_STATUS_BLOCK iosb;
	HANDLE fileHandle = NULL;
	PQH_VOLUME_HANDLE_ENTRY item;

	*OutHandleId = 0;

	if (KeGetCurrentIrql() != PASSIVE_LEVEL)
		return STATUS_INVALID_DEVICE_STATE;

	Status = QHFormatVolumeNtPath(VolumeGuid, path, sizeof(path));
	if (!NT_SUCCESS(Status))
		return Status;

	RtlInitUnicodeString(&pathStr, path);
	InitializeObjectAttributes(&oa, &pathStr,
		OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE,
		NULL, NULL);

	// Capture path must ZwWriteFile to this handle (overwrite target LBA 0).
	Status = ZwCreateFile(
		&fileHandle,
		GENERIC_READ | GENERIC_WRITE | SYNCHRONIZE,
		&oa,
		&iosb,
		NULL,
		FILE_ATTRIBUTE_NORMAL,
		FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
		FILE_OPEN,
		FILE_NON_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT | FILE_NO_INTERMEDIATE_BUFFERING,
		NULL,
		0);
	if (!NT_SUCCESS(Status))
	{
		QH_LOG("open volume failed 0x%08X path=%ws\n", Status, path);
		return Status;
	}

	item = (PQH_VOLUME_HANDLE_ENTRY)qhalloc(sizeof(*item));
	if (!item)
	{
		ZwClose(fileHandle);
		return STATUS_INSUFFICIENT_RESOURCES;
	}

	RtlZeroMemory(item, sizeof(*item));
	item->FileHandle = fileHandle;
	item->HandleId = (UINT64)InterlockedIncrement64(&DriverExt->VolumeHandleNextId);
	item->ReferenceCount = 1;
	item->VolumeGuid = *VolumeGuid;
	item->VolumeGuidValid = TRUE;
	KeInitializeEvent(&item->NoReferences, NotificationEvent, FALSE);

	Status = QHResolveTargetLowerDevice(DriverExt, fileHandle, &item->TargetLowerDevice);
	if (!NT_SUCCESS(Status))
	{
		QH_LOG("resolve target lower device failed 0x%08X\n", Status);
		ZwClose(fileHandle);
		qhfree(item);
		return Status;
	}
	Status = QHQueryVolumeGeometry(fileHandle, &item->PartitionSize, &item->SectorSize);
	if (!NT_SUCCESS(Status))
	{
		QH_LOG("query volume geometry failed 0x%08X\n", Status);
		ZwClose(fileHandle);
		qhfree(item);
		return Status;
	}

	ExAcquireFastMutex(&DriverExt->VolumeHandleMutex);
	InsertTailList(&DriverExt->VolumeHandleList, &item->Entry);
	ExReleaseFastMutex(&DriverExt->VolumeHandleMutex);

	*OutHandleId = item->HandleId;
	QH_DBG("opened volume handle id=%llu lower=%p\n",
		item->HandleId, item->TargetLowerDevice);
	QHDbgGuid("  Guid", VolumeGuid);
	return STATUS_SUCCESS;
}

static NTSTATUS QHCloseVolumeHandle(
	_In_ PQH_DRIVER_EXTENSION DriverExt,
	_In_ UINT64 HandleId)
{
	PQH_VOLUME_HANDLE_ENTRY item;
	BOOLEAN wasCaptureTarget = FALSE;

	ExAcquireFastMutex(&DriverExt->VolumeHandleMutex);
	item = QHLookupVolumeHandleLocked(DriverExt, HandleId);
	if (item)
	{
		RemoveEntryList(&item->Entry);
		item->Closing = TRUE;
		if (DriverExt->CaptureTargetHandleId == HandleId)
		{
			DriverExt->CaptureTargetHandleId = 0;
			wasCaptureTarget = TRUE;
		}
	}
	ExReleaseFastMutex(&DriverExt->VolumeHandleMutex);

	if (!item)
		return STATUS_NOT_FOUND;

	if (wasCaptureTarget)
	{
		QHDisableAllCaptureSources(DriverExt);
		if (item->Journal.Mounted)
		{
			NTSTATUS invStatus = QHJournalInvalidate(&item->Journal);
			QH_LOG("[COW] stop: invalidate journal status=0x%08X\n", invStatus);
		}
	}
	QHCloseVolumeHandleEntry(item);
	QH_DBG("closed volume handle id=%llu\n", HandleId);
	return STATUS_SUCCESS;
}

static VOID QHDisableAllCaptureSources(_In_ PQH_DRIVER_EXTENSION DriverExt)
{
	// Do not destroy Core while holding DeviceObjectListLock: a spin lock raises
	// IRQL to DISPATCH_LEVEL and Core destruction can free locks/memory.  Take a
	// reference to one matching filter device, release the spin lock, then
	// quiesce it.  Re-scan until no capture Core remains.
	for (;;)
	{
		KIRQL oldIrql;
		PLIST_ENTRY entry;
		PDEVICE_OBJECT filterDevice = NULL;

		KeAcquireSpinLock(&DriverExt->DeviceObjectListLock, &oldIrql);
		for (entry = DriverExt->DeviceObjectListHead.Flink;
			entry != &DriverExt->DeviceObjectListHead;
			entry = entry->Flink)
		{
			PQH_DEVICE_LIST_NODE node =
				CONTAINING_RECORD(entry, QH_DEVICE_LIST_NODE, Entry);
			PQH_DEVICE_EXTENSION ext =
				(PQH_DEVICE_EXTENSION)node->DeviceObject->DeviceExtension;
			if (ext && (InterlockedCompareExchange(&ext->CaptureEnabled, 0, 0) != 0 ||
				ext->Core != NULL))
			{
				filterDevice = node->DeviceObject;
				ObReferenceObject(filterDevice);
				break;
			}
		}
		KeReleaseSpinLock(&DriverExt->DeviceObjectListLock, oldIrql);

		if (!filterDevice)
			break;

		QHDisableAndDestroyCapture(
			(PQH_DEVICE_EXTENSION)filterDevice->DeviceExtension);
		ObDereferenceObject(filterDevice);
	}
}

static PQH_DEVICE_EXTENSION QHFindSourceExtension(
	_In_ PQH_DRIVER_EXTENSION DriverExt,
	_In_ PDEVICE_OBJECT LowerDevice)
{
	KIRQL oldIrql;
	PLIST_ENTRY entry;
	PQH_DEVICE_EXTENSION found = NULL;

	KeAcquireSpinLock(&DriverExt->DeviceObjectListLock, &oldIrql);
	for (entry = DriverExt->DeviceObjectListHead.Flink;
		entry != &DriverExt->DeviceObjectListHead;
		entry = entry->Flink)
	{
		PQH_DEVICE_LIST_NODE node =
			CONTAINING_RECORD(entry, QH_DEVICE_LIST_NODE, Entry);
		PQH_DEVICE_EXTENSION ext =
			(PQH_DEVICE_EXTENSION)node->DeviceObject->DeviceExtension;
		if (ext && ext->LowerDeviceObject == LowerDevice)
		{
			found = ext;
			break;
		}
	}
	KeReleaseSpinLock(&DriverExt->DeviceObjectListLock, oldIrql);
	return found;
}

static PQH_DEVICE_EXTENSION QHFindSourceExtensionByGuid(
	_In_ PQH_DRIVER_EXTENSION DriverExt,
	_In_ const GUID* VolumeGuid)
{
	KIRQL oldIrql;
	PLIST_ENTRY entry;
	PQH_DEVICE_EXTENSION found = NULL;

	KeAcquireSpinLock(&DriverExt->DeviceObjectListLock, &oldIrql);
	for (entry = DriverExt->DeviceObjectListHead.Flink;
		entry != &DriverExt->DeviceObjectListHead;
		entry = entry->Flink)
	{
		PQH_DEVICE_LIST_NODE node =
			CONTAINING_RECORD(entry, QH_DEVICE_LIST_NODE, Entry);
		PQH_DEVICE_EXTENSION ext =
			(PQH_DEVICE_EXTENSION)node->DeviceObject->DeviceExtension;
		if (ext &&
			ext->VolumeGuidValid &&
			RtlCompareMemory(
				&ext->VolumeGuid,
				VolumeGuid,
				sizeof(GUID)) == sizeof(GUID))
		{
			found = ext;
			break;
		}
	}
	KeReleaseSpinLock(&DriverExt->DeviceObjectListLock, oldIrql);
	return found;
}

static NTSTATUS QHConfigureCaptureInternal(
	_In_ PQH_DRIVER_EXTENSION DriverExt,
	_In_ const GUID* SourceVolumeGuid,
	_In_ const GUID* JournalPartitionGuid,
	_In_ BOOLEAN FormatJournal,
	_Out_ PUINT64 JournalHandleId)
{
	UINT64 sourceHandleId = 0;
	UINT64 journalHandleId = 0;
	UINT64 sourcePartitionSize = 0;
	ULONG sourceSectorSize = 512;
	PDEVICE_OBJECT sourceLower = NULL;
	PQH_DEVICE_EXTENSION sourceExt = NULL;
	PQH_VOLUME_HANDLE_ENTRY sourceEntry;
	PQH_VOLUME_HANDLE_ENTRY journalEntry;
	NTSTATUS status;

	*JournalHandleId = 0;
	QH_DBG("[COW] configure begin format=%u\n", FormatJournal ? 1u : 0u);
	QHDbgGuid("[COW] source", SourceVolumeGuid);
	QHDbgGuid("[COW] journal", JournalPartitionGuid);
	if (RtlCompareMemory(
		SourceVolumeGuid,
		JournalPartitionGuid,
		sizeof(GUID)) == sizeof(GUID))
	{
		return STATUS_INVALID_PARAMETER;
	}
	ExAcquireFastMutex(&DriverExt->VolumeHandleMutex);
	if (DriverExt->CaptureTargetHandleId != 0)
	{
		ExReleaseFastMutex(&DriverExt->VolumeHandleMutex);
		return STATUS_DEVICE_BUSY;
	}
	ExReleaseFastMutex(&DriverExt->VolumeHandleMutex);

	status = QHOpenVolumeHandle(DriverExt, SourceVolumeGuid, &sourceHandleId);
	if (!NT_SUCCESS(status))
		return status;
	ExAcquireFastMutex(&DriverExt->VolumeHandleMutex);
	sourceEntry = QHLookupVolumeHandleLocked(DriverExt, sourceHandleId);
	if (sourceEntry)
	{
		sourceLower = sourceEntry->TargetLowerDevice;
		sourcePartitionSize = sourceEntry->PartitionSize;
		sourceSectorSize = sourceEntry->SectorSize;
	}
	ExReleaseFastMutex(&DriverExt->VolumeHandleMutex);
	sourceExt = QHFindSourceExtension(DriverExt, sourceLower);
	(void)QHCloseVolumeHandle(DriverExt, sourceHandleId);
	if (!sourceExt)
		return STATUS_DEVICE_DOES_NOT_EXIST;

	status = QHOpenVolumeHandle(DriverExt, JournalPartitionGuid, &journalHandleId);
	if (!NT_SUCCESS(status))
		return status;

	ExAcquireFastMutex(&DriverExt->VolumeHandleMutex);
	journalEntry = QHLookupVolumeHandleLocked(DriverExt, journalHandleId);
	if (journalEntry)
		InterlockedIncrement(&journalEntry->ReferenceCount);
	ExReleaseFastMutex(&DriverExt->VolumeHandleMutex);
	if (!journalEntry)
	{
		(void)QHCloseVolumeHandle(DriverExt, journalHandleId);
		return STATUS_INVALID_HANDLE;
	}

	QH_DBG("[JOURNAL-RAW] backend lowerDevice=%p\n",
		journalEntry->TargetLowerDevice);

	QHJournalInitialize(
		&journalEntry->Journal,
		journalEntry->TargetLowerDevice,
		NULL,
		0,
		journalEntry->PartitionSize,
		journalEntry->SectorSize,
		SourceVolumeGuid);
	status = FormatJournal ?
		QHJournalFormat(&journalEntry->Journal) :
		QHJournalMount(&journalEntry->Journal);
	if (NT_SUCCESS(status) && !FormatJournal &&
		RtlCompareMemory(
			&journalEntry->Journal.SourceVolumeGuid,
			SourceVolumeGuid,
			sizeof(GUID)) != sizeof(GUID))
	{
		status = STATUS_OBJECT_TYPE_MISMATCH;
	}
	if (!NT_SUCCESS(status))
	{
		QHReleaseVolumeHandleEntry(journalEntry);
		(void)QHCloseVolumeHandle(DriverExt, journalHandleId);
		return status;
	}

	QHDisableAllCaptureSources(DriverExt);
	sourceExt->VolumeGuid = *SourceVolumeGuid;
	sourceExt->VolumeGuidValid = TRUE;
	sourceExt->SectorSize = sourceSectorSize;
	ExAcquireFastMutex(&DriverExt->VolumeHandleMutex);
	DriverExt->CaptureTargetHandleId = journalHandleId;
	ExReleaseFastMutex(&DriverExt->VolumeHandleMutex);

	if (!sourceExt->CaptureThreadHandle)
	{
		status = QHStartCaptureWorker(sourceExt);
		if (!NT_SUCCESS(status))
		{
			ExAcquireFastMutex(&DriverExt->VolumeHandleMutex);
			DriverExt->CaptureTargetHandleId = 0;
			ExReleaseFastMutex(&DriverExt->VolumeHandleMutex);
			QHReleaseVolumeHandleEntry(journalEntry);
			(void)QHCloseVolumeHandle(DriverExt, journalHandleId);
			return status;
		}
	}
	if (!sourceExt->RecoveryReadThreadHandle)
	{
		status = QHStartRecoveryReadWorker(sourceExt);
		if (!NT_SUCCESS(status))
		{
			QHStopCaptureWorker(sourceExt);
			ExAcquireFastMutex(&DriverExt->VolumeHandleMutex);
			DriverExt->CaptureTargetHandleId = 0;
			ExReleaseFastMutex(&DriverExt->VolumeHandleMutex);
			QHReleaseVolumeHandleEntry(journalEntry);
			(void)QHCloseVolumeHandle(DriverExt, journalHandleId);
			return status;
		}
	}
	{
		PQH_STORE sourceStore = NULL;
		status = QhDevStoreCreate(
			sourceExt->LowerDeviceObject,
			sourcePartitionSize,
			sourceSectorSize,
			&sourceStore);
		if (NT_SUCCESS(status))
		{
			status = QhCoreBind(
				sourceStore,
				&journalEntry->Journal,
				SourceVolumeGuid,
				&sourceExt->Core);
		}
		if (!NT_SUCCESS(status))
		{
			if (sourceStore)
				QhDevStoreDestroy(sourceStore);
			ExAcquireFastMutex(&DriverExt->VolumeHandleMutex);
			DriverExt->CaptureTargetHandleId = 0;
			ExReleaseFastMutex(&DriverExt->VolumeHandleMutex);
			QHReleaseVolumeHandleEntry(journalEntry);
			(void)QHCloseVolumeHandle(DriverExt, journalHandleId);
			return status;
		}
	}
	QHReleaseVolumeHandleEntry(journalEntry);

	InterlockedExchange(&sourceExt->CaptureEnabled, 1);

	*JournalHandleId = journalHandleId;
	QH_LOG("[COW] configured journalHandle=%llu size=%llu sector=%lu sourceExt=%p\n",
		journalHandleId,
		journalEntry->PartitionSize,
		journalEntry->SectorSize,
		sourceExt);
	return STATUS_SUCCESS;
}

static NTSTATUS QHConfigureCapture(
	_In_ PQH_DRIVER_EXTENSION DriverExt,
	_In_ const GUID* SourceVolumeGuid,
	_In_ const GUID* JournalPartitionGuid,
	_In_ BOOLEAN FormatJournal,
	_Out_ PUINT64 JournalHandleId)
{
	NTSTATUS status;

	status = KeWaitForSingleObject(
		&DriverExt->CaptureConfigMutex,
		Executive,
		KernelMode,
		FALSE,
		NULL);
	if (!NT_SUCCESS(status))
		return status;
	status = QHConfigureCaptureInternal(
		DriverExt,
		SourceVolumeGuid,
		JournalPartitionGuid,
		FormatJournal,
		JournalHandleId);
	KeReleaseMutex(&DriverExt->CaptureConfigMutex, FALSE);
	return status;
}

static BOOLEAN QHGuidIsZero(_In_ const GUID* Guid)
{
	static const GUID zeroGuid = { 0 };
	return RtlCompareMemory(Guid, &zeroGuid, sizeof(GUID)) == sizeof(GUID);
}

static NTSTATUS QHActivateAutoJournal(
	_In_ PQH_DRIVER_EXTENSION DriverExt,
	_In_ UINT64 JournalHandleId,
	_Inout_ PQH_VOLUME_HANDLE_ENTRY JournalEntry)
{
	UINT64 sourcePartitionSize = 0;
	ULONG sourceSectorSize = 0;
	PQH_DEVICE_EXTENSION sourceExt;
	PQH_STORE sourceStore = NULL;
	GUID sourceGuid = JournalEntry->Journal.SourceVolumeGuid;
	NTSTATUS status;

	if (QHGuidIsZero(&sourceGuid))
		return STATUS_INVALID_PARAMETER;

	ExAcquireFastMutex(&DriverExt->VolumeHandleMutex);
	if (DriverExt->CaptureTargetHandleId != 0)
	{
		ExReleaseFastMutex(&DriverExt->VolumeHandleMutex);
		return STATUS_DEVICE_BUSY;
	}
	ExReleaseFastMutex(&DriverExt->VolumeHandleMutex);

	sourceExt = QHFindSourceExtensionByGuid(DriverExt, &sourceGuid);
	if (!sourceExt ||
		InterlockedCompareExchange(&sourceExt->Started, 0, 0) == 0 ||
		!sourceExt->VolumeGuidValid ||
		!sourceExt->LowerDeviceObject)
	{
		return STATUS_DEVICE_DOES_NOT_EXIST;
	}

	status = QHQueryDeviceGeometry(
		sourceExt->LowerDeviceObject,
		&sourcePartitionSize,
		&sourceSectorSize);
	if (!NT_SUCCESS(status))
		return status;

	QHDisableAllCaptureSources(DriverExt);
	sourceExt->VolumeGuid = sourceGuid;
	sourceExt->VolumeGuidValid = TRUE;
	sourceExt->SectorSize = sourceSectorSize;
	status = QhDevStoreCreate(
		sourceExt->LowerDeviceObject,
		sourcePartitionSize,
		sourceSectorSize,
		&sourceStore);
	if (NT_SUCCESS(status))
	{
		status = QhCoreBind(
			sourceStore,
			&JournalEntry->Journal,
			&sourceGuid,
			&sourceExt->Core);
	}
	if (!NT_SUCCESS(status))
	{
		if (sourceStore)
			QhDevStoreDestroy(sourceStore);
		return status;
	}

	ExAcquireFastMutex(&DriverExt->VolumeHandleMutex);
	if (DriverExt->CaptureTargetHandleId == 0)
		DriverExt->CaptureTargetHandleId = JournalHandleId;
	else
		status = STATUS_DEVICE_BUSY;
	ExReleaseFastMutex(&DriverExt->VolumeHandleMutex);
	if (!NT_SUCCESS(status))
	{
		QHDisableAndDestroyCapture(sourceExt);
		return status;
	}

	InterlockedExchange(&sourceExt->CaptureEnabled, 1);
	QH_LOG("[AUTO-CDP] enabled journalHandle=%llu sourceExt=%p\n",
		JournalHandleId, sourceExt);
	QHDbgGuid("[AUTO-CDP] source", &sourceGuid);
	return STATUS_SUCCESS;
}

static BOOLEAN QHGuidIsEqual(_In_ const GUID* A, _In_ const GUID* B)
{
	return RtlCompareMemory(A, B, sizeof(GUID)) == sizeof(GUID);
}

#define QH_AUTO_KIND_UNKNOWN 0
#define QH_AUTO_KIND_SOURCE  1
#define QH_AUTO_KIND_JOURNAL 2

static VOID QHMarkAutoDiscoverySettled(_Inout_ PQH_DRIVER_EXTENSION DriverExt)
{
	InterlockedExchange(&DriverExt->AutoDiscoverySettled, 1);
	KeSetEvent(&DriverExt->AutoDiscoverySettledEvent, IO_NO_INCREMENT, FALSE);
}

static VOID QHClearAutoDiscoverySettled(_Inout_ PQH_DRIVER_EXTENSION DriverExt)
{
	InterlockedExchange(&DriverExt->AutoDiscoverySettled, 0);
	KeClearEvent(&DriverExt->AutoDiscoverySettledEvent);
}

static PQH_DEVICE_EXTENSION QHFindStartedSourceByGuid(
	_In_ PQH_DRIVER_EXTENSION DriverExt,
	_In_ const GUID* SourceGuid)
{
	KIRQL oldIrql;
	PLIST_ENTRY entry;
	PQH_DEVICE_EXTENSION found = NULL;

	if (!SourceGuid || QHGuidIsZero(SourceGuid))
		return NULL;

	KeAcquireSpinLock(&DriverExt->DeviceObjectListLock, &oldIrql);
	for (entry = DriverExt->DeviceObjectListHead.Flink;
		entry != &DriverExt->DeviceObjectListHead;
		entry = entry->Flink)
	{
		PQH_DEVICE_LIST_NODE node =
			CONTAINING_RECORD(entry, QH_DEVICE_LIST_NODE, Entry);
		PQH_DEVICE_EXTENSION ext =
			(PQH_DEVICE_EXTENSION)node->DeviceObject->DeviceExtension;
		if (!ext)
			continue;
		if (InterlockedCompareExchange(&ext->Started, 0, 0) == 0)
			continue;
		if (InterlockedCompareExchange(&ext->AutoKind, 0, 0) != QH_AUTO_KIND_SOURCE)
			continue;
		if (!ext->VolumeGuidValid)
			continue;
		if (QHGuidIsEqual(&ext->VolumeGuid, SourceGuid))
		{
			found = ext;
			break;
		}
	}
	KeReleaseSpinLock(&DriverExt->DeviceObjectListLock, oldIrql);
	return found;
}

static BOOLEAN QHAutoDiscoveryHasUnclassified(
	_In_ PQH_DRIVER_EXTENSION DriverExt)
{
	KIRQL oldIrql;
	PLIST_ENTRY entry;
	BOOLEAN found = FALSE;

	KeAcquireSpinLock(&DriverExt->DeviceObjectListLock, &oldIrql);
	for (entry = DriverExt->DeviceObjectListHead.Flink;
		entry != &DriverExt->DeviceObjectListHead;
		entry = entry->Flink)
	{
		PQH_DEVICE_LIST_NODE node =
			CONTAINING_RECORD(entry, QH_DEVICE_LIST_NODE, Entry);
		PQH_DEVICE_EXTENSION ext =
			(PQH_DEVICE_EXTENSION)node->DeviceObject->DeviceExtension;
		if (ext &&
			InterlockedCompareExchange(&ext->Started, 0, 0) != 0 &&
			InterlockedCompareExchange(&ext->AutoKind, 0, 0) == QH_AUTO_KIND_UNKNOWN)
		{
			found = TRUE;
			break;
		}
	}
	KeReleaseSpinLock(&DriverExt->DeviceObjectListLock, oldIrql);
	return found;
}

static BOOLEAN QHAutoDiscoveryHasPendingJournal(
	_In_ PQH_DRIVER_EXTENSION DriverExt)
{
	BOOLEAN pending = FALSE;
	PLIST_ENTRY entry;

	ExAcquireFastMutex(&DriverExt->VolumeHandleMutex);
	if (DriverExt->CaptureTargetHandleId == 0)
	{
		for (entry = DriverExt->VolumeHandleList.Flink;
			entry != &DriverExt->VolumeHandleList;
			entry = entry->Flink)
		{
			PQH_VOLUME_HANDLE_ENTRY item =
				CONTAINING_RECORD(entry, QH_VOLUME_HANDLE_ENTRY, Entry);
			if (item->Closing || !item->Journal.Mounted)
				continue;
			if (QHFindStartedSourceByGuid(
				DriverExt,
				&item->Journal.SourceVolumeGuid) == NULL)
			{
				pending = TRUE;
				break;
			}
		}
	}
	ExReleaseFastMutex(&DriverExt->VolumeHandleMutex);
	return pending;
}

static VOID QHAutoDiscoveryRefreshSettled(
	_Inout_ PQH_DRIVER_EXTENSION DriverExt)
{
	if (DriverExt->CaptureTargetHandleId != 0 ||
		InterlockedCompareExchange(&DriverExt->AutoDiscoverySuppressed, 0, 0) != 0 ||
		InterlockedCompareExchange(&DriverExt->AutoDiscoveryStopping, 0, 0) != 0)
	{
		QHMarkAutoDiscoverySettled(DriverExt);
		return;
	}
	if (QHAutoDiscoveryHasUnclassified(DriverExt) ||
		QHAutoDiscoveryHasPendingJournal(DriverExt))
	{
		QHClearAutoDiscoverySettled(DriverExt);
		return;
	}
	QHMarkAutoDiscoverySettled(DriverExt);
}

static NTSTATUS QHClassifyStartedVolume(
	_Inout_ PQH_DRIVER_EXTENSION DriverExt,
	_In_ PDEVICE_OBJECT FilterDevice)
{
	PQH_DEVICE_EXTENSION ext;
	PQH_VOLUME_HANDLE_ENTRY journalEntry = NULL;
	UINT64 partitionSize = 0;
	ULONG sectorSize = 0;
	NTSTATUS status;
	GUID volumeGuid;
	GUID zeroGuid = { 0 };

	ext = (PQH_DEVICE_EXTENSION)FilterDevice->DeviceExtension;
	if (!ext ||
		InterlockedCompareExchange(&ext->Started, 0, 0) == 0)
	{
		return STATUS_INVALID_DEVICE_STATE;
	}
	if (InterlockedCompareExchange(&ext->AutoKind, 0, 0) != QH_AUTO_KIND_UNKNOWN)
		return STATUS_SUCCESS;
	if (!ExAcquireRundownProtection(&ext->AutoDiscoveryRundown))
		return STATUS_DEVICE_NOT_READY;
	if (!ext->LowerDeviceObject)
	{
		ExReleaseRundownProtection(&ext->AutoDiscoveryRundown);
		return STATUS_DEVICE_NOT_READY;
	}

	status = QHQueryDeviceGeometry(
		ext->LowerDeviceObject,
		&partitionSize,
		&sectorSize);
	if (!NT_SUCCESS(status))
	{
		ExReleaseRundownProtection(&ext->AutoDiscoveryRundown);
		return status;
	}

	journalEntry = (PQH_VOLUME_HANDLE_ENTRY)qhalloc(sizeof(*journalEntry));
	if (!journalEntry)
	{
		ExReleaseRundownProtection(&ext->AutoDiscoveryRundown);
		return STATUS_INSUFFICIENT_RESOURCES;
	}
	RtlZeroMemory(journalEntry, sizeof(*journalEntry));
	journalEntry->TargetLowerDevice = ext->LowerDeviceObject;
	journalEntry->PartitionSize = partitionSize;
	journalEntry->SectorSize = sectorSize;
	journalEntry->ReferenceCount = 1;
	KeInitializeEvent(&journalEntry->NoReferences, NotificationEvent, FALSE);
	QHJournalInitialize(
		&journalEntry->Journal,
		journalEntry->TargetLowerDevice,
		NULL,
		0,
		partitionSize,
		sectorSize,
		&zeroGuid);
	status = QHJournalMount(&journalEntry->Journal);
	if (NT_SUCCESS(status) &&
		!QHGuidIsZero(&journalEntry->Journal.SourceVolumeGuid))
	{
		GUID journalGuid = { 0 };

		status = IoVolumeDeviceToGuid(
			ext->PhysicalDeviceObject ?
				ext->PhysicalDeviceObject :
				FilterDevice,
			&journalGuid);
		if (!NT_SUCCESS(status))
			status = IoVolumeDeviceToGuid(ext->LowerDeviceObject, &journalGuid);
		if (NT_SUCCESS(status) && !QHGuidIsZero(&journalGuid))
		{
			journalEntry->VolumeGuid = journalGuid;
			journalEntry->VolumeGuidValid = TRUE;
		}
		journalEntry->HandleId =
			(UINT64)InterlockedIncrement64(&DriverExt->VolumeHandleNextId);
		ExAcquireFastMutex(&DriverExt->VolumeHandleMutex);
		InsertTailList(&DriverExt->VolumeHandleList, &journalEntry->Entry);
		ExReleaseFastMutex(&DriverExt->VolumeHandleMutex);
		InterlockedExchange(&ext->AutoKind, QH_AUTO_KIND_JOURNAL);
		ext->SectorSize = sectorSize;
		QH_DBG("[AUTO-CDP] classified JOURNAL filter=%p handle=%llu\n",
			FilterDevice, journalEntry->HandleId);
		QHDbgGuid("[AUTO-CDP] journal sourceGuid",
			&journalEntry->Journal.SourceVolumeGuid);
		ExReleaseRundownProtection(&ext->AutoDiscoveryRundown);
		return STATUS_SUCCESS;
	}

	// Probe I/O / resource failures: keep UNKNOWN and retry later.
	// STATUS_DISK_CORRUPT_ERROR means readable but no valid journal magic.
	if (!NT_SUCCESS(status) && status != STATUS_DISK_CORRUPT_ERROR)
	{
		QH_DBG("[AUTO-CDP] classify probe failed status=0x%08X filter=%p; retry\n",
			status, FilterDevice);
		QHJournalClose(&journalEntry->Journal);
		qhfree(journalEntry);
		ExReleaseRundownProtection(&ext->AutoDiscoveryRundown);
		return status;
	}

	QHJournalClose(&journalEntry->Journal);
	qhfree(journalEntry);

	RtlZeroMemory(&volumeGuid, sizeof(volumeGuid));
	status = IoVolumeDeviceToGuid(
		ext->PhysicalDeviceObject ?
			ext->PhysicalDeviceObject :
			FilterDevice,
		&volumeGuid);
	if (!NT_SUCCESS(status))
		status = IoVolumeDeviceToGuid(ext->LowerDeviceObject, &volumeGuid);
	if (NT_SUCCESS(status) && !QHGuidIsZero(&volumeGuid))
	{
		ext->VolumeGuid = volumeGuid;
		ext->VolumeGuidValid = TRUE;
	}
	else
	{
		ext->VolumeGuidValid = FALSE;
		QH_DBG("[AUTO-CDP] source guid query failed status=0x%08X filter=%p\n",
			status, FilterDevice);
	}
	ext->SectorSize = sectorSize;
	InterlockedExchange(&ext->AutoKind, QH_AUTO_KIND_SOURCE);
	QH_DBG("[AUTO-CDP] classified SOURCE filter=%p guidValid=%u\n",
		FilterDevice, ext->VolumeGuidValid ? 1u : 0u);
	if (ext->VolumeGuidValid)
		QHDbgGuid("[AUTO-CDP] source Guid", &ext->VolumeGuid);
	ExReleaseRundownProtection(&ext->AutoDiscoveryRundown);
	return STATUS_SUCCESS;
}

static NTSTATUS QHTryActivateReadyPairs(
	_Inout_ PQH_DRIVER_EXTENSION DriverExt)
{
	NTSTATUS result = STATUS_NOT_FOUND;

	for (;;)
	{
		PQH_VOLUME_HANDLE_ENTRY journalEntry = NULL;
		UINT64 journalHandleId = 0;
		PLIST_ENTRY entry;
		NTSTATUS status;

		if (DriverExt->CaptureTargetHandleId != 0)
			return STATUS_SUCCESS;

		ExAcquireFastMutex(&DriverExt->VolumeHandleMutex);
		for (entry = DriverExt->VolumeHandleList.Flink;
			entry != &DriverExt->VolumeHandleList;
			entry = entry->Flink)
		{
			PQH_VOLUME_HANDLE_ENTRY item =
				CONTAINING_RECORD(entry, QH_VOLUME_HANDLE_ENTRY, Entry);
			if (item->Closing || !item->Journal.Mounted)
				continue;
			if (QHFindStartedSourceByGuid(
				DriverExt,
				&item->Journal.SourceVolumeGuid) == NULL)
			{
				continue;
			}
			journalEntry = item;
			journalHandleId = item->HandleId;
			InterlockedIncrement(&item->ReferenceCount);
			break;
		}
		ExReleaseFastMutex(&DriverExt->VolumeHandleMutex);

		if (!journalEntry)
			break;

		status = QHActivateAutoJournal(
			DriverExt,
			journalHandleId,
			journalEntry);
		QHReleaseVolumeHandleEntry(journalEntry);
		if (NT_SUCCESS(status))
		{
			QH_DBG("[AUTO-CDP] pair activated journalHandle=%llu\n",
				journalHandleId);
			return STATUS_SUCCESS;
		}
		QH_LOG("[AUTO-CDP] pair activate failed status=0x%08X\n", status);
		result = status;
		break;
	}
	return result;
}

static VOID QHClassifyAllUnknownVolumes(
	_Inout_ PQH_DRIVER_EXTENSION DriverExt)
{
	PDEVICE_OBJECT devices[128];
	ULONG deviceCount = 0;
	ULONG i;
	KIRQL oldIrql;

	KeAcquireSpinLock(&DriverExt->DeviceObjectListLock, &oldIrql);
	{
		PLIST_ENTRY entry;
		for (entry = DriverExt->DeviceObjectListHead.Flink;
			entry != &DriverExt->DeviceObjectListHead &&
			deviceCount < RTL_NUMBER_OF(devices);
			entry = entry->Flink)
		{
			PQH_DEVICE_LIST_NODE node =
				CONTAINING_RECORD(entry, QH_DEVICE_LIST_NODE, Entry);
			PQH_DEVICE_EXTENSION ext =
				(PQH_DEVICE_EXTENSION)node->DeviceObject->DeviceExtension;
			if (ext &&
				InterlockedCompareExchange(&ext->Started, 0, 0) != 0 &&
				InterlockedCompareExchange(&ext->AutoKind, 0, 0) ==
					QH_AUTO_KIND_UNKNOWN)
			{
				devices[deviceCount] = node->DeviceObject;
				ObReferenceObject(devices[deviceCount]);
				++deviceCount;
			}
		}
	}
	KeReleaseSpinLock(&DriverExt->DeviceObjectListLock, oldIrql);

	for (i = 0; i < deviceCount; ++i)
	{
		if (InterlockedCompareExchange(&DriverExt->AutoDiscoveryStopping, 0, 0) != 0)
			break;
		(void)QHClassifyStartedVolume(DriverExt, devices[i]);
		ObDereferenceObject(devices[i]);
	}
	for (; i < deviceCount; ++i)
		ObDereferenceObject(devices[i]);
}

static VOID QHAutoDiscoveryWorker(_In_ PVOID Context)
{
	PQH_DRIVER_EXTENSION driverExt = (PQH_DRIVER_EXTENSION)Context;
	NTSTATUS status;

	InterlockedExchange(&driverExt->AutoDiscoveryRunning, 1);
	status = KeWaitForSingleObject(
		&driverExt->CaptureConfigMutex,
		Executive,
		KernelMode,
		FALSE,
		NULL);
	if (NT_SUCCESS(status) &&
		InterlockedCompareExchange(&driverExt->AutoDiscoveryStopping, 0, 0) == 0 &&
		InterlockedCompareExchange(&driverExt->AutoDiscoverySuppressed, 0, 0) == 0)
	{
		QH_DBG("[AUTO-CDP] classify+pair begin\n");
		QHClassifyAllUnknownVolumes(driverExt);
		status = QHTryActivateReadyPairs(driverExt);
		if (NT_SUCCESS(status))
			QH_LOG("[AUTO-CDP] CDP enabled\n");
		else
			QH_DBG("[AUTO-CDP] no ready pair status=0x%08X\n", status);
		QHAutoDiscoveryRefreshSettled(driverExt);
		KeReleaseMutex(&driverExt->CaptureConfigMutex, FALSE);
	}
	else if (NT_SUCCESS(status))
	{
		QHAutoDiscoveryRefreshSettled(driverExt);
		KeReleaseMutex(&driverExt->CaptureConfigMutex, FALSE);
	}

	InterlockedExchange(&driverExt->AutoDiscoveryRunning, 0);
	InterlockedExchange(&driverExt->AutoDiscoveryQueued, 0);
	KeSetEvent(&driverExt->AutoDiscoveryIdle, IO_NO_INCREMENT, FALSE);

	if (InterlockedCompareExchange(&driverExt->AutoDiscoveryStopping, 0, 0) == 0 &&
		InterlockedCompareExchange(&driverExt->AutoDiscoverySuppressed, 0, 0) == 0 &&
		driverExt->CaptureTargetHandleId == 0 &&
		QHAutoDiscoveryHasUnclassified(driverExt))
	{
		LARGE_INTEGER delay;

		// Probe I/O may fail right after START; back off before retry.
		QH_DBG("[AUTO-CDP] unclassified remains; retry after delay\n");
		QHClearAutoDiscoverySettled(driverExt);
		delay.QuadPart = -1000000LL; // 100ms
		KeDelayExecutionThread(KernelMode, FALSE, &delay);
		if (InterlockedCompareExchange(&driverExt->AutoDiscoveryStopping, 0, 0) == 0 &&
			InterlockedCompareExchange(&driverExt->AutoDiscoverySuppressed, 0, 0) == 0 &&
			driverExt->CaptureTargetHandleId == 0 &&
			QHAutoDiscoveryHasUnclassified(driverExt))
		{
			QHQueueAutoDiscovery(driverExt);
		}
	}
	else if (
		InterlockedCompareExchange(&driverExt->AutoDiscoveryStopping, 0, 0) == 0 &&
		InterlockedCompareExchange(&driverExt->AutoDiscoverySuppressed, 0, 0) == 0 &&
		driverExt->CaptureTargetHandleId == 0 &&
		QHAutoDiscoveryHasPendingJournal(driverExt))
	{
		// Journal waits on a source START; do not spin �� next START re-queues.
		QH_DBG("[AUTO-CDP] journal pending source; wait for START\n");
		QHClearAutoDiscoverySettled(driverExt);
	}
}

VOID QHInitializeAutoDiscovery(_Inout_ PQH_DRIVER_EXTENSION DriverExt)
{
	KeInitializeEvent(&DriverExt->AutoDiscoveryIdle, NotificationEvent, TRUE);
	KeInitializeEvent(
		&DriverExt->AutoDiscoverySettledEvent,
		NotificationEvent,
		FALSE);
	InterlockedExchange(&DriverExt->AutoDiscoverySettled, 0);
	InterlockedExchange(&DriverExt->AutoDiscoveryRunning, 0);
	ExInitializeWorkItem(
		&DriverExt->AutoDiscoveryWorkItem,
		QHAutoDiscoveryWorker,
		DriverExt);
}

VOID QHQueueAutoDiscovery(_Inout_ PQH_DRIVER_EXTENSION DriverExt)
{
	if (!DriverExt)
		return;
	if (InterlockedCompareExchange(&DriverExt->AutoDiscoveryStopping, 0, 0) != 0)
		return;
	if (InterlockedCompareExchange(&DriverExt->AutoDiscoverySuppressed, 0, 0) != 0)
	{
		QHMarkAutoDiscoverySettled(DriverExt);
		return;
	}
	if (InterlockedCompareExchange(&DriverExt->AutoDiscoveryQueued, 1, 0) != 0)
	{
		QH_DBG("[AUTO-CDP] worker already queued/running\n");
		return;
	}
	QH_DBG("[AUTO-CDP] queueing classify+pair worker\n");
	KeClearEvent(&DriverExt->AutoDiscoveryIdle);
	ExQueueWorkItem(&DriverExt->AutoDiscoveryWorkItem, CriticalWorkQueue);
}

VOID QHScheduleAutoDiscovery(_Inout_ PQH_DRIVER_EXTENSION DriverExt)
{
	QHQueueAutoDiscovery(DriverExt);
}

VOID QHStopAutoDiscovery(_Inout_ PQH_DRIVER_EXTENSION DriverExt)
{
	InterlockedExchange(&DriverExt->AutoDiscoveryStopping, 1);
	QH_DBG("[AUTO-CDP] stopping worker\n");
	QHMarkAutoDiscoverySettled(DriverExt);
	if (InterlockedCompareExchange(&DriverExt->AutoDiscoveryQueued, 0, 0) != 0)
	{
		KeWaitForSingleObject(
			&DriverExt->AutoDiscoveryIdle,
			Executive,
			KernelMode,
			FALSE,
			NULL);
	}
}

static VOID QHWaitForAutoDiscoveryIfNeeded(
	_In_ PQH_DRIVER_EXTENSION DriverExt)
{
	if (!DriverExt)
		return;
	if (InterlockedCompareExchange(&DriverExt->AutoDiscoverySettled, 0, 0) != 0)
		return;
	if (InterlockedCompareExchange(&DriverExt->AutoDiscoverySuppressed, 0, 0) != 0)
		return;
	if (InterlockedCompareExchange(&DriverExt->AutoDiscoveryRunning, 0, 0) != 0)
		return;
	if (KeGetCurrentIrql() != PASSIVE_LEVEL)
		return;

	QH_DBG("[AUTO-CDP] write gated until discovery settles\n");
	KeWaitForSingleObject(
		&DriverExt->AutoDiscoverySettledEvent,
		Executive,
		KernelMode,
		FALSE,
		NULL);
}


static PQH_PREVIEW_SESSION QHLookupPreviewSessionLocked(
	_In_ PQH_DRIVER_EXTENSION DriverExt,
	_In_ UINT64 HandleId)
{
	PLIST_ENTRY entry = DriverExt->PreviewSessionList.Flink;
	while (entry != &DriverExt->PreviewSessionList)
	{
		PQH_PREVIEW_SESSION session =
			CONTAINING_RECORD(entry, QH_PREVIEW_SESSION, Entry);
		if (session->HandleId == HandleId)
			return session;
		entry = entry->Flink;
	}
	return NULL;
}

static BOOLEAN QHAnyPreviewSessionActive(
	_In_ PQH_DRIVER_EXTENSION DriverExt)
{
	BOOLEAN active;

	ExAcquireFastMutex(&DriverExt->PreviewSessionMutex);
	active = !IsListEmpty(&DriverExt->PreviewSessionList);
	ExReleaseFastMutex(&DriverExt->PreviewSessionMutex);
	return active;
}

static PQH_PREVIEW_SESSION QHAcquirePreviewSession(
	_In_ PQH_DRIVER_EXTENSION DriverExt,
	_In_ UINT64 HandleId)
{
	PQH_PREVIEW_SESSION session;

	ExAcquireFastMutex(&DriverExt->PreviewSessionMutex);
	session = QHLookupPreviewSessionLocked(DriverExt, HandleId);
	if (session && !session->Closing)
		InterlockedIncrement(&session->ReferenceCount);
	else
		session = NULL;
	ExReleaseFastMutex(&DriverExt->PreviewSessionMutex);
	return session;
}

static VOID QHReleasePreviewSession(_In_ PQH_PREVIEW_SESSION Session)
{
	if (InterlockedDecrement(&Session->ReferenceCount) == 0)
		KeSetEvent(&Session->NoReferences, IO_NO_INCREMENT, FALSE);
}

static VOID QHDestroyPreviewSession(
	_In_ PQH_DRIVER_EXTENSION DriverExt,
	_In_ PQH_PREVIEW_SESSION Session)
{
	QHReleasePreviewSession(Session); // Drop list ownership.
	KeWaitForSingleObject(
		&Session->NoReferences,
		Executive,
		KernelMode,
		FALSE,
		NULL);
	if (Session->SourceVolumeHandleId)
		(void)QHCloseVolumeHandle(
			DriverExt,
			Session->SourceVolumeHandleId);
	if (Session->JournalEntry)
		QHReleaseVolumeHandleEntry(Session->JournalEntry);
	qhfree(Session);
}

static NTSTATUS QHBeginPreviewSession(
	_In_ PQH_DRIVER_EXTENSION DriverExt,
	_In_ const QH_PREVIEW_BEGIN_REQUEST* Request,
	_Out_ PQH_PREVIEW_BEGIN_REPLY Reply)
{
	PQH_VOLUME_HANDLE_ENTRY journalEntry = NULL;
	PQH_PREVIEW_SESSION session = NULL;
	PQH_DEVICE_EXTENSION sourceExt = NULL;
	UINT64 sourceHandleId = 0;
	UINT64 oldestTime = 0;
	UINT64 newestTime = 0;
	BOOLEAN phaseTransitioned = FALSE;
	NTSTATUS status;

	RtlZeroMemory(Reply, sizeof(*Reply));
	journalEntry = QHAcquireCaptureTarget(DriverExt);
	if (!journalEntry)
		return STATUS_DEVICE_NOT_READY;

	sourceExt = QHFindSourceExtensionByGuid(
		DriverExt,
		&Request->SourceVolumeGuid);
	if (!sourceExt)
	{
		QHReleaseVolumeHandleEntry(journalEntry);
		return STATUS_DEVICE_DOES_NOT_EXIST;
	}
	if (InterlockedCompareExchange(&sourceExt->Phase, 0, 0) !=
		(LONG)QH_PHASE_GENERAL)
	{
		QHReleaseVolumeHandleEntry(journalEntry);
		return STATUS_INVALID_DEVICE_STATE;
	}
	if (QHAnyPreviewSessionActive(DriverExt))
	{
		QHReleaseVolumeHandleEntry(journalEntry);
		return STATUS_INVALID_DEVICE_STATE;
	}
	if (!sourceExt->Core)
	{
		QHReleaseVolumeHandleEntry(journalEntry);
		return STATUS_DEVICE_NOT_READY;
	}

	if (!journalEntry->Journal.Mounted ||
		RtlCompareMemory(
			&journalEntry->Journal.SourceVolumeGuid,
			&Request->SourceVolumeGuid,
			sizeof(GUID)) != sizeof(GUID))
	{
		status = STATUS_NOT_FOUND;
		goto cleanup;
	}

	status = QHJournalQueryTimeRange(
		&journalEntry->Journal,
		&oldestTime,
		&newestTime);
	if (status == STATUS_NOT_FOUND)
	{
		oldestTime = 0;
		newestTime = 0;
		status = STATUS_SUCCESS;
	}
	else if (!NT_SUCCESS(status))
	{
		goto cleanup;
	}
	else if (Request->TargetTime100ns < oldestTime)
	{
		status = STATUS_NOT_FOUND;
		goto cleanup;
	}

	status = QHOpenVolumeHandle(
		DriverExt,
		&Request->SourceVolumeGuid,
		&sourceHandleId);
	if (!NT_SUCCESS(status))
		goto cleanup;

	if (InterlockedCompareExchange(
			&sourceExt->Phase,
			(LONG)QH_PHASE_PREVIEW,
			(LONG)QH_PHASE_GENERAL) != (LONG)QH_PHASE_GENERAL)
	{
		status = STATUS_INVALID_DEVICE_STATE;
		goto cleanup;
	}
	phaseTransitioned = TRUE;

	if (QHAnyPreviewSessionActive(DriverExt))
	{
		status = STATUS_INVALID_DEVICE_STATE;
		goto cleanup;
	}

	session = (PQH_PREVIEW_SESSION)qhalloc(sizeof(*session));
	if (!session)
	{
		status = STATUS_INSUFFICIENT_RESOURCES;
		goto cleanup;
	}
	RtlZeroMemory(session, sizeof(*session));
	session->HandleId =
		(UINT64)InterlockedIncrement64(&DriverExt->PreviewSessionNextId);
	session->TargetTime100ns = Request->TargetTime100ns;
	session->SourceVolumeHandleId = sourceHandleId;
	session->JournalEntry = journalEntry;
	session->SourceVolumeGuid = Request->SourceVolumeGuid;
	session->ReferenceCount = 1;
	KeInitializeEvent(
		&session->NoReferences,
		NotificationEvent,
		FALSE);

	ExAcquireFastMutex(&DriverExt->PreviewSessionMutex);
	if (!IsListEmpty(&DriverExt->PreviewSessionList))
	{
		ExReleaseFastMutex(&DriverExt->PreviewSessionMutex);
		status = STATUS_INVALID_DEVICE_STATE;
		goto cleanup;
	}
	InsertTailList(&DriverExt->PreviewSessionList, &session->Entry);
	ExReleaseFastMutex(&DriverExt->PreviewSessionMutex);

	status = QhCorePreviewBegin(sourceExt->Core, Request->TargetTime100ns);
	if (!NT_SUCCESS(status))
	{
		ExAcquireFastMutex(&DriverExt->PreviewSessionMutex);
		RemoveEntryList(&session->Entry);
		ExReleaseFastMutex(&DriverExt->PreviewSessionMutex);
		goto cleanup;
	}
	Reply->PreviewHandle = session->HandleId;
	Reply->TargetTime100ns = session->TargetTime100ns;
	Reply->OldestRecoverable100ns = oldestTime;
	Reply->NewestRecoverable100ns = newestTime;

	QH_DBG("[PREVIEW] begin handle=%llu target=%llu (Core)\n",
		session->HandleId,
		session->TargetTime100ns);
	return STATUS_SUCCESS;

cleanup:
	if (session)
	{
		qhfree(session);
		journalEntry = NULL;
		sourceHandleId = 0;
	}
	if (phaseTransitioned && sourceExt)
	{
		if (sourceExt->Core)
			(void)QhCorePreviewEnd(sourceExt->Core);
		InterlockedExchange(&sourceExt->Phase, (LONG)QH_PHASE_GENERAL);
	}
	if (sourceHandleId)
		(void)QHCloseVolumeHandle(DriverExt, sourceHandleId);
	if (journalEntry)
		QHReleaseVolumeHandleEntry(journalEntry);
	return status;
}

static NTSTATUS QHEndPreviewSession(
	_In_ PQH_DRIVER_EXTENSION DriverExt,
	_In_ UINT64 HandleId)
{
	PQH_PREVIEW_SESSION session;
	GUID sourceGuid;
	BOOLEAN haveGuid = FALSE;

	ExAcquireFastMutex(&DriverExt->PreviewSessionMutex);
	session = QHLookupPreviewSessionLocked(DriverExt, HandleId);
	if (session)
	{
		RemoveEntryList(&session->Entry);
		session->Closing = TRUE;
		sourceGuid = session->SourceVolumeGuid;
		haveGuid = TRUE;
	}
	ExReleaseFastMutex(&DriverExt->PreviewSessionMutex);
	if (!session)
		return STATUS_NOT_FOUND;

	QHDestroyPreviewSession(DriverExt, session);

	if (haveGuid)
	{
		PQH_DEVICE_EXTENSION sourceExt =
			QHFindSourceExtensionByGuid(DriverExt, &sourceGuid);
		if (sourceExt)
		{
			if (sourceExt->Core)
				(void)QhCorePreviewEnd(sourceExt->Core);
			InterlockedExchange(&sourceExt->Phase, (LONG)QH_PHASE_GENERAL);
		}
	}

	QH_DBG("[PREVIEW] end handle=%llu\n", HandleId);
	return STATUS_SUCCESS;
}

VOID QHCloseAllPreviewSessions(_In_ PQH_DRIVER_EXTENSION DriverExt)
{
	for (;;)
	{
		PQH_PREVIEW_SESSION session = NULL;
		GUID sourceGuid;
		BOOLEAN haveGuid = FALSE;

		ExAcquireFastMutex(&DriverExt->PreviewSessionMutex);
		if (!IsListEmpty(&DriverExt->PreviewSessionList))
		{
			PLIST_ENTRY entry =
				RemoveHeadList(&DriverExt->PreviewSessionList);
			session = CONTAINING_RECORD(
				entry,
				QH_PREVIEW_SESSION,
				Entry);
			session->Closing = TRUE;
			sourceGuid = session->SourceVolumeGuid;
			haveGuid = TRUE;
		}
		ExReleaseFastMutex(&DriverExt->PreviewSessionMutex);
		if (!session)
			break;

		if (haveGuid)
		{
			PQH_DEVICE_EXTENSION sourceExt =
				QHFindSourceExtensionByGuid(DriverExt, &sourceGuid);
			if (sourceExt)
			{
				if (sourceExt->Core)
					(void)QhCorePreviewEnd(sourceExt->Core);
				InterlockedExchange(&sourceExt->Phase, (LONG)QH_PHASE_GENERAL);
			}
		}

		QHDestroyPreviewSession(DriverExt, session);
	}
}

static NTSTATUS QHReadPreviewSession(
	_In_ PQH_DRIVER_EXTENSION DriverExt,
	_In_ const QH_PREVIEW_READ_REQUEST* Request,
	_Out_writes_bytes_(Request->ByteLength) PVOID OutputBuffer)
{
	PQH_PREVIEW_SESSION session;
	PQH_DEVICE_EXTENSION sourceExt = NULL;
	BOOLEAN historyLocked = FALSE;
	NTSTATUS status;

	if (!Request->ByteLength ||
		Request->ByteLength > QH_CMD3_MAX_READ_BYTES)
	{
		return STATUS_INVALID_PARAMETER;
	}

	session = QHAcquirePreviewSession(
		DriverExt,
		Request->PreviewHandle);
	if (!session)
		return STATUS_INVALID_HANDLE;

	sourceExt = QHFindSourceExtensionByGuid(
		DriverExt,
		&session->SourceVolumeGuid);
	if (!sourceExt || !sourceExt->Core)
	{
		status = STATUS_DEVICE_NOT_READY;
		goto cleanup;
	}

	KeWaitForSingleObject(
		&sourceExt->HistoryMutex,
		Executive,
		KernelMode,
		FALSE,
		NULL);
	historyLocked = TRUE;

	status = QhCoreRead(
		sourceExt->Core,
		Request->ByteOffset,
		Request->ByteLength,
		OutputBuffer);
	if (NT_SUCCESS(status))
	{
		QH_DBG("[PREVIEW] core read handle=%llu offset=%llu len=%lu\n",
			Request->PreviewHandle,
			Request->ByteOffset,
			Request->ByteLength);
	}

cleanup:
	if (historyLocked && sourceExt)
		KeReleaseMutex(&sourceExt->HistoryMutex, FALSE);
	QHReleasePreviewSession(session);
	return status;
}

static NTSTATUS QHReadVolumeByHandle(
	_In_ PQH_DRIVER_EXTENSION DriverExt,
	_In_ UINT64 HandleId,
	_In_ UINT64 ByteOffset,
	_In_ ULONG ByteLength,
	_Out_writes_bytes_(ByteLength) PVOID Buffer)
{
	NTSTATUS Status;
	HANDLE fileHandle = NULL;
	IO_STATUS_BLOCK iosb;
	LARGE_INTEGER offset;
	PQH_VOLUME_HANDLE_ENTRY item;

	if (KeGetCurrentIrql() != PASSIVE_LEVEL)
		return STATUS_INVALID_DEVICE_STATE;

	if (ByteLength == 0 || ByteLength > QH_CMD3_MAX_READ_BYTES)
		return STATUS_INVALID_PARAMETER;

	if ((ByteOffset % QH_SECTOR_SIZE_DEFAULT) != 0 ||
		(ByteLength % QH_SECTOR_SIZE_DEFAULT) != 0)
	{
		return STATUS_INVALID_PARAMETER;
	}

	ExAcquireFastMutex(&DriverExt->VolumeHandleMutex);
	item = QHLookupVolumeHandleLocked(DriverExt, HandleId);
	if (item)
		fileHandle = item->FileHandle;
	ExReleaseFastMutex(&DriverExt->VolumeHandleMutex);

	if (!fileHandle)
		return STATUS_INVALID_HANDLE;

	offset.QuadPart = (LONGLONG)ByteOffset;
	Status = ZwReadFile(
		fileHandle,
		NULL,
		NULL,
		NULL,
		&iosb,
		Buffer,
		ByteLength,
		&offset,
		NULL);

	if (!NT_SUCCESS(Status))
	{
		QH_LOG("read handle=%llu offset=%llu len=%lu failed 0x%08X\n",
			HandleId, ByteOffset, ByteLength, Status);
		return Status;
	}

	if (iosb.Information != ByteLength)
		return STATUS_UNEXPECTED_IO_ERROR;

	return STATUS_SUCCESS;
}

static NTSTATUS QHSendToNextDevice(_In_ PDEVICE_OBJECT DeviceObject, _Inout_ PIRP Irp)
{
	IoSkipCurrentIrpStackLocation(Irp);
	return IoCallDriver(DeviceObject, Irp);
}

NTSTATUS QHIrpDispatchDefault(_In_ PDEVICE_OBJECT DeviceObject, _Inout_ PIRP Irp)
{
	PQH_DEVICE_EXTENSION DeviceExtension = (PQH_DEVICE_EXTENSION)DeviceObject->DeviceExtension;

	if (DeviceExtension && DeviceExtension->LowerDeviceObject)
		return QHSendToNextDevice(DeviceExtension->LowerDeviceObject, Irp);

	return QHCompleteIrp(Irp, STATUS_SUCCESS, 0);
}

static NTSTATUS QHBeginRecovery(
	_In_ PQH_DRIVER_EXTENSION DriverExt,
	_In_ const QH_RECOVERY_BEGIN_REQUEST* Request,
	_Out_ PQH_RECOVERY_BEGIN_REPLY Reply)
{
	PQH_DEVICE_EXTENSION sourceExt;
	UINT64 oldestTime = 0;
	UINT64 newestTime = 0;
	LONG previousPhase;
	NTSTATUS status;

	RtlZeroMemory(Reply, sizeof(*Reply));
	sourceExt = QHFindSourceExtensionByGuid(
		DriverExt,
		&Request->SourceVolumeGuid);
	if (!sourceExt)
		return STATUS_DEVICE_DOES_NOT_EXIST;
	if (InterlockedCompareExchange(&sourceExt->CaptureEnabled, 0, 0) == 0)
		return STATUS_DEVICE_NOT_READY;
	if (!sourceExt->Core)
		return STATUS_DEVICE_NOT_READY;
	if (QHAnyPreviewSessionActive(DriverExt))
		return STATUS_INVALID_DEVICE_STATE;
	// Recovery services paging reads from a worker and can therefore not be
	// activated on a volume that backs the system paging path.
	if (InterlockedCompareExchange(&sourceExt->PagingPathCount, 0, 0) != 0)
	{
		QH_LOG(
			"[RECOVERY] begin rejected: pagingPathCount=%ld\n",
			InterlockedCompareExchange(&sourceExt->PagingPathCount, 0, 0));
		return STATUS_DEVICE_BUSY;
	}

	previousPhase = InterlockedCompareExchange(
		&sourceExt->Phase,
		(LONG)QH_PHASE_RECOVERY,
		(LONG)QH_PHASE_GENERAL);
	if (previousPhase != (LONG)QH_PHASE_GENERAL)
		return STATUS_INVALID_DEVICE_STATE;

	status = QhCoreQueryTimeRange(
		sourceExt->Core,
		&oldestTime,
		&newestTime);
	if (status == STATUS_NOT_FOUND)
	{
		oldestTime = 0;
		newestTime = 0;
		status = STATUS_SUCCESS;
	}
	else if (!NT_SUCCESS(status))
	{
		InterlockedExchange(&sourceExt->Phase, previousPhase);
		return status;
	}
	else if (Request->TargetTime100ns < oldestTime)
	{
		InterlockedExchange(&sourceExt->Phase, previousPhase);
		return STATUS_NOT_FOUND;
	}

	KeWaitForSingleObject(
		&sourceExt->HistoryMutex,
		Executive,
		KernelMode,
		FALSE,
		NULL);
	status = QhCoreRecoveryBegin(sourceExt->Core, Request->TargetTime100ns);
	KeReleaseMutex(&sourceExt->HistoryMutex, FALSE);
	if (!NT_SUCCESS(status))
	{
		InterlockedExchange(&sourceExt->Phase, previousPhase);
		return status;
	}

	Reply->Phase = QH_PHASE_RECOVERY;
	Reply->TargetTime100ns = Request->TargetTime100ns;
	Reply->OldestRecoverable100ns = oldestTime;
	Reply->NewestRecoverable100ns = newestTime;
	QH_LOG("[PHASE] recovery prepared target=%llu; waiting for commit\n",
		Request->TargetTime100ns);
	return STATUS_SUCCESS;
}

static NTSTATUS QHCommitRecovery(
	_In_ PQH_DRIVER_EXTENSION DriverExt,
	_In_ const QH_RECOVERY_CONTROL_REQUEST* Request,
	_Out_ PQH_RECOVERY_COMMIT_REPLY Reply)
{
	PQH_DEVICE_EXTENSION sourceExt;
	UINT64 targetTime;
	NTSTATUS status;

	RtlZeroMemory(Reply, sizeof(*Reply));
	sourceExt = QHFindSourceExtensionByGuid(
		DriverExt,
		&Request->SourceVolumeGuid);
	if (!sourceExt)
		return STATUS_DEVICE_DOES_NOT_EXIST;
	if (!sourceExt->Core ||
		InterlockedCompareExchange(&sourceExt->Phase, 0, 0) !=
			(LONG)QH_PHASE_RECOVERY)
	{
		return STATUS_INVALID_DEVICE_STATE;
	}

	KeWaitForSingleObject(
		&sourceExt->HistoryMutex,
		Executive,
		KernelMode,
		FALSE,
		NULL);
	targetTime = QhCoreGetTargetTime100ns(sourceExt->Core);
	status = QhCoreRecoveryCommit(sourceExt->Core);
	KeReleaseMutex(&sourceExt->HistoryMutex, FALSE);
	if (!NT_SUCCESS(status))
		return status;

	InterlockedExchange(&sourceExt->Phase, (LONG)QH_PHASE_GENERAL);
	Reply->Phase = QH_PHASE_GENERAL;
	Reply->TargetTime100ns = targetTime;
	QH_LOG("[PHASE] recovery commit complete target=%llu -> normal\n",
		targetTime);
	return STATUS_SUCCESS;
}

static NTSTATUS QHCancelRecovery(
	_In_ PQH_DRIVER_EXTENSION DriverExt,
	_In_ const QH_RECOVERY_CONTROL_REQUEST* Request)
{
	PQH_DEVICE_EXTENSION sourceExt;
	NTSTATUS status;

	sourceExt = QHFindSourceExtensionByGuid(
		DriverExt,
		&Request->SourceVolumeGuid);
	if (!sourceExt)
		return STATUS_DEVICE_DOES_NOT_EXIST;
	if (!sourceExt->Core ||
		InterlockedCompareExchange(&sourceExt->Phase, 0, 0) !=
			(LONG)QH_PHASE_RECOVERY)
	{
		return STATUS_INVALID_DEVICE_STATE;
	}

	KeWaitForSingleObject(
		&sourceExt->HistoryMutex,
		Executive,
		KernelMode,
		FALSE,
		NULL);
	status = QhCoreRecoveryCancel(sourceExt->Core);
	KeReleaseMutex(&sourceExt->HistoryMutex, FALSE);
	if (!NT_SUCCESS(status))
		return status;

	InterlockedExchange(&sourceExt->Phase, (LONG)QH_PHASE_GENERAL);
	QH_LOG("[PHASE] recovery cancelled -> normal\n");
	return STATUS_SUCCESS;
}

static NTSTATUS QHQueryTimeRange(
	_In_ PQH_DRIVER_EXTENSION DriverExt,
	_In_ const QH_TIME_RANGE_QUERY_REQUEST* Request,
	_Out_ PQH_TIME_RANGE_QUERY_REPLY Reply)
{
	PQH_DEVICE_EXTENSION sourceExt;
	NTSTATUS status;

	RtlZeroMemory(Reply, sizeof(*Reply));
	sourceExt = QHFindSourceExtensionByGuid(
		DriverExt,
		&Request->SourceVolumeGuid);
	if (!sourceExt)
		return STATUS_DEVICE_DOES_NOT_EXIST;
	if (InterlockedCompareExchange(&sourceExt->CaptureEnabled, 0, 0) == 0)
		return STATUS_DEVICE_NOT_READY;
	if (!sourceExt->Core)
		return STATUS_DEVICE_NOT_READY;

	status = QhCoreQueryTimeRange(
		sourceExt->Core,
		&Reply->OldestRecord100ns,
		&Reply->NewestRecord100ns);
	if (status == STATUS_NOT_FOUND)
	{
		Reply->HasRecords = 0;
		Reply->OldestRecord100ns = 0;
		Reply->NewestRecord100ns = 0;
		return STATUS_SUCCESS;
	}
	if (!NT_SUCCESS(status))
		return status;

	Reply->HasRecords = 1;
	return STATUS_SUCCESS;
}

static NTSTATUS QHQueryPhase(
	_In_ PQH_DRIVER_EXTENSION DriverExt,
	_In_ const QH_PHASE_QUERY_REQUEST* Request,
	_Out_ PQH_PHASE_QUERY_REPLY Reply)
{
	PQH_DEVICE_EXTENSION sourceExt;
	PQH_VOLUME_HANDLE_ENTRY journalEntry = NULL;
	UINT64 journalHandleId = 0;

	RtlZeroMemory(Reply, sizeof(*Reply));
	Reply->Status = QH_STATUS_UNPROTECTED;

	sourceExt = QHFindSourceExtensionByGuid(
		DriverExt,
		&Request->SourceVolumeGuid);
	if (!sourceExt ||
		InterlockedCompareExchange(&sourceExt->CaptureEnabled, 0, 0) == 0)
	{
		return STATUS_SUCCESS;
	}

	Reply->Status = (LONG)InterlockedCompareExchange(&sourceExt->Phase, 0, 0);
	if (sourceExt->Core)
		Reply->RecoveryTargetTime100ns =
			QhCoreGetTargetTime100ns(sourceExt->Core);

	ExAcquireFastMutex(&DriverExt->VolumeHandleMutex);
	journalHandleId = DriverExt->CaptureTargetHandleId;
	if (journalHandleId != 0)
		journalEntry = QHLookupVolumeHandleLocked(DriverExt, journalHandleId);
	if (journalEntry && journalEntry->VolumeGuidValid)
		Reply->JournalPartitionGuid = journalEntry->VolumeGuid;
	ExReleaseFastMutex(&DriverExt->VolumeHandleMutex);
	return STATUS_SUCCESS;
}

static NTSTATUS QHRecoveryFillReadBuffer(
	_In_ PQH_DEVICE_EXTENSION DevExt,
	_In_ UINT64 Offset,
	_In_ ULONG Length,
	_Out_writes_bytes_(Length) PVOID Buffer)
{
	NTSTATUS status = STATUS_SUCCESS;
	ULONG completed = 0;

	if (!DevExt->Core)
		return STATUS_DEVICE_NOT_READY;

	QH_DBG(
		"[RECOVERY] read mutex wait offset=%llu len=%lu\n",
		Offset,
		Length);
	KeWaitForSingleObject(
		&DevExt->HistoryMutex,
		Executive,
		KernelMode,
		FALSE,
		NULL);
	QH_DBG(
		"[RECOVERY] read mutex acquired offset=%llu len=%lu\n",
		Offset,
		Length);
	while (completed < Length)
	{
		ULONG chunk = Length - completed;
		if (chunk > QH_CMD3_MAX_READ_BYTES)
			chunk = QH_CMD3_MAX_READ_BYTES;
		status = QhCoreRead(
			DevExt->Core,
			Offset + completed,
			chunk,
			(PUCHAR)Buffer + completed);
		if (!NT_SUCCESS(status))
			break;
		completed += chunk;
	}
	QH_DBG(
		"[RECOVERY] core read end offset=%llu len=%lu status=0x%08X\n",
		Offset,
		Length,
		status);
	KeReleaseMutex(&DevExt->HistoryMutex, FALSE);
	QH_DBG(
		"[RECOVERY] read mutex released offset=%llu len=%lu\n",
		Offset,
		Length);
	return status;
}

NTSTATUS QHIrpDispatchRead(_In_ PDEVICE_OBJECT DeviceObject, _Inout_ PIRP Irp)
{
	PQH_DEVICE_EXTENSION deviceExt =
		(PQH_DEVICE_EXTENSION)DeviceObject->DeviceExtension;
	PQH_DRIVER_EXTENSION driverExt =
		IoGetDriverObjectExtension(g_DriverObject, &g_DriverObject);
	PIO_STACK_LOCATION irpSp;
	UINT64 offset;
	ULONG length;
	PQH_RECOVERY_READ_ITEM item;
	KIRQL oldIrql;

	if (!deviceExt || !deviceExt->LowerDeviceObject)
		return QHCompleteIrp(Irp, STATUS_INVALID_DEVICE_REQUEST, 0);

	if (InterlockedCompareExchange(&deviceExt->Phase, 0, 0) !=
		(LONG)QH_PHASE_RECOVERY)
	{
		return QHSendToNextDevice(deviceExt->LowerDeviceObject, Irp);
	}

	irpSp = IoGetCurrentIrpStackLocation(Irp);
	if (irpSp->Parameters.Read.ByteOffset.QuadPart < 0 ||
		irpSp->Parameters.Read.Length == 0)
	{
		return QHSendToNextDevice(deviceExt->LowerDeviceObject, Irp);
	}

	offset = (UINT64)irpSp->Parameters.Read.ByteOffset.QuadPart;
	length = irpSp->Parameters.Read.Length;
	if (!driverExt ||
		(deviceExt->SectorSize != 512 && deviceExt->SectorSize != 4096) ||
		(offset % deviceExt->SectorSize) != 0 ||
		(length % deviceExt->SectorSize) != 0)
	{
		return QHSendToNextDevice(deviceExt->LowerDeviceObject, Irp);
	}

	item = (PQH_RECOVERY_READ_ITEM)qhalloc(sizeof(*item));
	if (!item)
		return QHCompleteIrp(Irp, STATUS_INSUFFICIENT_RESOURCES, 0);
	item->Irp = Irp;

	KeAcquireSpinLock(&deviceExt->RecoveryReadQueueLock, &oldIrql);
	if (InterlockedCompareExchange(&deviceExt->RecoveryReadStopping, 0, 0) == 0 &&
		InterlockedCompareExchange(&deviceExt->Phase, 0, 0) ==
			(LONG)QH_PHASE_RECOVERY)
	{
		IoMarkIrpPending(Irp);
		InsertTailList(&deviceExt->RecoveryReadQueue, &item->Entry);
		KeSetEvent(&deviceExt->RecoveryReadEvent, IO_NO_INCREMENT, FALSE);
		KeReleaseSpinLock(&deviceExt->RecoveryReadQueueLock, oldIrql);
		QH_DBG(
			"[RECOVERY] read queued irp=%p offset=%llu len=%lu "
			"flags=0x%08lX pagingIo=%lu irql=%lu thread=%p\n",
			Irp,
			offset,
			length,
			Irp->Flags,
			(Irp->Flags & IRP_PAGING_IO) != 0 ? 1UL : 0UL,
			(ULONG)KeGetCurrentIrql(),
			PsGetCurrentThreadId());
		return STATUS_PENDING;
	}
	KeReleaseSpinLock(&deviceExt->RecoveryReadQueueLock, oldIrql);
	qhfree(item);
	return QHSendToNextDevice(deviceExt->LowerDeviceObject, Irp);
}

static VOID QHForwardQueuedRead(
	_In_ PQH_DEVICE_EXTENSION DevExt,
	_Inout_ PIRP Irp)
{
	IoSkipCurrentIrpStackLocation(Irp);
	(void)IoCallDriver(DevExt->LowerDeviceObject, Irp);
}

static VOID QHRecoveryReadWorker(_In_ PVOID Context)
{
	PQH_DEVICE_EXTENSION devExt = (PQH_DEVICE_EXTENSION)Context;

	for (;;)
	{
		PLIST_ENTRY entry = NULL;
		KIRQL oldIrql;

		KeWaitForSingleObject(
			&devExt->RecoveryReadEvent,
			Executive,
			KernelMode,
			FALSE,
			NULL);
		for (;;)
		{
			KeAcquireSpinLock(&devExt->RecoveryReadQueueLock, &oldIrql);
			if (!IsListEmpty(&devExt->RecoveryReadQueue))
				entry = RemoveHeadList(&devExt->RecoveryReadQueue);
			else
			{
				KeClearEvent(&devExt->RecoveryReadEvent);
				entry = NULL;
			}
			KeReleaseSpinLock(&devExt->RecoveryReadQueueLock, oldIrql);
			if (!entry)
				break;

			{
				PQH_RECOVERY_READ_ITEM item = CONTAINING_RECORD(
					entry,
					QH_RECOVERY_READ_ITEM,
					Entry);
				PIRP irp = item->Irp;
				PIO_STACK_LOCATION irpSp = IoGetCurrentIrpStackLocation(irp);
				UINT64 offset = (UINT64)irpSp->Parameters.Read.ByteOffset.QuadPart;
				ULONG length = irpSp->Parameters.Read.Length;

				if (InterlockedCompareExchange(
						&devExt->RecoveryReadStopping, 0, 0) != 0 ||
					InterlockedCompareExchange(&devExt->Phase, 0, 0) !=
						(LONG)QH_PHASE_RECOVERY ||
					!devExt->Core)
				{
					QH_DBG("[RECOVERY] queued read forwarded irp=%p\n", irp);
					QHForwardQueuedRead(devExt, irp);
				}
				else
				{
					PVOID buffer = MmGetSystemAddressForMdlSafe(
						irp->MdlAddress,
						NormalPagePriority);
					NTSTATUS status = buffer ?
						QHRecoveryFillReadBuffer(
							devExt,
							offset,
							length,
							buffer) :
						STATUS_INSUFFICIENT_RESOURCES;

					QH_DBG(
						"[RECOVERY] worker read end irp=%p offset=%llu "
						"len=%lu status=0x%08X pagingIo=%lu "
						"irql=%lu thread=%p\n",
						irp,
						offset,
						length,
						status,
						(irp->Flags & IRP_PAGING_IO) != 0 ? 1UL : 0UL,
						(ULONG)KeGetCurrentIrql(),
						PsGetCurrentThreadId());
					QHCompleteIrp(
						irp,
						status,
						NT_SUCCESS(status) ? length : 0);
				}
				qhfree(item);
			}
		}
		if (InterlockedCompareExchange(
				&devExt->RecoveryReadStopping, 0, 0) != 0)
		{
			break;
		}
	}
	PsTerminateSystemThread(STATUS_SUCCESS);
}

NTSTATUS QHStartRecoveryReadWorker(_Inout_ PQH_DEVICE_EXTENSION DevExt)
{
	InterlockedExchange(&DevExt->RecoveryReadStopping, 0);
	return PsCreateSystemThread(
		&DevExt->RecoveryReadThreadHandle,
		THREAD_ALL_ACCESS,
		NULL,
		NULL,
		NULL,
		QHRecoveryReadWorker,
		DevExt);
}

VOID QHStopRecoveryReadWorker(_Inout_ PQH_DEVICE_EXTENSION DevExt)
{
	HANDLE threadHandle = DevExt->RecoveryReadThreadHandle;
	PVOID threadObject = NULL;

	if (!threadHandle)
		return;
	InterlockedExchange(&DevExt->RecoveryReadStopping, 1);
	KeSetEvent(&DevExt->RecoveryReadEvent, IO_NO_INCREMENT, FALSE);

	if (NT_SUCCESS(ObReferenceObjectByHandle(
		threadHandle,
		THREAD_ALL_ACCESS,
		*PsThreadType,
		KernelMode,
		&threadObject,
		NULL)))
	{
		KeWaitForSingleObject(threadObject, Executive, KernelMode, FALSE, NULL);
		ObDereferenceObject(threadObject);
	}

	ZwClose(threadHandle);
	DevExt->RecoveryReadThreadHandle = NULL;
}

static NTSTATUS QHCaptureBeforeImage(
	_In_ PQH_DEVICE_EXTENSION SourceExt,
	_In_ PIRP Irp,
	_In_ PQH_DRIVER_EXTENSION DriverExt)
{
	PIO_STACK_LOCATION irpSp = IoGetCurrentIrpStackLocation(Irp);
	UINT64 offset = (UINT64)irpSp->Parameters.Write.ByteOffset.QuadPart;
	ULONG remaining = irpSp->Parameters.Write.Length;
	NTSTATUS status = STATUS_SUCCESS;
	QH_JOURNAL_RECORD_HEADER writtenHdr;
	BOOLEAN seqLogged = FALSE;

	UNREFERENCED_PARAMETER(DriverExt);

	if (irpSp->Parameters.Write.ByteOffset.QuadPart < 0 || remaining == 0)
		return STATUS_INVALID_PARAMETER;
	if (!SourceExt->Core)
		return STATUS_DEVICE_NOT_READY;

	QH_DBG("[COW] capture begin offset=%llu len=%lu\n", offset, remaining);

	while (remaining)
	{
		ULONG chunk = remaining > QH_JOURNAL_MAX_RECORD_DATA ?
			QH_JOURNAL_MAX_RECORD_DATA : remaining;

		status = QhCoreCaptureAppend(SourceExt->Core, offset, chunk, &writtenHdr);
		if (!NT_SUCCESS(status))
		{
			QH_LOG("[COW] core capture failed status=0x%08X offset=%llu len=%lu\n",
				status, offset, chunk);
			break;
		}
		if (!seqLogged)
		{
			// Print journal record Sequence once per before-image capture.
			QH_DBG("[COW] journal seq=%lu offset=%llu len=%lu\n",
				writtenHdr.Sequence, offset, chunk);
			seqLogged = TRUE;
		}
		QH_DBG("[COW] core capture ok offset=%llu len=%lu\n", offset, chunk);
		offset += chunk;
		remaining -= chunk;
	}

	QH_DBG("[COW] capture end status=0x%08X remaining=%lu\n",
		status, remaining);
	return status;
}

static NTSTATUS QHForwardWriteCompletion(
	_In_ PDEVICE_OBJECT DeviceObject,
	_In_ PIRP Irp,
	_In_ PVOID Context)
{
	UNREFERENCED_PARAMETER(DeviceObject);
	UNREFERENCED_PARAMETER(Irp);
	KeSetEvent((PKEVENT)Context, IO_NO_INCREMENT, FALSE);
	return STATUS_MORE_PROCESSING_REQUIRED;
}

static VOID QHForwardQueuedWriteSynchronously(
	_In_ PQH_DEVICE_EXTENSION DevExt,
	_Inout_ PIRP Irp)
{
	KEVENT event;

	KeInitializeEvent(&event, NotificationEvent, FALSE);
	IoCopyCurrentIrpStackLocationToNext(Irp);
	IoSetCompletionRoutine(
		Irp,
		QHForwardWriteCompletion,
		&event,
		TRUE,
		TRUE,
		TRUE);
	(void)IoCallDriver(DevExt->LowerDeviceObject, Irp);
	KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, NULL);
	QH_DBG("[COW] original write completing irp=%p status=0x%08X bytes=%Iu\n",
		Irp,
		Irp->IoStatus.Status,
		Irp->IoStatus.Information);
	IoCompleteRequest(Irp, IO_NO_INCREMENT);
}

static VOID QHCaptureWorker(_In_ PVOID Context)
{
	PQH_DEVICE_EXTENSION devExt = (PQH_DEVICE_EXTENSION)Context;
	PQH_DRIVER_EXTENSION driverExt =
		IoGetDriverObjectExtension(g_DriverObject, &g_DriverObject);

	for (;;)
	{
		PLIST_ENTRY entry = NULL;
		KIRQL oldIrql;

		KeWaitForSingleObject(
			&devExt->CaptureEvent,
			Executive,
			KernelMode,
			FALSE,
			NULL);
		for (;;)
		{
			KeAcquireSpinLock(&devExt->CaptureQueueLock, &oldIrql);
			if (!IsListEmpty(&devExt->CaptureQueue))
				entry = RemoveHeadList(&devExt->CaptureQueue);
			else
			{
				KeClearEvent(&devExt->CaptureEvent);
				entry = NULL;
			}
			KeReleaseSpinLock(&devExt->CaptureQueueLock, oldIrql);
			if (!entry)
				break;

			{
				PQH_CAPTURE_ITEM item =
					CONTAINING_RECORD(entry, QH_CAPTURE_ITEM, Entry);

				// Capture before-image then apply the original write under one
				// HistoryMutex so preview cannot observe a torn timeline.
				QH_DBG("[COW] worker mutex wait irp=%p\n", item->Irp);
				KeWaitForSingleObject(
					&devExt->HistoryMutex,
					Executive,
					KernelMode,
					FALSE,
					NULL);
				QH_DBG("[COW] worker mutex acquired irp=%p\n", item->Irp);
				if (!devExt->CaptureStopping &&
					InterlockedCompareExchange(&devExt->CaptureEnabled, 0, 0) != 0 &&
					driverExt)
				{
					NTSTATUS captureStatus =
						QHCaptureBeforeImage(devExt, item->Irp, driverExt);
					QH_DBG("[COW] worker capture status=0x%08X irp=%p\n",
						captureStatus, item->Irp);
					UNREFERENCED_PARAMETER(captureStatus);
				}
				else
				{
					QH_DBG("[COW] worker forwarding without capture irp=%p\n",
						item->Irp);
				}
				QHForwardQueuedWriteSynchronously(devExt, item->Irp);
				KeReleaseMutex(&devExt->HistoryMutex, FALSE);
				QH_DBG("[COW] worker mutex released irp=%p\n", item->Irp);
				qhfree(item);
			}
		}
		if (InterlockedCompareExchange(&devExt->CaptureStopping, 0, 0) != 0)
			break;
	}
	PsTerminateSystemThread(STATUS_SUCCESS);
}

NTSTATUS QHStartCaptureWorker(_Inout_ PQH_DEVICE_EXTENSION DevExt)
{
	InterlockedExchange(&DevExt->CaptureStopping, 0);
	return PsCreateSystemThread(
		&DevExt->CaptureThreadHandle,
		THREAD_ALL_ACCESS,
		NULL,
		NULL,
		NULL,
		QHCaptureWorker,
		DevExt);
}

VOID QHStopCaptureWorker(_Inout_ PQH_DEVICE_EXTENSION DevExt)
{
	HANDLE threadHandle = DevExt->CaptureThreadHandle;
	PVOID threadObject = NULL;

	if (!threadHandle)
		return;
	InterlockedExchange(&DevExt->CaptureStopping, 1);
	KeSetEvent(&DevExt->CaptureEvent, IO_NO_INCREMENT, FALSE);

	if (NT_SUCCESS(ObReferenceObjectByHandle(threadHandle, THREAD_ALL_ACCESS,
		*PsThreadType, KernelMode, &threadObject, NULL)))
	{
		KeWaitForSingleObject(threadObject, Executive, KernelMode, FALSE, NULL);
		ObDereferenceObject(threadObject);
	}

	ZwClose(threadHandle);
	DevExt->CaptureThreadHandle = NULL;
}

VOID QHDisableAndDestroyCapture(_Inout_ PQH_DEVICE_EXTENSION DevExt)
{
	PQH_CORE core;

	if (!DevExt)
		return;

	InterlockedExchange(&DevExt->CaptureEnabled, 0);
	InterlockedExchange(&DevExt->Phase, (LONG)QH_PHASE_GENERAL);
	QHStopRecoveryReadWorker(DevExt);
	QHStopCaptureWorker(DevExt);

	// The capture worker holds HistoryMutex whenever it can access Core.  Take
	// ownership under that mutex only after the worker has stopped, then free
	// Core outside every spin lock and outside the mutex.
	KeWaitForSingleObject(&DevExt->HistoryMutex,
		Executive, KernelMode, FALSE, NULL);
	core = DevExt->Core;
	DevExt->Core = NULL;
	KeReleaseMutex(&DevExt->HistoryMutex, FALSE);

	if (core)
		QhCoreDestroy(core);
}

NTSTATUS QHIrpDispatchWrite(_In_ PDEVICE_OBJECT DeviceObject, _Inout_ PIRP Irp)
{
	PQH_DEVICE_EXTENSION deviceExt = (PQH_DEVICE_EXTENSION)DeviceObject->DeviceExtension;
	PQH_DRIVER_EXTENSION driverExt = IoGetDriverObjectExtension(g_DriverObject, &g_DriverObject);

	if (deviceExt && deviceExt->LowerDeviceObject && driverExt)
		QHWaitForAutoDiscoveryIfNeeded(driverExt);

	if (deviceExt && deviceExt->LowerDeviceObject && driverExt &&
		InterlockedCompareExchange(&deviceExt->CaptureEnabled, 0, 0) != 0 &&
		InterlockedCompareExchange(&deviceExt->CaptureStopping, 0, 0) == 0)
	{
		PQH_CAPTURE_ITEM item = (PQH_CAPTURE_ITEM)qhalloc(sizeof(*item));
		KIRQL oldIrql;
		if (item)
		{
			item->Irp = Irp;
			KeAcquireSpinLock(&deviceExt->CaptureQueueLock, &oldIrql);
			if (InterlockedCompareExchange(&deviceExt->CaptureStopping, 0, 0) == 0 &&
				InterlockedCompareExchange(&deviceExt->CaptureEnabled, 0, 0) != 0)
			{
				IoMarkIrpPending(Irp);
				InsertTailList(&deviceExt->CaptureQueue, &item->Entry);
				KeSetEvent(&deviceExt->CaptureEvent, IO_NO_INCREMENT, FALSE);
				KeReleaseSpinLock(&deviceExt->CaptureQueueLock, oldIrql);
#if DBG
				{
					PIO_STACK_LOCATION irpSp = IoGetCurrentIrpStackLocation(Irp);
					QH_DBG("[COW] write queued irp=%p offset=%lld len=%lu\n",
						Irp,
						irpSp->Parameters.Write.ByteOffset.QuadPart,
						irpSp->Parameters.Write.Length);
				}
#endif
				return STATUS_PENDING;
			}
			KeReleaseSpinLock(&deviceExt->CaptureQueueLock, oldIrql);
			qhfree(item);
		}
		else
		{
			QH_LOG("[COW] queue allocation failed; write passed through irp=%p\n",
				Irp);
		}
	}

	if (deviceExt && deviceExt->LowerDeviceObject)
		return QHSendToNextDevice(deviceExt->LowerDeviceObject, Irp);

	return QHCompleteIrp(Irp, STATUS_INVALID_DEVICE_REQUEST, 0);
}

static NTSTATUS PnpCompletionRoutine(PDEVICE_OBJECT DeviceObject, PIRP Irp, PVOID Context)
{
	UNREFERENCED_PARAMETER(DeviceObject);
	UNREFERENCED_PARAMETER(Irp);
	KeSetEvent((PKEVENT)Context, IO_NO_INCREMENT, FALSE);
	return STATUS_MORE_PROCESSING_REQUIRED;
}

static NTSTATUS QHStartDeviceCompletionRoutine(
	_In_ PDEVICE_OBJECT DeviceObject,
	_In_ PIRP Irp,
	_In_ PVOID Context)
{
	PQH_DEVICE_EXTENSION devExt = (PQH_DEVICE_EXTENSION)Context;
	PQH_DRIVER_EXTENSION driverExt;

	if (devExt && NT_SUCCESS(Irp->IoStatus.Status))
	{
		InterlockedExchange(&devExt->Started, 1);
		InterlockedExchange(&devExt->AutoKind, 0);
		devExt->VolumeGuidValid = FALSE;
#if DBG
		QH_DBG("[AUTO-CDP] volume started filter=%p lower=%p\n",
			DeviceObject,
			devExt->LowerDeviceObject);
#else
		UNREFERENCED_PARAMETER(DeviceObject);
#endif
		driverExt = IoGetDriverObjectExtension(g_DriverObject, &g_DriverObject);
		// Completion may run at DISPATCH_LEVEL: no FastMutex / waits here.
		if (driverExt &&
			driverExt->CaptureTargetHandleId == 0 &&
			InterlockedCompareExchange(
				&driverExt->AutoDiscoverySuppressed, 0, 0) == 0)
		{
			if (InterlockedCompareExchange(
				&driverExt->AutoDiscoverySettled, 0, 1) == 1)
			{
				KeClearEvent(&driverExt->AutoDiscoverySettledEvent);
			}
			QHQueueAutoDiscovery(driverExt);
		}
	}
	return STATUS_CONTINUE_COMPLETION;
}

NTSTATUS QHIrpDispatchPnp(_In_ PDEVICE_OBJECT DeviceObject, _Inout_ PIRP Irp)
{
	PQH_DEVICE_EXTENSION DevExt = DeviceObject->DeviceExtension;
	PIO_STACK_LOCATION IrpSp = IoGetCurrentIrpStackLocation(Irp);
	PQH_DRIVER_EXTENSION DriverExt = NULL;

	if (!DevExt)
		return QHIrpDispatchDefault(DeviceObject, Irp);

	DriverExt = IoGetDriverObjectExtension(g_DriverObject, &g_DriverObject);
	if (!DriverExt)
		return QHSendToNextDevice(DevExt->LowerDeviceObject, Irp);

	switch (IrpSp->MinorFunction)
	{
	case IRP_MN_START_DEVICE:
		IoCopyCurrentIrpStackLocationToNext(Irp);
		IoSetCompletionRoutine(
			Irp,
			QHStartDeviceCompletionRoutine,
			DevExt,
			TRUE,
			TRUE,
			TRUE);
		return IoCallDriver(DevExt->LowerDeviceObject, Irp);

	case IRP_MN_REMOVE_DEVICE:
	{
		KIRQL OldIrql;
		PQH_DEVICE_LIST_NODE NodeToFree = NULL;
		PDEVICE_OBJECT LowerDevice = NULL;
		NTSTATUS Status;

		KeAcquireSpinLock(&DriverExt->DeviceObjectListLock, &OldIrql);
		PLIST_ENTRY PEntry = DriverExt->DeviceObjectListHead.Flink;
		while (PEntry != &DriverExt->DeviceObjectListHead)
		{
			PQH_DEVICE_LIST_NODE Node = CONTAINING_RECORD(PEntry, QH_DEVICE_LIST_NODE, Entry);
			if (Node->DeviceObject == DeviceObject)
			{
				RemoveEntryList(&Node->Entry);
				NodeToFree = Node;
				break;
			}
			PEntry = PEntry->Flink;
		}
		KeReleaseSpinLock(&DriverExt->DeviceObjectListLock, OldIrql);

		if (NodeToFree)
			qhfree(NodeToFree);

		InterlockedExchange(&DevExt->Started, 0);
		// No new probe can acquire rundown after the node has left the list.
		// Wait for a probe already using LowerDeviceObject before detaching it.
		ExWaitForRundownProtectionRelease(&DevExt->AutoDiscoveryRundown);
		QHDisableAndDestroyCapture(DevExt);
		LowerDevice = DevExt->LowerDeviceObject;

		IoSkipCurrentIrpStackLocation(Irp);
		Status = IoCallDriver(LowerDevice, Irp);

		if (LowerDevice)
		{
			IoDetachDevice(LowerDevice);
			DevExt->LowerDeviceObject = NULL;
		}

		IoDeleteDevice(DeviceObject);
		return Status;
	}

	case IRP_MN_DEVICE_USAGE_NOTIFICATION:
	{
		if (IrpSp->Parameters.UsageNotification.Type != DeviceUsageTypePaging)
			return QHSendToNextDevice(DevExt->LowerDeviceObject, Irp);
		if (IrpSp->Parameters.UsageNotification.InPath &&
			InterlockedCompareExchange(&DevExt->Phase, 0, 0) ==
				(LONG)QH_PHASE_RECOVERY)
		{
			QH_LOG("[RECOVERY] paging path activation rejected while active\n");
			return QHCompleteIrp(Irp, STATUS_DEVICE_BUSY, 0);
		}

		BOOLEAN SetPagable = FALSE;
		if (!IrpSp->Parameters.UsageNotification.InPath &&
			DevExt->PagingPathCount == 1)
		{
			if (!(DeviceObject->Flags & DO_POWER_INRUSH))
			{
				DeviceObject->Flags |= DO_POWER_PAGABLE;
				SetPagable = TRUE;
			}
		}

		KEVENT Event;
		KeInitializeEvent(&Event, NotificationEvent, FALSE);
		IoCopyCurrentIrpStackLocationToNext(Irp);
		IoSetCompletionRoutine(Irp, PnpCompletionRoutine, &Event, TRUE, TRUE, TRUE);
		NTSTATUS Status = IoCallDriver(DevExt->LowerDeviceObject, Irp);
		if (Status == STATUS_PENDING)
		{
			KeWaitForSingleObject(&Event, Executive, KernelMode, FALSE, NULL);
			Status = Irp->IoStatus.Status;
		}

		if (NT_SUCCESS(Status))
		{
			IoAdjustPagingPathCount(&DevExt->PagingPathCount,
				IrpSp->Parameters.UsageNotification.InPath);
			if (IrpSp->Parameters.UsageNotification.InPath &&
				DevExt->PagingPathCount == 1)
			{
				DeviceObject->Flags &= ~DO_POWER_PAGABLE;
			}
		}
		else if (SetPagable)
		{
			DeviceObject->Flags &= ~DO_POWER_PAGABLE;
		}

		return QHCompleteIrp(Irp, Status, Irp->IoStatus.Information);
	}

	default:
		break;
	}

	return QHSendToNextDevice(DevExt->LowerDeviceObject, Irp);
}

NTSTATUS QHIrpDispatchPower(_In_ PDEVICE_OBJECT DeviceObject, _Inout_ PIRP Irp)
{
	PQH_DEVICE_EXTENSION DevExt = DeviceObject->DeviceExtension;
	if (!DevExt)
		return QHIrpDispatchDefault(DeviceObject, Irp);

#if (NTDDI_VERSION < NTDDI_VISTA)
	PoStartNextPowerIrp(Irp);
	IoSkipCurrentIrpStackLocation(Irp);
	return PoCallDriver(DevExt->LowerDeviceObject, Irp);
#else
	return QHSendToNextDevice(DevExt->LowerDeviceObject, Irp);
#endif
}

NTSTATUS QHIrpDispatchDeviceControl(_In_ PDEVICE_OBJECT DeviceObject, _Inout_ PIRP Irp)
{
	PQH_DEVICE_EXTENSION DevExt = (PQH_DEVICE_EXTENSION)DeviceObject->DeviceExtension;
	PIO_STACK_LOCATION IrpSp = IoGetCurrentIrpStackLocation(Irp);
	PQH_DRIVER_EXTENSION DriverExt = IoGetDriverObjectExtension(g_DriverObject, &g_DriverObject);
	BOOLEAN isControlDevice = (DriverExt != NULL && DeviceObject == DriverExt->ControlDevice);

	// ?????��?????????????????????? IOCTL
	if (isControlDevice)
	{
		switch (IrpSp->Parameters.DeviceIoControl.IoControlCode)
		{
		case IOCTL_QH_QUERY_PROTECT_STATUS:
		{
			if (IrpSp->Parameters.DeviceIoControl.OutputBufferLength < sizeof(BOOLEAN))
				return QHCompleteIrp(Irp, STATUS_BUFFER_TOO_SMALL, 0);

			BOOLEAN isProtecting = FALSE;

			if (DriverExt)
			{
				KIRQL OldIrql;
				KeAcquireSpinLock(&DriverExt->DeviceObjectListLock, &OldIrql);
				PLIST_ENTRY Entry = DriverExt->DeviceObjectListHead.Flink;
				while (Entry != &DriverExt->DeviceObjectListHead)
				{
					PQH_DEVICE_LIST_NODE Node = CONTAINING_RECORD(Entry, QH_DEVICE_LIST_NODE, Entry);
					PQH_DEVICE_EXTENSION VolExt = (PQH_DEVICE_EXTENSION)Node->DeviceObject->DeviceExtension;
					if (InterlockedCompareExchange(&VolExt->CaptureEnabled, 0, 0) != 0)
					{
						isProtecting = TRUE;
						break;
					}
					Entry = Entry->Flink;
				}
				KeReleaseSpinLock(&DriverExt->DeviceObjectListLock, OldIrql);
			}

			*(PBOOLEAN)Irp->AssociatedIrp.SystemBuffer = isProtecting;
			return QHCompleteIrp(Irp, STATUS_SUCCESS, sizeof(BOOLEAN));
		}

		case IOCTL_QH_SEND_COMMAND:
		{
			PULONG pCode;
			PQH_COMMAND_REPLY reply;
			ULONG outLen = IrpSp->Parameters.DeviceIoControl.OutputBufferLength;
			ULONG inLen = IrpSp->Parameters.DeviceIoControl.InputBufferLength;

			if (inLen < sizeof(ULONG) ||
				outLen < sizeof(QH_COMMAND_REPLY) ||
				!Irp->AssociatedIrp.SystemBuffer)
			{
				return QHCompleteIrp(Irp, STATUS_BUFFER_TOO_SMALL, 0);
			}

			pCode = (PULONG)Irp->AssociatedIrp.SystemBuffer;

			switch (*pCode)
			{
			case QH_CMD_1:
			{
				QH_CMD1_REQUEST local;
				UINT64 handleId = 0;
				NTSTATUS Status;

				if (inLen < sizeof(QH_CMD1_REQUEST))
					return QHCompleteIrp(Irp, STATUS_BUFFER_TOO_SMALL, 0);

				local = *(PQH_CMD1_REQUEST)Irp->AssociatedIrp.SystemBuffer;
				reply = (PQH_COMMAND_REPLY)Irp->AssociatedIrp.SystemBuffer;

				QH_DBG("CMD1 received\n");
				QH_LOG("version=%s journal=v%lu build=%s\n",
					QH_DRIVER_VERSION_STRING,
					QH_JOURNAL_VERSION,
					QH_DRIVER_BUILD_STRING);
				QHDbgGuid("  Guid1", &local.PartitionGuid1);
				QHDbgGuid("  Guid2", &local.PartitionGuid2);
				Status = QHConfigureCapture(
					DriverExt,
					&local.PartitionGuid1,
					&local.PartitionGuid2,
					local.FormatJournal != 0,
					&handleId);
				if (!NT_SUCCESS(Status))
				{
					QH_LOG("CMD1 configure failed status=0x%08X\n", Status);
					QHFillReply(reply, QH_CMD_1, (ULONG)Status, 0,
						L"ERROR: capture configuration failed");
					return QHCompleteIrp(Irp, Status, sizeof(QH_COMMAND_REPLY));
				}
				InterlockedExchange(&DriverExt->AutoDiscoverySuppressed, 0);
				QHFillReply(reply, QH_CMD_1, 0, handleId,
					L"OK: capture configured");
				return QHCompleteIrp(Irp, STATUS_SUCCESS, sizeof(QH_COMMAND_REPLY));
			}

			case QH_CMD_2:
			{
				QH_CMD2_REQUEST local;
				UINT64 handleId;
				NTSTATUS Status;

				if (inLen < sizeof(QH_CMD2_REQUEST))
					return QHCompleteIrp(Irp, STATUS_BUFFER_TOO_SMALL, 0);

				local = *(PQH_CMD2_REQUEST)Irp->AssociatedIrp.SystemBuffer;
				reply = (PQH_COMMAND_REPLY)Irp->AssociatedIrp.SystemBuffer;

				QH_DBG("CMD2 received\n");
				Status = KeWaitForSingleObject(
					&DriverExt->CaptureConfigMutex,
					Executive,
					KernelMode,
					FALSE,
					NULL);
				if (!NT_SUCCESS(Status))
				{
					QHFillReply(reply, QH_CMD_2, (ULONG)Status, 0,
						L"ERROR: stop capture failed");
					return QHCompleteIrp(Irp, Status, sizeof(QH_COMMAND_REPLY));
				}
				InterlockedExchange(&DriverExt->AutoDiscoverySuppressed, 1);
				QHMarkAutoDiscoverySettled(DriverExt);
				ExAcquireFastMutex(&DriverExt->VolumeHandleMutex);
				handleId = DriverExt->CaptureTargetHandleId;
				ExReleaseFastMutex(&DriverExt->VolumeHandleMutex);
				if (handleId == 0)
				{
					KeReleaseMutex(&DriverExt->CaptureConfigMutex, FALSE);
					QHFillReply(reply, QH_CMD_2, (ULONG)STATUS_NOT_FOUND, 0,
						L"ERROR: capture is not configured");
					return QHCompleteIrp(Irp, STATUS_NOT_FOUND, sizeof(QH_COMMAND_REPLY));
				}
				Status = QHCloseVolumeHandle(DriverExt, handleId);
				KeReleaseMutex(&DriverExt->CaptureConfigMutex, FALSE);
				if (!NT_SUCCESS(Status))
				{
					QH_LOG("CMD2 stop capture failed status=0x%08X\n", Status);
					QHFillReply(reply, QH_CMD_2, (ULONG)Status, 0,
						L"ERROR: stop capture failed");
					return QHCompleteIrp(Irp, Status, sizeof(QH_COMMAND_REPLY));
				}
				QH_LOG("capture stopped\n");
				QHFillReply(reply, QH_CMD_2, 0, 0, L"OK: capture stopped");
				return QHCompleteIrp(Irp, STATUS_SUCCESS, sizeof(QH_COMMAND_REPLY));
			}

			case QH_CMD_4:
			{
				QH_CMD4_REQUEST local;
				UINT64 handleId = 0;
				NTSTATUS Status;

				if (inLen < sizeof(QH_CMD4_REQUEST) || !DriverExt)
					return QHCompleteIrp(Irp, STATUS_BUFFER_TOO_SMALL, 0);

				local = *(PQH_CMD4_REQUEST)Irp->AssociatedIrp.SystemBuffer;
				reply = (PQH_COMMAND_REPLY)Irp->AssociatedIrp.SystemBuffer;

				Status = QHOpenVolumeHandle(DriverExt, &local.PartitionGuid, &handleId);
				if (!NT_SUCCESS(Status))
				{
					QHFillReply(reply, QH_CMD_4, (ULONG)Status, 0, L"ERROR: open volume failed");
					return QHCompleteIrp(Irp, Status, sizeof(QH_COMMAND_REPLY));
				}

				QHFillReply(reply, QH_CMD_4, 0, handleId, L"OK: volume opened");
				return QHCompleteIrp(Irp, STATUS_SUCCESS, sizeof(QH_COMMAND_REPLY));
			}

			case QH_CMD_5:
			{
				QH_CMD5_REQUEST local;
				NTSTATUS Status;

				if (inLen < sizeof(QH_CMD5_REQUEST) || !DriverExt)
					return QHCompleteIrp(Irp, STATUS_BUFFER_TOO_SMALL, 0);

				local = *(PQH_CMD5_REQUEST)Irp->AssociatedIrp.SystemBuffer;
				reply = (PQH_COMMAND_REPLY)Irp->AssociatedIrp.SystemBuffer;

				Status = QHCloseVolumeHandle(DriverExt, local.VolumeHandle);
				if (!NT_SUCCESS(Status))
				{
					QHFillReply(reply, QH_CMD_5, (ULONG)Status, 0, L"ERROR: close failed");
					return QHCompleteIrp(Irp, Status, sizeof(QH_COMMAND_REPLY));
				}

				QHFillReply(reply, QH_CMD_5, 0, 0, L"OK: volume closed");
				return QHCompleteIrp(Irp, STATUS_SUCCESS, sizeof(QH_COMMAND_REPLY));
			}

			default:
				reply = (PQH_COMMAND_REPLY)Irp->AssociatedIrp.SystemBuffer;
				QH_LOG("unknown command code %lu on SEND_COMMAND\n", *pCode);
				QHFillReply(reply, *pCode, (ULONG)STATUS_INVALID_PARAMETER, 0, L"ERROR: unknown command");
				return QHCompleteIrp(Irp, STATUS_INVALID_PARAMETER, sizeof(QH_COMMAND_REPLY));
			}
		}

		case IOCTL_QH_BEGIN_PREVIEW:
		{
			QH_PREVIEW_BEGIN_REQUEST request;
			PQH_PREVIEW_BEGIN_REPLY reply;
			ULONG inLen =
				IrpSp->Parameters.DeviceIoControl.InputBufferLength;
			ULONG outLen =
				IrpSp->Parameters.DeviceIoControl.OutputBufferLength;
			NTSTATUS status;

			if (!DriverExt || !Irp->AssociatedIrp.SystemBuffer ||
				inLen < sizeof(request) ||
				outLen < sizeof(QH_PREVIEW_BEGIN_REPLY))
			{
				return QHCompleteIrp(
					Irp,
					STATUS_BUFFER_TOO_SMALL,
					0);
			}

			request =
				*(PQH_PREVIEW_BEGIN_REQUEST)Irp->AssociatedIrp.SystemBuffer;
			reply =
				(PQH_PREVIEW_BEGIN_REPLY)Irp->AssociatedIrp.SystemBuffer;
			status = QHBeginPreviewSession(
				DriverExt,
				&request,
				reply);
			return QHCompleteIrp(
				Irp,
				status,
				NT_SUCCESS(status) ? sizeof(*reply) : 0);
		}

		case IOCTL_QH_READ_PREVIEW:
		{
			PQH_PREVIEW_READ_REQUEST request;
			PVOID outputBuffer;
			ULONG inLen =
				IrpSp->Parameters.DeviceIoControl.InputBufferLength;
			ULONG outLen =
				IrpSp->Parameters.DeviceIoControl.OutputBufferLength;
			NTSTATUS status;

			if (!DriverExt || !Irp->AssociatedIrp.SystemBuffer ||
				inLen < sizeof(QH_PREVIEW_READ_REQUEST))
			{
				return QHCompleteIrp(
					Irp,
					STATUS_BUFFER_TOO_SMALL,
					0);
			}
			request =
				(PQH_PREVIEW_READ_REQUEST)Irp->AssociatedIrp.SystemBuffer;
			if (!request->ByteLength ||
				outLen < request->ByteLength ||
				!Irp->MdlAddress)
			{
				return QHCompleteIrp(
					Irp,
					STATUS_BUFFER_TOO_SMALL,
					0);
			}
			outputBuffer = MmGetSystemAddressForMdlSafe(
				Irp->MdlAddress,
				NormalPagePriority);
			if (!outputBuffer)
			{
				return QHCompleteIrp(
					Irp,
					STATUS_INSUFFICIENT_RESOURCES,
					0);
			}

			status = QHReadPreviewSession(
				DriverExt,
				request,
				outputBuffer);
			return QHCompleteIrp(
				Irp,
				status,
				NT_SUCCESS(status) ? request->ByteLength : 0);
		}

		case IOCTL_QH_END_PREVIEW:
		{
			PQH_PREVIEW_END_REQUEST request;
			ULONG inLen =
				IrpSp->Parameters.DeviceIoControl.InputBufferLength;
			NTSTATUS status;

			if (!DriverExt || !Irp->AssociatedIrp.SystemBuffer ||
				inLen < sizeof(QH_PREVIEW_END_REQUEST))
			{
				return QHCompleteIrp(
					Irp,
					STATUS_BUFFER_TOO_SMALL,
					0);
			}
			request =
				(PQH_PREVIEW_END_REQUEST)Irp->AssociatedIrp.SystemBuffer;
			status = QHEndPreviewSession(
				DriverExt,
				request->PreviewHandle);
			return QHCompleteIrp(Irp, status, 0);
		}

		case IOCTL_QH_QUERY_PHASE:
		{
			QH_PHASE_QUERY_REQUEST request;
			PQH_PHASE_QUERY_REPLY reply;
			ULONG inLen =
				IrpSp->Parameters.DeviceIoControl.InputBufferLength;
			ULONG outLen =
				IrpSp->Parameters.DeviceIoControl.OutputBufferLength;
			NTSTATUS status;

			if (!DriverExt || !Irp->AssociatedIrp.SystemBuffer ||
				inLen < sizeof(QH_PHASE_QUERY_REQUEST) ||
				outLen < sizeof(QH_PHASE_QUERY_REPLY))
			{
				return QHCompleteIrp(
					Irp,
					STATUS_BUFFER_TOO_SMALL,
					0);
			}
			request =
				*(PQH_PHASE_QUERY_REQUEST)Irp->AssociatedIrp.SystemBuffer;
			reply = (PQH_PHASE_QUERY_REPLY)Irp->AssociatedIrp.SystemBuffer;
			status = QHQueryPhase(DriverExt, &request, reply);
			return QHCompleteIrp(
				Irp,
				status,
				NT_SUCCESS(status) ? sizeof(*reply) : 0);
		}

		case IOCTL_QH_BEGIN_RECOVERY:
		{
			QH_RECOVERY_BEGIN_REQUEST request;
			PQH_RECOVERY_BEGIN_REPLY reply;
			ULONG inLen =
				IrpSp->Parameters.DeviceIoControl.InputBufferLength;
			ULONG outLen =
				IrpSp->Parameters.DeviceIoControl.OutputBufferLength;
			NTSTATUS status;

			if (!DriverExt || !Irp->AssociatedIrp.SystemBuffer ||
				inLen < sizeof(QH_RECOVERY_BEGIN_REQUEST) ||
				outLen < sizeof(QH_RECOVERY_BEGIN_REPLY))
			{
				return QHCompleteIrp(
					Irp,
					STATUS_BUFFER_TOO_SMALL,
					0);
			}
			request =
				*(PQH_RECOVERY_BEGIN_REQUEST)Irp->AssociatedIrp.SystemBuffer;
			reply =
				(PQH_RECOVERY_BEGIN_REPLY)Irp->AssociatedIrp.SystemBuffer;
			status = QHBeginRecovery(DriverExt, &request, reply);
			return QHCompleteIrp(
				Irp,
				status,
				NT_SUCCESS(status) ? sizeof(*reply) : 0);
		}

		case IOCTL_QH_COMMIT_RECOVERY:
		{
			QH_RECOVERY_CONTROL_REQUEST request;
			PQH_RECOVERY_COMMIT_REPLY reply;
			ULONG inLen =
				IrpSp->Parameters.DeviceIoControl.InputBufferLength;
			ULONG outLen =
				IrpSp->Parameters.DeviceIoControl.OutputBufferLength;
			NTSTATUS status;

			if (!DriverExt || !Irp->AssociatedIrp.SystemBuffer ||
				inLen < sizeof(QH_RECOVERY_CONTROL_REQUEST) ||
				outLen < sizeof(QH_RECOVERY_COMMIT_REPLY))
			{
				return QHCompleteIrp(
					Irp,
					STATUS_BUFFER_TOO_SMALL,
					0);
			}
			request =
				*(PQH_RECOVERY_CONTROL_REQUEST)Irp->AssociatedIrp.SystemBuffer;
			reply =
				(PQH_RECOVERY_COMMIT_REPLY)Irp->AssociatedIrp.SystemBuffer;
			status = QHCommitRecovery(DriverExt, &request, reply);
			return QHCompleteIrp(
				Irp,
				status,
				NT_SUCCESS(status) ? sizeof(*reply) : 0);
		}

		case IOCTL_QH_CANCEL_RECOVERY:
		{
			QH_RECOVERY_CONTROL_REQUEST request;
			ULONG inLen =
				IrpSp->Parameters.DeviceIoControl.InputBufferLength;
			NTSTATUS status;

			if (!DriverExt || !Irp->AssociatedIrp.SystemBuffer ||
				inLen < sizeof(QH_RECOVERY_CONTROL_REQUEST))
			{
				return QHCompleteIrp(
					Irp,
					STATUS_BUFFER_TOO_SMALL,
					0);
			}
			request =
				*(PQH_RECOVERY_CONTROL_REQUEST)Irp->AssociatedIrp.SystemBuffer;
			status = QHCancelRecovery(DriverExt, &request);
			return QHCompleteIrp(Irp, status, 0);
		}

		case IOCTL_QH_QUERY_TIME_RANGE:
		{
			QH_TIME_RANGE_QUERY_REQUEST request;
			PQH_TIME_RANGE_QUERY_REPLY reply;
			ULONG inLen =
				IrpSp->Parameters.DeviceIoControl.InputBufferLength;
			ULONG outLen =
				IrpSp->Parameters.DeviceIoControl.OutputBufferLength;
			NTSTATUS status;

			if (!DriverExt || !Irp->AssociatedIrp.SystemBuffer ||
				inLen < sizeof(QH_TIME_RANGE_QUERY_REQUEST) ||
				outLen < sizeof(QH_TIME_RANGE_QUERY_REPLY))
			{
				return QHCompleteIrp(
					Irp,
					STATUS_BUFFER_TOO_SMALL,
					0);
			}
			request =
				*(PQH_TIME_RANGE_QUERY_REQUEST)Irp->AssociatedIrp.SystemBuffer;
			reply =
				(PQH_TIME_RANGE_QUERY_REPLY)Irp->AssociatedIrp.SystemBuffer;
			status = QHQueryTimeRange(DriverExt, &request, reply);
			return QHCompleteIrp(
				Irp,
				status,
				NT_SUCCESS(status) ? sizeof(*reply) : 0);
		}

		case IOCTL_QH_READ_SECTORS:
		{
			PQH_CMD3_REQUEST req;
			PVOID outBuf;
			ULONG outLen = IrpSp->Parameters.DeviceIoControl.OutputBufferLength;
			ULONG inLen = IrpSp->Parameters.DeviceIoControl.InputBufferLength;
			NTSTATUS Status;

			if (!DriverExt || inLen < sizeof(QH_CMD3_REQUEST) || !Irp->AssociatedIrp.SystemBuffer)
				return QHCompleteIrp(Irp, STATUS_BUFFER_TOO_SMALL, 0);

			req = (PQH_CMD3_REQUEST)Irp->AssociatedIrp.SystemBuffer;
			if (req->Code != QH_CMD_3)
				return QHCompleteIrp(Irp, STATUS_INVALID_PARAMETER, 0);

			if (outLen < req->ByteLength || req->ByteLength == 0)
				return QHCompleteIrp(Irp, STATUS_BUFFER_TOO_SMALL, 0);

			if (!Irp->MdlAddress)
				return QHCompleteIrp(Irp, STATUS_INVALID_PARAMETER, 0);

			outBuf = MmGetSystemAddressForMdlSafe(Irp->MdlAddress, NormalPagePriority);
			if (!outBuf)
				return QHCompleteIrp(Irp, STATUS_INSUFFICIENT_RESOURCES, 0);

			QH_DBG("CMD3 handle=%llu offset=%llu len=%lu\n",
				req->VolumeHandle, req->ByteOffset, req->ByteLength);

			Status = QHReadVolumeByHandle(
				DriverExt,
				req->VolumeHandle,
				req->ByteOffset,
				req->ByteLength,
				outBuf);

			if (!NT_SUCCESS(Status))
			{
				QH_LOG("CMD3 read failed 0x%08X\n", Status);
				return QHCompleteIrp(Irp, Status, 0);
			}

#if DBG
			{
				PUCHAR p = (PUCHAR)outBuf;
				QH_DBG("CMD3 read ok head=%02X %02X %02X %02X\n", p[0], p[1], p[2], p[3]);
			}
#endif
			return QHCompleteIrp(Irp, STATUS_SUCCESS, req->ByteLength);
		}

		default:
			QH_LOG("unknown IOCTL 0x%08X on control device\n",
				IrpSp->Parameters.DeviceIoControl.IoControlCode);
			return QHCompleteIrp(Irp, STATUS_INVALID_DEVICE_REQUEST, 0);
		}
	}

	if (!DevExt)
		return QHCompleteIrp(Irp, STATUS_INVALID_DEVICE_REQUEST, 0);

	return QHSendToNextDevice(DevExt->LowerDeviceObject, Irp);
}
