#pragma once

/* Types come from qh_portable.h / ntddk via the includer. */
#ifndef QH_USERMODE
#include <ntddk.h>
#else
#include "qh_portable.h"
#endif

typedef struct _QH_STORE QH_STORE, *PQH_STORE;

typedef NTSTATUS (*QH_STORE_READ)(
	_In_ PQH_STORE Store,
	_In_ UINT64 Offset,
	_In_ ULONG Length,
	_Out_writes_bytes_(Length) PVOID Buffer);

typedef NTSTATUS (*QH_STORE_WRITE)(
	_In_ PQH_STORE Store,
	_In_ UINT64 Offset,
	_In_ ULONG Length,
	_In_reads_bytes_(Length) const VOID* Buffer);

struct _QH_STORE
{
	QH_STORE_READ Read;
	QH_STORE_WRITE Write;
	UINT64 Size;
	ULONG SectorSize;
	PVOID Context;
};

typedef UINT64 (*QH_QUERY_TIME_100NS)(_In_opt_ PVOID Context);
