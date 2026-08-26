#pragma once

#ifndef Cdp_USERMODE

#include "cdp_store.h"
#include <ntddk.h>

/* Expose [AbsoluteStart, AbsoluteStart + Size) as the Store's logical
 * absolute-disk address space.  Requests below AbsoluteStart are rejected. */
NTSTATUS CdpDevStoreCreateAbsoluteRange(
	_In_ PDEVICE_OBJECT Device,
	_In_ UINT64 AbsoluteStart,
	_In_ UINT64 Size,
	_In_ ULONG SectorSize,
	_Outptr_ PCdp_STORE* OutStore);

VOID CdpDevStoreDestroy(_Inout_opt_ PCdp_STORE Store);

// Submit a synchronous absolute write below the physical-disk filter and mark
// the lower write stack with SL_FORCE_DIRECT_WRITE.
NTSTATUS CdpDevStoreWriteDiskAbsoluteForceDirect(
	_In_ PDEVICE_OBJECT DiskLowerDevice,
	_In_ UINT64 AbsoluteOffset,
	_In_ ULONG Length,
	_In_reads_bytes_(Length) const VOID* Buffer);

#endif
