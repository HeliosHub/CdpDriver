#pragma once

/* Types come from cdp_portable.h / ntddk via the includer. */
#ifndef Cdp_USERMODE
#include <ntddk.h>
#else
#include "cdp_portable.h"
#endif

typedef struct _Cdp_STORE Cdp_STORE, *PCdp_STORE;

typedef NTSTATUS (*Cdp_STORE_READ)(
	_In_ PCdp_STORE Store,
	_In_ UINT64 Offset,
	_In_ ULONG Length,
	_Out_writes_bytes_(Length) PVOID Buffer);

typedef NTSTATUS (*Cdp_STORE_WRITE)(
	_In_ PCdp_STORE Store,
	_In_ UINT64 Offset,
	_In_ ULONG Length,
	_In_reads_bytes_(Length) const VOID* Buffer);

struct _Cdp_STORE
{
	Cdp_STORE_READ Read;
	Cdp_STORE_WRITE Write;
	UINT64 Size;
	ULONG SectorSize;
	PVOID Context;
};

typedef UINT64 (*Cdp_QUERY_TIME_100NS)(_In_opt_ PVOID Context);
