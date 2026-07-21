#pragma once

#include "qh_store.h"
#include "QHJournal.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum _QH_CORE_PHASE
{
	QH_CORE_PHASE_NORMAL = 0,
	QH_CORE_PHASE_PREVIEW = 1,
	QH_CORE_PHASE_RECOVERY = 2
} QH_CORE_PHASE;

typedef struct _QH_CORE QH_CORE, *PQH_CORE;

/* Usermode: both stores; journal owned by core via Store backend. */
NTSTATUS QhCoreCreate(
	_In_ PQH_STORE Source,
	_In_ PQH_STORE Journal,
	_Outptr_ PQH_CORE* OutCore);

#ifndef QH_USERMODE
/* Kernel driver: source store + existing mounted QH_JOURNAL on journal volume. */
NTSTATUS QhCoreBind(
	_In_ PQH_STORE Source,
	_Inout_ PQH_JOURNAL Journal,
	_In_ const GUID* SourceVolumeGuid,
	_Outptr_ PQH_CORE* OutCore);
#endif

VOID QhCoreDestroy(_Inout_opt_ PQH_CORE Core);

VOID QhCoreSetTime100ns(_Inout_ PQH_CORE Core, _In_ UINT64 Time100ns);
UINT64 QhCoreGetTime100ns(_In_ PQH_CORE Core);
UINT64 QhCoreGetTargetTime100ns(_In_ PQH_CORE Core);
UINT64 QhCoreTick(_Inout_ PQH_CORE Core, _In_ UINT64 Delta100ns);

NTSTATUS QhCoreFormatJournal(_Inout_ PQH_CORE Core);
NTSTATUS QhCoreMountJournal(_Inout_ PQH_CORE Core);

NTSTATUS QhCoreQueryTimeRange(
	_Inout_ PQH_CORE Core,
	_Out_ PUINT64 OldestTime100ns,
	_Out_ PUINT64 NewestTime100ns);

QH_CORE_PHASE QhCoreGetPhase(_In_ PQH_CORE Core);
VOID QhCoreSetPhase(_Inout_ PQH_CORE Core, _In_ QH_CORE_PHASE Phase);

/* Full COW write (before-image + journal + source write). */
NTSTATUS QhCoreWrite(
	_Inout_ PQH_CORE Core,
	_In_ UINT64 Offset,
	_In_ ULONG Length,
	_In_reads_bytes_(Length) const VOID* Data);

/* Driver path: before-image + journal only; caller forwards original write IRP. */
NTSTATUS QhCoreCaptureAppend(
	_Inout_ PQH_CORE Core,
	_In_ UINT64 Offset,
	_In_ ULONG Length,
	_Out_opt_ PQH_JOURNAL_RECORD_HEADER WrittenHeader);

NTSTATUS QhCoreRead(
	_Inout_ PQH_CORE Core,
	_In_ UINT64 Offset,
	_In_ ULONG Length,
	_Out_writes_bytes_(Length) PVOID Buffer);

NTSTATUS QhCorePreviewBegin(_Inout_ PQH_CORE Core, _In_ UINT64 TargetTime100ns);
NTSTATUS QhCorePreviewEnd(_Inout_ PQH_CORE Core);

/* Build the target-time history view and remain in Recovery phase. */
NTSTATUS QhCoreRecoveryBegin(_Inout_ PQH_CORE Core, _In_ UINT64 TargetTime100ns);

/* Write the prepared history back to the source and return to Normal. */
NTSTATUS QhCoreRecoveryCommit(_Inout_ PQH_CORE Core);

/* Discard a prepared history view without writing back. */
NTSTATUS QhCoreRecoveryCancel(_Inout_ PQH_CORE Core);

VOID QhCoreSetWritebackActive(_Inout_ PQH_CORE Core, _In_ LONG Value);

#ifdef QH_USERMODE
typedef void (*QH_CORE_TEST_BUILD_HOOK)(_Inout_ struct _QH_CORE* Core);
typedef void (*QH_CORE_TEST_WRITEBACK_HOOK)(_Inout_ struct _QH_CORE* Core);
VOID QhCoreTestSetPreviewBuildHook(_In_opt_ QH_CORE_TEST_BUILD_HOOK Hook);
VOID QhCoreTestSetRecoveryBuildHook(_In_opt_ QH_CORE_TEST_BUILD_HOOK Hook);
VOID QhCoreTestSetWritebackHook(_In_opt_ QH_CORE_TEST_WRITEBACK_HOOK Hook);
#endif

#ifdef QH_USERMODE
NTSTATUS QhMemStoreCreate(
	_In_ UINT64 Size,
	_In_ ULONG SectorSize,
	_Outptr_ PQH_STORE* OutStore);

VOID QhMemStoreDestroy(_Inout_opt_ PQH_STORE Store);
PVOID QhMemStoreData(_In_ PQH_STORE Store);
#endif

#ifdef __cplusplus
}
#endif
