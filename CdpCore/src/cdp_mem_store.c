#include "cdp_core.h"
#include "cdp_alloc.h"
#include <string.h>

typedef struct _Cdp_MEM_CTX
{
	PUCHAR Data;
	UINT64 Size;
	ULONG SectorSize;
} Cdp_MEM_CTX, *PCdp_MEM_CTX;

static NTSTATUS CdpMemRead(
	_In_ PCdp_STORE Store,
	_In_ UINT64 Offset,
	_In_ ULONG Length,
	_Out_writes_bytes_(Length) PVOID Buffer)
{
	PCdp_MEM_CTX ctx = (PCdp_MEM_CTX)Store->Context;
	if (!ctx || Offset > ctx->Size || Length > ctx->Size - Offset)
		return STATUS_INVALID_PARAMETER;
	memcpy(Buffer, ctx->Data + Offset, Length);
	return STATUS_SUCCESS;
}

static NTSTATUS CdpMemWrite(
	_In_ PCdp_STORE Store,
	_In_ UINT64 Offset,
	_In_ ULONG Length,
	_In_reads_bytes_(Length) const VOID* Buffer)
{
	PCdp_MEM_CTX ctx = (PCdp_MEM_CTX)Store->Context;
	if (!ctx || Offset > ctx->Size || Length > ctx->Size - Offset)
		return STATUS_INVALID_PARAMETER;
	memcpy(ctx->Data + Offset, Buffer, Length);
	return STATUS_SUCCESS;
}

NTSTATUS CdpMemStoreCreate(
	_In_ UINT64 Size,
	_In_ ULONG SectorSize,
	_Outptr_ PCdp_STORE* OutStore)
{
	PCdp_STORE store;
	PCdp_MEM_CTX ctx;

	if (!OutStore || Size == 0 ||
		(SectorSize != 512 && SectorSize != 4096) ||
		(Size % SectorSize) != 0)
	{
		return STATUS_INVALID_PARAMETER;
	}

	store = (PCdp_STORE)Cdp_ALLOC0(sizeof(*store));
	ctx = (PCdp_MEM_CTX)Cdp_ALLOC0(sizeof(*ctx));
	if (!store || !ctx)
	{
		Cdp_FREE(store);
		Cdp_FREE(ctx);
		return STATUS_INSUFFICIENT_RESOURCES;
	}

	ctx->Data = (PUCHAR)Cdp_ALLOC0((size_t)Size);
	if (!ctx->Data)
	{
		Cdp_FREE(store);
		Cdp_FREE(ctx);
		return STATUS_INSUFFICIENT_RESOURCES;
	}

	ctx->Size = Size;
	ctx->SectorSize = SectorSize;
	store->Read = CdpMemRead;
	store->Write = CdpMemWrite;
	store->Size = Size;
	store->SectorSize = SectorSize;
	store->Context = ctx;
	*OutStore = store;
	return STATUS_SUCCESS;
}

VOID CdpMemStoreDestroy(_Inout_opt_ PCdp_STORE Store)
{
	PCdp_MEM_CTX ctx;
	if (!Store)
		return;
	ctx = (PCdp_MEM_CTX)Store->Context;
	if (ctx)
	{
		Cdp_FREE(ctx->Data);
		Cdp_FREE(ctx);
	}
	Cdp_FREE(Store);
}

PVOID CdpMemStoreData(_In_ PCdp_STORE Store)
{
	PCdp_MEM_CTX ctx;
	if (!Store)
		return NULL;
	ctx = (PCdp_MEM_CTX)Store->Context;
	return ctx ? ctx->Data : NULL;
}
