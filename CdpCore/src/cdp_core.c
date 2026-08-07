#include "cdp_core.h"
#include "cdp_alloc.h"
#include "CdpJournal.h"

#ifndef Cdp_USERMODE
#include "cdp_dev_store.h"
#define Cdp_RECOVERY_TRACE(fmt, ...) \
	Cdp_LOG("[RECOVERY] " fmt, ##__VA_ARGS__)
#else
#define Cdp_RECOVERY_TRACE(fmt, ...) ((void)0)
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
		NTSTATUS status = CdpCoreBuildMetaTree(core);
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

UINT64 CdpCoreGetTime100ns(_In_ PCdp_CORE Core)
{
	return Core ? Core->Time100ns : 0;
}

UINT64 CdpCoreGetTargetTime100ns(_In_ PCdp_CORE Core)
{
	return Core ? Core->TargetTime100ns : 0;
}

UINT64 CdpCoreTick(_Inout_ PCdp_CORE Core, _In_ UINT64 Delta100ns)
{
	if (!Core)
		return 0;
	Core->Time100ns += Delta100ns;
	return Core->Time100ns;
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
	UINT64 usedBytes;
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
	UNREFERENCED_PARAMETER(payloadBytesFree);
	UNREFERENCED_PARAMETER(totalRecords);
	if (!NT_SUCCESS(status))
		return status;
	if (metadataBytes > MAXUINT64 - payloadBytesUsed)
		return STATUS_INTEGER_OVERFLOW;
	usedBytes = metadataBytes + payloadBytesUsed;
	*AtLeast = usedBytes >=
		(partitionBytes / 100) * Percent +
		((partitionBytes % 100) * Percent + 99) / 100;
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
		if (Core->MergeActive)
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
	status = CdpJournalGetOldestCompactableRegion(
		Core->Journal, &regionOffset, &firstSequence, &endSequence);
	if (!NT_SUCCESS(status))
		goto cleanup;

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
		goto cleanup;

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

Cdp_CORE_PHASE CdpCoreGetPhase(_In_ PCdp_CORE Core)
{
	if (!Core)
		return Cdp_CORE_PHASE_GENERAL;
	return (Cdp_CORE_PHASE)Core->Phase;
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
#ifndef Cdp_USERMODE
	LARGE_INTEGER appendStart;
	LARGE_INTEGER treeWaitStart;
	LARGE_INTEGER treeWaitEnd;
	LARGE_INTEGER treeUpdateStart;
	LARGE_INTEGER treeUpdateEnd;
#endif
	if (!Core || !AfterImage || Length == 0 ||
		Length > Cdp_JOURNAL_MAX_RECORD_DATA)
	{
		return STATUS_INVALID_PARAMETER;
	}
	if (!Core->Journal->Mounted)
		return STATUS_DEVICE_NOT_READY;
	if (!Core->MetaTreeReady)
		return STATUS_DEVICE_NOT_READY;

#ifndef Cdp_USERMODE
	appendStart = KeQueryPerformanceCounter(NULL);
	treeWaitStart = appendStart;
#endif
	Cdp_LOCK_ACQUIRE(&Core->TreeLock);
#ifndef Cdp_USERMODE
	treeWaitEnd = KeQueryPerformanceCounter(NULL);
	InterlockedAdd64(
		&g_CdpPerfCounters.TreeLockWaitTicks,
		treeWaitEnd.QuadPart - treeWaitStart.QuadPart);
#endif
	if (!Core->MetaTreeReady)
	{
		Cdp_LOCK_RELEASE(&Core->TreeLock);
		return STATUS_DEVICE_NOT_READY;
	}
	status = CdpJournalAppendEx(
		Core->Journal,
		Offset,
		Length,
		AfterImage,
		0,
		&record);
	if (NT_SUCCESS(status))
	{
#ifndef Cdp_USERMODE
		treeUpdateStart = KeQueryPerformanceCounter(NULL);
#endif
		status = CdpPreviewTreeOverlayLatest(&Core->MetaTree, &record);
#ifndef Cdp_USERMODE
		treeUpdateEnd = KeQueryPerformanceCounter(NULL);
		InterlockedAdd64(
			&g_CdpPerfCounters.TreeUpdateTicks,
			treeUpdateEnd.QuadPart - treeUpdateStart.QuadPart);
#endif
		if (!NT_SUCCESS(status))
			Core->MetaTreeReady = FALSE;
	}
	Cdp_LOCK_RELEASE(&Core->TreeLock);
#ifndef Cdp_USERMODE
	InterlockedIncrement(&g_CdpPerfCounters.AppendCount);
	InterlockedAdd64(
		&g_CdpPerfCounters.AppendTicks,
		KeQueryPerformanceCounter(NULL).QuadPart - appendStart.QuadPart);
#endif
	if (NT_SUCCESS(status))
	{
		Core->Time100ns += 1;
		if (WrittenRecord)
			*WrittenRecord = record;
	}
	return status;
}

static NTSTATUS CdpCoreSynthesizeRead(
	_Inout_ PCdp_CORE Core,
	_In_ PCdp_PREVIEW_TREE Tree,
	_In_ UINT64 Offset,
	_In_ ULONG Length,
	_Out_writes_bytes_(Length) PVOID Buffer)
{
	PUCHAR coveredMask = NULL;
	ULONG coveredCount = 0;
	NTSTATUS status;
	ULONG i;
	ULONG maskBytes = (Length + 7UL) / 8UL;

	coveredMask = (PUCHAR)Cdp_ALLOC0(maskBytes);
	if (!coveredMask)
	{
		status = STATUS_INSUFFICIENT_RESOURCES;
		goto done;
	}

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
	if (coveredCount == Length)
		goto done;

	if (coveredCount == 0)
	{
		status = CdpCoreSourceRead(Core, Offset, Length, Buffer);
	}
	else
	{
		i = 0;
		while (i < Length)
		{
			ULONG runStart;

			// Skip covered bytes a bitmap byte at a time where possible.
			while (i < Length)
			{
				if ((i & 7) == 0 && i + 8 <= Length &&
					coveredMask[i >> 3] == 0xFF)
				{
					i += 8;
					continue;
				}
				if ((coveredMask[i >> 3] &
					(UCHAR)(1U << (i & 7))) == 0)
					break;
				++i;
			}
			if (i >= Length)
				break;

			runStart = i;
			while (i < Length)
			{
				if ((i & 7) == 0 && i + 8 <= Length &&
					coveredMask[i >> 3] == 0)
				{
					i += 8;
					continue;
				}
				if ((coveredMask[i >> 3] &
					(UCHAR)(1U << (i & 7))) != 0)
					break;
				++i;
			}

			status = CdpCoreSourceRead(
				Core,
				Offset + runStart,
				i - runStart,
				(PUCHAR)Buffer + runStart);
			if (!NT_SUCCESS(status))
				goto done;
		}
	}

done:
	Cdp_FREE(coveredMask);
	return status;
}

/* During Preview/Recovery tree construction, staging headers describe writes
 * after the snapshot.  Their payloads are before-images, so staging is used
 * only as a coverage map: its bytes must come from live source. */
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
			Core, &Core->PreviewTree, Offset, Length, Buffer);
	if (!Core->MetaTreeReady)
		return STATUS_DEVICE_NOT_READY;

	return CdpCoreSynthesizeRead(
		Core, &Core->MetaTree, Offset, Length, Buffer);
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
		Core, &Core->PreviewTree, Offset, Length, Buffer);
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

	if (!Core)
		return STATUS_INVALID_PARAMETER;
	if (!Core->Journal->Mounted)
		return STATUS_DEVICE_NOT_READY;
	status = CdpCoreResolveTargetTime(
		Core, TargetTime100ns, &effectiveTargetTime100ns);
	if (!NT_SUCCESS(status))
		return status;

	Cdp_LOCK_ACQUIRE(&Core->TreeLock);
	if (Core->Phase != Cdp_CORE_PHASE_GENERAL)
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

	status = CdpJournalBuildPreviewTree(
		Core->Journal,
		effectiveTargetTime100ns,
		Core->Journal->NextSequence,
		TRUE,
		&Core->PreviewTree,
		&targetRecordSequence);
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
	Core->PreviewTargetSequence = targetRecordSequence;
	Core->Building = 0;
	Cdp_LOCK_RELEASE(&Core->TreeLock);

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

NTSTATUS CdpCoreRecoveryBegin(_Inout_ PCdp_CORE Core, _In_ UINT64 TargetTime100ns)
{
	Cdp_PREVIEW_TREE newTree;
	Cdp_PREVIEW_TREE oldTree;
	LONG parentBranch = 0;
	LONG newBranch = 0;
	UINT64 inheritedSequence = 0;
	UINT64 effectiveTargetTime100ns;
	UINT64 previousTargetTime100ns = 0;
	BOOLEAN branchCreated = FALSE;
	BOOLEAN newTreeInitialized = FALSE;
	NTSTATUS status;

	if (!Core)
		return STATUS_INVALID_PARAMETER;
	if (!Core->Journal->Mounted)
		return STATUS_DEVICE_NOT_READY;
	status = CdpCoreResolveTargetTime(
		Core, TargetTime100ns, &effectiveTargetTime100ns);
	if (!NT_SUCCESS(status))
		return status;

	Cdp_LOCK_ACQUIRE(&Core->TreeLock);
	if (Core->Phase != Cdp_CORE_PHASE_GENERAL)
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

	status = CdpJournalResolveTargetBranch(
		Core->Journal,
		effectiveTargetTime100ns,
		&parentBranch,
		&inheritedSequence);
	if (!NT_SUCCESS(status))
		goto failure;
	if (Core->Journal->HighestBranchNumber >= 0x7FFFFFFFL)
	{
		status = STATUS_INTEGER_OVERFLOW;
		goto failure;
	}
	newBranch = Core->Journal->HighestBranchNumber + 1;
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
	status = CdpJournalBuildCurrentBranchTree(Core->Journal, &newTree);
	if (!NT_SUCCESS(status))
		goto failure;
	newTreeInitialized = TRUE;

	// Publish only after the complete replacement tree is valid. The old tree
	// remains available throughout target resolution, branch creation and scan.
	Cdp_LOCK_ACQUIRE(&Core->TreeLock);
	oldTree = Core->MetaTree;
	Core->MetaTree = newTree;
	Core->MetaTreeReady = TRUE;
	Core->Building = 0;
	Core->Phase = Cdp_CORE_PHASE_GENERAL;
	Cdp_LOCK_RELEASE(&Core->TreeLock);
	newTreeInitialized = FALSE;
	CdpPreviewTreeFree(&oldTree);
	Cdp_RECOVERY_TRACE(
		"branch switch complete target=%llu parent=%ld inherit=%llu new=%ld nodes=%lu\n",
		effectiveTargetTime100ns,
		parentBranch,
		inheritedSequence,
		newBranch,
		Core->MetaTree.NodeCount);
	return STATUS_SUCCESS;

failure:
	if (newTreeInitialized)
		CdpPreviewTreeFree(&newTree);
	if (branchCreated)
	{
		NTSTATUS rollbackStatus = CdpJournalRollbackLatestBranch(
			Core->Journal, newBranch);
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
