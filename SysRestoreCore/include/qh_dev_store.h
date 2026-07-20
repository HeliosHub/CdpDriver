#pragma once

#ifndef QH_USERMODE

#include "qh_store.h"
#include <ntddk.h>

NTSTATUS QhDevStoreCreate(
	_In_ PDEVICE_OBJECT Device,
	_In_ UINT64 Size,
	_In_ ULONG SectorSize,
	_Outptr_ PQH_STORE* OutStore);

VOID QhDevStoreDestroy(_Inout_opt_ PQH_STORE Store);

#endif
