#include "CdpIrpDispatchs.h"
#include "..\CdpCore\include\cdp_core.h"
#include "CdpCredential.h"
#include "..\CdpCore\include\cdp_dev_store.h"
#include <ntdddisk.h>
#include <ntddstor.h>
#include <ntstrsafe.h>

static VOID CdpDisableAllCaptureSources(_In_ PCdp_DRIVER_EXTENSION DriverExt);

static NTSTATUS CdpBeginRecovery(
	_In_ PCdp_DRIVER_EXTENSION DriverExt,
	_In_ const Cdp_RECOVERY_BEGIN_REQUEST* Request,
	_Out_ PCdp_RECOVERY_BEGIN_REPLY Reply);

static NTSTATUS CdpCommitRecovery(
	_In_ PCdp_DRIVER_EXTENSION DriverExt,
	_In_ const Cdp_RECOVERY_CONTROL_REQUEST* Request,
	_Out_ PCdp_RECOVERY_COMMIT_REPLY Reply);

static NTSTATUS CdpCoreReadAlignedView(
	_In_ PCdp_DEVICE_EXTENSION DevExt,
	_In_ UINT64 Offset,
	_In_ ULONG Length,
	_Out_writes_bytes_(Length) PVOID Buffer);

static VOID CdpFillReply(
	_Out_ PCdp_COMMAND_REPLY Reply,
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

static VOID CdpDbgGuid(_In_ PCSTR Tag, _In_ const GUID* G)
{
#if DBG
	Cdp_DBG("%s {%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}\n",
		Tag,
		G->Data1, G->Data2, G->Data3,
		G->Data4[0], G->Data4[1], G->Data4[2], G->Data4[3],
		G->Data4[4], G->Data4[5], G->Data4[6], G->Data4[7]);
#else
	UNREFERENCED_PARAMETER(Tag);
	UNREFERENCED_PARAMETER(G);
#endif
}

static NTSTATUS CdpFormatVolumeNtPath(
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

static NTSTATUS CdpQueryVolumeGeometry(
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

static NTSTATUS CdpSendDeviceControlSynchronously(
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

static NTSTATUS CdpQueryDeviceGeometry(
	_In_ PDEVICE_OBJECT Device,
	_Out_ PUINT64 PartitionSize,
	_Out_ PULONG SectorSize)
{
	GET_LENGTH_INFORMATION lengthInfo;
	DISK_GEOMETRY geometry;
	NTSTATUS status;

	RtlZeroMemory(&lengthInfo, sizeof(lengthInfo));
	RtlZeroMemory(&geometry, sizeof(geometry));
	status = CdpSendDeviceControlSynchronously(
		Device,
		IOCTL_DISK_GET_LENGTH_INFO,
		&lengthInfo,
		sizeof(lengthInfo));
	if (!NT_SUCCESS(status))
		return status;
	status = CdpSendDeviceControlSynchronously(
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

static PCdp_VOLUME_HANDLE_ENTRY CdpLookupVolumeHandleLocked(
	_In_ PCdp_DRIVER_EXTENSION DriverExt,
	_In_ UINT64 HandleId)
{
	PLIST_ENTRY entry = DriverExt->VolumeHandleList.Flink;
	while (entry != &DriverExt->VolumeHandleList)
	{
		PCdp_VOLUME_HANDLE_ENTRY item = CONTAINING_RECORD(entry, Cdp_VOLUME_HANDLE_ENTRY, Entry);
		if (item->HandleId == HandleId)
			return item;
		entry = entry->Flink;
	}
	return NULL;
}

static NTSTATUS CdpGetSharedCredential(
	_In_ PCdp_DRIVER_EXTENSION DriverExt,
	_Out_ PCdp_CREDENTIAL_DESCRIPTOR Credential,
	_Out_opt_ PULONG JournalCount)
{
	PLIST_ENTRY entry;
	BOOLEAN found = FALSE;
	ULONG count = 0;
	NTSTATUS status = STATUS_NOT_FOUND;

	RtlZeroMemory(Credential, sizeof(*Credential));
	ExAcquireFastMutex(&DriverExt->VolumeHandleMutex);
	for (entry = DriverExt->VolumeHandleList.Flink;
		entry != &DriverExt->VolumeHandleList; entry = entry->Flink)
	{
		PCdp_VOLUME_HANDLE_ENTRY item =
			CONTAINING_RECORD(entry, Cdp_VOLUME_HANDLE_ENTRY, Entry);
		Cdp_CREDENTIAL_DESCRIPTOR current;
		if (item->Closing || !item->Journal.Mounted ||
			!CdpJournalGetCredential(&item->Journal, &current))
		{
			continue;
		}
		if (!found)
		{
			*Credential = current;
			found = TRUE;
			status = STATUS_SUCCESS;
		}
		else if (RtlCompareMemory(Credential, &current, sizeof(current)) !=
			sizeof(current))
		{
			status = STATUS_OBJECT_TYPE_MISMATCH;
			break;
		}
		++count;
	}
	ExReleaseFastMutex(&DriverExt->VolumeHandleMutex);
	if (JournalCount)
		*JournalCount = count;
	if (status == STATUS_NOT_FOUND &&
		InterlockedCompareExchange(&DriverExt->AutoDiscoverySettled, 0, 0) == 0)
	{
		return STATUS_DEVICE_NOT_READY;
	}
	return status;
}

// Find our filter's LowerDeviceObject for a volume PDO / stack member.
static PDEVICE_OBJECT CdpFindTargetLowerDevice(
	_In_ PCdp_DRIVER_EXTENSION DriverExt,
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
			PCdp_DEVICE_LIST_NODE node = CONTAINING_RECORD(entry, Cdp_DEVICE_LIST_NODE, Entry);
			PCdp_DEVICE_EXTENSION volExt = (PCdp_DEVICE_EXTENSION)node->DeviceObject->DeviceExtension;
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
			PCdp_DEVICE_EXTENSION volExt = (PCdp_DEVICE_EXTENSION)walk->DeviceExtension;
			if (volExt)
				return volExt->LowerDeviceObject;
		}
	}
	return NULL;
}

static NTSTATUS CdpResolveTargetLowerDevice(
	_In_ PCdp_DRIVER_EXTENSION DriverExt,
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

	*OutLowerDevice = CdpFindTargetLowerDevice(DriverExt, volumeDevice);
	ObDereferenceObject(fileObject);

	if (!*OutLowerDevice)
		return STATUS_DEVICE_DOES_NOT_EXIST;
	return STATUS_SUCCESS;
}

static PCdp_VOLUME_HANDLE_ENTRY CdpAcquireJournalForSource(
	_In_ PCdp_DRIVER_EXTENSION DriverExt,
	_In_ PCdp_DEVICE_EXTENSION SourceExt)
{
	PCdp_VOLUME_HANDLE_ENTRY item = NULL;
	UINT64 journalHandleId;

	if (!SourceExt)
		return NULL;
	journalHandleId = SourceExt->JournalHandleId;
	if (journalHandleId == 0)
		return NULL;

	ExAcquireFastMutex(&DriverExt->VolumeHandleMutex);
	item = CdpLookupVolumeHandleLocked(DriverExt, journalHandleId);
	if (item && !item->Closing)
		InterlockedIncrement(&item->ReferenceCount);
	else
		item = NULL;
	ExReleaseFastMutex(&DriverExt->VolumeHandleMutex);
	return item;
}

static PCdp_DEVICE_EXTENSION CdpFindSourceByJournalHandle(
	_In_ PCdp_DRIVER_EXTENSION DriverExt,
	_In_ UINT64 JournalHandleId)
{
	KIRQL oldIrql;
	PLIST_ENTRY entry;
	PCdp_DEVICE_EXTENSION found = NULL;

	if (JournalHandleId == 0)
		return NULL;

	KeAcquireSpinLock(&DriverExt->DeviceObjectListLock, &oldIrql);
	for (entry = DriverExt->DeviceObjectListHead.Flink;
		entry != &DriverExt->DeviceObjectListHead;
		entry = entry->Flink)
	{
		PCdp_DEVICE_LIST_NODE node =
			CONTAINING_RECORD(entry, Cdp_DEVICE_LIST_NODE, Entry);
		PCdp_DEVICE_EXTENSION ext =
			(PCdp_DEVICE_EXTENSION)node->DeviceObject->DeviceExtension;
		if (ext && ext->JournalHandleId == JournalHandleId)
		{
			found = ext;
			break;
		}
	}
	KeReleaseSpinLock(&DriverExt->DeviceObjectListLock, oldIrql);
	return found;
}

static VOID CdpReleaseVolumeHandleEntry(_In_ PCdp_VOLUME_HANDLE_ENTRY Item)
{
	if (InterlockedDecrement(&Item->ReferenceCount) == 0)
		KeSetEvent(&Item->NoReferences, IO_NO_INCREMENT, FALSE);
}

static VOID CdpCloseVolumeHandleEntry(_In_ PCdp_VOLUME_HANDLE_ENTRY Item)
{
	CdpReleaseVolumeHandleEntry(Item); // Drop the list ownership reference.
	KeWaitForSingleObject(&Item->NoReferences, Executive, KernelMode, FALSE, NULL);
	if (Item->Journal.Mounted)
		CdpJournalClose(&Item->Journal);
	if (Item->FileHandle)
		ZwClose(Item->FileHandle);
	cdpfree(Item);
}

VOID CdpCloseAllVolumeHandles(_In_ PCdp_DRIVER_EXTENSION DriverExt)
{
	CdpDisableAllCaptureSources(DriverExt);
	ExAcquireFastMutex(&DriverExt->VolumeHandleMutex);
	while (!IsListEmpty(&DriverExt->VolumeHandleList))
	{
		PLIST_ENTRY entry = RemoveHeadList(&DriverExt->VolumeHandleList);
		PCdp_VOLUME_HANDLE_ENTRY item = CONTAINING_RECORD(entry, Cdp_VOLUME_HANDLE_ENTRY, Entry);
		item->Closing = TRUE;
		CdpCloseVolumeHandleEntry(item);
	}
	ExReleaseFastMutex(&DriverExt->VolumeHandleMutex);
}

static NTSTATUS CdpOpenVolumeHandle(
	_In_ PCdp_DRIVER_EXTENSION DriverExt,
	_In_ const GUID* VolumeGuid,
	_Out_ PUINT64 OutHandleId)
{
	NTSTATUS Status;
	WCHAR path[96];
	UNICODE_STRING pathStr;
	OBJECT_ATTRIBUTES oa;
	IO_STATUS_BLOCK iosb;
	HANDLE fileHandle = NULL;
	PCdp_VOLUME_HANDLE_ENTRY item;

	*OutHandleId = 0;

	if (KeGetCurrentIrql() != PASSIVE_LEVEL)
		return STATUS_INVALID_DEVICE_STATE;

	Status = CdpFormatVolumeNtPath(VolumeGuid, path, sizeof(path));
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
		Cdp_LOG("open volume failed 0x%08X path=%ws\n", Status, path);
		return Status;
	}

	item = (PCdp_VOLUME_HANDLE_ENTRY)cdpalloc(sizeof(*item));
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

	Status = CdpResolveTargetLowerDevice(DriverExt, fileHandle, &item->TargetLowerDevice);
	if (!NT_SUCCESS(Status))
	{
		Cdp_LOG("resolve target lower device failed 0x%08X\n", Status);
		ZwClose(fileHandle);
		cdpfree(item);
		return Status;
	}
	Status = CdpQueryVolumeGeometry(fileHandle, &item->PartitionSize, &item->SectorSize);
	if (!NT_SUCCESS(Status))
	{
		Cdp_LOG("query volume geometry failed 0x%08X\n", Status);
		ZwClose(fileHandle);
		cdpfree(item);
		return Status;
	}

	ExAcquireFastMutex(&DriverExt->VolumeHandleMutex);
	InsertTailList(&DriverExt->VolumeHandleList, &item->Entry);
	ExReleaseFastMutex(&DriverExt->VolumeHandleMutex);

	*OutHandleId = item->HandleId;
	Cdp_DBG("opened volume handle id=%llu lower=%p\n",
		item->HandleId, item->TargetLowerDevice);
	CdpDbgGuid("  Guid", VolumeGuid);
	return STATUS_SUCCESS;
}

static NTSTATUS CdpCloseVolumeHandle(
	_In_ PCdp_DRIVER_EXTENSION DriverExt,
	_In_ UINT64 HandleId)
{
	PCdp_VOLUME_HANDLE_ENTRY item;
	PCdp_DEVICE_EXTENSION pairedSource = NULL;

	ExAcquireFastMutex(&DriverExt->VolumeHandleMutex);
	item = CdpLookupVolumeHandleLocked(DriverExt, HandleId);
	if (item)
	{
		RemoveEntryList(&item->Entry);
		item->Closing = TRUE;
	}
	ExReleaseFastMutex(&DriverExt->VolumeHandleMutex);

	if (!item)
		return STATUS_NOT_FOUND;

	pairedSource = CdpFindSourceByJournalHandle(DriverExt, HandleId);
	if (pairedSource)
	{
		pairedSource->JournalHandleId = 0;
		CdpDisableAndDestroyCapture(pairedSource);
		if (item->Journal.Mounted)
		{
			NTSTATUS invStatus = CdpJournalInvalidate(&item->Journal);
			Cdp_LOG("[COW] stop: invalidate journal status=0x%08X\n", invStatus);
		}
	}
	CdpCloseVolumeHandleEntry(item);
	Cdp_DBG("closed volume handle id=%llu\n", HandleId);
	return STATUS_SUCCESS;
}

static VOID CdpDisableAllCaptureSources(_In_ PCdp_DRIVER_EXTENSION DriverExt)
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
			PCdp_DEVICE_LIST_NODE node =
				CONTAINING_RECORD(entry, Cdp_DEVICE_LIST_NODE, Entry);
			PCdp_DEVICE_EXTENSION ext =
				(PCdp_DEVICE_EXTENSION)node->DeviceObject->DeviceExtension;
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

		CdpDisableAndDestroyCapture(
			(PCdp_DEVICE_EXTENSION)filterDevice->DeviceExtension);
		ObDereferenceObject(filterDevice);
	}
}

static PCdp_DEVICE_EXTENSION CdpFindSourceExtension(
	_In_ PCdp_DRIVER_EXTENSION DriverExt,
	_In_ PDEVICE_OBJECT LowerDevice)
{
	KIRQL oldIrql;
	PLIST_ENTRY entry;
	PCdp_DEVICE_EXTENSION found = NULL;

	KeAcquireSpinLock(&DriverExt->DeviceObjectListLock, &oldIrql);
	for (entry = DriverExt->DeviceObjectListHead.Flink;
		entry != &DriverExt->DeviceObjectListHead;
		entry = entry->Flink)
	{
		PCdp_DEVICE_LIST_NODE node =
			CONTAINING_RECORD(entry, Cdp_DEVICE_LIST_NODE, Entry);
		PCdp_DEVICE_EXTENSION ext =
			(PCdp_DEVICE_EXTENSION)node->DeviceObject->DeviceExtension;
		if (ext && ext->LowerDeviceObject == LowerDevice)
		{
			found = ext;
			break;
		}
	}
	KeReleaseSpinLock(&DriverExt->DeviceObjectListLock, oldIrql);
	return found;
}

static PCdp_DEVICE_EXTENSION CdpFindSourceExtensionByGuid(
	_In_ PCdp_DRIVER_EXTENSION DriverExt,
	_In_ const GUID* VolumeGuid)
{
	KIRQL oldIrql;
	PLIST_ENTRY entry;
	PCdp_DEVICE_EXTENSION found = NULL;

	KeAcquireSpinLock(&DriverExt->DeviceObjectListLock, &oldIrql);
	for (entry = DriverExt->DeviceObjectListHead.Flink;
		entry != &DriverExt->DeviceObjectListHead;
		entry = entry->Flink)
	{
		PCdp_DEVICE_LIST_NODE node =
			CONTAINING_RECORD(entry, Cdp_DEVICE_LIST_NODE, Entry);
		PCdp_DEVICE_EXTENSION ext =
			(PCdp_DEVICE_EXTENSION)node->DeviceObject->DeviceExtension;
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

static BOOLEAN CdpControlHandleAuthorized(
	_In_ PIRP Irp,
	_In_ PCdp_DRIVER_EXTENSION DriverExt,
	_In_ const GUID* SourceVolumeGuid)
{
	PIO_STACK_LOCATION irpSp = IoGetCurrentIrpStackLocation(Irp);
	PCdp_CONTROL_FILE_CONTEXT context =
		(PCdp_CONTROL_FILE_CONTEXT)irpSp->FileObject->FsContext;
	PCdp_DEVICE_EXTENSION sourceExt;
	PCdp_VOLUME_HANDLE_ENTRY journalEntry;
	Cdp_CREDENTIAL_DESCRIPTOR credential;
	BOOLEAN authorized = FALSE;

	if (!context || !context->Authenticated)
		return FALSE;
	if (KeQueryInterruptTime() >= context->ExpiresAt100ns)
	{
		RtlSecureZeroMemory(context, sizeof(*context));
		return FALSE;
	}
	sourceExt = CdpFindSourceExtensionByGuid(DriverExt, SourceVolumeGuid);
	journalEntry = CdpAcquireJournalForSource(DriverExt, sourceExt);
	if (!journalEntry)
		return FALSE;
	if (CdpJournalGetCredential(&journalEntry->Journal, &credential) &&
		RtlCompareMemory(&context->CredentialId, &credential.CredentialId,
			sizeof(GUID)) == sizeof(GUID) &&
		context->AuthEpoch == credential.AuthEpoch)
	{
		authorized = TRUE;
		/* Keep an active privileged operation alive without extending an idle
		 * control handle indefinitely.  In particular, e -> mount work -> r
		 * must not lose authorization merely because the middle step is slow. */
		context->ExpiresAt100ns = KeQueryInterruptTime() + 60ULL * 60ULL * 10000000ULL;
	}
	CdpReleaseVolumeHandleEntry(journalEntry);
	return authorized;
}

static NTSTATUS CdpConfigureCaptureInternal(
	_In_ PCdp_DRIVER_EXTENSION DriverExt,
	_In_ const GUID* SourceVolumeGuid,
	_In_ const GUID* JournalPartitionGuid,
	_In_ BOOLEAN FormatJournal,
	_In_opt_ const Cdp_CREDENTIAL_DESCRIPTOR* Credential,
	_Out_ PUINT64 JournalHandleId)
{
	UINT64 sourceHandleId = 0;
	UINT64 journalHandleId = 0;
	UINT64 sourcePartitionSize = 0;
	ULONG sourceSectorSize = 512;
	PDEVICE_OBJECT sourceLower = NULL;
	PCdp_DEVICE_EXTENSION sourceExt = NULL;
	PCdp_VOLUME_HANDLE_ENTRY sourceEntry;
	PCdp_VOLUME_HANDLE_ENTRY journalEntry;
	NTSTATUS status;

	*JournalHandleId = 0;
	Cdp_DBG("[COW] configure begin format=%u\n", FormatJournal ? 1u : 0u);
	CdpDbgGuid("[COW] source", SourceVolumeGuid);
	CdpDbgGuid("[COW] journal", JournalPartitionGuid);
	if (RtlCompareMemory(
		SourceVolumeGuid,
		JournalPartitionGuid,
		sizeof(GUID)) == sizeof(GUID))
	{
		return STATUS_INVALID_PARAMETER;
	}

	status = CdpOpenVolumeHandle(DriverExt, SourceVolumeGuid, &sourceHandleId);
	if (!NT_SUCCESS(status))
		return status;
	ExAcquireFastMutex(&DriverExt->VolumeHandleMutex);
	sourceEntry = CdpLookupVolumeHandleLocked(DriverExt, sourceHandleId);
	if (sourceEntry)
	{
		sourceLower = sourceEntry->TargetLowerDevice;
		sourcePartitionSize = sourceEntry->PartitionSize;
		sourceSectorSize = sourceEntry->SectorSize;
	}
	ExReleaseFastMutex(&DriverExt->VolumeHandleMutex);
	sourceExt = CdpFindSourceExtension(DriverExt, sourceLower);
	(void)CdpCloseVolumeHandle(DriverExt, sourceHandleId);
	if (!sourceExt)
		return STATUS_DEVICE_DOES_NOT_EXIST;
	if (InterlockedCompareExchange(&sourceExt->CaptureEnabled, 0, 0) != 0 ||
		sourceExt->Core != NULL ||
		sourceExt->JournalHandleId != 0)
	{
		return STATUS_DEVICE_BUSY;
	}

	status = CdpOpenVolumeHandle(DriverExt, JournalPartitionGuid, &journalHandleId);
	if (!NT_SUCCESS(status))
		return status;

	ExAcquireFastMutex(&DriverExt->VolumeHandleMutex);
	journalEntry = CdpLookupVolumeHandleLocked(DriverExt, journalHandleId);
	if (journalEntry)
		InterlockedIncrement(&journalEntry->ReferenceCount);
	ExReleaseFastMutex(&DriverExt->VolumeHandleMutex);
	if (!journalEntry)
	{
		(void)CdpCloseVolumeHandle(DriverExt, journalHandleId);
		return STATUS_INVALID_HANDLE;
	}

	Cdp_DBG("[JOURNAL-RAW] backend lowerDevice=%p\n",
		journalEntry->TargetLowerDevice);

	CdpJournalInitialize(
		&journalEntry->Journal,
		journalEntry->TargetLowerDevice,
		NULL,
		0,
		journalEntry->PartitionSize,
		journalEntry->SectorSize,
		SourceVolumeGuid);
	if (FormatJournal)
	{
		if (!Credential)
		{
			CdpReleaseVolumeHandleEntry(journalEntry);
			(void)CdpCloseVolumeHandle(DriverExt, journalHandleId);
			return STATUS_PASSWORD_RESTRICTION;
		}
		status = CdpJournalSetCredential(&journalEntry->Journal, Credential);
		if (!NT_SUCCESS(status))
		{
			CdpReleaseVolumeHandleEntry(journalEntry);
			(void)CdpCloseVolumeHandle(DriverExt, journalHandleId);
			return status;
		}
	}
	status = FormatJournal ?
		CdpJournalFormat(&journalEntry->Journal) :
		CdpJournalMount(&journalEntry->Journal);
	if (NT_SUCCESS(status) && !journalEntry->Journal.CredentialConfigured)
		status = STATUS_PASSWORD_RESTRICTION;
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
		CdpReleaseVolumeHandleEntry(journalEntry);
		(void)CdpCloseVolumeHandle(DriverExt, journalHandleId);
		return status;
	}

	CdpDisableAndDestroyCapture(sourceExt);
	sourceExt->VolumeGuid = *SourceVolumeGuid;
	sourceExt->VolumeGuidValid = TRUE;
	sourceExt->SectorSize = sourceSectorSize;
	sourceExt->JournalHandleId = journalHandleId;

	if (!sourceExt->CaptureThreadHandle)
	{
		status = CdpStartCaptureWorker(sourceExt);
		if (!NT_SUCCESS(status))
		{
			CdpReleaseVolumeHandleEntry(journalEntry);
			(void)CdpCloseVolumeHandle(DriverExt, journalHandleId);
			return status;
		}
	}
	if (!sourceExt->RecoveryReadThreadHandle)
	{
		status = CdpStartRecoveryReadWorker(sourceExt);
		if (!NT_SUCCESS(status))
		{
			CdpStopCaptureWorker(sourceExt);
			CdpReleaseVolumeHandleEntry(journalEntry);
			(void)CdpCloseVolumeHandle(DriverExt, journalHandleId);
			return status;
		}
	}
	{
		PCdp_STORE sourceStore = NULL;
		status = CdpDevStoreCreate(
			sourceExt->LowerDeviceObject,
			sourcePartitionSize,
			sourceSectorSize,
			&sourceStore);
		if (NT_SUCCESS(status))
		{
			status = CdpCoreBind(
				sourceStore,
				&journalEntry->Journal,
				SourceVolumeGuid,
				&sourceExt->Core);
		}
		if (!NT_SUCCESS(status))
		{
			if (sourceStore)
				CdpDevStoreDestroy(sourceStore);
			CdpReleaseVolumeHandleEntry(journalEntry);
			(void)CdpCloseVolumeHandle(DriverExt, journalHandleId);
			return status;
		}
	}
	CdpReleaseVolumeHandleEntry(journalEntry);

	InterlockedExchange(&sourceExt->CaptureEnabled, 1);

	*JournalHandleId = journalHandleId;
	Cdp_LOG("[COW] configured journalHandle=%llu size=%llu sector=%lu sourceExt=%p\n",
		journalHandleId,
		journalEntry->PartitionSize,
		journalEntry->SectorSize,
		sourceExt);
	return STATUS_SUCCESS;
}

static NTSTATUS CdpConfigureCapture(
	_In_ PCdp_DRIVER_EXTENSION DriverExt,
	_In_ const GUID* SourceVolumeGuid,
	_In_ const GUID* JournalPartitionGuid,
	_In_ BOOLEAN FormatJournal,
	_In_opt_ const Cdp_CREDENTIAL_DESCRIPTOR* Credential,
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
	status = CdpConfigureCaptureInternal(
		DriverExt,
		SourceVolumeGuid,
		JournalPartitionGuid,
		FormatJournal,
		Credential,
		JournalHandleId);
	KeReleaseMutex(&DriverExt->CaptureConfigMutex, FALSE);
	return status;
}

static BOOLEAN CdpGuidIsZero(_In_ const GUID* Guid)
{
	static const GUID zeroGuid = { 0 };
	return RtlCompareMemory(Guid, &zeroGuid, sizeof(GUID)) == sizeof(GUID);
}

static NTSTATUS CdpActivateAutoJournal(
	_In_ PCdp_DRIVER_EXTENSION DriverExt,
	_In_ UINT64 JournalHandleId,
	_Inout_ PCdp_VOLUME_HANDLE_ENTRY JournalEntry)
{
	UINT64 sourcePartitionSize = 0;
	ULONG sourceSectorSize = 0;
	PCdp_DEVICE_EXTENSION sourceExt;
	PCdp_STORE sourceStore = NULL;
	GUID sourceGuid = JournalEntry->Journal.SourceVolumeGuid;
	NTSTATUS status;

	if (CdpGuidIsZero(&sourceGuid))
		return STATUS_INVALID_PARAMETER;

	sourceExt = CdpFindSourceExtensionByGuid(DriverExt, &sourceGuid);
	if (!sourceExt ||
		InterlockedCompareExchange(&sourceExt->Started, 0, 0) == 0 ||
		!sourceExt->VolumeGuidValid ||
		!sourceExt->LowerDeviceObject)
	{
		return STATUS_DEVICE_DOES_NOT_EXIST;
	}
	if (InterlockedCompareExchange(&sourceExt->CaptureEnabled, 0, 0) != 0 ||
		sourceExt->Core != NULL ||
		sourceExt->JournalHandleId != 0)
	{
		return STATUS_DEVICE_BUSY;
	}

	status = CdpQueryDeviceGeometry(
		sourceExt->LowerDeviceObject,
		&sourcePartitionSize,
		&sourceSectorSize);
	if (!NT_SUCCESS(status))
		return status;

	sourceExt->VolumeGuid = sourceGuid;
	sourceExt->VolumeGuidValid = TRUE;
	sourceExt->SectorSize = sourceSectorSize;
	status = CdpDevStoreCreate(
		sourceExt->LowerDeviceObject,
		sourcePartitionSize,
		sourceSectorSize,
		&sourceStore);
	if (NT_SUCCESS(status))
	{
		status = CdpCoreBind(
			sourceStore,
			&JournalEntry->Journal,
			&sourceGuid,
			&sourceExt->Core);
	}
	if (!NT_SUCCESS(status))
	{
		if (sourceStore)
			CdpDevStoreDestroy(sourceStore);
		return status;
	}
	if (!sourceExt->CaptureThreadHandle)
	{
		status = CdpStartCaptureWorker(sourceExt);
		if (!NT_SUCCESS(status))
		{
			CdpDisableAndDestroyCapture(sourceExt);
			return status;
		}
	}
	if (!sourceExt->RecoveryReadThreadHandle)
	{
		status = CdpStartRecoveryReadWorker(sourceExt);
		if (!NT_SUCCESS(status))
		{
			CdpDisableAndDestroyCapture(sourceExt);
			return status;
		}
	}

	sourceExt->JournalHandleId = JournalHandleId;
	InterlockedExchange(&sourceExt->CaptureEnabled, 1);
	Cdp_LOG("[AUTO-CDP] enabled journalHandle=%llu sourceExt=%p\n",
		JournalHandleId, sourceExt);
	CdpDbgGuid("[AUTO-CDP] source", &sourceGuid);
	return STATUS_SUCCESS;
}

static BOOLEAN CdpGuidIsEqual(_In_ const GUID* A, _In_ const GUID* B)
{
	return RtlCompareMemory(A, B, sizeof(GUID)) == sizeof(GUID);
}

#define Cdp_AUTO_KIND_UNKNOWN 0
#define Cdp_AUTO_KIND_SOURCE  1
#define Cdp_AUTO_KIND_JOURNAL 2

static VOID CdpMarkAutoDiscoverySettled(_Inout_ PCdp_DRIVER_EXTENSION DriverExt)
{
	InterlockedExchange(&DriverExt->AutoDiscoverySettled, 1);
	KeSetEvent(&DriverExt->AutoDiscoverySettledEvent, IO_NO_INCREMENT, FALSE);
}

static VOID CdpOpenAutoDiscoveryGate(_Inout_ PCdp_DEVICE_EXTENSION DevExt)
{
	if (!DevExt)
		return;
	InterlockedExchange(&DevExt->AutoDiscoveryGateActive, 0);
	KeSetEvent(&DevExt->AutoDiscoveryGateEvent, IO_NO_INCREMENT, FALSE);
}

static VOID CdpOpenAllAutoDiscoveryGates(_Inout_ PCdp_DRIVER_EXTENSION DriverExt)
{
	KIRQL oldIrql;
	PLIST_ENTRY entry;

	KeAcquireSpinLock(&DriverExt->DeviceObjectListLock, &oldIrql);
	for (entry = DriverExt->DeviceObjectListHead.Flink;
		entry != &DriverExt->DeviceObjectListHead;
		entry = entry->Flink)
	{
		PCdp_DEVICE_LIST_NODE node =
			CONTAINING_RECORD(entry, Cdp_DEVICE_LIST_NODE, Entry);
		PCdp_DEVICE_EXTENSION ext =
			(PCdp_DEVICE_EXTENSION)node->DeviceObject->DeviceExtension;
		if (ext)
			CdpOpenAutoDiscoveryGate(ext);
	}
	KeReleaseSpinLock(&DriverExt->DeviceObjectListLock, oldIrql);
}

static VOID CdpClearAutoDiscoverySettled(_Inout_ PCdp_DRIVER_EXTENSION DriverExt)
{
	InterlockedExchange(&DriverExt->AutoDiscoverySettled, 0);
	KeClearEvent(&DriverExt->AutoDiscoverySettledEvent);
}

static PCdp_DEVICE_EXTENSION CdpFindStartedSourceByGuid(
	_In_ PCdp_DRIVER_EXTENSION DriverExt,
	_In_ const GUID* SourceGuid)
{
	KIRQL oldIrql;
	PLIST_ENTRY entry;
	PCdp_DEVICE_EXTENSION found = NULL;

	if (!SourceGuid || CdpGuidIsZero(SourceGuid))
		return NULL;

	KeAcquireSpinLock(&DriverExt->DeviceObjectListLock, &oldIrql);
	for (entry = DriverExt->DeviceObjectListHead.Flink;
		entry != &DriverExt->DeviceObjectListHead;
		entry = entry->Flink)
	{
		PCdp_DEVICE_LIST_NODE node =
			CONTAINING_RECORD(entry, Cdp_DEVICE_LIST_NODE, Entry);
		PCdp_DEVICE_EXTENSION ext =
			(PCdp_DEVICE_EXTENSION)node->DeviceObject->DeviceExtension;
		if (!ext)
			continue;
		if (InterlockedCompareExchange(&ext->Started, 0, 0) == 0)
			continue;
		if (InterlockedCompareExchange(&ext->AutoKind, 0, 0) != Cdp_AUTO_KIND_SOURCE)
			continue;
		if (!ext->VolumeGuidValid)
			continue;
		if (CdpGuidIsEqual(&ext->VolumeGuid, SourceGuid))
		{
			found = ext;
			break;
		}
	}
	KeReleaseSpinLock(&DriverExt->DeviceObjectListLock, oldIrql);
	return found;
}

static BOOLEAN CdpAutoDiscoveryHasUnclassified(
	_In_ PCdp_DRIVER_EXTENSION DriverExt)
{
	KIRQL oldIrql;
	PLIST_ENTRY entry;
	BOOLEAN found = FALSE;

	KeAcquireSpinLock(&DriverExt->DeviceObjectListLock, &oldIrql);
	for (entry = DriverExt->DeviceObjectListHead.Flink;
		entry != &DriverExt->DeviceObjectListHead;
		entry = entry->Flink)
	{
		PCdp_DEVICE_LIST_NODE node =
			CONTAINING_RECORD(entry, Cdp_DEVICE_LIST_NODE, Entry);
		PCdp_DEVICE_EXTENSION ext =
			(PCdp_DEVICE_EXTENSION)node->DeviceObject->DeviceExtension;
		if (ext &&
			InterlockedCompareExchange(&ext->Started, 0, 0) != 0 &&
			InterlockedCompareExchange(&ext->AutoKind, 0, 0) == Cdp_AUTO_KIND_UNKNOWN)
		{
			found = TRUE;
			break;
		}
	}
	KeReleaseSpinLock(&DriverExt->DeviceObjectListLock, oldIrql);
	return found;
}

static BOOLEAN CdpAutoDiscoveryHasPendingJournal(
	_In_ PCdp_DRIVER_EXTENSION DriverExt)
{
	BOOLEAN pending = FALSE;
	PLIST_ENTRY entry;

	ExAcquireFastMutex(&DriverExt->VolumeHandleMutex);
	for (entry = DriverExt->VolumeHandleList.Flink;
		entry != &DriverExt->VolumeHandleList;
		entry = entry->Flink)
	{
		PCdp_VOLUME_HANDLE_ENTRY item =
			CONTAINING_RECORD(entry, Cdp_VOLUME_HANDLE_ENTRY, Entry);
		PCdp_DEVICE_EXTENSION sourceExt;

		if (item->Closing || !item->Journal.Mounted)
			continue;
		sourceExt = CdpFindStartedSourceByGuid(
			DriverExt,
			&item->Journal.SourceVolumeGuid);
		if (sourceExt == NULL)
		{
			pending = TRUE;
			break;
		}
		if (InterlockedCompareExchange(&sourceExt->CaptureEnabled, 0, 0) == 0 &&
			sourceExt->JournalHandleId == 0)
		{
			// Source is up but not paired yet — not "pending", ready to activate.
			continue;
		}
	}
	ExReleaseFastMutex(&DriverExt->VolumeHandleMutex);
	return pending;
}

static VOID CdpAutoDiscoveryRefreshSettled(
	_Inout_ PCdp_DRIVER_EXTENSION DriverExt)
{
	if (InterlockedCompareExchange(
			&DriverExt->BootEnumerationComplete, 0, 0) == 0)
	{
		// A source volume can START before its journal volume.  Treating the
		// currently visible set as complete here would let NTFS mount the source
		// before a later journal exposes a pending reboot recovery.
		CdpClearAutoDiscoverySettled(DriverExt);
		return;
	}
	if (InterlockedCompareExchange(&DriverExt->AutoDiscoverySuppressed, 0, 0) != 0 ||
		InterlockedCompareExchange(&DriverExt->AutoDiscoveryStopping, 0, 0) != 0)
	{
		CdpMarkAutoDiscoverySettled(DriverExt);
		return;
	}
	if (CdpAutoDiscoveryHasUnclassified(DriverExt) ||
		CdpAutoDiscoveryHasPendingJournal(DriverExt))
	{
		CdpClearAutoDiscoverySettled(DriverExt);
		return;
	}
	CdpMarkAutoDiscoverySettled(DriverExt);
	CdpOpenAllAutoDiscoveryGates(DriverExt);
}

static NTSTATUS CdpClassifyStartedVolume(
	_Inout_ PCdp_DRIVER_EXTENSION DriverExt,
	_In_ PDEVICE_OBJECT FilterDevice)
{
	PCdp_DEVICE_EXTENSION ext;
	PCdp_VOLUME_HANDLE_ENTRY journalEntry = NULL;
	UINT64 partitionSize = 0;
	ULONG sectorSize = 0;
	NTSTATUS status;
	GUID volumeGuid;
	GUID zeroGuid = { 0 };

	ext = (PCdp_DEVICE_EXTENSION)FilterDevice->DeviceExtension;
	if (!ext ||
		InterlockedCompareExchange(&ext->Started, 0, 0) == 0)
	{
		return STATUS_INVALID_DEVICE_STATE;
	}
	if (InterlockedCompareExchange(&ext->AutoKind, 0, 0) != Cdp_AUTO_KIND_UNKNOWN)
		return STATUS_SUCCESS;
	if (!ExAcquireRundownProtection(&ext->AutoDiscoveryRundown))
		return STATUS_DEVICE_NOT_READY;
	if (!ext->LowerDeviceObject)
	{
		ExReleaseRundownProtection(&ext->AutoDiscoveryRundown);
		return STATUS_DEVICE_NOT_READY;
	}

	status = CdpQueryDeviceGeometry(
		ext->LowerDeviceObject,
		&partitionSize,
		&sectorSize);
	if (!NT_SUCCESS(status))
	{
		ExReleaseRundownProtection(&ext->AutoDiscoveryRundown);
		return status;
	}

	journalEntry = (PCdp_VOLUME_HANDLE_ENTRY)cdpalloc(sizeof(*journalEntry));
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
	CdpJournalInitialize(
		&journalEntry->Journal,
		journalEntry->TargetLowerDevice,
		NULL,
		0,
		partitionSize,
		sectorSize,
		&zeroGuid);
	status = CdpJournalMount(&journalEntry->Journal);
	if (NT_SUCCESS(status) && journalEntry->Journal.CredentialConfigured &&
		!CdpGuidIsZero(&journalEntry->Journal.SourceVolumeGuid))
	{
		GUID journalGuid = { 0 };

		status = IoVolumeDeviceToGuid(
			ext->PhysicalDeviceObject ?
				ext->PhysicalDeviceObject :
				FilterDevice,
			&journalGuid);
		if (!NT_SUCCESS(status))
			status = IoVolumeDeviceToGuid(ext->LowerDeviceObject, &journalGuid);
		if (NT_SUCCESS(status) && !CdpGuidIsZero(&journalGuid))
		{
			journalEntry->VolumeGuid = journalGuid;
			journalEntry->VolumeGuidValid = TRUE;
		}
		journalEntry->HandleId =
			(UINT64)InterlockedIncrement64(&DriverExt->VolumeHandleNextId);
		ExAcquireFastMutex(&DriverExt->VolumeHandleMutex);
		InsertTailList(&DriverExt->VolumeHandleList, &journalEntry->Entry);
		ExReleaseFastMutex(&DriverExt->VolumeHandleMutex);
		InterlockedExchange(&ext->AutoKind, Cdp_AUTO_KIND_JOURNAL);
		ext->SectorSize = sectorSize;
		Cdp_DBG("[AUTO-CDP] classified JOURNAL filter=%p handle=%llu\n",
			FilterDevice, journalEntry->HandleId);
		CdpDbgGuid("[AUTO-CDP] journal sourceGuid",
			&journalEntry->Journal.SourceVolumeGuid);
		ExReleaseRundownProtection(&ext->AutoDiscoveryRundown);
		return STATUS_SUCCESS;
	}

	// Probe I/O / resource failures: keep UNKNOWN and retry later.
	// STATUS_DISK_CORRUPT_ERROR means readable but no valid journal magic.
	if (!NT_SUCCESS(status) && status != STATUS_DISK_CORRUPT_ERROR)
	{
		Cdp_DBG("[AUTO-CDP] classify probe failed status=0x%08X filter=%p; retry\n",
			status, FilterDevice);
		CdpJournalClose(&journalEntry->Journal);
		cdpfree(journalEntry);
		ExReleaseRundownProtection(&ext->AutoDiscoveryRundown);
		return status;
	}

	CdpJournalClose(&journalEntry->Journal);
	cdpfree(journalEntry);

	RtlZeroMemory(&volumeGuid, sizeof(volumeGuid));
	status = IoVolumeDeviceToGuid(
		ext->PhysicalDeviceObject ?
			ext->PhysicalDeviceObject :
			FilterDevice,
		&volumeGuid);
	if (!NT_SUCCESS(status))
		status = IoVolumeDeviceToGuid(ext->LowerDeviceObject, &volumeGuid);
	if (NT_SUCCESS(status) && !CdpGuidIsZero(&volumeGuid))
	{
		ext->VolumeGuid = volumeGuid;
		ext->VolumeGuidValid = TRUE;
	}
	else
	{
		ext->VolumeGuidValid = FALSE;
		Cdp_DBG("[AUTO-CDP] source guid query failed status=0x%08X filter=%p\n",
			status, FilterDevice);
	}
	ext->SectorSize = sectorSize;
	InterlockedExchange(&ext->AutoKind, Cdp_AUTO_KIND_SOURCE);
	Cdp_DBG("[AUTO-CDP] classified SOURCE filter=%p guidValid=%u\n",
		FilterDevice, ext->VolumeGuidValid ? 1u : 0u);
	if (ext->VolumeGuidValid)
		CdpDbgGuid("[AUTO-CDP] source Guid", &ext->VolumeGuid);
	ExReleaseRundownProtection(&ext->AutoDiscoveryRundown);
	return STATUS_SUCCESS;
}

static NTSTATUS CdpTryActivateReadyPairs(
	_Inout_ PCdp_DRIVER_EXTENSION DriverExt)
{
	NTSTATUS result = STATUS_NOT_FOUND;
	ULONG activated = 0;

	for (;;)
	{
		PCdp_VOLUME_HANDLE_ENTRY journalEntry = NULL;
		UINT64 journalHandleId = 0;
		PLIST_ENTRY entry;
		NTSTATUS status;

		ExAcquireFastMutex(&DriverExt->VolumeHandleMutex);
		for (entry = DriverExt->VolumeHandleList.Flink;
			entry != &DriverExt->VolumeHandleList;
			entry = entry->Flink)
		{
			PCdp_VOLUME_HANDLE_ENTRY item =
				CONTAINING_RECORD(entry, Cdp_VOLUME_HANDLE_ENTRY, Entry);
			PCdp_DEVICE_EXTENSION sourceExt;

			if (item->Closing || !item->Journal.Mounted)
				continue;
			sourceExt = CdpFindStartedSourceByGuid(
				DriverExt,
				&item->Journal.SourceVolumeGuid);
			if (sourceExt == NULL)
				continue;
			if (InterlockedCompareExchange(&sourceExt->CaptureEnabled, 0, 0) != 0 ||
				sourceExt->JournalHandleId != 0)
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

		status = CdpActivateAutoJournal(
			DriverExt,
			journalHandleId,
			journalEntry);
		if (NT_SUCCESS(status))
		{
			if (journalEntry->Journal.RecoveryPending)
			{
				Cdp_RECOVERY_BEGIN_REQUEST beginRequest;
				Cdp_RECOVERY_BEGIN_REPLY beginReply;
				Cdp_RECOVERY_CONTROL_REQUEST commitRequest;
				Cdp_RECOVERY_COMMIT_REPLY commitReply;

				RtlZeroMemory(&beginRequest, sizeof(beginRequest));
				beginRequest.SourceVolumeGuid =
					journalEntry->Journal.SourceVolumeGuid;
				beginRequest.TargetTime100ns =
					journalEntry->Journal.RecoveryTargetTime100ns;
				// The intent is already persisted.  This invocation performs the
				// actual begin; commit clears the marker on success.
				beginRequest.Flags = 0;
				Cdp_LOG("[RECOVERY] reboot intent found target=%llu; auto begin then release I/O and commit\n",
					beginRequest.TargetTime100ns);
				status = CdpBeginRecovery(DriverExt, &beginRequest, &beginReply);
				if (NT_SUCCESS(status))
				{
					// Begin has established the coherent recovery view.  Release
					// queued boot I/O now; reads use that view and writes COW/punch
					// overlapping history while Commit advances node by node.
					CdpOpenAutoDiscoveryGate(CdpFindStartedSourceByGuid(
						DriverExt, &beginRequest.SourceVolumeGuid));
					Cdp_LOG("[RECOVERY] auto begin completed; boot I/O released before commit\n");
					RtlZeroMemory(&commitRequest, sizeof(commitRequest));
					commitRequest.SourceVolumeGuid = beginRequest.SourceVolumeGuid;
					status = CdpCommitRecovery(DriverExt, &commitRequest, &commitReply);
					if (NT_SUCCESS(status))
					{
						Cdp_LOG("[RECOVERY] auto commit completed after boot I/O release\n");
					}
				}
				if (!NT_SUCCESS(status))
				{
					Cdp_LOG("[RECOVERY] reboot auto recovery failed status=0x%08X\n", status);
					// A failed Begin must not leave boot I/O behind the discovery
					// gate forever.  Keep the persisted intent for diagnosis/retry,
					// but let the current boot continue against the live volume.
					CdpOpenAutoDiscoveryGate(CdpFindStartedSourceByGuid(
						DriverExt, &beginRequest.SourceVolumeGuid));
				}
			}
			else
			{
				// A normally activated source has no pending reboot recovery.
				CdpOpenAutoDiscoveryGate(CdpFindStartedSourceByGuid(
					DriverExt, &journalEntry->Journal.SourceVolumeGuid));
			}
			Cdp_DBG("[AUTO-CDP] pair activated journalHandle=%llu\n",
				journalHandleId);
			++activated;
			result = STATUS_SUCCESS;
			CdpReleaseVolumeHandleEntry(journalEntry);
			continue;
		}
		CdpReleaseVolumeHandleEntry(journalEntry);
		Cdp_LOG("[AUTO-CDP] pair activate failed status=0x%08X\n", status);
		if (result == STATUS_NOT_FOUND)
			result = status;
		break;
	}
	UNREFERENCED_PARAMETER(activated);
	return result;
}

static VOID CdpClassifyAllUnknownVolumes(
	_Inout_ PCdp_DRIVER_EXTENSION DriverExt)
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
			PCdp_DEVICE_LIST_NODE node =
				CONTAINING_RECORD(entry, Cdp_DEVICE_LIST_NODE, Entry);
			PCdp_DEVICE_EXTENSION ext =
				(PCdp_DEVICE_EXTENSION)node->DeviceObject->DeviceExtension;
			if (ext &&
				InterlockedCompareExchange(&ext->Started, 0, 0) != 0 &&
				InterlockedCompareExchange(&ext->AutoKind, 0, 0) ==
					Cdp_AUTO_KIND_UNKNOWN)
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
		(void)CdpClassifyStartedVolume(DriverExt, devices[i]);
		ObDereferenceObject(devices[i]);
	}
	for (; i < deviceCount; ++i)
		ObDereferenceObject(devices[i]);
}

static VOID CdpAutoDiscoveryWorker(_In_ PVOID Context)
{
	PCdp_DRIVER_EXTENSION driverExt = (PCdp_DRIVER_EXTENSION)Context;
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
		Cdp_DBG("[AUTO-CDP] classify+pair begin\n");
		CdpClassifyAllUnknownVolumes(driverExt);
		status = CdpTryActivateReadyPairs(driverExt);
		if (NT_SUCCESS(status))
			Cdp_LOG("[AUTO-CDP] CDP enabled\n");
		else
			Cdp_DBG("[AUTO-CDP] no ready pair status=0x%08X\n", status);
		CdpAutoDiscoveryRefreshSettled(driverExt);
		KeReleaseMutex(&driverExt->CaptureConfigMutex, FALSE);
	}
	else if (NT_SUCCESS(status))
	{
		CdpAutoDiscoveryRefreshSettled(driverExt);
		KeReleaseMutex(&driverExt->CaptureConfigMutex, FALSE);
	}

	InterlockedExchange(&driverExt->AutoDiscoveryRunning, 0);
	InterlockedExchange(&driverExt->AutoDiscoveryQueued, 0);
	KeSetEvent(&driverExt->AutoDiscoveryIdle, IO_NO_INCREMENT, FALSE);

	if (InterlockedCompareExchange(&driverExt->AutoDiscoveryStopping, 0, 0) == 0 &&
		InterlockedCompareExchange(&driverExt->AutoDiscoverySuppressed, 0, 0) == 0 &&
		CdpAutoDiscoveryHasUnclassified(driverExt))
	{
		LARGE_INTEGER delay;

		// Probe I/O may fail right after START; back off before retry.
		Cdp_DBG("[AUTO-CDP] unclassified remains; retry after delay\n");
		CdpClearAutoDiscoverySettled(driverExt);
		delay.QuadPart = -1000000LL; // 100ms
		KeDelayExecutionThread(KernelMode, FALSE, &delay);
		if (InterlockedCompareExchange(&driverExt->AutoDiscoveryStopping, 0, 0) == 0 &&
			InterlockedCompareExchange(&driverExt->AutoDiscoverySuppressed, 0, 0) == 0 &&
			CdpAutoDiscoveryHasUnclassified(driverExt))
		{
			CdpQueueAutoDiscovery(driverExt);
		}
	}
	else if (
		InterlockedCompareExchange(&driverExt->AutoDiscoveryStopping, 0, 0) == 0 &&
		InterlockedCompareExchange(&driverExt->AutoDiscoverySuppressed, 0, 0) == 0 &&
		CdpAutoDiscoveryHasPendingJournal(driverExt))
	{
		// Journal waits on a source START; do not spin — next START re-queues.
		Cdp_DBG("[AUTO-CDP] journal pending source; wait for START\n");
		CdpClearAutoDiscoverySettled(driverExt);
	}
}

VOID CdpInitializeAutoDiscovery(_Inout_ PCdp_DRIVER_EXTENSION DriverExt)
{
	KeInitializeEvent(&DriverExt->AutoDiscoveryIdle, NotificationEvent, TRUE);
	KeInitializeEvent(
		&DriverExt->AutoDiscoverySettledEvent,
		NotificationEvent,
		FALSE);
	InterlockedExchange(&DriverExt->AutoDiscoverySettled, 0);
	InterlockedExchange(&DriverExt->AutoDiscoveryRunning, 0);
	InterlockedExchange(&DriverExt->BootEnumerationComplete, 0);
	ExInitializeWorkItem(
		&DriverExt->AutoDiscoveryWorkItem,
		CdpAutoDiscoveryWorker,
		DriverExt);
}

VOID CdpQueueAutoDiscovery(_Inout_ PCdp_DRIVER_EXTENSION DriverExt)
{
	if (!DriverExt)
		return;
	if (InterlockedCompareExchange(
			&DriverExt->BootEnumerationComplete, 0, 0) == 0)
	{
		Cdp_DBG("[AUTO-CDP] discovery deferred until boot enumeration completes\n");
		return;
	}
	if (InterlockedCompareExchange(&DriverExt->AutoDiscoveryStopping, 0, 0) != 0)
		return;
	if (InterlockedCompareExchange(&DriverExt->AutoDiscoverySuppressed, 0, 0) != 0)
	{
		CdpMarkAutoDiscoverySettled(DriverExt);
		return;
	}
	if (InterlockedCompareExchange(&DriverExt->AutoDiscoveryQueued, 1, 0) != 0)
	{
		Cdp_DBG("[AUTO-CDP] worker already queued/running\n");
		return;
	}
	Cdp_DBG("[AUTO-CDP] queueing classify+pair worker\n");
	KeClearEvent(&DriverExt->AutoDiscoveryIdle);
	ExQueueWorkItem(&DriverExt->AutoDiscoveryWorkItem, CriticalWorkQueue);
}

VOID CdpScheduleAutoDiscovery(_Inout_ PCdp_DRIVER_EXTENSION DriverExt)
{
	CdpQueueAutoDiscovery(DriverExt);
}

VOID CdpStopAutoDiscovery(_Inout_ PCdp_DRIVER_EXTENSION DriverExt)
{
	InterlockedExchange(&DriverExt->AutoDiscoveryStopping, 1);
	Cdp_DBG("[AUTO-CDP] stopping worker\n");
	CdpMarkAutoDiscoverySettled(DriverExt);
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

static PCdp_PREVIEW_SESSION CdpLookupPreviewSessionLocked(
	_In_ PCdp_DRIVER_EXTENSION DriverExt,
	_In_ UINT64 HandleId)
{
	PLIST_ENTRY entry = DriverExt->PreviewSessionList.Flink;
	while (entry != &DriverExt->PreviewSessionList)
	{
		PCdp_PREVIEW_SESSION session =
			CONTAINING_RECORD(entry, Cdp_PREVIEW_SESSION, Entry);
		if (session->HandleId == HandleId)
			return session;
		entry = entry->Flink;
	}
	return NULL;
}

static BOOLEAN CdpAnyPreviewSessionActive(
	_In_ PCdp_DRIVER_EXTENSION DriverExt)
{
	BOOLEAN active;

	ExAcquireFastMutex(&DriverExt->PreviewSessionMutex);
	active = !IsListEmpty(&DriverExt->PreviewSessionList);
	ExReleaseFastMutex(&DriverExt->PreviewSessionMutex);
	return active;
}

static PCdp_PREVIEW_SESSION CdpAcquirePreviewSession(
	_In_ PCdp_DRIVER_EXTENSION DriverExt,
	_In_ UINT64 HandleId)
{
	PCdp_PREVIEW_SESSION session;

	ExAcquireFastMutex(&DriverExt->PreviewSessionMutex);
	session = CdpLookupPreviewSessionLocked(DriverExt, HandleId);
	if (session && !session->Closing)
		InterlockedIncrement(&session->ReferenceCount);
	else
		session = NULL;
	ExReleaseFastMutex(&DriverExt->PreviewSessionMutex);
	return session;
}

static VOID CdpReleasePreviewSession(_In_ PCdp_PREVIEW_SESSION Session)
{
	if (InterlockedDecrement(&Session->ReferenceCount) == 0)
		KeSetEvent(&Session->NoReferences, IO_NO_INCREMENT, FALSE);
}

static VOID CdpDestroyPreviewSession(
	_In_ PCdp_DRIVER_EXTENSION DriverExt,
	_In_ PCdp_PREVIEW_SESSION Session)
{
	CdpReleasePreviewSession(Session); // Drop list ownership.
	KeWaitForSingleObject(
		&Session->NoReferences,
		Executive,
		KernelMode,
		FALSE,
		NULL);
	if (Session->SourceVolumeHandleId)
		(void)CdpCloseVolumeHandle(
			DriverExt,
			Session->SourceVolumeHandleId);
	if (Session->JournalEntry)
		CdpReleaseVolumeHandleEntry(Session->JournalEntry);
	cdpfree(Session);
}

static NTSTATUS CdpBeginPreviewSession(
	_In_ PCdp_DRIVER_EXTENSION DriverExt,
	_In_ const Cdp_PREVIEW_BEGIN_REQUEST* Request,
	_Out_ PCdp_PREVIEW_BEGIN_REPLY Reply)
{
	PCdp_VOLUME_HANDLE_ENTRY journalEntry = NULL;
	PCdp_PREVIEW_SESSION session = NULL;
	PCdp_DEVICE_EXTENSION sourceExt = NULL;
	UINT64 sourceHandleId = 0;
	UINT64 oldestTime = 0;
	UINT64 newestTime = 0;
	UINT64 targetTime = Request->TargetTime100ns;
	BOOLEAN phaseTransitioned = FALSE;
	NTSTATUS status;

		RtlZeroMemory(Reply, sizeof(*Reply));
	sourceExt = CdpFindSourceExtensionByGuid(
		DriverExt,
		&Request->SourceVolumeGuid);
	if (!sourceExt)
		return STATUS_DEVICE_DOES_NOT_EXIST;
	journalEntry = CdpAcquireJournalForSource(DriverExt, sourceExt);
	if (!journalEntry)
		return STATUS_DEVICE_NOT_READY;
	if (InterlockedCompareExchange(&sourceExt->Phase, 0, 0) !=
		(LONG)Cdp_PHASE_GENERAL)
	{
		CdpReleaseVolumeHandleEntry(journalEntry);
		return STATUS_INVALID_DEVICE_STATE;
	}
	if (CdpAnyPreviewSessionActive(DriverExt))
	{
		CdpReleaseVolumeHandleEntry(journalEntry);
		return STATUS_INVALID_DEVICE_STATE;
	}
	if (!sourceExt->Core)
	{
		CdpReleaseVolumeHandleEntry(journalEntry);
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

	status = CdpJournalQueryTimeRange(
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
	else if (targetTime < oldestTime)
		targetTime = oldestTime;

	status = CdpOpenVolumeHandle(
		DriverExt,
		&Request->SourceVolumeGuid,
		&sourceHandleId);
	if (!NT_SUCCESS(status))
		goto cleanup;

	if (InterlockedCompareExchange(
			&sourceExt->Phase,
			(LONG)Cdp_PHASE_PREVIEW,
			(LONG)Cdp_PHASE_GENERAL) != (LONG)Cdp_PHASE_GENERAL)
	{
		status = STATUS_INVALID_DEVICE_STATE;
		goto cleanup;
	}
	phaseTransitioned = TRUE;

	if (CdpAnyPreviewSessionActive(DriverExt))
	{
		status = STATUS_INVALID_DEVICE_STATE;
		goto cleanup;
	}

	session = (PCdp_PREVIEW_SESSION)cdpalloc(sizeof(*session));
	if (!session)
	{
		status = STATUS_INSUFFICIENT_RESOURCES;
		goto cleanup;
	}
	RtlZeroMemory(session, sizeof(*session));
	session->HandleId =
		(UINT64)InterlockedIncrement64(&DriverExt->PreviewSessionNextId);
	session->TargetTime100ns = targetTime;
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

	status = CdpCorePreviewBegin(sourceExt->Core, targetTime);
	if (!NT_SUCCESS(status))
	{
		ExAcquireFastMutex(&DriverExt->PreviewSessionMutex);
		RemoveEntryList(&session->Entry);
		ExReleaseFastMutex(&DriverExt->PreviewSessionMutex);
		goto cleanup;
	}
	session->TargetTime100ns = CdpCoreGetTargetTime100ns(sourceExt->Core);
	Reply->PreviewHandle = session->HandleId;
	Reply->TargetTime100ns = session->TargetTime100ns;
	Reply->OldestRecoverable100ns = oldestTime;
	Reply->NewestRecoverable100ns = newestTime;

	Cdp_DBG("[PREVIEW] begin handle=%llu target=%llu (Core)\n",
		session->HandleId,
		session->TargetTime100ns);
	return STATUS_SUCCESS;

cleanup:
	if (session)
	{
		cdpfree(session);
		journalEntry = NULL;
		sourceHandleId = 0;
	}
	if (phaseTransitioned && sourceExt)
	{
		if (sourceExt->Core)
			(void)CdpCorePreviewEnd(sourceExt->Core);
		InterlockedExchange(&sourceExt->Phase, (LONG)Cdp_PHASE_GENERAL);
	}
	if (sourceHandleId)
		(void)CdpCloseVolumeHandle(DriverExt, sourceHandleId);
	if (journalEntry)
		CdpReleaseVolumeHandleEntry(journalEntry);
	return status;
}

static NTSTATUS CdpEndPreviewSession(
	_In_ PCdp_DRIVER_EXTENSION DriverExt,
	_In_ UINT64 HandleId)
{
	PCdp_PREVIEW_SESSION session;
	GUID sourceGuid;
	BOOLEAN haveGuid = FALSE;

	ExAcquireFastMutex(&DriverExt->PreviewSessionMutex);
	session = CdpLookupPreviewSessionLocked(DriverExt, HandleId);
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

	CdpDestroyPreviewSession(DriverExt, session);

	if (haveGuid)
	{
		PCdp_DEVICE_EXTENSION sourceExt =
			CdpFindSourceExtensionByGuid(DriverExt, &sourceGuid);
		if (sourceExt)
		{
			if (sourceExt->Core)
				(void)CdpCorePreviewEnd(sourceExt->Core);
			InterlockedExchange(&sourceExt->Phase, (LONG)Cdp_PHASE_GENERAL);
		}
	}

	Cdp_DBG("[PREVIEW] end handle=%llu\n", HandleId);
	return STATUS_SUCCESS;
}

VOID CdpCloseAllPreviewSessions(_In_ PCdp_DRIVER_EXTENSION DriverExt)
{
	for (;;)
	{
		PCdp_PREVIEW_SESSION session = NULL;
		GUID sourceGuid;
		BOOLEAN haveGuid = FALSE;

		ExAcquireFastMutex(&DriverExt->PreviewSessionMutex);
		if (!IsListEmpty(&DriverExt->PreviewSessionList))
		{
			PLIST_ENTRY entry =
				RemoveHeadList(&DriverExt->PreviewSessionList);
			session = CONTAINING_RECORD(
				entry,
				Cdp_PREVIEW_SESSION,
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
			PCdp_DEVICE_EXTENSION sourceExt =
				CdpFindSourceExtensionByGuid(DriverExt, &sourceGuid);
			if (sourceExt)
			{
				if (sourceExt->Core)
					(void)CdpCorePreviewEnd(sourceExt->Core);
				InterlockedExchange(&sourceExt->Phase, (LONG)Cdp_PHASE_GENERAL);
			}
		}

		CdpDestroyPreviewSession(DriverExt, session);
	}
}

static NTSTATUS CdpReadPreviewSession(
	_In_ PCdp_DRIVER_EXTENSION DriverExt,
	_In_ const Cdp_PREVIEW_READ_REQUEST* Request,
	_Out_writes_bytes_(Request->ByteLength) PVOID OutputBuffer)
{
	PCdp_PREVIEW_SESSION session;
	PCdp_DEVICE_EXTENSION sourceExt = NULL;
	BOOLEAN historyLocked = FALSE;
	NTSTATUS status;

	if (!Request->ByteLength ||
		Request->ByteLength > Cdp_CMD3_MAX_READ_BYTES)
	{
		return STATUS_INVALID_PARAMETER;
	}

	session = CdpAcquirePreviewSession(
		DriverExt,
		Request->PreviewHandle);
	if (!session)
		return STATUS_INVALID_HANDLE;

	sourceExt = CdpFindSourceExtensionByGuid(
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

	status = CdpCoreReadAlignedView(
		sourceExt,
		Request->ByteOffset,
		Request->ByteLength,
		OutputBuffer);
	if (NT_SUCCESS(status))
	{
		Cdp_DBG("[PREVIEW] core read handle=%llu offset=%llu len=%lu\n",
			Request->PreviewHandle,
			Request->ByteOffset,
			Request->ByteLength);
	}

cleanup:
	if (historyLocked && sourceExt)
		KeReleaseMutex(&sourceExt->HistoryMutex, FALSE);
	CdpReleasePreviewSession(session);
	return status;
}

static NTSTATUS CdpSendToNextDevice(_In_ PDEVICE_OBJECT DeviceObject, _Inout_ PIRP Irp)
{
	IoSkipCurrentIrpStackLocation(Irp);
	return IoCallDriver(DeviceObject, Irp);
}

NTSTATUS CdpIrpDispatchDefault(_In_ PDEVICE_OBJECT DeviceObject, _Inout_ PIRP Irp)
{
	PCdp_DEVICE_EXTENSION DeviceExtension = (PCdp_DEVICE_EXTENSION)DeviceObject->DeviceExtension;

	if (DeviceExtension && DeviceExtension->LowerDeviceObject)
		return CdpSendToNextDevice(DeviceExtension->LowerDeviceObject, Irp);

	return CdpCompleteIrp(Irp, STATUS_SUCCESS, 0);
}

NTSTATUS CdpIrpDispatchCreateClose(_In_ PDEVICE_OBJECT DeviceObject, _Inout_ PIRP Irp)
{
	PCdp_DRIVER_EXTENSION driverExt =
		IoGetDriverObjectExtension(g_DriverObject, &g_DriverObject);
	PIO_STACK_LOCATION irpSp = IoGetCurrentIrpStackLocation(Irp);

	if (driverExt && DeviceObject == driverExt->ControlDevice)
	{
		if (irpSp->MajorFunction == IRP_MJ_CREATE)
		{
			PCdp_CONTROL_FILE_CONTEXT context =
				(PCdp_CONTROL_FILE_CONTEXT)cdpalloc(sizeof(*context));
			if (!context)
				return CdpCompleteIrp(Irp, STATUS_INSUFFICIENT_RESOURCES, 0);
			RtlZeroMemory(context, sizeof(*context));
			irpSp->FileObject->FsContext = context;
		}
		else if (irpSp->MajorFunction == IRP_MJ_CLOSE)
		{
			PCdp_CONTROL_FILE_CONTEXT context =
				(PCdp_CONTROL_FILE_CONTEXT)irpSp->FileObject->FsContext;
			irpSp->FileObject->FsContext = NULL;
			if (context)
			{
				RtlSecureZeroMemory(context, sizeof(*context));
				cdpfree(context);
			}
		}
		return CdpCompleteIrp(Irp, STATUS_SUCCESS, 0);
	}
	return CdpIrpDispatchDefault(DeviceObject, Irp);
}

static NTSTATUS CdpBeginRecovery(
	_In_ PCdp_DRIVER_EXTENSION DriverExt,
	_In_ const Cdp_RECOVERY_BEGIN_REQUEST* Request,
	_Out_ PCdp_RECOVERY_BEGIN_REPLY Reply)
{
	PCdp_DEVICE_EXTENSION sourceExt;
	PCdp_VOLUME_HANDLE_ENTRY journalEntry;
	UINT64 oldestTime = 0;
	UINT64 newestTime = 0;
	UINT64 targetTime = Request->TargetTime100ns;
	LONG previousPhase;
	NTSTATUS status;

	RtlZeroMemory(Reply, sizeof(*Reply));
	if ((Request->Flags & ~Cdp_RECOVERY_BEGIN_FLAG_ON_REBOOT) != 0)
		return STATUS_INVALID_PARAMETER;
	sourceExt = CdpFindSourceExtensionByGuid(
		DriverExt,
		&Request->SourceVolumeGuid);
	if (!sourceExt)
		return STATUS_DEVICE_DOES_NOT_EXIST;
	if (InterlockedCompareExchange(&sourceExt->CaptureEnabled, 0, 0) == 0)
		return STATUS_DEVICE_NOT_READY;
	if (!sourceExt->Core)
		return STATUS_DEVICE_NOT_READY;
	if ((Request->Flags & Cdp_RECOVERY_BEGIN_FLAG_ON_REBOOT) != 0)
	{
		status = CdpCoreQueryTimeRange(sourceExt->Core, &oldestTime, &newestTime);
		if (status == STATUS_NOT_FOUND)
		{
			oldestTime = 0;
			newestTime = 0;
			status = STATUS_SUCCESS;
		}
		if (!NT_SUCCESS(status))
			return status;
		if (oldestTime != 0 && targetTime < oldestTime)
			targetTime = oldestTime;
		journalEntry = CdpAcquireJournalForSource(DriverExt, sourceExt);
		if (!journalEntry)
			return STATUS_DEVICE_NOT_READY;
		status = CdpJournalSetRecoveryIntent(
			&journalEntry->Journal, targetTime);
		CdpReleaseVolumeHandleEntry(journalEntry);
		if (!NT_SUCCESS(status))
			return status;
		Reply->Phase = Cdp_PHASE_GENERAL;
		Reply->TargetTime100ns = targetTime;
		Reply->OldestRecoverable100ns = oldestTime;
		Reply->NewestRecoverable100ns = newestTime;
		Cdp_LOG("[RECOVERY] reboot intent persisted target=%llu; no begin before reboot\n",
			targetTime);
		return STATUS_SUCCESS;
	}
	if (CdpAnyPreviewSessionActive(DriverExt))
		return STATUS_INVALID_DEVICE_STATE;
	Cdp_LOG(
		"[RECOVERY] begin entering recovery pagingPathCount=%ld\n",
		InterlockedCompareExchange(&sourceExt->PagingPathCount, 0, 0));
	previousPhase = InterlockedCompareExchange(
		&sourceExt->Phase,
		(LONG)Cdp_PHASE_RECOVERY,
		(LONG)Cdp_PHASE_GENERAL);
	if (previousPhase != (LONG)Cdp_PHASE_GENERAL)
		return STATUS_INVALID_DEVICE_STATE;

	status = CdpCoreQueryTimeRange(
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
	else if (targetTime < oldestTime)
		targetTime = oldestTime;

	KeWaitForSingleObject(
		&sourceExt->HistoryMutex,
		Executive,
		KernelMode,
		FALSE,
		NULL);
	status = CdpCoreRecoveryBegin(sourceExt->Core, targetTime);
	KeReleaseMutex(&sourceExt->HistoryMutex, FALSE);
	if (!NT_SUCCESS(status))
	{
		InterlockedExchange(&sourceExt->Phase, previousPhase);
		return status;
	}

	Reply->Phase = Cdp_PHASE_RECOVERY;
	targetTime = CdpCoreGetTargetTime100ns(sourceExt->Core);
	Reply->TargetTime100ns = targetTime;
	Reply->OldestRecoverable100ns = oldestTime;
	Reply->NewestRecoverable100ns = newestTime;
	Cdp_LOG("[PHASE] recovery prepared target=%llu; waiting for commit\n",
		targetTime);
	return STATUS_SUCCESS;
}

static NTSTATUS CdpCommitRecovery(
	_In_ PCdp_DRIVER_EXTENSION DriverExt,
	_In_ const Cdp_RECOVERY_CONTROL_REQUEST* Request,
	_Out_ PCdp_RECOVERY_COMMIT_REPLY Reply)
{
	PCdp_DEVICE_EXTENSION sourceExt;
	UINT64 targetTime;
	NTSTATUS status;
	BOOLEAN complete = FALSE;

	RtlZeroMemory(Reply, sizeof(*Reply));
	sourceExt = CdpFindSourceExtensionByGuid(
		DriverExt,
		&Request->SourceVolumeGuid);
	if (!sourceExt)
		return STATUS_DEVICE_DOES_NOT_EXIST;
	if (!sourceExt->Core ||
		InterlockedCompareExchange(&sourceExt->Phase, 0, 0) !=
			(LONG)Cdp_PHASE_RECOVERY)
	{
		return STATUS_INVALID_DEVICE_STATE;
	}

	for (;;)
	{
		KeWaitForSingleObject(
			&sourceExt->HistoryMutex,
			Executive,
			KernelMode,
			FALSE,
			NULL);
		targetTime = CdpCoreGetTargetTime100ns(sourceExt->Core);
		status = CdpCoreRecoveryCommitStep(sourceExt->Core, &complete);
		KeReleaseMutex(&sourceExt->HistoryMutex, FALSE);
		if (!NT_SUCCESS(status))
		{
			Cdp_LOG("[PHASE] recovery commit failed target=%llu status=0x%08X\n",
				targetTime,
				status);
			return status;
		}
		if (complete)
			break;
		// A lock handoff between nodes gives queued normal writes a chance to
		// capture and punch the remaining history before the next step.
		YieldProcessor();
	}

	{
		PCdp_VOLUME_HANDLE_ENTRY journalEntry =
			CdpAcquireJournalForSource(DriverExt, sourceExt);
		if (journalEntry && journalEntry->Journal.RecoveryPending)
		{
			status = CdpJournalClearRecoveryIntent(&journalEntry->Journal);
			CdpReleaseVolumeHandleEntry(journalEntry);
			if (!NT_SUCCESS(status))
				return status;
		}
		else if (journalEntry)
		{
			CdpReleaseVolumeHandleEntry(journalEntry);
		}
	}
	InterlockedExchange(&sourceExt->Phase, (LONG)Cdp_PHASE_GENERAL);
	Reply->Phase = Cdp_PHASE_GENERAL;
	Reply->TargetTime100ns = targetTime;
	Cdp_LOG("[PHASE] recovery commit complete target=%llu -> normal\n",
		targetTime);
	return STATUS_SUCCESS;
}

static NTSTATUS CdpCancelRecovery(
	_In_ PCdp_DRIVER_EXTENSION DriverExt,
	_In_ const Cdp_RECOVERY_CONTROL_REQUEST* Request)
{
	PCdp_DEVICE_EXTENSION sourceExt;
	PCdp_VOLUME_HANDLE_ENTRY journalEntry;
	LONG phase;
	NTSTATUS status;

	sourceExt = CdpFindSourceExtensionByGuid(
		DriverExt,
		&Request->SourceVolumeGuid);
	if (!sourceExt)
		return STATUS_DEVICE_DOES_NOT_EXIST;
	phase = InterlockedCompareExchange(&sourceExt->Phase, 0, 0);
	if (phase != (LONG)Cdp_PHASE_RECOVERY)
	{
		// A reboot Recovery request only persists the intent and deliberately
		// leaves the source in General phase.  CMD 'c' must be able to cancel
		// that pending intent before reboot.
		if (phase == (LONG)Cdp_PHASE_GENERAL)
		{
			journalEntry = CdpAcquireJournalForSource(DriverExt, sourceExt);
			if (journalEntry && journalEntry->Journal.RecoveryPending)
			{
				status = CdpJournalClearRecoveryIntent(&journalEntry->Journal);
				CdpReleaseVolumeHandleEntry(journalEntry);
				if (NT_SUCCESS(status))
					Cdp_LOG("[RECOVERY] pending reboot intent cancelled in General phase\n");
				return status;
			}
			if (journalEntry)
				CdpReleaseVolumeHandleEntry(journalEntry);
		}
		return STATUS_INVALID_DEVICE_STATE;
	}
	if (!sourceExt->Core)
		return STATUS_DEVICE_NOT_READY;

	KeWaitForSingleObject(
		&sourceExt->HistoryMutex,
		Executive,
		KernelMode,
		FALSE,
		NULL);
	status = CdpCoreRecoveryCancel(sourceExt->Core);
	KeReleaseMutex(&sourceExt->HistoryMutex, FALSE);
	if (!NT_SUCCESS(status))
		return status;

	InterlockedExchange(&sourceExt->Phase, (LONG)Cdp_PHASE_GENERAL);
	journalEntry = CdpAcquireJournalForSource(DriverExt, sourceExt);
	if (journalEntry && journalEntry->Journal.RecoveryPending)
	{
		status = CdpJournalClearRecoveryIntent(&journalEntry->Journal);
		CdpReleaseVolumeHandleEntry(journalEntry);
		if (!NT_SUCCESS(status))
			return status;
	}
	else if (journalEntry)
	{
		CdpReleaseVolumeHandleEntry(journalEntry);
	}
	Cdp_LOG("[PHASE] recovery cancelled -> normal\n");
	return STATUS_SUCCESS;
}

static NTSTATUS CdpQueryTimeRange(
	_In_ PCdp_DRIVER_EXTENSION DriverExt,
	_In_ const Cdp_TIME_RANGE_QUERY_REQUEST* Request,
	_Out_ PCdp_TIME_RANGE_QUERY_REPLY Reply)
{
	PCdp_DEVICE_EXTENSION sourceExt;
	NTSTATUS status;

	RtlZeroMemory(Reply, sizeof(*Reply));
	sourceExt = CdpFindSourceExtensionByGuid(
		DriverExt,
		&Request->SourceVolumeGuid);
	if (!sourceExt)
		return STATUS_DEVICE_DOES_NOT_EXIST;
	if (InterlockedCompareExchange(&sourceExt->CaptureEnabled, 0, 0) == 0)
		return STATUS_DEVICE_NOT_READY;
	if (!sourceExt->Core)
		return STATUS_DEVICE_NOT_READY;

	status = CdpCoreQueryTimeRange(
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

static NTSTATUS CdpQueryJournalUsage(
	_In_ PCdp_DRIVER_EXTENSION DriverExt,
	_In_ const Cdp_JOURNAL_USAGE_QUERY_REQUEST* Request,
	_Out_ PCdp_JOURNAL_USAGE_QUERY_REPLY Reply)
{
	PCdp_DEVICE_EXTENSION sourceExt;

	RtlZeroMemory(Reply, sizeof(*Reply));
	sourceExt = CdpFindSourceExtensionByGuid(
		DriverExt,
		&Request->SourceVolumeGuid);
	if (!sourceExt)
		return STATUS_DEVICE_DOES_NOT_EXIST;
	if (InterlockedCompareExchange(&sourceExt->CaptureEnabled, 0, 0) == 0 ||
		!sourceExt->Core)
	{
		return STATUS_DEVICE_NOT_READY;
	}

	return CdpCoreQueryJournalUsage(
		sourceExt->Core,
		&Reply->JournalPartitionBytes,
		&Reply->JournalMetadataBytes,
		&Reply->RecordPayloadBytesUsed,
		&Reply->RecordPayloadBytesFree,
		&Reply->TotalRecords);
}

static NTSTATUS CdpQueryJournalRecords(
	_In_ PCdp_DRIVER_EXTENSION DriverExt,
	_In_ const Cdp_JOURNAL_RECORD_QUERY_REQUEST* Request,
	_Out_ PCdp_JOURNAL_RECORD_QUERY_REPLY Reply,
	_In_ ULONG RecordCapacity)
{
	PCdp_DEVICE_EXTENSION sourceExt;
	PCdp_JOURNAL_RECORD records;

	C_ASSERT(sizeof(Cdp_JOURNAL_RECORD_INFO) ==
		sizeof(Cdp_JOURNAL_RECORD));
	RtlZeroMemory(Reply, sizeof(*Reply));
	sourceExt = CdpFindSourceExtensionByGuid(
		DriverExt,
		&Request->SourceVolumeGuid);
	if (!sourceExt)
		return STATUS_DEVICE_DOES_NOT_EXIST;
	if (InterlockedCompareExchange(&sourceExt->CaptureEnabled, 0, 0) == 0 ||
		!sourceExt->Core)
	{
		return STATUS_DEVICE_NOT_READY;
	}

	if (RecordCapacity > Request->MaxRecords)
		RecordCapacity = Request->MaxRecords;
	if (RecordCapacity > Cdp_JOURNAL_RECORD_QUERY_MAX_PER_CALL)
		RecordCapacity = Cdp_JOURNAL_RECORD_QUERY_MAX_PER_CALL;

	records = (PCdp_JOURNAL_RECORD)(Reply + 1);
	return CdpCoreQueryRecordHeaders(
		sourceExt->Core,
		Request->StartIndex,
		Request->ExpectedGeneration,
		records,
		RecordCapacity,
		&Reply->TotalRecords,
		&Reply->Generation,
		&Reply->RecordCount);
}

static NTSTATUS CdpQueryPhase(
	_In_ PCdp_DRIVER_EXTENSION DriverExt,
	_In_ const Cdp_PHASE_QUERY_REQUEST* Request,
	_Out_ PCdp_PHASE_QUERY_REPLY Reply)
{
	PCdp_DEVICE_EXTENSION sourceExt;
	PCdp_VOLUME_HANDLE_ENTRY journalEntry = NULL;
	UINT64 journalHandleId = 0;

	RtlZeroMemory(Reply, sizeof(*Reply));
	Reply->Status = Cdp_STATUS_UNPROTECTED;

	sourceExt = CdpFindSourceExtensionByGuid(
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
			CdpCoreGetTargetTime100ns(sourceExt->Core);

	ExAcquireFastMutex(&DriverExt->VolumeHandleMutex);
	journalHandleId = sourceExt->JournalHandleId;
	if (journalHandleId != 0)
		journalEntry = CdpLookupVolumeHandleLocked(DriverExt, journalHandleId);
	if (journalEntry && journalEntry->VolumeGuidValid)
		Reply->JournalPartitionGuid = journalEntry->VolumeGuid;
	ExReleaseFastMutex(&DriverExt->VolumeHandleMutex);
	return STATUS_SUCCESS;
}

/* The caller holds HistoryMutex.  CdpCore operates on sector-aligned ranges,
 * while preview/recovery clients may request an arbitrary byte subrange. */
static NTSTATUS CdpCoreReadAlignedView(
	_In_ PCdp_DEVICE_EXTENSION DevExt,
	_In_ UINT64 Offset,
	_In_ ULONG Length,
	_Out_writes_bytes_(Length) PVOID Buffer)
{
	NTSTATUS status = STATUS_SUCCESS;
	ULONG completed = 0;
	ULONG sectorSize;

	if (!DevExt->Core)
		return STATUS_DEVICE_NOT_READY;
	sectorSize = DevExt->SectorSize;
	if (sectorSize != 512 && sectorSize != 4096)
		return STATUS_INVALID_DEVICE_REQUEST;

	while (completed < Length)
	{
		UINT64 requestOffset = Offset + completed;
		UINT64 alignedOffset = requestOffset - (requestOffset % sectorSize);
		ULONG prefix = (ULONG)(requestOffset - alignedOffset);
		ULONG chunk = Length - completed;
		ULONG span;
		PVOID alignedBuffer;
		if (chunk > Cdp_CMD3_MAX_READ_BYTES - prefix)
			chunk = Cdp_CMD3_MAX_READ_BYTES - prefix;
		span = prefix + chunk;
		span = (span + sectorSize - 1) / sectorSize * sectorSize;
		alignedBuffer = cdpalloc(span);
		if (!alignedBuffer)
		{
			status = STATUS_INSUFFICIENT_RESOURCES;
			break;
		}
		status = CdpCoreRead(
			DevExt->Core,
			alignedOffset,
			span,
			alignedBuffer);
		if (NT_SUCCESS(status))
			RtlCopyMemory((PUCHAR)Buffer + completed,
				(PUCHAR)alignedBuffer + prefix, chunk);
		cdpfree(alignedBuffer);
		if (!NT_SUCCESS(status))
			break;
		completed += chunk;
	}
	return status;
}

static NTSTATUS CdpRecoveryFillReadBuffer(
	_In_ PCdp_DEVICE_EXTENSION DevExt,
	_In_ UINT64 Offset,
	_In_ ULONG Length,
	_Out_writes_bytes_(Length) PVOID Buffer)
{
	NTSTATUS status;

	if (!DevExt->Core)
		return STATUS_DEVICE_NOT_READY;

	Cdp_DBG(
		"[RECOVERY] read mutex wait offset=%llu len=%lu\n",
		Offset,
		Length);
	KeWaitForSingleObject(
		&DevExt->HistoryMutex,
		Executive,
		KernelMode,
		FALSE,
		NULL);
	Cdp_DBG(
		"[RECOVERY] read mutex acquired offset=%llu len=%lu\n",
		Offset,
		Length);
	status = CdpCoreReadAlignedView(DevExt, Offset, Length, Buffer);
	Cdp_DBG(
		"[RECOVERY] core read end offset=%llu len=%lu status=0x%08X\n",
		Offset,
		Length,
		status);
	KeReleaseMutex(&DevExt->HistoryMutex, FALSE);
	Cdp_DBG(
		"[RECOVERY] read mutex released offset=%llu len=%lu\n",
		Offset,
		Length);
	return status;
}

NTSTATUS CdpIrpDispatchRead(_In_ PDEVICE_OBJECT DeviceObject, _Inout_ PIRP Irp)
{
	PCdp_DEVICE_EXTENSION deviceExt =
		(PCdp_DEVICE_EXTENSION)DeviceObject->DeviceExtension;
	PCdp_DRIVER_EXTENSION driverExt =
		IoGetDriverObjectExtension(g_DriverObject, &g_DriverObject);
	PIO_STACK_LOCATION irpSp;
	UINT64 offset;
	ULONG length;
	PCdp_RECOVERY_READ_ITEM item;
	KIRQL oldIrql;

	if (!deviceExt || !deviceExt->LowerDeviceObject)
		return CdpCompleteIrp(Irp, STATUS_INVALID_DEVICE_REQUEST, 0);

	if (InterlockedCompareExchange(&deviceExt->Phase, 0, 0) !=
		(LONG)Cdp_PHASE_RECOVERY &&
		InterlockedCompareExchange(&deviceExt->AutoDiscoveryGateActive, 0, 0) == 0)
	{
		return CdpSendToNextDevice(deviceExt->LowerDeviceObject, Irp);
	}

	irpSp = IoGetCurrentIrpStackLocation(Irp);
	if (irpSp->Parameters.Read.ByteOffset.QuadPart < 0 ||
		irpSp->Parameters.Read.Length == 0)
	{
		return CdpSendToNextDevice(deviceExt->LowerDeviceObject, Irp);
	}

	offset = (UINT64)irpSp->Parameters.Read.ByteOffset.QuadPart;
	length = irpSp->Parameters.Read.Length;
	if (!driverExt ||
		((deviceExt->SectorSize != 512 && deviceExt->SectorSize != 4096) &&
			InterlockedCompareExchange(
				&deviceExt->AutoDiscoveryGateActive, 0, 0) == 0))
	{
		return CdpSendToNextDevice(deviceExt->LowerDeviceObject, Irp);
	}

	item = (PCdp_RECOVERY_READ_ITEM)cdpalloc(sizeof(*item));
	if (!item)
		return CdpCompleteIrp(Irp, STATUS_INSUFFICIENT_RESOURCES, 0);
	item->Irp = Irp;

	KeAcquireSpinLock(&deviceExt->RecoveryReadQueueLock, &oldIrql);
	if (InterlockedCompareExchange(&deviceExt->RecoveryReadStopping, 0, 0) == 0 &&
		(InterlockedCompareExchange(&deviceExt->Phase, 0, 0) ==
			(LONG)Cdp_PHASE_RECOVERY ||
			InterlockedCompareExchange(
				&deviceExt->AutoDiscoveryGateActive, 0, 0) != 0))
	{
		IoMarkIrpPending(Irp);
		InsertTailList(&deviceExt->RecoveryReadQueue, &item->Entry);
		KeSetEvent(&deviceExt->RecoveryReadEvent, IO_NO_INCREMENT, FALSE);
		KeReleaseSpinLock(&deviceExt->RecoveryReadQueueLock, oldIrql);
		Cdp_DBG(
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
	cdpfree(item);
	return CdpSendToNextDevice(deviceExt->LowerDeviceObject, Irp);
}

static VOID CdpForwardQueuedRead(
	_In_ PCdp_DEVICE_EXTENSION DevExt,
	_Inout_ PIRP Irp)
{
	IoSkipCurrentIrpStackLocation(Irp);
	(void)IoCallDriver(DevExt->LowerDeviceObject, Irp);
}

static VOID CdpRecoveryReadWorker(_In_ PVOID Context)
{
	PCdp_DEVICE_EXTENSION devExt = (PCdp_DEVICE_EXTENSION)Context;

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
				PCdp_RECOVERY_READ_ITEM item = CONTAINING_RECORD(
					entry,
					Cdp_RECOVERY_READ_ITEM,
					Entry);
				PIRP irp = item->Irp;
				PIO_STACK_LOCATION irpSp = IoGetCurrentIrpStackLocation(irp);
				UINT64 offset = (UINT64)irpSp->Parameters.Read.ByteOffset.QuadPart;
				ULONG length = irpSp->Parameters.Read.Length;
				if (InterlockedCompareExchange(
						&devExt->AutoDiscoveryGateActive, 0, 0) != 0)
				{
					Cdp_LOG("[AUTO-CDP] read held until source discovery/begin irp=%p offset=%llu len=%lu\n",
						irp, offset, length);
					KeWaitForSingleObject(
						&devExt->AutoDiscoveryGateEvent,
						Executive,
						KernelMode,
						FALSE,
						NULL);
				}

				if (InterlockedCompareExchange(
						&devExt->RecoveryReadStopping, 0, 0) != 0 ||
					InterlockedCompareExchange(&devExt->Phase, 0, 0) !=
						(LONG)Cdp_PHASE_RECOVERY ||
					!devExt->Core)
				{
					Cdp_DBG("[RECOVERY] queued read forwarded irp=%p\n", irp);
					CdpForwardQueuedRead(devExt, irp);
				}
				else
				{
					PVOID buffer = MmGetSystemAddressForMdlSafe(
						irp->MdlAddress,
						NormalPagePriority);
					NTSTATUS status = buffer ?
						CdpRecoveryFillReadBuffer(
							devExt,
							offset,
							length,
							buffer) :
						STATUS_INSUFFICIENT_RESOURCES;

					Cdp_DBG(
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
					CdpCompleteIrp(
						irp,
						status,
						NT_SUCCESS(status) ? length : 0);
				}
				cdpfree(item);
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

NTSTATUS CdpStartRecoveryReadWorker(_Inout_ PCdp_DEVICE_EXTENSION DevExt)
{
	InterlockedExchange(&DevExt->RecoveryReadStopping, 0);
	return PsCreateSystemThread(
		&DevExt->RecoveryReadThreadHandle,
		THREAD_ALL_ACCESS,
		NULL,
		NULL,
		NULL,
		CdpRecoveryReadWorker,
		DevExt);
}

VOID CdpStopRecoveryReadWorker(_Inout_ PCdp_DEVICE_EXTENSION DevExt)
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

static NTSTATUS CdpCaptureBeforeImage(
	_In_ PCdp_DEVICE_EXTENSION SourceExt,
	_In_ PIRP Irp,
	_In_ PCdp_DRIVER_EXTENSION DriverExt)
{
	PIO_STACK_LOCATION irpSp = IoGetCurrentIrpStackLocation(Irp);
	UINT64 offset = (UINT64)irpSp->Parameters.Write.ByteOffset.QuadPart;
	ULONG remaining = irpSp->Parameters.Write.Length;
	NTSTATUS status = STATUS_SUCCESS;
	Cdp_JOURNAL_RECORD writtenRecord;
	BOOLEAN seqLogged = FALSE;

	UNREFERENCED_PARAMETER(DriverExt);

	if (irpSp->Parameters.Write.ByteOffset.QuadPart < 0 || remaining == 0)
		return STATUS_INVALID_PARAMETER;
	if (!SourceExt->Core)
		return STATUS_DEVICE_NOT_READY;

	Cdp_DBG("[COW-TRACE] capture begin irp=%p offset=%llu len=%lu\n",
		Irp, offset, remaining);
	Cdp_DBG("[COW] capture begin offset=%llu len=%lu\n", offset, remaining);

	while (remaining)
	{
		ULONG chunk = remaining > Cdp_JOURNAL_MAX_RECORD_DATA ?
			Cdp_JOURNAL_MAX_RECORD_DATA : remaining;

		status = CdpCoreCaptureAppend(SourceExt->Core, offset, chunk, &writtenRecord);
		if (!NT_SUCCESS(status))
		{
			Cdp_LOG("[COW] core capture failed status=0x%08X offset=%llu len=%lu\n",
				status, offset, chunk);
			break;
		}
		if (!seqLogged)
		{
			// Print journal record Sequence once per before-image capture.
			Cdp_DBG("[COW] journal seq=%llu offset=%llu len=%lu\n",
				writtenRecord.Sequence, offset, chunk);
			seqLogged = TRUE;
		}
		Cdp_DBG("[COW] core capture ok offset=%llu len=%lu\n", offset, chunk);
		offset += chunk;
		remaining -= chunk;
	}

	Cdp_DBG("[COW-TRACE] capture end irp=%p status=0x%08X remaining=%lu\n",
		Irp, status, remaining);
	return status;
}

static NTSTATUS CdpForwardWriteCompletion(
	_In_ PDEVICE_OBJECT DeviceObject,
	_In_ PIRP Irp,
	_In_ PVOID Context)
{
	UNREFERENCED_PARAMETER(DeviceObject);
	UNREFERENCED_PARAMETER(Irp);
	KeSetEvent((PKEVENT)Context, IO_NO_INCREMENT, FALSE);
	return STATUS_MORE_PROCESSING_REQUIRED;
}

static VOID CdpForwardQueuedWriteSynchronously(
	_In_ PCdp_DEVICE_EXTENSION DevExt,
	_Inout_ PIRP Irp)
{
	KEVENT event;

	KeInitializeEvent(&event, NotificationEvent, FALSE);
	IoCopyCurrentIrpStackLocationToNext(Irp);
	IoSetCompletionRoutine(
		Irp,
		CdpForwardWriteCompletion,
		&event,
		TRUE,
		TRUE,
		TRUE);
	(void)IoCallDriver(DevExt->LowerDeviceObject, Irp);
	KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, NULL);
	Cdp_DBG("[COW] original write completing irp=%p status=0x%08X bytes=%Iu\n",
		Irp,
		Irp->IoStatus.Status,
		Irp->IoStatus.Information);
	IoCompleteRequest(Irp, IO_NO_INCREMENT);
}

static VOID CdpCaptureWorker(_In_ PVOID Context)
{
	PCdp_DEVICE_EXTENSION devExt = (PCdp_DEVICE_EXTENSION)Context;
	PCdp_DRIVER_EXTENSION driverExt =
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
				PCdp_CAPTURE_ITEM item =
					CONTAINING_RECORD(entry, Cdp_CAPTURE_ITEM, Entry);
				PIO_STACK_LOCATION irpSp = IoGetCurrentIrpStackLocation(item->Irp);

				if (InterlockedCompareExchange(
						&devExt->AutoDiscoveryGateActive, 0, 0) != 0)
				{
					Cdp_LOG("[AUTO-CDP] write held until source discovery/begin irp=%p offset=%lld len=%lu\n",
						item->Irp,
						irpSp->Parameters.Write.ByteOffset.QuadPart,
						irpSp->Parameters.Write.Length);
					KeWaitForSingleObject(
						&devExt->AutoDiscoveryGateEvent,
						Executive,
						KernelMode,
						FALSE,
						NULL);
				}

				// Capture before-image then apply the original write under one
				// HistoryMutex so preview cannot observe a torn timeline.
				Cdp_DBG("[COW] worker mutex wait irp=%p\n", item->Irp);
				KeWaitForSingleObject(
					&devExt->HistoryMutex,
					Executive,
					KernelMode,
					FALSE,
					NULL);
				Cdp_DBG("[COW] worker mutex acquired irp=%p\n", item->Irp);
				if (!devExt->CaptureStopping &&
					InterlockedCompareExchange(&devExt->CaptureEnabled, 0, 0) != 0 &&
					driverExt)
				{
					NTSTATUS captureStatus =
						CdpCaptureBeforeImage(devExt, item->Irp, driverExt);
					Cdp_DBG("[COW] worker capture status=0x%08X irp=%p\n",
						captureStatus, item->Irp);
					UNREFERENCED_PARAMETER(captureStatus);
				}
				else
				{
					Cdp_DBG("[COW-TRACE] worker bypass irp=%p offset=%lld len=%lu enabled=%ld stopping=%ld driver=%p\n",
						item->Irp,
						irpSp->Parameters.Write.ByteOffset.QuadPart,
						irpSp->Parameters.Write.Length,
						InterlockedCompareExchange(&devExt->CaptureEnabled, 0, 0),
						InterlockedCompareExchange(&devExt->CaptureStopping, 0, 0),
						driverExt);
				}
				CdpForwardQueuedWriteSynchronously(devExt, item->Irp);
				KeReleaseMutex(&devExt->HistoryMutex, FALSE);
				Cdp_DBG("[COW] worker mutex released irp=%p\n", item->Irp);
				cdpfree(item);
			}
		}
		if (InterlockedCompareExchange(&devExt->CaptureStopping, 0, 0) != 0)
			break;
	}
	PsTerminateSystemThread(STATUS_SUCCESS);
}

NTSTATUS CdpStartCaptureWorker(_Inout_ PCdp_DEVICE_EXTENSION DevExt)
{
	InterlockedExchange(&DevExt->CaptureStopping, 0);
	return PsCreateSystemThread(
		&DevExt->CaptureThreadHandle,
		THREAD_ALL_ACCESS,
		NULL,
		NULL,
		NULL,
		CdpCaptureWorker,
		DevExt);
}

VOID CdpStopCaptureWorker(_Inout_ PCdp_DEVICE_EXTENSION DevExt)
{
	HANDLE threadHandle = DevExt->CaptureThreadHandle;
	PVOID threadObject = NULL;

	if (!threadHandle)
	{
		InterlockedExchange(&DevExt->CaptureStopping, 0);
		return;
	}
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
	// The worker has fully exited and no queued write can still reference the
	// stopping state. Clear it so ordinary writes after CMD2 do not continue
	// producing misleading "write seen/write bypass" trace messages.
	InterlockedExchange(&DevExt->CaptureStopping, 0);
}

VOID CdpDisableAndDestroyCapture(_Inout_ PCdp_DEVICE_EXTENSION DevExt)
{
	PCdp_CORE core;

	if (!DevExt)
		return;
	CdpOpenAutoDiscoveryGate(DevExt);

	InterlockedExchange(&DevExt->CaptureEnabled, 0);
	InterlockedExchange(&DevExt->Phase, (LONG)Cdp_PHASE_GENERAL);
	CdpStopRecoveryReadWorker(DevExt);
	CdpStopCaptureWorker(DevExt);

	// The capture worker holds HistoryMutex whenever it can access Core.  Take
	// ownership under that mutex only after the worker has stopped, then free
	// Core outside every spin lock and outside the mutex.
	KeWaitForSingleObject(&DevExt->HistoryMutex,
		Executive, KernelMode, FALSE, NULL);
	core = DevExt->Core;
	DevExt->Core = NULL;
	KeReleaseMutex(&DevExt->HistoryMutex, FALSE);

	if (core)
		CdpCoreDestroy(core);
}

NTSTATUS CdpIrpDispatchWrite(_In_ PDEVICE_OBJECT DeviceObject, _Inout_ PIRP Irp)
{
	PCdp_DEVICE_EXTENSION deviceExt = (PCdp_DEVICE_EXTENSION)DeviceObject->DeviceExtension;
	PCdp_DRIVER_EXTENSION driverExt = IoGetDriverObjectExtension(g_DriverObject, &g_DriverObject);
	BOOLEAN traceCaptureWrite = FALSE;
	PIO_STACK_LOCATION irpSp = IoGetCurrentIrpStackLocation(Irp);

	if (deviceExt)
	{
		traceCaptureWrite =
			InterlockedCompareExchange(&deviceExt->CaptureEnabled, 0, 0) != 0 ||
			InterlockedCompareExchange(&deviceExt->CaptureStopping, 0, 0) != 0 ||
			deviceExt->Core != NULL ||
			deviceExt->JournalHandleId != 0;
		if (traceCaptureWrite)
		{
			Cdp_DBG("[COW-TRACE] write seen irp=%p offset=%lld len=%lu flags=0x%08lX enabled=%ld stopping=%ld phase=%ld core=%p journal=%llu\n",
				Irp,
				irpSp->Parameters.Write.ByteOffset.QuadPart,
				irpSp->Parameters.Write.Length,
				Irp->Flags,
				InterlockedCompareExchange(&deviceExt->CaptureEnabled, 0, 0),
				InterlockedCompareExchange(&deviceExt->CaptureStopping, 0, 0),
				InterlockedCompareExchange(&deviceExt->Phase, 0, 0),
				deviceExt->Core,
				deviceExt->JournalHandleId);
		}
	}

	if (deviceExt && deviceExt->LowerDeviceObject && driverExt &&
		(InterlockedCompareExchange(&deviceExt->AutoDiscoveryGateActive, 0, 0) != 0 ||
			(InterlockedCompareExchange(&deviceExt->CaptureEnabled, 0, 0) != 0 &&
				InterlockedCompareExchange(&deviceExt->CaptureStopping, 0, 0) == 0)))
	{
		PCdp_CAPTURE_ITEM item = (PCdp_CAPTURE_ITEM)cdpalloc(sizeof(*item));
		KIRQL oldIrql;
		if (item)
		{
			item->Irp = Irp;
			KeAcquireSpinLock(&deviceExt->CaptureQueueLock, &oldIrql);
			if (InterlockedCompareExchange(&deviceExt->CaptureStopping, 0, 0) == 0 &&
				(InterlockedCompareExchange(&deviceExt->AutoDiscoveryGateActive, 0, 0) != 0 ||
					InterlockedCompareExchange(&deviceExt->CaptureEnabled, 0, 0) != 0))
			{
				IoMarkIrpPending(Irp);
				InsertTailList(&deviceExt->CaptureQueue, &item->Entry);
				KeSetEvent(&deviceExt->CaptureEvent, IO_NO_INCREMENT, FALSE);
				KeReleaseSpinLock(&deviceExt->CaptureQueueLock, oldIrql);
				Cdp_DBG("[COW-TRACE] write queued irp=%p offset=%lld len=%lu\n",
					Irp,
					irpSp->Parameters.Write.ByteOffset.QuadPart,
					irpSp->Parameters.Write.Length);
				return STATUS_PENDING;
			}
			KeReleaseSpinLock(&deviceExt->CaptureQueueLock, oldIrql);
			cdpfree(item);
		}
		else
		{
			if (InterlockedCompareExchange(
						&deviceExt->AutoDiscoveryGateActive, 0, 0) != 0)
			{
				Cdp_LOG("[AUTO-CDP] gate queue allocation failed; write blocked irp=%p offset=%lld len=%lu\n",
					Irp,
					irpSp->Parameters.Write.ByteOffset.QuadPart,
					irpSp->Parameters.Write.Length);
				return CdpCompleteIrp(Irp, STATUS_INSUFFICIENT_RESOURCES, 0);
			}
			Cdp_DBG("[COW-TRACE] queue allocation failed; write bypass irp=%p offset=%lld len=%lu\n",
				Irp,
				irpSp->Parameters.Write.ByteOffset.QuadPart,
				irpSp->Parameters.Write.Length);
		}
	}
	else if (traceCaptureWrite)
	{
		Cdp_DBG("[COW-TRACE] write bypass irp=%p offset=%lld len=%lu enabled=%ld stopping=%ld lower=%p driver=%p\n",
			Irp,
			irpSp->Parameters.Write.ByteOffset.QuadPart,
			irpSp->Parameters.Write.Length,
			InterlockedCompareExchange(&deviceExt->CaptureEnabled, 0, 0),
			InterlockedCompareExchange(&deviceExt->CaptureStopping, 0, 0),
			deviceExt->LowerDeviceObject,
			driverExt);
	}

	if (deviceExt && deviceExt->LowerDeviceObject)
		return CdpSendToNextDevice(deviceExt->LowerDeviceObject, Irp);

	return CdpCompleteIrp(Irp, STATUS_INVALID_DEVICE_REQUEST, 0);
}

static NTSTATUS PnpCompletionRoutine(PDEVICE_OBJECT DeviceObject, PIRP Irp, PVOID Context)
{
	UNREFERENCED_PARAMETER(DeviceObject);
	UNREFERENCED_PARAMETER(Irp);
	KeSetEvent((PKEVENT)Context, IO_NO_INCREMENT, FALSE);
	return STATUS_MORE_PROCESSING_REQUIRED;
}

static NTSTATUS CdpStartDeviceCompletionRoutine(
	_In_ PDEVICE_OBJECT DeviceObject,
	_In_ PIRP Irp,
	_In_ PVOID Context)
{
	PCdp_DEVICE_EXTENSION devExt = (PCdp_DEVICE_EXTENSION)Context;
	PCdp_DRIVER_EXTENSION driverExt;

	if (devExt && NT_SUCCESS(Irp->IoStatus.Status))
	{
		InterlockedExchange(&devExt->Started, 1);
		InterlockedExchange(&devExt->AutoKind, 0);
		devExt->VolumeGuidValid = FALSE;
#if DBG
		Cdp_DBG("[AUTO-CDP] volume started filter=%p lower=%p\n",
			DeviceObject,
			devExt->LowerDeviceObject);
#else
		UNREFERENCED_PARAMETER(DeviceObject);
#endif
		driverExt = IoGetDriverObjectExtension(g_DriverObject, &g_DriverObject);
		// Completion may run at DISPATCH_LEVEL: no FastMutex / waits here.
		if (driverExt &&
			InterlockedCompareExchange(
				&driverExt->AutoDiscoverySuppressed, 0, 0) == 0)
		{
			if (InterlockedCompareExchange(
				&driverExt->AutoDiscoverySettled, 0, 1) == 1)
			{
				KeClearEvent(&driverExt->AutoDiscoverySettledEvent);
			}
			CdpQueueAutoDiscovery(driverExt);
		}
	}
	return STATUS_CONTINUE_COMPLETION;
}

NTSTATUS CdpIrpDispatchPnp(_In_ PDEVICE_OBJECT DeviceObject, _Inout_ PIRP Irp)
{
	PCdp_DEVICE_EXTENSION DevExt = DeviceObject->DeviceExtension;
	PIO_STACK_LOCATION IrpSp = IoGetCurrentIrpStackLocation(Irp);
	PCdp_DRIVER_EXTENSION DriverExt = NULL;

	if (!DevExt)
		return CdpIrpDispatchDefault(DeviceObject, Irp);

	DriverExt = IoGetDriverObjectExtension(g_DriverObject, &g_DriverObject);
	if (!DriverExt)
		return CdpSendToNextDevice(DevExt->LowerDeviceObject, Irp);

	switch (IrpSp->MinorFunction)
	{
	case IRP_MN_START_DEVICE:
		IoCopyCurrentIrpStackLocationToNext(Irp);
		IoSetCompletionRoutine(
			Irp,
			CdpStartDeviceCompletionRoutine,
			DevExt,
			TRUE,
			TRUE,
			TRUE);
		return IoCallDriver(DevExt->LowerDeviceObject, Irp);

	case IRP_MN_REMOVE_DEVICE:
	{
		KIRQL OldIrql;
		PCdp_DEVICE_LIST_NODE NodeToFree = NULL;
		PDEVICE_OBJECT LowerDevice = NULL;
		NTSTATUS Status;

		KeAcquireSpinLock(&DriverExt->DeviceObjectListLock, &OldIrql);
		PLIST_ENTRY PEntry = DriverExt->DeviceObjectListHead.Flink;
		while (PEntry != &DriverExt->DeviceObjectListHead)
		{
			PCdp_DEVICE_LIST_NODE Node = CONTAINING_RECORD(PEntry, Cdp_DEVICE_LIST_NODE, Entry);
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
			cdpfree(NodeToFree);

		InterlockedExchange(&DevExt->Started, 0);
		// No new probe can acquire rundown after the node has left the list.
		// Wait for a probe already using LowerDeviceObject before detaching it.
		ExWaitForRundownProtectionRelease(&DevExt->AutoDiscoveryRundown);
		CdpDisableAndDestroyCapture(DevExt);
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
			return CdpSendToNextDevice(DevExt->LowerDeviceObject, Irp);

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

		return CdpCompleteIrp(Irp, Status, Irp->IoStatus.Information);
	}

	default:
		break;
	}

	return CdpSendToNextDevice(DevExt->LowerDeviceObject, Irp);
}

NTSTATUS CdpIrpDispatchPower(_In_ PDEVICE_OBJECT DeviceObject, _Inout_ PIRP Irp)
{
	PCdp_DEVICE_EXTENSION DevExt = DeviceObject->DeviceExtension;
	if (!DevExt)
		return CdpIrpDispatchDefault(DeviceObject, Irp);

#if (NTDDI_VERSION < NTDDI_VISTA)
	PoStartNextPowerIrp(Irp);
	IoSkipCurrentIrpStackLocation(Irp);
	return PoCallDriver(DevExt->LowerDeviceObject, Irp);
#else
	return CdpSendToNextDevice(DevExt->LowerDeviceObject, Irp);
#endif
}

NTSTATUS CdpIrpDispatchDeviceControl(_In_ PDEVICE_OBJECT DeviceObject, _Inout_ PIRP Irp)
{
	PCdp_DEVICE_EXTENSION DevExt = (PCdp_DEVICE_EXTENSION)DeviceObject->DeviceExtension;
	PIO_STACK_LOCATION IrpSp = IoGetCurrentIrpStackLocation(Irp);
	PCdp_DRIVER_EXTENSION DriverExt = IoGetDriverObjectExtension(g_DriverObject, &g_DriverObject);
	BOOLEAN isControlDevice = (DriverExt != NULL && DeviceObject == DriverExt->ControlDevice);

	// ?????��?????????????????????? IOCTL
	if (isControlDevice)
	{
		switch (IrpSp->Parameters.DeviceIoControl.IoControlCode)
		{
		case IOCTL_Cdp_QUERY_PROTECT_STATUS:
		{
			if (IrpSp->Parameters.DeviceIoControl.OutputBufferLength < sizeof(BOOLEAN))
				return CdpCompleteIrp(Irp, STATUS_BUFFER_TOO_SMALL, 0);

			BOOLEAN isProtecting = FALSE;

			if (DriverExt)
			{
				KIRQL OldIrql;
				KeAcquireSpinLock(&DriverExt->DeviceObjectListLock, &OldIrql);
				PLIST_ENTRY Entry = DriverExt->DeviceObjectListHead.Flink;
				while (Entry != &DriverExt->DeviceObjectListHead)
				{
					PCdp_DEVICE_LIST_NODE Node = CONTAINING_RECORD(Entry, Cdp_DEVICE_LIST_NODE, Entry);
					PCdp_DEVICE_EXTENSION VolExt = (PCdp_DEVICE_EXTENSION)Node->DeviceObject->DeviceExtension;
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
			return CdpCompleteIrp(Irp, STATUS_SUCCESS, sizeof(BOOLEAN));
		}

		case IOCTL_Cdp_QUERY_CREDENTIAL:
		{
			PCdp_CREDENTIAL_STATUS_REPLY reply;
			Cdp_CREDENTIAL_DESCRIPTOR credential;
			ULONG count = 0;
			NTSTATUS status;
			if (!Irp->AssociatedIrp.SystemBuffer ||
				IrpSp->Parameters.DeviceIoControl.OutputBufferLength < sizeof(*reply))
			{
				return CdpCompleteIrp(Irp, STATUS_BUFFER_TOO_SMALL, 0);
			}
			reply = (PCdp_CREDENTIAL_STATUS_REPLY)Irp->AssociatedIrp.SystemBuffer;
			RtlZeroMemory(reply, sizeof(*reply));
			status = CdpGetSharedCredential(DriverExt, &credential, &count);
			if (status == STATUS_NOT_FOUND)
				return CdpCompleteIrp(Irp, STATUS_SUCCESS, sizeof(*reply));
			if (!NT_SUCCESS(status))
				return CdpCompleteIrp(Irp, status, 0);
			reply->Configured = 1;
			reply->JournalCount = count;
			reply->CredentialId = credential.CredentialId;
			reply->AuthEpoch = credential.AuthEpoch;
			return CdpCompleteIrp(Irp, STATUS_SUCCESS, sizeof(*reply));
		}

		case IOCTL_Cdp_AUTHENTICATE:
		{
			Cdp_AUTH_REQUEST request;
			Cdp_CREDENTIAL_DESCRIPTOR credential;
			PCdp_CONTROL_FILE_CONTEXT context =
				(PCdp_CONTROL_FILE_CONTEXT)IrpSp->FileObject->FsContext;
			NTSTATUS status;
			UINT64 now = KeQueryInterruptTime();
			if (!context || !Irp->AssociatedIrp.SystemBuffer ||
				IrpSp->Parameters.DeviceIoControl.InputBufferLength < sizeof(request))
			{
				return CdpCompleteIrp(Irp, STATUS_BUFFER_TOO_SMALL, 0);
			}
			request = *(PCdp_AUTH_REQUEST)Irp->AssociatedIrp.SystemBuffer;
			RtlSecureZeroMemory(Irp->AssociatedIrp.SystemBuffer, sizeof(request));
			if (request.PasswordLength == 0 ||
				request.PasswordLength > Cdp_PASSWORD_MAX_UTF8_BYTES)
			{
				RtlSecureZeroMemory(&request, sizeof(request));
				return CdpCompleteIrp(Irp, STATUS_INVALID_PARAMETER, 0);
			}
			if ((UINT64)InterlockedCompareExchange64(
				&DriverExt->AuthBlockedUntil100ns, 0, 0) > now)
			{
				RtlSecureZeroMemory(&request, sizeof(request));
				return CdpCompleteIrp(Irp, STATUS_ACCOUNT_LOCKED_OUT, 0);
			}
			status = CdpGetSharedCredential(DriverExt, &credential, NULL);
			if (NT_SUCCESS(status) &&
				!CdpCredentialVerify(request.Password, request.PasswordLength, &credential))
			{
				status = STATUS_ACCESS_DENIED;
			}
			RtlSecureZeroMemory(&request, sizeof(request));
			if (!NT_SUCCESS(status))
			{
				if (status == STATUS_ACCESS_DENIED &&
					InterlockedIncrement(&DriverExt->AuthFailureCount) >= 5)
				{
					InterlockedExchange(&DriverExt->AuthFailureCount, 0);
					InterlockedExchange64(&DriverExt->AuthBlockedUntil100ns,
						(LONGLONG)(now + 60ULL * 60ULL * 10000000ULL));
					status = STATUS_ACCOUNT_LOCKED_OUT;
				}
				RtlSecureZeroMemory(context, sizeof(*context));
				return CdpCompleteIrp(Irp, status, 0);
			}
			context->Authenticated = TRUE;
			InterlockedExchange(&DriverExt->AuthFailureCount, 0);
			InterlockedExchange64(&DriverExt->AuthBlockedUntil100ns, 0);
			context->CredentialId = credential.CredentialId;
			context->AuthEpoch = credential.AuthEpoch;
			context->ExpiresAt100ns = KeQueryInterruptTime() + 60ULL * 60ULL * 10000000ULL;
			return CdpCompleteIrp(Irp, STATUS_SUCCESS, 0);
		}

		case IOCTL_Cdp_CHANGE_PASSWORD:
		{
			Cdp_CHANGE_PASSWORD_REQUEST request;
			Cdp_CREDENTIAL_DESCRIPTOR oldCredential;
			Cdp_CREDENTIAL_DESCRIPTOR newCredential;
			PCdp_CONTROL_FILE_CONTEXT context =
				(PCdp_CONTROL_FILE_CONTEXT)IrpSp->FileObject->FsContext;
			PLIST_ENTRY entry;
			PCdp_VOLUME_HANDLE_ENTRY* journalEntries = NULL;
			ULONG journalCapacity = 0;
			ULONG journalCount = 0;
			ULONG changedCount = 0;
			ULONG journalIndex;
			NTSTATUS status;
			if (!context || !context->Authenticated ||
				KeQueryInterruptTime() >= context->ExpiresAt100ns ||
				!Irp->AssociatedIrp.SystemBuffer ||
				IrpSp->Parameters.DeviceIoControl.InputBufferLength < sizeof(request))
			{
				return CdpCompleteIrp(Irp, STATUS_ACCESS_DENIED, 0);
			}
			request = *(PCdp_CHANGE_PASSWORD_REQUEST)Irp->AssociatedIrp.SystemBuffer;
			RtlSecureZeroMemory(Irp->AssociatedIrp.SystemBuffer, sizeof(request));
			if (request.PasswordLength == 0 ||
				request.PasswordLength > Cdp_PASSWORD_MAX_UTF8_BYTES)
			{
				RtlSecureZeroMemory(&request, sizeof(request));
				return CdpCompleteIrp(Irp, STATUS_INVALID_PARAMETER, 0);
			}
			status = CdpGetSharedCredential(DriverExt, &oldCredential, NULL);
			if (!NT_SUCCESS(status) ||
				RtlCompareMemory(&context->CredentialId, &oldCredential.CredentialId,
					sizeof(GUID)) != sizeof(GUID) ||
				context->AuthEpoch != oldCredential.AuthEpoch)
			{
				RtlSecureZeroMemory(&request, sizeof(request));
				return CdpCompleteIrp(Irp, STATUS_ACCESS_DENIED, 0);
			}
			status = CdpCredentialCreate(
				request.Password, request.PasswordLength, &newCredential);
			RtlSecureZeroMemory(&request, sizeof(request));
			if (!NT_SUCCESS(status))
				return CdpCompleteIrp(Irp, status, 0);
			newCredential.CredentialId = oldCredential.CredentialId;
			newCredential.AuthEpoch = oldCredential.AuthEpoch + 1;
			if (newCredential.AuthEpoch == 0)
				newCredential.AuthEpoch = 1;

			/*
			 * Pin matching journal entries while holding the list mutex, then drop
			 * that mutex before writing/flushing superblocks. Raw disk I/O can re-enter
			 * this driver and must never run under VolumeHandleMutex.
			 */
			ExAcquireFastMutex(&DriverExt->VolumeHandleMutex);
			for (entry = DriverExt->VolumeHandleList.Flink;
				entry != &DriverExt->VolumeHandleList; entry = entry->Flink)
			{
				++journalCapacity;
			}
			if (journalCapacity != 0)
			{
				journalEntries = (PCdp_VOLUME_HANDLE_ENTRY*)cdpalloc(
					sizeof(*journalEntries) * journalCapacity);
			}
			if (journalCapacity != 0 && !journalEntries)
			{
				ExReleaseFastMutex(&DriverExt->VolumeHandleMutex);
				RtlSecureZeroMemory(&newCredential, sizeof(newCredential));
				RtlSecureZeroMemory(&oldCredential, sizeof(oldCredential));
				return CdpCompleteIrp(Irp, STATUS_INSUFFICIENT_RESOURCES, 0);
			}
			for (entry = DriverExt->VolumeHandleList.Flink;
				entry != &DriverExt->VolumeHandleList; entry = entry->Flink)
			{
				PCdp_VOLUME_HANDLE_ENTRY item =
					CONTAINING_RECORD(entry, Cdp_VOLUME_HANDLE_ENTRY, Entry);
				Cdp_CREDENTIAL_DESCRIPTOR current;
				if (item->Closing || !item->Journal.Mounted ||
					!CdpJournalGetCredential(&item->Journal, &current) ||
					RtlCompareMemory(&oldCredential, &current, sizeof(current)) !=
						sizeof(current))
				{
					continue;
				}
				InterlockedIncrement(&item->ReferenceCount);
				journalEntries[journalCount++] = item;
			}
			ExReleaseFastMutex(&DriverExt->VolumeHandleMutex);

			status = journalCount != 0 ? STATUS_SUCCESS : STATUS_NOT_FOUND;
			for (journalIndex = 0; journalIndex < journalCount; ++journalIndex)
			{
				status = CdpJournalSetCredential(
					&journalEntries[journalIndex]->Journal, &newCredential);
				changedCount = journalIndex + 1;
				if (!NT_SUCCESS(status))
					break;
			}
			if (!NT_SUCCESS(status))
			{
				NTSTATUS originalStatus = status;
				for (journalIndex = 0; journalIndex < changedCount; ++journalIndex)
				{
					(void)CdpJournalSetCredential(
						&journalEntries[journalIndex]->Journal, &oldCredential);
				}
				status = originalStatus;
			}
			for (journalIndex = 0; journalIndex < journalCount; ++journalIndex)
				CdpReleaseVolumeHandleEntry(journalEntries[journalIndex]);
			if (journalEntries)
				cdpfree(journalEntries);
			if (NT_SUCCESS(status))
			{
				context->CredentialId = newCredential.CredentialId;
				context->AuthEpoch = newCredential.AuthEpoch;
			}
			RtlSecureZeroMemory(&newCredential, sizeof(newCredential));
			RtlSecureZeroMemory(&oldCredential, sizeof(oldCredential));
			return CdpCompleteIrp(Irp, status, 0);
		}

		case IOCTL_Cdp_SEND_COMMAND:
		{
			PULONG pCode;
			PCdp_COMMAND_REPLY reply;
			ULONG outLen = IrpSp->Parameters.DeviceIoControl.OutputBufferLength;
			ULONG inLen = IrpSp->Parameters.DeviceIoControl.InputBufferLength;

			if (inLen < sizeof(ULONG) ||
				outLen < sizeof(Cdp_COMMAND_REPLY) ||
				!Irp->AssociatedIrp.SystemBuffer)
			{
				return CdpCompleteIrp(Irp, STATUS_BUFFER_TOO_SMALL, 0);
			}

			pCode = (PULONG)Irp->AssociatedIrp.SystemBuffer;

			switch (*pCode)
			{
			case Cdp_CMD_1:
			{
				Cdp_CMD1_REQUEST_V2 local;
				Cdp_CREDENTIAL_DESCRIPTOR credential;
				const Cdp_CREDENTIAL_DESCRIPTOR* credentialPtr = NULL;
				UINT64 handleId = 0;
				NTSTATUS Status;

				if (inLen < sizeof(Cdp_CMD1_REQUEST))
					return CdpCompleteIrp(Irp, STATUS_BUFFER_TOO_SMALL, 0);

				RtlZeroMemory(&local, sizeof(local));
				RtlCopyMemory(&local, Irp->AssociatedIrp.SystemBuffer,
					inLen < sizeof(local) ? inLen : sizeof(local));
				reply = (PCdp_COMMAND_REPLY)Irp->AssociatedIrp.SystemBuffer;
				RtlZeroMemory(&credential, sizeof(credential));
				if (local.FormatJournal != 0)
				{
					Status = CdpGetSharedCredential(DriverExt, &credential, NULL);
					if (Status == STATUS_NOT_FOUND)
					{
						if (inLen < sizeof(Cdp_CMD1_REQUEST_V2) ||
							local.PasswordLength == 0 ||
							local.PasswordLength > Cdp_PASSWORD_MAX_UTF8_BYTES)
						{
							RtlSecureZeroMemory(&local, sizeof(local));
							return CdpCompleteIrp(Irp, STATUS_PASSWORD_RESTRICTION, 0);
						}
						Status = CdpCredentialCreate(local.Password,
							local.PasswordLength, &credential);
					}
					if (!NT_SUCCESS(Status))
					{
						RtlSecureZeroMemory(&local, sizeof(local));
						return CdpCompleteIrp(Irp, Status, 0);
					}
					credentialPtr = &credential;
				}

				Cdp_DBG("CMD1 received\n");
				Cdp_LOG("version=%s journal=v%lu build=%s\n",
					Cdp_DRIVER_VERSION_STRING,
					Cdp_JOURNAL_VERSION,
					Cdp_DRIVER_BUILD_STRING);
				CdpDbgGuid("  Guid1", &local.PartitionGuid1);
				CdpDbgGuid("  Guid2", &local.PartitionGuid2);
				Status = CdpConfigureCapture(
					DriverExt,
					&local.PartitionGuid1,
					&local.PartitionGuid2,
					local.FormatJournal != 0,
					credentialPtr,
					&handleId);
				RtlSecureZeroMemory(local.Password, sizeof(local.Password));
				RtlSecureZeroMemory(Irp->AssociatedIrp.SystemBuffer, inLen);
				if (!NT_SUCCESS(Status))
				{
					Cdp_LOG("CMD1 configure failed status=0x%08X\n", Status);
					CdpFillReply(reply, Cdp_CMD_1, (ULONG)Status, 0,
						L"ERROR: capture configuration failed");
					return CdpCompleteIrp(Irp, Status, sizeof(Cdp_COMMAND_REPLY));
				}
				InterlockedExchange(&DriverExt->AutoDiscoverySuppressed, 0);
				CdpFillReply(reply, Cdp_CMD_1, 0, handleId,
					L"OK: capture configured");
				return CdpCompleteIrp(Irp, STATUS_SUCCESS, sizeof(Cdp_COMMAND_REPLY));
			}

			case Cdp_CMD_2:
			{
				Cdp_CMD2_REQUEST local;
				PCdp_DEVICE_EXTENSION sourceExt;
				UINT64 handleId;
				NTSTATUS Status;

				if (inLen < sizeof(Cdp_CMD2_REQUEST))
					return CdpCompleteIrp(Irp, STATUS_BUFFER_TOO_SMALL, 0);

				local = *(PCdp_CMD2_REQUEST)Irp->AssociatedIrp.SystemBuffer;
				reply = (PCdp_COMMAND_REPLY)Irp->AssociatedIrp.SystemBuffer;
				if (!CdpControlHandleAuthorized(
					Irp, DriverExt, &local.SourceVolumeGuid))
				{
					CdpFillReply(reply, Cdp_CMD_2, (ULONG)STATUS_ACCESS_DENIED, 0,
						L"ERROR: password authentication required");
					return CdpCompleteIrp(Irp, STATUS_ACCESS_DENIED,
						sizeof(Cdp_COMMAND_REPLY));
				}

				Cdp_DBG("CMD2 received\n");
				CdpDbgGuid("  Source", &local.SourceVolumeGuid);
				Status = KeWaitForSingleObject(
					&DriverExt->CaptureConfigMutex,
					Executive,
					KernelMode,
					FALSE,
					NULL);
				if (!NT_SUCCESS(Status))
				{
					CdpFillReply(reply, Cdp_CMD_2, (ULONG)Status, 0,
						L"ERROR: stop capture failed");
					return CdpCompleteIrp(Irp, Status, sizeof(Cdp_COMMAND_REPLY));
				}
				sourceExt = CdpFindSourceExtensionByGuid(
					DriverExt,
					&local.SourceVolumeGuid);
				handleId = sourceExt ? sourceExt->JournalHandleId : 0;
				if (!sourceExt ||
					handleId == 0 ||
					InterlockedCompareExchange(
						&sourceExt->CaptureEnabled, 0, 0) == 0)
				{
					KeReleaseMutex(&DriverExt->CaptureConfigMutex, FALSE);
					CdpFillReply(reply, Cdp_CMD_2, (ULONG)STATUS_NOT_FOUND, 0,
						L"ERROR: capture is not configured for source");
					return CdpCompleteIrp(Irp, STATUS_NOT_FOUND, sizeof(Cdp_COMMAND_REPLY));
				}
				Status = CdpCloseVolumeHandle(DriverExt, handleId);
				KeReleaseMutex(&DriverExt->CaptureConfigMutex, FALSE);
				if (!NT_SUCCESS(Status))
				{
					Cdp_LOG("CMD2 stop capture failed status=0x%08X\n", Status);
					CdpFillReply(reply, Cdp_CMD_2, (ULONG)Status, 0,
						L"ERROR: stop capture failed");
					return CdpCompleteIrp(Irp, Status, sizeof(Cdp_COMMAND_REPLY));
				}
				Cdp_LOG("capture stopped for source\n");
				CdpFillReply(reply, Cdp_CMD_2, 0, 0, L"OK: capture stopped");
				return CdpCompleteIrp(Irp, STATUS_SUCCESS, sizeof(Cdp_COMMAND_REPLY));
			}

			default:
				reply = (PCdp_COMMAND_REPLY)Irp->AssociatedIrp.SystemBuffer;
				Cdp_LOG("unknown command code %lu on SEND_COMMAND\n", *pCode);
				CdpFillReply(reply, *pCode, (ULONG)STATUS_INVALID_PARAMETER, 0, L"ERROR: unknown command");
				return CdpCompleteIrp(Irp, STATUS_INVALID_PARAMETER, sizeof(Cdp_COMMAND_REPLY));
			}
		}

		case IOCTL_Cdp_BEGIN_PREVIEW:
		{
			Cdp_PREVIEW_BEGIN_REQUEST request;
			PCdp_PREVIEW_BEGIN_REPLY reply;
			ULONG inLen =
				IrpSp->Parameters.DeviceIoControl.InputBufferLength;
			ULONG outLen =
				IrpSp->Parameters.DeviceIoControl.OutputBufferLength;
			NTSTATUS status;

			if (!DriverExt || !Irp->AssociatedIrp.SystemBuffer ||
				inLen < sizeof(request) ||
				outLen < sizeof(Cdp_PREVIEW_BEGIN_REPLY))
			{
				return CdpCompleteIrp(
					Irp,
					STATUS_BUFFER_TOO_SMALL,
					0);
			}

			request =
				*(PCdp_PREVIEW_BEGIN_REQUEST)Irp->AssociatedIrp.SystemBuffer;
			reply =
				(PCdp_PREVIEW_BEGIN_REPLY)Irp->AssociatedIrp.SystemBuffer;
			status = CdpBeginPreviewSession(
				DriverExt,
				&request,
				reply);
			return CdpCompleteIrp(
				Irp,
				status,
				NT_SUCCESS(status) ? sizeof(*reply) : 0);
		}

		case IOCTL_Cdp_READ_PREVIEW:
		{
			PCdp_PREVIEW_READ_REQUEST request;
			PVOID outputBuffer;
			ULONG inLen =
				IrpSp->Parameters.DeviceIoControl.InputBufferLength;
			ULONG outLen =
				IrpSp->Parameters.DeviceIoControl.OutputBufferLength;
			NTSTATUS status;

			if (!DriverExt || !Irp->AssociatedIrp.SystemBuffer ||
				inLen < sizeof(Cdp_PREVIEW_READ_REQUEST))
			{
				return CdpCompleteIrp(
					Irp,
					STATUS_BUFFER_TOO_SMALL,
					0);
			}
			request =
				(PCdp_PREVIEW_READ_REQUEST)Irp->AssociatedIrp.SystemBuffer;
			if (!request->ByteLength ||
				outLen < request->ByteLength ||
				!Irp->MdlAddress)
			{
				return CdpCompleteIrp(
					Irp,
					STATUS_BUFFER_TOO_SMALL,
					0);
			}
			outputBuffer = MmGetSystemAddressForMdlSafe(
				Irp->MdlAddress,
				NormalPagePriority);
			if (!outputBuffer)
			{
				return CdpCompleteIrp(
					Irp,
					STATUS_INSUFFICIENT_RESOURCES,
					0);
			}

			status = CdpReadPreviewSession(
				DriverExt,
				request,
				outputBuffer);
			return CdpCompleteIrp(
				Irp,
				status,
				NT_SUCCESS(status) ? request->ByteLength : 0);
		}

		case IOCTL_Cdp_END_PREVIEW:
		{
			PCdp_PREVIEW_END_REQUEST request;
			ULONG inLen =
				IrpSp->Parameters.DeviceIoControl.InputBufferLength;
			NTSTATUS status;

			if (!DriverExt || !Irp->AssociatedIrp.SystemBuffer ||
				inLen < sizeof(Cdp_PREVIEW_END_REQUEST))
			{
				return CdpCompleteIrp(
					Irp,
					STATUS_BUFFER_TOO_SMALL,
					0);
			}
			request =
				(PCdp_PREVIEW_END_REQUEST)Irp->AssociatedIrp.SystemBuffer;
			status = CdpEndPreviewSession(
				DriverExt,
				request->PreviewHandle);
			return CdpCompleteIrp(Irp, status, 0);
		}

		case IOCTL_Cdp_QUERY_PHASE:
		{
			Cdp_PHASE_QUERY_REQUEST request;
			PCdp_PHASE_QUERY_REPLY reply;
			ULONG inLen =
				IrpSp->Parameters.DeviceIoControl.InputBufferLength;
			ULONG outLen =
				IrpSp->Parameters.DeviceIoControl.OutputBufferLength;
			NTSTATUS status;

			if (!DriverExt || !Irp->AssociatedIrp.SystemBuffer ||
				inLen < sizeof(Cdp_PHASE_QUERY_REQUEST) ||
				outLen < sizeof(Cdp_PHASE_QUERY_REPLY))
			{
				return CdpCompleteIrp(
					Irp,
					STATUS_BUFFER_TOO_SMALL,
					0);
			}
			request =
				*(PCdp_PHASE_QUERY_REQUEST)Irp->AssociatedIrp.SystemBuffer;
			reply = (PCdp_PHASE_QUERY_REPLY)Irp->AssociatedIrp.SystemBuffer;
			status = CdpQueryPhase(DriverExt, &request, reply);
			return CdpCompleteIrp(
				Irp,
				status,
				NT_SUCCESS(status) ? sizeof(*reply) : 0);
		}

		case IOCTL_Cdp_BEGIN_RECOVERY:
		{
			Cdp_RECOVERY_BEGIN_REQUEST request;
			PCdp_RECOVERY_BEGIN_REPLY reply;
			ULONG inLen =
				IrpSp->Parameters.DeviceIoControl.InputBufferLength;
			ULONG outLen =
				IrpSp->Parameters.DeviceIoControl.OutputBufferLength;
			NTSTATUS status;

			if (!DriverExt || !Irp->AssociatedIrp.SystemBuffer ||
				inLen < sizeof(Cdp_RECOVERY_BEGIN_REQUEST) ||
				outLen < sizeof(Cdp_RECOVERY_BEGIN_REPLY))
			{
				return CdpCompleteIrp(
					Irp,
					STATUS_BUFFER_TOO_SMALL,
					0);
			}
			request =
				*(PCdp_RECOVERY_BEGIN_REQUEST)Irp->AssociatedIrp.SystemBuffer;
			if (!CdpControlHandleAuthorized(Irp, DriverExt,
				&request.SourceVolumeGuid))
			{
				return CdpCompleteIrp(Irp, STATUS_ACCESS_DENIED, 0);
			}
			reply =
				(PCdp_RECOVERY_BEGIN_REPLY)Irp->AssociatedIrp.SystemBuffer;
			status = CdpBeginRecovery(DriverExt, &request, reply);
			return CdpCompleteIrp(
				Irp,
				status,
				NT_SUCCESS(status) ? sizeof(*reply) : 0);
		}

		case IOCTL_Cdp_COMMIT_RECOVERY:
		{
			Cdp_RECOVERY_CONTROL_REQUEST request;
			PCdp_RECOVERY_COMMIT_REPLY reply;
			ULONG inLen =
				IrpSp->Parameters.DeviceIoControl.InputBufferLength;
			ULONG outLen =
				IrpSp->Parameters.DeviceIoControl.OutputBufferLength;
			NTSTATUS status;

			if (!DriverExt || !Irp->AssociatedIrp.SystemBuffer ||
				inLen < sizeof(Cdp_RECOVERY_CONTROL_REQUEST) ||
				outLen < sizeof(Cdp_RECOVERY_COMMIT_REPLY))
			{
				return CdpCompleteIrp(
					Irp,
					STATUS_BUFFER_TOO_SMALL,
					0);
			}
			request =
				*(PCdp_RECOVERY_CONTROL_REQUEST)Irp->AssociatedIrp.SystemBuffer;
			if (!CdpControlHandleAuthorized(Irp, DriverExt,
				&request.SourceVolumeGuid))
			{
				return CdpCompleteIrp(Irp, STATUS_ACCESS_DENIED, 0);
			}
			reply =
				(PCdp_RECOVERY_COMMIT_REPLY)Irp->AssociatedIrp.SystemBuffer;
			status = CdpCommitRecovery(DriverExt, &request, reply);
			return CdpCompleteIrp(
				Irp,
				status,
				NT_SUCCESS(status) ? sizeof(*reply) : 0);
		}

		case IOCTL_Cdp_CANCEL_RECOVERY:
		{
			Cdp_RECOVERY_CONTROL_REQUEST request;
			ULONG inLen =
				IrpSp->Parameters.DeviceIoControl.InputBufferLength;
			NTSTATUS status;

			if (!DriverExt || !Irp->AssociatedIrp.SystemBuffer ||
				inLen < sizeof(Cdp_RECOVERY_CONTROL_REQUEST))
			{
				return CdpCompleteIrp(
					Irp,
					STATUS_BUFFER_TOO_SMALL,
					0);
			}
			request =
				*(PCdp_RECOVERY_CONTROL_REQUEST)Irp->AssociatedIrp.SystemBuffer;
			if (!CdpControlHandleAuthorized(Irp, DriverExt,
				&request.SourceVolumeGuid))
			{
				return CdpCompleteIrp(Irp, STATUS_ACCESS_DENIED, 0);
			}
			status = CdpCancelRecovery(DriverExt, &request);
			return CdpCompleteIrp(Irp, status, 0);
		}

		case IOCTL_Cdp_QUERY_TIME_RANGE:
		{
			Cdp_TIME_RANGE_QUERY_REQUEST request;
			PCdp_TIME_RANGE_QUERY_REPLY reply;
			ULONG inLen =
				IrpSp->Parameters.DeviceIoControl.InputBufferLength;
			ULONG outLen =
				IrpSp->Parameters.DeviceIoControl.OutputBufferLength;
			NTSTATUS status;

			if (!DriverExt || !Irp->AssociatedIrp.SystemBuffer ||
				inLen < sizeof(Cdp_TIME_RANGE_QUERY_REQUEST) ||
				outLen < sizeof(Cdp_TIME_RANGE_QUERY_REPLY))
			{
				return CdpCompleteIrp(
					Irp,
					STATUS_BUFFER_TOO_SMALL,
					0);
			}
			request =
				*(PCdp_TIME_RANGE_QUERY_REQUEST)Irp->AssociatedIrp.SystemBuffer;
			reply =
				(PCdp_TIME_RANGE_QUERY_REPLY)Irp->AssociatedIrp.SystemBuffer;
			status = CdpQueryTimeRange(DriverExt, &request, reply);
			return CdpCompleteIrp(
				Irp,
				status,
				NT_SUCCESS(status) ? sizeof(*reply) : 0);
		}

		case IOCTL_Cdp_QUERY_JOURNAL_USAGE:
		{
			Cdp_JOURNAL_USAGE_QUERY_REQUEST request;
			PCdp_JOURNAL_USAGE_QUERY_REPLY reply;
			ULONG inLen =
				IrpSp->Parameters.DeviceIoControl.InputBufferLength;
			ULONG outLen =
				IrpSp->Parameters.DeviceIoControl.OutputBufferLength;
			NTSTATUS status;

			if (!DriverExt || !Irp->AssociatedIrp.SystemBuffer ||
				inLen < sizeof(request) ||
				outLen < sizeof(Cdp_JOURNAL_USAGE_QUERY_REPLY))
			{
				return CdpCompleteIrp(
					Irp,
					STATUS_BUFFER_TOO_SMALL,
					0);
			}

			request =
				*(PCdp_JOURNAL_USAGE_QUERY_REQUEST)Irp->AssociatedIrp.SystemBuffer;
			reply =
				(PCdp_JOURNAL_USAGE_QUERY_REPLY)Irp->AssociatedIrp.SystemBuffer;
			status = CdpQueryJournalUsage(DriverExt, &request, reply);
			return CdpCompleteIrp(
				Irp,
				status,
				NT_SUCCESS(status) ? sizeof(*reply) : 0);
		}

		case IOCTL_Cdp_QUERY_JOURNAL_RECORDS:
		{
			Cdp_JOURNAL_RECORD_QUERY_REQUEST request;
			PCdp_JOURNAL_RECORD_QUERY_REPLY reply;
			ULONG inLen =
				IrpSp->Parameters.DeviceIoControl.InputBufferLength;
			ULONG outLen =
				IrpSp->Parameters.DeviceIoControl.OutputBufferLength;
			ULONG recordCapacity;
			NTSTATUS status;

			if (!DriverExt || !Irp->AssociatedIrp.SystemBuffer ||
				inLen < sizeof(request) ||
				outLen < sizeof(Cdp_JOURNAL_RECORD_QUERY_REPLY))
			{
				return CdpCompleteIrp(
					Irp,
					STATUS_BUFFER_TOO_SMALL,
					0);
			}

			request =
				*(PCdp_JOURNAL_RECORD_QUERY_REQUEST)Irp->AssociatedIrp.SystemBuffer;
			if (!CdpControlHandleAuthorized(Irp, DriverExt,
				&request.SourceVolumeGuid))
			{
				return CdpCompleteIrp(Irp, STATUS_ACCESS_DENIED, 0);
			}
			reply =
				(PCdp_JOURNAL_RECORD_QUERY_REPLY)Irp->AssociatedIrp.SystemBuffer;
			recordCapacity =
				(outLen - sizeof(*reply)) / sizeof(Cdp_JOURNAL_RECORD_INFO);
			status = CdpQueryJournalRecords(
				DriverExt,
				&request,
				reply,
				recordCapacity);
			return CdpCompleteIrp(
				Irp,
				status,
				NT_SUCCESS(status) ?
					sizeof(*reply) +
						(ULONG_PTR)reply->RecordCount *
							sizeof(Cdp_JOURNAL_RECORD_INFO) :
					0);
		}

		case IOCTL_Cdp_QUERY_VERSION:
		{
			PCdp_VERSION_REPLY reply;
			ULONG outLen =
				IrpSp->Parameters.DeviceIoControl.OutputBufferLength;

			if (!Irp->AssociatedIrp.SystemBuffer ||
				outLen < sizeof(Cdp_VERSION_REPLY))
			{
				return CdpCompleteIrp(
					Irp,
					STATUS_BUFFER_TOO_SMALL,
					0);
			}

			reply = (PCdp_VERSION_REPLY)Irp->AssociatedIrp.SystemBuffer;
			RtlZeroMemory(reply, sizeof(*reply));
			reply->JournalVersion = Cdp_JOURNAL_VERSION;
			(void)RtlStringCbCopyA(
				reply->Version,
				sizeof(reply->Version),
				Cdp_DRIVER_VERSION_STRING);
			(void)RtlStringCbCopyA(
				reply->Build,
				sizeof(reply->Build),
				Cdp_DRIVER_BUILD_STRING);
			return CdpCompleteIrp(Irp, STATUS_SUCCESS, sizeof(*reply));
		}

		default:
			Cdp_LOG("unknown IOCTL 0x%08X on control device\n",
				IrpSp->Parameters.DeviceIoControl.IoControlCode);
			return CdpCompleteIrp(Irp, STATUS_INVALID_DEVICE_REQUEST, 0);
		}
	}

	if (!DevExt)
		return CdpCompleteIrp(Irp, STATUS_INVALID_DEVICE_REQUEST, 0);

	if (IrpSp->Parameters.DeviceIoControl.IoControlCode ==
		IOCTL_STORAGE_MANAGE_DATA_SET_ATTRIBUTES &&
		DevExt->LowerDeviceObject &&
		InterlockedCompareExchange(&DevExt->CaptureEnabled, 0, 0) != 0 &&
		InterlockedCompareExchange(&DevExt->CaptureStopping, 0, 0) == 0 &&
		IrpSp->Parameters.DeviceIoControl.InputBufferLength >=
			sizeof(DEVICE_MANAGE_DATA_SET_ATTRIBUTES) &&
		Irp->AssociatedIrp.SystemBuffer)
	{
		PDEVICE_MANAGE_DATA_SET_ATTRIBUTES attributes =
			(PDEVICE_MANAGE_DATA_SET_ATTRIBUTES)Irp->AssociatedIrp.SystemBuffer;
		if (attributes->Action == DeviceDsmAction_Trim)
		{
			// Keep discarded clusters physically intact while CDP is enabled.
			// If a cluster is later reused, its real IRP_MJ_WRITE captures the
			// before-image on demand.  Capturing every deleted byte here would
			// exhaust the journal during large file deletions.
			Cdp_LOG("[COW-TRIM] trim suppressed irp=%p rangesOffset=%lu rangesLength=%lu flags=0x%08lX\n",
				Irp,
				attributes->DataSetRangesOffset,
				attributes->DataSetRangesLength,
				attributes->Flags);
			return CdpCompleteIrp(Irp, STATUS_SUCCESS, 0);
		}
	}

	return CdpSendToNextDevice(DevExt->LowerDeviceObject, Irp);
}
