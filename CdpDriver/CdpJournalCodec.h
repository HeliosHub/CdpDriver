#pragma once
#include "CdpJournal.h"

typedef struct _Cdp_PREVIEW_HIT
{
    UINT64 Start;
    UINT64 End;
    UINT64 FileOffset;
    ULONG DataLength;
    UINT64 Sequence;
} Cdp_PREVIEW_HIT, *PCdp_PREVIEW_HIT;

ULONG CdpJournalCodecCrc32c(ULONG InitialCrc, const VOID* Buffer,
    SIZE_T Length);
BOOLEAN CdpJournalCodecSuperblockValid(
    const Cdp_JOURNAL_SUPERBLOCK* Superblock, ULONG SectorSize,
    UINT64 PartitionSize, UINT64 UsableStart);
UINT64 CdpJournalCodecAlignDown64(UINT64 Value, ULONG Alignment);
UINT64 CdpJournalCodecAlignUp64(UINT64 Value, ULONG Alignment);
NTSTATUS CdpJournalCodecRingDistance(UINT64 UsableStart, UINT64 UsableEnd,
    UINT64 Start, UINT64 End, PUINT64 Distance);
BOOLEAN CdpJournalCodecHeaderRegionOffsetValid(UINT64 UsableStart,
    UINT64 UsableEnd, ULONG SectorSize, UINT64 RegionOff, ULONG HeaderRegionSize);
BOOLEAN CdpJournalCodecRegionLinkValid(UINT64 UsableStart, UINT64 UsableEnd,
    ULONG SectorSize, ULONG HeaderRegionSize, ULONG Reserved,
    UINT64 StartSequence, UINT64 PrevRegionOff, UINT64 NextRegionOff);

NTSTATUS CdpJournalCodecDecodeRecord(const Cdp_HEADER_REGION_LINK* Link,
    const Cdp_JOURNAL_RECORD_HEADER* Header, PCdp_JOURNAL_RECORD Record);

BOOLEAN CdpJournalCodecHeaderIsBranch(
    const Cdp_JOURNAL_RECORD_HEADER* Header);
BOOLEAN CdpJournalCodecHeaderIsDeleted(
    const Cdp_JOURNAL_RECORD_HEADER* Header);
BOOLEAN CdpJournalCodecBranchHeaderIsContinuation(
    const Cdp_JOURNAL_BRANCH_RECORD_HEADER* Header);
BOOLEAN CdpJournalCodecBranchHeaderReservedValid(
    const Cdp_JOURNAL_BRANCH_RECORD_HEADER* Header);
BOOLEAN CdpJournalCodecOffsetInRingSpan(UINT64 Offset, UINT64 SpanStart,
    UINT64 SpanEnd);
BOOLEAN CdpJournalCodecLatestPathLimit(PCdp_BRANCH_INFO_TREE Tree,
    PCdp_BRANCH_INFO_NODE Candidate, PUINT64 AllowedSequence);
BOOLEAN CdpJournalCodecLatestPathHasInheritancePoint(
    PCdp_BRANCH_INFO_TREE Tree, UINT64 InheritedRecordSequence);
BOOLEAN CdpJournalCodecRegionHasLiveBranchRecords(
    PCdp_BRANCH_INFO_TREE Tree, UINT64 FirstSequence, UINT64 EndSequence);
LONG CdpJournalCodecBranchPathFind(
    PCdp_BRANCH_INFO_NODE const* Path, ULONG PathCount,
    PCdp_BRANCH_INFO_NODE Branch);
PCdp_BRANCH_INFO_NODE CdpJournalCodecFindBranchBySequence(
    PCdp_BRANCH_INFO_TREE BranchTree, UINT64 Sequence);
PCdp_BRANCH_INFO_NODE CdpJournalCodecFindBranchAtTime(
    PCdp_BRANCH_INFO_TREE BranchTree, UINT64 TargetTime100ns);
ULONG CdpJournalCodecCountPreviewOverlaps(PCdp_PREVIEW_TREE_NODE Node,
    UINT64 QueryStart, UINT64 QueryEnd);
PCdp_PREVIEW_TREE_NODE CdpJournalCodecFindPreviewSequenceOverlap(
    PCdp_PREVIEW_TREE_NODE Node, UINT64 ExpectedSequence,
    UINT64 QueryStart, UINT64 QueryEnd);
PCdp_PREVIEW_TREE_NODE CdpJournalCodecFindPreviewPayloadOverlap(
    PCdp_PREVIEW_TREE_NODE Node, UINT64 QueryStart, UINT64 QueryEnd,
    UINT64 ExpectedFileOffset, UINT64 BaseVolumeOffset);
PCdp_PREVIEW_TREE_NODE CdpJournalCodecFindFirstPreviewOverlap(
    PCdp_PREVIEW_TREE_NODE Node, UINT64 QueryStart, UINT64 QueryEnd);
ULONG CdpJournalCodecBitmapByteCount(ULONG BitCount);
VOID CdpJournalCodecBitmapSetRange(PUCHAR Bitmap, ULONG StartBit,
    ULONG BitCount);
UINT64 CdpJournalCodecContiguousFree(UINT64 UsableEnd, UINT64 PayloadOffset,
    UINT64 OldestHeaderOffset, BOOLEAN IsEmpty);
BOOLEAN CdpJournalCodecPayloadRotationNeeded(ULONG CurrentHeaderCount,
    UINT64 PayloadSpan, UINT64 PartitionSize, UINT64 NextPayloadBytes);
LONG CdpJournalCodecPreviewNodeHeight(PCdp_PREVIEW_TREE_NODE Node);
PCdp_PREVIEW_TREE_NODE CdpJournalCodecPreviewAvlMinimum(
    PCdp_PREVIEW_TREE_NODE Root);
PCdp_BRANCH_INFO_NODE CdpJournalCodecFindBranchByNumber(
    PCdp_BRANCH_INFO_TREE Tree, LONG BranchNumber);
VOID CdpJournalCodecSetBranchRecordInfo(PCdp_BRANCH_RECORD_INFO Info,
    UINT64 Sequence, UINT64 WallClock100ns, UINT64 RegionOffset,
    ULONG HeaderIndex);
BOOLEAN CdpJournalCodecSequenceDiscardedByCompaction(
    PCdp_BRANCH_INFO_TREE Tree, UINT64 Sequence, UINT64 FirstSequence,
    UINT64 EndSequence);
VOID CdpJournalCodecCollectPreviewOverlaps(PCdp_PREVIEW_TREE_NODE Node,
    UINT64 QueryStart, UINT64 QueryEnd, PCdp_PREVIEW_HIT Hits,
    PULONG HitCount, ULONG HitCapacity);
VOID CdpJournalCodecUpdatePreviewNode(PCdp_PREVIEW_TREE_NODE Node);
VOID CdpJournalCodecCopyPreviewNodeData(PCdp_PREVIEW_TREE_NODE Destination,
    const PCdp_PREVIEW_TREE_NODE Source);
NTSTATUS CdpJournalCodecPreviewTreeInsertRaw(PCdp_PREVIEW_TREE Tree,
    const Cdp_JOURNAL_RECORD* Record);
NTSTATUS CdpJournalCodecPreviewTreeRemoveRange(PCdp_PREVIEW_TREE Tree,
    UINT64 CutStart, UINT64 CutEnd);
NTSTATUS CdpJournalCodecOverlayPreviewSnapshot(PCdp_PREVIEW_TREE Tree,
    PCdp_PREVIEW_TREE_NODE Node);
NTSTATUS CdpJournalCodecMeasureCheckpointGaps(
    PCdp_RUNTIME_CHECKPOINT FirstCheckpoint,
    const Cdp_CHECKPOINT_MERGE_RANGE* Ranges, ULONG RangeCount,
    ULONG SectorSize, PUINT64 NewCheckpointBytes);
NTSTATUS CdpJournalCodecCalculateCheckpointMergeReservation(
    PCdp_RUNTIME_CHECKPOINT FirstCheckpoint, UINT64 RegionOffset,
    UINT64 NextRegionOffset, ULONG HeaderRegionSize, ULONG SectorSize,
    UINT64 NewCheckpointBytes, UINT64 UsableStart, UINT64 UsableEnd,
    UINT64 PayloadRegionOffset, UINT64 PayloadBytesUsed,
    PUINT64 RelocationBytes, PUINT64 WrapPaddingBytes,
    PUINT64 ReservedBytes);
NTSTATUS CdpJournalCodecAppendCheckpointRemap(
    PCdp_CHECKPOINT_REMAP* Remaps, PULONG Count, PULONG Capacity,
    UINT64 VolumeOffset, UINT64 FileOffset, ULONG DataLength);

