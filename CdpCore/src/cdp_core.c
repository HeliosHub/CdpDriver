#include "cdp_core.h"
#include "cdp_alloc.h"
#include "CdpJournal.h"

#ifndef Cdp_USERMODE
#include "cdp_dev_store.h"
#define Cdp_RECOVERY_TRACE(fmt, ...) \
	Cdp_LOG("[RECOVERY] " fmt, ##__VA_ARGS__)

static VOID CdpCoreTraceTargetRecord(
	_In_ PCSTR Operation,
	_Inout_ PCdp_JOURNAL Journal,
	_In_ UINT64 RequestedTime100ns,
	_In_ UINT64 EffectiveTime100ns,
	_In_ UINT64 TargetSequence)
{
	UINT64 recordIndex = 0;
	UINT64 recordTime = 0;
	UINT64 regionOffset = 0;
	ULONG headerIndex = 0;
	NTSTATUS status;

	status = CdpJournalFindRecordLocationBySequence(
		Journal,
		TargetSequence,
		&recordIndex,
		&recordTime,
		&regionOffset,
		&headerIndex);
	if (NT_SUCCESS(status))
	{
		Cdp_LOG("[%s-TARGET] requestedTime=%llu effectiveTime=%llu recordTime=%llu recordIndex=%llu sequence=%llu rrOffset=%llu headerIndex=%lu\n",
			Operation, RequestedTime100ns, EffectiveTime100ns, recordTime,
			recordIndex, TargetSequence, regionOffset, headerIndex);
	}
	else
	{
		Cdp_LOG("[%s-TARGET] requestedTime=%llu effectiveTime=%llu target sequence=%llu location unavailable status=0x%08X\n",
			Operation, RequestedTime100ns, EffectiveTime100ns,
			TargetSequence, status);
	}
}
#else
#define Cdp_RECOVERY_TRACE(fmt, ...) ((void)0)
#define CdpCoreTraceTargetRecord(Operation, Journal, Requested, Effective, Sequence) ((void)0)
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
	struct _Cdp_WRITE_LEDGER_RANGE* WriteLedger;
	ULONG WriteLedgerRangeCount;
};

typedef struct _Cdp_WRITE_LEDGER_RANGE
{
	UINT64 Start;
	UINT64 End;
	UINT64 Sequence;
	UINT64 FileOffset;
	struct _Cdp_WRITE_LEDGER_RANGE* Next;
} Cdp_WRITE_LEDGER_RANGE, *PCdp_WRITE_LEDGER_RANGE;

static VOID CdpCoreClearWriteLedger(_Inout_ PCdp_CORE Core)
{
	PCdp_WRITE_LEDGER_RANGE range;
	PCdp_WRITE_LEDGER_RANGE next;

	if (!Core)
		return;
	range = Core->WriteLedger;
	while (range)
	{
		next = range->Next;
		Cdp_FREE(range);
		range = next;
	}
	Core->WriteLedger = NULL;
	Core->WriteLedgerRangeCount = 0;
}

/* TreeLock must be held.  The ledger is a sorted, non-overlapping union of
 * application-write ranges and is deliberately independent of MetaTree. */
static NTSTATUS CdpCorePunchWriteLedgerLocked(
	_Inout_ PCdp_CORE Core,
	_In_ UINT64 Start,
	_In_ UINT64 End);

static NTSTATUS CdpCoreInsertWriteLedgerLocked(
	_Inout_ PCdp_CORE Core,
	_Inout_ PCdp_WRITE_LEDGER_RANGE NewRange)
{
	PCdp_WRITE_LEDGER_RANGE* link = &Core->WriteLedger;
	NTSTATUS status;

	status = CdpCorePunchWriteLedgerLocked(
		Core, NewRange->Start, NewRange->End);
	if (!NT_SUCCESS(status))
		return status;
	while (*link && (*link)->Start < NewRange->Start)
		link = &(*link)->Next;
	NewRange->Next = *link;
	*link = NewRange;
	Core->WriteLedgerRangeCount++;
	return STATUS_SUCCESS;
}

/* TreeLock must be held.  Forget bytes that have deliberately fallen through
 * to the source (drain/direct-source write). */
static NTSTATUS CdpCorePunchWriteLedgerLocked(
	_Inout_ PCdp_CORE Core,
	_In_ UINT64 Start,
	_In_ UINT64 End)
{
	PCdp_WRITE_LEDGER_RANGE* link = &Core->WriteLedger;
	PCdp_WRITE_LEDGER_RANGE range;

	while ((range = *link) != NULL)
	{
		PCdp_WRITE_LEDGER_RANGE right;
		if (range->End <= Start)
		{
			link = &range->Next;
			continue;
		}
		if (range->Start >= End)
			break;
		if (Start <= range->Start && End >= range->End)
		{
			*link = range->Next;
			Cdp_FREE(range);
			Core->WriteLedgerRangeCount--;
			continue;
		}
		if (Start <= range->Start)
		{
			range->FileOffset += End - range->Start;
			range->Start = End;
			break;
		}
		if (End >= range->End)
		{
			range->End = Start;
			link = &range->Next;
			continue;
		}
		right = (PCdp_WRITE_LEDGER_RANGE)Cdp_ALLOC0(sizeof(*right));
		if (!right)
			return STATUS_INSUFFICIENT_RESOURCES;
		right->Start = End;
		right->End = range->End;
		right->Sequence = range->Sequence;
		right->FileOffset = range->FileOffset + (End - range->Start);
		right->Next = range->Next;
		range->End = Start;
		range->Next = right;
		Core->WriteLedgerRangeCount++;
		break;
	}
	return STATUS_SUCCESS;
}

static BOOLEAN CdpCoreMaskRangeCovered(
	_In_reads_bytes_((Length + 7) / 8) const UCHAR* Mask,
	_In_ ULONG Length,
	_In_ ULONG Start,
	_In_ ULONG End,
	_Out_ PULONG FirstGap)
{
	ULONG index = Start;
	while (index < End && (index & 7UL) != 0)
	{
		if ((Mask[index >> 3] & (UCHAR)(1U << (index & 7UL))) == 0)
		{
			*FirstGap = index;
			return FALSE;
		}
		index++;
	}
	while (index + 8UL <= End)
	{
		if (Mask[index >> 3] != 0xFFU)
		{
			ULONG bit;
			for (bit = 0; bit < 8; ++bit)
			{
				if ((Mask[index >> 3] & (UCHAR)(1U << bit)) == 0)
				{
					*FirstGap = index + bit;
					return FALSE;
				}
			}
		}
		index += 8UL;
	}
	while (index < End)
	{
		if ((Mask[index >> 3] & (UCHAR)(1U << (index & 7UL))) == 0)
		{
			*FirstGap = index;
			return FALSE;
		}
		index++;
	}
	UNREFERENCED_PARAMETER(Length);
	return TRUE;
}

static BOOLEAN CdpCoreValidateWriteLedgerLocked(
	_In_ PCdp_CORE Core,
	_In_ UINT64 Offset,
	_In_ ULONG Length,
	_In_reads_bytes_((Length + 7) / 8) const UCHAR* CoveredMask,
	_Out_ PUINT64 ExpectedStart,
	_Out_ PUINT64 ExpectedEnd,
	_Out_ PUINT64 FirstGap,
	_Out_ PUINT64 ExpectedSequence,
	_Out_ PUINT64 ExpectedFileOffset,
	_Out_ PUINT64 ActualSequence,
	_Out_ PUINT64 ActualFileOffset)
{
	PCdp_WRITE_LEDGER_RANGE range;
	UINT64 readEnd = Offset + Length;

	for (range = Core->WriteLedger; range; range = range->Next)
	{
		UINT64 start;
		UINT64 end;
		ULONG relativeGap;
		if (range->End <= Offset)
			continue;
		if (range->Start >= readEnd)
			break;
		start = range->Start > Offset ? range->Start : Offset;
		end = range->End < readEnd ? range->End : readEnd;
		if (!CdpCoreMaskRangeCovered(
			CoveredMask, Length, (ULONG)(start - Offset),
			(ULONG)(end - Offset), &relativeGap))
		{
			*ExpectedStart = range->Start;
			*ExpectedEnd = range->End;
			*FirstGap = Offset + relativeGap;
			*ExpectedSequence = range->Sequence;
			*ExpectedFileOffset = range->FileOffset +
				(*FirstGap - range->Start);
			return FALSE;
		}
		if (!CdpPreviewTreeValidateMapping(
			&Core->MetaTree, start, (ULONG)(end - start),
			range->Sequence,
			range->FileOffset + (start - range->Start),
			FirstGap, ActualSequence, ActualFileOffset))
		{
			*ExpectedStart = range->Start;
			*ExpectedEnd = range->End;
			*ExpectedSequence = range->Sequence;
			*ExpectedFileOffset = range->FileOffset +
				(*FirstGap - range->Start);
			return FALSE;
		}
	}
	return TRUE;
}

/* TreeLock is held by the caller, keeping ledger identity stable while the
 * independently addressed payload bytes are compared with the returned view. */
static NTSTATUS CdpCoreValidateWriteLedgerDataLocked(
	_Inout_ PCdp_CORE Core,
	_In_ UINT64 Offset,
	_In_ ULONG Length,
	_In_reads_bytes_(Length) const UCHAR* Buffer,
	_Out_ PUINT64 RangeStart,
	_Out_ PUINT64 RangeEnd,
	_Out_ PUINT64 Sequence,
	_Out_ PUINT64 PayloadOffset,
	_Out_ PUINT64 FirstMismatch,
	_Out_ PUCHAR ExpectedByte,
	_Out_ PUCHAR ActualByte)
{
	PCdp_WRITE_LEDGER_RANGE range;
	UINT64 readEnd = Offset + Length;

	for (range = Core->WriteLedger; range; range = range->Next)
	{
		UINT64 start;
		UINT64 end;
		ULONG compareLength;
		PUCHAR expected;
		UINT64 payloadAtStart;
		UINT64 alignedPayload;
		ULONG payloadPrefix;
		ULONG readLength;
		ULONG index;
		NTSTATUS status;
		if (range->End <= Offset)
			continue;
		if (range->Start >= readEnd)
			break;
		start = range->Start > Offset ? range->Start : Offset;
		end = range->End < readEnd ? range->End : readEnd;
		compareLength = (ULONG)(end - start);
		payloadAtStart = range->FileOffset + (start - range->Start);
		alignedPayload = payloadAtStart -
			(payloadAtStart % Core->Journal->SectorSize);
		payloadPrefix = (ULONG)(payloadAtStart - alignedPayload);
		readLength = payloadPrefix + compareLength;
		expected = (PUCHAR)Cdp_ALLOC(readLength);
		if (!expected)
			return STATUS_INSUFFICIENT_RESOURCES;
		status = CdpJournalReadPayload(
			Core->Journal,
			alignedPayload,
			readLength,
			expected);
		if (!NT_SUCCESS(status))
		{
			Cdp_FREE(expected);
			return status;
		}
		for (index = 0; index < compareLength; ++index)
		{
			UCHAR actual = Buffer[(ULONG)(start - Offset) + index];
			if (expected[payloadPrefix + index] != actual)
			{
				*RangeStart = range->Start;
				*RangeEnd = range->End;
				*Sequence = range->Sequence;
				*PayloadOffset = range->FileOffset +
					(start - range->Start) + index;
				*FirstMismatch = start + index;
				*ExpectedByte = expected[payloadPrefix + index];
				*ActualByte = actual;
				Cdp_FREE(expected);
				return STATUS_DATA_ERROR;
			}
		}
		Cdp_FREE(expected);
	}
	return STATUS_SUCCESS;
}

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
	CdpCoreClearWriteLedger(Core);
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
	CdpCoreClearWriteLedger(Core);
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
	CdpCoreClearWriteLedger(Core);
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
		status = CdpCorePunchWriteLedgerLocked(
			Core, start, start + length);
		if (!NT_SUCCESS(status))
			goto cleanup;
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
	PCdp_WRITE_LEDGER_RANGE ledgerRange;
	if (!Core || !AfterImage || Length == 0 ||
		Length > Cdp_JOURNAL_MAX_RECORD_DATA)
	{
		return STATUS_INVALID_PARAMETER;
	}
	if (!Core->Journal->Mounted)
		return STATUS_DEVICE_NOT_READY;
	if (!Core->MetaTreeReady)
		return STATUS_DEVICE_NOT_READY;
	ledgerRange = (PCdp_WRITE_LEDGER_RANGE)Cdp_ALLOC0(sizeof(*ledgerRange));
	if (!ledgerRange)
		return STATUS_INSUFFICIENT_RESOURCES;
	ledgerRange->Start = Offset;
	ledgerRange->End = Offset + Length;

	Cdp_LOCK_ACQUIRE(&Core->TreeLock);
	if (!Core->MetaTreeReady)
	{
		Cdp_LOCK_RELEASE(&Core->TreeLock);
		Cdp_FREE(ledgerRange);
		return STATUS_DEVICE_NOT_READY;
	}
	nodeCountBefore = Core->MetaTree.NodeCount;
	status = CdpCoreMaterializePendingRestoreResetLocked(Core);
	if (!NT_SUCCESS(status))
	{
		Cdp_LOCK_RELEASE(&Core->TreeLock);
		Cdp_FREE(ledgerRange);
		return status;
	}
	status = CdpCoreMaterializePendingRecoveryBranchLocked(Core);
	if (!NT_SUCCESS(status))
	{
		Cdp_LOCK_RELEASE(&Core->TreeLock);
		Cdp_FREE(ledgerRange);
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
		else
		{
			ledgerRange->Sequence = record.Sequence;
			ledgerRange->FileOffset = record.FileOffset;
			status = CdpCoreInsertWriteLedgerLocked(Core, ledgerRange);
			if (NT_SUCCESS(status))
				ledgerRange = NULL;
			else
			{
#ifndef Cdp_USERMODE
				Cdp_LOG("[REDIRECT-WRITE-FAIL] stage=write-ledger offset=%llu len=%lu sequence=%llu payloadOffset=%llu status=0x%08X\n",
					Offset, Length, record.Sequence, record.FileOffset, status);
#endif
				Core->MetaTreeReady = FALSE;
			}
		}
	}
	nodeCountAfter = Core->MetaTree.NodeCount;
	Cdp_LOCK_RELEASE(&Core->TreeLock);
	if (ledgerRange)
		Cdp_FREE(ledgerRange);
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
		else
			status = CdpCorePunchWriteLedgerLocked(
				Core, offset, offset + length);
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
		else
			status = CdpCorePunchWriteLedgerLocked(
				Core, Offset, Offset + Length);
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
	_In_ BOOLEAN ValidateWriteLedger,
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
	if (NT_SUCCESS(status) && ValidateWriteLedger)
	{
		UINT64 expectedStart = 0;
		UINT64 expectedEnd = 0;
		UINT64 firstGap = 0;
		UINT64 expectedSequence = 0;
		UINT64 expectedFileOffset = 0;
		UINT64 actualSequence = 0;
		UINT64 actualFileOffset = 0;
		ULONG ledgerRanges;
		ULONG metaNodes;
		BOOLEAN valid;
		Cdp_LOCK_ACQUIRE(&Core->TreeLock);
		valid = CdpCoreValidateWriteLedgerLocked(
			Core, Offset, Length, coveredMask,
			&expectedStart, &expectedEnd, &firstGap,
			&expectedSequence, &expectedFileOffset,
			&actualSequence, &actualFileOffset);
		ledgerRanges = Core->WriteLedgerRangeCount;
		metaNodes = Core->MetaTree.NodeCount;
		Cdp_LOCK_RELEASE(&Core->TreeLock);
		if (!valid)
		{
#ifndef Cdp_USERMODE
			Cdp_LOG("[WRITE-LEDGER-MAP-MISS] read=[%llu,%llu) expected=[%llu,%llu) firstMismatch=%llu expectedSeq=%llu expectedPayload=%llu actualSeq=%llu actualPayload=%llu ledgerRanges=%lu metaNodes=%lu covered=%lu\n",
				Offset, Offset + Length, expectedStart, expectedEnd,
				firstGap, expectedSequence, expectedFileOffset,
				actualSequence, actualFileOffset,
				ledgerRanges, metaNodes, coveredCount);
#endif
			status = STATUS_DISK_CORRUPT_ERROR;
		}
		else
		{
			UINT64 rangeStart = 0;
			UINT64 rangeEnd = 0;
			UINT64 sequence = 0;
			UINT64 payloadOffset = 0;
			UINT64 firstMismatch = 0;
			UCHAR expectedByte = 0;
			UCHAR actualByte = 0;
			Cdp_LOCK_ACQUIRE(&Core->TreeLock);
			status = CdpCoreValidateWriteLedgerDataLocked(
				Core, Offset, Length, (const UCHAR*)Buffer,
				&rangeStart, &rangeEnd, &sequence, &payloadOffset,
				&firstMismatch, &expectedByte, &actualByte);
			Cdp_LOCK_RELEASE(&Core->TreeLock);
#ifndef Cdp_USERMODE
			if (status == STATUS_DATA_ERROR)
			{
				Cdp_LOG("[WRITE-LEDGER-DATA-MISMATCH] read=[%llu,%llu) ledger=[%llu,%llu) firstMismatch=%llu sequence=%llu payloadOffset=%llu expected=0x%02X actual=0x%02X\n",
					Offset, Offset + Length, rangeStart, rangeEnd,
					firstMismatch, sequence, payloadOffset,
					expectedByte, actualByte);
			}
			else if (!NT_SUCCESS(status))
			{
				Cdp_LOG("[WRITE-LEDGER-DATA-READ-FAIL] read=[%llu,%llu) status=0x%08X\n",
					Offset, Offset + Length, status);
			}
#endif
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
			Core, &Core->PreviewTree, Offset, Length, Buffer, FALSE, TRUE);
	if (!Core->MetaTreeReady)
		return STATUS_DEVICE_NOT_READY;

	return CdpCoreSynthesizeRead(
		Core, &Core->MetaTree, Offset, Length, Buffer, TRUE, TRUE);
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
			Core, &Core->PreviewTree, Offset, Length, Buffer, FALSE, FALSE);
	if (!Core->MetaTreeReady)
		return STATUS_DEVICE_NOT_READY;

	/* Buffer already contains the source-disk baseline. Apply only journal
	 * ranges; holes deliberately retain the bytes returned by Disk Lower. */
	return CdpCoreSynthesizeRead(
		Core, &Core->MetaTree, Offset, Length, Buffer, TRUE, FALSE);
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
		Core, &Core->PreviewTree, Offset, Length, Buffer, FALSE, TRUE);
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
	CdpCoreTraceTargetRecord(
		"PREVIEW",
		Core->Journal,
		TargetTime100ns,
		effectiveTargetTime100ns,
		targetRecordSequence);

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

	failureStage = "resolve-target-branch";
	status = CdpJournalResolveTargetBranch(
		Core->Journal,
		effectiveTargetTime100ns,
		&parentBranch,
		&inheritedSequence);
	if (!NT_SUCCESS(status))
		goto failure;
	CdpCoreTraceTargetRecord(
		"REBOOT-RECOVERY",
		Core->Journal,
		TargetTime100ns,
		effectiveTargetTime100ns,
		inheritedSequence);

	failureStage = "build-target-metatree";
	status = CdpJournalBuildPreviewTree(
		Core->Journal,
		effectiveTargetTime100ns,
		Core->Journal->NextSequence,
		TRUE,
		&newTree,
		&inheritedSequence);
	if (!NT_SUCCESS(status) && status != STATUS_NOT_FOUND)
		goto failure;
	if (status == STATUS_NOT_FOUND)
	{
		CdpPreviewTreeInitialize(&newTree);
		status = STATUS_SUCCESS;
	}
	newTreeInitialized = TRUE;
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
	CdpCoreClearWriteLedger(Core);
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

NTSTATUS CdpCorePreparePersistentRestoreBoot(_Inout_ PCdp_CORE Core)
{
	Cdp_PREVIEW_TREE oldTree;

	if (!Core || !Core->Journal || !Core->Journal->Mounted)
		return STATUS_DEVICE_NOT_READY;
	Cdp_LOCK_ACQUIRE(&Core->TreeLock);
	if (Core->Phase != Cdp_CORE_PHASE_GENERAL || Core->Building ||
		Core->MergeActive || Core->PendingRecoveryBranch ||
		Core->Journal->RecoveryPending ||
		!Core->Journal->RestorePointSet)
	{
		Cdp_LOCK_RELEASE(&Core->TreeLock);
		return STATUS_INVALID_DEVICE_STATE;
	}
	oldTree = Core->MetaTree;
	CdpPreviewTreeInitialize(&Core->MetaTree);
	CdpCoreClearWriteLedger(Core);
	Core->MetaTreeReady = TRUE;
	Core->PendingRestoreReset = TRUE;
	Cdp_LOCK_RELEASE(&Core->TreeLock);
	CdpPreviewTreeFree(&oldTree);
#ifndef Cdp_USERMODE
	Cdp_LOG("[RESTORE-POINT] boot baseline published target=%llu; history reset deferred to first write\n",
		Core->Journal->RestorePointTime100ns);
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

NTSTATUS CdpCoreClearRestorePointMarker(_Inout_ PCdp_CORE Core)
{
	if (!Core)
		return STATUS_INVALID_PARAMETER;
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

	failureStage = "resolve-target-branch";
	status = CdpJournalResolveTargetBranch(
		Core->Journal,
		effectiveTargetTime100ns,
		&parentBranch,
		&inheritedSequence);
	if (!NT_SUCCESS(status))
		goto failure;
	CdpCoreTraceTargetRecord(
		"RECOVERY",
		Core->Journal,
		TargetTime100ns,
		effectiveTargetTime100ns,
		inheritedSequence);

	/* Build recovery from the same root-to-target scan used by Preview.  This
	 * tree is complete before the new branch marker is appended, so Preview
	 * and Recovery at the same time cannot diverge through different rebuild
	 * algorithms. */
	failureStage = "build-target-metatree";
	status = CdpJournalBuildPreviewTree(
		Core->Journal,
		effectiveTargetTime100ns,
		Core->Journal->NextSequence,
		TRUE,
		&newTree,
		&inheritedSequence);
	if (!NT_SUCCESS(status) && status != STATUS_NOT_FOUND)
		goto failure;
	if (status == STATUS_NOT_FOUND)
	{
		CdpPreviewTreeInitialize(&newTree);
		status = STATUS_SUCCESS;
	}
	newTreeInitialized = TRUE;

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
	CdpCoreClearWriteLedger(Core);
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
