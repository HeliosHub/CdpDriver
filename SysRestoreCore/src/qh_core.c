#include "qh_core.h"
#include "qh_alloc.h"
#include "QHJournal.h"

#ifndef QH_USERMODE
#include "qh_dev_store.h"
#if DBG
#define QH_RECOVERY_DBG(fmt, ...) \
	DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, \
		"SysRestoreDriver: [RECOVERY] " fmt, ##__VA_ARGS__)
#else
#define QH_RECOVERY_DBG(fmt, ...) ((void)0)
#endif
#else
#define QH_RECOVERY_DBG(fmt, ...) ((void)0)
#endif
#define QH_RECOVERY_DETAIL_LIMIT 512UL

#ifdef QH_USERMODE
#include <string.h>

typedef void (*QH_CORE_TEST_BUILD_HOOK)(_Inout_ PQH_CORE Core);
typedef void (*QH_CORE_TEST_WRITEBACK_HOOK)(_Inout_ PQH_CORE Core);
static QH_CORE_TEST_BUILD_HOOK g_previewBuildHook;
static QH_CORE_TEST_BUILD_HOOK g_recoveryBuildHook;
static QH_CORE_TEST_WRITEBACK_HOOK g_writebackHook;

VOID QhCoreTestSetPreviewBuildHook(_In_opt_ QH_CORE_TEST_BUILD_HOOK Hook)
{
	g_previewBuildHook = Hook;
}

VOID QhCoreTestSetRecoveryBuildHook(_In_opt_ QH_CORE_TEST_BUILD_HOOK Hook)
{
	g_recoveryBuildHook = Hook;
}

VOID QhCoreTestSetWritebackHook(_In_opt_ QH_CORE_TEST_WRITEBACK_HOOK Hook)
{
	g_writebackHook = Hook;
}
#endif

struct _QH_CORE
{
	PQH_STORE Source;
	PQH_STORE JournalStore;
	QH_JOURNAL JournalStorage;
	PQH_JOURNAL Journal;
	BOOLEAN OwnsJournal;
	LONG Phase;
	UINT64 Time100ns;
	QH_PREVIEW_TREE PreviewTree;
	QH_PREVIEW_TREE HistoryTree;
	QH_PREVIEW_TREE StagingTree;
	QH_LOCK TreeLock;
	UINT64 TargetTime100ns;
	ULONG SnapshotMaxSequence;
	LONG Building;
	LONG WritebackActive;
	GUID SourceGuid;
};

static UINT64 QhCoreQueryTime(_In_opt_ PVOID Context)
{
	PQH_CORE core = (PQH_CORE)Context;
	return core ? core->Time100ns : 0;
}

static NTSTATUS QhCoreSourceRead(
	_In_ PQH_CORE Core,
	_In_ UINT64 Offset,
	_In_ ULONG Length,
	_Out_writes_bytes_(Length) PVOID Buffer)
{
	NTSTATUS status;

	if (Core->Phase == QH_CORE_PHASE_RECOVERY)
	{
		QH_RECOVERY_DBG(
			"source read begin offset=%llu len=%lu\n",
			Offset,
			Length);
	}
	status = Core->Source->Read(Core->Source, Offset, Length, Buffer);
	if (Core->Phase == QH_CORE_PHASE_RECOVERY)
	{
		QH_RECOVERY_DBG(
			"source read end offset=%llu len=%lu status=0x%08X\n",
			Offset,
			Length,
			status);
	}
	return status;
}

static NTSTATUS QhCoreSourceWriteDirect(
	_In_ PQH_CORE Core,
	_In_ UINT64 Offset,
	_In_ ULONG Length,
	_In_reads_bytes_(Length) const VOID* Buffer)
{
	NTSTATUS status;

	if (Core->WritebackActive)
	{
		QH_RECOVERY_DBG(
			"source write begin offset=%llu len=%lu\n",
			Offset,
			Length);
	}
	status = Core->Source->Write(Core->Source, Offset, Length, Buffer);
	if (Core->WritebackActive)
	{
		QH_RECOVERY_DBG(
			"source write end offset=%llu len=%lu status=0x%08X\n",
			Offset,
			Length,
			status);
	}
	return status;
}

static VOID QhCoreInitCommon(_Inout_ PQH_CORE Core)
{
	Core->Time100ns = 1;
	Core->Phase = QH_CORE_PHASE_GENERAL;
	QHPreviewTreeInitialize(&Core->PreviewTree);
	QHPreviewTreeInitialize(&Core->HistoryTree);
	QHPreviewTreeInitialize(&Core->StagingTree);
	QH_LOCK_INIT(&Core->TreeLock);
}

NTSTATUS QhCoreCreate(
	_In_ PQH_STORE Source,
	_In_ PQH_STORE Journal,
	_Outptr_ PQH_CORE* OutCore)
{
	PQH_CORE core;
	GUID zero = { 0 };

	if (!Source || !Journal || !OutCore)
		return STATUS_INVALID_PARAMETER;

	core = (PQH_CORE)QH_ALLOC0(sizeof(*core));
	if (!core)
		return STATUS_INSUFFICIENT_RESOURCES;

	core->Source = Source;
	core->JournalStore = Journal;
	core->Journal = &core->JournalStorage;
	core->OwnsJournal = TRUE;
	core->SourceGuid = zero;
	QhCoreInitCommon(core);

	QHJournalInitializeWithStore(
		core->Journal,
		Journal,
		&core->SourceGuid,
		QhCoreQueryTime,
		core);

	*OutCore = core;
	return STATUS_SUCCESS;
}

#ifndef QH_USERMODE
NTSTATUS QhCoreBind(
	_In_ PQH_STORE Source,
	_Inout_ PQH_JOURNAL Journal,
	_In_ const GUID* SourceVolumeGuid,
	_Outptr_ PQH_CORE* OutCore)
{
	PQH_CORE core;

	if (!Source || !Journal || !SourceVolumeGuid || !OutCore)
		return STATUS_INVALID_PARAMETER;

	core = (PQH_CORE)QH_ALLOC0(sizeof(*core));
	if (!core)
		return STATUS_INSUFFICIENT_RESOURCES;

	core->Source = Source;
	core->JournalStore = NULL;
	core->Journal = Journal;
	core->OwnsJournal = FALSE;
	core->SourceGuid = *SourceVolumeGuid;
	QhCoreInitCommon(core);

	(*OutCore) = core;
	return STATUS_SUCCESS;
}
#endif

VOID QhCoreDestroy(_Inout_opt_ PQH_CORE Core)
{
	if (!Core)
		return;
	if (Core->OwnsJournal)
		QHJournalClose(Core->Journal);
	QHPreviewTreeFree(&Core->PreviewTree);
	QHPreviewTreeFree(&Core->HistoryTree);
	QHPreviewTreeFree(&Core->StagingTree);
	QH_LOCK_DELETE(&Core->TreeLock);
#ifndef QH_USERMODE
	if (Core->Source)
		QhDevStoreDestroy(Core->Source);
#endif
	QH_FREE(Core);
}

VOID QhCoreSetTime100ns(_Inout_ PQH_CORE Core, _In_ UINT64 Time100ns)
{
	if (Core)
		Core->Time100ns = Time100ns;
}

UINT64 QhCoreGetTime100ns(_In_ PQH_CORE Core)
{
	return Core ? Core->Time100ns : 0;
}

UINT64 QhCoreGetTargetTime100ns(_In_ PQH_CORE Core)
{
	return Core ? Core->TargetTime100ns : 0;
}

UINT64 QhCoreTick(_Inout_ PQH_CORE Core, _In_ UINT64 Delta100ns)
{
	if (!Core)
		return 0;
	Core->Time100ns += Delta100ns;
	return Core->Time100ns;
}

NTSTATUS QhCoreFormatJournal(_Inout_ PQH_CORE Core)
{
	if (!Core)
		return STATUS_INVALID_PARAMETER;
	return QHJournalFormat(Core->Journal);
}

NTSTATUS QhCoreMountJournal(_Inout_ PQH_CORE Core)
{
	if (!Core)
		return STATUS_INVALID_PARAMETER;
	return QHJournalMount(Core->Journal);
}

NTSTATUS QhCoreQueryTimeRange(
	_Inout_ PQH_CORE Core,
	_Out_ PUINT64 OldestTime100ns,
	_Out_ PUINT64 NewestTime100ns)
{
	if (!Core)
		return STATUS_INVALID_PARAMETER;
	return QHJournalQueryTimeRange(
		Core->Journal,
		OldestTime100ns,
		NewestTime100ns);
}

QH_CORE_PHASE QhCoreGetPhase(_In_ PQH_CORE Core)
{
	if (!Core)
		return QH_CORE_PHASE_GENERAL;
	return (QH_CORE_PHASE)Core->Phase;
}

VOID QhCoreSetPhase(_Inout_ PQH_CORE Core, _In_ QH_CORE_PHASE Phase)
{
	if (Core)
		Core->Phase = (LONG)Phase;
}

VOID QhCoreSetWritebackActive(_Inout_ PQH_CORE Core, _In_ LONG Value)
{
	if (Core)
		Core->WritebackActive = Value;
}

static VOID QhCoreAfterAppend(
	_Inout_ PQH_CORE Core,
	_In_ const QH_JOURNAL_RECORD_HEADER* Hdr,
	_In_ UINT64 Offset,
	_In_ ULONG Length)
{
	if (Core->Phase == QH_CORE_PHASE_PREVIEW)
	{
		QH_LOCK_ACQUIRE(&Core->TreeLock);
		if (Core->Building)
		{
			if (Hdr->Sequence >= Core->SnapshotMaxSequence)
				(void)QHPreviewTreeInsert(&Core->StagingTree, Hdr);
		}
		else
		{
			(void)QHPreviewTreeInsert(&Core->PreviewTree, Hdr);
		}
		QH_LOCK_RELEASE(&Core->TreeLock);
	}
	else if (Core->Phase == QH_CORE_PHASE_RECOVERY && !Core->WritebackActive)
	{
		QH_LOCK_ACQUIRE(&Core->TreeLock);
		if (Core->Building)
		{
			if (Hdr->Sequence >= Core->SnapshotMaxSequence)
				(void)QHPreviewTreeInsert(&Core->StagingTree, Hdr);
		}
		else
		{
			QHPreviewTreeInvalidateRange(
				&Core->HistoryTree,
				Offset,
				Length);
		}
		QH_LOCK_RELEASE(&Core->TreeLock);
	}
}

NTSTATUS QhCoreCaptureAppend(
	_Inout_ PQH_CORE Core,
	_In_ UINT64 Offset,
	_In_ ULONG Length,
	_Out_opt_ PQH_JOURNAL_RECORD_HEADER WrittenHeader)
{
	PUCHAR before = NULL;
	NTSTATUS status;
	QH_JOURNAL_RECORD_HEADER hdr;

	if (!Core || Length == 0)
		return STATUS_INVALID_PARAMETER;
	if (!Core->Journal->Mounted)
		return STATUS_DEVICE_NOT_READY;

	before = (PUCHAR)QH_ALLOC(Length);
	if (!before)
		return STATUS_INSUFFICIENT_RESOURCES;

	status = QhCoreSourceRead(Core, Offset, Length, before);
	if (!NT_SUCCESS(status))
		goto done;

	if (Core->WritebackActive)
	{
		QH_RECOVERY_DBG(
			"capture journal append begin offset=%llu len=%lu\n",
			Offset,
			Length);
	}
	status = QHJournalAppend(Core->Journal, Offset, Length, before, &hdr);
	if (!NT_SUCCESS(status))
	{
		if (Core->WritebackActive)
		{
			QH_RECOVERY_DBG(
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
		QH_RECOVERY_DBG(
			"capture journal append end offset=%llu len=%lu "
			"seq=%lu journalOff=%llu status=0x%08X\n",
			Offset,
			Length,
			hdr.Sequence,
			hdr.FileOffset,
			status);
	}

	QhCoreAfterAppend(Core, &hdr, Offset, Length);
	Core->Time100ns += 1;
	if (WrittenHeader)
		*WrittenHeader = hdr;

done:
	QH_FREE(before);
	return status;
}

NTSTATUS QhCoreWrite(
	_Inout_ PQH_CORE Core,
	_In_ UINT64 Offset,
	_In_ ULONG Length,
	_In_reads_bytes_(Length) const VOID* Data)
{
	NTSTATUS status;
	QH_JOURNAL_RECORD_HEADER hdr;

	if (!Core || !Data || Length == 0)
		return STATUS_INVALID_PARAMETER;

	status = QhCoreCaptureAppend(Core, Offset, Length, &hdr);
	if (!NT_SUCCESS(status))
		return status;

	return QhCoreSourceWriteDirect(Core, Offset, Length, Data);
}

static NTSTATUS QhCoreSynthesizeRead(
	_Inout_ PQH_CORE Core,
	_In_ PQH_PREVIEW_TREE Tree,
	_In_ UINT64 Offset,
	_In_ ULONG Length,
	_Out_writes_bytes_(Length) PVOID Buffer)
{
	PUCHAR coveredMask = NULL;
	ULONG coveredCount = 0;
	NTSTATUS status;
	ULONG i;
	ULONG maskBytes = (Length + 7UL) / 8UL;

	coveredMask = (PUCHAR)QH_ALLOC0(maskBytes);
	if (!coveredMask)
	{
		status = STATUS_INSUFFICIENT_RESOURCES;
		goto done;
	}

	if (Core->Phase == QH_CORE_PHASE_RECOVERY)
	{
		QH_RECOVERY_DBG(
			"synthesize tree begin offset=%llu len=%lu\n",
			Offset,
			Length);
	}
	status = QHJournalApplyPreviewTree(
		Core->Journal,
		Tree,
		&Core->TreeLock,
		Offset,
		Length,
		Buffer,
		coveredMask,
		&coveredCount);
	if (Core->Phase == QH_CORE_PHASE_RECOVERY)
	{
		QH_RECOVERY_DBG(
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
		status = QhCoreSourceRead(Core, Offset, Length, Buffer);
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

			status = QhCoreSourceRead(
				Core,
				Offset + runStart,
				i - runStart,
				(PUCHAR)Buffer + runStart);
			if (!NT_SUCCESS(status))
				goto done;
		}
	}

done:
	QH_FREE(coveredMask);
	return status;
}

NTSTATUS QhCoreRead(
	_Inout_ PQH_CORE Core,
	_In_ UINT64 Offset,
	_In_ ULONG Length,
	_Out_writes_bytes_(Length) PVOID Buffer)
{
	if (!Core || !Buffer || Length == 0)
		return STATUS_INVALID_PARAMETER;

	if (Core->Phase == QH_CORE_PHASE_PREVIEW)
	{
		return QhCoreSynthesizeRead(
			Core,
			&Core->PreviewTree,
			Offset,
			Length,
			Buffer);
	}
	if (Core->Phase == QH_CORE_PHASE_RECOVERY)
	{
		return QhCoreSynthesizeRead(
			Core,
			&Core->HistoryTree,
			Offset,
			Length,
			Buffer);
	}
	return QhCoreSourceRead(Core, Offset, Length, Buffer);
}

NTSTATUS QhCorePreviewBegin(_Inout_ PQH_CORE Core, _In_ UINT64 TargetTime100ns)
{
	NTSTATUS status;

	if (!Core || Core->Phase != QH_CORE_PHASE_GENERAL)
		return STATUS_INVALID_DEVICE_STATE;
	if (!Core->Journal->Mounted)
		return STATUS_DEVICE_NOT_READY;

	Core->Phase = QH_CORE_PHASE_PREVIEW;
	Core->Building = 1;
	Core->TargetTime100ns = TargetTime100ns;
	Core->SnapshotMaxSequence = Core->Journal->NextSequence;
	QHPreviewTreeFree(&Core->PreviewTree);
	QHPreviewTreeFree(&Core->StagingTree);
	QHPreviewTreeInitialize(&Core->PreviewTree);
	QHPreviewTreeInitialize(&Core->StagingTree);

#ifdef QH_USERMODE
	if (g_previewBuildHook)
		g_previewBuildHook(Core);
#endif

	status = QHJournalBuildPreviewTree(
		Core->Journal,
		TargetTime100ns,
		Core->SnapshotMaxSequence,
		TRUE,
		&Core->PreviewTree);
	if (!NT_SUCCESS(status) && status != STATUS_NOT_FOUND)
	{
		Core->Building = 0;
		Core->Phase = QH_CORE_PHASE_GENERAL;
		return status;
	}
	if (status == STATUS_NOT_FOUND)
		status = STATUS_SUCCESS;

	QH_LOCK_ACQUIRE(&Core->TreeLock);
	status = QHPreviewTreeMergeFrom(&Core->PreviewTree, &Core->StagingTree);
	if (NT_SUCCESS(status))
		status = QHPreviewTreeDedupEarliest(&Core->PreviewTree);
	Core->Building = 0;
	QH_LOCK_RELEASE(&Core->TreeLock);
	if (!NT_SUCCESS(status))
	{
		Core->Phase = QH_CORE_PHASE_GENERAL;
		return status;
	}

	return STATUS_SUCCESS;
}

NTSTATUS QhCorePreviewEnd(_Inout_ PQH_CORE Core)
{
	if (!Core || Core->Phase != QH_CORE_PHASE_PREVIEW)
		return STATUS_INVALID_DEVICE_STATE;
	QHPreviewTreeFree(&Core->PreviewTree);
	QHPreviewTreeFree(&Core->StagingTree);
	QHPreviewTreeInitialize(&Core->PreviewTree);
	QHPreviewTreeInitialize(&Core->StagingTree);
	Core->Phase = QH_CORE_PHASE_GENERAL;
	return STATUS_SUCCESS;
}

static NTSTATUS QhCoreWritebackHistory(_Inout_ PQH_CORE Core)
{
	PQH_PREVIEW_TREE_NODE* all = NULL;
	ULONG capacity;
	ULONG count = 0;
	ULONG i;
	ULONG writeRuns = 0;
	UINT64 writeBytes = 0;
	NTSTATUS status = STATUS_SUCCESS;
	PQH_PREVIEW_TREE_NODE stack[64];
	LONG sp = 0;
	PQH_PREVIEW_TREE_NODE cur;

	if (!Core->HistoryTree.Root || Core->HistoryTree.NodeCount == 0)
	{
		QH_RECOVERY_DBG("writeback skipped: history tree is empty\n");
		return STATUS_SUCCESS;
	}

	capacity = Core->HistoryTree.NodeCount;
	all = (PQH_PREVIEW_TREE_NODE*)QH_ALLOC0(
		sizeof(PQH_PREVIEW_TREE_NODE) * capacity);
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
		PQH_PREVIEW_TREE_NODE key = all[i];
		ULONG j = i;
		while (j > 0 && all[j - 1]->Sequence > key->Sequence)
		{
			all[j] = all[j - 1];
			--j;
		}
		all[j] = key;
	}

	Core->WritebackActive = 1;
	QH_RECOVERY_DBG("writeback begin nodes=%lu detailLimit=%lu\n",
		count, QH_RECOVERY_DETAIL_LIMIT);
#ifdef QH_USERMODE
	if (g_writebackHook)
		g_writebackHook(Core);
#endif
	for (i = 0; i < count; ++i)
	{
		PQH_PREVIEW_TREE_NODE node = all[i];
		PUCHAR payload;
		PUCHAR mask;
		ULONG idx;
		ULONG runStart;
		ULONG a;

		if (node->Invalid)
			continue;

		payload = (PUCHAR)QH_ALLOC(node->DataLength);
		mask = (PUCHAR)QH_ALLOC(node->DataLength);
		if (!payload || !mask)
		{
			status = STATUS_INSUFFICIENT_RESOURCES;
			QH_FREE(payload);
			QH_FREE(mask);
			break;
		}

		status = QHJournalReadPayload(
			Core->Journal,
			node->FileOffset,
			node->DataLength,
			payload);
		if (!NT_SUCCESS(status))
		{
			QH_FREE(payload);
			QH_FREE(mask);
			break;
		}

		if (i < QH_RECOVERY_DETAIL_LIMIT)
		{
			QH_RECOVERY_DBG(
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
			PQH_PREVIEW_TREE_NODE an = all[a];
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
			status = QhCoreWrite(
				Core,
				node->Start + runStart,
				idx - runStart,
				payload + runStart);
			if (i < QH_RECOVERY_DETAIL_LIMIT)
			{
				QH_RECOVERY_DBG(
					"write node=%lu seq=%lu offset=%llu len=%lu status=0x%08X\n",
					i,
					node->Sequence,
					node->Start + runStart,
					idx - runStart,
					status);
			}
			if (!NT_SUCCESS(status))
			{
				QH_FREE(payload);
				QH_FREE(mask);
				goto done;
			}
			++writeRuns;
			writeBytes += idx - runStart;
		}
		QH_FREE(payload);
		QH_FREE(mask);
	}

done:
	Core->WritebackActive = 0;
	if (count > QH_RECOVERY_DETAIL_LIMIT)
	{
		QH_RECOVERY_DBG("detail suppressed for %lu nodes\n",
			count - QH_RECOVERY_DETAIL_LIMIT);
	}
	QH_RECOVERY_DBG(
		"writeback end status=0x%08X nodes=%lu runs=%lu bytes=%llu\n",
		status,
		count,
		writeRuns,
		writeBytes);
	QH_FREE(all);
	return status;
}

NTSTATUS QhCoreRecoveryBegin(_Inout_ PQH_CORE Core, _In_ UINT64 TargetTime100ns)
{
	NTSTATUS status;

	if (!Core || Core->Phase != QH_CORE_PHASE_GENERAL)
		return STATUS_INVALID_DEVICE_STATE;
	if (!Core->Journal->Mounted)
		return STATUS_DEVICE_NOT_READY;

	Core->Phase = QH_CORE_PHASE_RECOVERY;
	Core->Building = 1;
	Core->TargetTime100ns = TargetTime100ns;
	Core->SnapshotMaxSequence = Core->Journal->NextSequence;
	QH_RECOVERY_DBG(
		"begin target=%llu snapshotMaxSeq=%lu records=%llu range=[%llu,%llu]\n",
		TargetTime100ns,
		Core->SnapshotMaxSequence,
		Core->Journal->TotalRecords,
		Core->Journal->Oldest100ns,
		Core->Journal->Newest100ns);
	QHPreviewTreeFree(&Core->HistoryTree);
	QHPreviewTreeFree(&Core->StagingTree);
	QHPreviewTreeInitialize(&Core->HistoryTree);
	QHPreviewTreeInitialize(&Core->StagingTree);

#ifdef QH_USERMODE
	if (g_recoveryBuildHook)
		g_recoveryBuildHook(Core);
#endif

	status = QHJournalBuildPreviewTree(
		Core->Journal,
		TargetTime100ns,
		Core->SnapshotMaxSequence,
		TRUE,
		&Core->HistoryTree);
	if (!NT_SUCCESS(status) && status != STATUS_NOT_FOUND)
	{
		Core->Building = 0;
		Core->Phase = QH_CORE_PHASE_GENERAL;
		return status;
	}
	if (status == STATUS_NOT_FOUND)
		status = STATUS_SUCCESS;
	QH_RECOVERY_DBG("tree build status=0x%08X nodes=%lu staging=%lu\n",
		status,
		Core->HistoryTree.NodeCount,
		Core->StagingTree.NodeCount);

	QH_LOCK_ACQUIRE(&Core->TreeLock);
	QHPreviewTreePunchByStaging(&Core->HistoryTree, &Core->StagingTree);
	status = QHPreviewTreeDedupEarliest(&Core->HistoryTree);
	Core->Building = 0;
	QH_LOCK_RELEASE(&Core->TreeLock);
	if (!NT_SUCCESS(status))
	{
		Core->Phase = QH_CORE_PHASE_GENERAL;
		return status;
	}

	QH_RECOVERY_DBG(
		"prepared target=%llu nodes=%lu; waiting for commit\n",
		Core->TargetTime100ns,
		Core->HistoryTree.NodeCount);
	return STATUS_SUCCESS;
}

NTSTATUS QhCoreRecoveryCommit(_Inout_ PQH_CORE Core)
{
	NTSTATUS status;

	if (!Core || Core->Phase != QH_CORE_PHASE_RECOVERY || Core->Building)
		return STATUS_INVALID_DEVICE_STATE;

	QH_RECOVERY_DBG(
		"commit begin target=%llu nodes=%lu\n",
		Core->TargetTime100ns,
		Core->HistoryTree.NodeCount);
	status = QhCoreWritebackHistory(Core);
	if (!NT_SUCCESS(status))
	{
		QH_RECOVERY_DBG(
			"commit failed status=0x%08X; remaining in Recovery\n",
			status);
		return status;
	}

	QHPreviewTreeFree(&Core->HistoryTree);
	QHPreviewTreeFree(&Core->StagingTree);
	QHPreviewTreeInitialize(&Core->HistoryTree);
	QHPreviewTreeInitialize(&Core->StagingTree);
	Core->TargetTime100ns = 0;
	Core->SnapshotMaxSequence = 0;
	Core->Phase = QH_CORE_PHASE_GENERAL;
	QH_RECOVERY_DBG("commit complete -> normal\n");
	return STATUS_SUCCESS;
}

NTSTATUS QhCoreRecoveryCancel(_Inout_ PQH_CORE Core)
{
	if (!Core || Core->Phase != QH_CORE_PHASE_RECOVERY || Core->Building)
		return STATUS_INVALID_DEVICE_STATE;

	QH_RECOVERY_DBG(
		"cancel target=%llu nodes=%lu -> normal\n",
		Core->TargetTime100ns,
		Core->HistoryTree.NodeCount);
	QHPreviewTreeFree(&Core->HistoryTree);
	QHPreviewTreeFree(&Core->StagingTree);
	QHPreviewTreeInitialize(&Core->HistoryTree);
	QHPreviewTreeInitialize(&Core->StagingTree);
	Core->TargetTime100ns = 0;
	Core->SnapshotMaxSequence = 0;
	Core->Building = 0;
	Core->WritebackActive = 0;
	Core->Phase = QH_CORE_PHASE_GENERAL;
	return STATUS_SUCCESS;
}
