#include "QHEngineDefs.h"
#include "..\SysRestoreCore\include\qh_alloc.h"
#include "..\SysRestoreCore\include\qh_dev_store.h"

typedef struct _QH_DEV_STORE_CTX
{
	PDEVICE_OBJECT Device;
	UINT64 Size;
	ULONG SectorSize;
} QH_DEV_STORE_CTX, *PQH_DEV_STORE_CTX;

static NTSTATUS QhDevStoreRawIo(
	_In_ PDEVICE_OBJECT Device,
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

	if (!Device || !Buffer || Length == 0)
		return STATUS_INVALID_PARAMETER;

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

	QH_DBG("[DEVSTORE] io begin device=%p major=0x%02X "
		"offset=%llu len=%lu irp=%p\n",
		Device,
		MajorFunction,
		Offset,
		Length,
		irp);
	status = IoCallDriver(Device, irp);
	QH_DBG("[DEVSTORE] IoCallDriver returned irp=%p "
		"status=0x%08X iosb=0x%08X bytes=%Iu\n",
		irp,
		status,
		iosb.Status,
		iosb.Information);
	if (status == STATUS_PENDING)
	{
		QH_DBG("[DEVSTORE] wait begin irp=%p\n",
			irp);
		KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, NULL);
		status = iosb.Status;
		QH_DBG("[DEVSTORE] wait end irp=%p "
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
		return STATUS_UNEXPECTED_IO_ERROR;
	QH_DBG("[DEVSTORE] io end irp=%p status=0x%08X "
		"bytes=%Iu\n",
		irp,
		status,
		iosb.Information);
	return status;
}

static NTSTATUS QhDevStoreRead(
	_In_ PQH_STORE Store,
	_In_ UINT64 Offset,
	_In_ ULONG Length,
	_Out_writes_bytes_(Length) PVOID Buffer)
{
	PQH_DEV_STORE_CTX ctx = (PQH_DEV_STORE_CTX)Store->Context;
	if (!ctx || Offset > ctx->Size || Length > ctx->Size - Offset)
		return STATUS_INVALID_PARAMETER;
	return QhDevStoreRawIo(
		ctx->Device,
		IRP_MJ_READ,
		Offset,
		Length,
		Buffer);
}

static NTSTATUS QhDevStoreWrite(
	_In_ PQH_STORE Store,
	_In_ UINT64 Offset,
	_In_ ULONG Length,
	_In_reads_bytes_(Length) const VOID* Buffer)
{
	PQH_DEV_STORE_CTX ctx = (PQH_DEV_STORE_CTX)Store->Context;
	if (!ctx || Offset > ctx->Size || Length > ctx->Size - Offset)
		return STATUS_INVALID_PARAMETER;
	return QhDevStoreRawIo(
		ctx->Device,
		IRP_MJ_WRITE,
		Offset,
		Length,
		(PVOID)Buffer);
}

NTSTATUS QhDevStoreCreate(
	_In_ PDEVICE_OBJECT Device,
	_In_ UINT64 Size,
	_In_ ULONG SectorSize,
	_Outptr_ PQH_STORE* OutStore)
{
	PQH_STORE store;
	PQH_DEV_STORE_CTX ctx;

	if (!Device || !OutStore || Size == 0)
		return STATUS_INVALID_PARAMETER;

	store = (PQH_STORE)QH_ALLOC(sizeof(*store));
	ctx = (PQH_DEV_STORE_CTX)QH_ALLOC(sizeof(*ctx));
	if (!store || !ctx)
	{
		if (store)
			QH_FREE(store);
		if (ctx)
			QH_FREE(ctx);
		return STATUS_INSUFFICIENT_RESOURCES;
	}

	RtlZeroMemory(store, sizeof(*store));
	RtlZeroMemory(ctx, sizeof(*ctx));
	ctx->Device = Device;
	ctx->Size = Size;
	ctx->SectorSize = SectorSize;
	store->Read = QhDevStoreRead;
	store->Write = QhDevStoreWrite;
	store->Size = Size;
	store->SectorSize = SectorSize;
	store->Context = ctx;
	*OutStore = store;
	return STATUS_SUCCESS;
}

VOID QhDevStoreDestroy(_Inout_opt_ PQH_STORE Store)
{
	if (!Store)
		return;
	if (Store->Context)
		QH_FREE(Store->Context);
	QH_FREE(Store);
}
