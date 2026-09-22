#include "CdpJournalCodec.h"

UINT64 CdpJournalCodecAlignDown64(UINT64 Value, ULONG Alignment)
{
    return Value - (Value % Alignment);
}

UINT64 CdpJournalCodecAlignUp64(UINT64 Value, ULONG Alignment)
{
    UINT64 remainder = Value % Alignment;
    return remainder ? Value + (Alignment - remainder) : Value;
}

NTSTATUS CdpJournalCodecRingDistance(UINT64 UsableStart, UINT64 UsableEnd,
    UINT64 Start, UINT64 End, PUINT64 Distance)
{
    if (!Distance)
        return STATUS_INVALID_PARAMETER;
    if (Start < UsableStart || Start > UsableEnd ||
        End < UsableStart || End > UsableEnd)
    {
        return STATUS_DISK_CORRUPT_ERROR;
    }
    *Distance = End >= Start ? End - Start :
        (UsableEnd - Start) + (End - UsableStart);
    return STATUS_SUCCESS;
}

BOOLEAN CdpJournalCodecHeaderRegionOffsetValid(UINT64 UsableStart,
    UINT64 UsableEnd, ULONG SectorSize, UINT64 RegionOff, ULONG HeaderRegionSize)
{
    return RegionOff >= UsableStart &&
        RegionOff <= UsableEnd - HeaderRegionSize &&
        (RegionOff % SectorSize) == 0;
}

BOOLEAN CdpJournalCodecRegionLinkValid(UINT64 UsableStart, UINT64 UsableEnd,
    ULONG SectorSize, ULONG HeaderRegionSize, ULONG Reserved,
    UINT64 StartSequence, UINT64 PrevRegionOff, UINT64 NextRegionOff)
{
    return Reserved == 0 && StartSequence != 0 &&
        CdpJournalCodecHeaderRegionOffsetValid(UsableStart, UsableEnd,
            SectorSize, PrevRegionOff, HeaderRegionSize) &&
        CdpJournalCodecHeaderRegionOffsetValid(UsableStart, UsableEnd,
            SectorSize, NextRegionOff, HeaderRegionSize);
}

NTSTATUS CdpJournalCodecDecodeRecord(const Cdp_HEADER_REGION_LINK* Link,
    const Cdp_JOURNAL_RECORD_HEADER* Header, PCdp_JOURNAL_RECORD Record)
{
    ULONG localSequence = Header ?
        (Header->Sequence & Cdp_JOURNAL_RECORD_INDEX_MASK) : 0;
    ULONG recordFlags = Header ?
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

BOOLEAN CdpJournalCodecHeaderIsBranch(
    const Cdp_JOURNAL_RECORD_HEADER* Header)
{
    return (Header->Sequence & Cdp_JOURNAL_RECORD_FLAG_BRANCH) != 0;
}

BOOLEAN CdpJournalCodecHeaderIsDeleted(
    const Cdp_JOURNAL_RECORD_HEADER* Header)
{
    return (Header->Sequence & Cdp_JOURNAL_RECORD_FLAG_DELETED) != 0;
}

BOOLEAN CdpJournalCodecBranchHeaderIsContinuation(
    const Cdp_JOURNAL_BRANCH_RECORD_HEADER* Header)
{
    return Header->Reserved == Cdp_JOURNAL_BRANCH_RECORD_FLAG_CONTINUATION;
}

BOOLEAN CdpJournalCodecBranchHeaderReservedValid(
    const Cdp_JOURNAL_BRANCH_RECORD_HEADER* Header)
{
    return Header->Reserved == Cdp_JOURNAL_BRANCH_RECORD_FLAG_FIRST ||
        CdpJournalCodecBranchHeaderIsContinuation(Header);
}

BOOLEAN CdpJournalCodecOffsetInRingSpan(UINT64 Offset, UINT64 SpanStart,
    UINT64 SpanEnd)
{
    if (SpanStart < SpanEnd)
        return Offset >= SpanStart && Offset < SpanEnd;
    if (SpanStart > SpanEnd)
        return Offset >= SpanStart || Offset < SpanEnd;
    return FALSE;
}

BOOLEAN CdpJournalCodecLatestPathLimit(PCdp_BRANCH_INFO_TREE Tree,
    PCdp_BRANCH_INFO_NODE Candidate, PUINT64 AllowedSequence)
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

BOOLEAN CdpJournalCodecLatestPathHasInheritancePoint(
    PCdp_BRANCH_INFO_TREE Tree, UINT64 InheritedRecordSequence)
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

BOOLEAN CdpJournalCodecRegionHasLiveBranchRecords(
    PCdp_BRANCH_INFO_TREE Tree, UINT64 FirstSequence, UINT64 EndSequence)
{
    PCdp_BRANCH_INFO_NODE branch;

    if (!Tree || FirstSequence >= EndSequence)
        return FALSE;
    for (branch = Tree->First; branch; branch = branch->Next)
    {
        if (branch->EndRecord.Sequence >= FirstSequence &&
            branch->StartRecord.Sequence < EndSequence)
        {
            return TRUE;
        }
    }
    return FALSE;
}

LONG CdpJournalCodecBranchPathFind(
    PCdp_BRANCH_INFO_NODE const* Path, ULONG PathCount,
    PCdp_BRANCH_INFO_NODE Branch)
{
    ULONG index;

    for (index = 0; index < PathCount; ++index)
    {
        if (Path[index] == Branch)
            return (LONG)index;
    }
    return -1;
}

PCdp_BRANCH_INFO_NODE CdpJournalCodecFindBranchBySequence(
    PCdp_BRANCH_INFO_TREE BranchTree, UINT64 Sequence)
{
    PCdp_BRANCH_INFO_NODE branch;

    if (!BranchTree)
        return NULL;
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

PCdp_BRANCH_INFO_NODE CdpJournalCodecFindBranchAtTime(
    PCdp_BRANCH_INFO_TREE BranchTree, UINT64 TargetTime100ns)
{
    PCdp_BRANCH_INFO_NODE branch;
    PCdp_BRANCH_INFO_NODE target;

    if (!BranchTree)
        return NULL;
    target = BranchTree->First;
    for (branch = BranchTree->First; branch; branch = branch->Next)
    {
        if (branch->StartRecord.WallClock100ns > TargetTime100ns)
            break;
        target = branch;
    }
    return target;
}

ULONG CdpJournalCodecCountPreviewOverlaps(PCdp_PREVIEW_TREE_NODE Node,
    UINT64 QueryStart, UINT64 QueryEnd)
{
    ULONG count = 0;

    if (!Node || Node->MaxEnd <= QueryStart)
        return 0;
    if (Node->Left)
        count += CdpJournalCodecCountPreviewOverlaps(
            Node->Left, QueryStart, QueryEnd);
    if (!Node->Invalid && Node->Start < QueryEnd && Node->End > QueryStart)
        ++count;
    if (Node->Start < QueryEnd && Node->Right)
        count += CdpJournalCodecCountPreviewOverlaps(
            Node->Right, QueryStart, QueryEnd);
    return count;
}

PCdp_PREVIEW_TREE_NODE CdpJournalCodecFindPreviewSequenceOverlap(
    PCdp_PREVIEW_TREE_NODE Node, UINT64 ExpectedSequence,
    UINT64 QueryStart, UINT64 QueryEnd)
{
    PCdp_PREVIEW_TREE_NODE found;

    if (!Node || Node->MaxEnd <= QueryStart)
        return NULL;
    found = CdpJournalCodecFindPreviewSequenceOverlap(
        Node->Left, ExpectedSequence, QueryStart, QueryEnd);
    if (found)
        return found;
    if (!Node->Invalid && Node->Sequence == ExpectedSequence &&
        Node->Start < QueryEnd && Node->End > QueryStart)
    {
        return Node;
    }
    if (Node->Start < QueryEnd)
        return CdpJournalCodecFindPreviewSequenceOverlap(
            Node->Right, ExpectedSequence, QueryStart, QueryEnd);
    return NULL;
}

PCdp_PREVIEW_TREE_NODE CdpJournalCodecFindPreviewPayloadOverlap(
    PCdp_PREVIEW_TREE_NODE Node, UINT64 QueryStart, UINT64 QueryEnd,
    UINT64 ExpectedFileOffset, UINT64 BaseVolumeOffset)
{
    PCdp_PREVIEW_TREE_NODE found;
    UINT64 overlapStart;

    if (!Node || Node->MaxEnd <= QueryStart)
        return NULL;
    found = CdpJournalCodecFindPreviewPayloadOverlap(
        Node->Left, QueryStart, QueryEnd,
        ExpectedFileOffset, BaseVolumeOffset);
    if (found)
        return found;
    if (!Node->Invalid && Node->Start < QueryEnd && Node->End > QueryStart)
    {
        overlapStart = Node->Start > QueryStart ? Node->Start : QueryStart;
        if (Node->FileOffset + (overlapStart - Node->Start) ==
            ExpectedFileOffset + (overlapStart - BaseVolumeOffset))
        {
            return Node;
        }
    }
    if (Node->Start < QueryEnd)
        return CdpJournalCodecFindPreviewPayloadOverlap(
            Node->Right, QueryStart, QueryEnd,
            ExpectedFileOffset, BaseVolumeOffset);
    return NULL;
}

PCdp_PREVIEW_TREE_NODE CdpJournalCodecFindFirstPreviewOverlap(
    PCdp_PREVIEW_TREE_NODE Node, UINT64 QueryStart, UINT64 QueryEnd)
{
    PCdp_PREVIEW_TREE_NODE hit;

    if (!Node || Node->MaxEnd <= QueryStart)
        return NULL;
    if (Node->Left)
    {
        hit = CdpJournalCodecFindFirstPreviewOverlap(
            Node->Left, QueryStart, QueryEnd);
        if (hit)
            return hit;
    }
    if (!Node->Invalid && Node->Start < QueryEnd && Node->End > QueryStart)
        return Node;
    if (Node->Start < QueryEnd && Node->Right)
        return CdpJournalCodecFindFirstPreviewOverlap(
            Node->Right, QueryStart, QueryEnd);
    return NULL;
}

ULONG CdpJournalCodecBitmapByteCount(ULONG BitCount)
{
    return (BitCount + 7UL) / 8UL;
}

VOID CdpJournalCodecBitmapSetRange(PUCHAR Bitmap, ULONG StartBit,
    ULONG BitCount)
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

UINT64 CdpJournalCodecContiguousFree(UINT64 UsableEnd, UINT64 PayloadOffset,
    UINT64 OldestHeaderOffset, BOOLEAN IsEmpty)
{
    if (IsEmpty)
        return UsableEnd - PayloadOffset;
    if (PayloadOffset == OldestHeaderOffset)
        return 0;
    if (PayloadOffset < OldestHeaderOffset)
        return OldestHeaderOffset - PayloadOffset;
    return UsableEnd - PayloadOffset;
}

BOOLEAN CdpJournalCodecPayloadRotationNeeded(ULONG CurrentHeaderCount,
    UINT64 PayloadSpan, UINT64 PartitionSize, UINT64 NextPayloadBytes)
{
    UINT64 threshold;

    if (CurrentHeaderCount == 0)
        return FALSE;
    threshold = PartitionSize / Cdp_JOURNAL_PAYLOAD_REGION_CAPACITY_DIVISOR;
    return threshold == 0 || PayloadSpan >= threshold ||
        NextPayloadBytes > threshold - PayloadSpan;
}

LONG CdpJournalCodecPreviewNodeHeight(PCdp_PREVIEW_TREE_NODE Node)
{
    return Node ? Node->Height : 0;
}

PCdp_PREVIEW_TREE_NODE CdpJournalCodecPreviewAvlMinimum(
    PCdp_PREVIEW_TREE_NODE Root)
{
    while (Root->Left)
        Root = Root->Left;
    return Root;
}

PCdp_BRANCH_INFO_NODE CdpJournalCodecFindBranchByNumber(
    PCdp_BRANCH_INFO_TREE Tree, LONG BranchNumber)
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

VOID CdpJournalCodecSetBranchRecordInfo(PCdp_BRANCH_RECORD_INFO Info,
    UINT64 Sequence, UINT64 WallClock100ns, UINT64 RegionOffset,
    ULONG HeaderIndex)
{
    Info->Sequence = Sequence;
    Info->WallClock100ns = WallClock100ns;
    Info->HeaderRegionOffset = RegionOffset;
    Info->HeaderIndex = HeaderIndex;
}

BOOLEAN CdpJournalCodecSequenceDiscardedByCompaction(
    PCdp_BRANCH_INFO_TREE Tree, UINT64 Sequence, UINT64 FirstSequence,
    UINT64 EndSequence)
{
    PCdp_BRANCH_INFO_NODE owner;
    BOOLEAN onLatestPath;

    owner = CdpJournalCodecFindBranchBySequence(Tree, Sequence);
    if (!owner || owner->PrunePending)
        return owner != NULL;
    onLatestPath = CdpJournalCodecLatestPathLimit(Tree, owner, NULL);
    if (Sequence >= FirstSequence && Sequence < EndSequence)
        return !onLatestPath;
    return FALSE;
}

VOID CdpJournalCodecCollectPreviewOverlaps(PCdp_PREVIEW_TREE_NODE Node,
    UINT64 QueryStart, UINT64 QueryEnd, PCdp_PREVIEW_HIT Hits,
    PULONG HitCount, ULONG HitCapacity)
{
    if (!Node || *HitCount >= HitCapacity || Node->MaxEnd <= QueryStart)
        return;
    if (Node->Left)
        CdpJournalCodecCollectPreviewOverlaps(Node->Left, QueryStart,
            QueryEnd, Hits, HitCount, HitCapacity);
    if (!Node->Invalid && Node->Start < QueryEnd && Node->End > QueryStart &&
        *HitCount < HitCapacity)
    {
        Hits[*HitCount].Start = Node->Start;
        Hits[*HitCount].End = Node->End;
        Hits[*HitCount].FileOffset = Node->FileOffset;
        Hits[*HitCount].DataLength = Node->DataLength;
        Hits[*HitCount].Sequence = Node->Sequence;
        (*HitCount)++;
    }
    if (Node->Start < QueryEnd && Node->Right)
        CdpJournalCodecCollectPreviewOverlaps(Node->Right, QueryStart,
            QueryEnd, Hits, HitCount, HitCapacity);
}

VOID CdpJournalCodecUpdatePreviewNode(PCdp_PREVIEW_TREE_NODE Node)
{
    LONG leftHeight = CdpJournalCodecPreviewNodeHeight(Node->Left);
    LONG rightHeight = CdpJournalCodecPreviewNodeHeight(Node->Right);
    UINT64 maxEnd = Node->End;
    UINT64 minValidSequence = Node->Invalid ? MAXUINT64 : Node->Sequence;

    Node->Height = 1 + (leftHeight > rightHeight ? leftHeight : rightHeight);
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

VOID CdpJournalCodecCopyPreviewNodeData(PCdp_PREVIEW_TREE_NODE Destination,
    const PCdp_PREVIEW_TREE_NODE Source)
{
    Destination->Start = Source->Start;
    Destination->End = Source->End;
    Destination->FileOffset = Source->FileOffset;
    Destination->WallClock100ns = Source->WallClock100ns;
    Destination->DataLength = Source->DataLength;
    Destination->Sequence = Source->Sequence;
    Destination->Invalid = Source->Invalid;
}
