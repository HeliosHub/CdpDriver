#include "cdp_view_policy.h"

PCdp_PREVIEW_TREE_NODE CdpViewPolicyFindFirstOverlapNode(
    PCdp_PREVIEW_TREE_NODE Node,
    UINT64 Start,
    UINT64 End)
{
    PCdp_PREVIEW_TREE_NODE found;

    if (!Node || Node->MaxEnd <= Start)
        return NULL;
    found = CdpViewPolicyFindFirstOverlapNode(Node->Left, Start, End);
    if (found)
        return found;
    if (!Node->Invalid && Node->Start < End && Start < Node->End)
        return Node;
    if (Node->Start >= End)
        return NULL;
    return CdpViewPolicyFindFirstOverlapNode(Node->Right, Start, End);
}

PCdp_PREVIEW_TREE_NODE CdpViewPolicyFindFirstValidNode(
    PCdp_PREVIEW_TREE_NODE Node)
{
    PCdp_PREVIEW_TREE_NODE found;

    if (!Node)
        return NULL;
    found = CdpViewPolicyFindFirstValidNode(Node->Left);
    if (found)
        return found;
    if (!Node->Invalid && Node->DataLength != 0)
        return Node;
    return CdpViewPolicyFindFirstValidNode(Node->Right);
}

PCdp_PREVIEW_TREE_NODE CdpViewPolicyFindNodeBySequenceRange(
    PCdp_PREVIEW_TREE_NODE Node,
    UINT64 FirstSequence,
    UINT64 EndSequence)
{
    PCdp_PREVIEW_TREE_NODE found;

    if (!Node)
        return NULL;
    found = CdpViewPolicyFindNodeBySequenceRange(
        Node->Left, FirstSequence, EndSequence);
    if (found)
        return found;
    if (!Node->Invalid && Node->Sequence >= FirstSequence &&
        Node->Sequence < EndSequence)
    {
        return Node;
    }
    return CdpViewPolicyFindNodeBySequenceRange(
        Node->Right, FirstSequence, EndSequence);
}

NTSTATUS CdpViewPolicyAccumulateCoverage(
    PCdp_PREVIEW_TREE_NODE Node,
    PUINT64 CoverageBytes)
{
    NTSTATUS status;
    UINT64 length;

    if (!Node)
        return STATUS_SUCCESS;
    status = CdpViewPolicyAccumulateCoverage(Node->Left, CoverageBytes);
    if (!NT_SUCCESS(status))
        return status;
    if (!Node->Invalid)
    {
        if (Node->End < Node->Start)
            return STATUS_DATA_ERROR;
        length = Node->End - Node->Start;
        if (*CoverageBytes > MAXUINT64 - length)
            return STATUS_INTEGER_OVERFLOW;
        *CoverageBytes += length;
    }
    return CdpViewPolicyAccumulateCoverage(Node->Right, CoverageBytes);
}

VOID CdpViewPolicyCollectCheckpointMergeRanges(
    PCdp_PREVIEW_TREE_NODE Node,
    PCdp_CHECKPOINT_MERGE_RANGE Ranges,
    ULONG Capacity,
    PULONG Count)
{
    if (!Node || *Count >= Capacity)
        return;
    CdpViewPolicyCollectCheckpointMergeRanges(
        Node->Left, Ranges, Capacity, Count);
    if (!Node->Invalid && *Count < Capacity)
    {
        Ranges[*Count].VolumeOffset = Node->Start;
        Ranges[*Count].DataLength = Node->DataLength;
        (*Count)++;
    }
    CdpViewPolicyCollectCheckpointMergeRanges(
        Node->Right, Ranges, Capacity, Count);
}

VOID CdpViewPolicyRecordCoverageGap(PCdp_CORE_COVERAGE_SCAN Scan,
    UINT64 Start, UINT64 End)
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

VOID CdpViewPolicyScanTreeCoverage(PCdp_PREVIEW_TREE_NODE Node,
    PCdp_CORE_COVERAGE_SCAN Scan)
{
    UINT64 nodeStart;
    UINT64 nodeEnd;

    if (!Node || Node->MaxEnd <= Scan->Start || Scan->Cursor >= Scan->End)
        return;
    CdpViewPolicyScanTreeCoverage(Node->Left, Scan);
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
                CdpViewPolicyRecordCoverageGap(
                    Scan, Scan->Cursor, nodeStart);
            if (nodeEnd > Scan->Cursor)
                Scan->Cursor = nodeEnd;
        }
    }
    CdpViewPolicyScanTreeCoverage(Node->Right, Scan);
}

NTSTATUS CdpViewPolicyAccumulateMaterializeBytes(
    PCdp_PREVIEW_TREE_NODE Node,
    PUINT64 TotalBytes)
{
    NTSTATUS status;

    if (!Node)
        return STATUS_SUCCESS;
    status = CdpViewPolicyAccumulateMaterializeBytes(
        Node->Left, TotalBytes);
    if (!NT_SUCCESS(status))
        return status;
    if (!Node->Invalid)
    {
        if (*TotalBytes > MAXUINT64 - Node->DataLength)
            return STATUS_INTEGER_OVERFLOW;
        *TotalBytes += Node->DataLength;
    }
    return CdpViewPolicyAccumulateMaterializeBytes(Node->Right, TotalBytes);
}
