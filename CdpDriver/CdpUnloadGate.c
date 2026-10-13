/*
 * CdpDriver 卸载闸门。
 * DriverUnload 默认保持 NULL，防止 sc stop 直接卸驱动。
 * 仅接受已认证的 KernelMode IOCTL_Cdp_ARM_PROTECTED_UNLOAD，
 * 成功后把真实 Unload 恢复 30 秒；到期且尚未开始卸载则重新置空。
 */
#include "CdpIrpDispatchs.h"

#define CDP_UNLOAD_ARM_100NS \
	((LONGLONG)Cdp_PROTECTED_UNLOAD_ARM_SEC * 10000000LL)

/* 30 秒窗口到期：若尚未进入 Unload，把 DriverUnload 重新置空。 */
static VOID CdpProtectedUnloadDpc(
	_In_ PKDPC Dpc,
	_In_opt_ PVOID DeferredContext,
	_In_opt_ PVOID SystemArgument1,
	_In_opt_ PVOID SystemArgument2)
{
	PCdp_DRIVER_EXTENSION ext = (PCdp_DRIVER_EXTENSION)DeferredContext;

	UNREFERENCED_PARAMETER(Dpc);
	UNREFERENCED_PARAMETER(SystemArgument1);
	UNREFERENCED_PARAMETER(SystemArgument2);

	if (ext == NULL)
		return;
	if (InterlockedCompareExchange(&ext->ProtectedUnloadStarted, 0, 0) != 0)
		return;
	if (g_DriverObject != NULL)
		g_DriverObject->DriverUnload = NULL;
	InterlockedExchange(&ext->ProtectedUnloadArmed, 0);
	Cdp_LOG("[UNLOAD-GATE] arm window expired\n");
}

/* 预览会话、Drain、Recovery 或 Merge 进行中时拒绝武装卸载。 */
static BOOLEAN CdpUnloadBusy(
	_In_ PCdp_DRIVER_EXTENSION DriverExt)
{
	KIRQL oldIrql;
	PLIST_ENTRY entry;
	BOOLEAN busy = FALSE;

	if (DriverExt == NULL)
		return TRUE;

	ExAcquireFastMutex(&DriverExt->PreviewSessionMutex);
	if (!IsListEmpty(&DriverExt->PreviewSessionList))
		busy = TRUE;
	ExReleaseFastMutex(&DriverExt->PreviewSessionMutex);
	if (busy)
		return TRUE;

	KeAcquireSpinLock(&DriverExt->DeviceObjectListLock, &oldIrql);
	for (entry = DriverExt->DeviceObjectListHead.Flink;
		entry != &DriverExt->DeviceObjectListHead;
		entry = entry->Flink)
	{
		PCdp_DEVICE_LIST_NODE node =
			CONTAINING_RECORD(entry, Cdp_DEVICE_LIST_NODE, Entry);
		PCdp_DEVICE_EXTENSION ext;
		LONG phase;
		LONG drain;

		if (node->DeviceObject == NULL)
			continue;
		ext = (PCdp_DEVICE_EXTENSION)node->DeviceObject->DeviceExtension;
		if (ext == NULL)
			continue;
		/* The driver now attaches only to volume stacks. */
		if (ext->DeviceKind != Cdp_DEVICE_KIND_VOLUME)
		{
			continue;
		}
		phase = InterlockedCompareExchange(&ext->Phase, 0, 0);
		drain = InterlockedCompareExchange(&ext->DrainProgressState, 0, 0);
		if (phase == (LONG)Cdp_PHASE_PREVIEW ||
			phase == (LONG)Cdp_PHASE_RECOVERY ||
			phase == (LONG)Cdp_PHASE_DRAINING ||
			phase == (LONG)Cdp_PHASE_MERGING ||
			drain == (LONG)Cdp_DRAIN_PROGRESS_RUNNING)
		{
			busy = TRUE;
			break;
		}
	}
	KeReleaseSpinLock(&DriverExt->DeviceObjectListLock, oldIrql);
	return busy;
}

VOID CdpInitializeUnloadGate(
	_In_ PDRIVER_OBJECT DriverObject,
	_Inout_ PCdp_DRIVER_EXTENSION DriverExt)
{
	UNREFERENCED_PARAMETER(DriverObject);

	DriverExt->RealDriverUnload = CdpDriverUnload;
	KeInitializeTimer(&DriverExt->ProtectedUnloadTimer);
	KeInitializeDpc(
		&DriverExt->ProtectedUnloadDpc,
		CdpProtectedUnloadDpc,
		DriverExt);
	InterlockedExchange(&DriverExt->ProtectedUnloadArmed, 0);
	InterlockedExchange(&DriverExt->ProtectedUnloadStarted, 0);
}

NTSTATUS CdpArmDriverUnload(
	_In_ PDRIVER_OBJECT DriverObject,
	_Inout_ PCdp_DRIVER_EXTENSION DriverExt)
{
	LARGE_INTEGER due;

	if (DriverObject == NULL || DriverExt == NULL ||
		DriverExt->RealDriverUnload == NULL)
	{
		return STATUS_INVALID_DEVICE_STATE;
	}
	if (InterlockedCompareExchange(&DriverExt->ProtectedUnloadStarted, 0, 0) != 0)
		return STATUS_TOO_LATE;
	if (CdpUnloadBusy(DriverExt))
		return STATUS_DEVICE_BUSY;

	/* 取消上一轮窗口，避免到期 DPC 在新窗口内把指针清掉。 */
	(VOID)KeCancelTimer(&DriverExt->ProtectedUnloadTimer);
	KeFlushQueuedDpcs();

	if (InterlockedCompareExchange(&DriverExt->ProtectedUnloadStarted, 0, 0) != 0)
		return STATUS_TOO_LATE;

	DriverObject->DriverUnload = DriverExt->RealDriverUnload;
	InterlockedExchange(&DriverExt->ProtectedUnloadArmed, 1);
	due.QuadPart = -CDP_UNLOAD_ARM_100NS;
	(VOID)KeSetTimer(
		&DriverExt->ProtectedUnloadTimer,
		due,
		&DriverExt->ProtectedUnloadDpc);
	Cdp_LOG("[UNLOAD-GATE] armed %u seconds\n", Cdp_PROTECTED_UNLOAD_ARM_SEC);
	return STATUS_SUCCESS;
}

VOID CdpMarkProtectedUnloadStarted(
	_Inout_ PCdp_DRIVER_EXTENSION DriverExt)
{
	if (DriverExt == NULL)
		return;
	InterlockedExchange(&DriverExt->ProtectedUnloadStarted, 1);
	(VOID)KeCancelTimer(&DriverExt->ProtectedUnloadTimer);
}
