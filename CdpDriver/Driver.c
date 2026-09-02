#include "CdpEngineDefs.h"
#include "CdpIrpDispatchs.h"
#include "..\CdpCore\include\cdp_core.h"

PDRIVER_OBJECT g_DriverObject = NULL;

static Cdp_DEVICE_KIND CdpGetAttachedDeviceKind(
	_In_ PDEVICE_OBJECT PhysicalDeviceObject)
{
	WCHAR classGuidBuffer[64];
	UNICODE_STRING classGuid;
	UNICODE_STRING volumeClass;
	UNICODE_STRING diskClass;
	ULONG bytes = 0;
	NTSTATUS status;

	RtlZeroMemory(classGuidBuffer, sizeof(classGuidBuffer));
	status = IoGetDeviceProperty(
		PhysicalDeviceObject,
		DevicePropertyClassGuid,
		sizeof(classGuidBuffer) - sizeof(WCHAR),
		classGuidBuffer,
		&bytes);
	if (!NT_SUCCESS(status))
		return Cdp_DEVICE_KIND_UNKNOWN;

	RtlInitUnicodeString(&classGuid, classGuidBuffer);
	RtlInitUnicodeString(
		&volumeClass,
		L"{71a27cdd-812a-11d0-bec7-08002be2092f}");
	RtlInitUnicodeString(
		&diskClass,
		L"{4d36e967-e325-11ce-bfc1-08002be10318}");
	if (RtlEqualUnicodeString(&classGuid, &volumeClass, TRUE))
		return Cdp_DEVICE_KIND_VOLUME;
	if (RtlEqualUnicodeString(&classGuid, &diskClass, TRUE))
		return Cdp_DEVICE_KIND_DISK;
	return Cdp_DEVICE_KIND_UNKNOWN;
}

VOID CdpDeleteFilterDevice(_In_ PDEVICE_OBJECT FilterDeviceObject)
{
	PCdp_DEVICE_EXTENSION DevExt = FilterDeviceObject->DeviceExtension;
	PCdp_DRIVER_EXTENSION driverExt;

	if (!DevExt)
		return;
	driverExt = IoGetDriverObjectExtension(g_DriverObject, &g_DriverObject);
	if (DevExt->DeviceKind == Cdp_DEVICE_KIND_VOLUME)
		CdpUnbindVolumeProtectionContext(DevExt);
	else if (DevExt->DeviceKind == Cdp_DEVICE_KIND_SOURCE && driverExt)
		CdpUnbindVolumesFromSource(driverExt, FilterDeviceObject);

	CdpDisableAndDestroyCapture(DevExt);
	if (DevExt->DeviceKind == Cdp_DEVICE_KIND_DISK)
		CdpDestroyDiskProtectionIndex(DevExt);

	if (DevExt->LowerDeviceObject)
	{
		if (DevExt->DeviceKind == Cdp_DEVICE_KIND_SOURCE)
			ObDereferenceObject(DevExt->LowerDeviceObject);
		else
			IoDetachDevice(DevExt->LowerDeviceObject);
		DevExt->LowerDeviceObject = NULL;
	}

	IoDeleteDevice(FilterDeviceObject);
}

NTSTATUS CdpCreateInternalSourceDevice(
	_In_ PCdp_DRIVER_EXTENSION DriverExt,
	_In_ PCdp_DEVICE_EXTENSION DiskExt,
	_Out_ PDEVICE_OBJECT* SourceDeviceObject,
	_Out_ PCdp_DEVICE_EXTENSION* SourceExt)
{
	PDEVICE_OBJECT deviceObject = NULL;
	PCdp_DEVICE_EXTENSION ext;
	PCdp_DEVICE_LIST_NODE node = NULL;
	NTSTATUS status;

	if (!DriverExt || !DiskExt || !SourceDeviceObject || !SourceExt ||
		DiskExt->DeviceKind != Cdp_DEVICE_KIND_DISK ||
		!DiskExt->LowerDeviceObject)
		return STATUS_INVALID_PARAMETER;
	*SourceDeviceObject = NULL;
	*SourceExt = NULL;

	status = IoCreateDevice(
		g_DriverObject,
		sizeof(Cdp_DEVICE_EXTENSION),
		NULL,
		FILE_DEVICE_DISK,
		FILE_DEVICE_SECURE_OPEN,
		FALSE,
		&deviceObject);
	if (!NT_SUCCESS(status))
		return status;

	ext = (PCdp_DEVICE_EXTENSION)deviceObject->DeviceExtension;
	RtlZeroMemory(ext, sizeof(*ext));
	ext->DeviceKind = Cdp_DEVICE_KIND_SOURCE;
	ext->FilterDeviceObject = deviceObject;
	ext->PhysicalDeviceObject = DiskExt->PhysicalDeviceObject;
	ext->LowerDeviceObject = DiskExt->LowerDeviceObject;
	ObReferenceObject(ext->LowerDeviceObject);
	ext->DiskNumber = DiskExt->DiskNumber;
	ext->SectorSize = DiskExt->SectorSize;
	KeInitializeSpinLock(&ext->CaptureQueueLock);
	KeInitializeSpinLock(&ext->ProtectionBindingLock);
	InitializeListHead(&ext->CaptureQueue);
	KeInitializeEvent(&ext->CaptureEvent, NotificationEvent, FALSE);
	KeInitializeEvent(
		&ext->RedirectWritesDrainedEvent, NotificationEvent, TRUE);
	KeInitializeEvent(&ext->DiskIoDrainedEvent, NotificationEvent, TRUE);
	KeInitializeEvent(&ext->MergeThreadDoneEvent, NotificationEvent, TRUE);
	KeInitializeMutex(&ext->HistoryMutex, 0);
	InterlockedExchange(&ext->Phase, Cdp_PHASE_GENERAL);
	InterlockedExchange(&ext->Started, 1);

	node = (PCdp_DEVICE_LIST_NODE)cdpalloc(sizeof(*node));
	if (!node)
	{
		ObDereferenceObject(ext->LowerDeviceObject);
		ext->LowerDeviceObject = NULL;
		IoDeleteDevice(deviceObject);
		return STATUS_INSUFFICIENT_RESOURCES;
	}
	node->DeviceObject = deviceObject;
	ExInterlockedInsertHeadList(
		&DriverExt->DeviceObjectListHead,
		&node->Entry,
		&DriverExt->DeviceObjectListLock);
	deviceObject->Flags =
		DiskExt->FilterDeviceObject->Flags | DO_DIRECT_IO;
	deviceObject->Flags &= ~DO_DEVICE_INITIALIZING;
	*SourceDeviceObject = deviceObject;
	*SourceExt = ext;
	return STATUS_SUCCESS;
}

NTSTATUS CdpBindVolumeProtectionContext(
	_Inout_ PCdp_DEVICE_EXTENSION VolumeExt,
	_In_ PDEVICE_OBJECT SourceDeviceObject)
{
	PDEVICE_OBJECT oldSource;
	PCdp_DEVICE_EXTENSION sourceExt;
	KIRQL oldIrql;

	if (!VolumeExt || VolumeExt->DeviceKind != Cdp_DEVICE_KIND_VOLUME ||
		!SourceDeviceObject)
		return STATUS_INVALID_PARAMETER;
	sourceExt = (PCdp_DEVICE_EXTENSION)SourceDeviceObject->DeviceExtension;
	if (!sourceExt || (sourceExt->DeviceKind != Cdp_DEVICE_KIND_VOLUME &&
		sourceExt->DeviceKind != Cdp_DEVICE_KIND_DISK &&
		sourceExt->DeviceKind != Cdp_DEVICE_KIND_SOURCE) ||
		sourceExt->DiskNumber != VolumeExt->DiskNumber ||
		sourceExt->PartitionStart != VolumeExt->PartitionStart)
	{
		return STATUS_OBJECT_TYPE_MISMATCH;
	}

	ObReferenceObject(SourceDeviceObject);
	KeAcquireSpinLock(&VolumeExt->ProtectionBindingLock, &oldIrql);
	oldSource = VolumeExt->ProtectionSourceDevice;
	VolumeExt->ProtectionSourceDevice = SourceDeviceObject;
	KeReleaseSpinLock(&VolumeExt->ProtectionBindingLock, oldIrql);
	if (oldSource)
		ObDereferenceObject(oldSource);
	return STATUS_SUCCESS;
}

VOID CdpUnbindVolumeProtectionContext(
	_Inout_ PCdp_DEVICE_EXTENSION VolumeExt)
{
	PDEVICE_OBJECT oldSource;
	KIRQL oldIrql;

	if (!VolumeExt || VolumeExt->DeviceKind != Cdp_DEVICE_KIND_VOLUME)
		return;
	KeAcquireSpinLock(&VolumeExt->ProtectionBindingLock, &oldIrql);
	oldSource = VolumeExt->ProtectionSourceDevice;
	VolumeExt->ProtectionSourceDevice = NULL;
	KeReleaseSpinLock(&VolumeExt->ProtectionBindingLock, oldIrql);
	if (oldSource)
		ObDereferenceObject(oldSource);
}

VOID CdpUnbindVolumesFromSource(
	_In_ PCdp_DRIVER_EXTENSION DriverExt,
	_In_ PDEVICE_OBJECT SourceDeviceObject)
{
	KIRQL listIrql;
	PLIST_ENTRY entry;
	ULONG releasedBindings = 0;

	if (!DriverExt || !SourceDeviceObject)
		return;
	KeAcquireSpinLock(&DriverExt->DeviceObjectListLock, &listIrql);
	for (entry = DriverExt->DeviceObjectListHead.Flink;
		entry != &DriverExt->DeviceObjectListHead;
		entry = entry->Flink)
	{
		PCdp_DEVICE_LIST_NODE node =
			CONTAINING_RECORD(entry, Cdp_DEVICE_LIST_NODE, Entry);
		PCdp_DEVICE_EXTENSION ext =
			(PCdp_DEVICE_EXTENSION)node->DeviceObject->DeviceExtension;

		if (!ext || ext->DeviceKind != Cdp_DEVICE_KIND_VOLUME)
			continue;
		KeAcquireSpinLockAtDpcLevel(&ext->ProtectionBindingLock);
		if (ext->ProtectionSourceDevice == SourceDeviceObject)
		{
			ext->ProtectionSourceDevice = NULL;
			releasedBindings++;
		}
		KeReleaseSpinLockFromDpcLevel(&ext->ProtectionBindingLock);
	}
	KeReleaseSpinLock(&DriverExt->DeviceObjectListLock, listIrql);
	while (releasedBindings != 0)
	{
		releasedBindings--;
		ObDereferenceObject(SourceDeviceObject);
	}
}

VOID CdpDeleteInternalSourceDevice(
	_In_ PCdp_DRIVER_EXTENSION DriverExt,
	_In_ PDEVICE_OBJECT SourceDeviceObject)
{
	KIRQL oldIrql;
	PLIST_ENTRY entry;
	PCdp_DEVICE_LIST_NODE nodeToFree = NULL;
	PCdp_DEVICE_EXTENSION ext;

	if (!DriverExt || !SourceDeviceObject)
		return;
	ext = (PCdp_DEVICE_EXTENSION)SourceDeviceObject->DeviceExtension;
	if (!ext || ext->DeviceKind != Cdp_DEVICE_KIND_SOURCE)
		return;

	KeAcquireSpinLock(&DriverExt->DeviceObjectListLock, &oldIrql);
	for (entry = DriverExt->DeviceObjectListHead.Flink;
		entry != &DriverExt->DeviceObjectListHead;
		entry = entry->Flink)
	{
		PCdp_DEVICE_LIST_NODE node =
			CONTAINING_RECORD(entry, Cdp_DEVICE_LIST_NODE, Entry);
		if (node->DeviceObject == SourceDeviceObject)
		{
			RemoveEntryList(&node->Entry);
			nodeToFree = node;
			break;
		}
	}
	KeReleaseSpinLock(&DriverExt->DeviceObjectListLock, oldIrql);
	if (nodeToFree)
		cdpfree(nodeToFree);
	CdpDeleteFilterDevice(SourceDeviceObject);
}

VOID CdpDeleteInternalSourceDevicesForDisk(
	_In_ PCdp_DRIVER_EXTENSION DriverExt,
	_In_ ULONG DiskNumber)
{
	for (;;)
	{
		KIRQL oldIrql;
		PLIST_ENTRY entry;
		PDEVICE_OBJECT sourceDevice = NULL;

		KeAcquireSpinLock(&DriverExt->DeviceObjectListLock, &oldIrql);
		for (entry = DriverExt->DeviceObjectListHead.Flink;
			entry != &DriverExt->DeviceObjectListHead;
			entry = entry->Flink)
		{
			PCdp_DEVICE_LIST_NODE node =
				CONTAINING_RECORD(entry, Cdp_DEVICE_LIST_NODE, Entry);
			PCdp_DEVICE_EXTENSION ext =
				(PCdp_DEVICE_EXTENSION)node->DeviceObject->DeviceExtension;
			if (ext && ext->DeviceKind == Cdp_DEVICE_KIND_SOURCE &&
				ext->DiskNumber == DiskNumber)
			{
				sourceDevice = node->DeviceObject;
				ObReferenceObject(sourceDevice);
				break;
			}
		}
		KeReleaseSpinLock(&DriverExt->DeviceObjectListLock, oldIrql);
		if (!sourceDevice)
			break;
		CdpDeleteInternalSourceDevice(DriverExt, sourceDevice);
		ObDereferenceObject(sourceDevice);
	}
}

NTSTATUS CdpCreateControlDevice(_In_ PDRIVER_OBJECT DriverObject)
{
	NTSTATUS Status = STATUS_SUCCESS;
	PCdp_DRIVER_EXTENSION DriverExtension = NULL;
	DECLARE_CONST_UNICODE_STRING(ControlDeviceName, Cdp_CONTROL_DEVICE_NAME);
	DECLARE_CONST_UNICODE_STRING(ControlSystemLinkName, Cdp_CONTROL_SYSTEM_LINK_NAME);

	DriverExtension = IoGetDriverObjectExtension(DriverObject, &g_DriverObject);
	if (!DriverExtension)
	{
		Status = STATUS_UNSUCCESSFUL;
		goto cleanup;
	}

	Status = IoCreateDevice(
		DriverObject,
		0,
		(PUNICODE_STRING)&ControlDeviceName,
		FILE_DEVICE_UNKNOWN,
		FILE_DEVICE_SECURE_OPEN,
		FALSE,
		&DriverExtension->ControlDevice
	);
	if (!NT_SUCCESS(Status))
		goto cleanup;

	Status = IoCreateSymbolicLink((PUNICODE_STRING)&ControlSystemLinkName, (PUNICODE_STRING)&ControlDeviceName);
	if (!NT_SUCCESS(Status))
		goto cleanup;

cleanup:
	if (!NT_SUCCESS(Status) && DriverExtension && DriverExtension->ControlDevice)
	{
		IoDeleteDevice(DriverExtension->ControlDevice);
		DriverExtension->ControlDevice = NULL;
	}
	return Status;
}

NTSTATUS CdpAddDevice(_In_ PDRIVER_OBJECT DriverObject, _In_ PDEVICE_OBJECT PhysicalDeviceObject)
{
	NTSTATUS Status = STATUS_SUCCESS;
	PCdp_DRIVER_EXTENSION DriverExtension = NULL;
	PCdp_DEVICE_EXTENSION DeviceExtension = NULL;
	PDEVICE_OBJECT FilterDeviceObject = NULL;
	PCdp_DEVICE_LIST_NODE DeviceListNode = NULL;
	Cdp_DEVICE_KIND deviceKind;

	deviceKind = CdpGetAttachedDeviceKind(PhysicalDeviceObject);
	if (deviceKind == Cdp_DEVICE_KIND_UNKNOWN)
	{
		Cdp_LOG("AddDevice rejected unknown class PDO=%p\n",
			PhysicalDeviceObject);
		return STATUS_NOT_SUPPORTED;
	}

	DriverExtension = IoGetDriverObjectExtension(DriverObject, &g_DriverObject);
	if (!DriverExtension)
	{
		Status = STATUS_UNSUCCESSFUL;
		goto cleanup;
	}

	Status = IoCreateDevice(
		DriverObject,
		sizeof(Cdp_DEVICE_EXTENSION),
		NULL,
		FILE_DEVICE_DISK,
		FILE_DEVICE_SECURE_OPEN,
		FALSE,
		&FilterDeviceObject
	);
	if (!NT_SUCCESS(Status))
		goto cleanup;

	DeviceExtension = (PCdp_DEVICE_EXTENSION)FilterDeviceObject->DeviceExtension;
	RtlZeroMemory(DeviceExtension, sizeof(Cdp_DEVICE_EXTENSION));
	DeviceExtension->DeviceKind = deviceKind;
	DeviceExtension->FilterDeviceObject = FilterDeviceObject;
	DeviceExtension->PhysicalDeviceObject = PhysicalDeviceObject;
	KeInitializeSpinLock(&DeviceExtension->CaptureQueueLock);
	KeInitializeSpinLock(&DeviceExtension->ProtectionBindingLock);
	InitializeListHead(&DeviceExtension->CaptureQueue);
	KeInitializeEvent(&DeviceExtension->CaptureEvent, NotificationEvent, FALSE);
	KeInitializeEvent(&DeviceExtension->RedirectWritesDrainedEvent,
		NotificationEvent, TRUE);
	InterlockedExchange(&DeviceExtension->RedirectWritesInFlight, 0);
	KeInitializeEvent(&DeviceExtension->DiskIoDrainedEvent,
		NotificationEvent, TRUE);
	InterlockedExchange(&DeviceExtension->DiskIoAccepting, 0);
	InterlockedExchange(&DeviceExtension->DiskIoOutstanding, 0);
	InterlockedExchange(&DeviceExtension->ShutdownInProgress, 0);
	InterlockedExchange64(&DeviceExtension->ShutdownIrpEntryCount, 0);
	InterlockedExchange64(&DeviceExtension->ShutdownIrpCompletionCount, 0);
	InterlockedExchange64(&DeviceExtension->PowerIrpEntryCount, 0);
	InterlockedExchange64(&DeviceExtension->PowerIrpCompletionCount, 0);
	KeInitializeEvent(&DeviceExtension->MergeThreadDoneEvent,
		NotificationEvent, TRUE);
	KeInitializeMutex(&DeviceExtension->HistoryMutex, 0);
	DeviceExtension->SectorSize = Cdp_SECTOR_SIZE_DEFAULT;
	InterlockedExchange(&DeviceExtension->Phase, Cdp_PHASE_GENERAL);
	if (deviceKind == Cdp_DEVICE_KIND_DISK)
	{
		DeviceExtension->DiskProtectionIndex =
			(PCdp_DISK_PROTECTION_INDEX)cdpalloc(
				sizeof(Cdp_DISK_PROTECTION_INDEX));
		if (!DeviceExtension->DiskProtectionIndex)
		{
			Status = STATUS_INSUFFICIENT_RESOURCES;
			goto cleanup;
		}
		RtlZeroMemory(
			DeviceExtension->DiskProtectionIndex,
			sizeof(Cdp_DISK_PROTECTION_INDEX));
		KeInitializeSpinLock(
			&DeviceExtension->DiskProtectionIndex->Lock);
		DeviceExtension->DiskProtectionIndex->RecentIndex = -1;
	}

	Status = IoAttachDeviceToDeviceStackSafe(FilterDeviceObject, PhysicalDeviceObject, &DeviceExtension->LowerDeviceObject);
	if (!NT_SUCCESS(Status))
		goto cleanup;

	FilterDeviceObject->Flags = DeviceExtension->LowerDeviceObject->Flags | DO_POWER_PAGABLE | DO_DIRECT_IO;
	if (deviceKind == Cdp_DEVICE_KIND_DISK)
	{
		Status = CdpStartCaptureWorker(DeviceExtension);
		if (!NT_SUCCESS(Status))
			goto cleanup;
	}
	DeviceListNode = cdpalloc(sizeof(Cdp_DEVICE_LIST_NODE));
	if (!DeviceListNode)
	{
		Status = STATUS_MEMORY_NOT_ALLOCATED;
		goto cleanup;
	}
	DeviceListNode->DeviceObject = FilterDeviceObject;
	ExInterlockedInsertHeadList(&DriverExtension->DeviceObjectListHead,
		&DeviceListNode->Entry,
		&DriverExtension->DeviceObjectListLock);

	FilterDeviceObject->Flags &= ~DO_DEVICE_INITIALIZING;
	Cdp_LOG("attached kind=%s filter=%p lower=%p pdo=%p\n",
		deviceKind == Cdp_DEVICE_KIND_DISK ? "disk" : "volume",
		FilterDeviceObject,
		DeviceExtension->LowerDeviceObject,
		PhysicalDeviceObject);

cleanup:
	if (!NT_SUCCESS(Status) && FilterDeviceObject)
	{
		if (DeviceExtension)
			CdpStopCaptureWorker(DeviceExtension);
		if (DeviceExtension && DeviceExtension->DeviceKind == Cdp_DEVICE_KIND_DISK)
			CdpDestroyDiskProtectionIndex(DeviceExtension);
		if (DeviceExtension && DeviceExtension->LowerDeviceObject)
		{
			IoDetachDevice(DeviceExtension->LowerDeviceObject);
			DeviceExtension->LowerDeviceObject = NULL;
		}
		IoDeleteDevice(FilterDeviceObject);
	}
	return Status;
}

VOID CdpDriverUnload(_In_ PDRIVER_OBJECT DriverObject)
{
	PCdp_DRIVER_EXTENSION DriverExtension = NULL;
	PLIST_ENTRY FilterNodeEntry = NULL;
	DECLARE_CONST_UNICODE_STRING(ControlSystemLinkName, Cdp_CONTROL_SYSTEM_LINK_NAME);

	DriverExtension = IoGetDriverObjectExtension(DriverObject, &g_DriverObject);
	if (!DriverExtension)
		return;

	// Preview sessions own source-volume handles and can call into Core.  Tear
	// them down before any filter device/Core is removed.
	CdpCloseAllPreviewSessions(DriverExtension);

	while (TRUE)
	{
		FilterNodeEntry = ExInterlockedRemoveHeadList(&DriverExtension->DeviceObjectListHead, &DriverExtension->DeviceObjectListLock);
		if (!FilterNodeEntry)
			break;
		PCdp_DEVICE_LIST_NODE DeviceListNode = CONTAINING_RECORD(FilterNodeEntry, Cdp_DEVICE_LIST_NODE, Entry);
		if (DeviceListNode->DeviceObject)
			CdpDeleteFilterDevice(DeviceListNode->DeviceObject);
		cdpfree(DeviceListNode);
	}

	CdpCloseAllVolumeHandles(DriverExtension);

	if (DriverExtension->ControlDevice)
	{
		IoDeleteSymbolicLink((PUNICODE_STRING)&ControlSystemLinkName);
		IoDeleteDevice(DriverExtension->ControlDevice);
		DriverExtension->ControlDevice = NULL;
	}
}

NTSTATUS DriverEntry(_In_ PDRIVER_OBJECT DriverObject, _In_ PUNICODE_STRING RegistryPath)
{
	NTSTATUS Status = STATUS_SUCCESS;
	PCdp_DRIVER_EXTENSION DriverExtension = NULL;

	UNREFERENCED_PARAMETER(RegistryPath);

	Cdp_LOG(
		"loaded version=%s journal=v%lu build=%s\n",
		Cdp_DRIVER_VERSION_STRING,
		Cdp_JOURNAL_VERSION,
		Cdp_DRIVER_BUILD_STRING);

	g_DriverObject = DriverObject;
	DriverObject->DriverUnload = CdpDriverUnload;

	Status = IoAllocateDriverObjectExtension(
		DriverObject,
		&g_DriverObject,
		sizeof(Cdp_DRIVER_EXTENSION),
		&DriverExtension
	);
	if (!NT_SUCCESS(Status))
		goto cleanup;

	RtlZeroMemory(DriverExtension, sizeof(Cdp_DRIVER_EXTENSION));
	InitializeListHead(&DriverExtension->DeviceObjectListHead);
	KeInitializeSpinLock(&DriverExtension->DeviceObjectListLock);
	InitializeListHead(&DriverExtension->VolumeHandleList);
	ExInitializeFastMutex(&DriverExtension->VolumeHandleMutex);
	DriverExtension->VolumeHandleNextId = 0;
	KeInitializeMutex(&DriverExtension->CaptureConfigMutex, 0);
	InitializeListHead(&DriverExtension->PreviewSessionList);
	ExInitializeFastMutex(&DriverExtension->PreviewSessionMutex);
	DriverExtension->PreviewSessionNextId = 0;

	Status = CdpCreateControlDevice(DriverObject);
	if (!NT_SUCCESS(Status))
		goto cleanup;

	for (ULONG i = 0; i <= IRP_MJ_MAXIMUM_FUNCTION; ++i)
		DriverObject->MajorFunction[i] = CdpIrpDispatchDefault;

	DriverObject->MajorFunction[IRP_MJ_PNP] = CdpIrpDispatchPnp;
	DriverObject->MajorFunction[IRP_MJ_POWER] = CdpIrpDispatchPower;
	DriverObject->MajorFunction[IRP_MJ_READ] = CdpIrpDispatchRead;
	DriverObject->MajorFunction[IRP_MJ_WRITE] = CdpIrpDispatchWrite;
	DriverObject->MajorFunction[IRP_MJ_FLUSH_BUFFERS] = CdpIrpDispatchFlush;
	DriverObject->MajorFunction[IRP_MJ_SHUTDOWN] = CdpIrpDispatchShutdown;
	DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = CdpIrpDispatchDeviceControl;
	DriverObject->MajorFunction[IRP_MJ_CREATE] = CdpIrpDispatchCreateClose;
	DriverObject->MajorFunction[IRP_MJ_CLOSE] = CdpIrpDispatchCreateClose;
	DriverObject->DriverExtension->AddDevice = CdpAddDevice;

cleanup:
	if (!NT_SUCCESS(Status))
		CdpDriverUnload(DriverObject);
	return Status;
}
