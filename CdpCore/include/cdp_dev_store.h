#pragma once

#ifndef Cdp_USERMODE

#include "cdp_store.h"
#include <ntddk.h>

NTSTATUS CdpDevStoreCreate(
	_In_ PDEVICE_OBJECT Device,
	_In_ UINT64 Size,
	_In_ ULONG SectorSize,
	_Outptr_ PCdp_STORE* OutStore);

NTSTATUS CdpDevStoreCreateOffset(
	_In_ PDEVICE_OBJECT Device,
	_In_ UINT64 BaseOffset,
	_In_ UINT64 Size,
	_In_ ULONG SectorSize,
	_Outptr_ PCdp_STORE* OutStore);

/* Expose [AbsoluteStart, AbsoluteStart + Size) as the Store's logical
 * absolute-disk address space.  Requests below AbsoluteStart are rejected. */
NTSTATUS CdpDevStoreCreateAbsoluteRange(
	_In_ PDEVICE_OBJECT Device,
	_In_ UINT64 AbsoluteStart,
	_In_ UINT64 Size,
	_In_ ULONG SectorSize,
	_Outptr_ PCdp_STORE* OutStore);

VOID CdpDevStoreDestroy(_Inout_opt_ PCdp_STORE Store);

#endif
