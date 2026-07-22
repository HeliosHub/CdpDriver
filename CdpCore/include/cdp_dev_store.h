#pragma once

#ifndef Cdp_USERMODE

#include "cdp_store.h"
#include <ntddk.h>

NTSTATUS CdpDevStoreCreate(
	_In_ PDEVICE_OBJECT Device,
	_In_ UINT64 Size,
	_In_ ULONG SectorSize,
	_Outptr_ PCdp_STORE* OutStore);

VOID CdpDevStoreDestroy(_Inout_opt_ PCdp_STORE Store);

#endif
