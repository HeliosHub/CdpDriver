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

// Diagnostic dual-I/O helpers. They bypass MetaTree and access only the
// lower source-volume store bound to this Core.
NTSTATUS CdpCoreReadSourceForTest(
	_Inout_ PCdp_CORE Core,
	_In_ UINT64 Offset,
	_In_ ULONG Length,
	_Out_writes_bytes_(Length) PVOID Buffer);

NTSTATUS CdpCoreWriteSourceForTest(
	_Inout_ PCdp_CORE Core,
	_In_ UINT64 Offset,
	_In_ ULONG Length,
	_In_reads_bytes_(Length) const VOID* Buffer);
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

NTSTATUS CdpCoreQueryJournalUsage(
	_Inout_ PCdp_CORE Core,
	_Out_ PUINT64 PartitionBytes,
	_Out_ PUINT64 MetadataBytes,
	_Out_ PUINT64 PayloadBytesUsed,
	_Out_ PUINT64 PayloadBytesFree,
	_Out_ PUINT64 TotalRecords);

NTSTATUS CdpCoreJournalUsageAtLeast(
	_Inout_ PCdp_CORE Core,
	_In_ ULONG Percent,
	_Out_ PBOOLEAN AtLeast);

// Materialize current-branch latest values referenced by the oldest complete
// header region, remove those values from MetaTree, then delete the region.
NTSTATUS CdpCoreCompactOldestRegion(_Inout_ PCdp_CORE Core);

// Merge-thread lifetime gate used to make PreviewBegin race-free. Only one
// merge owner may be active for a core at a time.
NTSTATUS CdpCoreSetMergeActive(
	_Inout_ PCdp_CORE Core,
	_In_ BOOLEAN Active);

// True once when compaction stopped Preview because it reached the region
// containing the preview target record.
BOOLEAN CdpCoreConsumePreviewStoppedByMerge(_Inout_ PCdp_CORE Core);

NTSTATUS CdpCoreQueryRecordHeaders(
	_Inout_ PCdp_CORE Core,
	_In_ UINT64 StartIndex,
	_In_ UINT64 ExpectedGeneration,
	_Out_writes_to_(RecordCapacity, *ReturnedCount) PCdp_JOURNAL_RECORD Records,
	_In_ ULONG RecordCapacity,
	_Out_ PUINT64 TotalRecords,
	_Out_ PUINT64 Generation,
	_Out_ PULONG ReturnedCount);

NTSTATUS CdpCoreQueryMetaTreeStats(
	_Inout_ PCdp_CORE Core,
	_Out_ PULONG NodeCount,
	_Out_ PUINT64 LowestOffset,
	_Out_ PUINT64 HighestEndOffset);

NTSTATUS CdpCoreQueryBranches(
	_Inout_ PCdp_CORE Core,
	_In_ UINT64 StartIndex,
	_In_ UINT64 ExpectedGeneration,
	_Out_writes_to_(BranchCapacity, *ReturnedCount) PCdp_JOURNAL_BRANCH_TREE_INFO Branches,
	_In_ ULONG BranchCapacity,
	_Out_ PULONG TotalBranches,
	_Out_ PLONG CurrentBranchNumber,
	_Out_ PUINT64 Generation,
	_Out_ PULONG ReturnedCount);

Cdp_CORE_PHASE CdpCoreGetPhase(_In_ PCdp_CORE Core);

// Persist application bytes to the journal and publish them in MetaTree.
NTSTATUS CdpCoreAppendAfterImage(
	_Inout_ PCdp_CORE Core,
	_In_ UINT64 Offset,
	_In_ ULONG Length,
	_In_reads_bytes_(Length) const VOID* AfterImage,
	_Out_opt_ PCdp_JOURNAL_RECORD WrittenRecord);

// Publish a record whose payload was already written directly to the Journal.
// This updates only the in-memory latest-value map; it never issues payload I/O.
NTSTATUS CdpCorePublishRedirectRecord(
	_Inout_ PCdp_CORE Core,
	_In_ const Cdp_JOURNAL_RECORD* Record);

// Graceful protection shutdown: materialize one current-view interval to the
// source and punch it from MetaTree. Complete is TRUE when no coverage remains.
NTSTATUS CdpCoreDrainOneMetaRange(
	_Inout_ PCdp_CORE Core,
	_Out_ PBOOLEAN Complete,
	_Out_opt_ PUINT64 DrainedOffset,
	_Out_opt_ PULONG DrainedLength);

// An application write was committed directly to the source while draining;
// remove the same interval from MetaTree so reads fall through to the source.
NTSTATUS CdpCorePunchMetaRange(
	_Inout_ PCdp_CORE Core,
	_In_ UINT64 Offset,
	_In_ ULONG Length);

NTSTATUS CdpCoreRead(
	_Inout_ PCdp_CORE Core,
	_In_ UINT64 Offset,
	_In_ ULONG Length,
	_Out_writes_bytes_(Length) PVOID Buffer);

BOOLEAN CdpCoreCurrentReadHasOverlap(
	_Inout_ PCdp_CORE Core,
	_In_ UINT64 Offset,
	_In_ ULONG Length);

// Snapshot the first current-view journal mapping intersecting the requested
// source range. The returned payload offset already includes any left-side
// fragment displacement and remains valid for an immediate diagnostic read.
NTSTATUS CdpCoreQueryFirstJournalOverlap(
	_Inout_ PCdp_CORE Core,
	_In_ UINT64 Offset,
	_In_ ULONG Length,
	_Out_ PUINT64 SourceIntersectionOffset,
	_Out_ PUINT64 JournalPayloadOffset,
	_Out_ PULONG IntersectionLength);

/* Test-only split read: Buffer already contains a complete baseline from a
 * separate shadow volume. Overlay current/preview journal hits only; this
 * function never reads Core->Source. */
NTSTATUS CdpCoreOverlayJournalRead(
	_Inout_ PCdp_CORE Core,
	_In_ BOOLEAN Preview,
	_In_ UINT64 Offset,
	_In_ ULONG Length,
	_Inout_updates_bytes_(Length) PVOID Buffer);

NTSTATUS CdpCorePreviewRead(
	_Inout_ PCdp_CORE Core,
	_In_ UINT64 Offset,
	_In_ ULONG Length,
	_Out_writes_bytes_(Length) PVOID Buffer);

NTSTATUS CdpCorePreviewBegin(_Inout_ PCdp_CORE Core, _In_ UINT64 TargetTime100ns);
NTSTATUS CdpCorePreviewEnd(_Inout_ PCdp_CORE Core);

/* Create a child branch at the target record, build and atomically publish
 * its MetaTree, then return to General. No source-volume writeback occurs. */
NTSTATUS CdpCoreRecoveryBegin(_Inout_ PCdp_CORE Core, _In_ UINT64 TargetTime100ns);

/* Idempotent compatibility acknowledgement; RecoveryBegin already completed. */
NTSTATUS CdpCoreRecoveryCommit(_Inout_ PCdp_CORE Core);

// Compatibility form of Commit. It always completes without source writeback.
NTSTATUS CdpCoreRecoveryCommitStep(
	_Inout_ PCdp_CORE Core,
	_Out_ PBOOLEAN Complete);

#ifdef Cdp_USERMODE
VOID CdpCoreTestSetRecoveryBuildFailure(_In_ NTSTATUS Status);
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
