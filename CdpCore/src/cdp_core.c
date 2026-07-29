#include "cdp_core.h"
#include "cdp_alloc.h"
#include "CdpJournal.h"

#ifndef Cdp_USERMODE
#include "cdp_dev_store.h"
#define Cdp_RECOVERY_ERR(fmt, ...) \
	Cdp_LOG("[RECOVERY] " fmt, ##__VA_ARGS__)
/*
 * Recovery writeback is intentionally traced in Release builds.  These
 * records contain only metadata (never payload bytes) and let field logs be
 * correlated with the record-header list returned by the console.
 */
#define Cdp_RECOVERY_TRACE(fmt, ...) \
	Cdp_LOG("[RECOVERY] " fmt, ##__VA_ARGS__)
#if DBG
#define Cdp_RECOVERY_DBG(fmt, ...) \
	DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, \
		"CdpDriver: [RECOVERY] " fmt, ##__VA_ARGS__)
#else
#define Cdp_RECOVERY_DBG(fmt, ...) ((void)0)
#endif
#else
#define Cdp_RECOVERY_ERR(fmt, ...) ((void)0)
#define Cdp_RECOVERY_TRACE(fmt, ...) ((void)0)
#define Cdp_RECOVERY_DBG(fmt, ...) ((void)0)
#endif

#ifdef Cdp_USERMODE
#include <string.h>

typedef void (*Cdp_CORE_TEST_BUILD_HOOK)(_Inout_ PCdp_CORE Core);
typedef void (*Cdp_CORE_TEST_WRITEBACK_HOOK)(_Inout_ PCdp_CORE Core);
static Cdp_CORE_TEST_BUILD_HOOK g_previewBuildHook;
static Cdp_CORE_TEST_BUILD_HOOK g_recoveryBuildHook;
static Cdp_CORE_TEST_WRITEBACK_HOOK g_writebackHook;

VOID CdpCoreTestSetPreviewBuildHook(_In_opt_ Cdp_CORE_TEST_BUILD_HOOK Hook)
{
	g_previewBuildHook = Hook;
}

VOID CdpCoreTestSetRecoveryBuildHook(_In_opt_ Cdp_CORE_TEST_BUILD_HOOK Hook)
{
	g_recoveryBuildHook = Hook;
}

VOID CdpCoreTestSetWritebackHook(_In_opt_ Cdp_CORE_TEST_WRITEBACK_HOOK Hook)
{
	g_writebackHook = Hook;
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
	Cdp_PREVIEW_TREE HistoryTree;
	Cdp_PREVIEW_TREE StagingTree;
	Cdp_LOCK TreeLock;
	UINT64 TargetTime100ns;
	UINT64 SnapshotMaxSequence;
	LONG Building;
	LONG WritebackActive;
	NTSTATUS RecoveryFailureStatus;
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
	NTSTATUS status;

	if (Core->Phase == Cdp_CORE_PHASE_RECOVERY)
	{
		Cdp_RECOVERY_DBG(
			"source read begin offset=%llu len=%lu\n",
			Offset,
			Length);
	}
	status = Core->Source->Read(Core->Source, Offset, Length, Buffer);
	if (Core->Phase == Cdp_CORE_PHASE_RECOVERY)
	{
		Cdp_RECOVERY_DBG(
			"source read end offset=%llu len=%lu status=0x%08X\n",
			Offset,
			Length,
			status);
	}
	return status;
}

static NTSTATUS CdpCoreSourceWriteDirect(
	_In_ PCdp_CORE Core,
	_In_ UINT64 Offset,
	_In_ ULONG Length,
	_In_reads_bytes_(Length) const VOID* Buffer)
{
	NTSTATUS status;

	if (Core->WritebackActive)
	{
		Cdp_RECOVERY_DBG(
			"source write begin offset=%llu len=%lu\n",
			Offset,
			Length);
	}
	status = Core->Source->Write(Core->Source, Offset, Length, Buffer);
	if (Core->WritebackActive)
	{
		Cdp_RECOVERY_DBG(
			"source write end offset=%llu len=%lu status=0x%08X\n",
			Offset,
			Length,
			status);
	}
	return status;
}

static VOID CdpCoreInitCommon(_Inout_ PCdp_CORE Core)
{
	Core->Time100ns = 1;
	Core->Phase = Cdp_CORE_PHASE_GENERAL;
	CdpPreviewTreeInitialize(&Core->PreviewTree);
	CdpPreviewTreeInitialize(&Core->HistoryTree);
	CdpPreviewTreeInitialize(&Core->StagingTree);
	Cdp_LOCK_INIT(&Core->TreeLock);
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
	CdpPreviewTreeFree(&Core->HistoryTree);
	CdpPreviewTreeFree(&Core->StagingTree);
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
	if (!Core)
		return STATUS_INVALID_PARAMETER;
	return CdpJournalFormat(Core->Journal);
}

NTSTATUS CdpCoreMountJournal(_Inout_ PCdp_CORE Core)
{
	if (!Core)
		return STATUS_INVALID_PARAMETER;
	return CdpJournalMount(Core->Journal);
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

static NTSTATUS CdpCoreAfterAppend(
	_Inout_ PCdp_CORE Core,
	_In_ const Cdp_JOURNAL_RECORD* Record,
	_In_ UINT64 Offset,
	_In_ ULONG Length)
{
	NTSTATUS punchStatus = STATUS_SUCCESS;
	if (Core->Phase == Cdp_CORE_PHASE_PREVIEW)
	{
		Cdp_LOCK_ACQUIRE(&Core->TreeLock);
		if (Core->Building)
		{
			if (Record->Sequence >= Core->SnapshotMaxSequence)
				(void)CdpPreviewTreeInsert(&Core->StagingTree, Record);
		}
		else
		{
			(void)CdpPreviewTreeInsert(&Core->PreviewTree, Record);
		}
		Cdp_LOCK_RELEASE(&Core->TreeLock);
		return STATUS_SUCCESS;
	}
	else if (Core->Phase == Cdp_CORE_PHASE_RECOVERY && !Core->WritebackActive)
	{
		Cdp_LOCK_ACQUIRE(&Core->TreeLock);
		if (Core->Building)
		{
			if (Record->Sequence >= Core->SnapshotMaxSequence)
			{
				Cdp_RECOVERY_TRACE(
					"build-time concurrent record seq=%llu volumeOff=%llu len=%lu "
					"journalOff=%llu -> staging\n",
					Record->Sequence,
					Offset,
					Length,
					Record->FileOffset);
				(void)CdpPreviewTreeInsert(&Core->StagingTree, Record);
			}
		}
		else
		{
			/* Do not overwrite a source range changed after recovery was prepared. */
			Cdp_RECOVERY_TRACE(
				"prepared-time concurrent record seq=%llu volumeOff=%llu len=%lu "
				"journalOff=%llu -> punch history range\n",
				Record->Sequence,
				Offset,
				Length,
				Record->FileOffset);
			punchStatus = CdpPreviewTreePunchRange(
				&Core->HistoryTree,
				Offset,
				Length);
			if (!NT_SUCCESS(punchStatus))
			{
				Cdp_RECOVERY_ERR(
					"history punch failed seq=%llu volumeOff=%llu len=%lu status=0x%08X; "
					"recovery is failed\n",
					Record->Sequence,
					Offset,
					Length,
					punchStatus);
				Core->RecoveryFailureStatus = punchStatus;
			}
		}
		Cdp_LOCK_RELEASE(&Core->TreeLock);
		return punchStatus;
	}
	return STATUS_SUCCESS;
}

NTSTATUS CdpCoreCaptureAppend(
	_Inout_ PCdp_CORE Core,
	_In_ UINT64 Offset,
	_In_ ULONG Length,
	_Out_opt_ PCdp_JOURNAL_RECORD WrittenRecord)
{
	PUCHAR before = NULL;
	NTSTATUS status;
	Cdp_JOURNAL_RECORD record;

	if (!Core || Length == 0)
		return STATUS_INVALID_PARAMETER;
	if (!Core->Journal->Mounted)
		return STATUS_DEVICE_NOT_READY;

	before = (PUCHAR)Cdp_ALLOC(Length);
	if (!before)
		return STATUS_INSUFFICIENT_RESOURCES;

	status = CdpCoreSourceRead(Core, Offset, Length, before);
	if (!NT_SUCCESS(status))
		goto done;

	if (Core->WritebackActive)
	{
		Cdp_RECOVERY_DBG(
			"capture journal append begin offset=%llu len=%lu\n",
			Offset,
			Length);
	}
	status = CdpJournalAppend(Core->Journal, Offset, Length, before, &record);
	if (!NT_SUCCESS(status))
	{
		if (Core->WritebackActive)
		{
			Cdp_RECOVERY_DBG(
				"capture journal append end offset=%llu len=%lu "
				"status=0x%08X\n",
				Offset,
				Length,
				status);
		}
		goto done;
	}
	if (Core->WritebackActive)
	{
		Cdp_RECOVERY_DBG(
			"capture journal append end offset=%llu len=%lu "
			"seq=%llu journalOff=%llu status=0x%08X\n",
			Offset,
			Length,
			record.Sequence,
			record.FileOffset,
			status);
	}

	status = CdpCoreAfterAppend(Core, &record, Offset, Length);
	if (!NT_SUCCESS(status))
		goto done;
	Core->Time100ns += 1;
	if (WrittenRecord)
		*WrittenRecord = record;

done:
	Cdp_FREE(before);
	return status;
}

NTSTATUS CdpCoreWrite(
	_Inout_ PCdp_CORE Core,
	_In_ UINT64 Offset,
	_In_ ULONG Length,
	_In_reads_bytes_(Length) const VOID* Data)
{
	NTSTATUS status;
	Cdp_JOURNAL_RECORD record;

	if (!Core || !Data || Length == 0)
		return STATUS_INVALID_PARAMETER;

	status = CdpCoreCaptureAppend(Core, Offset, Length, &record);
	if (!NT_SUCCESS(status))
		return status;

	return CdpCoreSourceWriteDirect(Core, Offset, Length, Data);
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

	if (Core->Phase == Cdp_CORE_PHASE_RECOVERY)
	{
		Cdp_RECOVERY_DBG(
			"synthesize tree begin offset=%llu len=%lu\n",
			Offset,
			Length);
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
	if (Core->Phase == Cdp_CORE_PHASE_RECOVERY)
	{
		Cdp_RECOVERY_DBG(
			"synthesize tree end offset=%llu len=%lu covered=%lu "
			"status=0x%08X\n",
			Offset,
			Length,
			coveredCount,
			status);
	}
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
static NTSTATUS CdpCoreSynthesizeReadWithStaging(
	_Inout_ PCdp_CORE Core,
	_In_ PCdp_PREVIEW_TREE PrimaryTree,
	_In_ BOOLEAN StagingUsesLiveSource,
	_In_ UINT64 Offset,
	_In_ ULONG Length,
	_Out_writes_bytes_(Length) PVOID Buffer)
{
	PUCHAR stagingMask = NULL;
	ULONG stagingCovered = 0;
	ULONG maskBytes = (Length + 7UL) / 8UL;
	ULONG i;
	NTSTATUS status;

	status = CdpCoreSynthesizeRead(
		Core, PrimaryTree, Offset, Length, Buffer);
	if (!NT_SUCCESS(status) || !Core->Building)
		return status;

	stagingMask = (PUCHAR)Cdp_ALLOC0(maskBytes);
	if (!stagingMask)
		return STATUS_INSUFFICIENT_RESOURCES;
	/* Preview staging holds before-images and therefore overlays the primary
	 * tree directly.  Recovery staging protects new writes during prepare, so
	 * its covered ranges must instead reflect the current live source. */
	status = CdpJournalApplyPreviewTree(
		Core->Journal,
		&Core->StagingTree,
		&Core->TreeLock,
		Offset,
		Length,
		Buffer,
		stagingMask,
		&stagingCovered);
	if (!NT_SUCCESS(status) || stagingCovered == 0 || !StagingUsesLiveSource)
		goto done;

	i = 0;
	while (i < Length)
	{
		ULONG runStart;
		while (i < Length &&
			(stagingMask[i >> 3] & (UCHAR)(1U << (i & 7))) == 0)
			++i;
		if (i >= Length)
			break;
		runStart = i;
		while (i < Length &&
			(stagingMask[i >> 3] & (UCHAR)(1U << (i & 7))) != 0)
			++i;
		status = CdpCoreSourceRead(
			Core,
			Offset + runStart,
			i - runStart,
			(PUCHAR)Buffer + runStart);
		if (!NT_SUCCESS(status))
			break;
	}

done:
	Cdp_FREE(stagingMask);
	return status;
}

NTSTATUS CdpCoreRead(
	_Inout_ PCdp_CORE Core,
	_In_ UINT64 Offset,
	_In_ ULONG Length,
	_Out_writes_bytes_(Length) PVOID Buffer)
{
	if (!Core || !Buffer || Length == 0)
		return STATUS_INVALID_PARAMETER;

	if (Core->Phase == Cdp_CORE_PHASE_PREVIEW)
	{
		return CdpCoreSynthesizeReadWithStaging(
			Core, &Core->PreviewTree, FALSE, Offset, Length, Buffer);
	}
	if (Core->Phase == Cdp_CORE_PHASE_RECOVERY)
	{
		return CdpCoreSynthesizeReadWithStaging(
			Core, &Core->HistoryTree, TRUE, Offset, Length, Buffer);
	}
	return CdpCoreSourceRead(Core, Offset, Length, Buffer);
}

NTSTATUS CdpCorePreviewBegin(_Inout_ PCdp_CORE Core, _In_ UINT64 TargetTime100ns)
{
	NTSTATUS status;

	if (!Core || Core->Phase != Cdp_CORE_PHASE_GENERAL)
		return STATUS_INVALID_DEVICE_STATE;
	if (!Core->Journal->Mounted)
		return STATUS_DEVICE_NOT_READY;

	Core->Phase = Cdp_CORE_PHASE_PREVIEW;
	Core->Building = 1;
	Core->TargetTime100ns = TargetTime100ns;
	Core->SnapshotMaxSequence = Core->Journal->NextSequence;
	CdpPreviewTreeFree(&Core->PreviewTree);
	CdpPreviewTreeFree(&Core->StagingTree);
	CdpPreviewTreeInitialize(&Core->PreviewTree);
	CdpPreviewTreeInitialize(&Core->StagingTree);

#ifdef Cdp_USERMODE
	if (g_previewBuildHook)
		g_previewBuildHook(Core);
#endif

	status = CdpJournalBuildPreviewTree(
		Core->Journal,
		TargetTime100ns,
		Core->SnapshotMaxSequence,
		TRUE,
		&Core->PreviewTree);
	if (!NT_SUCCESS(status) && status != STATUS_NOT_FOUND)
	{
		Core->Building = 0;
		Core->Phase = Cdp_CORE_PHASE_GENERAL;
		return status;
	}
	if (status == STATUS_NOT_FOUND)
		status = STATUS_SUCCESS;

	Cdp_LOCK_ACQUIRE(&Core->TreeLock);
	status = CdpPreviewTreeMergeFrom(&Core->PreviewTree, &Core->StagingTree);
	Core->Building = 0;
	Cdp_LOCK_RELEASE(&Core->TreeLock);
	if (!NT_SUCCESS(status))
	{
		Core->Phase = Cdp_CORE_PHASE_GENERAL;
		return status;
	}

	return STATUS_SUCCESS;
}

NTSTATUS CdpCorePreviewEnd(_Inout_ PCdp_CORE Core)
{
	if (!Core || Core->Phase != Cdp_CORE_PHASE_PREVIEW)
		return STATUS_INVALID_DEVICE_STATE;
	CdpPreviewTreeFree(&Core->PreviewTree);
	CdpPreviewTreeFree(&Core->StagingTree);
	CdpPreviewTreeInitialize(&Core->PreviewTree);
	CdpPreviewTreeInitialize(&Core->StagingTree);
	Core->Phase = Cdp_CORE_PHASE_GENERAL;
	return STATUS_SUCCESS;
}

static PCdp_PREVIEW_TREE_NODE CdpCoreFindEarliestHistoryNode(
	_In_opt_ PCdp_PREVIEW_TREE_NODE Node)
{
	UINT64 targetSequence;

	if (!Node || Node->MinValidSequence == MAXUINT64)
		return NULL;
	targetSequence = Node->MinValidSequence;

	while (Node)
	{
		if (Node->Left &&
			Node->Left->MinValidSequence == targetSequence)
		{
			Node = Node->Left;
			continue;
		}
		if (!Node->Invalid && Node->Sequence == targetSequence)
			return Node;
		if (Node->Right &&
			Node->Right->MinValidSequence == targetSequence)
		{
			Node = Node->Right;
			continue;
		}
		break;
	}
	return NULL;
}

NTSTATUS CdpCoreRecoveryCommitStep(
	_Inout_ PCdp_CORE Core,
	_Out_ PBOOLEAN Complete)
{
	PCdp_PREVIEW_TREE_NODE node;
	PUCHAR payload = NULL;
	ULONG writeOffset = 0;
	ULONG writeRuns = 0;
	NTSTATUS status = STATUS_SUCCESS;

	if (!Core || !Complete || Core->Phase != Cdp_CORE_PHASE_RECOVERY ||
		Core->Building)
	{
		return STATUS_INVALID_DEVICE_STATE;
	}
	if (!NT_SUCCESS(Core->RecoveryFailureStatus))
		return Core->RecoveryFailureStatus;

	*Complete = FALSE;
	Cdp_LOCK_ACQUIRE(&Core->TreeLock);
	node = CdpCoreFindEarliestHistoryNode(Core->HistoryTree.Root);
	if (!node)
	{
		CdpPreviewTreeFree(&Core->HistoryTree);
		CdpPreviewTreeFree(&Core->StagingTree);
		CdpPreviewTreeInitialize(&Core->HistoryTree);
		CdpPreviewTreeInitialize(&Core->StagingTree);
		Core->TargetTime100ns = 0;
		Core->SnapshotMaxSequence = 0;
		Core->RecoveryFailureStatus = STATUS_SUCCESS;
		Core->Phase = Cdp_CORE_PHASE_GENERAL;
		Cdp_LOCK_RELEASE(&Core->TreeLock);
		*Complete = TRUE;
		Cdp_RECOVERY_TRACE("commit complete -> normal\n");
		return STATUS_SUCCESS;
	}
	Cdp_LOCK_RELEASE(&Core->TreeLock);

	payload = (PUCHAR)Cdp_ALLOC(node->DataLength);
	if (!payload)
		return STATUS_INSUFFICIENT_RESOURCES;
	status = CdpJournalReadPayload(
		Core->Journal, node->FileOffset, node->DataLength, payload);
	if (!NT_SUCCESS(status))
		goto done;

	Core->WritebackActive = 1;
	Cdp_RECOVERY_TRACE(
		"commit step seq=%llu volumeOff=%llu len=%lu journalOff=%llu\n",
		node->Sequence, node->Start, node->DataLength, node->FileOffset);
	while (writeOffset < node->DataLength)
	{
		ULONG writeLength = node->DataLength - writeOffset;
		if (writeLength > Cdp_JOURNAL_MAX_RECORD_DATA)
			writeLength = Cdp_JOURNAL_MAX_RECORD_DATA;
		status = CdpCoreSourceWriteDirect(
			Core,
			node->Start + writeOffset,
			writeLength,
			payload + writeOffset);
		Cdp_RECOVERY_TRACE(
			"restore seq=%llu volumeOff=%llu len=%lu journalOff=%llu status=0x%08X\n",
			node->Sequence,
			node->Start + writeOffset,
			writeLength,
			node->FileOffset + writeOffset,
			status);
		if (!NT_SUCCESS(status))
			break;
		writeOffset += writeLength;
		++writeRuns;
	}
	Core->WritebackActive = 0;
	if (NT_SUCCESS(status))
	{
		Cdp_LOCK_ACQUIRE(&Core->TreeLock);
		status = CdpPreviewTreeMarkInvalidByStart(
			&Core->HistoryTree, node->Start);
		if (!NT_SUCCESS(status))
			Core->RecoveryFailureStatus = status;
		Cdp_LOCK_RELEASE(&Core->TreeLock);
		if (NT_SUCCESS(status))
		{
			Cdp_RECOVERY_TRACE(
				"commit step complete seq=%llu runs=%lu bytes=%lu\n",
				node->Sequence, writeRuns, node->DataLength);
		}
	}

done:
	Core->WritebackActive = 0;
	Cdp_FREE(payload);
	return status;
}

NTSTATUS CdpCoreRecoveryBegin(_Inout_ PCdp_CORE Core, _In_ UINT64 TargetTime100ns)
{
	NTSTATUS status;

	if (!Core || Core->Phase != Cdp_CORE_PHASE_GENERAL)
		return STATUS_INVALID_DEVICE_STATE;
	if (!Core->Journal->Mounted)
		return STATUS_DEVICE_NOT_READY;

	Core->Phase = Cdp_CORE_PHASE_RECOVERY;
	Core->Building = 1;
	Core->RecoveryFailureStatus = STATUS_SUCCESS;
	Core->TargetTime100ns = TargetTime100ns;
	Core->SnapshotMaxSequence = Core->Journal->NextSequence;
	Cdp_RECOVERY_TRACE(
		"begin target=%llu snapshotMaxSeq=%llu records=%llu range=[%llu,%llu]\n",
		TargetTime100ns,
		Core->SnapshotMaxSequence,
		Core->Journal->TotalRecords,
		Core->Journal->Oldest100ns,
		Core->Journal->Newest100ns);
	CdpPreviewTreeFree(&Core->HistoryTree);
	CdpPreviewTreeFree(&Core->StagingTree);
	CdpPreviewTreeInitialize(&Core->HistoryTree);
	CdpPreviewTreeInitialize(&Core->StagingTree);

#ifdef Cdp_USERMODE
	if (g_recoveryBuildHook)
		g_recoveryBuildHook(Core);
#endif

	status = CdpJournalBuildPreviewTree(
		Core->Journal,
		TargetTime100ns,
		Core->SnapshotMaxSequence,
		TRUE,
		&Core->HistoryTree);
	if (!NT_SUCCESS(status) && status != STATUS_NOT_FOUND)
	{
		Core->Building = 0;
		Core->Phase = Cdp_CORE_PHASE_GENERAL;
		return status;
	}
	if (status == STATUS_NOT_FOUND)
		status = STATUS_SUCCESS;
	Cdp_RECOVERY_TRACE("tree build status=0x%08X nodes=%lu staging=%lu\n",
		status,
		Core->HistoryTree.NodeCount,
		Core->StagingTree.NodeCount);

	Cdp_LOCK_ACQUIRE(&Core->TreeLock);
	status = CdpPreviewTreePunchByStaging(&Core->HistoryTree, &Core->StagingTree);
	Core->Building = 0;
	Cdp_LOCK_RELEASE(&Core->TreeLock);
	if (!NT_SUCCESS(status))
	{
		Core->Phase = Cdp_CORE_PHASE_GENERAL;
		return status;
	}

	Cdp_RECOVERY_TRACE(
		"prepared target=%llu nodes=%lu; waiting for commit\n",
		Core->TargetTime100ns,
		Core->HistoryTree.NodeCount);
	return STATUS_SUCCESS;
}

NTSTATUS CdpCoreRecoveryCommit(_Inout_ PCdp_CORE Core)
{
	NTSTATUS status = STATUS_SUCCESS;
	BOOLEAN complete = FALSE;

	if (!Core || Core->Phase != Cdp_CORE_PHASE_RECOVERY || Core->Building)
		return STATUS_INVALID_DEVICE_STATE;
	if (!NT_SUCCESS(Core->RecoveryFailureStatus))
	{
		Cdp_RECOVERY_ERR("commit rejected: recovery previously failed status=0x%08X\n",
			Core->RecoveryFailureStatus);
		return Core->RecoveryFailureStatus;
	}

	Cdp_RECOVERY_TRACE(
		"commit begin target=%llu nodes=%lu\n",
		Core->TargetTime100ns,
		Core->HistoryTree.NodeCount);
#ifdef Cdp_USERMODE
	if (g_writebackHook)
	{
		Core->WritebackActive = 1;
		g_writebackHook(Core);
		Core->WritebackActive = 0;
	}
#endif

	while (!complete)
	{
		status = CdpCoreRecoveryCommitStep(Core, &complete);
		if (!NT_SUCCESS(status))
		{
			Cdp_RECOVERY_TRACE(
				"commit failed status=0x%08X; remaining in Recovery\n",
				status);
			return status;
		}
	}
	return status;
}

NTSTATUS CdpCoreRecoveryCancel(_Inout_ PCdp_CORE Core)
{
	if (!Core || Core->Phase != Cdp_CORE_PHASE_RECOVERY || Core->Building)
		return STATUS_INVALID_DEVICE_STATE;

	Cdp_RECOVERY_DBG(
		"cancel target=%llu nodes=%lu -> normal\n",
		Core->TargetTime100ns,
		Core->HistoryTree.NodeCount);
	CdpPreviewTreeFree(&Core->HistoryTree);
	CdpPreviewTreeFree(&Core->StagingTree);
	CdpPreviewTreeInitialize(&Core->HistoryTree);
	CdpPreviewTreeInitialize(&Core->StagingTree);
	Core->TargetTime100ns = 0;
	Core->SnapshotMaxSequence = 0;
	Core->Building = 0;
	Core->WritebackActive = 0;
	Core->RecoveryFailureStatus = STATUS_SUCCESS;
	Core->Phase = Cdp_CORE_PHASE_GENERAL;
	return STATUS_SUCCESS;
}
