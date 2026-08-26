#include "CdpEngineDefs.h"
#include "..\CdpCore\include\cdp_alloc.h"
#include "..\CdpCore\include\cdp_dev_store.h"

typedef struct _Cdp_DEV_STORE_CTX
{
	PDEVICE_OBJECT Device;
	UINT64 BaseOffset;
	UINT64 LogicalStart;
	UINT64 Size;
	ULONG SectorSize;
} Cdp_DEV_STORE_CTX, *PCdp_DEV_STORE_CTX;

static NTSTATUS CdpDevStoreRawIo(
	_In_ PDEVICE_OBJECT Device,
	_In_ UCHAR MajorFunction,
	_In_ UCHAR StackFlags,
	_In_ UINT64 Offset,
	_In_ ULONG Length,
	_Inout_updates_bytes_(Length) PVOID Buffer)
{
	KEVENT event;
	IO_STATUS_BLOCK iosb;
	LARGE_INTEGER byteOffset;
	PIRP irp;
	NTSTATUS status;
	NTSTATUS waitStatus;
	LARGE_INTEGER diagnosticTimeout;

	if (!Device || !Buffer || Length == 0)
	{
		Cdp_LOG("[DEVSTORE] invalid I/O args device=%p buffer=%p len=%lu major=0x%02X\n",
			Device,
			Buffer,
			Length,
			MajorFunction);
		return STATUS_INVALID_PARAMETER;
	}

	byteOffset.QuadPart = (LONGLONG)Offset;
	KeInitializeEvent(&event, NotificationEvent, FALSE);
	RtlZeroMemory(&iosb, sizeof(iosb));
	irp = IoBuildSynchronousFsdRequest(
		MajorFunction,
		Device,
		Buffer,
		Length,
		&byteOffset,
		&event,
		&iosb);
	if (!irp)
		return STATUS_INSUFFICIENT_RESOURCES;
	IoGetNextIrpStackLocation(irp)->Flags |= StackFlags;

	Cdp_DBG("[DEVSTORE] io begin device=%p major=0x%02X "
		"offset=%llu len=%lu irp=%p\n",
		Device,
		MajorFunction,
		Offset,
		Length,
		irp);
	status = IoCallDriver(Device, irp);
	Cdp_DBG("[DEVSTORE] IoCallDriver returned irp=%p "
		"status=0x%08X iosb=0x%08X bytes=%Iu\n",
		irp,
		status,
		iosb.Status,
		iosb.Information);
	if (status == STATUS_PENDING)
	{
		diagnosticTimeout.QuadPart = -10LL * 1000LL * 1000LL * 10LL;
		Cdp_DBG("[DEVSTORE] wait begin irp=%p\n", irp);
		do
		{
			waitStatus = KeWaitForSingleObject(&event,
				Executive, KernelMode, FALSE, &diagnosticTimeout);
			if (waitStatus == STATUS_TIMEOUT)
			{
				Cdp_LOG("[DRAIN-DIAG] stage=devstore-io-wait-still-blocked device=%p major=0x%02X offset=%llu len=%lu irp=%p iosb=0x%08X\n",
					Device, MajorFunction, Offset, Length, irp, iosb.Status);
			}
		} while (waitStatus == STATUS_TIMEOUT);
		status = iosb.Status;
		Cdp_DBG("[DEVSTORE] wait end irp=%p "
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
	{
		Cdp_LOG("[DEVSTORE] short I/O major=0x%02X offset=%llu len=%lu bytes=%Iu\n",
			MajorFunction,
			Offset,
			Length,
			iosb.Information);
		return STATUS_UNEXPECTED_IO_ERROR;
	}
	if (!NT_SUCCESS(status))
	{
		Cdp_LOG("[DEVSTORE] I/O failed major=0x%02X offset=%llu len=%lu status=0x%08X\n",
			MajorFunction,
			Offset,
			Length,
			status);
	}
	Cdp_DBG("[DEVSTORE] io end irp=%p status=0x%08X "
		"bytes=%Iu\n",
		irp,
		status,
		iosb.Information);
	return status;
}

static NTSTATUS CdpDevStoreRead(
	_In_ PCdp_STORE Store,
	_In_ UINT64 Offset,
	_In_ ULONG Length,
	_Out_writes_bytes_(Length) PVOID Buffer)
{
	PCdp_DEV_STORE_CTX ctx = (PCdp_DEV_STORE_CTX)Store->Context;
	if (!ctx || Offset < ctx->LogicalStart || Offset > ctx->Size ||
		Length > ctx->Size - Offset)
		return STATUS_INVALID_PARAMETER;
	return CdpDevStoreRawIo(
		ctx->Device,
		IRP_MJ_READ,
		0,
		ctx->BaseOffset + Offset,
		Length,
		Buffer);
}

static NTSTATUS CdpDevStoreWrite(
	_In_ PCdp_STORE Store,
	_In_ UINT64 Offset,
	_In_ ULONG Length,
	_In_reads_bytes_(Length) const VOID* Buffer)
{
	PCdp_DEV_STORE_CTX ctx = (PCdp_DEV_STORE_CTX)Store->Context;
	if (!ctx || Offset < ctx->LogicalStart || Offset > ctx->Size ||
		Length > ctx->Size - Offset)
		return STATUS_INVALID_PARAMETER;
	/* Source stores are attached below the Disk upper filter.  Core uses this
	 * writer only to materialize a reclaimed HeaderRegion into the source
	 * baseline, so the write must be explicitly accepted by the disk stack. */
	return CdpDevStoreRawIo(
		ctx->Device,
		IRP_MJ_WRITE,
		SL_FORCE_DIRECT_WRITE,
		ctx->BaseOffset + Offset,
		Length,
		(PVOID)Buffer);
}

NTSTATUS CdpDevStoreWriteDiskAbsoluteForceDirect(
	_In_ PDEVICE_OBJECT DiskLowerDevice,
	_In_ UINT64 AbsoluteOffset,
	_In_ ULONG Length,
	_In_reads_bytes_(Length) const VOID* Buffer)
{
	return CdpDevStoreRawIo(
		DiskLowerDevice,
		IRP_MJ_WRITE,
		SL_FORCE_DIRECT_WRITE,
		AbsoluteOffset,
		Length,
		(PVOID)Buffer);
}

NTSTATUS CdpDevStoreCreateAbsoluteRange(
	_In_ PDEVICE_OBJECT Device,
	_In_ UINT64 AbsoluteStart,
	_In_ UINT64 Size,
	_In_ ULONG SectorSize,
	_Outptr_ PCdp_STORE* OutStore)
{
	PCdp_STORE store;
	PCdp_DEV_STORE_CTX ctx;
	UINT64 absoluteEnd;

	if (!Device || !OutStore || Size == 0 ||
		AbsoluteStart > MAXULONGLONG - Size)
	{
		return STATUS_INVALID_PARAMETER;
	}
	absoluteEnd = AbsoluteStart + Size;
	store = (PCdp_STORE)Cdp_ALLOC(sizeof(*store));
	ctx = (PCdp_DEV_STORE_CTX)Cdp_ALLOC(sizeof(*ctx));
	if (!store || !ctx)
	{
		if (store)
			Cdp_FREE(store);
		if (ctx)
			Cdp_FREE(ctx);
		return STATUS_INSUFFICIENT_RESOURCES;
	}

	RtlZeroMemory(store, sizeof(*store));
	RtlZeroMemory(ctx, sizeof(*ctx));
	ctx->Device = Device;
	ctx->BaseOffset = 0;
	ctx->LogicalStart = AbsoluteStart;
	ctx->Size = absoluteEnd;
	ctx->SectorSize = SectorSize;
	store->Read = CdpDevStoreRead;
	store->Write = CdpDevStoreWrite;
	store->Size = absoluteEnd;
	store->SectorSize = SectorSize;
	store->Context = ctx;
	*OutStore = store;
	return STATUS_SUCCESS;
}

VOID CdpDevStoreDestroy(_Inout_opt_ PCdp_STORE Store)
{
	if (!Store)
		return;
	if (Store->Context)
		Cdp_FREE(Store->Context);
	Cdp_FREE(Store);
}
