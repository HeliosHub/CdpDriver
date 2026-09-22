#pragma once

#include "CdpJournal.h"

typedef struct _Cdp_CORE_COVERAGE_SCAN
{
    UINT64 Start;
    UINT64 End;
    UINT64 Cursor;
    UINT64 FirstGap;
    UINT64 LastGapEnd;
    ULONG GapCount;
    BOOLEAN HasGap;
    BOOLEAN HasCoverage;
} Cdp_CORE_COVERAGE_SCAN, *PCdp_CORE_COVERAGE_SCAN;

PCdp_PREVIEW_TREE_NODE CdpViewPolicyFindFirstOverlapNode(
    PCdp_PREVIEW_TREE_NODE Node,
    UINT64 Start,
    UINT64 End);

PCdp_PREVIEW_TREE_NODE CdpViewPolicyFindFirstValidNode(
    PCdp_PREVIEW_TREE_NODE Node);

PCdp_PREVIEW_TREE_NODE CdpViewPolicyFindNodeBySequenceRange(
    PCdp_PREVIEW_TREE_NODE Node,
    UINT64 FirstSequence,
    UINT64 EndSequence);

NTSTATUS CdpViewPolicyAccumulateCoverage(
    PCdp_PREVIEW_TREE_NODE Node,
    PUINT64 CoverageBytes);

VOID CdpViewPolicyCollectCheckpointMergeRanges(
    PCdp_PREVIEW_TREE_NODE Node,
    PCdp_CHECKPOINT_MERGE_RANGE Ranges,
    ULONG Capacity,
    PULONG Count);

VOID CdpViewPolicyRecordCoverageGap(PCdp_CORE_COVERAGE_SCAN Scan,
    UINT64 Start, UINT64 End);
VOID CdpViewPolicyScanTreeCoverage(PCdp_PREVIEW_TREE_NODE Node,
    PCdp_CORE_COVERAGE_SCAN Scan);
NTSTATUS CdpViewPolicyAccumulateMaterializeBytes(
    PCdp_PREVIEW_TREE_NODE Node,
    PUINT64 TotalBytes);
