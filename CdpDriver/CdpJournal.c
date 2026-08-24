#ifdef Cdp_USERMODE
#include "cdp_portable.h"
#include "cdp_store.h"
#include "CdpJournal.h"
#else
#include "CdpEngineDefs.h"
#include "CdpJournal.h"
#endif

#define Cdp_CRC32C_POLY 0x82F63B78UL
#ifndef Cdp_USERMODE
#if DBG
#define Cdp_JOURNAL_DIAG(fmt, ...) \
	Cdp_LOG("[JOURNAL-APPLY] " fmt, ##__VA_ARGS__)
#else
#define Cdp_JOURNAL_DIAG(fmt, ...) ((void)0)
#endif
#else
#define Cdp_JOURNAL_DIAG(fmt, ...) ((void)0)
#endif

static ULONG g_CdpCrc32cTable[256];
static volatile LONG g_CdpCrc32cReady;
#ifndef Cdp_USERMODE
#endif

static NTSTATUS CdpJournalAppendBranchLocked(
	_Inout_ PCdp_JOURNAL Journal,
	_In_ LONG BranchNumber,
	_In_ LONG ParentBranchNumber,
	_In_ UINT64 InheritedRecordSequence);

static NTSTATUS CdpJournalAppendBranchContinuationLocked(
	_Inout_ PCdp_JOURNAL Journal);

static NTSTATUS CdpJournalRebuildRuntimeLocked(
	_Inout_ PCdp_JOURNAL Journal,
	_Out_opt_ PUINT64 ScannedRecords);

static PCdp_BRANCH_INFO_NODE CdpBranchTreeFindBySequence(
	_In_ PCdp_BRANCH_INFO_TREE BranchTree,
	_In_ UINT64 Sequence);

static VOID CdpStallBrief(VOID)
{
#ifdef Cdp_USERMODE
	SwitchToThread();
#else
	KeStallExecutionProcessor(1);
#endif
}

static UINT64 CdpAlignDown64(_In_ UINT64 Value, _In_ ULONG Alignment)
{
	return Value - (Value % Alignment);
}

static UINT64 CdpAlignUp64(_In_ UINT64 Value, _In_ ULONG Alignment)
{
	UINT64 remainder = Value % Alignment;
	return remainder ? Value + (Alignment - remainder) : Value;
}

static VOID CdpInitializeCrc32c(VOID)
{
	ULONG table[256];
	ULONG i;

	if (InterlockedCompareExchange(&g_CdpCrc32cReady, 1, 0) != 0)
	{
		while (InterlockedCompareExchange(&g_CdpCrc32cReady, 0, 0) != 2)
			CdpStallBrief();
		return;
	}

	for (i = 0; i < RTL_NUMBER_OF(table); ++i)
	{
		ULONG crc = i;
		ULONG bit;
		for (bit = 0; bit < 8; ++bit)
			crc = (crc & 1) ? ((crc >> 1) ^ Cdp_CRC32C_POLY) : (crc >> 1);
		table[i] = crc;
	}
	RtlCopyMemory(g_CdpCrc32cTable, table, sizeof(table));
	InterlockedExchange(&g_CdpCrc32cReady, 2);
}

static ULONG CdpCrc32c(
	_In_ ULONG InitialCrc,
	_In_reads_bytes_(Length) const VOID* Buffer,
	_In_ SIZE_T Length)
{
	const UCHAR* bytes = (const UCHAR*)Buffer;
	ULONG crc = InitialCrc ^ 0xFFFFFFFFUL;

	if (InterlockedCompareExchange(&g_CdpCrc32cReady, 0, 0) != 2)
		CdpInitializeCrc32c();

	while (Length--)
		crc = g_CdpCrc32cTable[(crc ^ *bytes++) & 0xFF] ^ (crc >> 8);
	return crc ^ 0xFFFFFFFFUL;
}

static PVOID CdpAllocateAligned(
	_In_ PCdp_JOURNAL Journal,
	_In_ SIZE_T Length,
	_Out_ PVOID* AllocationBase)
{
	SIZE_T alignment = sizeof(PVOID);
	ULONG_PTR address;

	if (Journal->SectorSize > alignment)
		alignment = Journal->SectorSize;
#ifndef Cdp_USERMODE
	if (Journal->TargetDevice)
	{
		SIZE_T deviceAlign =
			(SIZE_T)Journal->TargetDevice->AlignmentRequirement + 1;
		if (deviceAlign > alignment)
			alignment = deviceAlign;
	}
#endif
	*AllocationBase = cdpalloc(Length + alignment - 1);
	if (!*AllocationBase)
		return NULL;
	address = ((ULONG_PTR)*AllocationBase + alignment - 1) & ~(alignment - 1);
	return (PVOID)address;
}

static BOOLEAN CdpJournalBufferMeetsIoAlignment(
	_In_ PCdp_JOURNAL Journal,
	_In_ const VOID* Buffer)
{
	SIZE_T alignment = Journal->SectorSize;

#ifndef Cdp_USERMODE
	if (Journal->TargetDevice)
	{
		SIZE_T deviceAlignment =
			(SIZE_T)Journal->TargetDevice->AlignmentRequirement + 1;
		if (deviceAlignment > alignment)
			alignment = deviceAlignment;
	}
#endif
	return ((ULONG_PTR)Buffer % alignment) == 0;
}

static UINT64 CdpJournalQueryWallClock100ns(_In_ PCdp_JOURNAL Journal)
{
	if (Journal->QueryTime100ns)
		return Journal->QueryTime100ns(Journal->QueryTimeContext);
#ifdef Cdp_USERMODE
	{
		FILETIME utcFt;
		FILETIME localFt;
		ULARGE_INTEGER u;
		GetSystemTimeAsFileTime(&utcFt);
		if (!FileTimeToLocalFileTime(&utcFt, &localFt))
			localFt = utcFt;
		u.LowPart = localFt.dwLowDateTime;
		u.HighPart = localFt.dwHighDateTime;
		return u.QuadPart;
	}
#else
	{
		LARGE_INTEGER systemTime;
		LARGE_INTEGER localTime;
		KeQuerySystemTime(&systemTime);
		ExSystemTimeToLocalTime(&systemTime, &localTime);
		return (UINT64)localTime.QuadPart;
	}
#endif
}

static BOOLEAN CdpJournalHeaderIsBranch(
	_In_ const Cdp_JOURNAL_RECORD_HEADER* Header)
{
	return (Header->Sequence & Cdp_JOURNAL_RECORD_FLAG_BRANCH) != 0;
}

static BOOLEAN CdpJournalHeaderIsDeleted(
	_In_ const Cdp_JOURNAL_RECORD_HEADER* Header)
{
	return (Header->Sequence & Cdp_JOURNAL_RECORD_FLAG_DELETED) != 0;
}

static BOOLEAN CdpJournalBranchHeaderIsContinuation(
	_In_ const Cdp_JOURNAL_BRANCH_RECORD_HEADER* Header)
{
	return Header->Reserved == Cdp_JOURNAL_BRANCH_RECORD_FLAG_CONTINUATION;
}

static BOOLEAN CdpJournalBranchHeaderReservedValid(
	_In_ const Cdp_JOURNAL_BRANCH_RECORD_HEADER* Header)
{
	return Header->Reserved == Cdp_JOURNAL_BRANCH_RECORD_FLAG_FIRST ||
		CdpJournalBranchHeaderIsContinuation(Header);
}

static VOID CdpBranchTreeFree(_Inout_ PCdp_BRANCH_INFO_TREE Tree)
{
	PCdp_BRANCH_INFO_NODE node;
	PCdp_BRANCH_INFO_NODE next;

	if (!Tree)
		return;
	node = Tree->First;
	while (node)
	{
		next = node->Next;
		cdpfree(node);
		node = next;
	}
	RtlZeroMemory(Tree, sizeof(*Tree));
}

static PCdp_BRANCH_INFO_NODE CdpBranchTreeFind(
	_In_ PCdp_BRANCH_INFO_TREE Tree,
	_In_ LONG BranchNumber)
{
	PCdp_BRANCH_INFO_NODE node;

	if (!Tree || BranchNumber <= 0)
		return NULL;
	for (node = Tree->Last; node; node = node->Previous)
	{
		if (node->BranchNumber == BranchNumber)
			return node;
	}
	return NULL;
}

static BOOLEAN CdpBranchTreeLatestPathLimit(
	_In_ PCdp_BRANCH_INFO_TREE Tree,
	_In_ PCdp_BRANCH_INFO_NODE Candidate,
	_Out_opt_ PUINT64 AllowedSequence)
{
	PCdp_BRANCH_INFO_NODE branch;
	UINT64 limit = MAXUINT64;

	if (!Tree || !Tree->Latest || !Candidate)
		return FALSE;
	for (branch = Tree->Latest; branch; branch = branch->Parent)
	{
		if (branch == Candidate)
		{
			if (AllowedSequence)
				*AllowedSequence = limit;
			return TRUE;
		}
		limit = branch->InheritedRecordSequence;
	}
	return FALSE;
}

static BOOLEAN CdpBranchTreeLatestPathHasInheritancePoint(
	_In_ PCdp_BRANCH_INFO_TREE Tree,
	_In_ UINT64 InheritedRecordSequence)
{
	PCdp_BRANCH_INFO_NODE branch;

	if (!Tree || !Tree->Latest || InheritedRecordSequence == 0)
		return FALSE;
	for (branch = Tree->Latest; branch; branch = branch->Parent)
	{
		if (branch->ParentBranchNumber != 0 &&
			branch->InheritedRecordSequence == InheritedRecordSequence)
		{
			return TRUE;
		}
	}
	return FALSE;
}

static VOID CdpBranchTreeMarkPruneSubtree(
	_Inout_opt_ PCdp_BRANCH_INFO_NODE Branch)
{
	PCdp_BRANCH_INFO_NODE child;

	if (!Branch || Branch->PrunePending)
		return;
	Branch->PrunePending = TRUE;
	for (child = Branch->FirstChild; child; child = child->NextSibling)
		CdpBranchTreeMarkPruneSubtree(child);
}

static BOOLEAN CdpBranchTreeSequenceDiscardedByCompaction(
	_In_ PCdp_BRANCH_INFO_TREE Tree,
	_In_ UINT64 Sequence,
	_In_ UINT64 FirstSequence,
	_In_ UINT64 EndSequence,
	_In_ BOOLEAN PruneAncestorSuffix)
{
	PCdp_BRANCH_INFO_NODE owner;
	UINT64 allowedSequence;
	BOOLEAN onLatestPath;

	owner = CdpBranchTreeFindBySequence(Tree, Sequence);
	if (!owner || owner->PrunePending)
		return owner != NULL;
	onLatestPath = CdpBranchTreeLatestPathLimit(
		Tree, owner, &allowedSequence);
	if (Sequence >= FirstSequence && Sequence < EndSequence)
		return !onLatestPath || Sequence > allowedSequence;
	return PruneAncestorSuffix && onLatestPath &&
		Sequence > allowedSequence;
}

static VOID CdpBranchRecordInfoSet(
	_Out_ PCdp_BRANCH_RECORD_INFO Info,
	_In_ UINT64 Sequence,
	_In_ UINT64 WallClock100ns,
	_In_ UINT64 RegionOffset,
	_In_ ULONG HeaderIndex)
{
	Info->Sequence = Sequence;
	Info->WallClock100ns = WallClock100ns;
	Info->HeaderRegionOffset = RegionOffset;
	Info->HeaderIndex = HeaderIndex;
}

static NTSTATUS CdpBranchTreeAttachNode(
	_Inout_ PCdp_BRANCH_INFO_TREE Tree,
	_Inout_ PCdp_BRANCH_INFO_NODE Node)
{
	PCdp_BRANCH_INFO_NODE parent;

	if (!Tree || !Node || Node->BranchNumber <= 0 ||
		CdpBranchTreeFind(Tree, Node->BranchNumber))
	{
		return STATUS_INVALID_PARAMETER;
	}
	parent = CdpBranchTreeFind(Tree, Node->ParentBranchNumber);
	Node->Parent = parent;
	if (parent)
	{
		Node->NextSibling = parent->FirstChild;
		parent->FirstChild = Node;
	}
	else if (!Tree->Root)
	{
		Tree->Root = Node;
	}

	Node->Previous = Tree->Last;
	if (Tree->Last)
		Tree->Last->Next = Node;
	else
		Tree->First = Node;
	Tree->Last = Node;
	Tree->Latest = Node;
	Tree->Count++;
	return STATUS_SUCCESS;
}

static PCdp_BRANCH_INFO_NODE CdpBranchTreeAllocateNode(
	_In_ LONG BranchNumber,
	_In_ LONG ParentBranchNumber,
	_In_ UINT64 InheritedRecordSequence,
	_In_ UINT64 Sequence,
	_In_ UINT64 WallClock100ns,
	_In_ UINT64 RegionOffset,
	_In_ ULONG HeaderIndex,
	_In_ BOOLEAN SyntheticStart)
{
	PCdp_BRANCH_INFO_NODE node =
		(PCdp_BRANCH_INFO_NODE)cdpalloc(sizeof(*node));

	if (!node)
		return NULL;
	RtlZeroMemory(node, sizeof(*node));
	node->BranchNumber = BranchNumber;
	node->ParentBranchNumber = ParentBranchNumber;
	node->InheritedRecordSequence = InheritedRecordSequence;
	node->Latest = TRUE;
	node->SyntheticStart = SyntheticStart;
	CdpBranchRecordInfoSet(
		&node->StartRecord,
		Sequence,
		WallClock100ns,
		RegionOffset,
		HeaderIndex);
	node->EndRecord = node->StartRecord;
	return node;
}

static VOID CdpBranchTreeAdvanceLatest(
	_Inout_ PCdp_BRANCH_INFO_TREE Tree,
	_In_ UINT64 Sequence,
	_In_ UINT64 WallClock100ns,
	_In_ UINT64 RegionOffset,
	_In_ ULONG HeaderIndex)
{
	if (!Tree || !Tree->Latest)
		return;
	CdpBranchRecordInfoSet(
		&Tree->Latest->EndRecord,
		Sequence,
		WallClock100ns,
		RegionOffset,
		HeaderIndex);
}

// Rebase the already-built runtime tree after the oldest region is reclaimed.
// This performs no allocation: branches wholly before the new oldest record
// are freed, a branch crossing the boundary gets a synthetic retained start,
// and ancestry links are reconstructed from the surviving creation list.
static VOID CdpBranchTreePruneBefore(
	_Inout_ PCdp_BRANCH_INFO_TREE Tree,
	_In_ const Cdp_BRANCH_RECORD_INFO* NewOldest)
{
	PCdp_BRANCH_INFO_NODE node;
	PCdp_BRANCH_INFO_NODE next;
	ULONG count;

	if (!Tree || !NewOldest)
		return;
	node = Tree->First;
	count = Tree->Count;
	while (node && node->EndRecord.Sequence < NewOldest->Sequence)
	{
		next = node->Next;
		cdpfree(node);
		node = next;
		if (count)
			count--;
	}
	Tree->First = node;
	Tree->Count = count;
	if (!node)
	{
		RtlZeroMemory(Tree, sizeof(*Tree));
		return;
	}
	node->Previous = NULL;
	if (node->StartRecord.Sequence < NewOldest->Sequence)
	{
		node->StartRecord = *NewOldest;
		node->SyntheticStart = TRUE;
	}

	Tree->Root = NULL;
	Tree->Last = NULL;
	Tree->Latest = NULL;
	for (node = Tree->First; node; node = node->Next)
	{
		PCdp_BRANCH_INFO_NODE parent;
		node->Parent = NULL;
		node->FirstChild = NULL;
		node->NextSibling = NULL;
		node->Latest = FALSE;
		Tree->Last = node;
		parent = CdpBranchTreeFind(Tree, node->ParentBranchNumber);
		if (parent && parent != node)
		{
			node->Parent = parent;
			node->NextSibling = parent->FirstChild;
			parent->FirstChild = node;
		}
		else if (!Tree->Root)
		{
			Tree->Root = node;
		}
	}
	Tree->Latest = Tree->Last;
	Tree->Latest->Latest = TRUE;
}

static VOID CdpBranchTreeRemoveLatest(
	_Inout_ PCdp_BRANCH_INFO_TREE Tree)
{
	PCdp_BRANCH_INFO_NODE node;
	PCdp_BRANCH_INFO_NODE* link;

	if (!Tree || !Tree->Latest)
		return;
	node = Tree->Latest;
	if (node->Parent)
	{
		link = &node->Parent->FirstChild;
		while (*link && *link != node)
			link = &(*link)->NextSibling;
		if (*link == node)
			*link = node->NextSibling;
	}
	if (node->Previous)
		node->Previous->Next = NULL;
	else
		Tree->First = NULL;
	Tree->Last = node->Previous;
	Tree->Latest = Tree->Last;
	if (Tree->Latest)
		Tree->Latest->Latest = TRUE;
	if (Tree->Root == node)
		Tree->Root = Tree->First;
	if (Tree->Count)
		Tree->Count--;
	cdpfree(node);
}

// Caller holds Journal->Lock. A journal has at most one header scan in
// progress, so one aligned region buffer can be reused for its lifetime.
static NTSTATUS CdpJournalGetHeaderScanBufferLocked(
	_Inout_ PCdp_JOURNAL Journal,
	_Outptr_ PUCHAR* Buffer)
{
	if (!Journal || !Buffer)
		return STATUS_INVALID_PARAMETER;
	if (!Journal->HeaderScanBuffer)
	{
		Journal->HeaderScanBuffer = (PUCHAR)CdpAllocateAligned(
			Journal,
			Cdp_JOURNAL_HEADER_REGION_SIZE,
			&Journal->HeaderScanAllocationBase);
		if (!Journal->HeaderScanBuffer)
			return STATUS_INSUFFICIENT_RESOURCES;
	}
	*Buffer = Journal->HeaderScanBuffer;
	return STATUS_SUCCESS;
}

static UINT64 CdpJournalUsableStart(_In_ PCdp_JOURNAL Journal)
{
	return Journal->SectorSize;
}

static UINT64 CdpJournalUsableEnd(_In_ PCdp_JOURNAL Journal)
{
	return Journal->PartitionSize;
}

static NTSTATUS CdpJournalRingDistance(
	_In_ PCdp_JOURNAL Journal,
	_In_ UINT64 Start,
	_In_ UINT64 End,
	_Out_ PUINT64 Distance)
{
	UINT64 usableStart;
	UINT64 usableEnd;

	if (!Journal || !Distance)
		return STATUS_INVALID_PARAMETER;
	usableStart = CdpJournalUsableStart(Journal);
	usableEnd = CdpJournalUsableEnd(Journal);
	if (Start < usableStart || Start > usableEnd ||
		End < usableStart || End > usableEnd)
	{
		return STATUS_DISK_CORRUPT_ERROR;
	}
	if (End >= Start)
		*Distance = End - Start;
	else
		*Distance = (usableEnd - Start) + (End - usableStart);
	return STATUS_SUCCESS;
}

static BOOLEAN CdpJournalHeaderRegionOffsetValid(
	_In_ PCdp_JOURNAL Journal,
	_In_ UINT64 RegionOff)
{
	return RegionOff >= CdpJournalUsableStart(Journal) &&
		RegionOff <= CdpJournalUsableEnd(Journal) -
			Cdp_JOURNAL_HEADER_REGION_SIZE &&
		(RegionOff % Journal->SectorSize) == 0;
}

static BOOLEAN CdpJournalRegionLinkValid(
	_In_ PCdp_JOURNAL Journal,
	_In_ const Cdp_HEADER_REGION_LINK* Link)
{
	return Link && Link->Reserved == 0 && Link->StartSequence != 0 &&
		CdpJournalHeaderRegionOffsetValid(Journal, Link->PrevRegionOff) &&
		CdpJournalHeaderRegionOffsetValid(Journal, Link->NextRegionOff);
}

static NTSTATUS CdpJournalDecodeRecord(
	_In_ const Cdp_HEADER_REGION_LINK* Link,
	_In_ const Cdp_JOURNAL_RECORD_HEADER* Header,
	_Out_ PCdp_JOURNAL_RECORD Record)
{
	ULONG localSequence;
	ULONG recordFlags;

	localSequence = Header ?
		(Header->Sequence & Cdp_JOURNAL_RECORD_INDEX_MASK) : 0;
	recordFlags = Header ?
		(Header->Sequence & Cdp_JOURNAL_RECORD_FLAGS_MASK) : 0;
	if (!Link || !Header || !Record || recordFlags != 0 ||
		Link->StartSequence > MAXUINT64 - localSequence)
	{
		return STATUS_INTEGER_OVERFLOW;
	}
	RtlZeroMemory(Record, sizeof(*Record));
	Record->WallClock100ns = Header->WallClock100ns;
	Record->VolumeOffset = Header->VolumeOffset;
	Record->FileOffset = Header->FileOffset;
	Record->Sequence = Link->StartSequence + localSequence;
	Record->DataLength = Header->DataLength;
	Record->Flags = recordFlags;
	return STATUS_SUCCESS;
}

// Caller holds Journal->Lock. Any write that overlaps the cached header
// sector makes its in-memory copy stale. CdpJournalWriteHeaderAt restores
// validity after its own write-through succeeds.
static VOID CdpJournalInvalidateHeaderWriteCacheRangeLocked(
	_Inout_ PCdp_JOURNAL Journal,
	_In_ UINT64 Offset,
	_In_ ULONG Length)
{
	UINT64 writeEnd;
	UINT64 cacheEnd;

	if (!Journal->HeaderWriteCacheValid || Length == 0)
		return;
	writeEnd = Offset + Length;
	cacheEnd = Journal->HeaderWriteSectorOffset + Journal->SectorSize;
	if (Offset < cacheEnd && Journal->HeaderWriteSectorOffset < writeEnd)
		Journal->HeaderWriteCacheValid = FALSE;
}

static NTSTATUS CdpJournalRawIo(
	_In_ PCdp_JOURNAL Journal,
	_In_ UCHAR MajorFunction,
	_In_ UINT64 Offset,
	_In_ ULONG Length,
	_Inout_updates_bytes_(Length) PVOID Buffer)
{
	NTSTATUS status;

	if (!Buffer || Length == 0 ||
		(Offset % Journal->SectorSize) != 0 ||
		(Length % Journal->SectorSize) != 0 ||
		Offset > Journal->PartitionSize ||
		Length > Journal->PartitionSize - Offset)
	{
		return STATUS_INVALID_PARAMETER;
	}
	if (MajorFunction == IRP_MJ_WRITE)
	{
		CdpJournalInvalidateHeaderWriteCacheRangeLocked(
			Journal, Offset, Length);
	}

	if (Journal->Store)
	{
		if (MajorFunction == IRP_MJ_READ)
			status = Journal->Store->Read(
				Journal->Store, Offset, Length, Buffer);
		else if (MajorFunction == IRP_MJ_WRITE)
			status = Journal->Store->Write(
				Journal->Store,
				Offset,
				Length,
				Buffer);
		else
			status = STATUS_NOT_IMPLEMENTED;
#ifndef Cdp_USERMODE
		if (!NT_SUCCESS(status))
		{
			Cdp_LOG("[JOURNAL-RAW-FAIL] backend=store major=0x%02X status=0x%08X offset=%llu len=%lu\n",
				MajorFunction, status, Offset, Length);
		}
#endif
		return status;
	}

#ifndef Cdp_USERMODE
	// Prefer the volume stack below our own filter.  This keeps offsets volume-
	// relative and avoids blocking on a synchronous \\PhysicalDrive handle.
	// Retain the physical-disk backend only as a compatibility fallback when no
	// lower volume device was supplied.
	if (Journal->RawDiskHandle && !Journal->TargetDevice)
	{
		IO_STATUS_BLOCK iosb;
		LARGE_INTEGER byteOffset;

		if (MajorFunction != IRP_MJ_READ &&
			MajorFunction != IRP_MJ_WRITE)
			return STATUS_NOT_IMPLEMENTED;
		if (Journal->TargetBaseOffset >
			(UINT64)MAXLONGLONG - Offset)
			return STATUS_INTEGER_OVERFLOW;

		byteOffset.QuadPart =
			(LONGLONG)(Journal->TargetBaseOffset + Offset);
		RtlZeroMemory(&iosb, sizeof(iosb));
		Cdp_DBG("[JOURNAL-PHYSICAL] io begin handle=%p "
			"major=0x%02X partitionOffset=%llu diskOffset=%llu len=%lu\n",
			Journal->RawDiskHandle,
			MajorFunction,
			Offset,
			(UINT64)byteOffset.QuadPart,
			Length);
		if (MajorFunction == IRP_MJ_READ)
		{
			status = ZwReadFile(
				(HANDLE)Journal->RawDiskHandle,
				NULL, NULL, NULL, &iosb,
				Buffer, Length, &byteOffset, NULL);
		}
		else
		{
			status = ZwWriteFile(
				(HANDLE)Journal->RawDiskHandle,
				NULL, NULL, NULL, &iosb,
				Buffer, Length, &byteOffset, NULL);
		}
		if (NT_SUCCESS(status))
			status = iosb.Status;
		Cdp_DBG("[JOURNAL-PHYSICAL] io end "
			"status=0x%08X bytes=%Iu\n",
			status,
			iosb.Information);
		if (NT_SUCCESS(status) && iosb.Information != Length)
			return STATUS_UNEXPECTED_IO_ERROR;
		if (!NT_SUCCESS(status))
		{
			Cdp_LOG("[JOURNAL-RAW-FAIL] backend=physical major=0x%02X status=0x%08X partitionOffset=%llu diskOffset=%llu len=%lu\n",
				MajorFunction,
				status,
				Offset,
				(UINT64)byteOffset.QuadPart,
				Length);
		}
		return status;
	}

	{
		KEVENT event;
		IO_STATUS_BLOCK iosb;
		LARGE_INTEGER byteOffset;
		PIRP irp;

		if (!Journal->TargetDevice)
			return STATUS_DEVICE_NOT_READY;
		if (Journal->TargetBaseOffset > MAXUINT64 - Offset ||
			Journal->TargetBaseOffset + Offset > MAXLONGLONG)
			return STATUS_INTEGER_OVERFLOW;

	byteOffset.QuadPart = (LONGLONG)(
		Journal->TargetBaseOffset + Offset);
	KeInitializeEvent(&event, NotificationEvent, FALSE);
	RtlZeroMemory(&iosb, sizeof(iosb));
	irp = IoBuildSynchronousFsdRequest(
		MajorFunction,
		Journal->TargetDevice,
		Buffer,
		Length,
		&byteOffset,
		&event,
		&iosb);
	if (!irp)
		return STATUS_INSUFFICIENT_RESOURCES;
	Cdp_DBG("[JOURNAL-RAW] io begin target=%p major=0x%02X "
		"partitionOffset=%llu diskOffset=%llu len=%lu irp=%p\n",
		Journal->TargetDevice,
		MajorFunction,
		Offset,
		(UINT64)byteOffset.QuadPart,
		Length,
		irp);
	status = IoCallDriver(Journal->TargetDevice, irp);
	Cdp_DBG("[JOURNAL-RAW] IoCallDriver returned irp=%p "
		"status=0x%08X iosb=0x%08X bytes=%Iu\n",
		irp,
		status,
		iosb.Status,
		iosb.Information);
	if (status == STATUS_PENDING)
	{
		Cdp_DBG("[JOURNAL-RAW] wait begin irp=%p\n",
			irp);
		KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, NULL);
		status = iosb.Status;
		Cdp_DBG("[JOURNAL-RAW] wait end irp=%p "
			"status=0x%08X bytes=%Iu\n",
			irp,
			status,
			iosb.Information);
	}
	else if (NT_SUCCESS(status))
	{
		status = iosb.Status;
	}
	if (NT_SUCCESS(status) && iosb.Information != Length)
		return STATUS_UNEXPECTED_IO_ERROR;
	if (!NT_SUCCESS(status))
	{
		Cdp_LOG("[JOURNAL-RAW-FAIL] backend=device major=0x%02X status=0x%08X partitionOffset=%llu diskOffset=%llu len=%lu target=%p\n",
			MajorFunction,
			status,
			Offset,
			(UINT64)byteOffset.QuadPart,
			Length,
			Journal->TargetDevice);
	}
	Cdp_DBG("[JOURNAL-RAW] io end irp=%p status=0x%08X "
		"bytes=%Iu\n",
		irp,
		status,
		iosb.Information);
		return status;
	}
#else
	UNREFERENCED_PARAMETER(status);
	return STATUS_DEVICE_NOT_READY;
#endif
}

// Metadata can use the adjacent partition's volume stack while payload I/O
// continues to use the disk stack and absolute offsets. This matters during
// boot: the disk lower can already serve reads yet still reject a raw write
// issued while a partition START_DEVICE is being completed.
static NTSTATUS CdpJournalMetadataRawIo(
	_In_ PCdp_JOURNAL Journal,
	_In_ UCHAR MajorFunction,
	_In_ UINT64 Offset,
	_In_ ULONG Length,
	_Inout_updates_bytes_(Length) PVOID Buffer)
{
	NTSTATUS status;

	if (!Journal->MetadataTargetDevice || Journal->Store)
		return CdpJournalRawIo(
			Journal, MajorFunction, Offset, Length, Buffer);
	if (!Buffer || Length == 0 ||
		(Offset % Journal->SectorSize) != 0 ||
		(Length % Journal->SectorSize) != 0 ||
		Offset > Journal->PartitionSize ||
		Length > Journal->PartitionSize - Offset ||
		Journal->MetadataTargetBaseOffset > MAXUINT64 - Offset ||
		Journal->MetadataTargetBaseOffset + Offset > MAXLONGLONG)
	{
		return STATUS_INVALID_PARAMETER;
	}
	if (MajorFunction == IRP_MJ_WRITE)
	{
		CdpJournalInvalidateHeaderWriteCacheRangeLocked(
			Journal, Offset, Length);
	}

#ifndef Cdp_USERMODE
	{
		KEVENT event;
		IO_STATUS_BLOCK iosb;
		LARGE_INTEGER byteOffset;
		PIRP irp;

		byteOffset.QuadPart = (LONGLONG)(
			Journal->MetadataTargetBaseOffset + Offset);
		KeInitializeEvent(&event, NotificationEvent, FALSE);
		RtlZeroMemory(&iosb, sizeof(iosb));
		irp = IoBuildSynchronousFsdRequest(
			MajorFunction,
			Journal->MetadataTargetDevice,
			Buffer,
			Length,
			&byteOffset,
			&event,
			&iosb);
		if (!irp)
			return STATUS_INSUFFICIENT_RESOURCES;
		status = IoCallDriver(Journal->MetadataTargetDevice, irp);
		if (status == STATUS_PENDING)
		{
			KeWaitForSingleObject(
				&event, Executive, KernelMode, FALSE, NULL);
			status = iosb.Status;
		}
		else if (NT_SUCCESS(status))
		{
			status = iosb.Status;
		}
		if (NT_SUCCESS(status) && iosb.Information != Length)
			status = STATUS_UNEXPECTED_IO_ERROR;
		if (!NT_SUCCESS(status))
		{
			Cdp_LOG("[JOURNAL-METADATA-FAIL] major=0x%02X status=0x%08X offset=%llu deviceOffset=%llu len=%lu target=%p\n",
				MajorFunction,
				status,
				Offset,
				(UINT64)byteOffset.QuadPart,
				Length,
				Journal->MetadataTargetDevice);
		}
		return status;
	}
#else
	UNREFERENCED_PARAMETER(status);
	return STATUS_DEVICE_NOT_READY;
#endif
}

// Caller holds Journal->Lock.  Cache the active record-header sector so we do
// not reread it for every 32-byte header.  Each update is nevertheless copied
// to an independent buffer and submitted immediately: a partial sector must
// never remain memory-only until it happens to fill.
static NTSTATUS CdpJournalRawWriteSub(
	_In_ PCdp_JOURNAL Journal,
	_In_ UINT64 Offset,
	_In_ ULONG Length,
	_In_reads_bytes_(Length) const VOID* Data)
{
	ULONG sec = Journal->SectorSize;
	UINT64 start = (Offset / sec) * sec;
	UINT64 endB = CdpAlignUp64(Offset + Length, sec);
	ULONG span = (ULONG)(endB - start);
	PVOID allocationBase = NULL;
	PUCHAR buf;
	NTSTATUS status;

	if ((Offset % sec) == 0 && (Length % sec) == 0)
		return CdpJournalMetadataRawIo(
			Journal,
			IRP_MJ_WRITE,
			Offset,
			Length,
			(PVOID)Data);

	buf = (PUCHAR)CdpAllocateAligned(Journal, span, &allocationBase);
	if (!buf)
		return STATUS_INSUFFICIENT_RESOURCES;

	status = CdpJournalMetadataRawIo(
		Journal, IRP_MJ_READ, start, span, buf);
	if (NT_SUCCESS(status))
	{
		RtlCopyMemory(buf + (Offset - start), Data, Length);
		status = CdpJournalMetadataRawIo(
			Journal, IRP_MJ_WRITE, start, span, buf);
	}
	cdpfree(allocationBase);
	return status;
}

static NTSTATUS CdpJournalFlush(_In_ PCdp_JOURNAL Journal)
{
	if (Journal->Store)
		return STATUS_SUCCESS;

#ifndef Cdp_USERMODE
{
	KEVENT event;
	IO_STATUS_BLOCK iosb;
	PIRP irp;
	PDEVICE_OBJECT flushDevice = Journal->MetadataTargetDevice ?
		Journal->MetadataTargetDevice : Journal->TargetDevice;
	NTSTATUS status;

	if (!flushDevice)
		return STATUS_DEVICE_NOT_READY;
	KeInitializeEvent(&event, NotificationEvent, FALSE);
	RtlZeroMemory(&iosb, sizeof(iosb));
	irp = IoBuildSynchronousFsdRequest(
		IRP_MJ_FLUSH_BUFFERS,
		flushDevice,
		NULL,
		0,
		NULL,
		&event,
		&iosb);
	if (!irp)
		return STATUS_INSUFFICIENT_RESOURCES;
	status = IoCallDriver(flushDevice, irp);
	if (status == STATUS_PENDING)
	{
		KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, NULL);
		status = iosb.Status;
	}
	else if (NT_SUCCESS(status))
	{
		status = iosb.Status;
	}
	return status;
	}
#else
	UNREFERENCED_PARAMETER(Journal);
	return STATUS_SUCCESS;
#endif
}

NTSTATUS CdpJournalFlushBuffers(_Inout_ PCdp_JOURNAL Journal)
{
	NTSTATUS status;

	if (!Journal)
		return STATUS_INVALID_PARAMETER;
	Cdp_LOCK_ACQUIRE(&Journal->Lock);
	status = Journal->Mounted ?
		CdpJournalFlush(Journal) : STATUS_DEVICE_NOT_READY;
	Cdp_LOCK_RELEASE(&Journal->Lock);
	return status;
}

static BOOLEAN CdpJournalIsEmptyLocked(_In_ PCdp_JOURNAL Journal)
{
	return Journal->TotalRecords == 0;
}

static VOID CdpJournalAdvanceRecordGenerationLocked(
	_Inout_ PCdp_JOURNAL Journal)
{
	Journal->RecordGeneration++;
	if (Journal->RecordGeneration == 0)
		Journal->RecordGeneration = 1;
}

static NTSTATUS CdpJournalWriteSuperblockLocked(_Inout_ PCdp_JOURNAL Journal)
{
	PVOID allocationBase = NULL;
	PUCHAR sector;
	PCdp_JOURNAL_SUPERBLOCK superblock;
	NTSTATUS status;

	sector = (PUCHAR)CdpAllocateAligned(Journal,
		Journal->SectorSize,
		&allocationBase);
	if (!sector)
		return STATUS_INSUFFICIENT_RESOURCES;

	RtlZeroMemory(sector, Journal->SectorSize);
	superblock = (PCdp_JOURNAL_SUPERBLOCK)sector;
	superblock->Magic = Cdp_JOURNAL_MAGIC;
	superblock->Version = Cdp_JOURNAL_VERSION;
	superblock->SectorSize = Journal->SectorSize;
	superblock->Flags = Journal->RecoveryPending ?
		Cdp_JOURNAL_FLAG_RECOVERY_PENDING : 0;
	if (Journal->RecoveryFsRepairPending)
		superblock->Flags |= Cdp_JOURNAL_FLAG_RECOVERY_FS_REPAIR_PENDING;
	if (Journal->CredentialConfigured)
		superblock->Flags |= Cdp_JOURNAL_FLAG_CREDENTIAL_CONFIGURED;
	if (Journal->RestorePointSet)
		superblock->Flags |= Cdp_JOURNAL_FLAG_RESTORE_POINT_SET;
	superblock->PartitionSize = Journal->PartitionSize;
	superblock->LastHeaderRegionOff = Journal->LastHeaderRegionOff;
	superblock->SourceVolumeGuid = Journal->SourceVolumeGuid;
	superblock->RecoveryTargetTime100ns = Journal->RecoveryTargetTime100ns;
	if (Journal->CredentialConfigured)
		superblock->Credential = Journal->Credential;
	superblock->CurrentBranchNumber = Journal->CurrentBranchNumber;
	superblock->HighestBranchNumber = Journal->HighestBranchNumber;
	superblock->DiskPartitionStyle = Journal->DiskPartitionStyle;
	superblock->MbrSignature = Journal->MbrSignature;
	superblock->DiskGuid = Journal->DiskGuid;
	superblock->SourcePartitionStart = Journal->SourcePartitionStart;
	superblock->SourcePartitionSize = Journal->SourcePartitionSize;
	superblock->JournalPartitionStart = Journal->JournalPartitionStart;
	superblock->JournalPartitionSize = Journal->JournalPartitionSize;
	superblock->RestorePointTime100ns = Journal->RestorePointTime100ns;
	superblock->Crc32c = CdpCrc32c(
		0,
		superblock,
		FIELD_OFFSET(Cdp_JOURNAL_SUPERBLOCK, Crc32c));
	superblock->RecoveryCrc32c = CdpCrc32c(
		0,
		superblock,
		FIELD_OFFSET(Cdp_JOURNAL_SUPERBLOCK, RecoveryCrc32c));
	superblock->MetadataCrc32c = CdpCrc32c(
		0,
		superblock,
		FIELD_OFFSET(Cdp_JOURNAL_SUPERBLOCK, MetadataCrc32c));
	superblock->RestorePointCrc32c = CdpCrc32c(
		0,
		superblock,
		FIELD_OFFSET(Cdp_JOURNAL_SUPERBLOCK, RestorePointCrc32c));

	status = CdpJournalMetadataRawIo(
		Journal,
		IRP_MJ_WRITE,
		0,
		Journal->SectorSize,
		sector);
	cdpfree(allocationBase);
	if (NT_SUCCESS(status))
		Journal->SuperblockDirty = FALSE;
	return status;
}

static BOOLEAN CdpJournalSuperblockValid(
	_In_ PCdp_JOURNAL Journal,
	_In_ const Cdp_JOURNAL_SUPERBLOCK* Superblock)
{
	ULONG crc;
	UINT64 usableStart;
	UINT64 usableEnd;

	if (Superblock->Magic != Cdp_JOURNAL_MAGIC ||
		(Superblock->Version != Cdp_JOURNAL_VERSION &&
		 Superblock->Version != Cdp_JOURNAL_VERSION_PREVIOUS) ||
		Superblock->SectorSize != Journal->SectorSize ||
		Superblock->PartitionSize != Journal->PartitionSize)
	{
		return FALSE;
	}
	crc = CdpCrc32c(
		0,
		Superblock,
		FIELD_OFFSET(Cdp_JOURNAL_SUPERBLOCK, Crc32c));
	if (crc != Superblock->Crc32c)
		return FALSE;
	if ((Superblock->Flags & Cdp_JOURNAL_FLAG_RECOVERY_PENDING) != 0 &&
		(Superblock->RecoveryTargetTime100ns == 0 ||
		 CdpCrc32c(0, Superblock,
			FIELD_OFFSET(Cdp_JOURNAL_SUPERBLOCK, RecoveryCrc32c)) !=
			Superblock->RecoveryCrc32c))
	{
		return FALSE;
	}
	if (CdpCrc32c(0, Superblock,
		FIELD_OFFSET(Cdp_JOURNAL_SUPERBLOCK, MetadataCrc32c)) !=
		Superblock->MetadataCrc32c)
	{
		return FALSE;
	}
	if (Superblock->Version >= Cdp_JOURNAL_VERSION &&
		(Superblock->Flags & Cdp_JOURNAL_FLAG_RESTORE_POINT_SET) != 0 &&
		(Superblock->RestorePointTime100ns == 0 ||
		 CdpCrc32c(0, Superblock,
			FIELD_OFFSET(Cdp_JOURNAL_SUPERBLOCK, RestorePointCrc32c)) !=
			Superblock->RestorePointCrc32c))
	{
		return FALSE;
	}
	if ((Superblock->Flags & Cdp_JOURNAL_FLAG_CREDENTIAL_CONFIGURED) != 0 &&
		(Superblock->Credential.KdfAlgorithm != Cdp_CREDENTIAL_KDF_PBKDF2_SHA256 ||
		 Superblock->Credential.KdfIterations == 0 ||
		 Superblock->Credential.AuthEpoch == 0))
	{
		return FALSE;
	}
	if (Superblock->CurrentBranchNumber <= 0 ||
		Superblock->HighestBranchNumber < Superblock->CurrentBranchNumber)
	{
		return FALSE;
	}

	usableStart = Journal->SectorSize;
	usableEnd = Journal->PartitionSize;
	if (Superblock->LastHeaderRegionOff < usableStart ||
		Superblock->LastHeaderRegionOff + Cdp_JOURNAL_HEADER_REGION_SIZE > usableEnd ||
		(Superblock->LastHeaderRegionOff % Journal->SectorSize) != 0)
	{
		return FALSE;
	}
	return TRUE;
}

static NTSTATUS CdpJournalReadRegionLink(
	_In_ PCdp_JOURNAL Journal,
	_In_ UINT64 RegionOff,
	_Out_ PCdp_HEADER_REGION_LINK Link)
{
	PVOID allocationBase = NULL;
	PUCHAR sector;
	NTSTATUS status;

	if (!Journal || !Link || Journal->SectorSize < sizeof(*Link))
		return STATUS_INVALID_PARAMETER;
	sector = (PUCHAR)CdpAllocateAligned(
		Journal,
		Journal->SectorSize,
		&allocationBase);
	if (!sector)
		return STATUS_INSUFFICIENT_RESOURCES;

	status = CdpJournalMetadataRawIo(
		Journal,
		IRP_MJ_READ,
		RegionOff + Cdp_JOURNAL_HEADER_REGION_SIZE - Journal->SectorSize,
		Journal->SectorSize,
		sector);
	if (NT_SUCCESS(status))
	{
		RtlCopyMemory(
			Link,
			sector + Journal->SectorSize - sizeof(*Link),
			sizeof(*Link));
	}
	cdpfree(allocationBase);
	return status;
}

static NTSTATUS CdpJournalWriteRegionLink(
	_In_ PCdp_JOURNAL Journal,
	_In_ UINT64 RegionOff,
	_In_ const Cdp_HEADER_REGION_LINK* Link)
{
	return CdpJournalRawWriteSub(
		Journal,
		RegionOff + Cdp_JOURNAL_HEADER_REGION_SIZE - Cdp_JOURNAL_HEADER_LINK_SIZE,
		sizeof(*Link),
		Link);
}

static NTSTATUS CdpJournalInitHeaderRegion(
	_In_ PCdp_JOURNAL Journal,
	_In_ UINT64 RegionOff,
	_In_ UINT64 PrevOff,
	_In_ UINT64 NextOff,
	_In_ UINT64 StartSequence)
{
	PVOID allocationBase = NULL;
	PUCHAR region;
	PCdp_HEADER_REGION_LINK link;
	NTSTATUS status;
	ULONG offset;
	ULONG chunk;

	region = (PUCHAR)CdpAllocateAligned(Journal,
		Cdp_JOURNAL_HEADER_REGION_SIZE,
		&allocationBase);
	if (!region)
		return STATUS_INSUFFICIENT_RESOURCES;

	RtlZeroMemory(region, Cdp_JOURNAL_HEADER_REGION_SIZE);
	link = (PCdp_HEADER_REGION_LINK)(
		region + Cdp_JOURNAL_HEADER_REGION_SIZE - Cdp_JOURNAL_HEADER_LINK_SIZE);
	link->PrevRegionOff = PrevOff;
	link->NextRegionOff = NextOff;
	link->StartSequence = StartSequence;
	link->Reserved = 0;

	chunk = Journal->HeaderRegionWriteChunk;
	if (chunk == 0 || chunk > Cdp_JOURNAL_HEADER_REGION_SIZE)
		chunk = Cdp_JOURNAL_HEADER_REGION_SIZE;
	chunk = (chunk / Journal->SectorSize) * Journal->SectorSize;
	if (chunk < Journal->SectorSize)
		chunk = Journal->SectorSize;

	for (offset = 0; offset < Cdp_JOURNAL_HEADER_REGION_SIZE;)
	{
		ULONG remaining = Cdp_JOURNAL_HEADER_REGION_SIZE - offset;
		ULONG transfer = chunk < remaining ? chunk : remaining;

		status = CdpJournalMetadataRawIo(
			Journal,
			IRP_MJ_WRITE,
			RegionOff + offset,
			transfer,
			region + offset);
		if (!NT_SUCCESS(status))
		{
			if (chunk <= Journal->SectorSize)
			{
				cdpfree(allocationBase);
				return status;
			}

			chunk /= 2;
			chunk = (chunk / Journal->SectorSize) * Journal->SectorSize;
			if (chunk < Journal->SectorSize)
				chunk = Journal->SectorSize;
			continue;
		}

		// Cache the largest transfer that succeeded.  Later header regions
		// start directly with this size instead of probing from 1MB again.
		Journal->HeaderRegionWriteChunk = chunk;
		offset += transfer;
	}
#ifndef Cdp_USERMODE
	Cdp_DBG("[JOURNAL] HeaderRegionWriteChunk=%lu bytes\n",
		Journal->HeaderRegionWriteChunk);
#endif
	cdpfree(allocationBase);
	return STATUS_SUCCESS;
}

// A non-current region may be only partially filled when its payload span
// reaches the rotation threshold. Its exact record count is persisted
// implicitly by the next region's StartSequence, so only that region's link
// sector is needed; the unused header slots never need to be scanned.
static NTSTATUS CdpJournalGetRegionHeaderLimitLocked(
	_In_ PCdp_JOURNAL Journal,
	_In_ UINT64 RegionOff,
	_In_ const Cdp_HEADER_REGION_LINK* Link,
	_Out_ PULONG Limit,
	_Out_opt_ PCdp_HEADER_REGION_LINK NextLink)
{
	Cdp_HEADER_REGION_LINK next;
	UINT64 count;
	NTSTATUS status;

	if (!Journal || !Link || !Limit)
		return STATUS_INVALID_PARAMETER;
	if (RegionOff == Journal->LastHeaderRegionOff)
	{
		if (Journal->CurrentHeaderCount > Cdp_JOURNAL_HEADERS_PER_REGION)
			return STATUS_DISK_CORRUPT_ERROR;
		*Limit = Journal->CurrentHeaderCount;
		if (NextLink)
			RtlZeroMemory(NextLink, sizeof(*NextLink));
		return STATUS_SUCCESS;
	}
	if (Link->NextRegionOff == RegionOff)
		return STATUS_DISK_CORRUPT_ERROR;

	status = CdpJournalReadRegionLink(Journal, Link->NextRegionOff, &next);
	if (!NT_SUCCESS(status))
		return status;
	if (!CdpJournalRegionLinkValid(Journal, &next) ||
		next.PrevRegionOff != RegionOff ||
		next.StartSequence <= Link->StartSequence)
	{
		return STATUS_DISK_CORRUPT_ERROR;
	}
	count = next.StartSequence - Link->StartSequence;
	if (count == 0 || count > Cdp_JOURNAL_HEADERS_PER_REGION)
		return STATUS_DISK_CORRUPT_ERROR;

	*Limit = (ULONG)count;
	if (NextLink)
		*NextLink = next;
	return STATUS_SUCCESS;
}

static NTSTATUS CdpJournalReadHeaderAt(
	_In_ PCdp_JOURNAL Journal,
	_In_ UINT64 RegionOff,
	_In_ ULONG Index,
	_Out_ PCdp_JOURNAL_RECORD_HEADER Header)
{
	PVOID allocationBase = NULL;
	PUCHAR sector;
	UINT64 relativeOffset;
	UINT64 sectorOffset;
	NTSTATUS status;

	if (Index >= Cdp_JOURNAL_HEADERS_PER_REGION)
		return STATUS_INVALID_PARAMETER;
	relativeOffset = (UINT64)Index * sizeof(*Header);
	sectorOffset = (relativeOffset / Journal->SectorSize) * Journal->SectorSize;
	sector = (PUCHAR)CdpAllocateAligned(
		Journal,
		Journal->SectorSize,
		&allocationBase);
	if (!sector)
		return STATUS_INSUFFICIENT_RESOURCES;
	status = CdpJournalMetadataRawIo(
		Journal,
		IRP_MJ_READ,
		RegionOff + sectorOffset,
		Journal->SectorSize,
		sector);
	if (NT_SUCCESS(status))
	{
		RtlCopyMemory(
			Header,
			sector + (relativeOffset - sectorOffset),
			sizeof(*Header));
	}
	cdpfree(allocationBase);
	return status;
}

static NTSTATUS CdpJournalReadHeaderRegion(
	_Inout_ PCdp_JOURNAL Journal,
	_In_ UINT64 RegionOff,
	_Out_writes_bytes_(Cdp_JOURNAL_HEADER_REGION_SIZE) PUCHAR Region)
{
	NTSTATUS status;
	ULONG offset = 0;
	ULONG chunk = Journal->HeaderRegionWriteChunk;

	if (chunk == 0 || chunk > Cdp_JOURNAL_HEADER_REGION_SIZE)
		chunk = Cdp_JOURNAL_HEADER_REGION_SIZE;
	chunk = (chunk / Journal->SectorSize) * Journal->SectorSize;
	if (chunk < Journal->SectorSize)
		chunk = Journal->SectorSize;

	while (offset < Cdp_JOURNAL_HEADER_REGION_SIZE)
	{
		ULONG remaining = Cdp_JOURNAL_HEADER_REGION_SIZE - offset;
		ULONG transfer = chunk < remaining ? chunk : remaining;

		status = CdpJournalMetadataRawIo(
			Journal,
			IRP_MJ_READ,
			RegionOff + offset,
			transfer,
			Region + offset);
		if (!NT_SUCCESS(status))
		{
			if (chunk <= Journal->SectorSize)
				return status;
			chunk /= 2;
			chunk = (chunk / Journal->SectorSize) * Journal->SectorSize;
			if (chunk < Journal->SectorSize)
				chunk = Journal->SectorSize;
			continue;
		}

		Journal->HeaderRegionWriteChunk = chunk;
		offset += transfer;
	}

	return STATUS_SUCCESS;
}

static NTSTATUS CdpJournalWriteHeaderAt(
	_In_ PCdp_JOURNAL Journal,
	_In_ UINT64 RegionOff,
	_In_ ULONG Index,
	_In_ const Cdp_JOURNAL_RECORD_HEADER* Header)
{
	UINT64 headerOffset;
	UINT64 sectorOffset;
	ULONG sectorByteOffset;
	NTSTATUS status;

	if (Index >= Cdp_JOURNAL_HEADERS_PER_REGION)
		return STATUS_INVALID_PARAMETER;

	headerOffset = RegionOff +
		(UINT64)Index * sizeof(Cdp_JOURNAL_RECORD_HEADER);
	sectorOffset = CdpAlignDown64(headerOffset, Journal->SectorSize);
	sectorByteOffset = (ULONG)(headerOffset - sectorOffset);
	if (sectorByteOffset + sizeof(*Header) > Journal->SectorSize)
		return STATUS_DISK_CORRUPT_ERROR;

	if (!Journal->HeaderWriteBuffer)
	{
		Journal->HeaderWriteBuffer = (PUCHAR)CdpAllocateAligned(
			Journal,
			Journal->SectorSize,
			&Journal->HeaderWriteAllocationBase);
		if (!Journal->HeaderWriteBuffer)
			return STATUS_INSUFFICIENT_RESOURCES;
	}

	if (!Journal->HeaderWriteCacheValid ||
		Journal->HeaderWriteSectorOffset != sectorOffset)
	{
		Journal->HeaderWriteCacheValid = FALSE;
		status = CdpJournalMetadataRawIo(
			Journal,
			IRP_MJ_READ,
			sectorOffset,
			Journal->SectorSize,
			Journal->HeaderWriteBuffer);
		if (!NT_SUCCESS(status))
			return status;
		Journal->HeaderWriteSectorOffset = sectorOffset;
		Journal->HeaderWriteCacheValid = TRUE;
	}

	RtlCopyMemory(
		Journal->HeaderWriteBuffer + sectorByteOffset,
		Header,
		sizeof(*Header));
	status = CdpJournalMetadataRawIo(
		Journal,
		IRP_MJ_WRITE,
		sectorOffset,
		Journal->SectorSize,
		Journal->HeaderWriteBuffer);
	if (NT_SUCCESS(status))
	{
		Journal->HeaderWriteSectorOffset = sectorOffset;
		Journal->HeaderWriteCacheValid = TRUE;
	}
	return status;
}

static NTSTATUS CdpJournalRefreshOldestTimeLocked(_Inout_ PCdp_JOURNAL Journal)
{
	UINT64 regionOff;
	ULONG startIndex;
	ULONG guard = 0;
	NTSTATUS status = STATUS_SUCCESS;

	if (CdpJournalIsEmptyLocked(Journal))
	{
		Journal->Oldest100ns = 0;
		Journal->Newest100ns = 0;
		return STATUS_SUCCESS;
	}

	regionOff = Journal->OldestHeaderRegionOff;
	startIndex = Journal->OldestHeaderIndex;
	for (;;)
	{
		Cdp_HEADER_REGION_LINK link;
		ULONG limit;
		ULONG index;
		BOOLEAN isLast = regionOff == Journal->LastHeaderRegionOff;

		status = CdpJournalReadRegionLink(Journal, regionOff, &link);
		if (!NT_SUCCESS(status) || !CdpJournalRegionLinkValid(Journal, &link))
			return STATUS_DISK_CORRUPT_ERROR;
		status = CdpJournalGetRegionHeaderLimitLocked(
			Journal, regionOff, &link, &limit, NULL);
		if (!NT_SUCCESS(status))
			return status;
		for (index = startIndex; index < limit; ++index)
		{
			Cdp_JOURNAL_RECORD_HEADER header;
			status = CdpJournalReadHeaderAt(
				Journal, regionOff, index, &header);
			if (!NT_SUCCESS(status))
				return status;
			if (!CdpJournalHeaderIsDeleted(&header))
			{
				Journal->Oldest100ns = header.WallClock100ns;
				return STATUS_SUCCESS;
			}
		}
		if (isLast || link.NextRegionOff == regionOff || ++guard > 100000UL)
			break;
		regionOff = link.NextRegionOff;
		startIndex = 0;
	}
	return STATUS_DISK_CORRUPT_ERROR;
}

// Contiguous free bytes from PayloadRegionOff without wrapping and without
// entering the oldest live header region.
static UINT64 CdpJournalContiguousFreeLocked(_In_ PCdp_JOURNAL Journal)
{
	UINT64 usableEnd = CdpJournalUsableEnd(Journal);
	UINT64 head = Journal->PayloadRegionOff;
	UINT64 tail = Journal->OldestHeaderRegionOff;

	if (CdpJournalIsEmptyLocked(Journal))
		return usableEnd - head;

	// Write cursor caught up with oldest header: no contiguous free in front.
	if (head == tail)
		return 0;

	if (head < tail)
		return tail - head;

	// head > tail: free until partition end; wrap is handled separately.
	return usableEnd - head;
}

static NTSTATUS CdpJournalDropOldestRegionLocked(
	_Inout_ PCdp_JOURNAL Journal)
{
	Cdp_HEADER_REGION_LINK link;
	Cdp_HEADER_REGION_LINK nextLink;
	Cdp_JOURNAL_RECORD_HEADER newOldestHeader;
	Cdp_BRANCH_RECORD_INFO newOldestInfo;
	UINT64 regionOff;
	UINT64 reclaimedBytes = 0;
	UINT64 reclaimedRecords = 0;
	ULONG limit;
	ULONG index;
	NTSTATUS status;

	if (CdpJournalIsEmptyLocked(Journal))
		return STATUS_NOT_FOUND;

	regionOff = Journal->OldestHeaderRegionOff;
	status = CdpJournalReadRegionLink(Journal, regionOff, &link);
	if (!NT_SUCCESS(status) || !CdpJournalRegionLinkValid(Journal, &link))
		return STATUS_DISK_CORRUPT_ERROR;
	status = CdpJournalGetRegionHeaderLimitLocked(
		Journal,
		regionOff,
		&link,
		&limit,
		&nextLink);
	if (!NT_SUCCESS(status))
		return status;
	if (Journal->OldestHeaderIndex >= limit)
		return STATUS_DISK_CORRUPT_ERROR;
	for (index = Journal->OldestHeaderIndex; index < limit; ++index)
	{
		Cdp_JOURNAL_RECORD_HEADER header;
		status = CdpJournalReadHeaderAt(Journal, regionOff, index, &header);
		if (!NT_SUCCESS(status))
			return status;
		if ((header.Sequence & Cdp_JOURNAL_RECORD_INDEX_MASK) != index)
			return STATUS_DISK_CORRUPT_ERROR;
		if (!CdpJournalHeaderIsDeleted(&header))
			reclaimedRecords++;
	}

	if (regionOff == Journal->LastHeaderRegionOff)
	{
		reclaimedBytes = Journal->PayloadBytesUsed;
		if (reclaimedRecords != Journal->TotalRecords)
			return STATUS_DISK_CORRUPT_ERROR;
	}
	else
	{
		UINT64 firstSequence;

		if (link.NextRegionOff == regionOff ||
			link.StartSequence > MAXUINT64 - Journal->OldestHeaderIndex)
		{
			return STATUS_DISK_CORRUPT_ERROR;
		}
		firstSequence = link.StartSequence + Journal->OldestHeaderIndex;
		if (nextLink.StartSequence <= firstSequence)
			return STATUS_DISK_CORRUPT_ERROR;
		if (nextLink.StartSequence - firstSequence !=
			limit - Journal->OldestHeaderIndex)
			return STATUS_DISK_CORRUPT_ERROR;
		status = CdpJournalRingDistance(
			Journal,
			regionOff + Cdp_JOURNAL_HEADER_REGION_SIZE,
			link.NextRegionOff,
			&reclaimedBytes);
		if (!NT_SUCCESS(status))
			return status;
	}

	if (reclaimedRecords > Journal->TotalRecords ||
		reclaimedBytes > Journal->PayloadBytesUsed)
		return STATUS_DISK_CORRUPT_ERROR;

	Journal->TotalRecords -= reclaimedRecords;
	Journal->PayloadBytesUsed -= reclaimedBytes;
	CdpJournalAdvanceRecordGenerationLocked(Journal);

	if (regionOff == Journal->LastHeaderRegionOff)
	{
		if (Journal->TotalRecords != 0 || Journal->PayloadBytesUsed != 0)
			return STATUS_DISK_CORRUPT_ERROR;
		// The active region is about to be reused from header[0]. Clear stale
		// headers and start a new local-sequence epoch at the next global id.
		status = CdpJournalInitHeaderRegion(
			Journal,
			Journal->LastHeaderRegionOff,
			Journal->LastHeaderRegionOff,
			Journal->LastHeaderRegionOff,
			Journal->NextSequence);
		if (!NT_SUCCESS(status))
			return status;
		Journal->OldestHeaderRegionOff = Journal->LastHeaderRegionOff;
		Journal->CurrentHeaderRegionStartSequence = Journal->NextSequence;
		Journal->OldestHeaderIndex = 0;
		Journal->CurrentHeaderCount = 0;
		Journal->PayloadRegionOff =
			Journal->LastHeaderRegionOff + Cdp_JOURNAL_HEADER_REGION_SIZE;
		Journal->Oldest100ns = 0;
		Journal->Newest100ns = 0;
		return STATUS_SUCCESS;
	}

	// Persist the logical deletion: Mount walking backwards from the newest
	// region must stop at the new oldest region, not rediscover stale headers.
	status = CdpJournalReadHeaderAt(
		Journal, link.NextRegionOff, 0, &newOldestHeader);
	if (!NT_SUCCESS(status))
		return status;
	CdpBranchRecordInfoSet(
		&newOldestInfo,
		nextLink.StartSequence,
		newOldestHeader.WallClock100ns,
		link.NextRegionOff,
		0);
	nextLink.PrevRegionOff = link.NextRegionOff;
	status = CdpJournalWriteRegionLink(
		Journal,
		link.NextRegionOff,
		&nextLink);
	if (!NT_SUCCESS(status))
		return status;

	Journal->OldestHeaderRegionOff = link.NextRegionOff;
	Journal->OldestHeaderIndex = 0;
	if (Journal->ActiveHeaderRegionCount <= 1)
		return STATUS_DISK_CORRUPT_ERROR;
	Journal->ActiveHeaderRegionCount--;
	CdpBranchTreePruneBefore(&Journal->BranchTree, &newOldestInfo);
	return CdpJournalRefreshOldestTimeLocked(Journal);
}

static NTSTATUS CdpJournalEnsureContiguousLocked(
	_Inout_ PCdp_JOURNAL Journal,
	_In_ UINT64 BytesNeeded)
{
	UINT64 usableStart = CdpJournalUsableStart(Journal);
	UINT64 usableEnd = CdpJournalUsableEnd(Journal);

	if (BytesNeeded > usableEnd - usableStart)
		return STATUS_INSUFFICIENT_RESOURCES;

	for (;;)
	{
		// Payload hits the end: wrap write cursor; do NOT open a new header.
		if (Journal->PayloadRegionOff + BytesNeeded > usableEnd)
		{
			UINT64 skipped = usableEnd - Journal->PayloadRegionOff;
			UINT64 wrapFree = CdpJournalIsEmptyLocked(Journal) ?
				usableEnd - usableStart :
				(Journal->OldestHeaderRegionOff > usableStart ?
					Journal->OldestHeaderRegionOff - usableStart : 0);
			if (wrapFree < BytesNeeded)
				return STATUS_DISK_FULL;
			if (Journal->PayloadBytesUsed > MAXUINT64 - skipped)
				return STATUS_INTEGER_OVERFLOW;
			Journal->PayloadBytesUsed += skipped;
			Journal->PayloadRegionOff = usableStart;
		}

		if (CdpJournalContiguousFreeLocked(Journal) >= BytesNeeded)
			return STATUS_SUCCESS;

		// The merge thread must materialize live values before reclaiming a
		// complete region. Never discard after-image data from the append path.
		return STATUS_DISK_FULL;
	}
}

static NTSTATUS CdpJournalPayloadRotationNeededLocked(
	_In_ PCdp_JOURNAL Journal,
	_In_ UINT64 NextPayloadBytes,
	_Out_ PBOOLEAN RotationNeeded)
{
	UINT64 payloadSpan;
	UINT64 threshold;
	NTSTATUS status;

	if (!Journal || !RotationNeeded || NextPayloadBytes == 0)
		return STATUS_INVALID_PARAMETER;
	*RotationNeeded = FALSE;
	if (Journal->CurrentHeaderCount == 0)
		return STATUS_SUCCESS;

	status = CdpJournalRingDistance(
		Journal,
		Journal->LastHeaderRegionOff + Cdp_JOURNAL_HEADER_REGION_SIZE,
		Journal->PayloadRegionOff,
		&payloadSpan);
	if (!NT_SUCCESS(status))
		return status;

	threshold = Journal->PartitionSize /
		Cdp_JOURNAL_PAYLOAD_REGION_CAPACITY_DIVISOR;
	if (threshold == 0 || payloadSpan >= threshold ||
		NextPayloadBytes > threshold - payloadSpan)
	{
		*RotationNeeded = TRUE;
	}
	return STATUS_SUCCESS;
}

// Place a new 1MB header region at the current payload cursor, then start a
// fresh payload area immediately after it: ...[Pprev][Hnew 1MB][Pnew...]
static NTSTATUS CdpJournalAllocateHeaderRegionLocked(
	_Inout_ PCdp_JOURNAL Journal,
	_Out_ PUINT64 NewRegionOff)
{
	UINT64 usableStart = CdpJournalUsableStart(Journal);
	UINT64 usableEnd = CdpJournalUsableEnd(Journal);
	UINT64 candidate;
	UINT64 oldRegion;
	Cdp_HEADER_REGION_LINK oldLink;
	Cdp_HEADER_REGION_LINK newLink;
	NTSTATUS status;

	candidate = CdpAlignUp64(Journal->PayloadRegionOff, Journal->SectorSize);
	if (candidate + Cdp_JOURNAL_HEADER_REGION_SIZE > usableEnd)
		candidate = usableStart;

	// Reclaim until the candidate 1MB range does not overlap the oldest live unit.
	while (!CdpJournalIsEmptyLocked(Journal))
	{
		UINT64 old = Journal->OldestHeaderRegionOff;
		UINT64 oldEnd = old + Cdp_JOURNAL_HEADER_REGION_SIZE;
		BOOLEAN overlaps =
			!(oldEnd <= candidate ||
				old >= candidate + Cdp_JOURNAL_HEADER_REGION_SIZE);

		if (!overlaps)
		{
			if (candidate == usableStart)
			{
				if (Journal->OldestHeaderRegionOff >=
					candidate + Cdp_JOURNAL_HEADER_REGION_SIZE)
				{
					break;
				}
			}
			else if (Journal->PayloadRegionOff <= candidate ||
				candidate == Journal->PayloadRegionOff)
			{
				break;
			}
		}

		return STATUS_DISK_FULL;
	}

	if (candidate + Cdp_JOURNAL_HEADER_REGION_SIZE > usableEnd)
		return STATUS_INSUFFICIENT_RESOURCES;

	oldRegion = Journal->LastHeaderRegionOff;
	status = CdpJournalReadRegionLink(Journal, oldRegion, &oldLink);
	if (!NT_SUCCESS(status) || !CdpJournalRegionLinkValid(Journal, &oldLink))
		return STATUS_DISK_CORRUPT_ERROR;

	status = CdpJournalInitHeaderRegion(
		Journal,
		candidate,
		oldRegion,
		candidate,
		Journal->NextSequence);
	if (!NT_SUCCESS(status))
		return status;

	oldLink.NextRegionOff = candidate;
	status = CdpJournalWriteRegionLink(Journal, oldRegion, &oldLink);
	if (!NT_SUCCESS(status))
		return status;

	newLink.PrevRegionOff = oldRegion;
	newLink.NextRegionOff = candidate;
	newLink.StartSequence = Journal->NextSequence;
	newLink.Reserved = 0;
	status = CdpJournalWriteRegionLink(Journal, candidate, &newLink);
	if (!NT_SUCCESS(status))
		return status;

	Journal->LastHeaderRegionOff = candidate;
	Journal->CurrentHeaderRegionStartSequence = Journal->NextSequence;
	Journal->SuperblockDirty = TRUE;
	Journal->CurrentHeaderCount = 0;
	if (Journal->ActiveHeaderRegionCount == MAXUINT64)
		return STATUS_INTEGER_OVERFLOW;
	Journal->ActiveHeaderRegionCount++;
	// Payload for this header region starts immediately after it.
	Journal->PayloadRegionOff = candidate + Cdp_JOURNAL_HEADER_REGION_SIZE;

	*NewRegionOff = candidate;
	return STATUS_SUCCESS;
}

// Undo a just-allocated newest region before any record has been published.
// The abandoned bytes remain reusable at the restored payload cursor.
static NTSTATUS CdpJournalDiscardEmptyNewestRegionLocked(
	_Inout_ PCdp_JOURNAL Journal)
{
	Cdp_HEADER_REGION_LINK currentLink;
	Cdp_HEADER_REGION_LINK previousLink;
	PCdp_BRANCH_INFO_NODE latest;
	UINT64 discardedRegion;
	NTSTATUS status;

	if (!Journal || Journal->CurrentHeaderCount != 0)
		return STATUS_INVALID_DEVICE_STATE;
	latest = Journal->BranchTree.Latest;
	if (!latest)
		return STATUS_INVALID_DEVICE_STATE;
	discardedRegion = Journal->LastHeaderRegionOff;
	status = CdpJournalReadRegionLink(Journal, discardedRegion, &currentLink);
	if (!NT_SUCCESS(status) ||
		!CdpJournalRegionLinkValid(Journal, &currentLink) ||
		currentLink.PrevRegionOff == discardedRegion)
	{
		return STATUS_DISK_CORRUPT_ERROR;
	}
	status = CdpJournalReadRegionLink(
		Journal, currentLink.PrevRegionOff, &previousLink);
	if (!NT_SUCCESS(status) ||
		!CdpJournalRegionLinkValid(Journal, &previousLink) ||
		previousLink.NextRegionOff != discardedRegion ||
		latest->EndRecord.HeaderRegionOffset != currentLink.PrevRegionOff)
	{
		return STATUS_DISK_CORRUPT_ERROR;
	}
	previousLink.NextRegionOff = currentLink.PrevRegionOff;
	status = CdpJournalWriteRegionLink(
		Journal, currentLink.PrevRegionOff, &previousLink);
	if (!NT_SUCCESS(status))
		return status;

	Journal->LastHeaderRegionOff = currentLink.PrevRegionOff;
	Journal->CurrentHeaderRegionStartSequence = previousLink.StartSequence;
	Journal->CurrentHeaderCount = latest->EndRecord.HeaderIndex + 1;
	Journal->PayloadRegionOff = discardedRegion;
	if (Journal->ActiveHeaderRegionCount <= 1)
		return STATUS_DISK_CORRUPT_ERROR;
	Journal->ActiveHeaderRegionCount--;
	Journal->SuperblockDirty = TRUE;
	return STATUS_SUCCESS;
}

static NTSTATUS CdpJournalRebuildRuntimeLocked(
	_Inout_ PCdp_JOURNAL Journal,
	_Out_opt_ PUINT64 ScannedRecords)
{
	UINT64 regionOff;
	UINT64 oldestOff;
	ULONG guard = 0;
	Cdp_HEADER_REGION_LINK link;
	UINT64 expectedSequence = 0;
	BOOLEAN haveExpectedSequence = FALSE;
	BOOLEAN havePrefixRecords = FALSE;
	Cdp_BRANCH_RECORD_INFO prefixStart = { 0 };
	Cdp_BRANCH_RECORD_INFO prefixEnd = { 0 };
	LONG persistedCurrentBranch = Journal->CurrentBranchNumber;
	LONG persistedHighestBranch = Journal->HighestBranchNumber;
	LONG discoveredHighestBranch = 0;
	NTSTATUS status = STATUS_SUCCESS;
	PUCHAR region = NULL;
	UINT64 scannedRecords = 0;

	if (ScannedRecords)
		*ScannedRecords = 0;

	Journal->TotalRecords = 0;
	Journal->ActiveHeaderRegionCount = 1;
	Journal->PayloadBytesUsed = 0;
	Journal->CurrentHeaderCount = 0;
	Journal->NextSequence = 1;
	Journal->Oldest100ns = 0;
	Journal->Newest100ns = 0;
	Journal->CurrentBranchNumber = 0;
	Journal->HighestBranchNumber = 0;
	Journal->HeaderWriteCacheValid = FALSE;
	Journal->HeaderWriteCacheDirty = FALSE;
	CdpBranchTreeFree(&Journal->BranchTree);
	Journal->OldestHeaderIndex = 0;
	Journal->PayloadRegionOff =
		Journal->LastHeaderRegionOff + Cdp_JOURNAL_HEADER_REGION_SIZE;

	regionOff = Journal->LastHeaderRegionOff;
	for (;;)
	{
		status = CdpJournalReadRegionLink(Journal, regionOff, &link);
		if (!NT_SUCCESS(status) || !CdpJournalRegionLinkValid(Journal, &link))
		{
			status = STATUS_DISK_CORRUPT_ERROR;
			goto cleanup;
		}
		if (link.PrevRegionOff == regionOff)
			break;
		regionOff = link.PrevRegionOff;
		if (++guard > 100000UL ||
			++Journal->ActiveHeaderRegionCount > 100001ULL)
		{
			status = STATUS_DISK_CORRUPT_ERROR;
			goto cleanup;
		}
	}
	oldestOff = regionOff;
	Journal->OldestHeaderRegionOff = oldestOff;
	status = CdpJournalGetHeaderScanBufferLocked(Journal, &region);
	if (!NT_SUCCESS(status))
	{
		goto cleanup;
	}

	regionOff = oldestOff;
	guard = 0;
	for (;;)
	{
		ULONG index;
		ULONG limit = Cdp_JOURNAL_HEADERS_PER_REGION;
		BOOLEAN isLast = (regionOff == Journal->LastHeaderRegionOff);

		status = CdpJournalReadHeaderRegion(Journal, regionOff, region);
		if (!NT_SUCCESS(status))
			goto cleanup;
		RtlCopyMemory(
			&link,
			region + Cdp_JOURNAL_HEADER_REGION_SIZE -
				Cdp_JOURNAL_HEADER_LINK_SIZE,
			sizeof(link));
		if (!CdpJournalRegionLinkValid(Journal, &link))
		{
			status = STATUS_DISK_CORRUPT_ERROR;
			goto cleanup;
		}
		if (!isLast)
		{
			status = CdpJournalGetRegionHeaderLimitLocked(
				Journal,
				regionOff,
				&link,
				&limit,
				NULL);
			if (!NT_SUCCESS(status))
				goto cleanup;
		}
		if (haveExpectedSequence && link.StartSequence != expectedSequence)
		{
			status = STATUS_DISK_CORRUPT_ERROR;
			goto cleanup;
		}
		if (isLast)
		{
			Journal->CurrentHeaderRegionStartSequence = link.StartSequence;
			Journal->NextSequence = link.StartSequence;
		}

		for (index = 0; index < limit; ++index)
		{
			Cdp_JOURNAL_RECORD_HEADER header;
			BOOLEAN isBranch;
			BOOLEAN isDeleted;
			ULONG recordFlags;
			UINT64 globalSequence;

			RtlCopyMemory(
				&header,
				region + index * sizeof(Cdp_JOURNAL_RECORD_HEADER),
				sizeof(header));

			if (header.DataLength == 0 && header.Sequence == 0)
			{
				if (!isLast)
				{
					status = STATUS_DISK_CORRUPT_ERROR;
					goto cleanup;
				}
				Journal->CurrentHeaderCount = index;
				break;
			}
			scannedRecords++;
			isBranch = CdpJournalHeaderIsBranch(&header);
			isDeleted = CdpJournalHeaderIsDeleted(&header);
			recordFlags = header.Sequence & Cdp_JOURNAL_RECORD_FLAGS_MASK;
			if ((header.Sequence & Cdp_JOURNAL_RECORD_INDEX_MASK) != index ||
				(recordFlags & ~(Cdp_JOURNAL_RECORD_FLAG_BRANCH |
					Cdp_JOURNAL_RECORD_FLAG_DELETED)) != 0 ||
				(isBranch && isDeleted) ||
				(isDeleted && header.DataLength != 0) ||
				(isBranch &&
					recordFlags != Cdp_JOURNAL_RECORD_FLAG_BRANCH) ||
				(!isBranch && !isDeleted &&
					(header.DataLength == 0 ||
					header.DataLength > Cdp_JOURNAL_MAX_RECORD_DATA ||
					header.FileOffset < CdpJournalUsableStart(Journal) ||
					header.FileOffset > CdpJournalUsableEnd(Journal) ||
					header.DataLength >
						CdpJournalUsableEnd(Journal) - header.FileOffset ||
					header.VolumeOffset > MAXUINT64 - header.DataLength)))
			{
				status = STATUS_DISK_CORRUPT_ERROR;
				goto cleanup;
			}
			if (link.StartSequence > MAXUINT64 - index)
			{
				status = STATUS_INTEGER_OVERFLOW;
				goto cleanup;
			}
			globalSequence = link.StartSequence + index;
			if (isDeleted)
			{
				// Tombstones reserve their global Sequence but have no logical
				// record, payload or branch-tree membership.
			}
			else if (isBranch)
			{
				Cdp_JOURNAL_BRANCH_RECORD_HEADER branchHeader;
				PCdp_BRANCH_INFO_NODE branchNode;
				BOOLEAN branchContinuation;
				RtlCopyMemory(&branchHeader, &header, sizeof(branchHeader));
				branchContinuation =
					CdpJournalBranchHeaderIsContinuation(&branchHeader);
				if (!CdpJournalBranchHeaderReservedValid(&branchHeader) ||
					branchHeader.BranchNumber <= 0 ||
					branchHeader.ParentBranchNumber < 0 ||
					branchHeader.ParentBranchNumber >= branchHeader.BranchNumber ||
					(branchHeader.ParentBranchNumber == 0 &&
						branchHeader.InheritedRecordSequence != 0) ||
					(branchHeader.ParentBranchNumber != 0 &&
						(branchHeader.InheritedRecordSequence == 0 ||
						 link.StartSequence > MAXUINT64 - index ||
						 branchHeader.InheritedRecordSequence >=
							link.StartSequence + index)) ||
					(!branchContinuation &&
						branchHeader.BranchNumber <= discoveredHighestBranch))
				{
					status = STATUS_DISK_CORRUPT_ERROR;
					goto cleanup;
				}

				if (branchContinuation)
				{
					branchNode = Journal->BranchTree.Latest;
					if (!branchNode)
					{
						/* Compaction may reclaim the region containing the
						 * branch's FIRST marker.  The continuation at the new
						 * oldest boundary is then its durable synthetic start. */
						branchNode = CdpBranchTreeAllocateNode(
							branchHeader.BranchNumber,
							branchHeader.ParentBranchNumber,
							branchHeader.InheritedRecordSequence,
							globalSequence,
							branchHeader.WallClock100ns,
							regionOff,
							index,
							TRUE);
						if (!branchNode)
						{
							status = STATUS_INSUFFICIENT_RESOURCES;
							goto cleanup;
						}
						status = CdpBranchTreeAttachNode(
							&Journal->BranchTree, branchNode);
						if (!NT_SUCCESS(status))
						{
							cdpfree(branchNode);
							goto cleanup;
						}
						discoveredHighestBranch = branchHeader.BranchNumber;
					}
					else if (
						branchNode->BranchNumber != branchHeader.BranchNumber ||
						branchNode->ParentBranchNumber !=
							branchHeader.ParentBranchNumber ||
						branchNode->InheritedRecordSequence !=
							branchHeader.InheritedRecordSequence)
					{
						status = STATUS_DISK_CORRUPT_ERROR;
						goto cleanup;
					}
					if (branchNode->EndRecord.Sequence != globalSequence)
					{
						CdpBranchTreeAdvanceLatest(
							&Journal->BranchTree,
							globalSequence,
							branchHeader.WallClock100ns,
							regionOff,
							index);
					}
				}
				else
				{
					if (havePrefixRecords && !Journal->BranchTree.First)
					{
						PCdp_BRANCH_INFO_NODE prefixNode;
						if (branchHeader.BranchNumber <= 1)
						{
							status = STATUS_DISK_CORRUPT_ERROR;
							goto cleanup;
						}
						prefixNode = CdpBranchTreeAllocateNode(
							branchHeader.BranchNumber - 1,
							0,
							0,
							prefixStart.Sequence,
							prefixStart.WallClock100ns,
							prefixStart.HeaderRegionOffset,
							prefixStart.HeaderIndex,
							TRUE);
						if (!prefixNode)
						{
							status = STATUS_INSUFFICIENT_RESOURCES;
							goto cleanup;
						}
						prefixNode->EndRecord = prefixEnd;
						prefixNode->Latest = FALSE;
						status = CdpBranchTreeAttachNode(
							&Journal->BranchTree, prefixNode);
						if (!NT_SUCCESS(status))
						{
							cdpfree(prefixNode);
							goto cleanup;
						}
						discoveredHighestBranch = prefixNode->BranchNumber;
					}

					if (Journal->BranchTree.Latest)
						Journal->BranchTree.Latest->Latest = FALSE;
					branchNode = CdpBranchTreeAllocateNode(
						branchHeader.BranchNumber,
						branchHeader.ParentBranchNumber,
						branchHeader.InheritedRecordSequence,
						globalSequence,
						branchHeader.WallClock100ns,
						regionOff,
						index,
						FALSE);
					if (!branchNode)
					{
						status = STATUS_INSUFFICIENT_RESOURCES;
						goto cleanup;
					}
					status = CdpBranchTreeAttachNode(
						&Journal->BranchTree, branchNode);
					if (!NT_SUCCESS(status))
					{
						cdpfree(branchNode);
						goto cleanup;
					}
					discoveredHighestBranch = branchHeader.BranchNumber;
				}
			}
			else
			{
				if (Journal->BranchTree.Latest)
				{
					CdpBranchTreeAdvanceLatest(
						&Journal->BranchTree,
						globalSequence,
						header.WallClock100ns,
						regionOff,
						index);
				}
				else
				{
					Cdp_BRANCH_RECORD_INFO info;
					CdpBranchRecordInfoSet(
						&info,
						globalSequence,
						header.WallClock100ns,
						regionOff,
						index);
					if (!havePrefixRecords)
						prefixStart = info;
					prefixEnd = info;
					havePrefixRecords = TRUE;
				}
			}

			if (!isDeleted)
				Journal->TotalRecords++;
			if (globalSequence == MAXUINT64)
			{
				status = STATUS_INTEGER_OVERFLOW;
				goto cleanup;
			}
			expectedSequence = globalSequence + 1;
			haveExpectedSequence = TRUE;
			if (isLast)
				Journal->NextSequence = expectedSequence;

			if (!isDeleted && (Journal->Oldest100ns == 0 ||
				header.WallClock100ns < Journal->Oldest100ns))
			{
				Journal->Oldest100ns = header.WallClock100ns;
			}
			if (!isDeleted && header.WallClock100ns > Journal->Newest100ns)
				Journal->Newest100ns = header.WallClock100ns;

			if (isLast)
			{
				Journal->CurrentHeaderCount = index + 1;
				if (!isBranch)
				{
					Journal->PayloadRegionOff = CdpAlignUp64(
						header.FileOffset + header.DataLength,
						Journal->SectorSize);
				}
			}
		}

		{
			UINT64 payloadBytes;
			UINT64 payloadEnd = isLast ?
				Journal->PayloadRegionOff : link.NextRegionOff;

			status = CdpJournalRingDistance(
				Journal,
				regionOff + Cdp_JOURNAL_HEADER_REGION_SIZE,
				payloadEnd,
				&payloadBytes);
			if (!NT_SUCCESS(status) ||
				Journal->PayloadBytesUsed > MAXUINT64 - payloadBytes)
			{
				status = STATUS_DISK_CORRUPT_ERROR;
				goto cleanup;
			}
			Journal->PayloadBytesUsed += payloadBytes;
		}

		if (isLast)
			break;
		if (link.NextRegionOff == regionOff)
			break;
		regionOff = link.NextRegionOff;
		if (++guard > 100000UL)
		{
			status = STATUS_DISK_CORRUPT_ERROR;
			goto cleanup;
		}
		if (regionOff == oldestOff)
			break;
	}

	if (!Journal->BranchTree.First && havePrefixRecords)
	{
		PCdp_BRANCH_INFO_NODE prefixNode;
		if (persistedCurrentBranch <= 0)
		{
			status = STATUS_DISK_CORRUPT_ERROR;
			goto cleanup;
		}
		prefixNode = CdpBranchTreeAllocateNode(
			persistedCurrentBranch,
			0,
			0,
			prefixStart.Sequence,
			prefixStart.WallClock100ns,
			prefixStart.HeaderRegionOffset,
			prefixStart.HeaderIndex,
			TRUE);
		if (!prefixNode)
		{
			status = STATUS_INSUFFICIENT_RESOURCES;
			goto cleanup;
		}
		prefixNode->EndRecord = prefixEnd;
		status = CdpBranchTreeAttachNode(&Journal->BranchTree, prefixNode);
		if (!NT_SUCCESS(status))
		{
			cdpfree(prefixNode);
			goto cleanup;
		}
		discoveredHighestBranch = persistedCurrentBranch;
	}

	if (discoveredHighestBranch > persistedHighestBranch ||
		persistedCurrentBranch <= 0 || persistedHighestBranch <= 0)
	{
		status = STATUS_DISK_CORRUPT_ERROR;
		goto cleanup;
	}
	Journal->CurrentBranchNumber = persistedCurrentBranch;
	Journal->HighestBranchNumber = persistedHighestBranch;

cleanup:
	if (ScannedRecords)
		*ScannedRecords = scannedRecords;
	if (!NT_SUCCESS(status))
		CdpBranchTreeFree(&Journal->BranchTree);
	return status;
}

VOID CdpJournalInitialize(
	_Out_ PCdp_JOURNAL Journal,
	_In_opt_ PVOID TargetDevice,
	_In_opt_ PVOID RawDiskHandle,
	_In_ UINT64 TargetBaseOffset,
	_In_ UINT64 PartitionSize,
	_In_ ULONG SectorSize,
	_In_ const GUID* SourceVolumeGuid)
{
	RtlZeroMemory(Journal, sizeof(*Journal));
	Journal->TargetDevice = TargetDevice;
	Journal->RawDiskHandle = RawDiskHandle;
	Journal->TargetBaseOffset = TargetBaseOffset;
	Journal->Store = NULL;
	Journal->PartitionSize = CdpAlignDown64(PartitionSize, SectorSize);
	Journal->SectorSize = SectorSize;
	Journal->SourceVolumeGuid = *SourceVolumeGuid;
	Cdp_LOCK_INIT(&Journal->Lock);
}

VOID CdpJournalInitializeWithStore(
	_Out_ PCdp_JOURNAL Journal,
	_In_ PCdp_STORE Store,
	_In_ const GUID* SourceVolumeGuid,
	_In_opt_ Cdp_QUERY_TIME_100NS QueryTime100ns,
	_In_opt_ PVOID QueryTimeContext)
{
	RtlZeroMemory(Journal, sizeof(*Journal));
	Journal->TargetDevice = NULL;
	Journal->Store = Store;
	Journal->PartitionSize = CdpAlignDown64(Store->Size, Store->SectorSize);
	Journal->SectorSize = Store->SectorSize;
	Journal->SourceVolumeGuid = *SourceVolumeGuid;
	Journal->QueryTime100ns = QueryTime100ns;
	Journal->QueryTimeContext = QueryTimeContext;
	Cdp_LOCK_INIT(&Journal->Lock);
}

VOID CdpJournalSetMetadataDevice(
	_Inout_ PCdp_JOURNAL Journal,
	_In_opt_ PVOID TargetDevice,
	_In_ UINT64 TargetBaseOffset)
{
	if (!Journal || Journal->Mounted)
		return;
	Journal->MetadataTargetDevice = TargetDevice;
	Journal->MetadataTargetBaseOffset = TargetBaseOffset;
}

VOID CdpJournalSetPhysicalLayout(
	_Inout_ PCdp_JOURNAL Journal,
	_In_ ULONG DiskPartitionStyle,
	_In_ ULONG MbrSignature,
	_In_ const GUID* DiskGuid,
	_In_ UINT64 SourcePartitionStart,
	_In_ UINT64 SourcePartitionSize,
	_In_ UINT64 JournalPartitionStart,
	_In_ UINT64 JournalPartitionSize)
{
	if (!Journal)
		return;
	Journal->DiskPartitionStyle = DiskPartitionStyle;
	Journal->MbrSignature = MbrSignature;
	if (DiskGuid)
		Journal->DiskGuid = *DiskGuid;
	else
		RtlZeroMemory(&Journal->DiskGuid, sizeof(Journal->DiskGuid));
	Journal->SourcePartitionStart = SourcePartitionStart;
	Journal->SourcePartitionSize = SourcePartitionSize;
	Journal->JournalPartitionStart = JournalPartitionStart;
	Journal->JournalPartitionSize = JournalPartitionSize;
}

static BOOLEAN CdpJournalHasBackend(_In_ PCdp_JOURNAL Journal)
{
	return Journal->Store != NULL ||
		Journal->RawDiskHandle != NULL ||
		Journal->TargetDevice != NULL;
}

NTSTATUS CdpJournalFormat(_Inout_ PCdp_JOURNAL Journal)
{
	UINT64 usableStart;
	UINT64 usableEnd;
	UINT64 headerOff;
	UINT64 minSize;
	NTSTATUS status;

	minSize = (UINT64)Journal->SectorSize +
		Cdp_JOURNAL_HEADER_REGION_SIZE + (UINT64)Journal->SectorSize;
	if (!CdpJournalHasBackend(Journal) ||
		(Journal->SectorSize != 512 && Journal->SectorSize != 4096) ||
		Journal->PartitionSize < minSize)
	{
		return STATUS_INVALID_PARAMETER;
	}

	Cdp_LOCK_ACQUIRE(&Journal->Lock);

	usableStart = CdpJournalUsableStart(Journal);
	usableEnd = CdpJournalUsableEnd(Journal);
	headerOff = usableStart;
	if (headerOff + Cdp_JOURNAL_HEADER_REGION_SIZE >= usableEnd)
	{
		status = STATUS_INVALID_PARAMETER;
		goto cleanup;
	}

	status = CdpJournalInitHeaderRegion(
		Journal,
		headerOff,
		headerOff,
		headerOff,
		1);
	if (!NT_SUCCESS(status))
		goto cleanup;

	Journal->LastHeaderRegionOff = headerOff;
	Journal->CurrentHeaderRegionStartSequence = 1;
	// Payload area 0 starts immediately after header region 0.
	Journal->PayloadRegionOff = headerOff + Cdp_JOURNAL_HEADER_REGION_SIZE;
	Journal->OldestHeaderRegionOff = headerOff;
	Journal->OldestHeaderIndex = 0;
	Journal->CurrentHeaderCount = 0;
	Journal->NextSequence = 1;
	Journal->TotalRecords = 0;
	Journal->ActiveHeaderRegionCount = 1;
	Journal->PayloadBytesUsed = 0;
	Journal->RecordGeneration = 1;
	Journal->Oldest100ns = 0;
	Journal->Newest100ns = 0;
	Journal->CurrentBranchNumber = 0;
	Journal->HighestBranchNumber = 0;
	Journal->RecoveryPending = FALSE;
	Journal->RecoveryFsRepairPending = FALSE;
	Journal->RecoveryTargetTime100ns = 0;
	Journal->RestorePointSet = FALSE;
	Journal->RestorePointTime100ns = 0;
	Journal->HeaderWriteCacheValid = FALSE;
	Journal->HeaderWriteCacheDirty = FALSE;
	CdpBranchTreeFree(&Journal->BranchTree);

	Journal->SuperblockDirty = TRUE;
	status = CdpJournalWriteSuperblockLocked(Journal);
	if (NT_SUCCESS(status))
		status = CdpJournalFlush(Journal);
	if (NT_SUCCESS(status))
	{
		Journal->Mounted = TRUE;
		status = CdpJournalAppendBranchLocked(Journal, 1, 0, 0);
		if (!NT_SUCCESS(status))
			Journal->Mounted = FALSE;
	}

cleanup:
	Cdp_LOCK_RELEASE(&Journal->Lock);
	return status;
}

NTSTATUS CdpJournalResetHistoryPreserveRestorePoint(
	_Inout_ PCdp_JOURNAL Journal)
{
	UINT64 usableStart;
	UINT64 usableEnd;
	UINT64 headerOff;
	UINT64 minSize;
	NTSTATUS status;

	if (!Journal)
		return STATUS_INVALID_PARAMETER;
	minSize = (UINT64)Journal->SectorSize +
		Cdp_JOURNAL_HEADER_REGION_SIZE + (UINT64)Journal->SectorSize;
	if (!CdpJournalHasBackend(Journal) || !Journal->Mounted ||
		(Journal->SectorSize != 512 && Journal->SectorSize != 4096) ||
		Journal->PartitionSize < minSize)
	{
		return STATUS_INVALID_PARAMETER;
	}

	Cdp_LOCK_ACQUIRE(&Journal->Lock);
	/* Recovery has precedence and must never be destroyed by this operation. */
	if (Journal->RecoveryPending)
	{
		status = STATUS_DEVICE_BUSY;
		goto cleanup;
	}
	if (!Journal->RestorePointSet || Journal->RestorePointTime100ns == 0)
	{
		status = STATUS_INVALID_DEVICE_STATE;
		goto cleanup;
	}

	usableStart = CdpJournalUsableStart(Journal);
	usableEnd = CdpJournalUsableEnd(Journal);
	headerOff = usableStart;
	if (headerOff + Cdp_JOURNAL_HEADER_REGION_SIZE >= usableEnd)
	{
		status = STATUS_INVALID_PARAMETER;
		goto cleanup;
	}
	status = CdpJournalInitHeaderRegion(
		Journal, headerOff, headerOff, headerOff, 1);
	if (!NT_SUCCESS(status))
		goto cleanup;

	CdpBranchTreeFree(&Journal->BranchTree);
	Journal->HeaderWriteCacheValid = FALSE;
	Journal->HeaderWriteCacheDirty = FALSE;
	Journal->LastHeaderRegionOff = headerOff;
	Journal->CurrentHeaderRegionStartSequence = 1;
	Journal->PayloadRegionOff = headerOff + Cdp_JOURNAL_HEADER_REGION_SIZE;
	Journal->OldestHeaderRegionOff = headerOff;
	Journal->OldestHeaderIndex = 0;
	Journal->CurrentHeaderCount = 0;
	Journal->NextSequence = 1;
	Journal->TotalRecords = 0;
	Journal->ActiveHeaderRegionCount = 1;
	Journal->PayloadBytesUsed = 0;
	Journal->Oldest100ns = 0;
	Journal->Newest100ns = 0;
	Journal->CurrentBranchNumber = 0;
	Journal->HighestBranchNumber = 0;
	Journal->RecoveryPending = FALSE;
	Journal->RecoveryFsRepairPending = FALSE;
	Journal->RecoveryTargetTime100ns = 0;
	InterlockedExchange(&Journal->RecoveryFsRepairAttempts, 0);
	CdpJournalAdvanceRecordGenerationLocked(Journal);
	Journal->SuperblockDirty = TRUE;
	/* AppendBranch persists the initialized RR and the valid branch-1
	 * superblock in its existing flush -> superblock -> flush order. */
	status = CdpJournalAppendBranchLocked(Journal, 1, 0, 0);

cleanup:
	Cdp_LOCK_RELEASE(&Journal->Lock);
	return status;
}

NTSTATUS CdpJournalMount(_Inout_ PCdp_JOURNAL Journal)
{
	PVOID allocationBase = NULL;
	PUCHAR sector;
	PCdp_JOURNAL_SUPERBLOCK superblock;
	UINT64 minSize;
	LONG persistedCurrentBranch;
	LONG persistedHighestBranch;
	NTSTATUS status;
	UINT64 scannedRecords = 0;
#ifndef Cdp_USERMODE
	UINT64 mountStart100ns;
	UINT64 mountElapsed100ns;
#endif

	minSize = (UINT64)Journal->SectorSize +
		Cdp_JOURNAL_HEADER_REGION_SIZE + (UINT64)Journal->SectorSize;
	if (!CdpJournalHasBackend(Journal) ||
		(Journal->SectorSize != 512 && Journal->SectorSize != 4096) ||
		Journal->PartitionSize < minSize)
	{
		return STATUS_INVALID_PARAMETER;
	}

	Cdp_LOCK_ACQUIRE(&Journal->Lock);
#ifndef Cdp_USERMODE
	mountStart100ns = KeQueryInterruptTime();
#endif
	sector = (PUCHAR)CdpAllocateAligned(Journal,
		Journal->SectorSize,
		&allocationBase);
	if (!sector)
	{
		status = STATUS_INSUFFICIENT_RESOURCES;
		goto cleanup;
	}

	status = CdpJournalMetadataRawIo(
		Journal,
		IRP_MJ_READ,
		0,
		Journal->SectorSize,
		sector);
	if (!NT_SUCCESS(status))
		goto cleanup;
	superblock = (PCdp_JOURNAL_SUPERBLOCK)sector;
	if (!CdpJournalSuperblockValid(Journal, superblock))
	{
		status = STATUS_DISK_CORRUPT_ERROR;
		goto cleanup;
	}

	Journal->LastHeaderRegionOff = superblock->LastHeaderRegionOff;
	persistedCurrentBranch = superblock->CurrentBranchNumber;
	persistedHighestBranch = superblock->HighestBranchNumber;
	Journal->CurrentBranchNumber = persistedCurrentBranch;
	Journal->HighestBranchNumber = persistedHighestBranch;
	Journal->SourceVolumeGuid = superblock->SourceVolumeGuid;
	Journal->DiskPartitionStyle = superblock->DiskPartitionStyle;
	Journal->MbrSignature = superblock->MbrSignature;
	Journal->DiskGuid = superblock->DiskGuid;
	Journal->SourcePartitionStart = superblock->SourcePartitionStart;
	Journal->SourcePartitionSize = superblock->SourcePartitionSize;
	Journal->JournalPartitionStart = superblock->JournalPartitionStart;
	Journal->JournalPartitionSize = superblock->JournalPartitionSize;
	Journal->RecoveryPending =
		(superblock->Flags & Cdp_JOURNAL_FLAG_RECOVERY_PENDING) != 0;
	Journal->RecoveryFsRepairPending =
		(superblock->Flags & Cdp_JOURNAL_FLAG_RECOVERY_FS_REPAIR_PENDING) != 0;
	InterlockedExchange(&Journal->RecoveryFsRepairAttempts, 0);
	Journal->RecoveryTargetTime100ns = Journal->RecoveryPending ?
		superblock->RecoveryTargetTime100ns : 0;
	Journal->RestorePointSet =
		(superblock->Version >= Cdp_JOURNAL_VERSION) &&
		((superblock->Flags & Cdp_JOURNAL_FLAG_RESTORE_POINT_SET) != 0);
	Journal->RestorePointTime100ns = Journal->RestorePointSet ?
		superblock->RestorePointTime100ns : 0;
	Journal->CredentialConfigured =
		(superblock->Flags & Cdp_JOURNAL_FLAG_CREDENTIAL_CONFIGURED) != 0;
	if (Journal->CredentialConfigured)
		Journal->Credential = superblock->Credential;
	else
		RtlZeroMemory(&Journal->Credential, sizeof(Journal->Credential));
	Journal->SuperblockDirty = FALSE;

	status = CdpJournalRebuildRuntimeLocked(Journal, &scannedRecords);
	if (!NT_SUCCESS(status))
		goto cleanup;
	if (Journal->CurrentBranchNumber != persistedCurrentBranch ||
		Journal->HighestBranchNumber != persistedHighestBranch)
	{
		status = STATUS_DISK_CORRUPT_ERROR;
		goto cleanup;
	}

	Journal->RecordGeneration = 1;
	Journal->Mounted = TRUE;

#ifndef Cdp_USERMODE
	Cdp_DBG("[JOURNAL] mounted lastHeader=%llu "
		"payloadHead=%llu records=%llu\n",
		Journal->LastHeaderRegionOff,
		Journal->PayloadRegionOff,
		Journal->TotalRecords);
#endif
	status = STATUS_SUCCESS;

cleanup:
#ifndef Cdp_USERMODE
	mountElapsed100ns = KeQueryInterruptTime() - mountStart100ns;
	Cdp_LOG("[JOURNAL-MOUNT-SCAN] status=0x%08X scannedRecords=%llu activeRecords=%llu headerRegions=%llu elapsedUs=%llu\n",
		status,
		scannedRecords,
		Journal->TotalRecords,
		Journal->ActiveHeaderRegionCount,
		mountElapsed100ns / 10ULL);
#endif
	if (allocationBase)
		cdpfree(allocationBase);
	if (!NT_SUCCESS(status))
		Journal->Mounted = FALSE;
	Cdp_LOCK_RELEASE(&Journal->Lock);
	return status;
}

static NTSTATUS CdpJournalAppendBranchLocked(
	_Inout_ PCdp_JOURNAL Journal,
	_In_ LONG BranchNumber,
	_In_ LONG ParentBranchNumber,
	_In_ UINT64 InheritedRecordSequence)
{
	Cdp_JOURNAL_BRANCH_RECORD_HEADER branchHeader;
	PCdp_BRANCH_INFO_NODE branchNode = NULL;
	UINT64 writeSequence;
	UINT64 writeTime;
	BOOLEAN newRegionAllocated = FALSE;
	NTSTATUS status;

	if (!Journal->Mounted)
		return STATUS_DEVICE_NOT_READY;
	if (BranchNumber <= 0 || BranchNumber != Journal->HighestBranchNumber + 1 ||
		ParentBranchNumber < 0 ||
		ParentBranchNumber > Journal->HighestBranchNumber ||
		(ParentBranchNumber == 0 && InheritedRecordSequence != 0) ||
		(ParentBranchNumber != 0 &&
			(InheritedRecordSequence == 0 ||
			 InheritedRecordSequence >= Journal->NextSequence)))
	{
		return STATUS_INVALID_PARAMETER;
	}

	// Branch boundaries are also header-region boundaries. Branch 1 uses the
	// fresh region created by Format; every later branch starts at header[0]
	// of a newly allocated region even when the previous region has free slots.
	if (Journal->TotalRecords != 0)
	{
		UINT64 newRegion = 0;
		status = CdpJournalAllocateHeaderRegionLocked(Journal, &newRegion);
		if (!NT_SUCCESS(status))
		{
#ifndef Cdp_USERMODE
			Cdp_LOG("[RECOVERY-BRANCH-FAIL] stage=allocate-header-region status=0x%08X branch=%ld parent=%ld inherit=%llu payload=%llu lastRegion=%llu\n",
				status,
				BranchNumber,
				ParentBranchNumber,
				InheritedRecordSequence,
				Journal->PayloadRegionOff,
				Journal->LastHeaderRegionOff);
#endif
			return status;
		}
		newRegionAllocated = TRUE;
	}
	if (Journal->CurrentHeaderCount != 0)
	{
		if (newRegionAllocated)
			(void)CdpJournalDiscardEmptyNewestRegionLocked(Journal);
		return STATUS_DISK_CORRUPT_ERROR;
	}
	if (Journal->NextSequence == MAXUINT64 ||
		Journal->CurrentHeaderRegionStartSequence >
			MAXUINT64 - Journal->CurrentHeaderCount ||
		Journal->CurrentHeaderRegionStartSequence +
			Journal->CurrentHeaderCount != Journal->NextSequence)
	{
		if (newRegionAllocated)
			(void)CdpJournalDiscardEmptyNewestRegionLocked(Journal);
		return STATUS_INTEGER_OVERFLOW;
	}

	writeSequence = Journal->NextSequence;
	writeTime = CdpJournalQueryWallClock100ns(Journal);
	if (Journal->Newest100ns != 0 && writeTime <= Journal->Newest100ns)
	{
		if (Journal->Newest100ns == MAXUINT64)
		{
			if (newRegionAllocated)
				(void)CdpJournalDiscardEmptyNewestRegionLocked(Journal);
			return STATUS_INTEGER_OVERFLOW;
		}
		writeTime = Journal->Newest100ns + 1;
	}
	branchNode = CdpBranchTreeAllocateNode(
		BranchNumber,
		ParentBranchNumber,
		InheritedRecordSequence,
		writeSequence,
		writeTime,
		Journal->LastHeaderRegionOff,
		Journal->CurrentHeaderCount,
		FALSE);
	if (!branchNode)
	{
		if (newRegionAllocated)
			(void)CdpJournalDiscardEmptyNewestRegionLocked(Journal);
		return STATUS_INSUFFICIENT_RESOURCES;
	}
	RtlZeroMemory(&branchHeader, sizeof(branchHeader));
	branchHeader.WallClock100ns = writeTime;
	branchHeader.BranchNumber = BranchNumber;
	branchHeader.ParentBranchNumber = ParentBranchNumber;
	branchHeader.InheritedRecordSequence = InheritedRecordSequence;
	branchHeader.Reserved = Cdp_JOURNAL_BRANCH_RECORD_FLAG_FIRST;
	branchHeader.Sequence = Journal->CurrentHeaderCount |
		Cdp_JOURNAL_RECORD_FLAG_BRANCH;

	status = CdpJournalWriteHeaderAt(
		Journal,
		Journal->LastHeaderRegionOff,
		Journal->CurrentHeaderCount,
		(PCdp_JOURNAL_RECORD_HEADER)&branchHeader);
	if (!NT_SUCCESS(status))
	{
		NTSTATUS discardStatus = STATUS_SUCCESS;
#ifndef Cdp_USERMODE
		Cdp_LOG("[RECOVERY-BRANCH-FAIL] stage=write-branch-header status=0x%08X branch=%ld region=%llu index=%lu\n",
			status,
			BranchNumber,
			Journal->LastHeaderRegionOff,
			Journal->CurrentHeaderCount);
#endif
		cdpfree(branchNode);
		if (newRegionAllocated)
			discardStatus = CdpJournalDiscardEmptyNewestRegionLocked(Journal);
		return NT_SUCCESS(discardStatus) ? status : discardStatus;
	}

	if (Journal->BranchTree.Latest)
		Journal->BranchTree.Latest->Latest = FALSE;
	status = CdpBranchTreeAttachNode(&Journal->BranchTree, branchNode);
	if (!NT_SUCCESS(status))
	{
		cdpfree(branchNode);
		return status;
	}

	Journal->CurrentHeaderCount++;
	Journal->NextSequence = writeSequence + 1;
	Journal->TotalRecords++;
	Journal->CurrentBranchNumber = BranchNumber;
	Journal->HighestBranchNumber = BranchNumber;
	Journal->SuperblockDirty = TRUE;
	CdpJournalAdvanceRecordGenerationLocked(Journal);
	Journal->Newest100ns = writeTime;
	if (Journal->TotalRecords == 1)
	{
		Journal->OldestHeaderRegionOff = Journal->LastHeaderRegionOff;
		Journal->OldestHeaderIndex = Journal->CurrentHeaderCount - 1;
		Journal->Oldest100ns = writeTime;
	}

	if (Journal->SuperblockDirty)
	{
		status = CdpJournalFlush(Journal);
		if (!NT_SUCCESS(status))
		{
#ifndef Cdp_USERMODE
			Cdp_LOG("[RECOVERY-BRANCH-FAIL] stage=pre-superblock-flush status=0x%08X branch=%ld\n",
				status, BranchNumber);
#endif
		}
		if (NT_SUCCESS(status))
		{
			status = CdpJournalWriteSuperblockLocked(Journal);
			if (!NT_SUCCESS(status))
			{
#ifndef Cdp_USERMODE
				Cdp_LOG("[RECOVERY-BRANCH-FAIL] stage=write-superblock status=0x%08X branch=%ld\n",
					status, BranchNumber);
#endif
			}
		}
		if (NT_SUCCESS(status))
		{
			status = CdpJournalFlush(Journal);
			if (!NT_SUCCESS(status))
			{
#ifndef Cdp_USERMODE
				Cdp_LOG("[RECOVERY-BRANCH-FAIL] stage=post-superblock-flush status=0x%08X branch=%ld\n",
					status, BranchNumber);
#endif
			}
		}
	}
	else
	{
		status = CdpJournalFlush(Journal);
		if (!NT_SUCCESS(status))
		{
#ifndef Cdp_USERMODE
			Cdp_LOG("[RECOVERY-BRANCH-FAIL] stage=branch-flush status=0x%08X branch=%ld\n",
				status, BranchNumber);
#endif
		}
	}
	return status;
}

// Every header region begins with the identity of the currently active branch.
// Unlike a branch-creation marker, this continuation does not create a tree
// node or alter ancestry; Reserved makes the two forms unambiguous on disk.
static NTSTATUS CdpJournalAppendBranchContinuationLocked(
	_Inout_ PCdp_JOURNAL Journal)
{
	PCdp_BRANCH_INFO_NODE branchNode;
	Cdp_JOURNAL_BRANCH_RECORD_HEADER branchHeader;
	UINT64 writeSequence;
	UINT64 writeTime;
	NTSTATUS status;

	if (!Journal || !Journal->Mounted || Journal->CurrentHeaderCount != 0 ||
		Journal->NextSequence == MAXUINT64)
	{
		return STATUS_INVALID_DEVICE_STATE;
	}
	branchNode = Journal->BranchTree.Latest;
	if (!branchNode || branchNode->BranchNumber != Journal->CurrentBranchNumber)
		return STATUS_INVALID_DEVICE_STATE;
	if (Journal->CurrentHeaderRegionStartSequence != Journal->NextSequence)
		return STATUS_DISK_CORRUPT_ERROR;

	writeSequence = Journal->NextSequence;
	writeTime = CdpJournalQueryWallClock100ns(Journal);
	if (Journal->Newest100ns != 0 && writeTime <= Journal->Newest100ns)
	{
		if (Journal->Newest100ns == MAXUINT64)
			return STATUS_INTEGER_OVERFLOW;
		writeTime = Journal->Newest100ns + 1;
	}
	RtlZeroMemory(&branchHeader, sizeof(branchHeader));
	branchHeader.WallClock100ns = writeTime;
	branchHeader.BranchNumber = branchNode->BranchNumber;
	branchHeader.ParentBranchNumber = branchNode->ParentBranchNumber;
	branchHeader.InheritedRecordSequence = branchNode->InheritedRecordSequence;
	branchHeader.Reserved = Cdp_JOURNAL_BRANCH_RECORD_FLAG_CONTINUATION;
	branchHeader.Sequence = Cdp_JOURNAL_RECORD_FLAG_BRANCH;
	status = CdpJournalWriteHeaderAt(
		Journal,
		Journal->LastHeaderRegionOff,
		0,
		(PCdp_JOURNAL_RECORD_HEADER)&branchHeader);
	if (!NT_SUCCESS(status))
		return status;

	CdpBranchTreeAdvanceLatest(
		&Journal->BranchTree,
		writeSequence,
		writeTime,
		Journal->LastHeaderRegionOff,
		0);
	Journal->CurrentHeaderCount = 1;
	Journal->NextSequence = writeSequence + 1;
	Journal->TotalRecords++;
	Journal->Newest100ns = writeTime;
	CdpJournalAdvanceRecordGenerationLocked(Journal);
	return STATUS_SUCCESS;
}

NTSTATUS CdpJournalAppendBranch(
	_Inout_ PCdp_JOURNAL Journal,
	_In_ LONG BranchNumber,
	_In_ LONG ParentBranchNumber,
	_In_ UINT64 InheritedRecordSequence)
{
	NTSTATUS status;
	if (!Journal)
		return STATUS_INVALID_PARAMETER;
	Cdp_LOCK_ACQUIRE(&Journal->Lock);
	status = CdpJournalAppendBranchLocked(
		Journal,
		BranchNumber,
		ParentBranchNumber,
		InheritedRecordSequence);
	Cdp_LOCK_RELEASE(&Journal->Lock);
	return status;
}

NTSTATUS CdpJournalRollbackLatestBranch(
	_Inout_ PCdp_JOURNAL Journal,
	_In_ LONG BranchNumber)
{
	Cdp_JOURNAL_RECORD_HEADER header;
	Cdp_HEADER_REGION_LINK currentLink;
	Cdp_HEADER_REGION_LINK previousLink;
	PCdp_BRANCH_INFO_NODE latest;
	PCdp_BRANCH_INFO_NODE previousNode;
	UINT64 rolledBackRegion;
	LONG previousBranch;
	NTSTATUS status;

	if (!Journal || BranchNumber <= 0)
		return STATUS_INVALID_PARAMETER;
	Cdp_LOCK_ACQUIRE(&Journal->Lock);
	latest = Journal->BranchTree.Latest;
	if (!Journal->Mounted)
	{
		status = STATUS_DEVICE_NOT_READY;
		goto cleanup;
	}
	if (!latest || latest->BranchNumber != BranchNumber ||
		Journal->CurrentBranchNumber != BranchNumber ||
		Journal->HighestBranchNumber != BranchNumber ||
		Journal->CurrentHeaderCount != 1 || Journal->NextSequence <= 1 ||
		latest->StartRecord.Sequence != Journal->NextSequence - 1 ||
		latest->EndRecord.Sequence != latest->StartRecord.Sequence ||
		latest->StartRecord.HeaderIndex != 0 || !latest->Previous)
	{
		status = STATUS_INVALID_DEVICE_STATE;
		goto cleanup;
	}
	status = CdpJournalReadHeaderAt(
		Journal,
		Journal->LastHeaderRegionOff,
		0,
		&header);
	if (!NT_SUCCESS(status))
		goto cleanup;
	if (!CdpJournalHeaderIsBranch(&header) ||
		(header.Sequence & Cdp_JOURNAL_RECORD_INDEX_MASK) != 0)
	{
		status = STATUS_DISK_CORRUPT_ERROR;
		goto cleanup;
	}
	rolledBackRegion = Journal->LastHeaderRegionOff;
	status = CdpJournalReadRegionLink(Journal, rolledBackRegion, &currentLink);
	if (!NT_SUCCESS(status) ||
		!CdpJournalRegionLinkValid(Journal, &currentLink) ||
		currentLink.PrevRegionOff == rolledBackRegion)
	{
		status = STATUS_DISK_CORRUPT_ERROR;
		goto cleanup;
	}
	status = CdpJournalReadRegionLink(
		Journal, currentLink.PrevRegionOff, &previousLink);
	if (!NT_SUCCESS(status) ||
		!CdpJournalRegionLinkValid(Journal, &previousLink) ||
		previousLink.NextRegionOff != rolledBackRegion)
	{
		status = STATUS_DISK_CORRUPT_ERROR;
		goto cleanup;
	}
	previousNode = latest->Previous;
	if (previousNode->EndRecord.HeaderRegionOffset != currentLink.PrevRegionOff)
	{
		status = STATUS_DISK_CORRUPT_ERROR;
		goto cleanup;
	}
	previousBranch = previousNode->BranchNumber;
	previousLink.NextRegionOff = currentLink.PrevRegionOff;
	status = CdpJournalWriteRegionLink(
		Journal, currentLink.PrevRegionOff, &previousLink);
	if (!NT_SUCCESS(status))
		goto cleanup;

	Journal->LastHeaderRegionOff = currentLink.PrevRegionOff;
	Journal->CurrentHeaderRegionStartSequence = previousLink.StartSequence;
	Journal->CurrentHeaderCount = previousNode->EndRecord.HeaderIndex + 1;
	Journal->PayloadRegionOff = rolledBackRegion;
	Journal->NextSequence--;
	Journal->TotalRecords--;
	Journal->CurrentBranchNumber = previousBranch;
	Journal->HighestBranchNumber = previousBranch;
	CdpBranchTreeRemoveLatest(&Journal->BranchTree);
	Journal->Newest100ns = Journal->BranchTree.Latest ?
		Journal->BranchTree.Latest->EndRecord.WallClock100ns : 0;
	Journal->SuperblockDirty = TRUE;
	CdpJournalAdvanceRecordGenerationLocked(Journal);
	status = CdpJournalFlush(Journal);
	if (NT_SUCCESS(status))
		status = CdpJournalWriteSuperblockLocked(Journal);
	if (NT_SUCCESS(status))
		status = CdpJournalFlush(Journal);

cleanup:
	Cdp_LOCK_RELEASE(&Journal->Lock);
	return status;
}

NTSTATUS CdpJournalSetRecoveryIntent(
	_Inout_ PCdp_JOURNAL Journal,
	_In_ UINT64 TargetTime100ns)
{
	NTSTATUS status;

	if (!Journal || TargetTime100ns == 0)
		return STATUS_INVALID_PARAMETER;
	Cdp_LOCK_ACQUIRE(&Journal->Lock);
	if (!Journal->Mounted)
	{
		status = STATUS_DEVICE_NOT_READY;
		goto done;
	}
	Journal->RecoveryPending = TRUE;
	Journal->RecoveryFsRepairPending = FALSE;
	InterlockedExchange(&Journal->RecoveryFsRepairAttempts, 0);
	Journal->RecoveryTargetTime100ns = TargetTime100ns;
	Journal->SuperblockDirty = TRUE;
	status = CdpJournalWriteSuperblockLocked(Journal);
	if (NT_SUCCESS(status))
		status = CdpJournalFlush(Journal);
done:
	Cdp_LOCK_RELEASE(&Journal->Lock);
	return status;
}

NTSTATUS CdpJournalClearRecoveryIntent(_Inout_ PCdp_JOURNAL Journal)
{
	NTSTATUS status;

	if (!Journal)
		return STATUS_INVALID_PARAMETER;
	Cdp_LOCK_ACQUIRE(&Journal->Lock);
	if (!Journal->Mounted)
	{
		status = STATUS_DEVICE_NOT_READY;
		goto done;
	}
	Journal->RecoveryPending = FALSE;
	Journal->RecoveryFsRepairPending = FALSE;
	Journal->RecoveryTargetTime100ns = 0;
	Journal->SuperblockDirty = TRUE;
	status = CdpJournalWriteSuperblockLocked(Journal);
	if (NT_SUCCESS(status))
		status = CdpJournalFlush(Journal);
done:
	Cdp_LOCK_RELEASE(&Journal->Lock);
	return status;
}

NTSTATUS CdpJournalCompleteRecoveryIntent(_Inout_ PCdp_JOURNAL Journal)
{
	NTSTATUS status;

	if (!Journal)
		return STATUS_INVALID_PARAMETER;
	Cdp_LOCK_ACQUIRE(&Journal->Lock);
	if (!Journal->Mounted)
	{
		status = STATUS_DEVICE_NOT_READY;
		goto done;
	}
	Journal->RecoveryPending = FALSE;
	Journal->RecoveryTargetTime100ns = 0;
	Journal->SuperblockDirty = TRUE;
	status = CdpJournalWriteSuperblockLocked(Journal);
	if (NT_SUCCESS(status))
		status = CdpJournalFlush(Journal);
done:
	Cdp_LOCK_RELEASE(&Journal->Lock);
	return status;
}

NTSTATUS CdpJournalClearRecoveryFsRepairPending(_Inout_ PCdp_JOURNAL Journal)
{
	NTSTATUS status;

	if (!Journal)
		return STATUS_INVALID_PARAMETER;
	Cdp_LOCK_ACQUIRE(&Journal->Lock);
	if (!Journal->Mounted)
	{
		status = STATUS_DEVICE_NOT_READY;
		goto done;
	}
	Journal->RecoveryFsRepairPending = FALSE;
	Journal->SuperblockDirty = TRUE;
	status = CdpJournalWriteSuperblockLocked(Journal);
	if (NT_SUCCESS(status))
		status = CdpJournalFlush(Journal);
done:
	Cdp_LOCK_RELEASE(&Journal->Lock);
	return status;
}

NTSTATUS CdpJournalSetRestorePoint(
	_Inout_ PCdp_JOURNAL Journal,
	_In_ UINT64 TargetTime100ns)
{
	NTSTATUS status;
	BOOLEAN oldSet;
	UINT64 oldTime;

	if (!Journal || TargetTime100ns == 0)
		return STATUS_INVALID_PARAMETER;
	Cdp_LOCK_ACQUIRE(&Journal->Lock);
	if (!Journal->Mounted)
	{
		status = STATUS_DEVICE_NOT_READY;
		goto done;
	}
	oldSet = Journal->RestorePointSet;
	oldTime = Journal->RestorePointTime100ns;
	Journal->RestorePointSet = TRUE;
	Journal->RestorePointTime100ns = TargetTime100ns;
	Journal->SuperblockDirty = TRUE;
	status = CdpJournalWriteSuperblockLocked(Journal);
	if (NT_SUCCESS(status))
		status = CdpJournalFlush(Journal);
	if (!NT_SUCCESS(status))
	{
		Journal->RestorePointSet = oldSet;
		Journal->RestorePointTime100ns = oldTime;
		Journal->SuperblockDirty = TRUE;
	}
done:
	Cdp_LOCK_RELEASE(&Journal->Lock);
	return status;
}

NTSTATUS CdpJournalClearRestorePoint(_Inout_ PCdp_JOURNAL Journal)
{
	NTSTATUS status;
	BOOLEAN oldSet;
	UINT64 oldTime;

	if (!Journal)
		return STATUS_INVALID_PARAMETER;
	Cdp_LOCK_ACQUIRE(&Journal->Lock);
	if (!Journal->Mounted)
	{
		status = STATUS_DEVICE_NOT_READY;
		goto done;
	}
	oldSet = Journal->RestorePointSet;
	oldTime = Journal->RestorePointTime100ns;
	Journal->RestorePointSet = FALSE;
	Journal->RestorePointTime100ns = 0;
	Journal->SuperblockDirty = TRUE;
	status = CdpJournalWriteSuperblockLocked(Journal);
	if (NT_SUCCESS(status))
		status = CdpJournalFlush(Journal);
	if (!NT_SUCCESS(status))
	{
		Journal->RestorePointSet = oldSet;
		Journal->RestorePointTime100ns = oldTime;
		Journal->SuperblockDirty = TRUE;
	}
done:
	Cdp_LOCK_RELEASE(&Journal->Lock);
	return status;
}

NTSTATUS CdpJournalSetCredential(
	_Inout_ PCdp_JOURNAL Journal,
	_In_ const Cdp_CREDENTIAL_DESCRIPTOR* Credential)
{
	NTSTATUS status = STATUS_SUCCESS;

	if (!Journal || !Credential ||
		Credential->KdfAlgorithm != Cdp_CREDENTIAL_KDF_PBKDF2_SHA256 ||
		Credential->KdfIterations == 0 || Credential->AuthEpoch == 0)
	{
		return STATUS_INVALID_PARAMETER;
	}
	Cdp_LOCK_ACQUIRE(&Journal->Lock);
	Journal->Credential = *Credential;
	Journal->CredentialConfigured = TRUE;
	if (Journal->Mounted)
	{
		Journal->SuperblockDirty = TRUE;
		status = CdpJournalWriteSuperblockLocked(Journal);
		if (NT_SUCCESS(status))
			status = CdpJournalFlush(Journal);
	}
	Cdp_LOCK_RELEASE(&Journal->Lock);
	return status;
}

BOOLEAN CdpJournalGetCredential(
	_In_ PCdp_JOURNAL Journal,
	_Out_ PCdp_CREDENTIAL_DESCRIPTOR Credential)
{
	BOOLEAN configured;

	if (!Journal || !Credential)
		return FALSE;
	Cdp_LOCK_ACQUIRE(&Journal->Lock);
	configured = Journal->CredentialConfigured;
	if (configured)
		*Credential = Journal->Credential;
	else
		RtlZeroMemory(Credential, sizeof(*Credential));
	Cdp_LOCK_RELEASE(&Journal->Lock);
	return configured;
}

NTSTATUS CdpJournalInvalidate(_Inout_ PCdp_JOURNAL Journal)
{
	PVOID allocationBase = NULL;
	PUCHAR sector;
	NTSTATUS status;

	if (!Journal)
		return STATUS_INVALID_PARAMETER;
	if (!CdpJournalHasBackend(Journal) ||
		(Journal->SectorSize != 512 && Journal->SectorSize != 4096))
	{
		return STATUS_INVALID_PARAMETER;
	}

	Cdp_LOCK_ACQUIRE(&Journal->Lock);
	sector = (PUCHAR)CdpAllocateAligned(Journal,
		Journal->SectorSize,
		&allocationBase);
	if (!sector)
	{
		status = STATUS_INSUFFICIENT_RESOURCES;
		goto cleanup;
	}

	RtlZeroMemory(sector, Journal->SectorSize);
	status = CdpJournalMetadataRawIo(
		Journal,
		IRP_MJ_WRITE,
		0,
		Journal->SectorSize,
		sector);
	if (NT_SUCCESS(status))
	{
		Journal->Mounted = FALSE;
#ifndef Cdp_USERMODE
		Cdp_LOG("[JOURNAL] invalidated superblock\n");
#endif
	}
cleanup:
	if (allocationBase)
		cdpfree(allocationBase);
	Cdp_LOCK_RELEASE(&Journal->Lock);
	return status;
}

NTSTATUS CdpJournalAppend(
	_Inout_ PCdp_JOURNAL Journal,
	_In_ UINT64 VolumeOffset,
	_In_ ULONG DataLength,
	_In_reads_bytes_(DataLength) const VOID* AfterImage,
	_Out_opt_ PCdp_JOURNAL_RECORD WrittenRecord)
{
	return CdpJournalAppendEx(
		Journal,
		VolumeOffset,
		DataLength,
		AfterImage,
		0,
		WrittenRecord);
}

NTSTATUS CdpJournalAppendEx(
	_Inout_ PCdp_JOURNAL Journal,
	_In_ UINT64 VolumeOffset,
	_In_ ULONG DataLength,
	_In_reads_bytes_(DataLength) const VOID* AfterImage,
	_In_ ULONG RecordFlags,
	_Out_opt_ PCdp_JOURNAL_RECORD WrittenRecord)
{
	Cdp_JOURNAL_RECORD_HEADER header;
	Cdp_JOURNAL_RECORD record;
	UINT64 payloadOff;
	UINT64 alignedSize;
	PVOID allocationBase = NULL;
	PUCHAR payloadBuffer = NULL;
	UINT64 writeSeq;
	UINT64 writeTime;
	BOOLEAN rotateHeaderRegion;
	NTSTATUS status = STATUS_SUCCESS;

	if (!Journal->Mounted || !AfterImage || RecordFlags != 0 ||
		DataLength == 0 || DataLength > Cdp_JOURNAL_MAX_RECORD_DATA)
	{
		return STATUS_INVALID_PARAMETER;
	}

	Cdp_LOCK_ACQUIRE(&Journal->Lock);
	if (!Journal->Mounted)
	{
		status = STATUS_DEVICE_NOT_READY;
		goto cleanup;
	}
	if (Journal->CurrentBranchNumber <= 0)
	{
		status = STATUS_INVALID_DEVICE_STATE;
		goto cleanup;
	}

	alignedSize = CdpAlignUp64(DataLength, Journal->SectorSize);

	// Rotate either when header slots are exhausted or when appending this
	// payload would make the current payload span exceed 10% of the journal.
	rotateHeaderRegion =
		Journal->CurrentHeaderCount >= Cdp_JOURNAL_HEADERS_PER_REGION;
	if (!rotateHeaderRegion)
	{
		status = CdpJournalPayloadRotationNeededLocked(
			Journal,
			alignedSize,
			&rotateHeaderRegion);
		if (!NT_SUCCESS(status))
			goto cleanup;
	}
	if (rotateHeaderRegion)
	{
		UINT64 newRegion = 0;
		status = CdpJournalAllocateHeaderRegionLocked(Journal, &newRegion);
		if (!NT_SUCCESS(status))
			goto cleanup;
		status = CdpJournalAppendBranchContinuationLocked(Journal);
		if (!NT_SUCCESS(status))
			goto cleanup;
	}
	if (Journal->NextSequence == MAXUINT64 ||
		Journal->CurrentHeaderRegionStartSequence >
			MAXUINT64 - Journal->CurrentHeaderCount ||
		Journal->CurrentHeaderRegionStartSequence +
			Journal->CurrentHeaderCount != Journal->NextSequence)
	{
		status = STATUS_INTEGER_OVERFLOW;
		goto cleanup;
	}

	// Payload: wrap at partition end and/or drop complete oldest header
	// regions until there is room.
	status = CdpJournalEnsureContiguousLocked(Journal, alignedSize);
	if (!NT_SUCCESS(status))
		goto cleanup;

	payloadOff = Journal->PayloadRegionOff;
	// The queued write IRP remains alive and its MDL remains locked until this
	// synchronous journal write completes. Reuse that mapping when it already
	// satisfies the journal device's transfer alignment. Only the partial-
	// sector/address-misaligned path needs a temporary buffer and zero padding.
	if (alignedSize == DataLength &&
		CdpJournalBufferMeetsIoAlignment(Journal, AfterImage))
	{
		payloadBuffer = (PUCHAR)AfterImage;
	}
	else
	{
		payloadBuffer = (PUCHAR)CdpAllocateAligned(Journal,
			(SIZE_T)alignedSize,
			&allocationBase);
		if (!payloadBuffer)
		{
			status = STATUS_INSUFFICIENT_RESOURCES;
			goto cleanup;
		}
		RtlZeroMemory(payloadBuffer, (SIZE_T)alignedSize);
		RtlCopyMemory(payloadBuffer, AfterImage, DataLength);
	}

	status = CdpJournalRawIo(
		Journal,
		IRP_MJ_WRITE,
		payloadOff,
		(ULONG)alignedSize,
		payloadBuffer);
	if (!NT_SUCCESS(status))
		goto cleanup;

	writeSeq = Journal->NextSequence;
	if (Journal->QueryTime100ns)
	{
		writeTime = Journal->QueryTime100ns(Journal->QueryTimeContext);
	}
	else
	{
#ifdef Cdp_USERMODE
		FILETIME utcFt;
		FILETIME localFt;
		ULARGE_INTEGER u;
		GetSystemTimeAsFileTime(&utcFt);
		if (!FileTimeToLocalFileTime(&utcFt, &localFt))
			localFt = utcFt;
		u.LowPart = localFt.dwLowDateTime;
		u.HighPart = localFt.dwHighDateTime;
		writeTime = u.QuadPart;
#else
		{
			LARGE_INTEGER systemTime;
			LARGE_INTEGER localTime;
			KeQuerySystemTime(&systemTime);
			ExSystemTimeToLocalTime(&systemTime, &localTime);
			writeTime = (UINT64)localTime.QuadPart;
		}
#endif
	}
	if (Journal->Newest100ns != 0 && writeTime <= Journal->Newest100ns)
	{
		if (Journal->Newest100ns == MAXUINT64)
		{
			status = STATUS_INTEGER_OVERFLOW;
			goto cleanup;
		}
		writeTime = Journal->Newest100ns + 1;
	}

	RtlZeroMemory(&header, sizeof(header));
	header.WallClock100ns = writeTime;
	header.VolumeOffset = VolumeOffset;
	header.FileOffset = payloadOff;
	header.DataLength = DataLength;
	header.Sequence = Journal->CurrentHeaderCount | RecordFlags;

	status = CdpJournalWriteHeaderAt(
		Journal,
		Journal->LastHeaderRegionOff,
		Journal->CurrentHeaderCount,
		&header);
	if (!NT_SUCCESS(status))
		goto cleanup;
	CdpBranchTreeAdvanceLatest(
		&Journal->BranchTree,
		writeSeq,
		writeTime,
		Journal->LastHeaderRegionOff,
		Journal->CurrentHeaderCount);

	Journal->CurrentHeaderCount++;
	Journal->PayloadRegionOff = payloadOff + alignedSize;
	Journal->NextSequence = writeSeq + 1;
	if (Journal->PayloadBytesUsed > MAXUINT64 - alignedSize)
	{
		status = STATUS_DISK_CORRUPT_ERROR;
		goto cleanup;
	}
	Journal->TotalRecords++;
	Journal->PayloadBytesUsed += alignedSize;
	CdpJournalAdvanceRecordGenerationLocked(Journal);
	Journal->Newest100ns = writeTime;
	if (Journal->TotalRecords == 1)
	{
		Journal->OldestHeaderRegionOff = Journal->LastHeaderRegionOff;
		Journal->OldestHeaderIndex = Journal->CurrentHeaderCount - 1;
		Journal->Oldest100ns = writeTime;
	}

	if (WrittenRecord)
	{
		RtlZeroMemory(&record, sizeof(record));
		record.WallClock100ns = header.WallClock100ns;
		record.VolumeOffset = header.VolumeOffset;
		record.FileOffset = header.FileOffset;
		record.Sequence = writeSeq;
		record.DataLength = header.DataLength;
		record.Flags = RecordFlags;
		*WrittenRecord = record;
	}

	if (Journal->SuperblockDirty)
	{
		// Make the new region, link, payload and record header durable before
		// publishing the region through the superblock.
		status = CdpJournalFlush(Journal);
		if (NT_SUCCESS(status))
			status = CdpJournalWriteSuperblockLocked(Journal);
		if (NT_SUCCESS(status))
			status = CdpJournalFlush(Journal);
	}
	else
	{
		status = CdpJournalFlush(Journal);
	}

cleanup:
	if (allocationBase)
		cdpfree(allocationBase);
	Cdp_LOCK_RELEASE(&Journal->Lock);
	return status;
}

NTSTATUS CdpJournalQueryTimeRange(
	_Inout_ PCdp_JOURNAL Journal,
	_Out_ PUINT64 OldestTime100ns,
	_Out_ PUINT64 NewestTime100ns)
{
	NTSTATUS status;

	if (!OldestTime100ns || !NewestTime100ns)
		return STATUS_INVALID_PARAMETER;
	*OldestTime100ns = 0;
	*NewestTime100ns = 0;

	Cdp_LOCK_ACQUIRE(&Journal->Lock);
	if (!Journal->Mounted)
		status = STATUS_DEVICE_NOT_READY;
	else if (CdpJournalIsEmptyLocked(Journal))
		status = STATUS_NOT_FOUND;
	else
	{
		*OldestTime100ns = Journal->Oldest100ns;
		*NewestTime100ns = Journal->Newest100ns;
		status = STATUS_SUCCESS;
	}
	Cdp_LOCK_RELEASE(&Journal->Lock);
	return status;
}

NTSTATUS CdpJournalQueryUsage(
	_Inout_ PCdp_JOURNAL Journal,
	_Out_ PUINT64 PartitionBytes,
	_Out_ PUINT64 MetadataBytes,
	_Out_ PUINT64 PayloadBytesUsed,
	_Out_ PUINT64 PayloadBytesFree,
	_Out_ PUINT64 TotalRecords)
{
	UINT64 metadataBytes;
	NTSTATUS status;

	if (!Journal || !PartitionBytes || !MetadataBytes ||
		!PayloadBytesUsed || !PayloadBytesFree || !TotalRecords)
	{
		return STATUS_INVALID_PARAMETER;
	}

	*PartitionBytes = 0;
	*MetadataBytes = 0;
	*PayloadBytesUsed = 0;
	*PayloadBytesFree = 0;
	*TotalRecords = 0;

	Cdp_LOCK_ACQUIRE(&Journal->Lock);
	if (!Journal->Mounted)
	{
		status = STATUS_DEVICE_NOT_READY;
		goto cleanup;
	}
	if (Journal->ActiveHeaderRegionCount == 0 ||
		Journal->ActiveHeaderRegionCount >
		(MAXUINT64 - Journal->SectorSize) / Cdp_JOURNAL_HEADER_REGION_SIZE)
	{
		status = STATUS_DISK_CORRUPT_ERROR;
		goto cleanup;
	}

	metadataBytes = Journal->SectorSize +
		Journal->ActiveHeaderRegionCount * Cdp_JOURNAL_HEADER_REGION_SIZE;
	if (metadataBytes > Journal->PartitionSize ||
		Journal->PayloadBytesUsed > Journal->PartitionSize - metadataBytes)
	{
		status = STATUS_DISK_CORRUPT_ERROR;
		goto cleanup;
	}

	*PartitionBytes = Journal->PartitionSize;
	*MetadataBytes = metadataBytes;
	*PayloadBytesUsed = Journal->PayloadBytesUsed;
	*PayloadBytesFree =
		Journal->PartitionSize - metadataBytes - Journal->PayloadBytesUsed;
	*TotalRecords = Journal->TotalRecords;
	status = STATUS_SUCCESS;

cleanup:
	Cdp_LOCK_RELEASE(&Journal->Lock);
	return status;
}

NTSTATUS CdpJournalGetOldestCompactableRegion(
	_Inout_ PCdp_JOURNAL Journal,
	_Out_ PUINT64 RegionOffset,
	_Out_ PUINT64 FirstSequence,
	_Out_ PUINT64 EndSequence)
{
	Cdp_HEADER_REGION_LINK link;
	Cdp_HEADER_REGION_LINK nextLink;
	ULONG limit;
	NTSTATUS status;

	if (!Journal || !RegionOffset || !FirstSequence || !EndSequence)
		return STATUS_INVALID_PARAMETER;
	*RegionOffset = 0;
	*FirstSequence = 0;
	*EndSequence = 0;

	Cdp_LOCK_ACQUIRE(&Journal->Lock);
	if (!Journal->Mounted)
	{
		status = STATUS_DEVICE_NOT_READY;
		goto cleanup;
	}
	if (Journal->OldestHeaderRegionOff == Journal->LastHeaderRegionOff)
	{
		status = STATUS_NOT_FOUND;
		goto cleanup;
	}
	status = CdpJournalReadRegionLink(
		Journal, Journal->OldestHeaderRegionOff, &link);
	if (!NT_SUCCESS(status) || !CdpJournalRegionLinkValid(Journal, &link))
	{
		status = STATUS_DISK_CORRUPT_ERROR;
		goto cleanup;
	}
	status = CdpJournalGetRegionHeaderLimitLocked(
		Journal,
		Journal->OldestHeaderRegionOff,
		&link,
		&limit,
		&nextLink);
	if (!NT_SUCCESS(status))
		goto cleanup;
	if (Journal->OldestHeaderIndex >= limit ||
		link.StartSequence > MAXUINT64 - Journal->OldestHeaderIndex ||
		nextLink.StartSequence <=
			link.StartSequence + Journal->OldestHeaderIndex)
	{
		status = STATUS_DISK_CORRUPT_ERROR;
		goto cleanup;
	}
	*RegionOffset = Journal->OldestHeaderRegionOff;
	*FirstSequence = link.StartSequence + Journal->OldestHeaderIndex;
	*EndSequence = nextLink.StartSequence;
	status = STATUS_SUCCESS;

cleanup:
	Cdp_LOCK_RELEASE(&Journal->Lock);
	return status;
}

NTSTATUS CdpJournalDeleteOldestRegion(
	_Inout_ PCdp_JOURNAL Journal,
	_In_ UINT64 ExpectedRegionOffset)
{
	NTSTATUS status;

	if (!Journal)
		return STATUS_INVALID_PARAMETER;
	Cdp_LOCK_ACQUIRE(&Journal->Lock);
	if (!Journal->Mounted)
		status = STATUS_DEVICE_NOT_READY;
	else if (Journal->OldestHeaderRegionOff != ExpectedRegionOffset)
		status = STATUS_RETRY;
	else if (Journal->OldestHeaderRegionOff == Journal->LastHeaderRegionOff)
		status = STATUS_NOT_FOUND;
	else
	{
		status = CdpJournalDropOldestRegionLocked(Journal);
		if (NT_SUCCESS(status))
			status = CdpJournalFlush(Journal);
	}
	Cdp_LOCK_RELEASE(&Journal->Lock);
	return status;
}

NTSTATUS CdpJournalDeleteContiguousTombstonedRegions(
	_Inout_ PCdp_JOURNAL Journal,
	_Out_ PULONG DeletedRegionCount)
{
	ULONG deleted = 0;
	NTSTATUS status = STATUS_SUCCESS;

	if (!Journal || !DeletedRegionCount)
		return STATUS_INVALID_PARAMETER;
	*DeletedRegionCount = 0;
	Cdp_LOCK_ACQUIRE(&Journal->Lock);
	if (!Journal->Mounted)
	{
		status = STATUS_DEVICE_NOT_READY;
		goto cleanup;
	}

	while (Journal->OldestHeaderRegionOff != Journal->LastHeaderRegionOff)
	{
		Cdp_HEADER_REGION_LINK link;
		ULONG limit;
		ULONG index;
		BOOLEAN allDeleted = TRUE;

		status = CdpJournalReadRegionLink(
			Journal, Journal->OldestHeaderRegionOff, &link);
		if (!NT_SUCCESS(status) || !CdpJournalRegionLinkValid(Journal, &link))
		{
			status = STATUS_DISK_CORRUPT_ERROR;
			break;
		}
		status = CdpJournalGetRegionHeaderLimitLocked(
			Journal, Journal->OldestHeaderRegionOff, &link, &limit, NULL);
		if (!NT_SUCCESS(status))
			break;
		if (Journal->OldestHeaderIndex >= limit)
		{
			status = STATUS_DISK_CORRUPT_ERROR;
			break;
		}
		for (index = Journal->OldestHeaderIndex; index < limit; ++index)
		{
			Cdp_JOURNAL_RECORD_HEADER header;
			status = CdpJournalReadHeaderAt(
				Journal, Journal->OldestHeaderRegionOff, index, &header);
			if (!NT_SUCCESS(status))
				break;
			if ((header.Sequence & Cdp_JOURNAL_RECORD_INDEX_MASK) != index)
			{
				status = STATUS_DISK_CORRUPT_ERROR;
				break;
			}
			if (!CdpJournalHeaderIsDeleted(&header))
			{
				allDeleted = FALSE;
				break;
			}
		}
		if (!NT_SUCCESS(status) || !allDeleted)
			break;
		status = CdpJournalDropOldestRegionLocked(Journal);
		if (!NT_SUCCESS(status))
			break;
		deleted++;
	}

	if (NT_SUCCESS(status) && deleted != 0)
		status = CdpJournalFlush(Journal);
	*DeletedRegionCount = deleted;

cleanup:
	Cdp_LOCK_RELEASE(&Journal->Lock);
	return status;
}

NTSTATUS CdpJournalPruneUnreachableForCompaction(
	_Inout_ PCdp_JOURNAL Journal,
	_In_ UINT64 FirstSequence,
	_In_ UINT64 EndSequence,
	_In_ UINT64 PreviewTargetSequence,
	_Out_opt_ PBOOLEAN PreviewTargetDeleted)
{
	PCdp_BRANCH_INFO_NODE branch;
	PUCHAR region = NULL;
	UINT64 regionOff;
	ULONG guard = 0;
	BOOLEAN containsInheritancePoint = FALSE;
	BOOLEAN haveBranchToPrune = FALSE;
	BOOLEAN changed = FALSE;
	BOOLEAN previewDeleted = FALSE;
	NTSTATUS status = STATUS_SUCCESS;

	if (!Journal || FirstSequence >= EndSequence)
		return STATUS_INVALID_PARAMETER;
	if (PreviewTargetDeleted)
		*PreviewTargetDeleted = FALSE;

	Cdp_LOCK_ACQUIRE(&Journal->Lock);
	if (!Journal->Mounted || !Journal->BranchTree.Latest)
	{
		status = STATUS_DEVICE_NOT_READY;
		goto cleanup;
	}
	for (branch = Journal->BranchTree.First; branch; branch = branch->Next)
	{
		branch->PrunePending = FALSE;
		if (branch->ParentBranchNumber != 0 &&
			branch->InheritedRecordSequence >= FirstSequence &&
			branch->InheritedRecordSequence < EndSequence)
		{
			containsInheritancePoint = TRUE;
			/* Once source materialization advances through this RR, an
			 * off-path branch can survive only if the current ancestry uses
			 * the exact same inherited record as its baseline. */
			if (!CdpBranchTreeLatestPathLimit(
					&Journal->BranchTree, branch, NULL) &&
				!CdpBranchTreeLatestPathHasInheritancePoint(
					&Journal->BranchTree,
					branch->InheritedRecordSequence))
			{
				CdpBranchTreeMarkPruneSubtree(branch);
				haveBranchToPrune = TRUE;
			}
		}
	}

	// Reaching the first region of a non-current branch discards that branch
	// itself. Its descendants depend on it and are pruned recursively. Merely
	// being a sibling of the latest branch is not enough to delete it early.
	for (branch = Journal->BranchTree.First; branch; branch = branch->Next)
	{
		if (!branch->SyntheticStart &&
			branch->StartRecord.Sequence >= FirstSequence &&
			branch->StartRecord.Sequence < EndSequence &&
			!CdpBranchTreeLatestPathLimit(
				&Journal->BranchTree, branch, NULL))
		{
			CdpBranchTreeMarkPruneSubtree(branch);
			haveBranchToPrune = TRUE;
		}
	}

	// A branch is invalid only when its inherited ordinary record is being
	// discarded (or an ancestor branch was already invalidated). A sibling
	// inheriting from a retained/materialized record remains valid.
	if (containsInheritancePoint || haveBranchToPrune)
	{
		BOOLEAN marked;
		do
		{
			marked = FALSE;
			for (branch = Journal->BranchTree.First;
				branch;
				branch = branch->Next)
			{
				if (branch->PrunePending ||
					branch->ParentBranchNumber == 0)
				{
					continue;
				}
				if ((branch->Parent && branch->Parent->PrunePending) ||
					CdpBranchTreeSequenceDiscardedByCompaction(
						&Journal->BranchTree,
						branch->InheritedRecordSequence,
						FirstSequence,
						EndSequence,
						containsInheritancePoint))
				{
					CdpBranchTreeMarkPruneSubtree(branch);
					haveBranchToPrune = TRUE;
					marked = TRUE;
				}
			}
		} while (marked);
	}
	if (!containsInheritancePoint && !haveBranchToPrune)
		goto cleanup;

	status = CdpJournalGetHeaderScanBufferLocked(Journal, &region);
	if (!NT_SUCCESS(status))
		goto cleanup;
	regionOff = Journal->OldestHeaderRegionOff;
	for (;;)
	{
		Cdp_HEADER_REGION_LINK link;
		ULONG limit;
		ULONG startIndex;
		ULONG index;
		BOOLEAN isLast;

		if (++guard > 100000UL)
		{
			status = STATUS_DISK_CORRUPT_ERROR;
			break;
		}
		status = CdpJournalReadHeaderRegion(Journal, regionOff, region);
		if (!NT_SUCCESS(status))
			break;
		RtlCopyMemory(
			&link,
			region + Cdp_JOURNAL_HEADER_REGION_SIZE -
				Cdp_JOURNAL_HEADER_LINK_SIZE,
			sizeof(link));
		if (!CdpJournalRegionLinkValid(Journal, &link))
		{
			status = STATUS_DISK_CORRUPT_ERROR;
			break;
		}
		isLast = regionOff == Journal->LastHeaderRegionOff;
		status = CdpJournalGetRegionHeaderLimitLocked(
			Journal, regionOff, &link, &limit, NULL);
		if (!NT_SUCCESS(status))
			break;
		startIndex = regionOff == Journal->OldestHeaderRegionOff ?
			Journal->OldestHeaderIndex : 0;

		for (index = startIndex; index < limit; ++index)
		{
			Cdp_JOURNAL_RECORD_HEADER header;
			Cdp_JOURNAL_RECORD_HEADER tombstone;
			PCdp_BRANCH_INFO_NODE recordBranch;
			UINT64 globalSequence;
			BOOLEAN deleteRecord = FALSE;

			RtlCopyMemory(
				&header,
				region + index * sizeof(header),
				sizeof(header));
			if ((header.Sequence & Cdp_JOURNAL_RECORD_INDEX_MASK) != index ||
				link.StartSequence > MAXUINT64 - index)
			{
				status = STATUS_DISK_CORRUPT_ERROR;
				break;
			}
			if (CdpJournalHeaderIsDeleted(&header))
				continue;
			globalSequence = link.StartSequence + index;
			if (CdpJournalHeaderIsBranch(&header))
			{
				Cdp_JOURNAL_BRANCH_RECORD_HEADER branchHeader;
				UINT64 allowedSequence = MAXUINT64;
				BOOLEAN onLatestPath;
				RtlCopyMemory(&branchHeader, &header, sizeof(branchHeader));
				recordBranch = CdpBranchTreeFind(
					&Journal->BranchTree, branchHeader.BranchNumber);
				onLatestPath = CdpBranchTreeLatestPathLimit(
					&Journal->BranchTree, recordBranch, &allowedSequence);
				deleteRecord = recordBranch &&
					(recordBranch->PrunePending ||
					 (CdpJournalBranchHeaderIsContinuation(&branchHeader) &&
					  containsInheritancePoint && onLatestPath &&
					  globalSequence > allowedSequence));
			}
			else
			{
				UINT64 allowedSequence = MAXUINT64;
				BOOLEAN onLatestPath;
				recordBranch = CdpBranchTreeFindBySequence(
					&Journal->BranchTree, globalSequence);
				onLatestPath = CdpBranchTreeLatestPathLimit(
					&Journal->BranchTree,
					recordBranch,
					&allowedSequence);
				deleteRecord = recordBranch &&
					(recordBranch->PrunePending ||
					 (containsInheritancePoint && onLatestPath &&
					  globalSequence > allowedSequence));
			}
			if (!deleteRecord)
				continue;

			RtlZeroMemory(&tombstone, sizeof(tombstone));
			tombstone.Sequence = index | Cdp_JOURNAL_RECORD_FLAG_DELETED;
			status = CdpJournalWriteHeaderAt(
				Journal, regionOff, index, &tombstone);
			if (!NT_SUCCESS(status))
				break;
			changed = TRUE;
			if (globalSequence == PreviewTargetSequence)
				previewDeleted = TRUE;
		}
		if (!NT_SUCCESS(status) || isLast)
			break;
		if (link.NextRegionOff == regionOff)
		{
			status = STATUS_DISK_CORRUPT_ERROR;
			break;
		}
		regionOff = link.NextRegionOff;
	}

	if (changed)
	{
		NTSTATUS rebuildStatus;
		NTSTATUS flushStatus = CdpJournalFlush(Journal);
		CdpJournalAdvanceRecordGenerationLocked(Journal);
		rebuildStatus = CdpJournalRebuildRuntimeLocked(Journal, NULL);
		if (NT_SUCCESS(status) && !NT_SUCCESS(flushStatus))
			status = flushStatus;
		if (NT_SUCCESS(status) && !NT_SUCCESS(rebuildStatus))
			status = rebuildStatus;
	}

cleanup:
	if (!changed)
	{
		for (branch = Journal->BranchTree.First; branch; branch = branch->Next)
			branch->PrunePending = FALSE;
	}
	if (PreviewTargetDeleted)
		*PreviewTargetDeleted = previewDeleted;
	Cdp_LOCK_RELEASE(&Journal->Lock);
	return status;
}

NTSTATUS CdpJournalQueryRecordHeaders(
	_Inout_ PCdp_JOURNAL Journal,
	_In_ UINT64 StartIndex,
	_In_ UINT64 ExpectedGeneration,
	_Out_writes_to_(RecordCapacity, *ReturnedCount) PCdp_JOURNAL_RECORD Records,
	_In_ ULONG RecordCapacity,
	_Out_ PUINT64 TotalRecords,
	_Out_ PUINT64 Generation,
	_Out_ PULONG ReturnedCount)
{
	UINT64 skip;
	UINT64 regionOff;
	ULONG returned = 0;
	ULONG guard = 0;
	NTSTATUS status;

	if (!Journal || !TotalRecords || !Generation || !ReturnedCount ||
		(RecordCapacity != 0 && !Records))
	{
		return STATUS_INVALID_PARAMETER;
	}
	*TotalRecords = 0;
	*Generation = 0;
	*ReturnedCount = 0;

	Cdp_LOCK_ACQUIRE(&Journal->Lock);
	if (!Journal->Mounted)
	{
		status = STATUS_DEVICE_NOT_READY;
		goto cleanup;
	}
	*TotalRecords = Journal->TotalRecords;
	*Generation = Journal->RecordGeneration;
	if (ExpectedGeneration != 0 &&
		ExpectedGeneration != Journal->RecordGeneration)
	{
		status = STATUS_RETRY;
		goto cleanup;
	}
	if (StartIndex > Journal->TotalRecords)
	{
		status = STATUS_INVALID_PARAMETER;
		goto cleanup;
	}
	if (StartIndex == Journal->TotalRecords || RecordCapacity == 0)
	{
		status = STATUS_SUCCESS;
		goto cleanup;
	}

	skip = StartIndex;
	regionOff = Journal->OldestHeaderRegionOff;
	for (;;)
	{
		Cdp_HEADER_REGION_LINK link;
		ULONG limit;
		ULONG index;
		ULONG startIndex;
		BOOLEAN isLast;

		if (++guard > 100000UL)
		{
			status = STATUS_DISK_CORRUPT_ERROR;
			goto cleanup;
		}
		status = CdpJournalReadRegionLink(Journal, regionOff, &link);
		if (!NT_SUCCESS(status) || !CdpJournalRegionLinkValid(Journal, &link))
		{
			status = STATUS_DISK_CORRUPT_ERROR;
			goto cleanup;
		}
		status = CdpJournalGetRegionHeaderLimitLocked(
			Journal, regionOff, &link, &limit, NULL);
		if (!NT_SUCCESS(status))
			goto cleanup;
		isLast = regionOff == Journal->LastHeaderRegionOff;
		startIndex = regionOff == Journal->OldestHeaderRegionOff ?
			Journal->OldestHeaderIndex : 0;

		for (index = startIndex; index < limit; ++index)
		{
			Cdp_JOURNAL_RECORD_HEADER header;
			Cdp_JOURNAL_RECORD record;

			status = CdpJournalReadHeaderAt(
				Journal, regionOff, index, &header);
			if (!NT_SUCCESS(status))
				goto cleanup;
			if ((header.Sequence & Cdp_JOURNAL_RECORD_INDEX_MASK) != index)
			{
				status = STATUS_DISK_CORRUPT_ERROR;
				goto cleanup;
			}
			if (CdpJournalHeaderIsDeleted(&header))
			{
				if ((header.Sequence & Cdp_JOURNAL_RECORD_FLAGS_MASK) !=
					Cdp_JOURNAL_RECORD_FLAG_DELETED || header.DataLength != 0)
				{
					status = STATUS_DISK_CORRUPT_ERROR;
					goto cleanup;
				}
				continue;
			}
			if (skip != 0)
			{
				skip--;
				continue;
			}
			if (CdpJournalHeaderIsBranch(&header))
			{
				Cdp_JOURNAL_BRANCH_RECORD_HEADER branch;
				RtlCopyMemory(&branch, &header, sizeof(branch));
				if ((header.Sequence & Cdp_JOURNAL_RECORD_FLAGS_MASK) !=
						Cdp_JOURNAL_RECORD_FLAG_BRANCH ||
					!CdpJournalBranchHeaderReservedValid(&branch) ||
					branch.BranchNumber <= 0 ||
					branch.ParentBranchNumber < 0 ||
					link.StartSequence > MAXUINT64 - index)
				{
					status = STATUS_DISK_CORRUPT_ERROR;
					goto cleanup;
				}
				RtlZeroMemory(&record, sizeof(record));
				record.WallClock100ns = branch.WallClock100ns;
				record.Sequence = link.StartSequence + index;
				record.Flags = Cdp_JOURNAL_RECORD_FLAG_BRANCH |
					(CdpJournalBranchHeaderIsContinuation(&branch) ?
						Cdp_JOURNAL_RECORD_FLAG_BRANCH_CONTINUATION : 0);
			}
			else
			{
				if (header.DataLength == 0 ||
					(header.Sequence & Cdp_JOURNAL_RECORD_FLAGS_MASK) != 0 ||
					header.DataLength > Cdp_JOURNAL_MAX_RECORD_DATA)
				{
					status = STATUS_DISK_CORRUPT_ERROR;
					goto cleanup;
				}
				status = CdpJournalDecodeRecord(&link, &header, &record);
				if (!NT_SUCCESS(status))
					goto cleanup;
			}
			Records[returned++] = record;
			if (returned == RecordCapacity)
			{
				*ReturnedCount = returned;
				status = STATUS_SUCCESS;
				goto cleanup;
			}
		}
		if (isLast)
			break;
		if (link.NextRegionOff == regionOff)
		{
			status = STATUS_DISK_CORRUPT_ERROR;
			goto cleanup;
		}
		regionOff = link.NextRegionOff;
	}
	if (skip != 0 || returned == 0)
	{
		status = STATUS_DISK_CORRUPT_ERROR;
		goto cleanup;
	}
	*ReturnedCount = returned;
	status = STATUS_SUCCESS;

cleanup:
	Cdp_LOCK_RELEASE(&Journal->Lock);
	return status;
}

NTSTATUS CdpJournalFindRecordLocationBySequence(
	_Inout_ PCdp_JOURNAL Journal,
	_In_ UINT64 RecordSequence,
	_Out_ PUINT64 RecordIndex,
	_Out_ PUINT64 RecordTime100ns,
	_Out_ PUINT64 HeaderRegionOffset,
	_Out_ PULONG HeaderIndex)
{
	PUCHAR region = NULL;
	UINT64 regionOff;
	UINT64 liveIndex = 0;
	ULONG guard = 0;
	NTSTATUS status = STATUS_NOT_FOUND;

	if (!Journal || !RecordIndex || !RecordTime100ns ||
		!HeaderRegionOffset || !HeaderIndex)
	{
		return STATUS_INVALID_PARAMETER;
	}
	*RecordIndex = 0;
	*RecordTime100ns = 0;
	*HeaderRegionOffset = 0;
	*HeaderIndex = 0;

	Cdp_LOCK_ACQUIRE(&Journal->Lock);
	if (!Journal->Mounted)
	{
		status = STATUS_DEVICE_NOT_READY;
		goto cleanup;
	}
	if (CdpJournalIsEmptyLocked(Journal))
		goto cleanup;
	status = CdpJournalGetHeaderScanBufferLocked(Journal, &region);
	if (!NT_SUCCESS(status))
		goto cleanup;

	regionOff = Journal->OldestHeaderRegionOff;
	for (;;)
	{
		Cdp_HEADER_REGION_LINK link;
		ULONG limit;
		ULONG startIndex;
		ULONG index;
		BOOLEAN isLast;

		if (++guard > 100000UL)
		{
			status = STATUS_DISK_CORRUPT_ERROR;
			goto cleanup;
		}
		status = CdpJournalReadHeaderRegion(Journal, regionOff, region);
		if (!NT_SUCCESS(status))
			goto cleanup;
		RtlCopyMemory(
			&link,
			region + Cdp_JOURNAL_HEADER_REGION_SIZE -
				Cdp_JOURNAL_HEADER_LINK_SIZE,
			sizeof(link));
		if (!CdpJournalRegionLinkValid(Journal, &link))
		{
			status = STATUS_DISK_CORRUPT_ERROR;
			goto cleanup;
		}
		status = CdpJournalGetRegionHeaderLimitLocked(
			Journal, regionOff, &link, &limit, NULL);
		if (!NT_SUCCESS(status))
			goto cleanup;
		isLast = regionOff == Journal->LastHeaderRegionOff;
		startIndex = regionOff == Journal->OldestHeaderRegionOff ?
			Journal->OldestHeaderIndex : 0;

		for (index = startIndex; index < limit; ++index)
		{
			Cdp_JOURNAL_RECORD_HEADER header;
			UINT64 globalSequence;

			RtlCopyMemory(
				&header,
				region + index * sizeof(header),
				sizeof(header));
			if ((header.Sequence & Cdp_JOURNAL_RECORD_INDEX_MASK) != index ||
				link.StartSequence > MAXUINT64 - index)
			{
				status = STATUS_DISK_CORRUPT_ERROR;
				goto cleanup;
			}
			if (CdpJournalHeaderIsDeleted(&header))
				continue;
			globalSequence = link.StartSequence + index;
			if (globalSequence == RecordSequence)
			{
				*RecordIndex = liveIndex;
				*RecordTime100ns = header.WallClock100ns;
				*HeaderRegionOffset = regionOff;
				*HeaderIndex = index;
				status = STATUS_SUCCESS;
				goto cleanup;
			}
			liveIndex++;
		}
		if (isLast)
			break;
		if (link.NextRegionOff == regionOff)
		{
			status = STATUS_DISK_CORRUPT_ERROR;
			goto cleanup;
		}
		regionOff = link.NextRegionOff;
	}
	status = STATUS_NOT_FOUND;

cleanup:
	Cdp_LOCK_RELEASE(&Journal->Lock);
	return status;
}

NTSTATUS CdpJournalQueryBranches(
	_Inout_ PCdp_JOURNAL Journal,
	_In_ UINT64 StartIndex,
	_In_ UINT64 ExpectedGeneration,
	_Out_writes_to_(BranchCapacity, *ReturnedCount) PCdp_JOURNAL_BRANCH_TREE_INFO Branches,
	_In_ ULONG BranchCapacity,
	_Out_ PULONG TotalBranches,
	_Out_ PLONG CurrentBranchNumber,
	_Out_ PUINT64 Generation,
	_Out_ PULONG ReturnedCount)
{
	PCdp_BRANCH_INFO_NODE node;
	ULONG skipped = 0;
	ULONG returned = 0;
	NTSTATUS status = STATUS_SUCCESS;

	if (!Journal || !TotalBranches || !CurrentBranchNumber || !Generation ||
		!ReturnedCount || (BranchCapacity != 0 && !Branches))
	{
		return STATUS_INVALID_PARAMETER;
	}
	*TotalBranches = 0;
	*CurrentBranchNumber = 0;
	*Generation = 0;
	*ReturnedCount = 0;

	Cdp_LOCK_ACQUIRE(&Journal->Lock);
	if (!Journal->Mounted)
	{
		status = STATUS_DEVICE_NOT_READY;
		goto cleanup;
	}
	*TotalBranches = Journal->BranchTree.Count;
	*CurrentBranchNumber = Journal->CurrentBranchNumber;
	*Generation = Journal->RecordGeneration;
	if (ExpectedGeneration != 0 && ExpectedGeneration != *Generation)
	{
		status = STATUS_RETRY;
		goto cleanup;
	}
	if (StartIndex > Journal->BranchTree.Count)
	{
		status = STATUS_INVALID_PARAMETER;
		goto cleanup;
	}
	if (StartIndex == Journal->BranchTree.Count || BranchCapacity == 0)
		goto cleanup;

	for (node = Journal->BranchTree.First;
		node && skipped < StartIndex;
		node = node->Next)
	{
		skipped++;
	}
	if (!node)
	{
		status = STATUS_DISK_CORRUPT_ERROR;
		goto cleanup;
	}
	for (; node && returned < BranchCapacity; node = node->Next)
	{
		PCdp_JOURNAL_BRANCH_TREE_INFO info = &Branches[returned++];
		RtlZeroMemory(info, sizeof(*info));
		info->BranchNumber = node->BranchNumber;
		info->ParentBranchNumber = node->ParentBranchNumber;
		info->InheritedRecordSequence = node->InheritedRecordSequence;
		info->CreatedWallClock100ns = node->StartRecord.WallClock100ns;
		info->StartSequence = node->StartRecord.Sequence;
		info->EndSequence = node->EndRecord.Sequence;
		if (node->Latest || node->BranchNumber == Journal->CurrentBranchNumber)
			info->Flags |= Cdp_JOURNAL_BRANCH_INFO_FLAG_CURRENT;
		if (node->SyntheticStart)
			info->Flags |= Cdp_JOURNAL_BRANCH_INFO_FLAG_SYNTHETIC;
	}
	*ReturnedCount = returned;

cleanup:
	Cdp_LOCK_RELEASE(&Journal->Lock);
	return status;
}

VOID CdpPreviewTreeInitialize(_Out_ PCdp_PREVIEW_TREE Tree)
{
	RtlZeroMemory(Tree, sizeof(*Tree));
}

static VOID CdpPreviewTreeFreeNode(_In_opt_ PCdp_PREVIEW_TREE_NODE Node)
{
	if (!Node)
		return;
	CdpPreviewTreeFreeNode(Node->Left);
	CdpPreviewTreeFreeNode(Node->Right);
	cdpfree(Node);
}

VOID CdpPreviewTreeFree(_Inout_ PCdp_PREVIEW_TREE Tree)
{
	if (!Tree)
		return;
	CdpPreviewTreeFreeNode(Tree->Root);
	Tree->Root = NULL;
	Tree->NodeCount = 0;
}

static LONG CdpPreviewTreeNodeHeight(
	_In_opt_ PCdp_PREVIEW_TREE_NODE Node)
{
	return Node ? Node->Height : 0;
}

static VOID CdpPreviewTreeNodeUpdate(
	_Inout_ PCdp_PREVIEW_TREE_NODE Node)
{
	LONG hl = CdpPreviewTreeNodeHeight(Node->Left);
	LONG hr = CdpPreviewTreeNodeHeight(Node->Right);
	UINT64 maxEnd = Node->End;
	UINT64 minValidSequence = Node->Invalid ? MAXUINT64 : Node->Sequence;

	Node->Height = 1 + (hl > hr ? hl : hr);
	if (Node->Left && Node->Left->MaxEnd > maxEnd)
		maxEnd = Node->Left->MaxEnd;
	if (Node->Right && Node->Right->MaxEnd > maxEnd)
		maxEnd = Node->Right->MaxEnd;
	Node->MaxEnd = maxEnd;
	if (Node->Left && Node->Left->MinValidSequence < minValidSequence)
		minValidSequence = Node->Left->MinValidSequence;
	if (Node->Right && Node->Right->MinValidSequence < minValidSequence)
		minValidSequence = Node->Right->MinValidSequence;
	Node->MinValidSequence = minValidSequence;
}

static PCdp_PREVIEW_TREE_NODE CdpPreviewTreeRotateRight(
	_Inout_ PCdp_PREVIEW_TREE_NODE Y)
{
	PCdp_PREVIEW_TREE_NODE x = Y->Left;
	PCdp_PREVIEW_TREE_NODE t2 = x->Right;

	x->Right = Y;
	Y->Left = t2;
	CdpPreviewTreeNodeUpdate(Y);
	CdpPreviewTreeNodeUpdate(x);
	return x;
}

static PCdp_PREVIEW_TREE_NODE CdpPreviewTreeRotateLeft(
	_Inout_ PCdp_PREVIEW_TREE_NODE X)
{
	PCdp_PREVIEW_TREE_NODE y = X->Right;
	PCdp_PREVIEW_TREE_NODE t2 = y->Left;

	y->Left = X;
	X->Right = t2;
	CdpPreviewTreeNodeUpdate(X);
	CdpPreviewTreeNodeUpdate(y);
	return y;
}

static PCdp_PREVIEW_TREE_NODE CdpPreviewTreeAvlInsertNode(
	_In_opt_ PCdp_PREVIEW_TREE_NODE Root,
	_In_ PCdp_PREVIEW_TREE_NODE Node)
{
	LONG balance;

	if (!Root)
		return Node;

	if (Node->Start < Root->Start)
		Root->Left = CdpPreviewTreeAvlInsertNode(Root->Left, Node);
	else
		Root->Right = CdpPreviewTreeAvlInsertNode(Root->Right, Node);

	CdpPreviewTreeNodeUpdate(Root);
	balance = CdpPreviewTreeNodeHeight(Root->Left) -
		CdpPreviewTreeNodeHeight(Root->Right);

	if (balance > 1 && Node->Start < Root->Left->Start)
		return CdpPreviewTreeRotateRight(Root);
	if (balance < -1 && Node->Start >= Root->Right->Start)
		return CdpPreviewTreeRotateLeft(Root);
	if (balance > 1 && Node->Start >= Root->Left->Start)
	{
		Root->Left = CdpPreviewTreeRotateLeft(Root->Left);
		return CdpPreviewTreeRotateRight(Root);
	}
	if (balance < -1 && Node->Start < Root->Right->Start)
	{
		Root->Right = CdpPreviewTreeRotateRight(Root->Right);
		return CdpPreviewTreeRotateLeft(Root);
	}
	return Root;
}

static NTSTATUS CdpPreviewTreeInsertRaw(
	_Inout_ PCdp_PREVIEW_TREE Tree,
	_In_ const Cdp_JOURNAL_RECORD* Record)
{
	PCdp_PREVIEW_TREE_NODE node;

	if (!Tree || !Record)
		return STATUS_INVALID_PARAMETER;
	if (Record->DataLength == 0)
		return STATUS_SUCCESS;

	node = (PCdp_PREVIEW_TREE_NODE)cdpalloc(sizeof(*node));
	if (!node)
		return STATUS_INSUFFICIENT_RESOURCES;

	RtlZeroMemory(node, sizeof(*node));
	node->Start = Record->VolumeOffset;
	node->End = Record->VolumeOffset + Record->DataLength;
	node->MaxEnd = node->End;
	node->FileOffset = Record->FileOffset;
	node->WallClock100ns = Record->WallClock100ns;
	node->DataLength = Record->DataLength;
	node->Sequence = Record->Sequence;
	node->MinValidSequence = Record->Sequence;
	node->Height = 1;
	node->Invalid = FALSE;

	Tree->Root = CdpPreviewTreeAvlInsertNode(Tree->Root, node);
	Tree->NodeCount++;
	return STATUS_SUCCESS;
}

static PCdp_PREVIEW_TREE_NODE CdpPreviewTreeAvlRebalance(
	_Inout_ PCdp_PREVIEW_TREE_NODE Root)
{
	LONG balance;

	if (!Root)
		return NULL;
	CdpPreviewTreeNodeUpdate(Root);
	balance = CdpPreviewTreeNodeHeight(Root->Left) -
		CdpPreviewTreeNodeHeight(Root->Right);
	if (balance > 1)
	{
		if (CdpPreviewTreeNodeHeight(Root->Left->Left) <
			CdpPreviewTreeNodeHeight(Root->Left->Right))
		{
			Root->Left = CdpPreviewTreeRotateLeft(Root->Left);
		}
		return CdpPreviewTreeRotateRight(Root);
	}
	if (balance < -1)
	{
		if (CdpPreviewTreeNodeHeight(Root->Right->Right) <
			CdpPreviewTreeNodeHeight(Root->Right->Left))
		{
			Root->Right = CdpPreviewTreeRotateRight(Root->Right);
		}
		return CdpPreviewTreeRotateLeft(Root);
	}
	return Root;
}

static PCdp_PREVIEW_TREE_NODE CdpPreviewTreeAvlMinimum(
	_In_ PCdp_PREVIEW_TREE_NODE Root)
{
	while (Root->Left)
		Root = Root->Left;
	return Root;
}

static VOID CdpPreviewTreeCopyNodeData(
	_Inout_ PCdp_PREVIEW_TREE_NODE Dest,
	_In_ const PCdp_PREVIEW_TREE_NODE Source)
{
	Dest->Start = Source->Start;
	Dest->End = Source->End;
	Dest->FileOffset = Source->FileOffset;
	Dest->WallClock100ns = Source->WallClock100ns;
	Dest->DataLength = Source->DataLength;
	Dest->Sequence = Source->Sequence;
	Dest->Invalid = Source->Invalid;
}

static PCdp_PREVIEW_TREE_NODE CdpPreviewTreeAvlDeleteByStart(
	_In_opt_ PCdp_PREVIEW_TREE_NODE Root,
	_In_ UINT64 Start,
	_Out_ PBOOLEAN Removed)
{
	if (!Root)
		return NULL;
	if (Start < Root->Start)
	{
		Root->Left = CdpPreviewTreeAvlDeleteByStart(
			Root->Left, Start, Removed);
	}
	else if (Start > Root->Start)
	{
		Root->Right = CdpPreviewTreeAvlDeleteByStart(
			Root->Right, Start, Removed);
	}
	else if (!Root->Left || !Root->Right)
	{
		PCdp_PREVIEW_TREE_NODE child = Root->Left ? Root->Left : Root->Right;
		cdpfree(Root);
		*Removed = TRUE;
		return child;
	}
	else
	{
		PCdp_PREVIEW_TREE_NODE successor =
			CdpPreviewTreeAvlMinimum(Root->Right);
		UINT64 successorStart = successor->Start;

		CdpPreviewTreeCopyNodeData(Root, successor);
		Root->Right = CdpPreviewTreeAvlDeleteByStart(
			Root->Right, successorStart, Removed);
	}
	return CdpPreviewTreeAvlRebalance(Root);
}

static PCdp_PREVIEW_TREE_NODE CdpPreviewTreeFindFirstOverlap(
	_In_opt_ PCdp_PREVIEW_TREE_NODE Node,
	_In_ UINT64 QueryStart,
	_In_ UINT64 QueryEnd)
{
	PCdp_PREVIEW_TREE_NODE hit;

	if (!Node || Node->MaxEnd <= QueryStart)
		return NULL;
	if (Node->Left)
	{
		hit = CdpPreviewTreeFindFirstOverlap(
			Node->Left, QueryStart, QueryEnd);
		if (hit)
			return hit;
	}
	if (!Node->Invalid && Node->Start < QueryEnd && Node->End > QueryStart)
		return Node;
	if (Node->Start < QueryEnd && Node->Right)
		return CdpPreviewTreeFindFirstOverlap(
			Node->Right, QueryStart, QueryEnd);
	return NULL;
}

// Remove a byte range without rebuilding the tree.  Only overlapping nodes
// are deleted; their unaffected left/right fragments are reinserted with the
// original payload offsets.  The caller serializes access to Tree.
static NTSTATUS CdpPreviewTreeRemoveRangeInPlace(
	_Inout_ PCdp_PREVIEW_TREE Tree,
	_In_ UINT64 CutStart,
	_In_ UINT64 CutEnd)
{
	NTSTATUS status;

	if (!Tree || CutStart >= CutEnd)
		return STATUS_INVALID_PARAMETER;

	for (;;)
	{
		PCdp_PREVIEW_TREE_NODE overlap = CdpPreviewTreeFindFirstOverlap(
			Tree->Root, CutStart, CutEnd);
		Cdp_JOURNAL_RECORD saved;
		BOOLEAN removed = FALSE;

		if (!overlap)
			break;
		RtlZeroMemory(&saved, sizeof(saved));
		saved.WallClock100ns = overlap->WallClock100ns;
		saved.VolumeOffset = overlap->Start;
		saved.FileOffset = overlap->FileOffset;
		saved.DataLength = overlap->DataLength;
		saved.Sequence = overlap->Sequence;

		Tree->Root = CdpPreviewTreeAvlDeleteByStart(
			Tree->Root, overlap->Start, &removed);
		if (!removed)
			return STATUS_DISK_CORRUPT_ERROR;
		Tree->NodeCount--;

		if (saved.VolumeOffset < CutStart)
		{
			Cdp_JOURNAL_RECORD left = saved;
			left.DataLength = (ULONG)(CutStart - saved.VolumeOffset);
			status = CdpPreviewTreeInsertRaw(Tree, &left);
			if (!NT_SUCCESS(status))
				return status;
		}
		if (saved.VolumeOffset + saved.DataLength > CutEnd)
		{
			Cdp_JOURNAL_RECORD right = saved;
			right.VolumeOffset = CutEnd;
			right.FileOffset = saved.FileOffset +
				(CutEnd - saved.VolumeOffset);
			right.DataLength = (ULONG)(saved.VolumeOffset +
				saved.DataLength - CutEnd);
			status = CdpPreviewTreeInsertRaw(Tree, &right);
			if (!NT_SUCCESS(status))
				return status;
		}
	}
	return STATUS_SUCCESS;
}

// Build scans journal headers newest-to-oldest.  A newly scanned header is
// therefore an earlier before-image and must replace newer overlapping bytes.
// Remove overlaps in-place, preserve their non-overlapping fragments, then
// insert the complete earlier header.  No header array or tree-wide rebuild.
NTSTATUS CdpPreviewTreeOverlayLatest(
	_Inout_ PCdp_PREVIEW_TREE Tree,
	_In_ const Cdp_JOURNAL_RECORD* Record)
{
	NTSTATUS status;
	UINT64 end;

	if (!Tree || !Record || Record->DataLength == 0 ||
		Record->VolumeOffset > MAXUINT64 - Record->DataLength)
	{
		return STATUS_INVALID_PARAMETER;
	}
	end = Record->VolumeOffset + Record->DataLength;
	status = CdpPreviewTreeRemoveRangeInPlace(
		Tree, Record->VolumeOffset, end);
	if (!NT_SUCCESS(status))
		return status;
	return CdpPreviewTreeInsertRaw(Tree, Record);
}

static ULONG CdpBitmapByteCount(_In_ ULONG BitCount)
{
	return (BitCount + 7UL) / 8UL;
}

static VOID CdpBitmapSetRange(
	_Inout_ PUCHAR Bitmap,
	_In_ ULONG StartBit,
	_In_ ULONG BitCount)
{
	ULONG bit = StartBit;
	ULONG end = StartBit + BitCount;

	while (bit < end && (bit & 7) != 0)
	{
		Bitmap[bit >> 3] |= (UCHAR)(1U << (bit & 7));
		++bit;
	}
	if (bit + 8 <= end)
	{
		ULONG bytes = (end - bit) >> 3;
		RtlFillMemory(Bitmap + (bit >> 3), bytes, 0xFF);
		bit += bytes << 3;
	}
	while (bit < end)
	{
		Bitmap[bit >> 3] |= (UCHAR)(1U << (bit & 7));
		++bit;
	}
}

NTSTATUS CdpPreviewTreeInsert(
	_Inout_ PCdp_PREVIEW_TREE Tree,
	_In_ const Cdp_JOURNAL_RECORD* Record)
{
	UINT64 cursor;
	UINT64 end;
	NTSTATUS status = STATUS_SUCCESS;

	if (!Tree || !Record)
		return STATUS_INVALID_PARAMETER;
	if (Record->DataLength == 0)
		return STATUS_SUCCESS;
	if (Record->VolumeOffset > MAXUINT64 - Record->DataLength)
		return STATUS_INVALID_PARAMETER;

	// Empty tree: raw insert.
	if (!Tree->Root)
		return CdpPreviewTreeInsertRaw(Tree, Record);

	cursor = Record->VolumeOffset;
	end = cursor + Record->DataLength;
	while (cursor < end)
	{
		PCdp_PREVIEW_TREE_NODE overlap = CdpPreviewTreeFindFirstOverlap(
			Tree->Root, cursor, end);
		UINT64 overlapStart;
		UINT64 overlapEnd;

		if (!overlap)
		{
			Cdp_JOURNAL_RECORD frag = *Record;
			frag.VolumeOffset = cursor;
			frag.FileOffset = Record->FileOffset +
				(cursor - Record->VolumeOffset);
			frag.DataLength = (ULONG)(end - cursor);
			status = CdpPreviewTreeInsertRaw(Tree, &frag);
			break;
		}

		// Save scalar values before insertion because AVL rotations may move
		// the overlap node. Existing valid nodes retain priority over Header.
		overlapStart = overlap->Start;
		overlapEnd = overlap->End;
		if (overlapStart > cursor)
		{
			Cdp_JOURNAL_RECORD frag = *Record;
			UINT64 gapEnd = overlapStart < end ? overlapStart : end;
			frag.VolumeOffset = cursor;
			frag.FileOffset = Record->FileOffset +
				(cursor - Record->VolumeOffset);
			frag.DataLength = (ULONG)(gapEnd - cursor);
			status = CdpPreviewTreeInsertRaw(Tree, &frag);
			if (!NT_SUCCESS(status))
				break;
		}
		if (overlapEnd <= cursor)
			return STATUS_DISK_CORRUPT_ERROR;
		cursor = overlapEnd < end ? overlapEnd : end;
	}
	return status;
}

typedef struct _Cdp_PREVIEW_HIT
{
	UINT64 Start;
	UINT64 End;
	UINT64 FileOffset;
	ULONG DataLength;
	UINT64 Sequence;
} Cdp_PREVIEW_HIT, *PCdp_PREVIEW_HIT;

static ULONG CdpPreviewTreeCountOverlaps(
	_In_opt_ PCdp_PREVIEW_TREE_NODE Node,
	_In_ UINT64 QueryStart,
	_In_ UINT64 QueryEnd)
{
	ULONG count = 0;

	if (!Node || Node->MaxEnd <= QueryStart)
		return 0;
	if (Node->Left)
		count += CdpPreviewTreeCountOverlaps(
			Node->Left, QueryStart, QueryEnd);
	if (!Node->Invalid && Node->Start < QueryEnd && Node->End > QueryStart)
		++count;
	if (Node->Start < QueryEnd && Node->Right)
		count += CdpPreviewTreeCountOverlaps(
			Node->Right, QueryStart, QueryEnd);
	return count;
}

static VOID CdpPreviewTreeCollectOverlaps(
	_In_opt_ PCdp_PREVIEW_TREE_NODE Node,
	_In_ UINT64 QueryStart,
	_In_ UINT64 QueryEnd,
	_Inout_updates_(HitCapacity) Cdp_PREVIEW_HIT* Hits,
	_Inout_ PULONG HitCount,
	_In_ ULONG HitCapacity)
{
	if (!Node || *HitCount >= HitCapacity)
		return;
	if (Node->MaxEnd <= QueryStart)
		return;

	if (Node->Left)
		CdpPreviewTreeCollectOverlaps(
			Node->Left,
			QueryStart,
			QueryEnd,
			Hits,
			HitCount,
			HitCapacity);

	if (!Node->Invalid &&
		Node->Start < QueryEnd &&
		Node->End > QueryStart)
	{
		if (*HitCount < HitCapacity)
		{
			Hits[*HitCount].Start = Node->Start;
			Hits[*HitCount].End = Node->End;
			Hits[*HitCount].FileOffset = Node->FileOffset;
			Hits[*HitCount].DataLength = Node->DataLength;
			Hits[*HitCount].Sequence = Node->Sequence;
			(*HitCount)++;
		}
	}

	if (Node->Start < QueryEnd && Node->Right)
		CdpPreviewTreeCollectOverlaps(
			Node->Right,
			QueryStart,
			QueryEnd,
			Hits,
			HitCount,
			HitCapacity);
}

NTSTATUS CdpPreviewTreePunchRange(
	_Inout_ PCdp_PREVIEW_TREE Tree,
	_In_ UINT64 VolumeOffset,
	_In_ ULONG DataLength)
{
	UINT64 cutEnd;

	if (!Tree || DataLength == 0 ||
		VolumeOffset > MAXUINT64 - DataLength)
	{
		return STATUS_INVALID_PARAMETER;
	}
	if (!Tree->Root || Tree->NodeCount == 0)
		return STATUS_SUCCESS;

	cutEnd = VolumeOffset + DataLength;
	return CdpPreviewTreeRemoveRangeInPlace(Tree, VolumeOffset, cutEnd);
}

BOOLEAN CdpPreviewTreeValidateMapping(
	_In_ PCdp_PREVIEW_TREE Tree,
	_In_ UINT64 VolumeOffset,
	_In_ ULONG DataLength,
	_In_ UINT64 ExpectedSequence,
	_In_ UINT64 ExpectedFileOffset,
	_Out_ PUINT64 FirstMismatch,
	_Out_opt_ PUINT64 ActualSequence,
	_Out_opt_ PUINT64 ActualFileOffset)
{
	UINT64 cursor;
	UINT64 end;

	if (!Tree || !FirstMismatch || DataLength == 0 ||
		VolumeOffset > MAXUINT64 - DataLength)
	{
		return FALSE;
	}
	if (ActualSequence)
		*ActualSequence = 0;
	if (ActualFileOffset)
		*ActualFileOffset = 0;
	cursor = VolumeOffset;
	end = VolumeOffset + DataLength;
	while (cursor < end)
	{
		PCdp_PREVIEW_TREE_NODE node = CdpPreviewTreeFindFirstOverlap(
			Tree->Root, cursor, end);
		UINT64 expectedAtCursor = ExpectedFileOffset +
			(cursor - VolumeOffset);
		if (!node || node->Start > cursor || node->End <= cursor ||
			node->Sequence != ExpectedSequence ||
			node->FileOffset + (cursor - node->Start) != expectedAtCursor)
		{
			*FirstMismatch = cursor;
			if (node && node->Start <= cursor && node->End > cursor)
			{
				if (ActualSequence)
					*ActualSequence = node->Sequence;
				if (ActualFileOffset)
					*ActualFileOffset = node->FileOffset +
						(cursor - node->Start);
			}
			return FALSE;
		}
		cursor = node->End < end ? node->End : end;
	}
	return TRUE;
}

static NTSTATUS CdpJournalBuildCurrentBranchTreeInternal(
	_Inout_ PCdp_JOURNAL Journal,
	_In_ BOOLEAN RestrictSequenceRange,
	_In_ UINT64 FirstSequence,
	_In_ UINT64 EndSequence,
	_Out_ PCdp_PREVIEW_TREE Tree)
{
	NTSTATUS status = STATUS_SUCCESS;
	UINT64 regionOff;
	UINT64 regionEndSequence;
	UINT64 allowedSequence = MAXUINT64;
	LONG expectedBranch;
	ULONG guardRegions = 0;
	PUCHAR region = NULL;
	BOOLEAN ancestryComplete = FALSE;

	if (!Journal || !Tree ||
		(RestrictSequenceRange && FirstSequence >= EndSequence))
		return STATUS_INVALID_PARAMETER;
	CdpPreviewTreeInitialize(Tree);

	Cdp_LOCK_ACQUIRE(&Journal->Lock);
	if (!Journal->Mounted || Journal->CurrentBranchNumber <= 0)
	{
		status = STATUS_DEVICE_NOT_READY;
		goto cleanup_locked;
	}
	expectedBranch = Journal->CurrentBranchNumber;
	status = CdpJournalGetHeaderScanBufferLocked(Journal, &region);
	if (!NT_SUCCESS(status))
		goto cleanup_locked;

	regionOff = Journal->LastHeaderRegionOff;
	regionEndSequence = Journal->NextSequence;
	for (;;)
	{
		Cdp_HEADER_REGION_LINK link;
		ULONG limit;
		ULONG startIndex;
		LONG index;
		BOOLEAN isLast;
		BOOLEAN isOldest;

		if (++guardRegions > 100000UL)
		{
			status = STATUS_DISK_CORRUPT_ERROR;
			goto cleanup_locked;
		}
		status = CdpJournalReadHeaderRegion(Journal, regionOff, region);
		if (!NT_SUCCESS(status))
			goto cleanup_locked;
		RtlCopyMemory(
			&link,
			region + Cdp_JOURNAL_HEADER_REGION_SIZE -
				Cdp_JOURNAL_HEADER_LINK_SIZE,
			sizeof(link));
		if (!CdpJournalRegionLinkValid(Journal, &link) ||
			regionEndSequence <= link.StartSequence ||
			regionEndSequence - link.StartSequence >
				Cdp_JOURNAL_HEADERS_PER_REGION)
		{
			status = STATUS_DISK_CORRUPT_ERROR;
			goto cleanup_locked;
		}

		isLast = regionOff == Journal->LastHeaderRegionOff;
		isOldest = regionOff == Journal->OldestHeaderRegionOff;
		limit = (ULONG)(regionEndSequence - link.StartSequence);
		startIndex = isOldest ? Journal->OldestHeaderIndex : 0;
		if ((isLast && limit != Journal->CurrentHeaderCount) ||
			startIndex >= limit)
		{
			status = STATUS_DISK_CORRUPT_ERROR;
			goto cleanup_locked;
		}

		for (index = (LONG)limit - 1; index >= (LONG)startIndex; --index)
		{
			Cdp_JOURNAL_RECORD_HEADER header;
			UINT64 globalSequence;

			if (link.StartSequence > MAXUINT64 - (ULONG)index)
			{
				status = STATUS_INTEGER_OVERFLOW;
				goto cleanup_locked;
			}
			globalSequence = link.StartSequence + (ULONG)index;
			RtlCopyMemory(
				&header,
				region + (ULONG)index * sizeof(header),
				sizeof(header));
			if ((header.Sequence & Cdp_JOURNAL_RECORD_INDEX_MASK) !=
				(ULONG)index)
			{
				status = STATUS_DISK_CORRUPT_ERROR;
				goto cleanup_locked;
			}
			if (CdpJournalHeaderIsDeleted(&header))
			{
				if ((header.Sequence & Cdp_JOURNAL_RECORD_FLAGS_MASK) !=
					Cdp_JOURNAL_RECORD_FLAG_DELETED || header.DataLength != 0)
				{
					status = STATUS_DISK_CORRUPT_ERROR;
					goto cleanup_locked;
				}
				continue;
			}

			if (CdpJournalHeaderIsBranch(&header))
			{
				Cdp_JOURNAL_BRANCH_RECORD_HEADER branch;
				RtlCopyMemory(&branch, &header, sizeof(branch));
				if ((header.Sequence & Cdp_JOURNAL_RECORD_FLAGS_MASK) !=
					Cdp_JOURNAL_RECORD_FLAG_BRANCH ||
					!CdpJournalBranchHeaderReservedValid(&branch) ||
					branch.BranchNumber <= 0 ||
					branch.ParentBranchNumber < 0 ||
					branch.ParentBranchNumber >= branch.BranchNumber)
				{
					status = STATUS_DISK_CORRUPT_ERROR;
					goto cleanup_locked;
				}
				// This merely labels the beginning of a later header region.  It
				// does not create an ancestry boundary; only the FIRST marker does.
				if (CdpJournalBranchHeaderIsContinuation(&branch))
					continue;
				if (branch.BranchNumber != expectedBranch ||
					globalSequence > allowedSequence)
				{
					continue;
				}
				if (branch.ParentBranchNumber == 0)
				{
					if (branch.InheritedRecordSequence != 0)
					{
						status = STATUS_DISK_CORRUPT_ERROR;
						goto cleanup_locked;
					}
					ancestryComplete = TRUE;
					break;
				}
				if (branch.InheritedRecordSequence == 0 ||
					branch.InheritedRecordSequence >= globalSequence)
				{
					status = STATUS_DISK_CORRUPT_ERROR;
					goto cleanup_locked;
				}
				expectedBranch = branch.ParentBranchNumber;
				allowedSequence = branch.InheritedRecordSequence;
				continue;
			}

			if ((header.Sequence & Cdp_JOURNAL_RECORD_FLAGS_MASK) != 0 ||
				header.DataLength == 0 ||
				header.DataLength > Cdp_JOURNAL_MAX_RECORD_DATA ||
				header.VolumeOffset > MAXUINT64 - header.DataLength ||
				header.FileOffset < CdpJournalUsableStart(Journal) ||
				header.FileOffset > CdpJournalUsableEnd(Journal) ||
				header.DataLength >
					CdpJournalUsableEnd(Journal) - header.FileOffset)
			{
				status = STATUS_DISK_CORRUPT_ERROR;
				goto cleanup_locked;
			}
			if (globalSequence <= allowedSequence &&
				(!RestrictSequenceRange ||
					(globalSequence >= FirstSequence &&
					 globalSequence < EndSequence)))
			{
				Cdp_JOURNAL_RECORD record;
				status = CdpJournalDecodeRecord(&link, &header, &record);
				if (!NT_SUCCESS(status))
					goto cleanup_locked;
				status = CdpPreviewTreeInsert(Tree, &record);
				if (!NT_SUCCESS(status))
					goto cleanup_locked;
			}
		}

		if (ancestryComplete)
			break;
		if (isOldest)
		{
			// Compaction materializes everything before the retained oldest
			// region into the source volume, so a reclaimed branch marker is
			// represented by this implicit source-backed ancestry base.
			ancestryComplete = TRUE;
			break;
		}
		if (link.PrevRegionOff == regionOff)
			break;
		regionEndSequence = link.StartSequence;
		regionOff = link.PrevRegionOff;
	}

	if (!ancestryComplete)
		status = STATUS_DISK_CORRUPT_ERROR;

cleanup_locked:
	Cdp_LOCK_RELEASE(&Journal->Lock);
	if (!NT_SUCCESS(status))
		CdpPreviewTreeFree(Tree);
	return status;
}

NTSTATUS CdpJournalBuildCurrentBranchTree(
	_Inout_ PCdp_JOURNAL Journal,
	_Out_ PCdp_PREVIEW_TREE Tree)
{
	return CdpJournalBuildCurrentBranchTreeInternal(
		Journal, FALSE, 0, 0, Tree);
}

NTSTATUS CdpJournalBuildCurrentBranchRegionTree(
	_Inout_ PCdp_JOURNAL Journal,
	_In_ UINT64 FirstSequence,
	_In_ UINT64 EndSequence,
	_Out_ PCdp_PREVIEW_TREE Tree)
{
	return CdpJournalBuildCurrentBranchTreeInternal(
		Journal, TRUE, FirstSequence, EndSequence, Tree);
}

static PCdp_BRANCH_INFO_NODE CdpBranchTreeFindBySequence(
	_In_ PCdp_BRANCH_INFO_TREE BranchTree,
	_In_ UINT64 Sequence)
{
	PCdp_BRANCH_INFO_NODE branch;
	for (branch = BranchTree->Last; branch; branch = branch->Previous)
	{
		if (Sequence >= branch->StartRecord.Sequence &&
			Sequence <= branch->EndRecord.Sequence)
		{
			return branch;
		}
	}
	return NULL;
}

static PCdp_BRANCH_INFO_NODE CdpBranchTreeFindTargetTime(
	_In_ PCdp_BRANCH_INFO_TREE BranchTree,
	_In_ UINT64 TargetTime100ns)
{
	PCdp_BRANCH_INFO_NODE branch;
	PCdp_BRANCH_INFO_NODE target = BranchTree->First;
	for (branch = BranchTree->First; branch; branch = branch->Next)
	{
		if (branch->StartRecord.WallClock100ns > TargetTime100ns)
			break;
		target = branch;
	}
	return target;
}

static LONG CdpBranchPathFind(
	_In_reads_(PathCount) PCdp_BRANCH_INFO_NODE const* Path,
	_In_ ULONG PathCount,
	_In_ PCdp_BRANCH_INFO_NODE Branch)
{
	ULONG index;

	for (index = 0; index < PathCount; ++index)
	{
		if (Path[index] == Branch)
			return (LONG)index;
	}
	return -1;
}

NTSTATUS CdpJournalResolveTargetBranch(
	_Inout_ PCdp_JOURNAL Journal,
	_In_ UINT64 TargetTime100ns,
	_Out_ PLONG BranchNumber,
	_Out_ PUINT64 InheritedRecordSequence)
{
	PCdp_BRANCH_INFO_NODE targetBranch;
	PUCHAR region = NULL;
	UINT64 regionOff;
	ULONG guardRegions = 0;
	NTSTATUS status = STATUS_SUCCESS;

	if (!Journal || !BranchNumber || !InheritedRecordSequence)
		return STATUS_INVALID_PARAMETER;
	*BranchNumber = 0;
	*InheritedRecordSequence = 0;
	Cdp_LOCK_ACQUIRE(&Journal->Lock);
	if (!Journal->Mounted || !Journal->BranchTree.First)
	{
		status = STATUS_DEVICE_NOT_READY;
		goto cleanup;
	}
	if (TargetTime100ns < Journal->Oldest100ns)
		TargetTime100ns = Journal->Oldest100ns;
	targetBranch = CdpBranchTreeFindTargetTime(
		&Journal->BranchTree, TargetTime100ns);
	if (!targetBranch)
	{
		status = STATUS_NOT_FOUND;
		goto cleanup;
	}
	*BranchNumber = targetBranch->BranchNumber;
	*InheritedRecordSequence = targetBranch->StartRecord.Sequence;
	status = CdpJournalGetHeaderScanBufferLocked(Journal, &region);
	if (!NT_SUCCESS(status))
		goto cleanup;

	regionOff = Journal->OldestHeaderRegionOff;
	for (;;)
	{
		Cdp_HEADER_REGION_LINK link;
		ULONG limit;
		ULONG startIndex;
		ULONG index;
		BOOLEAN isLast;

		if (++guardRegions > 100000UL)
		{
			status = STATUS_DISK_CORRUPT_ERROR;
			goto cleanup;
		}
		status = CdpJournalReadHeaderRegion(Journal, regionOff, region);
		if (!NT_SUCCESS(status))
			goto cleanup;
		RtlCopyMemory(
			&link,
			region + Cdp_JOURNAL_HEADER_REGION_SIZE -
				Cdp_JOURNAL_HEADER_LINK_SIZE,
			sizeof(link));
		if (!CdpJournalRegionLinkValid(Journal, &link))
		{
			status = STATUS_DISK_CORRUPT_ERROR;
			goto cleanup;
		}
		isLast = regionOff == Journal->LastHeaderRegionOff;
		status = CdpJournalGetRegionHeaderLimitLocked(
			Journal, regionOff, &link, &limit, NULL);
		if (!NT_SUCCESS(status))
			goto cleanup;
		startIndex = regionOff == Journal->OldestHeaderRegionOff ?
			Journal->OldestHeaderIndex : 0;
		for (index = startIndex; index < limit; ++index)
		{
			Cdp_JOURNAL_RECORD_HEADER header;
			Cdp_JOURNAL_RECORD record;
			PCdp_BRANCH_INFO_NODE recordBranch;

			RtlCopyMemory(
				&header, region + index * sizeof(header), sizeof(header));
			if ((header.Sequence & Cdp_JOURNAL_RECORD_INDEX_MASK) != index)
			{
				status = STATUS_DISK_CORRUPT_ERROR;
				goto cleanup;
			}
			if (CdpJournalHeaderIsDeleted(&header))
				continue;
			if (CdpJournalHeaderIsBranch(&header))
				continue;
			status = CdpJournalDecodeRecord(&link, &header, &record);
			if (!NT_SUCCESS(status))
				goto cleanup;
			recordBranch = CdpBranchTreeFindBySequence(
				&Journal->BranchTree, record.Sequence);
			if (recordBranch == targetBranch &&
				record.WallClock100ns <= TargetTime100ns)
			{
				*InheritedRecordSequence = record.Sequence;
			}
		}
		if (isLast)
			break;
		regionOff = link.NextRegionOff;
	}

cleanup:
	Cdp_LOCK_RELEASE(&Journal->Lock);
	return status;
}

NTSTATUS CdpJournalBuildPreviewTree(
	_Inout_ PCdp_JOURNAL Journal,
	_In_ UINT64 TargetTime100ns,
	_In_ UINT64 MaxSequence,
	_In_ BOOLEAN IncludeTargetTime,
	_Out_ PCdp_PREVIEW_TREE Tree,
	_Out_opt_ PUINT64 TargetRecordSequence)
{
	NTSTATUS status = STATUS_SUCCESS;
	PCdp_BRANCH_INFO_NODE targetBranch;
	UINT64 regionOff;
	ULONG guardRegions = 0;
	PUCHAR region = NULL;
	PCdp_BRANCH_INFO_NODE* branchPath = NULL;
	ULONG branchPathCount = 0;
	PCdp_BRANCH_INFO_NODE branch;
	BOOLEAN targetReached = FALSE;

	if (!Journal || !Tree || MaxSequence == 0)
		return STATUS_INVALID_PARAMETER;
	CdpPreviewTreeInitialize(Tree);
	if (TargetRecordSequence)
		*TargetRecordSequence = 0;

	Cdp_LOCK_ACQUIRE(&Journal->Lock);
	if (!Journal->Mounted)
	{
		status = STATUS_DEVICE_NOT_READY;
		goto cleanup_locked;
	}
	if (CdpJournalIsEmptyLocked(Journal) || !Journal->BranchTree.First)
		goto cleanup_locked;
	if (TargetTime100ns < Journal->Oldest100ns)
		TargetTime100ns = Journal->Oldest100ns;
	targetBranch = CdpBranchTreeFindTargetTime(
		&Journal->BranchTree, TargetTime100ns);
	if (!targetBranch)
	{
		status = STATUS_NOT_FOUND;
		goto cleanup_locked;
	}
	if (TargetRecordSequence)
		*TargetRecordSequence = targetBranch->StartRecord.Sequence;

	/* Materialize the one valid ancestry path explicitly.  Path[0] is the
	 * oldest retained ancestor and Path[count-1] is the branch containing the
	 * target time.  Siblings never participate in the target view. */
	branchPath = (PCdp_BRANCH_INFO_NODE*)cdpalloc(
		sizeof(*branchPath) * Journal->BranchTree.Count);
	if (!branchPath)
	{
		status = STATUS_INSUFFICIENT_RESOURCES;
		goto cleanup_locked;
	}
	for (branch = targetBranch; branch; branch = branch->Parent)
	{
		if (branchPathCount >= Journal->BranchTree.Count)
		{
			status = STATUS_DISK_CORRUPT_ERROR;
			goto cleanup_locked;
		}
		branchPath[branchPathCount++] = branch;
	}
	if (branchPathCount == 0)
	{
		status = STATUS_DISK_CORRUPT_ERROR;
		goto cleanup_locked;
	}
	{
		ULONG left = 0;
		ULONG right = branchPathCount - 1;
		while (left < right)
		{
			PCdp_BRANCH_INFO_NODE swap = branchPath[left];
			branchPath[left++] = branchPath[right];
			branchPath[right--] = swap;
		}
	}

	status = CdpJournalGetHeaderScanBufferLocked(Journal, &region);
	if (!NT_SUCCESS(status))
		goto cleanup_locked;

	// One chronological pass over the explicit root-to-target branch path.
	// Each ancestor stops at the next child's inheritance point.  The target
	// branch stops at the requested time.  Later after-images replace earlier
	// coverage, so the resulting tree is the exact target view.
	regionOff = Journal->OldestHeaderRegionOff;
	for (;;)
	{
		Cdp_HEADER_REGION_LINK link;
		ULONG limit;
		ULONG startIndex;
		ULONG index;
		BOOLEAN isLast;

		if (++guardRegions > 100000UL)
		{
			status = STATUS_DISK_CORRUPT_ERROR;
			goto cleanup_locked;
		}
		status = CdpJournalReadHeaderRegion(Journal, regionOff, region);
		if (!NT_SUCCESS(status))
			goto cleanup_locked;
		RtlCopyMemory(
			&link,
			region + Cdp_JOURNAL_HEADER_REGION_SIZE -
				Cdp_JOURNAL_HEADER_LINK_SIZE,
			sizeof(link));
		if (!CdpJournalRegionLinkValid(Journal, &link))
		{
			status = STATUS_DISK_CORRUPT_ERROR;
			goto cleanup_locked;
		}
		isLast = regionOff == Journal->LastHeaderRegionOff;
		status = CdpJournalGetRegionHeaderLimitLocked(
			Journal, regionOff, &link, &limit, NULL);
		if (!NT_SUCCESS(status))
			goto cleanup_locked;
		startIndex = regionOff == Journal->OldestHeaderRegionOff ?
			Journal->OldestHeaderIndex : 0;

		for (index = startIndex; index < limit; ++index)
		{
			Cdp_JOURNAL_RECORD_HEADER header;
			Cdp_JOURNAL_RECORD record;
			PCdp_BRANCH_INFO_NODE recordBranch;
			LONG pathIndex;
			UINT64 allowedSequence;

			RtlCopyMemory(
				&header,
				region + index * sizeof(header),
				sizeof(header));
			if ((header.Sequence & Cdp_JOURNAL_RECORD_INDEX_MASK) != index)
			{
				status = STATUS_DISK_CORRUPT_ERROR;
				goto cleanup_locked;
			}
			if (CdpJournalHeaderIsDeleted(&header))
				continue;
			if (header.WallClock100ns > TargetTime100ns)
			{
				targetReached = TRUE;
				break;
			}
			if (CdpJournalHeaderIsBranch(&header))
				continue;
			if ((header.Sequence & Cdp_JOURNAL_RECORD_FLAGS_MASK) != 0 ||
				header.DataLength == 0 ||
				header.DataLength > Cdp_JOURNAL_MAX_RECORD_DATA ||
				header.VolumeOffset > MAXUINT64 - header.DataLength)
			{
				status = STATUS_DISK_CORRUPT_ERROR;
				goto cleanup_locked;
			}
			status = CdpJournalDecodeRecord(&link, &header, &record);
			if (!NT_SUCCESS(status))
				goto cleanup_locked;
			if (record.Sequence >= MaxSequence)
				continue;
			recordBranch = CdpBranchTreeFindBySequence(
				&Journal->BranchTree, record.Sequence);
			pathIndex = recordBranch ? CdpBranchPathFind(
				branchPath, branchPathCount, recordBranch) : -1;
			if (pathIndex < 0)
			{
				continue;
			}
			allowedSequence = ((ULONG)pathIndex + 1 < branchPathCount) ?
				branchPath[pathIndex + 1]->InheritedRecordSequence : MAXUINT64;
			if (record.Sequence > allowedSequence)
				continue;
			if (recordBranch == targetBranch &&
				(IncludeTargetTime ?
					record.WallClock100ns > TargetTime100ns :
					record.WallClock100ns >= TargetTime100ns))
			{
				targetReached = TRUE;
				break;
			}
			status = CdpPreviewTreeOverlayLatest(Tree, &record);
			if (!NT_SUCCESS(status))
				goto cleanup_locked;
			if (TargetRecordSequence && recordBranch == targetBranch)
				*TargetRecordSequence = record.Sequence;
		}
		if (targetReached)
			break;

		if (isLast)
			break;
		if (link.NextRegionOff == regionOff)
		{
			status = STATUS_DISK_CORRUPT_ERROR;
			goto cleanup_locked;
		}
		regionOff = link.NextRegionOff;
	}

cleanup_locked:
	if (branchPath)
		cdpfree(branchPath);
	Cdp_LOCK_RELEASE(&Journal->Lock);
	if (!NT_SUCCESS(status))
		CdpPreviewTreeFree(Tree);
	return status;
}

NTSTATUS CdpJournalApplyPreviewTree(
	_Inout_ PCdp_JOURNAL Journal,
	_In_ PCdp_PREVIEW_TREE Tree,
	_Inout_ Cdp_LOCK* TreeLock,
	_In_ UINT64 VolumeOffset,
	_In_ ULONG DataLength,
	_Out_writes_bytes_(DataLength) PVOID Buffer,
	_Out_writes_bytes_((DataLength + 7) / 8) PUCHAR CoveredMask,
	_Out_ PULONG CoveredCount)
{
	NTSTATUS status = STATUS_SUCCESS;
	PCdp_PREVIEW_HIT hits = NULL;
	ULONG hitCount = 0;
	ULONG hitCapacity;
	ULONG covered = 0;
	ULONG i;
	BOOLEAN treeLocked = FALSE;

	if (!Tree || !TreeLock || !Buffer || !CoveredMask || !CoveredCount ||
		DataLength == 0 ||
		VolumeOffset > MAXUINT64 - DataLength)
	{
		return STATUS_INVALID_PARAMETER;
	}

	*CoveredCount = 0;
	RtlZeroMemory(CoveredMask, CdpBitmapByteCount(DataLength));

	Cdp_LOCK_ACQUIRE(TreeLock);
	treeLocked = TRUE;
	if (!Tree->Root || Tree->NodeCount == 0)
	{
		goto cleanup;
	}

	if (!Journal->Mounted)
	{
		status = STATUS_DEVICE_NOT_READY;
		goto cleanup;
	}

	hitCapacity = CdpPreviewTreeCountOverlaps(
		Tree->Root,
		VolumeOffset,
		VolumeOffset + DataLength);
	if (hitCapacity == 0)
	{
		goto cleanup;
	}
	hits = (PCdp_PREVIEW_HIT)cdpalloc(sizeof(Cdp_PREVIEW_HIT) * hitCapacity);
	if (!hits)
	{
		status = STATUS_INSUFFICIENT_RESOURCES;
		goto cleanup;
	}

	Cdp_JOURNAL_DIAG(
		"collect begin volumeOff=%llu len=%lu nodes=%lu\n",
		VolumeOffset,
		DataLength,
		Tree->NodeCount);
	CdpPreviewTreeCollectOverlaps(
		Tree->Root,
		VolumeOffset,
		VolumeOffset + DataLength,
		hits,
		&hitCount,
		hitCapacity);
	// Hits contain value copies.  The tree may now be modified or replaced
	// without keeping a mutex held across journal I/O.
	Cdp_LOCK_RELEASE(TreeLock);
	treeLocked = FALSE;
	Cdp_JOURNAL_DIAG(
		"collect end volumeOff=%llu len=%lu hits=%lu\n",
		VolumeOffset,
		DataLength,
		hitCount);
	for (i = 0; i < hitCount && covered < DataLength; ++i)
	{
		PCdp_PREVIEW_HIT node = &hits[i];
		PVOID payloadBase = NULL;
		PUCHAR payload;
		UINT64 alignedSize;
		UINT64 overlapStart;
		UINT64 overlapEnd;
		ULONG outputIndex;
		ULONG copyLength;

		Cdp_JOURNAL_DIAG(
			"hit begin index=%lu/%lu seq=%llu node=[%llu,%llu) "
			"dataLen=%lu fileOff=%llu\n",
			i,
			hitCount,
			node->Sequence,
			node->Start,
			node->End,
			node->DataLength,
			node->FileOffset);
		alignedSize = CdpAlignUp64(node->DataLength, Journal->SectorSize);
		payload = (PUCHAR)CdpAllocateAligned(Journal,
			(SIZE_T)alignedSize,
			&payloadBase);
		if (!payload)
		{
			status = STATUS_INSUFFICIENT_RESOURCES;
			goto cleanup;
		}

		Cdp_JOURNAL_DIAG(
			"payload read begin index=%lu seq=%llu fileOff=%llu len=%lu\n",
			i,
			node->Sequence,
			node->FileOffset,
			(ULONG)alignedSize);
		status = CdpJournalRawIo(
			Journal,
			IRP_MJ_READ,
			node->FileOffset,
			(ULONG)alignedSize,
			payload);
		Cdp_JOURNAL_DIAG(
			"payload read end index=%lu seq=%llu status=0x%08X\n",
			i,
			node->Sequence,
			status);
		if (!NT_SUCCESS(status))
		{
			cdpfree(payloadBase);
			goto cleanup;
		}

		overlapStart = node->Start > VolumeOffset ? node->Start : VolumeOffset;
		overlapEnd = node->End < (VolumeOffset + DataLength) ?
			node->End : (VolumeOffset + DataLength);

		outputIndex = (ULONG)(overlapStart - VolumeOffset);
		copyLength = (ULONG)(overlapEnd - overlapStart);
		RtlCopyMemory(
			(PUCHAR)Buffer + outputIndex,
			payload + (ULONG)(overlapStart - node->Start),
			copyLength);
		CdpBitmapSetRange(CoveredMask, outputIndex, copyLength);
		covered += copyLength;
		cdpfree(payloadBase);
		Cdp_JOURNAL_DIAG(
			"hit end index=%lu seq=%llu covered=%lu\n",
			i,
			node->Sequence,
			covered);
	}

	*CoveredCount = covered;
	Cdp_JOURNAL_DIAG(
		"apply end volumeOff=%llu len=%lu hits=%lu covered=%lu "
		"status=0x%08X\n",
		VolumeOffset,
		DataLength,
		hitCount,
		covered,
		status);

cleanup:
	if (treeLocked)
		Cdp_LOCK_RELEASE(TreeLock);
	if (!NT_SUCCESS(status))
	{
		Cdp_JOURNAL_DIAG(
			"apply failed volumeOff=%llu len=%lu hits=%lu covered=%lu "
			"status=0x%08X\n",
			VolumeOffset,
			DataLength,
			hitCount,
			covered,
			status);
	}
	if (hits)
		cdpfree(hits);
	return status;
}

NTSTATUS CdpJournalReadPayload(
	_Inout_ PCdp_JOURNAL Journal,
	_In_ UINT64 FileOffset,
	_In_ ULONG DataLength,
	_Out_writes_bytes_(DataLength) PVOID Buffer)
{
	UINT64 currentOffset;
	ULONG remaining;
	PUCHAR output;
	NTSTATUS status = STATUS_SUCCESS;

	if (!Journal || !Journal->Mounted || !Buffer || DataLength == 0)
	{
		return STATUS_INVALID_PARAMETER;
	}

	// Coalesced preview/recovery nodes may span many adjacent records.  Keep
	// each raw I/O at the per-record maximum, but assemble the full logical
	// payload for the caller.
	currentOffset = FileOffset;
	remaining = DataLength;
	output = (PUCHAR)Buffer;
	while (remaining != 0)
	{
		ULONG chunk = remaining > Cdp_JOURNAL_MAX_RECORD_DATA ?
			Cdp_JOURNAL_MAX_RECORD_DATA : remaining;
		UINT64 alignedSize = CdpAlignUp64(chunk, Journal->SectorSize);
		PVOID allocationBase = NULL;
		PUCHAR payload = (PUCHAR)CdpAllocateAligned(
			Journal,
			(SIZE_T)alignedSize,
			&allocationBase);

		if (!payload)
			return STATUS_INSUFFICIENT_RESOURCES;

		status = CdpJournalRawIo(
			Journal,
			IRP_MJ_READ,
			currentOffset,
			(ULONG)alignedSize,
			payload);
		if (NT_SUCCESS(status))
			RtlCopyMemory(output, payload, chunk);
		cdpfree(allocationBase);
		if (!NT_SUCCESS(status))
			break;

		currentOffset += chunk;
		output += chunk;
		remaining -= chunk;
	}
	return status;
}

VOID CdpJournalClose(_Inout_ PCdp_JOURNAL Journal)
{
	Cdp_LOCK_ACQUIRE(&Journal->Lock);
	if (Journal->Mounted)
		(VOID)CdpJournalWriteSuperblockLocked(Journal);
	Journal->Mounted = FALSE;
	Journal->TargetDevice = NULL;
	Journal->Store = NULL;
	if (Journal->HeaderScanAllocationBase)
		cdpfree(Journal->HeaderScanAllocationBase);
	Journal->HeaderScanAllocationBase = NULL;
	Journal->HeaderScanBuffer = NULL;
	if (Journal->HeaderWriteAllocationBase)
		cdpfree(Journal->HeaderWriteAllocationBase);
	Journal->HeaderWriteAllocationBase = NULL;
	Journal->HeaderWriteBuffer = NULL;
	Journal->HeaderWriteCacheValid = FALSE;
	Journal->HeaderWriteCacheDirty = FALSE;
	CdpBranchTreeFree(&Journal->BranchTree);
	Cdp_LOCK_RELEASE(&Journal->Lock);
	Cdp_LOCK_DELETE(&Journal->Lock);
}
