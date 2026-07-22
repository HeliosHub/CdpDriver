#include "cdp_core.h"
#include "cdp_alloc.h"
#include "CdpJournal.h"

#ifndef Cdp_USERMODE
#include "cdp_dev_store.h"
#if DBG
#define Cdp_RECOVERY_DBG(fmt, ...) \
	DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, \
		"CdpDriver: [RECOVERY] " fmt, ##__VA_ARGS__)
#else
#define Cdp_RECOVERY_DBG(fmt, ...) ((void)0)
#endif
#else
#define Cdp_RECOVERY_DBG(fmt, ...) ((void)0)
#endif
#define Cdp_RECOVERY_DETAIL_LIMIT 512UL

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
	ULONG SnapshotMaxSequence;
	LONG Building;
	LONG WritebackActive;
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

Cdp_CORE_PHASE CdpCoreGetPhase(_In_ PCdp_CORE Core)
{
	if (!Core)
		return Cdp_CORE_PHASE_GENERAL;
	return (Cdp_CORE_PHASE)Core->Phase;
}

VOID CdpCoreSetPhase(_Inout_ PCdp_CORE Core, _In_ Cdp_CORE_PHASE Phase)
{
	if (Core)
		Core->Phase = (LONG)Phase;
}

VOID CdpCoreSetWritebackActive(_Inout_ PCdp_CORE Core, _In_ LONG Value)
{
	if (Core)
		Core->WritebackActive = Value;
}

static VOID CdpCoreAfterAppend(
	_Inout_ PCdp_CORE Core,
	_In_ const Cdp_JOURNAL_RECORD_HEADER* Hdr,
	_In_ UINT64 Offset,
	_In_ ULONG Length)
{
	if (Core->Phase == Cdp_CORE_PHASE_PREVIEW)
	{
		Cdp_LOCK_ACQUIRE(&Core->TreeLock);
		if (Core->Building)
		{
			if (Hdr->Sequence >= Core->SnapshotMaxSequence)
				(void)CdpPreviewTreeInsert(&Core->StagingTree, Hdr);
		}
		else
		{
			(void)CdpPreviewTreeInsert(&Core->PreviewTree, Hdr);
		}
		Cdp_LOCK_RELEASE(&Core->TreeLock);
	}
	else if (Core->Phase == Cdp_CORE_PHASE_RECOVERY && !Core->WritebackActive)
	{
		Cdp_LOCK_ACQUIRE(&Core->TreeLock);
		if (Core->Building)
		{
			if (Hdr->Sequence >= Core->SnapshotMaxSequence)
				(void)CdpPreviewTreeInsert(&Core->StagingTree, Hdr);
		}
		else
		{
			CdpPreviewTreeInvalidateRange(
				&Core->HistoryTree,
				Offset,
				Length);
		}
		Cdp_LOCK_RELEASE(&Core->TreeLock);
	}
}

NTSTATUS CdpCoreCaptureAppend(
	_Inout_ PCdp_CORE Core,
	_In_ UINT64 Offset,
	_In_ ULONG Length,
	_Out_opt_ PCdp_JOURNAL_RECORD_HEADER WrittenHeader)
{
	PUCHAR before = NULL;
	NTSTATUS status;
	Cdp_JOURNAL_RECORD_HEADER hdr;

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
	status = CdpJournalAppend(Core->Journal, Offset, Length, before, &hdr);
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
			"seq=%lu journalOff=%llu status=0x%08X\n",
			Offset,
			Length,
			hdr.Sequence,
			hdr.FileOffset,
			status);
	}

	CdpCoreAfterAppend(Core, &hdr, Offset, Length);
	Core->Time100ns += 1;
	if (WrittenHeader)
		*WrittenHeader = hdr;

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
	Cdp_JOURNAL_RECORD_HEADER hdr;

	if (!Core || !Data || Length == 0)
		return STATUS_INVALID_PARAMETER;

	status = CdpCoreCaptureAppend(Core, Offset, Length, &hdr);
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
		return CdpCoreSynthesizeRead(
			Core,
			&Core->PreviewTree,
			Offset,
			Length,
			Buffer);
	}
	if (Core->Phase == Cdp_CORE_PHASE_RECOVERY)
	{
		return CdpCoreSynthesizeRead(
			Core,
			&Core->HistoryTree,
			Offset,
			Length,
			Buffer);
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
	if (NT_SUCCESS(status))
		status = CdpPreviewTreeDedupEarliest(&Core->PreviewTree);
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

static NTSTATUS CdpCoreWritebackHistory(_Inout_ PCdp_CORE Core)
{
	PCdp_PREVIEW_TREE_NODE* all = NULL;
	ULONG capacity;
	ULONG count = 0;
	ULONG i;
	ULONG writeRuns = 0;
	UINT64 writeBytes = 0;
	NTSTATUS status = STATUS_SUCCESS;
	PCdp_PREVIEW_TREE_NODE stack[64];
	LONG sp = 0;
	PCdp_PREVIEW_TREE_NODE cur;

	if (!Core->HistoryTree.Root || Core->HistoryTree.NodeCount == 0)
	{
		Cdp_RECOVERY_DBG("writeback skipped: history tree is empty\n");
		return STATUS_SUCCESS;
	}

	capacity = Core->HistoryTree.NodeCount;
	all = (PCdp_PREVIEW_TREE_NODE*)Cdp_ALLOC0(
		sizeof(PCdp_PREVIEW_TREE_NODE) * capacity);
	if (!all)
		return STATUS_INSUFFICIENT_RESOURCES;

	cur = Core->HistoryTree.Root;
	while (cur || sp > 0)
	{
		while (cur)
		{
			if (sp >= 64)
			{
				status = STATUS_INSUFFICIENT_RESOURCES;
				goto done;
			}
			stack[sp++] = cur;
			cur = cur->Left;
		}
		cur = stack[--sp];
		if (!cur->Invalid && count < capacity)
			all[count++] = cur;
		cur = cur->Right;
	}

	for (i = 1; i < count; ++i)
	{
		PCdp_PREVIEW_TREE_NODE key = all[i];
		ULONG j = i;
		while (j > 0 && all[j - 1]->Sequence > key->Sequence)
		{
			all[j] = all[j - 1];
			--j;
		}
		all[j] = key;
	}

	Core->WritebackActive = 1;
	Cdp_RECOVERY_DBG("writeback begin nodes=%lu detailLimit=%lu\n",
		count, Cdp_RECOVERY_DETAIL_LIMIT);
#ifdef Cdp_USERMODE
	if (g_writebackHook)
		g_writebackHook(Core);
#endif
	for (i = 0; i < count; ++i)
	{
		PCdp_PREVIEW_TREE_NODE node = all[i];
		PUCHAR payload;
		PUCHAR mask;
		ULONG idx;
		ULONG runStart;
		ULONG a;

		if (node->Invalid)
			continue;

		payload = (PUCHAR)Cdp_ALLOC(node->DataLength);
		mask = (PUCHAR)Cdp_ALLOC(node->DataLength);
		if (!payload || !mask)
		{
			status = STATUS_INSUFFICIENT_RESOURCES;
			Cdp_FREE(payload);
			Cdp_FREE(mask);
			break;
		}

		status = CdpJournalReadPayload(
			Core->Journal,
			node->FileOffset,
			node->DataLength,
			payload);
		if (!NT_SUCCESS(status))
		{
			Cdp_FREE(payload);
			Cdp_FREE(mask);
			break;
		}

		if (i < Cdp_RECOVERY_DETAIL_LIMIT)
		{
			Cdp_RECOVERY_DBG(
				"node index=%lu seq=%lu time=%llu volumeOff=%llu len=%lu "
				"journalOff=%llu head=%02X%02X%02X%02X\n",
				i,
				node->Sequence,
				node->WallClock100ns,
				node->Start,
				node->DataLength,
				node->FileOffset,
				node->DataLength > 0 ? payload[0] : 0,
				node->DataLength > 1 ? payload[1] : 0,
				node->DataLength > 2 ? payload[2] : 0,
				node->DataLength > 3 ? payload[3] : 0);
		}

		RtlFillMemory(mask, node->DataLength, 1);
		for (a = 0; a < i; ++a)
		{
			PCdp_PREVIEW_TREE_NODE an = all[a];
			UINT64 o0 = an->Start > node->Start ? an->Start : node->Start;
			UINT64 o1 = an->End < node->End ? an->End : node->End;
			UINT64 b;
			if (an->Invalid)
				continue;
			for (b = o0; b < o1; ++b)
				mask[(ULONG)(b - node->Start)] = 0;
		}

		idx = 0;
		while (idx < node->DataLength)
		{
			while (idx < node->DataLength && mask[idx] == 0)
				++idx;
			if (idx >= node->DataLength)
				break;
			runStart = idx;
			while (idx < node->DataLength && mask[idx] != 0)
				++idx;
			status = CdpCoreWrite(
				Core,
				node->Start + runStart,
				idx - runStart,
				payload + runStart);
			if (i < Cdp_RECOVERY_DETAIL_LIMIT)
			{
				Cdp_RECOVERY_DBG(
					"write node=%lu seq=%lu offset=%llu len=%lu status=0x%08X\n",
					i,
					node->Sequence,
					node->Start + runStart,
					idx - runStart,
					status);
			}
			if (!NT_SUCCESS(status))
			{
				Cdp_FREE(payload);
				Cdp_FREE(mask);
				goto done;
			}
			++writeRuns;
			writeBytes += idx - runStart;
		}
		Cdp_FREE(payload);
		Cdp_FREE(mask);
	}

done:
	Core->WritebackActive = 0;
	if (count > Cdp_RECOVERY_DETAIL_LIMIT)
	{
		Cdp_RECOVERY_DBG("detail suppressed for %lu nodes\n",
			count - Cdp_RECOVERY_DETAIL_LIMIT);
	}
	Cdp_RECOVERY_DBG(
		"writeback end status=0x%08X nodes=%lu runs=%lu bytes=%llu\n",
		status,
		count,
		writeRuns,
		writeBytes);
	Cdp_FREE(all);
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
	Core->TargetTime100ns = TargetTime100ns;
	Core->SnapshotMaxSequence = Core->Journal->NextSequence;
	Cdp_RECOVERY_DBG(
		"begin target=%llu snapshotMaxSeq=%lu records=%llu range=[%llu,%llu]\n",
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
	Cdp_RECOVERY_DBG("tree build status=0x%08X nodes=%lu staging=%lu\n",
		status,
		Core->HistoryTree.NodeCount,
		Core->StagingTree.NodeCount);

	Cdp_LOCK_ACQUIRE(&Core->TreeLock);
	CdpPreviewTreePunchByStaging(&Core->HistoryTree, &Core->StagingTree);
	status = CdpPreviewTreeDedupEarliest(&Core->HistoryTree);
	Core->Building = 0;
	Cdp_LOCK_RELEASE(&Core->TreeLock);
	if (!NT_SUCCESS(status))
	{
		Core->Phase = Cdp_CORE_PHASE_GENERAL;
		return status;
	}

	Cdp_RECOVERY_DBG(
		"prepared target=%llu nodes=%lu; waiting for commit\n",
		Core->TargetTime100ns,
		Core->HistoryTree.NodeCount);
	return STATUS_SUCCESS;
}

NTSTATUS CdpCoreRecoveryCommit(_Inout_ PCdp_CORE Core)
{
	NTSTATUS status;

	if (!Core || Core->Phase != Cdp_CORE_PHASE_RECOVERY || Core->Building)
		return STATUS_INVALID_DEVICE_STATE;

	Cdp_RECOVERY_DBG(
		"commit begin target=%llu nodes=%lu\n",
		Core->TargetTime100ns,
		Core->HistoryTree.NodeCount);
	status = CdpCoreWritebackHistory(Core);
	if (!NT_SUCCESS(status))
	{
		Cdp_RECOVERY_DBG(
			"commit failed status=0x%08X; remaining in Recovery\n",
			status);
		return status;
	}

	CdpPreviewTreeFree(&Core->HistoryTree);
	CdpPreviewTreeFree(&Core->StagingTree);
	CdpPreviewTreeInitialize(&Core->HistoryTree);
	CdpPreviewTreeInitialize(&Core->StagingTree);
	Core->TargetTime100ns = 0;
	Core->SnapshotMaxSequence = 0;
	Core->Phase = Cdp_CORE_PHASE_GENERAL;
	Cdp_RECOVERY_DBG("commit complete -> normal\n");
	return STATUS_SUCCESS;
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
	Core->Phase = Cdp_CORE_PHASE_GENERAL;
	return STATUS_SUCCESS;
}
