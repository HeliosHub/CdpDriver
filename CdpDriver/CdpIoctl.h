/*
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#ifdef _KERNEL_MODE
#include <ntddk.h>
#define Cdp_CONTROL_DEVICE_NAME L"\\Device\\CdpEngineControlDevice"
#define Cdp_CONTROL_SYSTEM_LINK_NAME L"\\DosDevices\\CdpEngineControlDevice"
#else
#include <Windows.h>
#define Cdp_CONTROL_SYSTEM_LINK_NAME L"\\\\.\\CdpEngineControlDevice"
#endif

#define Cdp_IOCTL_TYPE 0x8000

#define IOCTL_Cdp_QUERY_PROTECT_STATUS CTL_CODE(Cdp_IOCTL_TYPE, 0x801, METHOD_BUFFERED, FILE_ANY_ACCESS)

// 指令 1 / 2：METHOD_BUFFERED
#define IOCTL_Cdp_SEND_COMMAND CTL_CODE(Cdp_IOCTL_TYPE, 0x802, METHOD_BUFFERED, FILE_ANY_ACCESS)

// 文件预览：创建时间点会话、读取该时间点的卷数据、关闭会话
#define IOCTL_Cdp_BEGIN_PREVIEW CTL_CODE(Cdp_IOCTL_TYPE, 0x804, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_Cdp_READ_PREVIEW  CTL_CODE(Cdp_IOCTL_TYPE, 0x805, METHOD_OUT_DIRECT, FILE_ANY_ACCESS)
#define IOCTL_Cdp_END_PREVIEW   CTL_CODE(Cdp_IOCTL_TYPE, 0x806, METHOD_BUFFERED, FILE_ANY_ACCESS)

// 卷工作阶段：查询 / 准备恢复 / 提交回填 / 取消恢复
#define IOCTL_Cdp_QUERY_PHASE    CTL_CODE(Cdp_IOCTL_TYPE, 0x807, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_Cdp_BEGIN_RECOVERY CTL_CODE(Cdp_IOCTL_TYPE, 0x808, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_Cdp_COMMIT_RECOVERY CTL_CODE(Cdp_IOCTL_TYPE, 0x809, METHOD_BUFFERED, FILE_ANY_ACCESS)

// 查询 journal 内最早/最新历史记录的 WallClock100ns
#define IOCTL_Cdp_QUERY_TIME_RANGE CTL_CODE(Cdp_IOCTL_TYPE, 0x80A, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_Cdp_CANCEL_RECOVERY CTL_CODE(Cdp_IOCTL_TYPE, 0x80B, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_Cdp_QUERY_VERSION   CTL_CODE(Cdp_IOCTL_TYPE, 0x80C, METHOD_BUFFERED, FILE_ANY_ACCESS)
// Query the current journal payload-space accounting and record metadata.
#define IOCTL_Cdp_QUERY_JOURNAL_USAGE   CTL_CODE(Cdp_IOCTL_TYPE, 0x80D, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_Cdp_QUERY_JOURNAL_RECORDS CTL_CODE(Cdp_IOCTL_TYPE, 0x80E, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_Cdp_AUTHENTICATE          CTL_CODE(Cdp_IOCTL_TYPE, 0x80F, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_Cdp_QUERY_CREDENTIAL      CTL_CODE(Cdp_IOCTL_TYPE, 0x810, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_Cdp_CHANGE_PASSWORD       CTL_CODE(Cdp_IOCTL_TYPE, 0x811, METHOD_BUFFERED, FILE_ANY_ACCESS)
// Return the Journal runtime BranchTree; this is not derived by scanning
// record headers and contains only retained, currently valid branches.
#define IOCTL_Cdp_QUERY_JOURNAL_BRANCHES CTL_CODE(Cdp_IOCTL_TYPE, 0x812, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_Cdp_SET_RESTORE_POINT       CTL_CODE(Cdp_IOCTL_TYPE, 0x813, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_Cdp_DELETE_RESTORE_POINT    CTL_CODE(Cdp_IOCTL_TYPE, 0x814, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_Cdp_QUERY_RESTORE_POINT     CTL_CODE(Cdp_IOCTL_TYPE, 0x815, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_Cdp_MANUAL_MERGE             CTL_CODE(Cdp_IOCTL_TYPE, 0x816, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_Cdp_QUERY_RUNTIME_CHECKPOINTS CTL_CODE(Cdp_IOCTL_TYPE, 0x817, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_Cdp_QUERY_CHECKPOINT_RECORDS  CTL_CODE(Cdp_IOCTL_TYPE, 0x818, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_Cdp_CONFIRM_RESTORE_BOOT      CTL_CODE(Cdp_IOCTL_TYPE, 0x819, METHOD_BUFFERED, FILE_ANY_ACCESS)
// In-memory only. The preview UI calls this before creating its first VHD;
// reboot resets the gate and preserves normal persistent-Journal discovery.
#define IOCTL_Cdp_DISABLE_AUTO_DISCOVERY     CTL_CODE(Cdp_IOCTL_TYPE, 0x81A, METHOD_BUFFERED, FILE_ANY_ACCESS)

#define Cdp_PHASE_GENERAL  0UL
#define Cdp_PHASE_PREVIEW  1UL
#define Cdp_PHASE_RECOVERY 2UL
#define Cdp_PHASE_DRAINING 3UL
// A protected volume whose ordinary phase is General but whose history merge
// worker is materializing and reclaiming an RR.  This is a query-only state:
// callers must wait before starting Preview.
#define Cdp_PHASE_MERGING  4UL
#define Cdp_STATUS_UNPROTECTED (-1L)

#define Cdp_CMD_1 1
#define Cdp_CMD_2 2

#define Cdp_CMD3_MAX_READ_BYTES (2u * 1024u * 1024u)
#define Cdp_SECTOR_SIZE_DEFAULT 512u
#define Cdp_COMMAND_REPLY_MSG_CHARS 64
#define Cdp_VERSION_STRING_CHARS 32
#define Cdp_BUILD_STRING_CHARS 32
#define Cdp_JOURNAL_RECORD_QUERY_MAX_PER_CALL 512u
#define Cdp_JOURNAL_BRANCH_QUERY_MAX_PER_CALL 512u
#define Cdp_CHECKPOINT_QUERY_MAX_PER_CALL 256u
#define Cdp_CHECKPOINT_RECORD_QUERY_MAX_PER_CALL 512u
#define Cdp_RECORD_FLAG_BRANCH   0x80000000UL
#define Cdp_BRANCH_INFO_FLAG_CURRENT   0x00000001UL
#define Cdp_BRANCH_INFO_FLAG_SYNTHETIC 0x00000002UL
#define Cdp_PASSWORD_MAX_UTF8_BYTES 128u

#pragma pack(push, 8)

typedef struct _Cdp_CMD1_REQUEST
{
	ULONG Code;
	GUID PartitionGuid1;    // protected source volume
	GUID PartitionGuid2;    // dedicated journal partition
	ULONG FormatJournal;    // nonzero: initialize journal; zero: mount existing journal
} Cdp_CMD1_REQUEST, *PCdp_CMD1_REQUEST;

typedef struct _Cdp_CMD1_REQUEST_V2
{
	ULONG Code;
	GUID PartitionGuid1;
	GUID PartitionGuid2;
	ULONG FormatJournal;
	ULONG PasswordLength;
	UCHAR Password[Cdp_PASSWORD_MAX_UTF8_BYTES];
} Cdp_CMD1_REQUEST_V2, *PCdp_CMD1_REQUEST_V2;

typedef struct _Cdp_AUTH_REQUEST
{
	ULONG PasswordLength;
	UCHAR Password[Cdp_PASSWORD_MAX_UTF8_BYTES];
} Cdp_AUTH_REQUEST, *PCdp_AUTH_REQUEST;

typedef struct _Cdp_CREDENTIAL_STATUS_REPLY
{
	ULONG Configured;
	ULONG JournalCount;
	GUID CredentialId;
	UINT64 AuthEpoch;
} Cdp_CREDENTIAL_STATUS_REPLY, *PCdp_CREDENTIAL_STATUS_REPLY;

typedef struct _Cdp_CHANGE_PASSWORD_REQUEST
{
	ULONG PasswordLength;
	UCHAR Password[Cdp_PASSWORD_MAX_UTF8_BYTES];
} Cdp_CHANGE_PASSWORD_REQUEST, *PCdp_CHANGE_PASSWORD_REQUEST;

typedef struct _Cdp_CMD2_REQUEST
{
	ULONG Code;
	GUID SourceVolumeGuid; // stop CDP for this protected source only
} Cdp_CMD2_REQUEST, *PCdp_CMD2_REQUEST;

typedef struct _Cdp_COMMAND_REPLY
{
	ULONG Command;
	ULONG Result;
	UINT64 VolumeHandle;   // CMD1 成功时有效，其余为 0
	WCHAR Message[Cdp_COMMAND_REPLY_MSG_CHARS];
} Cdp_COMMAND_REPLY, *PCdp_COMMAND_REPLY;

// TargetTime100ns 使用本地时区 wall-clock（与 COW 记录 WallClock100ns 同口径）。
typedef struct _Cdp_PREVIEW_BEGIN_REQUEST
{
	GUID SourceVolumeGuid;
	UINT64 TargetTime100ns;
} Cdp_PREVIEW_BEGIN_REQUEST, *PCdp_PREVIEW_BEGIN_REQUEST;

typedef struct _Cdp_PREVIEW_BEGIN_REPLY
{
	/* State observed while processing the request.  A normal state denial is
	 * returned as a successful IOCTL with PreviewHandle == 0 so callers can
	 * explain the reason without a second query. */
	LONG Status;
	ULONG Reserved;
	UINT64 PreviewHandle;
	UINT64 TargetTime100ns;
	UINT64 OldestRecoverable100ns;
	UINT64 NewestRecoverable100ns;
} Cdp_PREVIEW_BEGIN_REPLY, *PCdp_PREVIEW_BEGIN_REPLY;

typedef struct _Cdp_PREVIEW_READ_REQUEST
{
	UINT64 PreviewHandle;
	UINT64 ByteOffset;
	ULONG ByteLength;
	ULONG Reserved;
} Cdp_PREVIEW_READ_REQUEST, *PCdp_PREVIEW_READ_REQUEST;

typedef struct _Cdp_PREVIEW_END_REQUEST
{
	UINT64 PreviewHandle;
} Cdp_PREVIEW_END_REQUEST, *PCdp_PREVIEW_END_REQUEST;

typedef struct _Cdp_PHASE_QUERY_REQUEST
{
	GUID SourceVolumeGuid;
} Cdp_PHASE_QUERY_REQUEST, *PCdp_PHASE_QUERY_REQUEST;

typedef struct _Cdp_PHASE_QUERY_REPLY
{
	// Cdp_PHASE_GENERAL / PREVIEW / RECOVERY / DRAINING / MERGING,
	// or Cdp_STATUS_UNPROTECTED (-1).
	LONG Status;
	ULONG Reserved;
	GUID JournalPartitionGuid; // valid when Status >= 0 and protection is on
	UINT64 RecoveryTargetTime100ns;
	/* Always populated for an active protection session, including pre-mount
	 * automatic discovery where a stable journal Volume GUID is unavailable. */
	ULONG JournalDiskNumber;
	ULONG JournalPartitionNumber;
	UINT64 JournalPartitionOffset;
	UINT64 JournalPartitionBytes;
} Cdp_PHASE_QUERY_REPLY, *PCdp_PHASE_QUERY_REPLY;

typedef struct _Cdp_RECOVERY_BEGIN_REQUEST
{
	GUID SourceVolumeGuid;
	UINT64 TargetTime100ns;
	ULONG Flags;
	ULONG Reserved;
} Cdp_RECOVERY_BEGIN_REQUEST, *PCdp_RECOVERY_BEGIN_REQUEST;

typedef struct _Cdp_RECOVERY_BEGIN_REPLY
{
	ULONG Phase; // Cdp_PHASE_RECOVERY after history view is prepared
	UINT64 TargetTime100ns;
	UINT64 OldestRecoverable100ns;
	UINT64 NewestRecoverable100ns;
} Cdp_RECOVERY_BEGIN_REPLY, *PCdp_RECOVERY_BEGIN_REPLY;

typedef struct _Cdp_RECOVERY_CONTROL_REQUEST
{
	GUID SourceVolumeGuid;
} Cdp_RECOVERY_CONTROL_REQUEST, *PCdp_RECOVERY_CONTROL_REQUEST;

typedef struct _Cdp_RECOVERY_COMMIT_REPLY
{
	ULONG Phase; // Cdp_PHASE_GENERAL after synchronous writeback completes
	UINT64 TargetTime100ns;
} Cdp_RECOVERY_COMMIT_REPLY, *PCdp_RECOVERY_COMMIT_REPLY;

typedef struct _Cdp_TIME_RANGE_QUERY_REQUEST
{
	GUID SourceVolumeGuid;
} Cdp_TIME_RANGE_QUERY_REQUEST, *PCdp_TIME_RANGE_QUERY_REQUEST;

typedef struct _Cdp_TIME_RANGE_QUERY_REPLY
{
	ULONG HasRecords; // 1 if journal has retained history; 0 if empty
	ULONG Reserved;
	UINT64 OldestRecord100ns; // earliest surviving WallClock100ns
	// Exclusive upper bound for second-precision clients: latest retained
	// WallClock100ns plus one second (saturated at MAXUINT64).
	UINT64 NewestRecord100ns;
} Cdp_TIME_RANGE_QUERY_REPLY, *PCdp_TIME_RANGE_QUERY_REPLY;

typedef struct _Cdp_JOURNAL_USAGE_QUERY_REQUEST
{
	GUID SourceVolumeGuid;
} Cdp_JOURNAL_USAGE_QUERY_REQUEST, *PCdp_JOURNAL_USAGE_QUERY_REQUEST;

typedef struct _Cdp_JOURNAL_USAGE_QUERY_REPLY
{
	UINT64 JournalPartitionBytes;       // total journal partition size
	UINT64 JournalMetadataBytes;        // superblock + active header regions
	UINT64 RecordPayloadBytesUsed;      // sector-aligned payload space in use
	UINT64 RecordPayloadBytesFree;      // free payload space with current headers
	UINT64 TotalRecords;                // surviving history record count
} Cdp_JOURNAL_USAGE_QUERY_REPLY, *PCdp_JOURNAL_USAGE_QUERY_REPLY;

// The returned records contain metadata only.  No payload bytes are returned.
typedef struct _Cdp_JOURNAL_RECORD_QUERY_REQUEST
{
	GUID SourceVolumeGuid;
	UINT64 StartIndex;          // zero-based, oldest record first
	UINT64 ExpectedGeneration;  // zero for first page; later pages must echo reply
	ULONG MaxRecords;           // capped by Cdp_JOURNAL_RECORD_QUERY_MAX_PER_CALL
	ULONG Reserved;
} Cdp_JOURNAL_RECORD_QUERY_REQUEST, *PCdp_JOURNAL_RECORD_QUERY_REQUEST;

typedef struct _Cdp_JOURNAL_RECORD_QUERY_REPLY
{
	UINT64 TotalRecords;
	UINT64 Generation;          // changes whenever retained records change
	ULONG RecordCount;
	ULONG Reserved;
} Cdp_JOURNAL_RECORD_QUERY_REPLY, *PCdp_JOURNAL_RECORD_QUERY_REPLY;

typedef struct _Cdp_JOURNAL_RECORD_INFO
{
	UINT64 WallClock100ns;
	UINT64 VolumeOffset;
	UINT64 FileOffset;
	UINT64 Sequence;
	ULONG DataLength;
	ULONG Flags; // Cdp_RECORD_FLAG_*; currently BRANCH=0x80000000
} Cdp_JOURNAL_RECORD_INFO, *PCdp_JOURNAL_RECORD_INFO;

C_ASSERT(sizeof(Cdp_JOURNAL_RECORD_INFO) == 40);

typedef struct _Cdp_RUNTIME_CHECKPOINT_QUERY_REQUEST
{
	GUID SourceVolumeGuid;
	UINT64 StartIndex;
	UINT64 ExpectedGeneration;
	ULONG MaxCheckpoints;
	ULONG Reserved;
} Cdp_RUNTIME_CHECKPOINT_QUERY_REQUEST,
	*PCdp_RUNTIME_CHECKPOINT_QUERY_REQUEST;

typedef struct _Cdp_RUNTIME_CHECKPOINT_QUERY_REPLY
{
	UINT64 TotalCheckpoints;
	UINT64 Generation;
	ULONG CheckpointCount;
	ULONG Reserved;
} Cdp_RUNTIME_CHECKPOINT_QUERY_REPLY,
	*PCdp_RUNTIME_CHECKPOINT_QUERY_REPLY;

typedef struct _Cdp_RUNTIME_CHECKPOINT_INFO
{
	UINT64 CheckpointId;
	UINT64 SourceRegionOffset;
	UINT64 SourceFirstSequence;
	UINT64 SourceEndSequence;
	UINT64 DataBytes;
	UINT64 AllocatedBytes;
	ULONG RecordCount;
	ULONG Reserved;
} Cdp_RUNTIME_CHECKPOINT_INFO, *PCdp_RUNTIME_CHECKPOINT_INFO;

C_ASSERT(sizeof(Cdp_RUNTIME_CHECKPOINT_INFO) == 56);

typedef struct _Cdp_CHECKPOINT_RECORD_QUERY_REQUEST
{
	GUID SourceVolumeGuid;
	UINT64 CheckpointId;
	UINT64 StartIndex;
	UINT64 ExpectedGeneration;
	ULONG MaxRecords;
	ULONG Reserved;
} Cdp_CHECKPOINT_RECORD_QUERY_REQUEST,
	*PCdp_CHECKPOINT_RECORD_QUERY_REQUEST;

typedef struct _Cdp_CHECKPOINT_RECORD_QUERY_REPLY
{
	UINT64 CheckpointId;
	UINT64 TotalRecords;
	UINT64 Generation;
	ULONG RecordCount;
	ULONG Reserved;
} Cdp_CHECKPOINT_RECORD_QUERY_REPLY,
	*PCdp_CHECKPOINT_RECORD_QUERY_REPLY;

typedef struct _Cdp_CHECKPOINT_RECORD_INFO
{
	UINT64 CheckpointId;
	UINT64 RecordIndex;
	UINT64 VolumeOffset;
	UINT64 FileOffset;
	ULONG DataLength;
	ULONG AllocatedLength;
} Cdp_CHECKPOINT_RECORD_INFO, *PCdp_CHECKPOINT_RECORD_INFO;

C_ASSERT(sizeof(Cdp_CHECKPOINT_RECORD_INFO) == 40);

typedef struct _Cdp_JOURNAL_BRANCH_QUERY_REQUEST
{
	GUID SourceVolumeGuid;
	UINT64 StartIndex;          // zero-based, BranchTree creation order
	UINT64 ExpectedGeneration;  // zero for first page; later pages echo reply
	ULONG MaxBranches;
	ULONG Reserved;
} Cdp_JOURNAL_BRANCH_QUERY_REQUEST, *PCdp_JOURNAL_BRANCH_QUERY_REQUEST;

typedef struct _Cdp_JOURNAL_BRANCH_QUERY_REPLY
{
	ULONG TotalBranches;
	LONG CurrentBranchNumber;
	UINT64 Generation;
	ULONG BranchCount;
	ULONG Reserved;
} Cdp_JOURNAL_BRANCH_QUERY_REPLY, *PCdp_JOURNAL_BRANCH_QUERY_REPLY;

typedef struct _Cdp_JOURNAL_BRANCH_INFO
{
	LONG BranchNumber;
	LONG ParentBranchNumber;
	UINT64 InheritedRecordSequence;
	UINT64 CreatedWallClock100ns;
	UINT64 StartSequence;
	UINT64 EndSequence;
	ULONG Flags; // Cdp_BRANCH_INFO_FLAG_*
	ULONG Reserved;
} Cdp_JOURNAL_BRANCH_INFO, *PCdp_JOURNAL_BRANCH_INFO;

C_ASSERT(sizeof(Cdp_JOURNAL_BRANCH_INFO) == 48);

typedef struct _Cdp_VERSION_REPLY
{
	ULONG JournalVersion;
	ULONG Reserved;
	CHAR Version[Cdp_VERSION_STRING_CHARS];
	CHAR Build[Cdp_BUILD_STRING_CHARS];
} Cdp_VERSION_REPLY, *PCdp_VERSION_REPLY;

typedef struct _Cdp_RESTORE_POINT_SET_REQUEST
{
	GUID SourceVolumeGuid;
	UINT64 TargetTime100ns;
} Cdp_RESTORE_POINT_SET_REQUEST, *PCdp_RESTORE_POINT_SET_REQUEST;

typedef struct _Cdp_RESTORE_POINT_SET_REPLY
{
	UINT64 TargetTime100ns;
	UINT64 OldestRecoverable100ns;
	UINT64 NewestRecoverable100ns;
	UINT64 WrittenBytes;
	ULONG WrittenRanges;
	ULONG Reserved;
} Cdp_RESTORE_POINT_SET_REPLY, *PCdp_RESTORE_POINT_SET_REPLY;

typedef struct _Cdp_RESTORE_POINT_DELETE_REQUEST
{
	GUID SourceVolumeGuid;
} Cdp_RESTORE_POINT_DELETE_REQUEST, *PCdp_RESTORE_POINT_DELETE_REQUEST;

// Compacts one normal oldest region without applying the automatic 90%
// Journal-usage threshold. Branch-invalidated tombstone regions caused by
// that same pass are still reclaimed. Completion is asynchronous and visible
// in logs.
typedef struct _Cdp_MANUAL_MERGE_REQUEST
{
	GUID SourceVolumeGuid;
} Cdp_MANUAL_MERGE_REQUEST, *PCdp_MANUAL_MERGE_REQUEST;

typedef struct _Cdp_RESTORE_POINT_QUERY_REQUEST
{
	GUID SourceVolumeGuid;
} Cdp_RESTORE_POINT_QUERY_REQUEST, *PCdp_RESTORE_POINT_QUERY_REQUEST;

typedef struct _Cdp_RESTORE_BOOT_CONFIRM_REQUEST
{
	GUID SourceVolumeGuid;
} Cdp_RESTORE_BOOT_CONFIRM_REQUEST, *PCdp_RESTORE_BOOT_CONFIRM_REQUEST;

typedef struct _Cdp_RESTORE_POINT_QUERY_REPLY
{
	ULONG IsSet;
	ULONG BootConfirmed;
	UINT64 TargetTime100ns;
} Cdp_RESTORE_POINT_QUERY_REPLY, *PCdp_RESTORE_POINT_QUERY_REPLY;

#pragma pack(pop)
// Persist this recovery request in the journal.  After a system restart the
// driver discovers the journal and automatically runs begin + commit.
#define Cdp_RECOVERY_BEGIN_FLAG_ON_REBOOT 0x00000001UL
