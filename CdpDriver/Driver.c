#include "CdpEngineDefs.h"
#include "CdpIrpDispatchs.h"
#include "..\CdpCore\include\cdp_core.h"

PDRIVER_OBJECT g_DriverObject = NULL;

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
	DeviceExtension->PhysicalDeviceObject = PhysicalDeviceObject;
	KeInitializeSpinLock(&DeviceExtension->CaptureQueueLock);
	InitializeListHead(&DeviceExtension->CaptureQueue);
	KeInitializeEvent(&DeviceExtension->CaptureEvent, NotificationEvent, FALSE);
	KeInitializeSpinLock(&DeviceExtension->RecoveryReadQueueLock);
	InitializeListHead(&DeviceExtension->RecoveryReadQueue);
	KeInitializeEvent(&DeviceExtension->RecoveryReadEvent, NotificationEvent, FALSE);
	KeInitializeMutex(&DeviceExtension->HistoryMutex, 0);
	ExInitializeRundownProtection(&DeviceExtension->AutoDiscoveryRundown);
	DeviceExtension->SectorSize = Cdp_SECTOR_SIZE_DEFAULT;
	InterlockedExchange(&DeviceExtension->Phase, Cdp_PHASE_GENERAL);

	Status = IoAttachDeviceToDeviceStackSafe(FilterDeviceObject, PhysicalDeviceObject, &DeviceExtension->LowerDeviceObject);
	if (!NT_SUCCESS(Status))
		goto cleanup;

	FilterDeviceObject->Flags = DeviceExtension->LowerDeviceObject->Flags | DO_POWER_PAGABLE | DO_DIRECT_IO;
	Status = CdpStartCaptureWorker(DeviceExtension);
	if (!NT_SUCCESS(Status))
		goto cleanup;
	Status = CdpStartRecoveryReadWorker(DeviceExtension);
	if (!NT_SUCCESS(Status))
		goto cleanup;

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
	DriverObject->DriverExtension->AddDevice = CdpAddDevice;

cleanup:
	if (!NT_SUCCESS(Status))
		CdpDriverUnload(DriverObject);
	return Status;
}
