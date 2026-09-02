#pragma once

#ifdef Cdp_USERMODE
#include "cdp_portable.h"
#include "cdp_store.h"
#else
#include "..\CdpCore\include\cdp_portable.h"
#include "..\CdpCore\include\cdp_store.h"
#endif

#define Cdp_JOURNAL_MAGIC            0x4C4E4A51UL /* 'QJNL' */
#define Cdp_JOURNAL_VERSION          17UL
#define Cdp_JOURNAL_MAX_RECORD_DATA  (2UL * 1024UL * 1024UL)
#define Cdp_JOURNAL_HEADER_REGION_SIZE (1UL * 1024UL * 1024UL)
#define Cdp_JOURNAL_HEADER_LINK_SIZE 32UL
#define Cdp_JOURNAL_PAYLOAD_REGION_CAPACITY_DIVISOR 10ULL
#define Cdp_JOURNAL_FLAG_RECOVERY_PENDING 0x00000001UL
#define Cdp_JOURNAL_FLAG_CREDENTIAL_CONFIGURED 0x00000002UL
// Reserved compatibility bit used by an earlier AutoChk experiment. New
// recovery intents do not set it and reboot recovery performs no filesystem
// repair.
#define Cdp_JOURNAL_FLAG_RECOVERY_FS_REPAIR_PENDING 0x00000004UL
#define Cdp_JOURNAL_FLAG_RESTORE_POINT_SET 0x00000008UL
// Boot-success acknowledgement for a persistent restore point. Setting a
// restore point and a successful user-mode boot acknowledgement set this bit.
// The driver durably clears it before publishing each restore boot. A cleared
// bit on the next boot means that the previous boot was not acknowledged and
// its current Journal view must first be materialized to the source baseline.
#define Cdp_JOURNAL_FLAG_RESTORE_BOOT_PENDING 0x00000010UL
#define Cdp_JOURNAL_RECORD_INDEX_MASK     0x0000FFFFUL
#define Cdp_JOURNAL_RECORD_FLAGS_MASK     0xFFFF0000UL
// Highest Sequence bit selects the branch-record interpretation.
#define Cdp_JOURNAL_RECORD_FLAG_BRANCH    0x80000000UL
// Internal persistent tombstone. The slot keeps its global Sequence reserved
// but is absent from runtime trees, time ranges and record queries.
#define Cdp_JOURNAL_RECORD_FLAG_DELETED   0x40000000UL
// Runtime/query-only annotation: the branch header is the per-region
// continuation marker, rather than this branch's creation marker.
#define Cdp_JOURNAL_RECORD_FLAG_BRANCH_CONTINUATION 0x20000000UL
#define Cdp_JOURNAL_BRANCH_RECORD_FLAG_FIRST        0UL
#define Cdp_JOURNAL_BRANCH_RECORD_FLAG_CONTINUATION 1UL
#define Cdp_CREDENTIAL_KDF_PBKDF2_SHA256 1UL
#define Cdp_CREDENTIAL_SALT_BYTES 16UL
#define Cdp_CREDENTIAL_VERIFIER_BYTES 32UL
#define Cdp_CREDENTIAL_DEFAULT_ITERATIONS 200000UL

#pragma pack(push, 1)

// Per-record header stored in 1MB header regions (32 bytes).
typedef struct _Cdp_JOURNAL_RECORD_HEADER
{
	UINT64 WallClock100ns; // 8  UTC Unix seconds (v17; field offset preserved)
	UINT64 VolumeOffset;   // 8  absolute byte offset on the source physical disk
	UINT64 FileOffset;     // 8  payload offset inside the CDP partition
	ULONG DataLength;      // 4
	// Low 16 bits: zero-based index within this header region.
	// High 16 bits: Cdp_JOURNAL_RECORD_FLAG_*.
	ULONG Sequence;
} Cdp_JOURNAL_RECORD_HEADER, *PCdp_JOURNAL_RECORD_HEADER;

C_ASSERT(sizeof(Cdp_JOURNAL_RECORD_HEADER) == 32);

// On-disk interpretation of Cdp_JOURNAL_RECORD_HEADER when Sequence has
// Cdp_JOURNAL_RECORD_FLAG_BRANCH. It occupies exactly the same 32 bytes:
// BranchNumber/ParentBranchNumber overlay VolumeOffset,
// InheritedRecordSequence overlays FileOffset, and Reserved overlays
// DataLength. Reserved is FIRST (0) only at branch creation; CONTINUATION (1)
// marks the otherwise identical branch header at the start of later regions.
// Branch records have no payload.
typedef struct _Cdp_JOURNAL_BRANCH_RECORD_HEADER
{
	UINT64 WallClock100ns;
	LONG BranchNumber;
	LONG ParentBranchNumber;
	UINT64 InheritedRecordSequence;
	ULONG Reserved;
	ULONG Sequence;
} Cdp_JOURNAL_BRANCH_RECORD_HEADER, *PCdp_JOURNAL_BRANCH_RECORD_HEADER;

C_ASSERT(sizeof(Cdp_JOURNAL_BRANCH_RECORD_HEADER) ==
	sizeof(Cdp_JOURNAL_RECORD_HEADER));

// Last 32 bytes of each 1MB header region.
typedef struct _Cdp_HEADER_REGION_LINK
{
	UINT64 PrevRegionOff;  // previous header region (may be self)
	UINT64 NextRegionOff;  // next header region (may be self)
	UINT64 StartSequence;  // global sequence of header[0]
	UINT64 Reserved;       // must be zero
} Cdp_HEADER_REGION_LINK, *PCdp_HEADER_REGION_LINK;

C_ASSERT(sizeof(Cdp_HEADER_REGION_LINK) == 32);

typedef struct _Cdp_CREDENTIAL_DESCRIPTOR
{
	GUID CredentialId;
	ULONG KdfAlgorithm;
	ULONG KdfIterations;
	UCHAR Salt[Cdp_CREDENTIAL_SALT_BYTES];
	UCHAR Verifier[Cdp_CREDENTIAL_VERIFIER_BYTES];
	UINT64 AuthEpoch;
} Cdp_CREDENTIAL_DESCRIPTOR, *PCdp_CREDENTIAL_DESCRIPTOR;

// On-disk layout (v15): VolumeOffset uses the absolute physical-disk address;
// FileOffset remains relative to the journal partition's own storage backend.
// One superblock is followed by alternating header/payload areas.
//   [Superblock]
//   [HeaderRegion0 1MB][Payload0 ...]
//   [HeaderRegion1 1MB][Payload1 ...]
//   ...
typedef struct _Cdp_JOURNAL_SUPERBLOCK
{
	ULONG Magic;
	ULONG Version;
	ULONG SectorSize;
	ULONG Flags;
	UINT64 PartitionSize;
	UINT64 LastHeaderRegionOff; // newest 1MB header region
	GUID SourceVolumeGuid;
	ULONG Crc32c;
	// Kept after the legacy CRC to preserve the superblock field ordering.
	UINT64 RecoveryTargetTime100ns;
	ULONG RecoveryCrc32c;
	Cdp_CREDENTIAL_DESCRIPTOR Credential;
	LONG CurrentBranchNumber;
	LONG HighestBranchNumber;
	/* Stable pre-mount identity.  Volume GUID may not yet be available while
	 * IRP_MN_START_DEVICE is held, so automatic discovery can instead validate
	 * the complete physical source/journal layout on the same disk. */
	ULONG DiskPartitionStyle;
	ULONG MbrSignature;
	GUID DiskGuid;
	UINT64 SourcePartitionStart;
	UINT64 SourcePartitionSize;
	UINT64 JournalPartitionStart;
	UINT64 JournalPartitionSize;
	ULONG MetadataCrc32c;
	/* Appended in v15 so every v14 field and CRC retains its old offset. */
	UINT64 RestorePointTime100ns;
	ULONG RestorePointCrc32c;
} Cdp_JOURNAL_SUPERBLOCK, *PCdp_JOURNAL_SUPERBLOCK;

#pragma pack(pop)

// Decoded runtime/query record. Unlike the on-disk header, Sequence is the
// 64-bit global value: RegionLink.StartSequence +
// (Header.Sequence & Cdp_JOURNAL_RECORD_INDEX_MASK).
typedef struct _Cdp_JOURNAL_RECORD
{
	UINT64 WallClock100ns;
	UINT64 VolumeOffset;
	UINT64 FileOffset;
	UINT64 Sequence;
	ULONG DataLength;
	ULONG Flags; // Cdp_JOURNAL_RECORD_FLAG_* from the header high 16 bits
} Cdp_JOURNAL_RECORD, *PCdp_JOURNAL_RECORD;

C_ASSERT(sizeof(Cdp_JOURNAL_RECORD) == 40);

#define Cdp_JOURNAL_HEADERS_PER_REGION \
	((Cdp_JOURNAL_HEADER_REGION_SIZE - Cdp_JOURNAL_HEADER_LINK_SIZE) / \
		sizeof(Cdp_JOURNAL_RECORD_HEADER))

C_ASSERT(Cdp_JOURNAL_HEADERS_PER_REGION == 32767);
C_ASSERT(Cdp_JOURNAL_HEADERS_PER_REGION <=
	Cdp_JOURNAL_RECORD_INDEX_MASK + 1UL);

typedef struct _Cdp_BRANCH_RECORD_INFO
{
	UINT64 Sequence;
	UINT64 WallClock100ns;
	UINT64 HeaderRegionOffset;
	ULONG HeaderIndex;
} Cdp_BRANCH_RECORD_INFO, *PCdp_BRANCH_RECORD_INFO;

// Runtime-only branch topology. Parent/FirstChild/NextSibling form the
// ancestry tree; Previous/Next preserve branch creation order for target-time
// lookup without allocating a per-record index.
typedef struct _Cdp_BRANCH_INFO_NODE
{
	LONG BranchNumber;
	LONG ParentBranchNumber;
	UINT64 InheritedRecordSequence;
	Cdp_BRANCH_RECORD_INFO StartRecord;
	Cdp_BRANCH_RECORD_INFO EndRecord;
	// Latest durable FIRST/CONTINUATION marker. EndRecord also advances for
	// ordinary records, so it cannot answer whether a later RR anchors branch.
	Cdp_BRANCH_RECORD_INFO LatestAnchorRecord;
	UINT64 LiveRecordCount;
	UINT64 CompactScanRecords; // transient count produced by oldest-RR scan
	BOOLEAN Latest;
	BOOLEAN SyntheticStart;
	BOOLEAN PrunePending; // transient mark used only while compaction holds Lock
	struct _Cdp_BRANCH_INFO_NODE* Parent;
	struct _Cdp_BRANCH_INFO_NODE* FirstChild;
	struct _Cdp_BRANCH_INFO_NODE* NextSibling;
	struct _Cdp_BRANCH_INFO_NODE* Previous;
	struct _Cdp_BRANCH_INFO_NODE* Next;
} Cdp_BRANCH_INFO_NODE, *PCdp_BRANCH_INFO_NODE;

typedef struct _Cdp_BRANCH_INFO_TREE
{
	PCdp_BRANCH_INFO_NODE Root;
	PCdp_BRANCH_INFO_NODE First;
	PCdp_BRANCH_INFO_NODE Last;
	PCdp_BRANCH_INFO_NODE Latest;
	ULONG Count;
} Cdp_BRANCH_INFO_TREE, *PCdp_BRANCH_INFO_TREE;

// Runtime-only checkpoint coverage.  Persistent restore-point boots use the
// source volume as their durable baseline, so these payload mappings are
// intentionally not serialized in the superblock or record headers.
typedef struct _Cdp_RUNTIME_CHECKPOINT
{
	UINT64 CheckpointId;
	UINT64 SourceRegionOffset;
	UINT64 SourceFirstSequence;
	UINT64 SourceEndSequence;
	UINT64 VolumeOffset;
	UINT64 FileOffset;
	ULONG DataLength;
	struct _Cdp_RUNTIME_CHECKPOINT* Next;
} Cdp_RUNTIME_CHECKPOINT, *PCdp_RUNTIME_CHECKPOINT;

typedef struct _Cdp_CHECKPOINT_REMAP
{
	UINT64 VolumeOffset;
	UINT64 FileOffset;
	UINT64 PreviousFileOffset;
	ULONG DataLength;
} Cdp_CHECKPOINT_REMAP, *PCdp_CHECKPOINT_REMAP;

typedef struct _Cdp_RUNTIME_CHECKPOINT_TREE_INFO
{
	UINT64 CheckpointId;
	UINT64 SourceRegionOffset;
	UINT64 SourceFirstSequence;
	UINT64 SourceEndSequence;
	UINT64 DataBytes;
	UINT64 AllocatedBytes;
	ULONG RecordCount;
	ULONG Reserved;
} Cdp_RUNTIME_CHECKPOINT_TREE_INFO, *PCdp_RUNTIME_CHECKPOINT_TREE_INFO;

typedef struct _Cdp_RUNTIME_CHECKPOINT_RECORD_TREE_INFO
{
	UINT64 CheckpointId;
	UINT64 RecordIndex;
	UINT64 VolumeOffset;
	UINT64 FileOffset;
	ULONG DataLength;
	ULONG AllocatedLength;
} Cdp_RUNTIME_CHECKPOINT_RECORD_TREE_INFO,
	*PCdp_RUNTIME_CHECKPOINT_RECORD_TREE_INFO;

// Snapshot-safe public projection of an in-memory BranchTree node.  This
// deliberately contains topology only; no record-header scan is required.
#define Cdp_JOURNAL_BRANCH_INFO_FLAG_CURRENT   0x00000001UL
#define Cdp_JOURNAL_BRANCH_INFO_FLAG_SYNTHETIC 0x00000002UL
typedef struct _Cdp_JOURNAL_BRANCH_TREE_INFO
{
	LONG BranchNumber;
	LONG ParentBranchNumber;
	UINT64 InheritedRecordSequence;
	UINT64 CreatedWallClock100ns;
	UINT64 StartSequence;
	UINT64 EndSequence;
	ULONG Flags;
	ULONG Reserved;
} Cdp_JOURNAL_BRANCH_TREE_INFO, *PCdp_JOURNAL_BRANCH_TREE_INFO;

typedef struct _Cdp_JOURNAL
{
	BOOLEAN Mounted;
	ULONG SectorSize;
	UINT64 PartitionSize;

	UINT64 LastHeaderRegionOff;
	UINT64 PayloadRegionOff;

	UINT64 OldestHeaderRegionOff;
	UINT64 CurrentHeaderRegionStartSequence;
	ULONG OldestHeaderIndex;
	ULONG CurrentHeaderCount;
	// Transfer size used for a 1MB header region. Formatting discovers it
	// from the largest successful write; preview/recovery scans reuse it for
	// reads instead of issuing one sector read per 32-byte record header.
	ULONG HeaderRegionWriteChunk;
	// Lazily allocated aligned 1MB scan buffer. Journal.Lock serializes all
	// users; retained across Preview/Recovery builds and freed by Close.
	PVOID HeaderScanAllocationBase;
	PUCHAR HeaderScanBuffer;
	// One-sector write-through cache for 32-byte record headers. Header
	// append is sequential, so retaining the current sector avoids a disk
	// read and an allocation for every record while preserving the existing
	// per-record sector write + flush durability boundary.
	PVOID HeaderWriteAllocationBase;
	PUCHAR HeaderWriteBuffer;
	UINT64 HeaderWriteSectorOffset;
	BOOLEAN HeaderWriteCacheValid;
	BOOLEAN HeaderWriteCacheDirty;
	UINT64 NextSequence;
	UINT64 TotalRecords;
	// Maintained under Lock. Mount reconstructs this once; hot merge-threshold
	// checks use it directly and never walk Header region links.
	UINT64 ActiveHeaderRegionCount;
	UINT64 PayloadBytesUsed;
	UINT64 RecordGeneration;
	// One-RR compaction snapshot. BuildCurrentBranchRegionTree fills this while
	// performing the only permitted Record Header scan; DeleteOldestRegion
	// consumes it instead of scanning the same RR again.
	UINT64 CompactScanGeneration;
	UINT64 CompactScanRegionOffset;
	UINT64 CompactScanFirstSequence;
	UINT64 CompactScanEndSequence;
	UINT64 CompactScanLiveRecords;
	BOOLEAN CompactScanValid;
	UINT64 Oldest100ns;
	UINT64 Newest100ns;
	LONG CurrentBranchNumber;
	LONG HighestBranchNumber;
	Cdp_BRANCH_INFO_TREE BranchTree;
	PCdp_RUNTIME_CHECKPOINT CheckpointFirst;
	PCdp_RUNTIME_CHECKPOINT CheckpointLast;
	UINT64 NextCheckpointId;
	UINT64 CheckpointGeneration;
	ULONG CheckpointCount;
	ULONG CheckpointRecordCount;
	UINT64 CheckpointPayloadBytes;
	BOOLEAN RecoveryPending;
	BOOLEAN RestorePointSet;
	BOOLEAN RestoreBootPending;
	// Auto discovery may intentionally leave the old history opaque when a
	// persistent restore point makes it irrelevant to the boot view.  The
	// first protected write resets that history before appending anything.
	BOOLEAN HistoryScanSkipped;
	// Preserved only for v14/v15 on-disk compatibility; current recovery never
	// sets or acts on the retired filesystem-repair flag.
	BOOLEAN RecoveryFsRepairPending;
	BOOLEAN SuperblockDirty;
	UINT64 RecoveryTargetTime100ns;
	UINT64 RestorePointTime100ns;
	BOOLEAN CredentialConfigured;
	Cdp_CREDENTIAL_DESCRIPTOR Credential;
	GUID SourceVolumeGuid;
	ULONG DiskPartitionStyle;
	ULONG MbrSignature;
	GUID DiskGuid;
	UINT64 SourcePartitionStart;
	UINT64 SourcePartitionSize;
	UINT64 JournalPartitionStart;
	UINT64 JournalPartitionSize;
#ifndef Cdp_USERMODE
	PDEVICE_OBJECT TargetDevice;
	// Optional volume-stack backend used only for journal metadata. Payload
	// remains on TargetDevice with an absolute disk offset.
	PDEVICE_OBJECT MetadataTargetDevice;
#else
	PVOID TargetDevice;
	PVOID MetadataTargetDevice;
#endif
	PVOID RawDiskHandle; // kernel HANDLE; physical disk backend when non-NULL
	UINT64 TargetBaseOffset; // partition start on RawDiskHandle
	UINT64 MetadataTargetBaseOffset; // normally zero for a partition volume
	PCdp_STORE Store; // if set, RawIo uses store instead of TargetDevice
	/* Set when the owning physical disk accepts a non-D0 device-power IRP.
	 * No new raw Journal request may be issued after that point: its target
	 * stack can already be powering down. */
	volatile LONG RawIoQuiesced;
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
	UINT64 Sequence;
	UINT64 MinValidSequence; // subtree minimum Sequence; MAXUINT64 when none
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

VOID CdpJournalSetMetadataDevice(
	_Inout_ PCdp_JOURNAL Journal,
	_In_opt_ PVOID TargetDevice,
	_In_ UINT64 TargetBaseOffset);

VOID CdpJournalSetPhysicalLayout(
	_Inout_ PCdp_JOURNAL Journal,
	_In_ ULONG DiskPartitionStyle,
	_In_ ULONG MbrSignature,
	_In_ const GUID* DiskGuid,
	_In_ UINT64 SourcePartitionStart,
	_In_ UINT64 SourcePartitionSize,
	_In_ UINT64 JournalPartitionStart,
	_In_ UINT64 JournalPartitionSize);

NTSTATUS CdpJournalFormat(_Inout_ PCdp_JOURNAL Journal);

NTSTATUS CdpJournalMount(_Inout_ PCdp_JOURNAL Journal);

// Auto discovery only: when a restore point is present (and no recovery
// intent takes precedence), mount from the superblock without walking Record
// headers.  The journal remains reset-only until the restore boot is prepared.
NTSTATUS CdpJournalMountForAutoDiscovery(_Inout_ PCdp_JOURNAL Journal);

// Append a branch marker. BranchNumber must be the next monotonically
// increasing number. Parent 0 means no parent and requires inherit sequence 0.
NTSTATUS CdpJournalAppendBranch(
	_Inout_ PCdp_JOURNAL Journal,
	_In_ LONG BranchNumber,
	_In_ LONG ParentBranchNumber,
	_In_ UINT64 InheritedRecordSequence);

// Resolve the branch active at TargetTime and its last included record. The
// returned sequence is suitable as a new child branch inheritance point.
NTSTATUS CdpJournalResolveTargetBranch(
	_Inout_ PCdp_JOURNAL Journal,
	_In_ UINT64 TargetTime100ns,
	_Out_ PLONG BranchNumber,
	_Out_ PUINT64 InheritedRecordSequence);

// Preview-only target settling.  First resolve the last valid record at or
// before the requested UTC Unix second, then inspect the following seconds.
// Accept a contiguous run only when an empty second is found within
// MaxLookaheadSeconds.  A fully occupied window falls back to that original
// record.  Sibling branches and marker/deleted records are ignored.
NTSTATUS CdpJournalResolveSettledPreviewTime(
	_Inout_ PCdp_JOURNAL Journal,
	_In_ UINT64 RequestedTime100ns,
	_In_ ULONG MaxLookaheadSeconds,
	_Out_ PUINT64 SettledTime100ns);

// Undo the newest empty branch marker. Used only when Recovery created a
// branch but failed before publishing its replacement MetaTree.
NTSTATUS CdpJournalRollbackLatestBranch(
	_Inout_ PCdp_JOURNAL Journal,
	_In_ LONG BranchNumber);

NTSTATUS CdpJournalSetRecoveryIntent(
	_Inout_ PCdp_JOURNAL Journal,
	_In_ UINT64 TargetTime100ns);

NTSTATUS CdpJournalClearRecoveryIntent(_Inout_ PCdp_JOURNAL Journal);

// Clears the active reboot-recovery intent after the delayed branch has been
// persisted. Reserved compatibility flags are left unchanged.
NTSTATUS CdpJournalCompleteRecoveryIntent(_Inout_ PCdp_JOURNAL Journal);

NTSTATUS CdpJournalSetRestorePoint(
	_Inout_ PCdp_JOURNAL Journal,
	_In_ UINT64 TargetTime100ns);

// Start one restore boot. Returns the previously persisted boot-success
// acknowledgement and durably clears it before any restored-system write.
NTSTATUS CdpJournalBeginRestoreBoot(
	_Inout_ PCdp_JOURNAL Journal,
	_Out_ PBOOLEAN PreviousBootConfirmed);

// User-mode service acknowledgement after Windows has started successfully.
NTSTATUS CdpJournalConfirmRestoreBoot(_Inout_ PCdp_JOURNAL Journal);

NTSTATUS CdpJournalClearRestorePoint(_Inout_ PCdp_JOURNAL Journal);

// Discard every retained record and create a new root branch while retaining
// the persistent restore-point marker itself.
NTSTATUS CdpJournalResetHistoryPreserveRestorePoint(
	_Inout_ PCdp_JOURNAL Journal);

NTSTATUS CdpJournalSetCredential(
	_Inout_ PCdp_JOURNAL Journal,
	_In_ const Cdp_CREDENTIAL_DESCRIPTOR* Credential);

BOOLEAN CdpJournalGetCredential(
	_In_ PCdp_JOURNAL Journal,
	_Out_ PCdp_CREDENTIAL_DESCRIPTOR Credential);

// Clear on-disk superblock magic so auto-discovery will not remount this journal.
NTSTATUS CdpJournalInvalidate(_Inout_ PCdp_JOURNAL Journal);

NTSTATUS CdpJournalAppend(
	_Inout_ PCdp_JOURNAL Journal,
	_In_ UINT64 VolumeOffset,
	_In_ ULONG DataLength,
	_In_reads_bytes_(DataLength) const VOID* AfterImage,
	_Out_opt_ PCdp_JOURNAL_RECORD WrittenRecord);

NTSTATUS CdpJournalAppendEx(
	_Inout_ PCdp_JOURNAL Journal,
	_In_ UINT64 VolumeOffset,
	_In_ ULONG DataLength,
	_In_reads_bytes_(DataLength) const VOID* AfterImage,
	_In_ ULONG RecordFlags,
	_Out_opt_ PCdp_JOURNAL_RECORD WrittenRecord);

// Serialize a durability barrier with journal append transactions.
NTSTATUS CdpJournalFlushBuffers(_Inout_ PCdp_JOURNAL Journal);

NTSTATUS CdpJournalQueryTimeRange(
	_Inout_ PCdp_JOURNAL Journal,
	_Out_ PUINT64 OldestTime100ns,
	_Out_ PUINT64 NewestTime100ns);

// Payload-space accounting excludes the superblock and active 1MB header
// regions. Used bytes are the occupied ring spans, including any tail gap
// skipped when a payload allocation wraps to the usable-area start.
NTSTATUS CdpJournalQueryUsage(
	_Inout_ PCdp_JOURNAL Journal,
	_Out_ PUINT64 PartitionBytes,
	_Out_ PUINT64 MetadataBytes,
	_Out_ PUINT64 PayloadBytesUsed,
	_Out_ PUINT64 PayloadBytesFree,
	_Out_ PUINT64 TotalRecords);

// Return the global sequence range owned by the oldest complete header
// region. The active/newest region is never returned for compaction.
NTSTATUS CdpJournalGetOldestCompactableRegion(
	_Inout_ PCdp_JOURNAL Journal,
	_Out_ PUINT64 RegionOffset,
	_Out_ PUINT64 FirstSequence,
	_Out_ PUINT64 EndSequence);

// Delete the expected oldest complete region after its live current-branch
// values have been materialized to the source volume.
NTSTATUS CdpJournalDeleteOldestRegion(
	_Inout_ PCdp_JOURNAL Journal,
	_In_ UINT64 ExpectedRegionOffset);

// Merge one already-deduplicated logical range into runtime checkpoints.
// Existing checkpoints are inspected in creation order and consume matching
// fragments.  Only fragments still uncovered after that pass allocate new
// payload.  Returned remaps describe every resulting payload fragment.
NTSTATUS CdpJournalMergeIntoRuntimeCheckpoints(
	_Inout_ PCdp_JOURNAL Journal,
	_In_ UINT64 SourceRegionOffset,
	_In_ UINT64 SourceFirstSequence,
	_In_ UINT64 SourceEndSequence,
	_Inout_ PUINT64 CheckpointId,
	_In_ UINT64 VolumeOffset,
	_In_ ULONG DataLength,
	_In_reads_bytes_(DataLength) const VOID* Data,
	_Outptr_result_buffer_(*RemapCount) PCdp_CHECKPOINT_REMAP* Remaps,
	_Out_ PULONG RemapCount);

VOID CdpJournalFreeCheckpointRemaps(
	_Inout_opt_ PCdp_CHECKPOINT_REMAP Remaps);

// Before reclaiming a physical RR span, move any runtime checkpoint payload
// that happens to reside in that span to the current free cursor.
NTSTATUS CdpJournalRelocateCheckpointsFromRegion(
	_Inout_ PCdp_JOURNAL Journal,
	_In_ UINT64 RegionOffset,
	_Outptr_result_buffer_(*RemapCount) PCdp_CHECKPOINT_REMAP* Remaps,
	_Out_ PULONG RemapCount);

NTSTATUS CdpJournalSnapshotRuntimeCheckpoints(
	_Inout_ PCdp_JOURNAL Journal,
	_Outptr_result_buffer_(*CheckpointCount) PCdp_CHECKPOINT_REMAP* Checkpoints,
	_Out_ PULONG CheckpointCount);

NTSTATUS CdpJournalQueryRuntimeCheckpointInfos(
	_Inout_ PCdp_JOURNAL Journal,
	_In_ UINT64 StartIndex,
	_In_ UINT64 ExpectedGeneration,
	_Out_writes_to_(InfoCapacity, *ReturnedCount)
		PCdp_RUNTIME_CHECKPOINT_TREE_INFO Infos,
	_In_ ULONG InfoCapacity,
	_Out_ PUINT64 TotalCheckpoints,
	_Out_ PUINT64 Generation,
	_Out_ PULONG ReturnedCount);

NTSTATUS CdpJournalQueryRuntimeCheckpointRecords(
	_Inout_ PCdp_JOURNAL Journal,
	_In_ UINT64 CheckpointId,
	_In_ UINT64 StartIndex,
	_In_ UINT64 ExpectedGeneration,
	_Out_writes_to_(RecordCapacity, *ReturnedCount)
		PCdp_RUNTIME_CHECKPOINT_RECORD_TREE_INFO Records,
	_In_ ULONG RecordCapacity,
	_Out_ PUINT64 TotalRecords,
	_Out_ PUINT64 Generation,
	_Out_ PULONG ReturnedCount);

VOID CdpJournalClearRuntimeCheckpoints(_Inout_ PCdp_JOURNAL Journal);

// After one materializing compaction, reclaim every immediately following
// complete RR whose headers are all tombstones. Never skips a retained RR and
// never removes the active/newest RR.
NTSTATUS CdpJournalDeleteContiguousTombstonedRegions(
	_Inout_ PCdp_JOURNAL Journal,
	_Out_ PULONG DeletedRegionCount);

// The source has been materialized to SequenceInclusive.  Reclaim every
// wholly obsolete oldest RR and tombstone obsolete data headers in the first
// retained RR.  Structural branch headers in a partially retained RR remain
// so later records keep a durable branch identity.
NTSTATUS CdpJournalDeleteRecordsThroughSequence(
	_Inout_ PCdp_JOURNAL Journal,
	_In_ UINT64 SequenceInclusive,
	_Out_ PULONG DeletedRegionCount,
	_Out_ PULONG TombstonedRecordCount);

// If the compacted range contains a branch inheritance point, tombstone all
// records and branch markers unreachable from the current branch. Otherwise
// this is a no-op. PreviewTargetDeleted reports whether the active preview
// anchor was removed by the reachability cleanup.
NTSTATUS CdpJournalPruneUnreachableForCompaction(
	_Inout_ PCdp_JOURNAL Journal,
	_In_ UINT64 FirstSequence,
	_In_ UINT64 EndSequence,
	_In_ UINT64 PreviewTargetSequence,
	_Out_opt_ PBOOLEAN PreviewTargetDeleted);

// Read retained records in chronological order. Records contain only
// record metadata; callers never receive journal payload data.  The caller
// can page with StartIndex and must echo Generation after the first page to
// detect concurrent capture/eviction.
NTSTATUS CdpJournalQueryRecordHeaders(
	_Inout_ PCdp_JOURNAL Journal,
	_In_ UINT64 StartIndex,
	_In_ UINT64 ExpectedGeneration,
	_Out_writes_to_(RecordCapacity, *ReturnedCount) PCdp_JOURNAL_RECORD Records,
	_In_ ULONG RecordCapacity,
	_Out_ PUINT64 TotalRecords,
	_Out_ PUINT64 Generation,
	_Out_ PULONG ReturnedCount);

// Resolve a live record's global Sequence to the same zero-based logical
// Index printed by CdpConsole command 'l', plus its physical RR location.
NTSTATUS CdpJournalFindRecordLocationBySequence(
	_Inout_ PCdp_JOURNAL Journal,
	_In_ UINT64 RecordSequence,
	_Out_ PUINT64 RecordIndex,
	_Out_ PUINT64 RecordTime100ns,
	_Out_ PUINT64 HeaderRegionOffset,
	_Out_ PULONG HeaderIndex);

// Query the retained in-memory branch topology in creation order. Generation
// is the journal record generation and makes multi-page snapshots coherent.
NTSTATUS CdpJournalQueryBranches(
	_Inout_ PCdp_JOURNAL Journal,
	_In_ UINT64 StartIndex,
	_In_ UINT64 ExpectedGeneration,
	_Out_writes_to_(BranchCapacity, *ReturnedCount) PCdp_JOURNAL_BRANCH_TREE_INFO Branches,
	_In_ ULONG BranchCapacity,
	_Out_ PULONG TotalBranches,
	_Out_ PLONG CurrentBranchNumber,
	_Out_ PUINT64 Generation,
	_Out_ PULONG ReturnedCount);

// Build the latest-value interval map for Journal->CurrentBranchNumber.
// Headers are scanned once from newest to oldest. Existing (newer) tree
// coverage wins; branch markers restrict the scan to the current ancestry.
NTSTATUS CdpJournalBuildCurrentBranchTree(
	_Inout_ PCdp_JOURNAL Journal,
	_Out_ PCdp_PREVIEW_TREE Tree);

// Build only the current-branch records in [FirstSequence, EndSequence),
// keeping the newest value per byte within that sequence range itself.
NTSTATUS CdpJournalBuildCurrentBranchRegionTree(
	_Inout_ PCdp_JOURNAL Journal,
	_In_ UINT64 FirstSequence,
	_In_ UINT64 EndSequence,
	_Out_ PCdp_PREVIEW_TREE Tree);

NTSTATUS CdpJournalBuildPreviewTree(
	_Inout_ PCdp_JOURNAL Journal,
	_In_ UINT64 TargetTime100ns,
	_In_ UINT64 MaxSequence,
	_In_ BOOLEAN IncludeTargetTime,
	_Out_ PCdp_PREVIEW_TREE Tree,
	_Out_opt_ PUINT64 TargetRecordSequence);

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
	_In_ const Cdp_JOURNAL_RECORD* Record);

// Replace existing overlapping coverage with a newly appended after-image.
// Caller serializes access to Tree.
NTSTATUS CdpPreviewTreeOverlayLatest(
	_Inout_ PCdp_PREVIEW_TREE Tree,
	_In_ const Cdp_JOURNAL_RECORD* Record);

// Remove only the intersecting byte range from the history tree.  Remaining
// left/right fragments keep their original journal payload offsets.
NTSTATUS CdpPreviewTreePunchRange(
	_Inout_ PCdp_PREVIEW_TREE Tree,
	_In_ UINT64 VolumeOffset,
	_In_ ULONG DataLength);

// Update only nodes owned by ExpectedSequence.  Intersections owned by newer
// records remain unchanged; partial nodes are split as required.
NTSTATUS CdpPreviewTreeRemapSequenceRange(
	_Inout_ PCdp_PREVIEW_TREE Tree,
	_In_ UINT64 ExpectedSequence,
	_In_ UINT64 VolumeOffset,
	_In_ ULONG DataLength,
	_In_ UINT64 NewFileOffset);

NTSTATUS CdpPreviewTreeRemapPayloadRange(
	_Inout_ PCdp_PREVIEW_TREE Tree,
	_In_ UINT64 VolumeOffset,
	_In_ ULONG DataLength,
	_In_ UINT64 ExpectedFileOffset,
	_In_ UINT64 NewFileOffset);

// Caller serializes Tree. Verify that every byte in [VolumeOffset, end) maps
// to the expected record identity and consecutive payload bytes.
BOOLEAN CdpPreviewTreeValidateMapping(
	_In_ PCdp_PREVIEW_TREE Tree,
	_In_ UINT64 VolumeOffset,
	_In_ ULONG DataLength,
	_In_ UINT64 ExpectedSequence,
	_In_ UINT64 ExpectedFileOffset,
	_Out_ PUINT64 FirstMismatch,
	_Out_opt_ PUINT64 ActualSequence,
	_Out_opt_ PUINT64 ActualFileOffset);

VOID CdpJournalClose(_Inout_ PCdp_JOURNAL Journal);
