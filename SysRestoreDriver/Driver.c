#include "QHEngineDefs.h"
#include "QHIrpDispatchs.h"
#include "..\SysRestoreCore\include\qh_core.h"

PDRIVER_OBJECT g_DriverObject = NULL;

VOID QHDeleteFilterDevice(_In_ PDEVICE_OBJECT FilterDeviceObject)
{
	PQH_DEVICE_EXTENSION DevExt = FilterDeviceObject->DeviceExtension;

	if (!DevExt)
		return;

	QHDisableAndDestroyCapture(DevExt);

	if (DevExt->LowerDeviceObject)
	{
		IoDetachDevice(DevExt->LowerDeviceObject);
		DevExt->LowerDeviceObject = NULL;
	}

	IoDeleteDevice(FilterDeviceObject);
}

NTSTATUS QHCreateControlDevice(_In_ PDRIVER_OBJECT DriverObject)
{
	NTSTATUS Status = STATUS_SUCCESS;
	PQH_DRIVER_EXTENSION DriverExtension = NULL;
	DECLARE_CONST_UNICODE_STRING(ControlDeviceName, QH_CONTROL_DEVICE_NAME);
	DECLARE_CONST_UNICODE_STRING(ControlSystemLinkName, QH_CONTROL_SYSTEM_LINK_NAME);

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

NTSTATUS QHAddDevice(_In_ PDRIVER_OBJECT DriverObject, _In_ PDEVICE_OBJECT PhysicalDeviceObject)
{
	NTSTATUS Status = STATUS_SUCCESS;
	PQH_DRIVER_EXTENSION DriverExtension = NULL;
	PQH_DEVICE_EXTENSION DeviceExtension = NULL;
	PDEVICE_OBJECT FilterDeviceObject = NULL;
	PQH_DEVICE_LIST_NODE DeviceListNode = NULL;

	DriverExtension = IoGetDriverObjectExtension(DriverObject, &g_DriverObject);
	if (!DriverExtension)
	{
		Status = STATUS_UNSUCCESSFUL;
		goto cleanup;
	}

	Status = IoCreateDevice(
		DriverObject,
		sizeof(QH_DEVICE_EXTENSION),
		NULL,
		FILE_DEVICE_DISK,
		FILE_DEVICE_SECURE_OPEN,
		FALSE,
		&FilterDeviceObject
	);
	if (!NT_SUCCESS(Status))
		goto cleanup;

	DeviceExtension = (PQH_DEVICE_EXTENSION)FilterDeviceObject->DeviceExtension;
	RtlZeroMemory(DeviceExtension, sizeof(QH_DEVICE_EXTENSION));
	DeviceExtension->PhysicalDeviceObject = PhysicalDeviceObject;
	KeInitializeSpinLock(&DeviceExtension->CaptureQueueLock);
	InitializeListHead(&DeviceExtension->CaptureQueue);
	KeInitializeEvent(&DeviceExtension->CaptureEvent, NotificationEvent, FALSE);
	KeInitializeSpinLock(&DeviceExtension->RecoveryReadQueueLock);
	InitializeListHead(&DeviceExtension->RecoveryReadQueue);
	KeInitializeEvent(&DeviceExtension->RecoveryReadEvent, NotificationEvent, FALSE);
	KeInitializeMutex(&DeviceExtension->HistoryMutex, 0);
	DeviceExtension->SectorSize = QH_SECTOR_SIZE_DEFAULT;
	InterlockedExchange(&DeviceExtension->Phase, QH_PHASE_NORMAL);

	Status = IoAttachDeviceToDeviceStackSafe(FilterDeviceObject, PhysicalDeviceObject, &DeviceExtension->LowerDeviceObject);
	if (!NT_SUCCESS(Status))
		goto cleanup;

	FilterDeviceObject->Flags = DeviceExtension->LowerDeviceObject->Flags | DO_POWER_PAGABLE | DO_DIRECT_IO;
	Status = QHStartCaptureWorker(DeviceExtension);
	if (!NT_SUCCESS(Status))
		goto cleanup;
	Status = QHStartRecoveryReadWorker(DeviceExtension);
	if (!NT_SUCCESS(Status))
		goto cleanup;

	DeviceListNode = qhalloc(sizeof(QH_DEVICE_LIST_NODE));
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

cleanup:
	if (!NT_SUCCESS(Status) && FilterDeviceObject)
	{
		if (DeviceExtension)
			QHStopRecoveryReadWorker(DeviceExtension);
		if (DeviceExtension)
			QHStopCaptureWorker(DeviceExtension);
		if (DeviceExtension && DeviceExtension->LowerDeviceObject)
		{
			IoDetachDevice(DeviceExtension->LowerDeviceObject);
			DeviceExtension->LowerDeviceObject = NULL;
		}
		IoDeleteDevice(FilterDeviceObject);
	}
	return Status;
}

VOID QHDriverUnload(_In_ PDRIVER_OBJECT DriverObject)
{
	PQH_DRIVER_EXTENSION DriverExtension = NULL;
	PLIST_ENTRY FilterNodeEntry = NULL;
	DECLARE_CONST_UNICODE_STRING(ControlSystemLinkName, QH_CONTROL_SYSTEM_LINK_NAME);

	DriverExtension = IoGetDriverObjectExtension(DriverObject, &g_DriverObject);
	if (!DriverExtension)
		return;

	// Preview sessions own source-volume handles and can call into Core.  Tear
	// them down before any filter device/Core is removed.
	QHCloseAllPreviewSessions(DriverExtension);

	while (TRUE)
	{
		FilterNodeEntry = ExInterlockedRemoveHeadList(&DriverExtension->DeviceObjectListHead, &DriverExtension->DeviceObjectListLock);
		if (!FilterNodeEntry)
			break;
		PQH_DEVICE_LIST_NODE DeviceListNode = CONTAINING_RECORD(FilterNodeEntry, QH_DEVICE_LIST_NODE, Entry);
		if (DeviceListNode->DeviceObject)
			QHDeleteFilterDevice(DeviceListNode->DeviceObject);
		qhfree(DeviceListNode);
	}

	QHCloseAllVolumeHandles(DriverExtension);

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
	PQH_DRIVER_EXTENSION DriverExtension = NULL;

	UNREFERENCED_PARAMETER(RegistryPath);

	DbgPrintEx(
		DPFLTR_IHVDRIVER_ID,
		DPFLTR_ERROR_LEVEL,
		"SysRestoreDriver: loaded version=%s journal=v%lu build=%s\n",
		QH_DRIVER_VERSION_STRING,
		QH_JOURNAL_VERSION,
		QH_DRIVER_BUILD_STRING);

	g_DriverObject = DriverObject;
	DriverObject->DriverUnload = QHDriverUnload;

	Status = IoAllocateDriverObjectExtension(
		DriverObject,
		&g_DriverObject,
		sizeof(QH_DRIVER_EXTENSION),
		&DriverExtension
	);
	if (!NT_SUCCESS(Status))
		goto cleanup;

	RtlZeroMemory(DriverExtension, sizeof(QH_DRIVER_EXTENSION));
	InitializeListHead(&DriverExtension->DeviceObjectListHead);
	KeInitializeSpinLock(&DriverExtension->DeviceObjectListLock);
	InitializeListHead(&DriverExtension->VolumeHandleList);
	ExInitializeFastMutex(&DriverExtension->VolumeHandleMutex);
	DriverExtension->VolumeHandleNextId = 0;
	DriverExtension->CaptureTargetHandleId = 0;
	InitializeListHead(&DriverExtension->PreviewSessionList);
	ExInitializeFastMutex(&DriverExtension->PreviewSessionMutex);
	DriverExtension->PreviewSessionNextId = 0;

	Status = QHCreateControlDevice(DriverObject);
	if (!NT_SUCCESS(Status))
		goto cleanup;

	for (ULONG i = 0; i <= IRP_MJ_MAXIMUM_FUNCTION; ++i)
		DriverObject->MajorFunction[i] = QHIrpDispatchDefault;

	DriverObject->MajorFunction[IRP_MJ_PNP] = QHIrpDispatchPnp;
	DriverObject->MajorFunction[IRP_MJ_POWER] = QHIrpDispatchPower;
	DriverObject->MajorFunction[IRP_MJ_READ] = QHIrpDispatchRead;
	DriverObject->MajorFunction[IRP_MJ_WRITE] = QHIrpDispatchWrite;
	DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = QHIrpDispatchDeviceControl;
	DriverObject->DriverExtension->AddDevice = QHAddDevice;

cleanup:
	if (!NT_SUCCESS(Status))
		QHDriverUnload(DriverObject);
	return Status;
}
