#include "CdpJournalCodec.h"

#ifdef Cdp_USERMODE
#include <stdlib.h>
#define CdpJournalCodecAlloc(Size) malloc(Size)
#define CdpJournalCodecFree(Pointer) free(Pointer)
#else
#include "CdpEngineDefs.h"
#define CdpJournalCodecAlloc(Size) cdpalloc(Size)
#define CdpJournalCodecFree(Pointer) cdpfree(Pointer)
#endif

#define Cdp_CODEC_CRC32C_POLY 0x82F63B78UL

static ULONG g_CdpJournalCodecCrc32cTable[256];
static volatile LONG g_CdpJournalCodecCrc32cReady;

static VOID CdpJournalCodecStallBrief(VOID)
{
#ifdef Cdp_USERMODE
    SwitchToThread();
#else
    KeStallExecutionProcessor(1);
#endif
}

static VOID CdpJournalCodecInitializeCrc32c(VOID)
{
    ULONG table[256];
    ULONG index;

    if (InterlockedCompareExchange(&g_CdpJournalCodecCrc32cReady, 1, 0) != 0)
    {
        while (InterlockedCompareExchange(&g_CdpJournalCodecCrc32cReady, 0, 0) != 2)
            CdpJournalCodecStallBrief();
        return;
    }
    for (index = 0; index < RTL_NUMBER_OF(table); ++index)
    {
        ULONG crc = index;
        ULONG bit;

        for (bit = 0; bit < 8; ++bit)
            crc = (crc & 1) ? ((crc >> 1) ^ Cdp_CODEC_CRC32C_POLY) : (crc >> 1);
        table[index] = crc;
    }
    RtlCopyMemory(g_CdpJournalCodecCrc32cTable, table, sizeof(table));
    InterlockedExchange(&g_CdpJournalCodecCrc32cReady, 2);
}

ULONG CdpJournalCodecCrc32c(ULONG InitialCrc, const VOID* Buffer,
    SIZE_T Length)
{
    const UCHAR* bytes = (const UCHAR*)Buffer;
    ULONG crc = InitialCrc ^ 0xFFFFFFFFUL;

    if (InterlockedCompareExchange(&g_CdpJournalCodecCrc32cReady, 0, 0) != 2)
        CdpJournalCodecInitializeCrc32c();
    while (Length--)
        crc = g_CdpJournalCodecCrc32cTable[(crc ^ *bytes++) & 0xFF] ^
            (crc >> 8);
    return crc ^ 0xFFFFFFFFUL;
}

BOOLEAN CdpJournalCodecSuperblockValid(
    const Cdp_JOURNAL_SUPERBLOCK* Superblock, ULONG SectorSize,
    UINT64 PartitionSize, UINT64 UsableStart)
{
    ULONG crc;

    if (!Superblock || Superblock->Magic != Cdp_JOURNAL_MAGIC ||
        Superblock->Version != Cdp_JOURNAL_VERSION ||
        Superblock->SectorSize != SectorSize ||
        Superblock->PartitionSize != PartitionSize)
    {
        return FALSE;
    }
    crc = CdpJournalCodecCrc32c(0, Superblock,
        FIELD_OFFSET(Cdp_JOURNAL_SUPERBLOCK, Crc32c));
    if (crc != Superblock->Crc32c)
        return FALSE;
    if ((Superblock->Flags & Cdp_JOURNAL_FLAG_RECOVERY_PENDING) != 0 &&
        (Superblock->RecoveryTargetTime100ns == 0 ||
         CdpJournalCodecCrc32c(0, Superblock,
            FIELD_OFFSET(Cdp_JOURNAL_SUPERBLOCK, RecoveryCrc32c)) !=
            Superblock->RecoveryCrc32c))
    {
        return FALSE;
    }
    if (CdpJournalCodecCrc32c(0, Superblock,
        FIELD_OFFSET(Cdp_JOURNAL_SUPERBLOCK, MetadataCrc32c)) !=
        Superblock->MetadataCrc32c)
    {
        return FALSE;
    }
    if (Superblock->Version >= Cdp_JOURNAL_VERSION &&
        (Superblock->Flags & Cdp_JOURNAL_FLAG_RESTORE_POINT_SET) != 0 &&
        (Superblock->RestorePointTime100ns == 0 ||
         CdpJournalCodecCrc32c(0, Superblock,
            FIELD_OFFSET(Cdp_JOURNAL_SUPERBLOCK, RestorePointCrc32c)) !=
            Superblock->RestorePointCrc32c))
    {
        return FALSE;
    }
    if ((Superblock->Flags & Cdp_JOURNAL_FLAG_RESTORE_BOOT_PENDING) != 0 &&
        (Superblock->Flags & Cdp_JOURNAL_FLAG_RESTORE_POINT_SET) == 0)
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
#ifdef CDP_LICENSE
    if ((Superblock->Flags & Cdp_JOURNAL_FLAG_LICENSE_CONFIGURED) != 0 &&
        (Superblock->LicenseBlobLength == 0 ||
         Superblock->LicenseBlobLength > Cdp_LICENSE_BLOB_MAX ||
         Superblock->E0Length == 0 || Superblock->E0Length > Cdp_E0_SEAL_MAX ||
         CdpJournalCodecCrc32c(0, Superblock,
            FIELD_OFFSET(Cdp_JOURNAL_SUPERBLOCK, LicenseCrc32c)) !=
            Superblock->LicenseCrc32c))
    {
        return FALSE;
    }
#endif
    if (Superblock->CurrentBranchNumber <= 0 ||
        Superblock->HighestBranchNumber < Superblock->CurrentBranchNumber ||
        Superblock->LastHeaderRegionOff < UsableStart ||
        Superblock->LastHeaderRegionOff + Cdp_JOURNAL_HEADER_REGION_SIZE >
            PartitionSize ||
        (Superblock->LastHeaderRegionOff % SectorSize) != 0)
    {
        return FALSE;
    }
    return TRUE;
}

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

static PCdp_PREVIEW_TREE_NODE CdpJournalCodecRotateRight(
    PCdp_PREVIEW_TREE_NODE root)
{
    PCdp_PREVIEW_TREE_NODE left = root->Left;
    PCdp_PREVIEW_TREE_NODE middle = left->Right;

    left->Right = root;
    root->Left = middle;
    CdpJournalCodecUpdatePreviewNode(root);
    CdpJournalCodecUpdatePreviewNode(left);
    return left;
}

static PCdp_PREVIEW_TREE_NODE CdpJournalCodecRotateLeft(
    PCdp_PREVIEW_TREE_NODE root)
{
    PCdp_PREVIEW_TREE_NODE right = root->Right;
    PCdp_PREVIEW_TREE_NODE middle = right->Left;

    right->Left = root;
    root->Right = middle;
    CdpJournalCodecUpdatePreviewNode(root);
    CdpJournalCodecUpdatePreviewNode(right);
    return right;
}

static PCdp_PREVIEW_TREE_NODE CdpJournalCodecInsertNode(
    PCdp_PREVIEW_TREE_NODE root, PCdp_PREVIEW_TREE_NODE node)
{
    LONG balance;

    if (!root)
        return node;
    if (node->Start < root->Start)
        root->Left = CdpJournalCodecInsertNode(root->Left, node);
    else
        root->Right = CdpJournalCodecInsertNode(root->Right, node);
    CdpJournalCodecUpdatePreviewNode(root);
    balance = CdpJournalCodecPreviewNodeHeight(root->Left) -
        CdpJournalCodecPreviewNodeHeight(root->Right);
    if (balance > 1 && node->Start < root->Left->Start)
        return CdpJournalCodecRotateRight(root);
    if (balance < -1 && node->Start >= root->Right->Start)
        return CdpJournalCodecRotateLeft(root);
    if (balance > 1 && node->Start >= root->Left->Start)
    {
        root->Left = CdpJournalCodecRotateLeft(root->Left);
        return CdpJournalCodecRotateRight(root);
    }
    if (balance < -1 && node->Start < root->Right->Start)
    {
        root->Right = CdpJournalCodecRotateRight(root->Right);
        return CdpJournalCodecRotateLeft(root);
    }
    return root;
}

NTSTATUS CdpJournalCodecPreviewTreeInsertRaw(PCdp_PREVIEW_TREE Tree,
    const Cdp_JOURNAL_RECORD* Record)
{
    PCdp_PREVIEW_TREE_NODE node;

    if (!Tree || !Record)
        return STATUS_INVALID_PARAMETER;
    if (Record->DataLength == 0)
        return STATUS_SUCCESS;
    node = (PCdp_PREVIEW_TREE_NODE)CdpJournalCodecAlloc(sizeof(*node));
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
    Tree->Root = CdpJournalCodecInsertNode(Tree->Root, node);
    Tree->NodeCount++;
    return STATUS_SUCCESS;
}

static PCdp_PREVIEW_TREE_NODE CdpJournalCodecRebalance(
    PCdp_PREVIEW_TREE_NODE root)
{
    LONG balance;

    if (!root)
        return NULL;
    CdpJournalCodecUpdatePreviewNode(root);
    balance = CdpJournalCodecPreviewNodeHeight(root->Left) -
        CdpJournalCodecPreviewNodeHeight(root->Right);
    if (balance > 1)
    {
        if (CdpJournalCodecPreviewNodeHeight(root->Left->Left) <
            CdpJournalCodecPreviewNodeHeight(root->Left->Right))
            root->Left = CdpJournalCodecRotateLeft(root->Left);
        return CdpJournalCodecRotateRight(root);
    }
    if (balance < -1)
    {
        if (CdpJournalCodecPreviewNodeHeight(root->Right->Right) <
            CdpJournalCodecPreviewNodeHeight(root->Right->Left))
            root->Right = CdpJournalCodecRotateRight(root->Right);
        return CdpJournalCodecRotateLeft(root);
    }
    return root;
}

static PCdp_PREVIEW_TREE_NODE CdpJournalCodecDeleteByStart(
    PCdp_PREVIEW_TREE_NODE root, UINT64 start, PBOOLEAN removed)
{
    if (!root)
        return NULL;
    if (start < root->Start)
        root->Left = CdpJournalCodecDeleteByStart(root->Left, start, removed);
    else if (start > root->Start)
        root->Right = CdpJournalCodecDeleteByStart(root->Right, start, removed);
    else if (!root->Left || !root->Right)
    {
        PCdp_PREVIEW_TREE_NODE child = root->Left ? root->Left : root->Right;
        CdpJournalCodecFree(root);
        *removed = TRUE;
        return child;
    }
    else
    {
        PCdp_PREVIEW_TREE_NODE successor =
            CdpJournalCodecPreviewAvlMinimum(root->Right);
        UINT64 successorStart = successor->Start;

        CdpJournalCodecCopyPreviewNodeData(root, successor);
        root->Right = CdpJournalCodecDeleteByStart(
            root->Right, successorStart, removed);
    }
    return CdpJournalCodecRebalance(root);
}

NTSTATUS CdpJournalCodecPreviewTreeRemoveRange(PCdp_PREVIEW_TREE Tree,
    UINT64 CutStart, UINT64 CutEnd)
{
    NTSTATUS status;

    if (!Tree || CutStart >= CutEnd)
        return STATUS_INVALID_PARAMETER;
    for (;;)
    {
        PCdp_PREVIEW_TREE_NODE overlap =
            CdpJournalCodecFindFirstPreviewOverlap(
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
        Tree->Root = CdpJournalCodecDeleteByStart(
            Tree->Root, overlap->Start, &removed);
        if (!removed)
            return STATUS_DISK_CORRUPT_ERROR;
        Tree->NodeCount--;
        if (saved.VolumeOffset < CutStart)
        {
            Cdp_JOURNAL_RECORD left = saved;
            left.DataLength = (ULONG)(CutStart - saved.VolumeOffset);
            status = CdpJournalCodecPreviewTreeInsertRaw(Tree, &left);
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
            status = CdpJournalCodecPreviewTreeInsertRaw(Tree, &right);
            if (!NT_SUCCESS(status))
                return status;
        }
    }
    return STATUS_SUCCESS;
}

NTSTATUS CdpJournalCodecOverlayPreviewSnapshot(PCdp_PREVIEW_TREE Tree,
    PCdp_PREVIEW_TREE_NODE Node)
{
    Cdp_JOURNAL_RECORD record;
    NTSTATUS status;

    if (!Node)
        return STATUS_SUCCESS;
    status = CdpJournalCodecOverlayPreviewSnapshot(Tree, Node->Left);
    if (!NT_SUCCESS(status))
        return status;
    RtlZeroMemory(&record, sizeof(record));
    record.WallClock100ns = Node->WallClock100ns;
    record.VolumeOffset = Node->Start;
    record.FileOffset = Node->FileOffset;
    record.Sequence = Node->Sequence;
    record.DataLength = Node->DataLength;
    status = CdpPreviewTreeOverlayLatest(Tree, &record);
    if (!NT_SUCCESS(status))
        return status;
    return CdpJournalCodecOverlayPreviewSnapshot(Tree, Node->Right);
}

NTSTATUS CdpJournalCodecMeasureCheckpointGaps(
    PCdp_RUNTIME_CHECKPOINT FirstCheckpoint,
    const Cdp_CHECKPOINT_MERGE_RANGE* Ranges, ULONG RangeCount,
    ULONG SectorSize, PUINT64 NewCheckpointBytes)
{
    UINT64 total = 0;
    ULONG rangeIndex;

    if (!NewCheckpointBytes || (!Ranges && RangeCount != 0) || SectorSize == 0)
        return STATUS_INVALID_PARAMETER;
    for (rangeIndex = 0; rangeIndex < RangeCount; ++rangeIndex)
    {
        UINT64 rangeStart = Ranges[rangeIndex].VolumeOffset;
        UINT64 rangeEnd;
        UINT64 cursor;

        if (Ranges[rangeIndex].DataLength == 0 ||
            rangeStart > MAXUINT64 - Ranges[rangeIndex].DataLength)
        {
            return STATUS_INVALID_PARAMETER;
        }
        rangeEnd = rangeStart + Ranges[rangeIndex].DataLength;
        if (rangeIndex != 0)
        {
            UINT64 previousEnd = Ranges[rangeIndex - 1].VolumeOffset +
                Ranges[rangeIndex - 1].DataLength;
            if (rangeStart < previousEnd)
                return STATUS_INVALID_PARAMETER;
        }
        cursor = rangeStart;
        while (cursor < rangeEnd)
        {
            PCdp_RUNTIME_CHECKPOINT checkpoint;
            UINT64 coveredEnd = cursor;
            UINT64 nextCoveredStart = rangeEnd;

            for (checkpoint = FirstCheckpoint; checkpoint;
                checkpoint = checkpoint->Next)
            {
                UINT64 checkpointEnd;

                if (checkpoint->VolumeOffset >
                    MAXUINT64 - checkpoint->DataLength)
                {
                    return STATUS_DISK_CORRUPT_ERROR;
                }
                checkpointEnd = checkpoint->VolumeOffset + checkpoint->DataLength;
                if (checkpoint->VolumeOffset <= cursor && checkpointEnd > cursor)
                {
                    if (checkpointEnd > coveredEnd)
                        coveredEnd = checkpointEnd;
                }
                else if (checkpoint->VolumeOffset > cursor &&
                    checkpoint->VolumeOffset < nextCoveredStart)
                {
                    nextCoveredStart = checkpoint->VolumeOffset;
                }
            }
            if (coveredEnd > cursor)
            {
                cursor = coveredEnd < rangeEnd ? coveredEnd : rangeEnd;
            }
            else
            {
                UINT64 gapEnd = nextCoveredStart < rangeEnd ?
                    nextCoveredStart : rangeEnd;
                UINT64 alignedLength = CdpJournalCodecAlignUp64(
                    gapEnd - cursor, SectorSize);
                if (total > MAXUINT64 - alignedLength)
                    return STATUS_INTEGER_OVERFLOW;
                total += alignedLength;
                cursor = gapEnd;
            }
        }
    }
    *NewCheckpointBytes = total;
    return STATUS_SUCCESS;
}

NTSTATUS CdpJournalCodecCalculateCheckpointMergeReservation(
    PCdp_RUNTIME_CHECKPOINT FirstCheckpoint, UINT64 RegionOffset,
    UINT64 NextRegionOffset, ULONG HeaderRegionSize, ULONG SectorSize,
    UINT64 NewCheckpointBytes, UINT64 UsableStart, UINT64 UsableEnd,
    UINT64 PayloadRegionOffset, UINT64 PayloadBytesUsed,
    PUINT64 RelocationBytes, PUINT64 WrapPaddingBytes,
    PUINT64 ReservedBytes)
{
    PCdp_RUNTIME_CHECKPOINT checkpoint;
    UINT64 relocation = 0;
    UINT64 total;
    UINT64 wrapPadding = 0;

    if (!RelocationBytes || !WrapPaddingBytes || !ReservedBytes ||
        SectorSize == 0 || RegionOffset > MAXUINT64 - HeaderRegionSize ||
        UsableStart > UsableEnd || PayloadRegionOffset < UsableStart ||
        PayloadRegionOffset > UsableEnd)
    {
        return STATUS_INVALID_PARAMETER;
    }
    for (checkpoint = FirstCheckpoint; checkpoint; checkpoint = checkpoint->Next)
    {
        UINT64 alignedLength;

        if (!CdpJournalCodecOffsetInRingSpan(checkpoint->FileOffset,
            RegionOffset + HeaderRegionSize, NextRegionOffset))
        {
            continue;
        }
        alignedLength = CdpJournalCodecAlignUp64(checkpoint->DataLength,
            SectorSize);
        if (relocation > MAXUINT64 - alignedLength)
            return STATUS_INTEGER_OVERFLOW;
        relocation += alignedLength;
    }
    if (NewCheckpointBytes > MAXUINT64 - relocation)
        return STATUS_INTEGER_OVERFLOW;
    total = NewCheckpointBytes + relocation;
    if (total != 0 && total > UsableEnd - UsableStart)
        return STATUS_DISK_FULL;
    if (total != 0 && total > UsableEnd - PayloadRegionOffset)
        wrapPadding = UsableEnd - PayloadRegionOffset;
    if (PayloadBytesUsed > MAXUINT64 - wrapPadding ||
        PayloadBytesUsed + wrapPadding > MAXUINT64 - total)
    {
        return STATUS_INTEGER_OVERFLOW;
    }
    *RelocationBytes = relocation;
    *WrapPaddingBytes = wrapPadding;
    *ReservedBytes = total;
    return STATUS_SUCCESS;
}

NTSTATUS CdpJournalCodecAppendCheckpointRemap(
    PCdp_CHECKPOINT_REMAP* Remaps, PULONG Count, PULONG Capacity,
    UINT64 VolumeOffset, UINT64 FileOffset, ULONG DataLength)
{
    PCdp_CHECKPOINT_REMAP grown;
    ULONG newCapacity;
    ULONG index;

    if (!Remaps || !Count || !Capacity || *Count > *Capacity)
        return STATUS_INVALID_PARAMETER;
    if (*Count == *Capacity)
    {
        newCapacity = *Capacity == 0 ? 8 : *Capacity * 2;
        if (newCapacity < *Capacity ||
            newCapacity > MAXULONG / sizeof(**Remaps))
        {
            return STATUS_INTEGER_OVERFLOW;
        }
        grown = (PCdp_CHECKPOINT_REMAP)CdpJournalCodecAlloc(
            newCapacity * sizeof(*grown));
        if (!grown)
            return STATUS_INSUFFICIENT_RESOURCES;
        for (index = 0; index < *Count; ++index)
            grown[index] = (*Remaps)[index];
        if (*Remaps)
            CdpJournalCodecFree(*Remaps);
        *Remaps = grown;
        *Capacity = newCapacity;
    }
    (*Remaps)[*Count].VolumeOffset = VolumeOffset;
    (*Remaps)[*Count].FileOffset = FileOffset;
    (*Remaps)[*Count].PreviousFileOffset = FileOffset;
    (*Remaps)[*Count].DataLength = DataLength;
    (*Count)++;
    return STATUS_SUCCESS;
}
