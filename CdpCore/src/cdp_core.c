#include "cdp_core.h"
#include "cdp_alloc.h"
#include "CdpJournal.h"

#ifndef Cdp_USERMODE
#include "cdp_dev_store.h"
#define Cdp_RECOVERY_TRACE(fmt, ...) \
	Cdp_LOG("[RECOVERY] " fmt, ##__VA_ARGS__)

static VOID CdpCoreTraceTargetRecord(
	_In_ PCSTR Operation,
	_In_ UINT64 RequestedTime100ns,
	_In_ UINT64 EffectiveTime100ns,
	_In_ const Cdp_JOURNAL_RECORD_LOCATION* Location)
{
	if (Location && Location->Sequence != 0)
	{
		Cdp_LOG("[%s-TARGET] requestedTime=%llu effectiveTime=%llu recordTime=%llu sequence=%llu rrOffset=%llu headerIndex=%lu\n",
			Operation, RequestedTime100ns, EffectiveTime100ns,
			Location->WallClock100ns, Location->Sequence,
			Location->HeaderRegionOffset, Location->HeaderIndex);
	}
	else
	{
		Cdp_LOG("[%s-TARGET] requestedTime=%llu effectiveTime=%llu location unavailable\n",
			Operation, RequestedTime100ns, EffectiveTime100ns);
	}
}
#else
#define Cdp_RECOVERY_TRACE(fmt, ...) ((void)0)
#define CdpCoreTraceTargetRecord(Operation, Requested, Effective, Location) ((void)0)
#endif

#ifdef Cdp_USERMODE
#include <string.h>

static NTSTATUS g_recoveryBuildFailureStatus = STATUS_SUCCESS;

VOID CdpCoreTestSetRecoveryBuildFailure(_In_ NTSTATUS Status)
{
	g_recoveryBuildFailureStatus = Status;
}

#endif

struct _Cdp_CORE
{
	PCdp_STORE Source;
	PCdp_STORE JournalStore;
	Cdp_JOURNAL JournalStorage;
	PCdp_JOURNAL Journal;
	BOOLEAN OwnsJournal;
	LONG Phase;
	UINT64 Time100ns;
	Cdp_PREVIEW_TREE PreviewTree;
	Cdp_PREVIEW_TREE MetaTree;
	Cdp_LOCK TreeLock;
	BOOLEAN MetaTreeReady;
	UINT64 TargetTime100ns;
	UINT64 PreviewTargetSequence;
	LONG Building;
	LONG MergeActive;
	BOOLEAN PreviewStoppedByMerge;
	BOOLEAN PendingRecoveryBranch;
	BOOLEAN PendingRestoreReset;
	LONG PendingRecoveryParentBranch;
	LONG PendingRecoveryBranchNumber;
	UINT64 PendingRecoveryInheritedSequence;
	GUID SourceGuid;
};

static UINT64 CdpCoreQueryTime(_In_opt_ PVOID Context)
{
	PCdp_CORE core = (PCdp_CORE)Context;
	return core ? core->Time100ns : 0;
}

static NTSTATUS CdpCoreSourceRead(
	_In_ PCdp_CORE Core,
	_In_ UINT64 Offset,
	_In_ ULONG Length,
	_Out_writes_bytes_(Length) PVOID Buffer)
{
	return Core->Source->Read(Core->Source, Offset, Length, Buffer);
}

static NTSTATUS CdpCoreSourceWriteDirect(
	_In_ PCdp_CORE Core,
	_In_ UINT64 Offset,
	_In_ ULONG Length,
	_In_reads_bytes_(Length) const VOID* Buffer)
{
	return Core->Source->Write(Core->Source, Offset, Length, Buffer);
}

static VOID CdpCoreInitCommon(_Inout_ PCdp_CORE Core)
{
	Core->Time100ns = 1;
	Core->Phase = Cdp_CORE_PHASE_GENERAL;
	CdpPreviewTreeInitialize(&Core->PreviewTree);
	CdpPreviewTreeInitialize(&Core->MetaTree);
	Cdp_LOCK_INIT(&Core->TreeLock);
}

static NTSTATUS CdpCoreBuildMetaTree(_Inout_ PCdp_CORE Core)
{
	Cdp_PREVIEW_TREE newTree;
	Cdp_PREVIEW_TREE oldTree;
	NTSTATUS status;

	if (!Core || !Core->Journal || !Core->Journal->Mounted)
		return STATUS_DEVICE_NOT_READY;
	Core->MetaTreeReady = FALSE;
	// A normal mount already built this tree during its single reverse Header
	// scan.  Rebuild explicitly only for later callers whose mount snapshot has
	// already been consumed or invalidated by a runtime transition.
	status = CdpJournalTakeMountMetaTree(Core->Journal, &newTree);
	if (status == STATUS_NOT_FOUND)
		status = CdpJournalBuildCurrentBranchTree(Core->Journal, &newTree);
	if (!NT_SUCCESS(status))
		return status;

	Cdp_LOCK_ACQUIRE(&Core->TreeLock);
	oldTree = Core->MetaTree;
	Core->MetaTree = newTree;
	Core->MetaTreeReady = TRUE;
	Cdp_LOCK_RELEASE(&Core->TreeLock);
	CdpPreviewTreeFree(&oldTree);
	return STATUS_SUCCESS;
}

NTSTATUS CdpCoreCreate(
	_In_ PCdp_STORE Source,
	_In_ PCdp_STORE Journal,
	_Outptr_ PCdp_CORE* OutCore)
{
	PCdp_CORE core;
	GUID zero = { 0 };

	if (!Source || !Journal || !OutCore)
		return STATUS_INVALID_PARAMETER;

	core = (PCdp_CORE)Cdp_ALLOC0(sizeof(*core));
	if (!core)
		return STATUS_INSUFFICIENT_RESOURCES;

	core->Source = Source;
	core->JournalStore = Journal;
	core->Journal = &core->JournalStorage;
	core->OwnsJournal = TRUE;
	core->SourceGuid = zero;
	CdpCoreInitCommon(core);

	CdpJournalInitializeWithStore(
		core->Journal,
		Journal,
		&core->SourceGuid,
		CdpCoreQueryTime,
		core);

	*OutCore = core;
	return STATUS_SUCCESS;
}

#ifndef Cdp_USERMODE
NTSTATUS CdpCoreBind(
	_In_ PCdp_STORE Source,
	_Inout_ PCdp_JOURNAL Journal,
	_In_ const GUID* SourceVolumeGuid,
	_Outptr_ PCdp_CORE* OutCore)
{
	PCdp_CORE core;

	if (!Source || !Journal || !SourceVolumeGuid || !OutCore)
		return STATUS_INVALID_PARAMETER;

	core = (PCdp_CORE)Cdp_ALLOC0(sizeof(*core));
	if (!core)
		return STATUS_INSUFFICIENT_RESOURCES;

	core->Source = Source;
	core->JournalStore = NULL;
	core->Journal = Journal;
	core->OwnsJournal = FALSE;
	core->SourceGuid = *SourceVolumeGuid;
	CdpCoreInitCommon(core);
	if (Journal->Mounted)
	{
		NTSTATUS status;
		if (Journal->HistoryScanSkipped)
		{
			/* Auto restore boot reads the already-materialized source baseline.
			 * No Record-derived MetaTree is needed before the reset. */
			core->MetaTreeReady = TRUE;
			status = STATUS_SUCCESS;
		}
		else
		{
			status = CdpCoreBuildMetaTree(core);
		}
		if (!NT_SUCCESS(status))
		{
			CdpPreviewTreeFree(&core->MetaTree);
			CdpPreviewTreeFree(&core->PreviewTree);
			Cdp_LOCK_DELETE(&core->TreeLock);
			Cdp_FREE(core);
			return status;
		}
	}

	(*OutCore) = core;
	return STATUS_SUCCESS;
}
#endif

VOID CdpCoreDestroy(_Inout_opt_ PCdp_CORE Core)
{
	if (!Core)
		return;
	if (Core->OwnsJournal)
		CdpJournalClose(Core->Journal);
	CdpPreviewTreeFree(&Core->PreviewTree);
	CdpPreviewTreeFree(&Core->MetaTree);
	Cdp_LOCK_DELETE(&Core->TreeLock);
#ifndef Cdp_USERMODE
	if (Core->Source)
		CdpDevStoreDestroy(Core->Source);
#endif
	Cdp_FREE(Core);
}

VOID CdpCoreSetTime100ns(_Inout_ PCdp_CORE Core, _In_ UINT64 Time100ns)
{
	if (Core)
		Core->Time100ns = Time100ns;
}

UINT64 CdpCoreGetTargetTime100ns(_In_ PCdp_CORE Core)
{
	return Core ? Core->TargetTime100ns : 0;
}

NTSTATUS CdpCoreFormatJournal(_Inout_ PCdp_CORE Core)
{
	Cdp_PREVIEW_TREE oldTree;
	NTSTATUS status;

	if (!Core)
		return STATUS_INVALID_PARAMETER;
	status = CdpJournalFormat(Core->Journal);
	if (!NT_SUCCESS(status))
		return status;
	Cdp_LOCK_ACQUIRE(&Core->TreeLock);
	oldTree = Core->MetaTree;
	CdpPreviewTreeInitialize(&Core->MetaTree);
	Core->MetaTreeReady = TRUE;
	Cdp_LOCK_RELEASE(&Core->TreeLock);
	CdpPreviewTreeFree(&oldTree);
	return STATUS_SUCCESS;
}

NTSTATUS CdpCoreMountJournal(_Inout_ PCdp_CORE Core)
{
	NTSTATUS status;
	if (!Core)
		return STATUS_INVALID_PARAMETER;
	Core->MetaTreeReady = FALSE;
	status = CdpJournalMount(Core->Journal);
	if (!NT_SUCCESS(status))
		return status;
	return CdpCoreBuildMetaTree(Core);
}

NTSTATUS CdpCoreQueryTimeRange(
	_Inout_ PCdp_CORE Core,
	_Out_ PUINT64 OldestTime100ns,
	_Out_ PUINT64 NewestTime100ns)
{
	if (!Core)
		return STATUS_INVALID_PARAMETER;
	return CdpJournalQueryTimeRange(
		Core->Journal,
		OldestTime100ns,
		NewestTime100ns);
}

NTSTATUS CdpCoreQueryJournalUsage(
	_Inout_ PCdp_CORE Core,
	_Out_ PUINT64 PartitionBytes,
	_Out_ PUINT64 MetadataBytes,
	_Out_ PUINT64 PayloadBytesUsed,
	_Out_ PUINT64 PayloadBytesFree,
	_Out_ PUINT64 TotalRecords)
{
	if (!Core)
		return STATUS_INVALID_PARAMETER;
	return CdpJournalQueryUsage(
		Core->Journal,
		PartitionBytes,
		MetadataBytes,
		PayloadBytesUsed,
		PayloadBytesFree,
		TotalRecords);
}

NTSTATUS CdpCoreJournalUsageAtLeast(
	_Inout_ PCdp_CORE Core,
	_In_ ULONG Percent,
	_Out_ PBOOLEAN AtLeast)
{
	UINT64 partitionBytes;
	UINT64 metadataBytes;
	UINT64 payloadBytesUsed;
	UINT64 payloadBytesFree;
	UINT64 totalRecords;
	UINT64 payloadCapacity;
	NTSTATUS status;

	if (!Core || !AtLeast || Percent == 0 || Percent > 100)
		return STATUS_INVALID_PARAMETER;
	*AtLeast = FALSE;
	status = CdpCoreQueryJournalUsage(
		Core,
		&partitionBytes,
		&metadataBytes,
		&payloadBytesUsed,
		&payloadBytesFree,
		&totalRecords);
	UNREFERENCED_PARAMETER(partitionBytes);
	UNREFERENCED_PARAMETER(metadataBytes);
	UNREFERENCED_PARAMETER(totalRecords);
	if (!NT_SUCCESS(status))
		return status;
	if (payloadBytesUsed > MAXUINT64 - payloadBytesFree)
		return STATUS_INTEGER_OVERFLOW;
	payloadCapacity = payloadBytesUsed + payloadBytesFree;
	*AtLeast = payloadBytesUsed >=
		(payloadCapacity / 100) * Percent +
		((payloadCapacity % 100) * Percent + 99) / 100;
	return STATUS_SUCCESS;
}

NTSTATUS CdpCoreSetMergeActive(
	_Inout_ PCdp_CORE Core,
	_In_ BOOLEAN Active)
{
	NTSTATUS status = STATUS_SUCCESS;

	if (!Core)
		return STATUS_INVALID_PARAMETER;
	Cdp_LOCK_ACQUIRE(&Core->TreeLock);
	if (Active)
	{
		if (Core->MergeActive || Core->PendingRecoveryBranch)
			status = STATUS_DEVICE_BUSY;
		else
			Core->MergeActive = 1;
	}
	else
	{
		Core->MergeActive = 0;
	}
	Cdp_LOCK_RELEASE(&Core->TreeLock);
	return status;
}

BOOLEAN CdpCoreConsumePreviewStoppedByMerge(_Inout_ PCdp_CORE Core)
{
	BOOLEAN stopped;

	if (!Core)
		return FALSE;
	Cdp_LOCK_ACQUIRE(&Core->TreeLock);
	stopped = Core->PreviewStoppedByMerge;
	Core->PreviewStoppedByMerge = FALSE;
	Cdp_LOCK_RELEASE(&Core->TreeLock);
	return stopped;
}

static NTSTATUS CdpCoreMaterializeMetaNodes(
	_Inout_ PCdp_CORE Core,
	_In_opt_ PCdp_PREVIEW_TREE_NODE Node,
	_In_ UINT64 FirstSequence,
	_In_ UINT64 EndSequence)
{
	PVOID payload;
	NTSTATUS status;

	if (!Node)
		return STATUS_SUCCESS;
	status = CdpCoreMaterializeMetaNodes(
		Core, Node->Left, FirstSequence, EndSequence);
	if (!NT_SUCCESS(status))
		return status;
	if (!Node->Invalid && Node->Sequence >= FirstSequence &&
		Node->Sequence < EndSequence)
	{
		payload = Cdp_ALLOC(Node->DataLength);
		if (!payload)
			return STATUS_INSUFFICIENT_RESOURCES;
		status = CdpJournalReadPayload(
			Core->Journal, Node->FileOffset, Node->DataLength, payload);
		if (NT_SUCCESS(status))
		{
			status = CdpCoreSourceWriteDirect(
				Core, Node->Start, Node->DataLength, payload);
		}
		Cdp_FREE(payload);
		if (!NT_SUCCESS(status))
			return status;
	}
	return CdpCoreMaterializeMetaNodes(
		Core, Node->Right, FirstSequence, EndSequence);
}

static PCdp_PREVIEW_TREE_NODE CdpCoreFindMetaNodeBySequenceRange(
	_In_opt_ PCdp_PREVIEW_TREE_NODE Node,
	_In_ UINT64 FirstSequence,
	_In_ UINT64 EndSequence)
{
	PCdp_PREVIEW_TREE_NODE found;

	if (!Node)
		return NULL;
	found = CdpCoreFindMetaNodeBySequenceRange(
		Node->Left, FirstSequence, EndSequence);
	if (found)
		return found;
	if (!Node->Invalid && Node->Sequence >= FirstSequence &&
		Node->Sequence < EndSequence)
	{
		return Node;
	}
	return CdpCoreFindMetaNodeBySequenceRange(
		Node->Right, FirstSequence, EndSequence);
}

static VOID CdpCoreCollectCheckpointMergeRanges(
	_In_opt_ PCdp_PREVIEW_TREE_NODE Node,
	_Out_writes_(Capacity) PCdp_CHECKPOINT_MERGE_RANGE Ranges,
	_In_ ULONG Capacity,
	_Inout_ PULONG Count)
{
	if (!Node || *Count >= Capacity)
		return;
	CdpCoreCollectCheckpointMergeRanges(
		Node->Left, Ranges, Capacity, Count);
	if (!Node->Invalid && *Count < Capacity)
	{
		Ranges[*Count].VolumeOffset = Node->Start;
		Ranges[*Count].DataLength = Node->DataLength;
		(*Count)++;
	}
	CdpCoreCollectCheckpointMergeRanges(
		Node->Right, Ranges, Capacity, Count);
}

static NTSTATUS CdpCoreCheckpointRegionNodes(
	_Inout_ PCdp_CORE Core,
	_In_opt_ PCdp_PREVIEW_TREE_NODE Node,
	_In_ UINT64 SourceRegionOffset,
	_In_ UINT64 SourceFirstSequence,
	_In_ UINT64 SourceEndSequence,
	_Inout_ PUINT64 CheckpointId,
	_Inout_ PULONG MergedNodes,
	_Inout_ PUINT64 MergedBytes)
{
	PVOID payload;
	PCdp_CHECKPOINT_REMAP remaps = NULL;
	ULONG remapCount = 0;
	ULONG index;
	NTSTATUS status;

	if (!Node)
		return STATUS_SUCCESS;
	status = CdpCoreCheckpointRegionNodes(
		Core, Node->Left, SourceRegionOffset, SourceFirstSequence,
		SourceEndSequence, CheckpointId, MergedNodes, MergedBytes);
	if (!NT_SUCCESS(status))
		return status;
	if (!Node->Invalid)
	{
		payload = Cdp_ALLOC(Node->DataLength);
		if (!payload)
			return STATUS_INSUFFICIENT_RESOURCES;
		status = CdpJournalReadPayload(
			Core->Journal, Node->FileOffset, Node->DataLength, payload);
		if (NT_SUCCESS(status))
		{
			status = CdpJournalMergeIntoRuntimeCheckpoints(
				Core->Journal,
				SourceRegionOffset,
				SourceFirstSequence,
				SourceEndSequence,
				CheckpointId,
				Node->Start,
				Node->DataLength,
				payload,
				&remaps,
				&remapCount);
		}
		Cdp_FREE(payload);
		if (!NT_SUCCESS(status))
			return status;

		Cdp_LOCK_ACQUIRE(&Core->TreeLock);
		for (index = 0; index < remapCount; ++index)
		{
			status = CdpPreviewTreeRemapSequenceRange(
				&Core->MetaTree,
				Node->Sequence,
				remaps[index].VolumeOffset,
				remaps[index].DataLength,
				remaps[index].FileOffset);
			if (!NT_SUCCESS(status))
				break;
		}
		if (!NT_SUCCESS(status))
			Core->MetaTreeReady = FALSE;
		Cdp_LOCK_RELEASE(&Core->TreeLock);
		CdpJournalFreeCheckpointRemaps(remaps);
		if (!NT_SUCCESS(status))
			return status;
		(*MergedNodes)++;
		*MergedBytes += Node->DataLength;
	}
	return CdpCoreCheckpointRegionNodes(
		Core, Node->Right, SourceRegionOffset, SourceFirstSequence,
		SourceEndSequence, CheckpointId, MergedNodes, MergedBytes);
}

NTSTATUS CdpCoreCheckpointOldestRegion(_Inout_ PCdp_CORE Core)
{
	Cdp_PREVIEW_TREE regionTree;
	UINT64 regionOffset = 0;
	UINT64 firstSequence = 0;
	UINT64 endSequence = 0;
	UINT64 checkpointId = 0;
	ULONG mergedNodes = 0;
	UINT64 mergedBytes = 0;
	ULONG deletedTombstoneRegions = 0;
	PCdp_CHECKPOINT_REMAP relocated = NULL;
	ULONG relocatedCount = 0;
	ULONG relocatedIndex;
	PCdp_CHECKPOINT_MERGE_RANGE mergeRanges = NULL;
	ULONG mergeRangeCount = 0;
	UINT64 newCheckpointBytes = 0;
	UINT64 relocationBytes = 0;
	UINT64 wrapPaddingBytes = 0;
	UINT64 reservedBytes = 0;
	BOOLEAN previewTargetDeleted = FALSE;
	BOOLEAN treeInitialized = FALSE;
	BOOLEAN reservationActive = FALSE;
	NTSTATUS status;

	if (!Core || !Core->Journal || !Core->Journal->RestorePointSet)
		return STATUS_INVALID_DEVICE_STATE;
	Cdp_LOCK_ACQUIRE(&Core->TreeLock);
	if (!Core->MetaTreeReady || Core->PendingRecoveryBranch ||
		Core->Phase != Cdp_CORE_PHASE_GENERAL)
	{
		Cdp_LOCK_RELEASE(&Core->TreeLock);
		return STATUS_DEVICE_BUSY;
	}
	Cdp_LOCK_RELEASE(&Core->TreeLock);

	status = CdpJournalGetOldestCompactableRegion(
		Core->Journal, &regionOffset, &firstSequence, &endSequence);
	if (!NT_SUCCESS(status))
		return status;
#ifndef Cdp_USERMODE
	Cdp_LOG("[CHECKPOINT-MERGE] region begin rrOffset=%llu sequence=[%llu,%llu) existingCheckpoints=%lu\n",
		regionOffset, firstSequence, endSequence,
		Core->Journal->CheckpointCount);
#endif
	status = CdpJournalPruneUnreachableForCompaction(
		Core->Journal,
		firstSequence,
		endSequence,
		0,
		&previewTargetDeleted);
	UNREFERENCED_PARAMETER(previewTargetDeleted);
	if (!NT_SUCCESS(status))
		return status;
	status = CdpJournalBuildCurrentBranchRegionTree(
		Core->Journal, firstSequence, endSequence, &regionTree);
	if (!NT_SUCCESS(status))
		return status;
	treeInitialized = TRUE;
	if (regionTree.NodeCount != 0)
	{
		if (regionTree.NodeCount > MAXULONG /
			sizeof(Cdp_CHECKPOINT_MERGE_RANGE))
		{
			status = STATUS_INTEGER_OVERFLOW;
			goto cleanup;
		}
		mergeRanges = (PCdp_CHECKPOINT_MERGE_RANGE)Cdp_ALLOC(
			regionTree.NodeCount * sizeof(*mergeRanges));
		if (!mergeRanges)
		{
			status = STATUS_INSUFFICIENT_RESOURCES;
			goto cleanup;
		}
		CdpCoreCollectCheckpointMergeRanges(
			regionTree.Root, mergeRanges, regionTree.NodeCount,
			&mergeRangeCount);
	}
	status = CdpJournalBeginCheckpointMergeReservation(
		Core->Journal,
		regionOffset,
		mergeRanges,
		mergeRangeCount,
		&newCheckpointBytes,
		&relocationBytes,
		&wrapPaddingBytes,
		&reservedBytes);
	if (!NT_SUCCESS(status))
		goto cleanup;
	reservationActive = TRUE;
#ifndef Cdp_USERMODE
	Cdp_LOG("[CHECKPOINT-MERGE-RESERVE] rrOffset=%llu ranges=%lu newBytes=%llu relocationBytes=%llu wrapPadding=%llu reserved=%llu\n",
		regionOffset, mergeRangeCount, newCheckpointBytes,
		relocationBytes, wrapPaddingBytes, reservedBytes);
#endif

	status = CdpCoreCheckpointRegionNodes(
		Core, regionTree.Root, regionOffset, firstSequence, endSequence,
		&checkpointId, &mergedNodes, &mergedBytes);
	if (!NT_SUCCESS(status))
		goto cleanup;
	status = CdpJournalRelocateCheckpointsFromRegion(
		Core->Journal, regionOffset, &relocated, &relocatedCount);
	if (!NT_SUCCESS(status))
		goto cleanup;
	if (relocatedCount != 0)
	{
		Cdp_LOCK_ACQUIRE(&Core->TreeLock);
		for (relocatedIndex = 0;
			relocatedIndex < relocatedCount;
			++relocatedIndex)
		{
			status = CdpPreviewTreeRemapPayloadRange(
				&Core->MetaTree,
				relocated[relocatedIndex].VolumeOffset,
				relocated[relocatedIndex].DataLength,
				relocated[relocatedIndex].PreviousFileOffset,
				relocated[relocatedIndex].FileOffset);
			if (!NT_SUCCESS(status))
				break;
		}
		if (!NT_SUCCESS(status))
			Core->MetaTreeReady = FALSE;
		Cdp_LOCK_RELEASE(&Core->TreeLock);
		if (!NT_SUCCESS(status))
			goto cleanup;
	}
	status = CdpJournalValidateCheckpointMergeReservationConsumed(
		Core->Journal, regionOffset);
	if (!NT_SUCCESS(status))
		goto cleanup;

	// Payloads and MetaTree remaps are complete.  The short final switch is
	// the only point at which the old RR becomes reclaimable.
	Cdp_LOCK_ACQUIRE(&Core->TreeLock);
	status = CdpJournalDeleteOldestRegion(Core->Journal, regionOffset);
	if (NT_SUCCESS(status))
	{
		status = CdpJournalDeleteContiguousTombstonedRegions(
			Core->Journal, &deletedTombstoneRegions);
	}
	Cdp_LOCK_RELEASE(&Core->TreeLock);
#ifndef Cdp_USERMODE
	if (NT_SUCCESS(status))
	{
		Cdp_LOG("[CHECKPOINT-MERGE] region complete rrOffset=%llu sequence=[%llu,%llu) checkpointId=%llu nodes=%lu bytes=%llu checkpoints=%lu records=%lu tombstoneRrs=%lu\n",
			regionOffset, firstSequence, endSequence, checkpointId,
			mergedNodes, mergedBytes, Core->Journal->CheckpointCount,
			Core->Journal->CheckpointRecordCount,
			deletedTombstoneRegions);
	}
#endif

cleanup:
	if (reservationActive)
	{
#ifndef Cdp_USERMODE
		Cdp_LOG("[CHECKPOINT-MERGE-RESERVE] rrOffset=%llu consumed=%llu remaining=%llu status=0x%08X\n",
			regionOffset,
			Core->Journal->CheckpointMergeReservedBytes -
				Core->Journal->CheckpointMergeReservedRemaining,
			Core->Journal->CheckpointMergeReservedRemaining,
			status);
#endif
		CdpJournalEndCheckpointMergeReservation(Core->Journal);
	}
	if (mergeRanges)
		Cdp_FREE(mergeRanges);
	CdpJournalFreeCheckpointRemaps(relocated);
	if (treeInitialized)
		CdpPreviewTreeFree(&regionTree);
	return status;
}

NTSTATUS CdpCoreMaterializeRuntimeCheckpoints(_Inout_ PCdp_CORE Core)
{
	PCdp_CHECKPOINT_REMAP checkpoints = NULL;
	ULONG checkpointCount = 0;
	ULONG index;
	NTSTATUS status;

	if (!Core || !Core->Journal || !Core->Journal->RestorePointSet)
		return STATUS_INVALID_DEVICE_STATE;
	status = CdpJournalSnapshotRuntimeCheckpoints(
		Core->Journal, &checkpoints, &checkpointCount);
	if (!NT_SUCCESS(status) || checkpointCount == 0)
	{
		CdpJournalFreeCheckpointRemaps(checkpoints);
		return status;
	}
	for (index = 0; index < checkpointCount; ++index)
	{
		PVOID payload = Cdp_ALLOC(checkpoints[index].DataLength);
		if (!payload)
		{
			status = STATUS_INSUFFICIENT_RESOURCES;
			break;
		}
		status = CdpJournalReadPayload(
			Core->Journal,
			checkpoints[index].FileOffset,
			checkpoints[index].DataLength,
			payload);
		if (NT_SUCCESS(status))
		{
			status = CdpCoreSourceWriteDirect(
				Core,
				checkpoints[index].VolumeOffset,
				checkpoints[index].DataLength,
				payload);
		}
		Cdp_FREE(payload);
		if (!NT_SUCCESS(status))
			break;
	}
	CdpJournalFreeCheckpointRemaps(checkpoints);
	if (!NT_SUCCESS(status))
		return status;

	CdpJournalClearRuntimeCheckpoints(Core->Journal);
	status = CdpCoreBuildMetaTree(Core);
#ifndef Cdp_USERMODE
	if (NT_SUCCESS(status))
	{
		Cdp_LOG("[CHECKPOINT-MERGE] materialized runtime baseline before restore-point deletion ranges=%lu\n",
			checkpointCount);
	}
#endif
	return status;
}

NTSTATUS CdpCoreCompactOldestRegion(_Inout_ PCdp_CORE Core)
{
	Cdp_PREVIEW_TREE regionTree;
	UINT64 regionOffset;
	UINT64 firstSequence;
	UINT64 endSequence;
	NTSTATUS status;
	BOOLEAN regionTreeInitialized = FALSE;
	BOOLEAN previewTargetDeleted = FALSE;

	if (!Core)
		return STATUS_INVALID_PARAMETER;
	Cdp_LOCK_ACQUIRE(&Core->TreeLock);
	if (!Core->MetaTreeReady)
	{
		status = STATUS_DEVICE_NOT_READY;
		goto cleanup;
	}
	if (Core->PendingRecoveryBranch)
	{
		status = STATUS_DEVICE_BUSY;
		goto cleanup;
	}
	status = CdpJournalGetOldestCompactableRegion(
		Core->Journal, &regionOffset, &firstSequence, &endSequence);
	if (!NT_SUCCESS(status))
		goto cleanup;
#ifndef Cdp_USERMODE
	Cdp_LOG("[MERGE] region begin rrOffset=%llu sequence=[%llu,%llu)\n",
		regionOffset, firstSequence, endSequence);
#endif

	// Once reclamation reaches the record that anchors the requested preview
	// time, that historical view can no longer be maintained from the retained
	// journal. Stop it before any payload in the region is reclaimed.
	if (Core->Phase == Cdp_CORE_PHASE_PREVIEW &&
		Core->PreviewTargetSequence >= firstSequence &&
		Core->PreviewTargetSequence < endSequence)
	{
		CdpPreviewTreeFree(&Core->PreviewTree);
		CdpPreviewTreeInitialize(&Core->PreviewTree);
		Core->Building = 0;
		Core->PreviewTargetSequence = 0;
		Core->Phase = Cdp_CORE_PHASE_GENERAL;
		Core->PreviewStoppedByMerge = TRUE;
	}

	status = CdpJournalPruneUnreachableForCompaction(
		Core->Journal,
		firstSequence,
		endSequence,
		Core->Phase == Cdp_CORE_PHASE_PREVIEW ?
			Core->PreviewTargetSequence : 0,
		&previewTargetDeleted);
	if (!NT_SUCCESS(status))
		goto cleanup;
	if (previewTargetDeleted && Core->Phase == Cdp_CORE_PHASE_PREVIEW)
	{
		CdpPreviewTreeFree(&Core->PreviewTree);
		CdpPreviewTreeInitialize(&Core->PreviewTree);
		Core->Building = 0;
		Core->PreviewTargetSequence = 0;
		Core->Phase = Cdp_CORE_PHASE_GENERAL;
		Core->PreviewStoppedByMerge = TRUE;
	}

	// Build the current-branch view local to the region being deleted. Newer
	// regions deliberately do not suppress these values: the source becomes
	// the complete base produced by this region, while newer MetaTree records
	// continue to overlay it.
	status = CdpJournalBuildCurrentBranchRegionTree(
		Core->Journal, firstSequence, endSequence, &regionTree);
	if (!NT_SUCCESS(status))
		goto cleanup;
	regionTreeInitialized = TRUE;
	status = CdpCoreMaterializeMetaNodes(
		Core, regionTree.Root, firstSequence, endSequence);
	if (!NT_SUCCESS(status))
	{
#ifndef Cdp_USERMODE
		Cdp_LOG("[MERGE] region materialize failed rrOffset=%llu sequence=[%llu,%llu) liveNodes=%lu status=0x%08X\n",
			regionOffset, firstSequence, endSequence,
			regionTree.NodeCount, status);
#endif
		goto cleanup;
	}

	// Update only MetaTree coverage that still points into the deleted region.
	// Intersections owned by newer regions remain intact and keep overriding
	// the newly materialized source baseline.
	for (;;)
	{
		PCdp_PREVIEW_TREE_NODE node =
			CdpCoreFindMetaNodeBySequenceRange(
				Core->MetaTree.Root, firstSequence, endSequence);
		UINT64 start;
		ULONG length;
		if (!node)
			break;
		start = node->Start;
		length = node->DataLength;
		status = CdpPreviewTreePunchRange(
			&Core->MetaTree, start, length);
		if (!NT_SUCCESS(status))
		{
			Core->MetaTreeReady = FALSE;
			goto cleanup;
		}
	}

	// If Preview remains active, records reclaimed below its target boundary
	// now live in the source baseline. Remove only PreviewTree fragments still
	// pointing into this region; newer retained preview records stay overlaid.
	if (Core->Phase == Cdp_CORE_PHASE_PREVIEW)
	{
		for (;;)
		{
			PCdp_PREVIEW_TREE_NODE node =
				CdpCoreFindMetaNodeBySequenceRange(
					Core->PreviewTree.Root, firstSequence, endSequence);
			UINT64 start;
			ULONG length;
			if (!node)
				break;
			start = node->Start;
			length = node->DataLength;
			status = CdpPreviewTreePunchRange(
				&Core->PreviewTree, start, length);
			if (!NT_SUCCESS(status))
				goto cleanup;
		}
	}

	// The source now contains every live value referenced by this region.
	// Only after that point is its header/payload span made reclaimable.
	status = CdpJournalDeleteOldestRegion(
		Core->Journal, regionOffset);
	if (NT_SUCCESS(status))
	{
		ULONG deletedTombstoneRegions = 0;
		status = CdpJournalDeleteContiguousTombstonedRegions(
			Core->Journal, &deletedTombstoneRegions);
		if (NT_SUCCESS(status))
		{
#ifndef Cdp_USERMODE
			Cdp_LOG("[MERGE] region complete rrOffset=%llu sequence=[%llu,%llu) liveNodes=%lu tombstoneRrs=%lu\n",
				regionOffset, firstSequence, endSequence,
				regionTree.NodeCount, deletedTombstoneRegions);
#endif
		}
		Cdp_RECOVERY_TRACE(
			"compaction reclaimed primary RR plus %lu contiguous tombstone RRs\n",
			deletedTombstoneRegions);
	}

cleanup:
	if (regionTreeInitialized)
		CdpPreviewTreeFree(&regionTree);
	Cdp_LOCK_RELEASE(&Core->TreeLock);
	return status;
}

NTSTATUS CdpCoreQueryRecordHeaders(
	_Inout_ PCdp_CORE Core,
	_In_ UINT64 StartIndex,
	_In_ UINT64 ExpectedGeneration,
	_Out_writes_to_(RecordCapacity, *ReturnedCount) PCdp_JOURNAL_RECORD Records,
	_In_ ULONG RecordCapacity,
	_Out_ PUINT64 TotalRecords,
	_Out_ PUINT64 Generation,
	_Out_ PULONG ReturnedCount)
{
	if (!Core)
		return STATUS_INVALID_PARAMETER;
	return CdpJournalQueryRecordHeaders(
		Core->Journal,
		StartIndex,
		ExpectedGeneration,
		Records,
		RecordCapacity,
		TotalRecords,
		Generation,
		ReturnedCount);
}

NTSTATUS CdpCoreQueryRuntimeCheckpointInfos(
	_Inout_ PCdp_CORE Core,
	_In_ UINT64 StartIndex,
	_In_ UINT64 ExpectedGeneration,
	_Out_writes_to_(InfoCapacity, *ReturnedCount)
		PCdp_RUNTIME_CHECKPOINT_TREE_INFO Infos,
	_In_ ULONG InfoCapacity,
	_Out_ PUINT64 TotalCheckpoints,
	_Out_ PUINT64 Generation,
	_Out_ PULONG ReturnedCount)
{
	if (!Core || !Core->Journal)
		return STATUS_INVALID_PARAMETER;
	return CdpJournalQueryRuntimeCheckpointInfos(
		Core->Journal, StartIndex, ExpectedGeneration, Infos, InfoCapacity,
		TotalCheckpoints, Generation, ReturnedCount);
}

NTSTATUS CdpCoreQueryRuntimeCheckpointRecords(
	_Inout_ PCdp_CORE Core,
	_In_ UINT64 CheckpointId,
	_In_ UINT64 StartIndex,
	_In_ UINT64 ExpectedGeneration,
	_Out_writes_to_(RecordCapacity, *ReturnedCount)
		PCdp_RUNTIME_CHECKPOINT_RECORD_TREE_INFO Records,
	_In_ ULONG RecordCapacity,
	_Out_ PUINT64 TotalRecords,
	_Out_ PUINT64 Generation,
	_Out_ PULONG ReturnedCount)
{
	if (!Core || !Core->Journal)
		return STATUS_INVALID_PARAMETER;
	return CdpJournalQueryRuntimeCheckpointRecords(
		Core->Journal, CheckpointId, StartIndex, ExpectedGeneration,
		Records, RecordCapacity, TotalRecords, Generation, ReturnedCount);
}

NTSTATUS CdpCoreQueryBranches(
	_Inout_ PCdp_CORE Core,
	_In_ UINT64 StartIndex,
	_In_ UINT64 ExpectedGeneration,
	_Out_writes_to_(BranchCapacity, *ReturnedCount) PCdp_JOURNAL_BRANCH_TREE_INFO Branches,
	_In_ ULONG BranchCapacity,
	_Out_ PULONG TotalBranches,
	_Out_ PLONG CurrentBranchNumber,
	_Out_ PUINT64 Generation,
	_Out_ PULONG ReturnedCount)
{
	if (!Core)
		return STATUS_INVALID_PARAMETER;
	return CdpJournalQueryBranches(
		Core->Journal,
		StartIndex,
		ExpectedGeneration,
		Branches,
		BranchCapacity,
		TotalBranches,
		CurrentBranchNumber,
		Generation,
		ReturnedCount);
}

Cdp_CORE_PHASE CdpCoreGetPhase(_In_ PCdp_CORE Core)
{
	if (!Core)
		return Cdp_CORE_PHASE_GENERAL;
	return (Cdp_CORE_PHASE)Core->Phase;
}

BOOLEAN CdpCoreHasPendingRecoveryBranch(_In_ PCdp_CORE Core)
{
	BOOLEAN pending;

	if (!Core)
		return FALSE;
	Cdp_LOCK_ACQUIRE(&Core->TreeLock);
	pending = Core->PendingRecoveryBranch;
	Cdp_LOCK_RELEASE(&Core->TreeLock);
	return pending;
}

/* TreeLock is held by the append path. A previous attempt can have written
 * and attached the branch marker but failed while publishing/clearing the
 * superblock. In that case CurrentBranchNumber already identifies the planned
 * branch, so retry only the recovery-intent completion instead of creating a
 * duplicate branch. */
static NTSTATUS CdpCoreMaterializePendingRecoveryBranchLocked(
	_Inout_ PCdp_CORE Core)
{
	NTSTATUS status;
	LONG currentBranch;

	if (!Core->PendingRecoveryBranch)
		return STATUS_SUCCESS;
	currentBranch = Core->Journal->CurrentBranchNumber;
	if (currentBranch != Core->PendingRecoveryBranchNumber)
	{
		if (currentBranch != Core->PendingRecoveryParentBranch ||
			Core->Journal->HighestBranchNumber + 1 !=
				Core->PendingRecoveryBranchNumber)
		{
			return STATUS_INVALID_DEVICE_STATE;
		}
		status = CdpJournalAppendBranch(
			Core->Journal,
			Core->PendingRecoveryBranchNumber,
			Core->PendingRecoveryParentBranch,
			Core->PendingRecoveryInheritedSequence);
		if (!NT_SUCCESS(status))
		{
#ifndef Cdp_USERMODE
			Cdp_LOG("[RECOVERY-DEFERRED-FAIL] stage=append-branch status=0x%08X branch=%ld parent=%ld inherit=%llu current=%ld\n",
				status,
				Core->PendingRecoveryBranchNumber,
				Core->PendingRecoveryParentBranch,
				Core->PendingRecoveryInheritedSequence,
				Core->Journal->CurrentBranchNumber);
#endif
			return status;
		}
	}
	status = CdpJournalCompleteRecoveryIntent(Core->Journal);
	if (!NT_SUCCESS(status))
	{
#ifndef Cdp_USERMODE
		Cdp_LOG("[RECOVERY-DEFERRED-FAIL] stage=clear-intent status=0x%08X branch=%ld\n",
			status, Core->PendingRecoveryBranchNumber);
#endif
		return status;
	}
#ifndef Cdp_USERMODE
	Cdp_LOG("[RECOVERY-DEFERRED] branch materialized before first write branch=%ld parent=%ld inherit=%llu\n",
		Core->PendingRecoveryBranchNumber,
		Core->PendingRecoveryParentBranch,
		Core->PendingRecoveryInheritedSequence);
#endif
	Core->PendingRecoveryBranch = FALSE;
	Core->PendingRecoveryParentBranch = 0;
	Core->PendingRecoveryBranchNumber = 0;
	Core->PendingRecoveryInheritedSequence = 0;
	return STATUS_SUCCESS;
}

static NTSTATUS CdpCoreMaterializePendingRestoreResetLocked(
	_Inout_ PCdp_CORE Core)
{
	NTSTATUS status;

	if (!Core->PendingRestoreReset)
		return STATUS_SUCCESS;
	/* A recovery intent always wins. This state combination indicates an
	 * activation ordering bug, so fail closed instead of deleting history. */
	if (Core->PendingRecoveryBranch || Core->Journal->RecoveryPending)
		return STATUS_INVALID_DEVICE_STATE;
	status = CdpJournalResetHistoryPreserveRestorePoint(Core->Journal);
	if (!NT_SUCCESS(status))
	{
#ifndef Cdp_USERMODE
		Cdp_LOG("[RESTORE-POINT-FAIL] stage=reset-before-first-write status=0x%08X\n",
			status);
#endif
		return status;
	}
	Core->PendingRestoreReset = FALSE;
#ifndef Cdp_USERMODE
	Cdp_LOG("[RESTORE-POINT] old history discarded; fresh root branch persisted before first write\n");
#endif
	return STATUS_SUCCESS;
}

NTSTATUS CdpCoreAppendAfterImage(
	_Inout_ PCdp_CORE Core,
	_In_ UINT64 Offset,
	_In_ ULONG Length,
	_In_reads_bytes_(Length) const VOID* AfterImage,
	_Out_opt_ PCdp_JOURNAL_RECORD WrittenRecord)
{
	NTSTATUS status;
	Cdp_JOURNAL_RECORD record;
	ULONG nodeCountBefore = 0;
	ULONG nodeCountAfter = 0;
	if (!Core || !AfterImage || Length == 0 ||
		Length > Cdp_JOURNAL_MAX_RECORD_DATA)
	{
		return STATUS_INVALID_PARAMETER;
	}
	if (!Core->Journal->Mounted)
		return STATUS_DEVICE_NOT_READY;
	if (!Core->MetaTreeReady)
		return STATUS_DEVICE_NOT_READY;

	Cdp_LOCK_ACQUIRE(&Core->TreeLock);
	if (!Core->MetaTreeReady)
	{
		Cdp_LOCK_RELEASE(&Core->TreeLock);
		return STATUS_DEVICE_NOT_READY;
	}
	nodeCountBefore = Core->MetaTree.NodeCount;
	status = CdpCoreMaterializePendingRestoreResetLocked(Core);
	if (!NT_SUCCESS(status))
	{
		Cdp_LOCK_RELEASE(&Core->TreeLock);
		return status;
	}
	status = CdpCoreMaterializePendingRecoveryBranchLocked(Core);
	if (!NT_SUCCESS(status))
	{
		Cdp_LOCK_RELEASE(&Core->TreeLock);
		return status;
	}
	status = CdpJournalAppendEx(
		Core->Journal,
		Offset,
		Length,
		AfterImage,
		0,
		&record);
	if (!NT_SUCCESS(status))
	{
#ifndef Cdp_USERMODE
		Cdp_LOG("[REDIRECT-WRITE-FAIL] stage=journal offset=%llu len=%lu status=0x%08X\n",
			Offset, Length, status);
#endif
	}
	if (NT_SUCCESS(status))
	{
		status = CdpPreviewTreeOverlayLatest(&Core->MetaTree, &record);
		if (!NT_SUCCESS(status))
		{
#ifndef Cdp_USERMODE
			Cdp_LOG("[REDIRECT-WRITE-FAIL] stage=metatree offset=%llu len=%lu sequence=%llu payloadOffset=%llu status=0x%08X\n",
				Offset, Length, record.Sequence, record.FileOffset, status);
#endif
			Core->MetaTreeReady = FALSE;
		}
	}
	nodeCountAfter = Core->MetaTree.NodeCount;
	Cdp_LOCK_RELEASE(&Core->TreeLock);
	if (NT_SUCCESS(status))
	{
		Core->Time100ns += 1;
		if (WrittenRecord)
			*WrittenRecord = record;
	}
	return status;
}

static PCdp_PREVIEW_TREE_NODE CdpCoreFindFirstValidMetaNode(
	_In_opt_ PCdp_PREVIEW_TREE_NODE Node)
{
	PCdp_PREVIEW_TREE_NODE found;

	if (!Node)
		return NULL;
	found = CdpCoreFindFirstValidMetaNode(Node->Left);
	if (found)
		return found;
	if (!Node->Invalid && Node->DataLength != 0)
		return Node;
	return CdpCoreFindFirstValidMetaNode(Node->Right);
}

NTSTATUS CdpCoreDrainOneMetaRangeWithWriter(
	_Inout_ PCdp_CORE Core,
	_In_ Cdp_CORE_DRAIN_WRITE_ROUTINE WriteRoutine,
	_In_opt_ PVOID WriteContext,
	_Out_ PBOOLEAN Complete,
	_Out_opt_ PUINT64 DrainedOffset,
	_Out_opt_ PULONG DrainedLength)
{
	PCdp_PREVIEW_TREE_NODE node;
	PVOID payload = NULL;
	UINT64 offset = 0;
	ULONG length = 0;
	NTSTATUS status = STATUS_SUCCESS;

	if (!Core || !WriteRoutine || !Complete)
		return STATUS_INVALID_PARAMETER;
	*Complete = FALSE;
	if (DrainedOffset)
		*DrainedOffset = 0;
	if (DrainedLength)
		*DrainedLength = 0;

	Cdp_LOCK_ACQUIRE(&Core->TreeLock);
	if (!Core->MetaTreeReady)
	{
		status = STATUS_DEVICE_NOT_READY;
		goto cleanup;
	}
	node = CdpCoreFindFirstValidMetaNode(Core->MetaTree.Root);
	if (!node)
	{
		*Complete = TRUE;
		goto cleanup;
	}
	offset = node->Start;
	length = node->DataLength;
	payload = Cdp_ALLOC(length);
	if (!payload)
	{
		status = STATUS_INSUFFICIENT_RESOURCES;
		goto cleanup;
	}
	status = CdpJournalReadPayload(
		Core->Journal, node->FileOffset, length, payload);
	if (NT_SUCCESS(status))
		status = WriteRoutine(WriteContext, offset, length, payload);
	if (NT_SUCCESS(status))
	{
		status = CdpPreviewTreePunchRange(&Core->MetaTree, offset, length);
		if (!NT_SUCCESS(status))
			Core->MetaTreeReady = FALSE;
	}
	if (NT_SUCCESS(status))
	{
		if (DrainedOffset)
			*DrainedOffset = offset;
		if (DrainedLength)
			*DrainedLength = length;
		*Complete = CdpCoreFindFirstValidMetaNode(Core->MetaTree.Root) == NULL;
	}

cleanup:
	if (payload)
		Cdp_FREE(payload);
	Cdp_LOCK_RELEASE(&Core->TreeLock);
	return status;
}

NTSTATUS CdpCorePunchMetaRange(
	_Inout_ PCdp_CORE Core,
	_In_ UINT64 Offset,
	_In_ ULONG Length)
{
	NTSTATUS status;

	if (!Core || Length == 0 || Offset > MAXUINT64 - Length)
		return STATUS_INVALID_PARAMETER;
	Cdp_LOCK_ACQUIRE(&Core->TreeLock);
	if (!Core->MetaTreeReady)
	{
		status = STATUS_DEVICE_NOT_READY;
	}
	else
	{
		status = CdpPreviewTreePunchRange(&Core->MetaTree, Offset, Length);
		if (!NT_SUCCESS(status))
			Core->MetaTreeReady = FALSE;
	}
	Cdp_LOCK_RELEASE(&Core->TreeLock);
	return status;
}

static NTSTATUS CdpCoreSynthesizeRead(
	_Inout_ PCdp_CORE Core,
	_In_ PCdp_PREVIEW_TREE Tree,
	_In_ UINT64 Offset,
	_In_ ULONG Length,
	_Out_writes_bytes_(Length) PVOID Buffer,
	_In_ BOOLEAN ReadSourceBaseline)
{
	PUCHAR coveredMask = NULL;
	ULONG coveredCount = 0;
	NTSTATUS status;
	ULONG maskBytes = (Length + 7UL) / 8UL;
	ULONG cursor;

	coveredMask = (PUCHAR)Cdp_ALLOC0(maskBytes);
	if (!coveredMask)
	{
		status = STATUS_INSUFFICIENT_RESOURCES;
		goto done;
	}

	/* Resolve the current-value map first. Only holes are fetched from the
	 * source. This is the DiskDrive-upper fast path: a fully covered request
	 * never touches the source partition. */
	status = CdpJournalApplyPreviewTree(
		Core->Journal,
		Tree,
		&Core->TreeLock,
		Offset,
		Length,
		Buffer,
		coveredMask,
		&coveredCount);
	if (!NT_SUCCESS(status))
		goto done;
	if (ReadSourceBaseline && coveredCount < Length)
	{
		cursor = 0;
		while (cursor < Length)
		{
			ULONG gapStart;
			ULONG gapEnd;
			UINT64 absoluteStart;
			UINT64 alignedStart;
			UINT64 alignedEnd;
			ULONG alignedLength;
			PUCHAR sourceBuffer;

			while (cursor < Length &&
				(coveredMask[cursor >> 3] & (UCHAR)(1u << (cursor & 7))) != 0)
				++cursor;
			if (cursor == Length)
				break;
			gapStart = cursor;
			while (cursor < Length &&
				(coveredMask[cursor >> 3] & (UCHAR)(1u << (cursor & 7))) == 0)
				++cursor;
			gapEnd = cursor;
			absoluteStart = Offset + gapStart;
			alignedStart = absoluteStart -
				(absoluteStart % Core->Source->SectorSize);
			alignedEnd = Offset + gapEnd;
			if (alignedEnd > MAXUINT64 - (Core->Source->SectorSize - 1))
			{
				status = STATUS_INTEGER_OVERFLOW;
				goto done;
			}
			alignedEnd = ((alignedEnd + Core->Source->SectorSize - 1) /
				Core->Source->SectorSize) * Core->Source->SectorSize;
			if (alignedEnd > Core->Source->Size)
				alignedEnd = Core->Source->Size;
			if (alignedEnd <= alignedStart ||
				alignedEnd - alignedStart > MAXULONG)
			{
				status = STATUS_INVALID_PARAMETER;
				goto done;
			}
			alignedLength = (ULONG)(alignedEnd - alignedStart);
			sourceBuffer = (PUCHAR)Cdp_ALLOC(alignedLength);
			if (!sourceBuffer)
			{
				status = STATUS_INSUFFICIENT_RESOURCES;
				goto done;
			}
			status = CdpCoreSourceRead(
				Core, alignedStart, alignedLength, sourceBuffer);
			if (NT_SUCCESS(status))
			{
				RtlCopyMemory(
					(PUCHAR)Buffer + gapStart,
					sourceBuffer + (ULONG)(absoluteStart - alignedStart),
					gapEnd - gapStart);
			}
			Cdp_FREE(sourceBuffer);
			if (!NT_SUCCESS(status))
				goto done;
		}
	}
done:
	Cdp_FREE(coveredMask);
	return status;
}

/* MetaTree and PreviewTree store after-image payload locations. Missing ranges
 * are read from the source baseline. */
NTSTATUS CdpCoreRead(
	_Inout_ PCdp_CORE Core,
	_In_ UINT64 Offset,
	_In_ ULONG Length,
	_Out_writes_bytes_(Length) PVOID Buffer)
{
	if (!Core || !Buffer || Length == 0)
		return STATUS_INVALID_PARAMETER;
	if (Core->Phase == Cdp_CORE_PHASE_PREVIEW)
		return CdpCoreSynthesizeRead(
			Core, &Core->PreviewTree, Offset, Length, Buffer, TRUE);
	if (!Core->MetaTreeReady)
		return STATUS_DEVICE_NOT_READY;

	return CdpCoreSynthesizeRead(
		Core, &Core->MetaTree, Offset, Length, Buffer, TRUE);
}

NTSTATUS CdpCoreOverlayCurrentRead(
	_Inout_ PCdp_CORE Core,
	_In_ UINT64 Offset,
	_In_ ULONG Length,
	_Inout_updates_bytes_(Length) PVOID Buffer)
{
	if (!Core || !Buffer || Length == 0)
		return STATUS_INVALID_PARAMETER;
	if (Core->Phase == Cdp_CORE_PHASE_PREVIEW)
		return CdpCoreSynthesizeRead(
			Core, &Core->PreviewTree, Offset, Length, Buffer, FALSE);
	if (!Core->MetaTreeReady)
		return STATUS_DEVICE_NOT_READY;

	/* Buffer already contains the source-disk baseline. Apply only journal
	 * ranges; holes deliberately retain the bytes returned by Disk Lower. */
	return CdpCoreSynthesizeRead(
		Core, &Core->MetaTree, Offset, Length, Buffer, FALSE);
}

typedef struct _Cdp_CORE_COVERAGE_SCAN
{
	UINT64 Start;
	UINT64 End;
	UINT64 Cursor;
	UINT64 FirstGap;
	UINT64 LastGapEnd;
	ULONG GapCount;
	BOOLEAN HasGap;
	BOOLEAN HasCoverage;
} Cdp_CORE_COVERAGE_SCAN, *PCdp_CORE_COVERAGE_SCAN;

static VOID CdpCoreRecordCoverageGap(
	_Inout_ PCdp_CORE_COVERAGE_SCAN Scan,
	_In_ UINT64 Start,
	_In_ UINT64 End)
{
	if (Start >= End)
		return;
	if (!Scan->HasGap)
	{
		Scan->FirstGap = Start;
		Scan->HasGap = TRUE;
	}
	Scan->LastGapEnd = End;
	Scan->GapCount += 1;
}

static VOID CdpCoreScanTreeCoverage(
	_In_opt_ PCdp_PREVIEW_TREE_NODE Node,
	_Inout_ PCdp_CORE_COVERAGE_SCAN Scan)
{
	UINT64 nodeStart;
	UINT64 nodeEnd;

	if (!Node || Node->MaxEnd <= Scan->Start || Scan->Cursor >= Scan->End)
		return;
	CdpCoreScanTreeCoverage(Node->Left, Scan);
	if (Scan->Cursor >= Scan->End || Node->Start >= Scan->End)
		return;
	if (!Node->Invalid && Node->End > Scan->Start)
	{
		nodeStart = Node->Start > Scan->Start ? Node->Start : Scan->Start;
		nodeEnd = Node->End < Scan->End ? Node->End : Scan->End;
		if (nodeStart < nodeEnd)
		{
			Scan->HasCoverage = TRUE;
			if (nodeStart > Scan->Cursor)
				CdpCoreRecordCoverageGap(Scan, Scan->Cursor, nodeStart);
			if (nodeEnd > Scan->Cursor)
				Scan->Cursor = nodeEnd;
		}
	}
	CdpCoreScanTreeCoverage(Node->Right, Scan);
}

NTSTATUS CdpCoreQueryCurrentReadCoverage(
	_Inout_ PCdp_CORE Core,
	_In_ UINT64 Offset,
	_In_ ULONG Length,
	_Out_ Cdp_CORE_READ_COVERAGE* Coverage,
	_Out_ PUINT64 SourceOffset,
	_Out_ PULONG SourceLength)
{
	PCdp_PREVIEW_TREE tree;
	Cdp_CORE_COVERAGE_SCAN scan;
	NTSTATUS status = STATUS_SUCCESS;

	if (!Core || !Coverage || !SourceOffset || !SourceLength || Length == 0 ||
		Offset > MAXUINT64 - Length)
	{
		return STATUS_INVALID_PARAMETER;
	}
	*Coverage = Cdp_CORE_READ_COVERAGE_NONE;
	*SourceOffset = Offset;
	*SourceLength = Length;
	RtlZeroMemory(&scan, sizeof(scan));
	scan.Start = Offset;
	scan.End = Offset + Length;
	scan.Cursor = Offset;
	Cdp_LOCK_ACQUIRE(&Core->TreeLock);
	if (Core->Phase == Cdp_CORE_PHASE_PREVIEW)
	{
		tree = &Core->PreviewTree;
	}
	else if (!Core->MetaTreeReady)
	{
		status = STATUS_DEVICE_NOT_READY;
		tree = NULL;
	}
	else
	{
		tree = &Core->MetaTree;
	}
	if (tree)
	{
		CdpCoreScanTreeCoverage(tree->Root, &scan);
		if (scan.Cursor < scan.End)
			CdpCoreRecordCoverageGap(&scan, scan.Cursor, scan.End);
		if (!scan.HasCoverage)
		{
			*Coverage = Cdp_CORE_READ_COVERAGE_NONE;
		}
		else if (!scan.HasGap)
		{
			*Coverage = Cdp_CORE_READ_COVERAGE_FULL;
			*SourceOffset = Offset;
			*SourceLength = 0;
		}
		else
		{
			*Coverage = Cdp_CORE_READ_COVERAGE_PARTIAL;
			if (scan.GapCount == 1)
			{
				*SourceOffset = scan.FirstGap;
				*SourceLength = (ULONG)(scan.LastGapEnd - scan.FirstGap);
			}
			else
			{
				*SourceOffset = Offset;
				*SourceLength = Length;
			}
		}
	}
	Cdp_LOCK_RELEASE(&Core->TreeLock);
	return status;
}

static PCdp_PREVIEW_TREE_NODE CdpCoreFindFirstOverlapNode(
	_In_opt_ PCdp_PREVIEW_TREE_NODE Node,
	_In_ UINT64 Start,
	_In_ UINT64 End)
{
	PCdp_PREVIEW_TREE_NODE found;

	if (!Node || Node->MaxEnd <= Start)
		return NULL;
	found = CdpCoreFindFirstOverlapNode(Node->Left, Start, End);
	if (found)
		return found;
	if (!Node->Invalid && Node->Start < End && Start < Node->End)
		return Node;
	if (Node->Start >= End)
		return NULL;
	return CdpCoreFindFirstOverlapNode(Node->Right, Start, End);
}

NTSTATUS CdpCorePreviewRead(
	_Inout_ PCdp_CORE Core,
	_In_ UINT64 Offset,
	_In_ ULONG Length,
	_Out_writes_bytes_(Length) PVOID Buffer)
{
	if (!Core || !Buffer || Length == 0)
		return STATUS_INVALID_PARAMETER;
	if (Core->Phase != Cdp_CORE_PHASE_PREVIEW || Core->Building)
		return STATUS_INVALID_DEVICE_STATE;
	return CdpCoreSynthesizeRead(
		Core, &Core->PreviewTree, Offset, Length, Buffer, TRUE);
}

static NTSTATUS CdpCoreResolveTargetTime(
	_Inout_ PCdp_CORE Core,
	_In_ UINT64 RequestedTime100ns,
	_Out_ PUINT64 EffectiveTime100ns)
{
	UINT64 oldestTime;
	UINT64 newestTime;
	NTSTATUS status;

	if (!Core || !EffectiveTime100ns)
		return STATUS_INVALID_PARAMETER;
	*EffectiveTime100ns = RequestedTime100ns;
	status = CdpJournalQueryTimeRange(
		Core->Journal,
		&oldestTime,
		&newestTime);
	if (status == STATUS_NOT_FOUND)
		return STATUS_SUCCESS;
	if (!NT_SUCCESS(status))
		return status;
	if (RequestedTime100ns < oldestTime)
		*EffectiveTime100ns = oldestTime;
	return STATUS_SUCCESS;
}

NTSTATUS CdpCorePreviewBegin(_Inout_ PCdp_CORE Core, _In_ UINT64 TargetTime100ns)
{
	NTSTATUS status;
	UINT64 effectiveTargetTime100ns;
	UINT64 targetRecordSequence = 0;
	Cdp_JOURNAL_RECORD_LOCATION targetLocation;

	if (!Core)
		return STATUS_INVALID_PARAMETER;
	if (!Core->Journal->Mounted)
		return STATUS_DEVICE_NOT_READY;
	status = CdpCoreResolveTargetTime(
		Core, TargetTime100ns, &effectiveTargetTime100ns);
	if (!NT_SUCCESS(status))
		return status;
	Cdp_LOCK_ACQUIRE(&Core->TreeLock);
	if (Core->Phase != Cdp_CORE_PHASE_GENERAL ||
		Core->PendingRecoveryBranch)
	{
		Cdp_LOCK_RELEASE(&Core->TreeLock);
		return STATUS_INVALID_DEVICE_STATE;
	}
	if (Core->MergeActive)
	{
		Cdp_LOCK_RELEASE(&Core->TreeLock);
		return STATUS_DEVICE_BUSY;
	}
	Core->Phase = Cdp_CORE_PHASE_PREVIEW;
	Core->Building = 1;
	Core->PreviewStoppedByMerge = FALSE;
	Core->TargetTime100ns = effectiveTargetTime100ns;
	Core->PreviewTargetSequence = 0;
	CdpPreviewTreeFree(&Core->PreviewTree);
	CdpPreviewTreeInitialize(&Core->PreviewTree);
	Cdp_LOCK_RELEASE(&Core->TreeLock);

	RtlZeroMemory(&targetLocation, sizeof(targetLocation));
	status = CdpJournalBuildSettledPreviewTree(
		Core->Journal,
		effectiveTargetTime100ns,
		Core->Journal->NextSequence,
		10,
		&Core->PreviewTree,
		&effectiveTargetTime100ns,
		&targetRecordSequence,
		&targetLocation);
	if (!NT_SUCCESS(status) && status != STATUS_NOT_FOUND)
	{
		Cdp_LOCK_ACQUIRE(&Core->TreeLock);
		Core->Building = 0;
		Core->Phase = Cdp_CORE_PHASE_GENERAL;
		Cdp_LOCK_RELEASE(&Core->TreeLock);
		return status;
	}
	if (status == STATUS_NOT_FOUND)
		status = STATUS_SUCCESS;

	Cdp_LOCK_ACQUIRE(&Core->TreeLock);
	Core->TargetTime100ns = effectiveTargetTime100ns;
	Core->PreviewTargetSequence = targetRecordSequence;
	Core->Building = 0;
	Cdp_LOCK_RELEASE(&Core->TreeLock);
	CdpCoreTraceTargetRecord(
		"PREVIEW",
		TargetTime100ns,
		effectiveTargetTime100ns,
		&targetLocation);

	return STATUS_SUCCESS;
}

NTSTATUS CdpCorePreviewEnd(_Inout_ PCdp_CORE Core)
{
	if (!Core)
		return STATUS_INVALID_PARAMETER;
	Cdp_LOCK_ACQUIRE(&Core->TreeLock);
	if (Core->Phase != Cdp_CORE_PHASE_PREVIEW)
	{
		Cdp_LOCK_RELEASE(&Core->TreeLock);
		return STATUS_INVALID_DEVICE_STATE;
	}
	CdpPreviewTreeFree(&Core->PreviewTree);
	CdpPreviewTreeInitialize(&Core->PreviewTree);
	Core->PreviewTargetSequence = 0;
	Core->Phase = Cdp_CORE_PHASE_GENERAL;
	Cdp_LOCK_RELEASE(&Core->TreeLock);
	return STATUS_SUCCESS;
}

NTSTATUS CdpCoreRecoveryCommitStep(
	_Inout_ PCdp_CORE Core,
	_Out_ PBOOLEAN Complete)
{
	if (!Core || !Complete)
		return STATUS_INVALID_PARAMETER;
	if (Core->Building || Core->Phase != Cdp_CORE_PHASE_GENERAL)
		return STATUS_INVALID_DEVICE_STATE;
	// After-image Recovery is complete when Begin publishes the replacement
	// MetaTree. Commit remains as an idempotent compatibility operation and
	// never writes data to the source volume.
	*Complete = TRUE;
	return STATUS_SUCCESS;
}

NTSTATUS CdpCorePrepareRebootRecovery(
	_Inout_ PCdp_CORE Core,
	_In_ UINT64 TargetTime100ns)
{
	Cdp_PREVIEW_TREE newTree;
	Cdp_PREVIEW_TREE oldTree;
	LONG parentBranch = 0;
	LONG newBranch = 0;
	UINT64 inheritedSequence = 0;
	Cdp_JOURNAL_RECORD_LOCATION targetLocation;
	UINT64 effectiveTargetTime100ns;
	UINT64 previousTargetTime100ns = 0;
	ULONG publishedNodeCount = 0;
	BOOLEAN newTreeInitialized = FALSE;
	const char* failureStage = "entry";
	NTSTATUS status;

	if (!Core)
		return STATUS_INVALID_PARAMETER;
	if (!Core->Journal->Mounted)
		return STATUS_DEVICE_NOT_READY;
	failureStage = "resolve-target-time";
	status = CdpCoreResolveTargetTime(
		Core, TargetTime100ns, &effectiveTargetTime100ns);
	if (!NT_SUCCESS(status))
		return status;

	Cdp_LOCK_ACQUIRE(&Core->TreeLock);
	if (Core->Phase != Cdp_CORE_PHASE_GENERAL || Core->Building ||
		Core->PendingRecoveryBranch)
	{
		Cdp_LOCK_RELEASE(&Core->TreeLock);
		return STATUS_INVALID_DEVICE_STATE;
	}
	if (Core->MergeActive)
	{
		Cdp_LOCK_RELEASE(&Core->TreeLock);
		return STATUS_DEVICE_BUSY;
	}
	Core->Phase = Cdp_CORE_PHASE_RECOVERY;
	Core->Building = 1;
	previousTargetTime100ns = Core->TargetTime100ns;
	Core->TargetTime100ns = effectiveTargetTime100ns;
	Cdp_LOCK_RELEASE(&Core->TreeLock);

	failureStage = "build-target-metatree";
	RtlZeroMemory(&targetLocation, sizeof(targetLocation));
	status = CdpJournalBuildPreviewTreeEx(
		Core->Journal,
		effectiveTargetTime100ns,
		Core->Journal->NextSequence,
		TRUE,
		&newTree,
		&parentBranch,
		&inheritedSequence,
		&targetLocation);
	if (!NT_SUCCESS(status) && status != STATUS_NOT_FOUND)
		goto failure;
	if (status == STATUS_NOT_FOUND)
	{
		CdpPreviewTreeInitialize(&newTree);
		status = STATUS_SUCCESS;
	}
	newTreeInitialized = TRUE;
	CdpCoreTraceTargetRecord(
		"REBOOT-RECOVERY",
		TargetTime100ns,
		effectiveTargetTime100ns,
		&targetLocation);
	if (Core->Journal->HighestBranchNumber >= 0x7FFFFFFFL)
	{
		status = STATUS_INTEGER_OVERFLOW;
		failureStage = "reserve-branch-number";
		goto failure;
	}
	newBranch = Core->Journal->HighestBranchNumber + 1;

	Cdp_LOCK_ACQUIRE(&Core->TreeLock);
	oldTree = Core->MetaTree;
	Core->MetaTree = newTree;
	Core->MetaTreeReady = TRUE;
	Core->PendingRecoveryParentBranch = parentBranch;
	Core->PendingRecoveryBranchNumber = newBranch;
	Core->PendingRecoveryInheritedSequence = inheritedSequence;
	Core->PendingRecoveryBranch = TRUE;
	Core->Building = 0;
	Core->Phase = Cdp_CORE_PHASE_GENERAL;
	publishedNodeCount = Core->MetaTree.NodeCount;
	Cdp_LOCK_RELEASE(&Core->TreeLock);
	newTreeInitialized = FALSE;
	CdpPreviewTreeFree(&oldTree);
#ifndef Cdp_USERMODE
	Cdp_LOG("[RECOVERY-DEFERRED] target view published target=%llu parent=%ld inherit=%llu pendingBranch=%ld nodes=%lu; no boot write issued\n",
		effectiveTargetTime100ns,
		parentBranch,
		inheritedSequence,
		newBranch,
		publishedNodeCount);
#endif
	return STATUS_SUCCESS;

failure:
#ifndef Cdp_USERMODE
	Cdp_LOG("[RECOVERY-DEFERRED-FAIL] stage=%s status=0x%08X requested=%llu effective=%llu parent=%ld inherit=%llu\n",
		failureStage,
		status,
		TargetTime100ns,
		effectiveTargetTime100ns,
		parentBranch,
		inheritedSequence);
#endif
	if (newTreeInitialized)
		CdpPreviewTreeFree(&newTree);
	Cdp_LOCK_ACQUIRE(&Core->TreeLock);
	Core->Building = 0;
	Core->TargetTime100ns = previousTargetTime100ns;
	Core->Phase = Cdp_CORE_PHASE_GENERAL;
	Cdp_LOCK_RELEASE(&Core->TreeLock);
	return status;
}

NTSTATUS CdpCorePreparePersistentRestoreBoot(
	_Inout_ PCdp_CORE Core,
	_In_ Cdp_CORE_DRAIN_WRITE_ROUTINE WriteRoutine,
	_In_opt_ PVOID WriteContext,
	_Out_opt_ PBOOLEAN PreviousBootConfirmed,
	_Out_opt_ PULONG MaterializedRanges,
	_Out_opt_ PUINT64 MaterializedBytes)
{
	Cdp_PREVIEW_TREE oldTree;
	BOOLEAN previousConfirmed = FALSE;
	BOOLEAN complete = FALSE;
	ULONG ranges = 0;
	UINT64 bytes = 0;
	ULONG drainedLength = 0;
	NTSTATUS status;

	if (PreviousBootConfirmed)
		*PreviousBootConfirmed = FALSE;
	if (MaterializedRanges)
		*MaterializedRanges = 0;
	if (MaterializedBytes)
		*MaterializedBytes = 0;
	if (!Core || !WriteRoutine || !Core->Journal || !Core->Journal->Mounted)
		return STATUS_DEVICE_NOT_READY;
	Cdp_LOCK_ACQUIRE(&Core->TreeLock);
	if (Core->Phase != Cdp_CORE_PHASE_GENERAL || Core->Building ||
		Core->MergeActive || Core->PendingRecoveryBranch ||
		Core->Journal->RecoveryPending ||
		!Core->Journal->RestorePointSet || !Core->MetaTreeReady)
	{
		Cdp_LOCK_RELEASE(&Core->TreeLock);
		return STATUS_INVALID_DEVICE_STATE;
	}
	Cdp_LOCK_RELEASE(&Core->TreeLock);

	status = CdpJournalBeginRestoreBoot(
		Core->Journal, &previousConfirmed);
	if (!NT_SUCCESS(status))
		return status;
	if (PreviousBootConfirmed)
		*PreviousBootConfirmed = previousConfirmed;

	/* A zero acknowledgement means the last boot never reached the service.
	 * Preserve all repair/startup writes from that attempt in the physical
	 * source before discarding its Journal history. Each successful range is
	 * punched, so a crash or retry resumes idempotently from what remains. */
	while (!previousConfirmed && !complete)
	{
		drainedLength = 0;
		status = CdpCoreDrainOneMetaRangeWithWriter(
			Core, WriteRoutine, WriteContext, &complete, NULL,
			&drainedLength);
		if (!NT_SUCCESS(status))
			return status;
		if (drainedLength != 0)
		{
			ranges++;
			bytes += drainedLength;
		}
	}
	if (MaterializedRanges)
		*MaterializedRanges = ranges;
	if (MaterializedBytes)
		*MaterializedBytes = bytes;

	Cdp_LOCK_ACQUIRE(&Core->TreeLock);
	if (Core->Phase != Cdp_CORE_PHASE_GENERAL || Core->Building ||
		Core->MergeActive || Core->PendingRecoveryBranch ||
		Core->Journal->RecoveryPending ||
		!Core->Journal->RestorePointSet ||
		Core->Journal->RestoreBootPending || !Core->MetaTreeReady)
	{
		Cdp_LOCK_RELEASE(&Core->TreeLock);
		return STATUS_INVALID_DEVICE_STATE;
	}
	oldTree = Core->MetaTree;
	CdpPreviewTreeInitialize(&Core->MetaTree);
	Core->MetaTreeReady = TRUE;
	Core->PendingRestoreReset = TRUE;
	Cdp_LOCK_RELEASE(&Core->TreeLock);
	CdpPreviewTreeFree(&oldTree);
#ifndef Cdp_USERMODE
	Cdp_LOG("[RESTORE-POINT] boot baseline published target=%llu previousBootConfirmed=%u materializedRanges=%lu materializedBytes=%llu; history reset deferred to first write\n",
		Core->Journal->RestorePointTime100ns,
		previousConfirmed ? 1u : 0u, ranges, bytes);
#endif
	return STATUS_SUCCESS;
}

NTSTATUS CdpCoreCancelPersistentRestoreBoot(_Inout_ PCdp_CORE Core)
{
	BOOLEAN pending;

	if (!Core)
		return STATUS_INVALID_PARAMETER;
	Cdp_LOCK_ACQUIRE(&Core->TreeLock);
	pending = Core->PendingRestoreReset;
	Cdp_LOCK_RELEASE(&Core->TreeLock);
	if (!pending)
		return STATUS_SUCCESS;
	/* Deleting the restore point before its first protected write is not a
	 * supported transition.  In particular, never scan the preserved old
	 * Record history to reconstruct a view for that case. */
	return STATUS_INVALID_DEVICE_STATE;
}

NTSTATUS CdpCoreSetRestorePointMarker(
	_Inout_ PCdp_CORE Core,
	_In_ UINT64 TargetTime100ns)
{
	if (!Core)
		return STATUS_INVALID_PARAMETER;
	return CdpJournalSetRestorePoint(Core->Journal, TargetTime100ns);
}

NTSTATUS CdpCoreConfirmPersistentRestoreBoot(_Inout_ PCdp_CORE Core)
{
	if (!Core || !Core->Journal)
		return STATUS_INVALID_PARAMETER;
	return CdpJournalConfirmRestoreBoot(Core->Journal);
}

NTSTATUS CdpCoreClearRestorePointMarker(_Inout_ PCdp_CORE Core)
{
	NTSTATUS status;
	if (!Core)
		return STATUS_INVALID_PARAMETER;
	/* Keep the explicit delete operation idempotent for existing callers. */
	if (!Core->Journal || !Core->Journal->RestorePointSet)
		return STATUS_SUCCESS;
	status = CdpCoreMaterializeRuntimeCheckpoints(Core);
	if (!NT_SUCCESS(status))
		return status;
	return CdpJournalClearRestorePoint(Core->Journal);
}

NTSTATUS CdpCoreRebuildCurrentView(_Inout_ PCdp_CORE Core)
{
	if (!Core)
		return STATUS_INVALID_PARAMETER;
	return CdpCoreBuildMetaTree(Core);
}

static NTSTATUS CdpCoreMaterializeTreeWithWriter(
	_Inout_ PCdp_CORE Core,
	_In_opt_ PCdp_PREVIEW_TREE_NODE Node,
	_In_ Cdp_CORE_DRAIN_WRITE_ROUTINE WriteRoutine,
	_In_opt_ PVOID WriteContext,
	_Inout_ PULONG WrittenRanges,
	_Inout_ PUINT64 WrittenBytes)
{
	PVOID payload;
	NTSTATUS status;

	if (!Node)
		return STATUS_SUCCESS;
	status = CdpCoreMaterializeTreeWithWriter(
		Core, Node->Left, WriteRoutine, WriteContext,
		WrittenRanges, WrittenBytes);
	if (!NT_SUCCESS(status))
		return status;
	if (!Node->Invalid)
	{
		payload = Cdp_ALLOC(Node->DataLength);
		if (!payload)
			return STATUS_INSUFFICIENT_RESOURCES;
		status = CdpJournalReadPayload(
			Core->Journal, Node->FileOffset, Node->DataLength, payload);
		if (NT_SUCCESS(status))
			status = WriteRoutine(
				WriteContext, Node->Start, Node->DataLength, payload);
		Cdp_FREE(payload);
		if (!NT_SUCCESS(status))
			return status;
		(*WrittenRanges)++;
		*WrittenBytes += Node->DataLength;
	}
	return CdpCoreMaterializeTreeWithWriter(
		Core, Node->Right, WriteRoutine, WriteContext,
		WrittenRanges, WrittenBytes);
}

NTSTATUS CdpCoreMaterializeTimeWithWriter(
	_Inout_ PCdp_CORE Core,
	_In_ UINT64 TargetTime100ns,
	_In_ Cdp_CORE_DRAIN_WRITE_ROUTINE WriteRoutine,
	_In_opt_ PVOID WriteContext,
	_Out_opt_ PUINT64 EffectiveTime100ns,
	_Out_opt_ PUINT64 TargetSequence,
	_Out_opt_ PULONG WrittenRanges,
	_Out_opt_ PUINT64 WrittenBytes)
{
	Cdp_PREVIEW_TREE tree;
	UINT64 effective = TargetTime100ns;
	UINT64 targetSequence = 0;
	ULONG ranges = 0;
	UINT64 bytes = 0;
	NTSTATUS status;

	if (!Core || !WriteRoutine || TargetTime100ns == 0)
		return STATUS_INVALID_PARAMETER;
	if (!Core->Journal || !Core->Journal->Mounted)
		return STATUS_DEVICE_NOT_READY;
	CdpPreviewTreeInitialize(&tree);
	status = CdpCoreResolveTargetTime(Core, TargetTime100ns, &effective);
	if (!NT_SUCCESS(status))
		goto done;
	status = CdpJournalBuildPreviewTree(
		Core->Journal, effective, Core->Journal->NextSequence,
		TRUE, &tree, &targetSequence);
	if (status == STATUS_NOT_FOUND)
		status = STATUS_SUCCESS;
	if (NT_SUCCESS(status))
		status = CdpCoreMaterializeTreeWithWriter(
			Core, tree.Root, WriteRoutine, WriteContext, &ranges, &bytes);
done:
	CdpPreviewTreeFree(&tree);
	if (EffectiveTime100ns)
		*EffectiveTime100ns = effective;
	if (TargetSequence)
		*TargetSequence = targetSequence;
	if (WrittenRanges)
		*WrittenRanges = ranges;
	if (WrittenBytes)
		*WrittenBytes = bytes;
	return status;
}

NTSTATUS CdpCoreRecoveryBegin(_Inout_ PCdp_CORE Core, _In_ UINT64 TargetTime100ns)
{
	Cdp_PREVIEW_TREE newTree;
	Cdp_PREVIEW_TREE oldTree;
	LONG parentBranch = 0;
	LONG newBranch = 0;
	UINT64 inheritedSequence = 0;
	Cdp_JOURNAL_RECORD_LOCATION targetLocation;
	UINT64 effectiveTargetTime100ns;
	UINT64 previousTargetTime100ns = 0;
	ULONG publishedNodeCount = 0;
	BOOLEAN branchCreated = FALSE;
	BOOLEAN newTreeInitialized = FALSE;
	const char* failureStage = "entry";
	NTSTATUS status;

	if (!Core)
		return STATUS_INVALID_PARAMETER;
	if (!Core->Journal->Mounted)
		return STATUS_DEVICE_NOT_READY;
	failureStage = "resolve-target-time";
	status = CdpCoreResolveTargetTime(
		Core, TargetTime100ns, &effectiveTargetTime100ns);
	if (!NT_SUCCESS(status))
	{
#ifndef Cdp_USERMODE
		Cdp_LOG("[RECOVERY-BEGIN-FAIL] stage=%s status=0x%08X requested=%llu\n",
			failureStage, status, TargetTime100ns);
#endif
		return status;
	}

	Cdp_LOCK_ACQUIRE(&Core->TreeLock);
	if (Core->Phase != Cdp_CORE_PHASE_GENERAL ||
		Core->PendingRecoveryBranch)
	{
		Cdp_LOCK_RELEASE(&Core->TreeLock);
		return STATUS_INVALID_DEVICE_STATE;
	}
	if (Core->MergeActive)
	{
		Cdp_LOCK_RELEASE(&Core->TreeLock);
		return STATUS_DEVICE_BUSY;
	}
	Core->Phase = Cdp_CORE_PHASE_RECOVERY;
	Core->Building = 1;
	previousTargetTime100ns = Core->TargetTime100ns;
	Core->TargetTime100ns = effectiveTargetTime100ns;
	Cdp_LOCK_RELEASE(&Core->TreeLock);

	/* Build recovery from the same root-to-target scan used by Preview.  This
	 * tree is complete before the new branch marker is appended, so Preview
	 * and Recovery at the same time cannot diverge through different rebuild
	 * algorithms. */
	failureStage = "build-target-metatree";
	RtlZeroMemory(&targetLocation, sizeof(targetLocation));
	status = CdpJournalBuildPreviewTreeEx(
		Core->Journal,
		effectiveTargetTime100ns,
		Core->Journal->NextSequence,
		TRUE,
		&newTree,
		&parentBranch,
		&inheritedSequence,
		&targetLocation);
	if (!NT_SUCCESS(status) && status != STATUS_NOT_FOUND)
		goto failure;
	if (status == STATUS_NOT_FOUND)
	{
		CdpPreviewTreeInitialize(&newTree);
		status = STATUS_SUCCESS;
	}
	newTreeInitialized = TRUE;
	CdpCoreTraceTargetRecord(
		"RECOVERY",
		TargetTime100ns,
		effectiveTargetTime100ns,
		&targetLocation);

	if (Core->Journal->HighestBranchNumber >= 0x7FFFFFFFL)
	{
		status = STATUS_INTEGER_OVERFLOW;
		goto failure;
	}
	newBranch = Core->Journal->HighestBranchNumber + 1;
	failureStage = "append-new-branch";
	status = CdpJournalAppendBranch(
		Core->Journal,
		newBranch,
		parentBranch,
		inheritedSequence);
	if (!NT_SUCCESS(status))
	{
		branchCreated =
			Core->Journal->CurrentBranchNumber == newBranch &&
			Core->Journal->HighestBranchNumber == newBranch;
		goto failure;
	}
	branchCreated = TRUE;

#ifdef Cdp_USERMODE
	if (!NT_SUCCESS(g_recoveryBuildFailureStatus))
	{
		status = g_recoveryBuildFailureStatus;
		g_recoveryBuildFailureStatus = STATUS_SUCCESS;
		goto failure;
	}
#endif

	// Publish only after the complete replacement tree is valid. The old tree
	// remains available throughout target resolution, branch creation and scan.
	Cdp_LOCK_ACQUIRE(&Core->TreeLock);
	oldTree = Core->MetaTree;
	Core->MetaTree = newTree;
	Core->MetaTreeReady = TRUE;
	Core->Building = 0;
	Core->Phase = Cdp_CORE_PHASE_GENERAL;
	publishedNodeCount = Core->MetaTree.NodeCount;
	Cdp_LOCK_RELEASE(&Core->TreeLock);
	newTreeInitialized = FALSE;
	CdpPreviewTreeFree(&oldTree);
	Cdp_RECOVERY_TRACE(
		"branch switch complete target=%llu parent=%ld inherit=%llu new=%ld nodes=%lu\n",
		effectiveTargetTime100ns,
		parentBranch,
		inheritedSequence,
		newBranch,
		publishedNodeCount);
#ifndef Cdp_USERMODE
	Cdp_LOG("[RECOVERY-METATREE] e complete target=%llu parentBranch=%ld inheritedSequence=%llu newBranch=%ld nodeCount=%lu\n",
		effectiveTargetTime100ns,
		parentBranch,
		inheritedSequence,
		newBranch,
		publishedNodeCount);
#endif
	return STATUS_SUCCESS;

failure:
#ifndef Cdp_USERMODE
	Cdp_LOG("[RECOVERY-BEGIN-FAIL] stage=%s status=0x%08X requested=%llu effective=%llu parent=%ld inherit=%llu newBranch=%ld treeInitialized=%u branchCreated=%u\n",
		failureStage,
		status,
		TargetTime100ns,
		effectiveTargetTime100ns,
		parentBranch,
		inheritedSequence,
		newBranch,
		newTreeInitialized ? 1u : 0u,
		branchCreated ? 1u : 0u);
#endif
	if (newTreeInitialized)
		CdpPreviewTreeFree(&newTree);
	if (branchCreated)
	{
		NTSTATUS rollbackStatus = CdpJournalRollbackLatestBranch(
			Core->Journal, newBranch);
		#ifndef Cdp_USERMODE
		if (!NT_SUCCESS(rollbackStatus))
			Cdp_LOG("[RECOVERY-BEGIN-FAIL] stage=rollback-new-branch status=0x%08X branch=%ld originalStatus=0x%08X\n",
				rollbackStatus, newBranch, status);
		#endif
		if (!NT_SUCCESS(rollbackStatus))
			status = rollbackStatus;
	}
	Cdp_LOCK_ACQUIRE(&Core->TreeLock);
	Core->Building = 0;
	Core->TargetTime100ns = previousTargetTime100ns;
	Core->Phase = Cdp_CORE_PHASE_GENERAL;
	Cdp_LOCK_RELEASE(&Core->TreeLock);
	return status;
}

NTSTATUS CdpCoreRecoveryCommit(_Inout_ PCdp_CORE Core)
{
	BOOLEAN complete;
	return CdpCoreRecoveryCommitStep(Core, &complete);
}
