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

static VOID CdpBootDriverReinitialize(
	_In_ PDRIVER_OBJECT DriverObject,
	_In_opt_ PVOID Context,
	_In_ ULONG Count)
{
	PCdp_DRIVER_EXTENSION driverExtension =
		(PCdp_DRIVER_EXTENSION)Context;

	UNREFERENCED_PARAMETER(DriverObject);
	UNREFERENCED_PARAMETER(Count);
	if (!driverExtension)
		return;

	InterlockedExchange(&driverExtension->BootEnumerationComplete, 1);
	Cdp_LOG("[AUTO-CDP] boot volume enumeration complete; starting gated full scan\n");
	CdpQueueAutoDiscovery(driverExtension);
}

VOID CdpDeleteFilterDevice(_In_ PDEVICE_OBJECT FilterDeviceObject)
{
	PCdp_DEVICE_EXTENSION DevExt = FilterDeviceObject->DeviceExtension;

	if (!DevExt)
		return;

	CdpDisableAndDestroyCapture(DevExt);

	if (DevExt->LowerDeviceObject)
	{
		IoDetachDevice(DevExt->LowerDeviceObject);
		DevExt->LowerDeviceObject = NULL;
	}

	IoDeleteDevice(FilterDeviceObject);
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
	InitializeListHead(&DeviceExtension->CaptureQueue);
	KeInitializeEvent(&DeviceExtension->CaptureEvent, NotificationEvent, FALSE);
	KeInitializeEvent(&DeviceExtension->RedirectWritesDrainedEvent,
		NotificationEvent, TRUE);
	InterlockedExchange(&DeviceExtension->RedirectWritesInFlight, 0);
	KeInitializeEvent(&DeviceExtension->DiskIoDrainedEvent,
		NotificationEvent, TRUE);
	InterlockedExchange(&DeviceExtension->DiskIoAccepting, 0);
	InterlockedExchange(&DeviceExtension->DiskIoOutstanding, 0);
	KeInitializeSpinLock(&DeviceExtension->RecoveryReadQueueLock);
	InitializeListHead(&DeviceExtension->RecoveryReadQueue);
	KeInitializeEvent(&DeviceExtension->RecoveryReadEvent, NotificationEvent, FALSE);
	KeInitializeEvent(&DeviceExtension->MergeThreadDoneEvent,
		NotificationEvent, TRUE);
	KeInitializeMutex(&DeviceExtension->HistoryMutex, 0);
	ExInitializeRundownProtection(&DeviceExtension->AutoDiscoveryRundown);
	InterlockedExchange(
		&DeviceExtension->AutoDiscoveryGateActive,
		deviceKind == Cdp_DEVICE_KIND_VOLUME ? 1 : 0);
	InterlockedExchange(&DeviceExtension->RebootRecoveryGateRequired, 0);
	KeInitializeEvent(&DeviceExtension->AutoDiscoveryGateEvent,
		NotificationEvent,
		deviceKind == Cdp_DEVICE_KIND_VOLUME ? FALSE : TRUE);
	DeviceExtension->SectorSize = Cdp_SECTOR_SIZE_DEFAULT;
	InterlockedExchange(&DeviceExtension->Phase, Cdp_PHASE_GENERAL);

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
	else if (deviceKind == Cdp_DEVICE_KIND_VOLUME)
	{
		Status = CdpStartRecoveryReadWorker(DeviceExtension);
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
	CdpScheduleAutoDiscovery(DriverExtension);

cleanup:
	if (!NT_SUCCESS(Status) && FilterDeviceObject)
	{
		if (DeviceExtension)
			CdpStopRecoveryReadWorker(DeviceExtension);
		if (DeviceExtension)
			CdpStopCaptureWorker(DeviceExtension);
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
	CdpStopAutoDiscovery(DriverExtension);
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
	CdpInitializeAutoDiscovery(DriverExtension);

	Status = CdpCreateControlDevice(DriverObject);
	if (!NT_SUCCESS(Status))
		goto cleanup;

	for (ULONG i = 0; i <= IRP_MJ_MAXIMUM_FUNCTION; ++i)
		DriverObject->MajorFunction[i] = CdpIrpDispatchDefault;

	DriverObject->MajorFunction[IRP_MJ_PNP] = CdpIrpDispatchPnp;
	DriverObject->MajorFunction[IRP_MJ_POWER] = CdpIrpDispatchPower;
	DriverObject->MajorFunction[IRP_MJ_READ] = CdpIrpDispatchRead;
	DriverObject->MajorFunction[IRP_MJ_WRITE] = CdpIrpDispatchWrite;
	DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = CdpIrpDispatchDeviceControl;
	DriverObject->MajorFunction[IRP_MJ_CREATE] = CdpIrpDispatchCreateClose;
	DriverObject->MajorFunction[IRP_MJ_CLOSE] = CdpIrpDispatchCreateClose;
	DriverObject->DriverExtension->AddDevice = CdpAddDevice;

	// All volume devices start with their data-I/O gate closed.  Waiting for
	// this I/O-manager milestone prevents a source that enumerates before its
	// journal from being mistaken for an ordinary volume and released early.
	IoRegisterBootDriverReinitialization(
		DriverObject,
		CdpBootDriverReinitialize,
		DriverExtension);

cleanup:
	if (!NT_SUCCESS(Status))
		CdpDriverUnload(DriverObject);
	return Status;
}
