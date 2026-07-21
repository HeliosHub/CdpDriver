#pragma once

#ifdef QH_USERMODE
#include "qh_portable.h"
#include "qh_store.h"
#else
#include "..\SysRestoreCore\include\qh_portable.h"
#include "..\SysRestoreCore\include\qh_store.h"
#endif

#define QH_JOURNAL_MAGIC            0x4C4E4A51UL /* 'QJNL' */
#define QH_JOURNAL_VERSION          6UL
#define QH_JOURNAL_MAX_RECORD_DATA  (2UL * 1024UL * 1024UL)
#define QH_JOURNAL_HEADER_REGION_SIZE (2UL * 1024UL * 1024UL)
#define QH_JOURNAL_HEADER_LINK_SIZE 32UL
#define QH_JOURNAL_HEADER_LINK_MARK 0xFFFFFFFFFFFFFFFFULL

#pragma pack(push, 1)

// Per-record header stored in 2MB header regions (32 bytes).
typedef struct _QH_JOURNAL_RECORD_HEADER
{
	UINT64 WallClock100ns; // 8
	UINT64 VolumeOffset;   // 8  source volume byte offset
	UINT64 FileOffset;     // 8  payload offset inside CDP partition
	ULONG DataLength;      // 4
	ULONG Sequence;        // 4
} QH_JOURNAL_RECORD_HEADER, *PQH_JOURNAL_RECORD_HEADER;

C_ASSERT(sizeof(QH_JOURNAL_RECORD_HEADER) == 32);

// Last 32 bytes of each 2MB header region.
typedef struct _QH_HEADER_REGION_LINK
{
	UINT64 Marker;         // must be 0xFF..
	UINT64 PrevRegionOff;  // previous header region (may be self)
	UINT64 NextRegionOff;  // next header region (may be self)
	UINT64 Reserved;
} QH_HEADER_REGION_LINK, *PQH_HEADER_REGION_LINK;

C_ASSERT(sizeof(QH_HEADER_REGION_LINK) == 32);

// On-disk layout (v6): alternating header region + its payload area
//   [Superblock]
//   [HeaderRegion0 2MB][Payload0 ...]
//   [HeaderRegion1 2MB][Payload1 ...]
//   ...
//   [Superblock backup]
typedef struct _QH_JOURNAL_SUPERBLOCK
{
	ULONG Magic;
	ULONG Version;
	ULONG SectorSize;
	ULONG Flags;
	UINT64 PartitionSize;
	UINT64 LastHeaderRegionOff; // newest 2MB header region
	UINT64 PayloadRegionOff;    // next payload write offset (after last header)
	ULONG Crc32c;
} QH_JOURNAL_SUPERBLOCK, *PQH_JOURNAL_SUPERBLOCK;

#pragma pack(pop)

#define QH_JOURNAL_HEADERS_PER_REGION \
	((QH_JOURNAL_HEADER_REGION_SIZE - QH_JOURNAL_HEADER_LINK_SIZE) / \
		sizeof(QH_JOURNAL_RECORD_HEADER))

typedef struct _QH_JOURNAL
{
	BOOLEAN Mounted;
	ULONG SectorSize;
	UINT64 PartitionSize;

	UINT64 LastHeaderRegionOff;
	UINT64 PayloadRegionOff;

	UINT64 OldestHeaderRegionOff;
	ULONG OldestHeaderIndex;
	ULONG CurrentHeaderCount;
	// Largest successful transfer used to initialize a 2MB header region.
	// Starts at 2MB and is halved on write failure, down to one sector.
	ULONG HeaderRegionWriteChunk;
	ULONG NextSequence;
	UINT64 TotalRecords;
	UINT64 Oldest100ns;
	UINT64 Newest100ns;
	GUID SourceVolumeGuid;
#ifndef QH_USERMODE
	PDEVICE_OBJECT TargetDevice;
#else
	PVOID TargetDevice;
#endif
	PVOID RawDiskHandle; // kernel HANDLE; physical disk backend when non-NULL
	UINT64 TargetBaseOffset; // partition start on RawDiskHandle
	PQH_STORE Store; // if set, RawIo uses store instead of TargetDevice
	QH_QUERY_TIME_100NS QueryTime100ns;
	PVOID QueryTimeContext;
	QH_LOCK Lock;
} QH_JOURNAL, *PQH_JOURNAL;

typedef struct _QH_PREVIEW_TREE_NODE
{
	UINT64 Start;
	UINT64 End;
	UINT64 MaxEnd; // subtree max End (interval-tree prune)
	UINT64 FileOffset;
	UINT64 WallClock100ns;
	ULONG DataLength;
	ULONG Sequence;
	LONG Height; // AVL height
	BOOLEAN Invalid; // Recovery: punched by a newer live write; skip apply/writeback
	struct _QH_PREVIEW_TREE_NODE* Left;
	struct _QH_PREVIEW_TREE_NODE* Right;
} QH_PREVIEW_TREE_NODE, *PQH_PREVIEW_TREE_NODE;

typedef struct _QH_PREVIEW_TREE
{
	PQH_PREVIEW_TREE_NODE Root;
	ULONG NodeCount;
} QH_PREVIEW_TREE, *PQH_PREVIEW_TREE;

VOID QHJournalInitialize(
	_Out_ PQH_JOURNAL Journal,
	_In_opt_ PVOID TargetDevice,
	_In_opt_ PVOID RawDiskHandle,
	_In_ UINT64 TargetBaseOffset,
	_In_ UINT64 PartitionSize,
	_In_ ULONG SectorSize,
	_In_ const GUID* SourceVolumeGuid);

VOID QHJournalInitializeWithStore(
	_Out_ PQH_JOURNAL Journal,
	_In_ PQH_STORE Store,
	_In_ const GUID* SourceVolumeGuid,
	_In_opt_ QH_QUERY_TIME_100NS QueryTime100ns,
	_In_opt_ PVOID QueryTimeContext);

NTSTATUS QHJournalFormat(_Inout_ PQH_JOURNAL Journal);

NTSTATUS QHJournalMount(_Inout_ PQH_JOURNAL Journal);

NTSTATUS QHJournalAppend(
	_Inout_ PQH_JOURNAL Journal,
	_In_ UINT64 VolumeOffset,
	_In_ ULONG DataLength,
	_In_reads_bytes_(DataLength) const VOID* BeforeImage,
	_Out_opt_ PQH_JOURNAL_RECORD_HEADER WrittenHeader);

NTSTATUS QHJournalQueryTimeRange(
	_Inout_ PQH_JOURNAL Journal,
	_Out_ PUINT64 OldestTime100ns,
	_Out_ PUINT64 NewestTime100ns);

NTSTATUS QHJournalBuildPreviewTree(
	_Inout_ PQH_JOURNAL Journal,
	_In_ UINT64 TargetTime100ns,
	_In_ ULONG MaxSequence,
	_In_ BOOLEAN IncludeTargetTime,
	_Out_ PQH_PREVIEW_TREE Tree);

NTSTATUS QHJournalApplyPreviewTree(
	_Inout_ PQH_JOURNAL Journal,
	_In_ PQH_PREVIEW_TREE Tree,
	_In_ UINT64 VolumeOffset,
	_In_ ULONG DataLength,
	_Out_writes_bytes_(DataLength) PVOID Buffer,
	_Out_writes_bytes_(DataLength) PUCHAR CoveredMask,
	_Out_ PULONG CoveredCount);

// Read a single record payload from the journal (FileOffset from record header).
NTSTATUS QHJournalReadPayload(
	_Inout_ PQH_JOURNAL Journal,
	_In_ UINT64 FileOffset,
	_In_ ULONG DataLength,
	_Out_writes_bytes_(DataLength) PVOID Buffer);

VOID QHPreviewTreeInitialize(_Out_ PQH_PREVIEW_TREE Tree);

VOID QHPreviewTreeFree(_Inout_ PQH_PREVIEW_TREE Tree);

NTSTATUS QHPreviewTreeInsert(
	_Inout_ PQH_PREVIEW_TREE Tree,
	_In_ const QH_JOURNAL_RECORD_HEADER* Header);

// Mark overlapping nodes Invalid (no structural delete). Used by recovery writes.
VOID QHPreviewTreeInvalidateRange(
	_Inout_ PQH_PREVIEW_TREE Tree,
	_In_ UINT64 VolumeOffset,
	_In_ ULONG DataLength);

NTSTATUS QHPreviewTreeMergeFrom(
	_Inout_ PQH_PREVIEW_TREE Dest,
	_Inout_ PQH_PREVIEW_TREE Source);

// Recovery build finish: Staging holds concurrent new-write ranges;
// invalidate overlapping HistoryTree nodes, then free Staging.
VOID QHPreviewTreePunchByStaging(
	_Inout_ PQH_PREVIEW_TREE HistoryTree,
	_Inout_ PQH_PREVIEW_TREE StagingTree);

// Rebuild Tree so intervals do not overlap: per byte keep earliest Sequence.
// Drops Invalid nodes. Used after History Punch / Preview Merge.
NTSTATUS QHPreviewTreeDedupEarliest(
	_Inout_ PQH_PREVIEW_TREE Tree);

// Merge volume-adjacent nodes whose journal payloads are also contiguous.
NTSTATUS QHPreviewTreeCoalesceAdjacent(
	_Inout_ PQH_PREVIEW_TREE Tree);

VOID QHJournalClose(_Inout_ PQH_JOURNAL Journal);
