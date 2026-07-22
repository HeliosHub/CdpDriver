#pragma once

#include "cdp_store.h"
#include "CdpJournal.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum _Cdp_CORE_PHASE
{
	Cdp_CORE_PHASE_GENERAL = 0,
	Cdp_CORE_PHASE_PREVIEW = 1,
	Cdp_CORE_PHASE_RECOVERY = 2
} Cdp_CORE_PHASE;

typedef struct _Cdp_CORE Cdp_CORE, *PCdp_CORE;

/* Usermode: both stores; journal owned by core via Store backend. */
NTSTATUS CdpCoreCreate(
	_In_ PCdp_STORE Source,
	_In_ PCdp_STORE Journal,
	_Outptr_ PCdp_CORE* OutCore);

#ifndef Cdp_USERMODE
/* Kernel driver: source store + existing mounted Cdp_JOURNAL on journal volume. */
NTSTATUS CdpCoreBind(
	_In_ PCdp_STORE Source,
	_Inout_ PCdp_JOURNAL Journal,
	_In_ const GUID* SourceVolumeGuid,
	_Outptr_ PCdp_CORE* OutCore);
#endif

VOID CdpCoreDestroy(_Inout_opt_ PCdp_CORE Core);

VOID CdpCoreSetTime100ns(_Inout_ PCdp_CORE Core, _In_ UINT64 Time100ns);
UINT64 CdpCoreGetTime100ns(_In_ PCdp_CORE Core);
UINT64 CdpCoreGetTargetTime100ns(_In_ PCdp_CORE Core);
UINT64 CdpCoreTick(_Inout_ PCdp_CORE Core, _In_ UINT64 Delta100ns);

NTSTATUS CdpCoreFormatJournal(_Inout_ PCdp_CORE Core);
NTSTATUS CdpCoreMountJournal(_Inout_ PCdp_CORE Core);

NTSTATUS CdpCoreQueryTimeRange(
	_Inout_ PCdp_CORE Core,
	_Out_ PUINT64 OldestTime100ns,
	_Out_ PUINT64 NewestTime100ns);

Cdp_CORE_PHASE CdpCoreGetPhase(_In_ PCdp_CORE Core);
VOID CdpCoreSetPhase(_Inout_ PCdp_CORE Core, _In_ Cdp_CORE_PHASE Phase);

/* Full COW write (before-image + journal + source write). */
NTSTATUS CdpCoreWrite(
	_Inout_ PCdp_CORE Core,
	_In_ UINT64 Offset,
	_In_ ULONG Length,
	_In_reads_bytes_(Length) const VOID* Data);

/* Driver path: before-image + journal only; caller forwards original write IRP. */
NTSTATUS CdpCoreCaptureAppend(
	_Inout_ PCdp_CORE Core,
	_In_ UINT64 Offset,
	_In_ ULONG Length,
	_Out_opt_ PCdp_JOURNAL_RECORD_HEADER WrittenHeader);

NTSTATUS CdpCoreRead(
	_Inout_ PCdp_CORE Core,
	_In_ UINT64 Offset,
	_In_ ULONG Length,
	_Out_writes_bytes_(Length) PVOID Buffer);

NTSTATUS CdpCorePreviewBegin(_Inout_ PCdp_CORE Core, _In_ UINT64 TargetTime100ns);
NTSTATUS CdpCorePreviewEnd(_Inout_ PCdp_CORE Core);

/* Build the target-time history view and remain in Recovery phase. */
NTSTATUS CdpCoreRecoveryBegin(_Inout_ PCdp_CORE Core, _In_ UINT64 TargetTime100ns);

/* Write the prepared history back to the source and return to General. */
NTSTATUS CdpCoreRecoveryCommit(_Inout_ PCdp_CORE Core);

/* Discard a prepared history view without writing back. */
NTSTATUS CdpCoreRecoveryCancel(_Inout_ PCdp_CORE Core);

VOID CdpCoreSetWritebackActive(_Inout_ PCdp_CORE Core, _In_ LONG Value);

#ifdef Cdp_USERMODE
typedef void (*Cdp_CORE_TEST_BUILD_HOOK)(_Inout_ struct _Cdp_CORE* Core);
typedef void (*Cdp_CORE_TEST_WRITEBACK_HOOK)(_Inout_ struct _Cdp_CORE* Core);
VOID CdpCoreTestSetPreviewBuildHook(_In_opt_ Cdp_CORE_TEST_BUILD_HOOK Hook);
VOID CdpCoreTestSetRecoveryBuildHook(_In_opt_ Cdp_CORE_TEST_BUILD_HOOK Hook);
VOID CdpCoreTestSetWritebackHook(_In_opt_ Cdp_CORE_TEST_WRITEBACK_HOOK Hook);
#endif

#ifdef Cdp_USERMODE
NTSTATUS CdpMemStoreCreate(
	_In_ UINT64 Size,
	_In_ ULONG SectorSize,
	_Outptr_ PCdp_STORE* OutStore);

VOID CdpMemStoreDestroy(_Inout_opt_ PCdp_STORE Store);
PVOID CdpMemStoreData(_In_ PCdp_STORE Store);
#endif

#ifdef __cplusplus
}
#endif
