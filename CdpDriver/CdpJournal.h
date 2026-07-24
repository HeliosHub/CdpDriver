#pragma once

#ifdef Cdp_USERMODE
#include "cdp_portable.h"
#include "cdp_store.h"
#else
#include "..\CdpCore\include\cdp_portable.h"
#include "..\CdpCore\include\cdp_store.h"
#endif

#define Cdp_JOURNAL_MAGIC            0x4C4E4A51UL /* 'QJNL' */
#define Cdp_JOURNAL_VERSION          7UL
#define Cdp_JOURNAL_MAX_RECORD_DATA  (2UL * 1024UL * 1024UL)
#define Cdp_JOURNAL_HEADER_REGION_SIZE (2UL * 1024UL * 1024UL)
#define Cdp_JOURNAL_HEADER_LINK_SIZE 32UL
#define Cdp_JOURNAL_HEADER_LINK_MARK 0xFFFFFFFFFFFFFFFFULL

#pragma pack(push, 1)

// Per-record header stored in 2MB header regions (32 bytes).
typedef struct _Cdp_JOURNAL_RECORD_HEADER
{
	UINT64 WallClock100ns; // 8  local wall-clock 100ns (FILETIME epoch)
	UINT64 VolumeOffset;   // 8  source volume byte offset
	UINT64 FileOffset;     // 8  payload offset inside CDP partition
	ULONG DataLength;      // 4
	ULONG Sequence;        // 4
} Cdp_JOURNAL_RECORD_HEADER, *PCdp_JOURNAL_RECORD_HEADER;

C_ASSERT(sizeof(Cdp_JOURNAL_RECORD_HEADER) == 32);

// Last 32 bytes of each 2MB header region.
typedef struct _Cdp_HEADER_REGION_LINK
{
	UINT64 Marker;         // must be 0xFF..
	UINT64 PrevRegionOff;  // previous header region (may be self)
	UINT64 NextRegionOff;  // next header region (may be self)
	UINT64 Reserved;
} Cdp_HEADER_REGION_LINK, *PCdp_HEADER_REGION_LINK;

C_ASSERT(sizeof(Cdp_HEADER_REGION_LINK) == 32);

// On-disk layout (v7): one superblock, then alternating header/payload areas.
//   [Superblock]
//   [HeaderRegion0 2MB][Payload0 ...]
//   [HeaderRegion1 2MB][Payload1 ...]
//   ...
typedef struct _Cdp_JOURNAL_SUPERBLOCK
{
	ULONG Magic;
	ULONG Version;
	ULONG SectorSize;
	ULONG Flags;
	UINT64 PartitionSize;
	UINT64 LastHeaderRegionOff; // newest 2MB header region
	GUID SourceVolumeGuid;
	ULONG Crc32c;
} Cdp_JOURNAL_SUPERBLOCK, *PCdp_JOURNAL_SUPERBLOCK;

#pragma pack(pop)

#define Cdp_JOURNAL_HEADERS_PER_REGION \
	((Cdp_JOURNAL_HEADER_REGION_SIZE - Cdp_JOURNAL_HEADER_LINK_SIZE) / \
		sizeof(Cdp_JOURNAL_RECORD_HEADER))

typedef struct _Cdp_JOURNAL
{
	BOOLEAN Mounted;
	ULONG SectorSize;
	UINT64 PartitionSize;

	UINT64 LastHeaderRegionOff;
	UINT64 PayloadRegionOff;

	UINT64 OldestHeaderRegionOff;
	ULONG OldestHeaderIndex;
	ULONG CurrentHeaderCount;
	// Transfer size used for a 2MB header region.  Formatting discovers it
	// from the largest successful write; preview/recovery scans reuse it for
	// reads instead of issuing one sector read per 32-byte record header.
	ULONG HeaderRegionWriteChunk;
	ULONG NextSequence;
	UINT64 TotalRecords;
	UINT64 PayloadBytesUsed;
	UINT64 RecordGeneration;
	UINT64 Oldest100ns;
	UINT64 Newest100ns;
	GUID SourceVolumeGuid;
#ifndef Cdp_USERMODE
	PDEVICE_OBJECT TargetDevice;
#else
	PVOID TargetDevice;
#endif
	PVOID RawDiskHandle; // kernel HANDLE; physical disk backend when non-NULL
	UINT64 TargetBaseOffset; // partition start on RawDiskHandle
	PCdp_STORE Store; // if set, RawIo uses store instead of TargetDevice
	Cdp_QUERY_TIME_100NS QueryTime100ns;
	PVOID QueryTimeContext;
	Cdp_LOCK Lock;
} Cdp_JOURNAL, *PCdp_JOURNAL;

typedef struct _Cdp_PREVIEW_TREE_NODE
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
	struct _Cdp_PREVIEW_TREE_NODE* Left;
	struct _Cdp_PREVIEW_TREE_NODE* Right;
} Cdp_PREVIEW_TREE_NODE, *PCdp_PREVIEW_TREE_NODE;

typedef struct _Cdp_PREVIEW_TREE
{
	PCdp_PREVIEW_TREE_NODE Root;
	ULONG NodeCount;
} Cdp_PREVIEW_TREE, *PCdp_PREVIEW_TREE;

VOID CdpJournalInitialize(
	_Out_ PCdp_JOURNAL Journal,
	_In_opt_ PVOID TargetDevice,
	_In_opt_ PVOID RawDiskHandle,
	_In_ UINT64 TargetBaseOffset,
	_In_ UINT64 PartitionSize,
	_In_ ULONG SectorSize,
	_In_ const GUID* SourceVolumeGuid);

VOID CdpJournalInitializeWithStore(
	_Out_ PCdp_JOURNAL Journal,
	_In_ PCdp_STORE Store,
	_In_ const GUID* SourceVolumeGuid,
	_In_opt_ Cdp_QUERY_TIME_100NS QueryTime100ns,
	_In_opt_ PVOID QueryTimeContext);

NTSTATUS CdpJournalFormat(_Inout_ PCdp_JOURNAL Journal);

NTSTATUS CdpJournalMount(_Inout_ PCdp_JOURNAL Journal);

// Clear on-disk superblock magic so auto-discovery will not remount this journal.
NTSTATUS CdpJournalInvalidate(_Inout_ PCdp_JOURNAL Journal);

NTSTATUS CdpJournalAppend(
	_Inout_ PCdp_JOURNAL Journal,
	_In_ UINT64 VolumeOffset,
	_In_ ULONG DataLength,
	_In_reads_bytes_(DataLength) const VOID* BeforeImage,
	_Out_opt_ PCdp_JOURNAL_RECORD_HEADER WrittenHeader);

NTSTATUS CdpJournalQueryTimeRange(
	_Inout_ PCdp_JOURNAL Journal,
	_Out_ PUINT64 OldestTime100ns,
	_Out_ PUINT64 NewestTime100ns);

// Payload-space accounting excludes the superblock and active 2MB header
// regions.  Used payload bytes are sector-aligned on-disk allocations.
NTSTATUS CdpJournalQueryUsage(
	_Inout_ PCdp_JOURNAL Journal,
	_Out_ PUINT64 PartitionBytes,
	_Out_ PUINT64 MetadataBytes,
	_Out_ PUINT64 PayloadBytesUsed,
	_Out_ PUINT64 PayloadBytesFree,
	_Out_ PUINT64 TotalRecords);

// Read retained record headers in chronological order.  Headers contain only
// record metadata; callers never receive journal payload data.  The caller
// can page with StartIndex and must echo Generation after the first page to
// detect concurrent capture/eviction.
NTSTATUS CdpJournalQueryRecordHeaders(
	_Inout_ PCdp_JOURNAL Journal,
	_In_ UINT64 StartIndex,
	_In_ UINT64 ExpectedGeneration,
	_Out_writes_to_(HeaderCapacity, *ReturnedCount) PCdp_JOURNAL_RECORD_HEADER Headers,
	_In_ ULONG HeaderCapacity,
	_Out_ PUINT64 TotalRecords,
	_Out_ PUINT64 Generation,
	_Out_ PULONG ReturnedCount);

NTSTATUS CdpJournalBuildPreviewTree(
	_Inout_ PCdp_JOURNAL Journal,
	_In_ UINT64 TargetTime100ns,
	_In_ ULONG MaxSequence,
	_In_ BOOLEAN IncludeTargetTime,
	_Out_ PCdp_PREVIEW_TREE Tree);

NTSTATUS CdpJournalApplyPreviewTree(
	_Inout_ PCdp_JOURNAL Journal,
	_In_ PCdp_PREVIEW_TREE Tree,
	_Inout_ Cdp_LOCK* TreeLock,
	_In_ UINT64 VolumeOffset,
	_In_ ULONG DataLength,
	_Out_writes_bytes_(DataLength) PVOID Buffer,
	// One bit per output byte; caller supplies (DataLength + 7) / 8 bytes.
	_Out_writes_bytes_((DataLength + 7) / 8) PUCHAR CoveredMask,
	_Out_ PULONG CoveredCount);

// Read a single record payload from the journal (FileOffset from record header).
NTSTATUS CdpJournalReadPayload(
	_Inout_ PCdp_JOURNAL Journal,
	_In_ UINT64 FileOffset,
	_In_ ULONG DataLength,
	_Out_writes_bytes_(DataLength) PVOID Buffer);

VOID CdpPreviewTreeInitialize(_Out_ PCdp_PREVIEW_TREE Tree);

VOID CdpPreviewTreeFree(_Inout_ PCdp_PREVIEW_TREE Tree);

NTSTATUS CdpPreviewTreeInsert(
	_Inout_ PCdp_PREVIEW_TREE Tree,
	_In_ const Cdp_JOURNAL_RECORD_HEADER* Header);

// Mark overlapping nodes Invalid (no structural delete). Kept for the
// allocation-failure safety fallback used by recovery writes.
VOID CdpPreviewTreeInvalidateRange(
	_Inout_ PCdp_PREVIEW_TREE Tree,
	_In_ UINT64 VolumeOffset,
	_In_ ULONG DataLength);

// Remove only the intersecting byte range from the history tree.  Remaining
// left/right fragments keep their original journal payload offsets.
NTSTATUS CdpPreviewTreePunchRange(
	_Inout_ PCdp_PREVIEW_TREE Tree,
	_In_ UINT64 VolumeOffset,
	_In_ ULONG DataLength);

NTSTATUS CdpPreviewTreeMergeFrom(
	_Inout_ PCdp_PREVIEW_TREE Dest,
	_Inout_ PCdp_PREVIEW_TREE Source);

// Recovery build finish: Staging holds concurrent new-write ranges;
// remove only their overlapping bytes from HistoryTree, then free Staging.
NTSTATUS CdpPreviewTreePunchByStaging(
	_Inout_ PCdp_PREVIEW_TREE HistoryTree,
	_Inout_ PCdp_PREVIEW_TREE StagingTree);

// Rebuild Tree so intervals do not overlap: per byte keep earliest Sequence.
// Drops Invalid nodes. Used after History Punch / Preview Merge.
NTSTATUS CdpPreviewTreeDedupEarliest(
	_Inout_ PCdp_PREVIEW_TREE Tree);

// Merge volume-adjacent nodes whose journal payloads are also contiguous.
NTSTATUS CdpPreviewTreeCoalesceAdjacent(
	_Inout_ PCdp_PREVIEW_TREE Tree);

VOID CdpJournalClose(_Inout_ PCdp_JOURNAL Journal);
