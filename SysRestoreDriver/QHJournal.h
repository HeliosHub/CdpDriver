#pragma once

#include <ntddk.h>

#define QH_JOURNAL_MAGIC          0x4C4E4A51UL /* 'QJNL' */
#define QH_JOURNAL_RECORD_MAGIC   0x43455251UL /* 'QREC' */
#define QH_JOURNAL_VERSION        2UL
#define QH_JOURNAL_FLAG_WRAP      0x00000001UL
#define QH_JOURNAL_MAX_RECORD_DATA (1024UL * 1024UL)
#define QH_JOURNAL_INDEX_SECTORS  256UL
#define QH_JOURNAL_DEFAULT_STRIDE 1024UL

#pragma pack(push, 1)

typedef struct _QH_JOURNAL_SUPERBLOCK
{
	ULONG Magic;
	ULONG Version;
	ULONG SectorSize;
	ULONG Flags;
	UINT64 PartitionSize;
	UINT64 IndexRegionOff;
	UINT64 IndexRegionSize;
	UINT64 LogOffset;
	UINT64 LogSize;
	UINT64 LogHead;
	UINT64 LogTail;
	UINT64 IndexHead;
	UINT64 IndexTail;
	ULONG IndexStride;
	ULONG Pad0;
	UINT64 NextSequence;
	UINT64 DroppedRecords;
	UINT64 OldestRecoverable100ns;
	UINT64 NewestRecoverable100ns;
	GUID SourceVolumeGuid;
	ULONG Crc32c;
} QH_JOURNAL_SUPERBLOCK, *PQH_JOURNAL_SUPERBLOCK;

typedef struct _QH_JOURNAL_RECORD_HEADER
{
	ULONG Magic;
	ULONG RecordSize;
	UINT64 Sequence;
	UINT64 WallClock100ns;
	UINT64 PerformanceCounter;
	GUID SourceVolumeGuid;
	UINT64 VolumeOffset;
	ULONG DataLength;
	ULONG Flags;
	ULONG DataCrc32c;
	ULONG HeaderCrc32c;
} QH_JOURNAL_RECORD_HEADER, *PQH_JOURNAL_RECORD_HEADER;

// Sparse on-disk anchor: one every IndexStride records (CDP §6.5).
typedef struct _QH_INDEX_ANCHOR
{
	UINT64 Sequence;
	UINT64 WallClock100ns;
	UINT64 LogRingOffset;
	ULONG Flags;
	ULONG Crc32c;
} QH_INDEX_ANCHOR, *PQH_INDEX_ANCHOR;

#pragma pack(pop)

// Per-record in-memory index used for O(log n) time lookup.
typedef struct _QH_INDEX_MEM_ENTRY
{
	UINT64 Sequence;
	UINT64 WallClock100ns;
	UINT64 LogRingOffset;
	UINT64 VolumeOffset;
	ULONG DataLength;
	ULONG Pad;
} QH_INDEX_MEM_ENTRY, *PQH_INDEX_MEM_ENTRY;

typedef struct _QH_JOURNAL
{
	BOOLEAN Mounted;
	ULONG SectorSize;
	UINT64 PartitionSize;
	UINT64 IndexRegionOff;
	UINT64 IndexRegionSize;
	UINT64 LogOffset;
	UINT64 LogSize;
	UINT64 LogHead;
	UINT64 LogTail;
	UINT64 IndexHead;
	UINT64 IndexTail;
	ULONG IndexStride;
	ULONG RecsSinceAnchor;
	UINT64 NextSequence;
	UINT64 DroppedRecords;
	UINT64 Oldest100ns;
	UINT64 Newest100ns;
	GUID SourceVolumeGuid;
	PDEVICE_OBJECT TargetDevice;
	KMUTEX Lock;
	PQH_INDEX_MEM_ENTRY IndexArray;
	ULONG IndexStart;
	ULONG IndexCount;
	ULONG IndexCapacity;
} QH_JOURNAL, *PQH_JOURNAL;

VOID QHJournalInitialize(
	_Out_ PQH_JOURNAL Journal,
	_In_ PDEVICE_OBJECT TargetDevice,
	_In_ UINT64 PartitionSize,
	_In_ ULONG SectorSize,
	_In_ const GUID* SourceVolumeGuid);

NTSTATUS QHJournalFormat(_Inout_ PQH_JOURNAL Journal);

NTSTATUS QHJournalMount(_Inout_ PQH_JOURNAL Journal);

NTSTATUS QHJournalAppend(
	_Inout_ PQH_JOURNAL Journal,
	_In_ UINT64 VolumeOffset,
	_In_ ULONG DataLength,
	_In_reads_bytes_(DataLength) const VOID* BeforeImage);

NTSTATUS QHJournalQueryTimeRange(
	_Inout_ PQH_JOURNAL Journal,
	_Out_ PUINT64 OldestTime100ns,
	_Out_ PUINT64 NewestTime100ns);

ULONG QHJournalFindFirstAfter(
	_In_ PQH_JOURNAL Journal,
	_In_ UINT64 TargetTime100ns);

// Fills Buffer from journal before-images for bytes changed after TargetTime.
// CoveredMask[i]=1 means Buffer[i] came from journal; remaining gaps must be
// filled from the live source volume by the caller.
NTSTATUS QHJournalBuildPreview(
	_Inout_ PQH_JOURNAL Journal,
	_In_ UINT64 TargetTime100ns,
	_In_ UINT64 VolumeOffset,
	_In_ ULONG DataLength,
	_Out_writes_bytes_(DataLength) PVOID Buffer,
	_Out_writes_bytes_(DataLength) PUCHAR CoveredMask,
	_Out_ PULONG CoveredCount);

VOID QHJournalClose(_Inout_ PQH_JOURNAL Journal);
