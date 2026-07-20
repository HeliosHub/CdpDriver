#include "qh_core.h"
#include "qh_alloc.h"
#include <string.h>

typedef struct _QH_MEM_CTX
{
	PUCHAR Data;
	UINT64 Size;
	ULONG SectorSize;
} QH_MEM_CTX, *PQH_MEM_CTX;

static NTSTATUS QhMemRead(
	_In_ PQH_STORE Store,
	_In_ UINT64 Offset,
	_In_ ULONG Length,
	_Out_writes_bytes_(Length) PVOID Buffer)
{
	PQH_MEM_CTX ctx = (PQH_MEM_CTX)Store->Context;
	if (!ctx || Offset > ctx->Size || Length > ctx->Size - Offset)
		return STATUS_INVALID_PARAMETER;
	memcpy(Buffer, ctx->Data + Offset, Length);
	return STATUS_SUCCESS;
}

static NTSTATUS QhMemWrite(
	_In_ PQH_STORE Store,
	_In_ UINT64 Offset,
	_In_ ULONG Length,
	_In_reads_bytes_(Length) const VOID* Buffer)
{
	PQH_MEM_CTX ctx = (PQH_MEM_CTX)Store->Context;
	if (!ctx || Offset > ctx->Size || Length > ctx->Size - Offset)
		return STATUS_INVALID_PARAMETER;
	memcpy(ctx->Data + Offset, Buffer, Length);
	return STATUS_SUCCESS;
}

NTSTATUS QhMemStoreCreate(
	_In_ UINT64 Size,
	_In_ ULONG SectorSize,
	_Outptr_ PQH_STORE* OutStore)
{
	PQH_STORE store;
	PQH_MEM_CTX ctx;

	if (!OutStore || Size == 0 ||
		(SectorSize != 512 && SectorSize != 4096) ||
		(Size % SectorSize) != 0)
	{
		return STATUS_INVALID_PARAMETER;
	}

	store = (PQH_STORE)QH_ALLOC0(sizeof(*store));
	ctx = (PQH_MEM_CTX)QH_ALLOC0(sizeof(*ctx));
	if (!store || !ctx)
	{
		QH_FREE(store);
		QH_FREE(ctx);
		return STATUS_INSUFFICIENT_RESOURCES;
	}

	ctx->Data = (PUCHAR)QH_ALLOC0((size_t)Size);
	if (!ctx->Data)
	{
		QH_FREE(store);
		QH_FREE(ctx);
		return STATUS_INSUFFICIENT_RESOURCES;
	}

	ctx->Size = Size;
	ctx->SectorSize = SectorSize;
	store->Read = QhMemRead;
	store->Write = QhMemWrite;
	store->Size = Size;
	store->SectorSize = SectorSize;
	store->Context = ctx;
	*OutStore = store;
	return STATUS_SUCCESS;
}

VOID QhMemStoreDestroy(_Inout_opt_ PQH_STORE Store)
{
	PQH_MEM_CTX ctx;
	if (!Store)
		return;
	ctx = (PQH_MEM_CTX)Store->Context;
	if (ctx)
	{
		QH_FREE(ctx->Data);
		QH_FREE(ctx);
	}
	QH_FREE(Store);
}

PVOID QhMemStoreData(_In_ PQH_STORE Store)
{
	PQH_MEM_CTX ctx;
	if (!Store)
		return NULL;
	ctx = (PQH_MEM_CTX)Store->Context;
	return ctx ? ctx->Data : NULL;
}
