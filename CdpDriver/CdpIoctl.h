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

// Query Journal timestamps as UTC Unix seconds.
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
#define IOCTL_Cdp_QUERY_RESTORE_SPACE_ALERT  CTL_CODE(Cdp_IOCTL_TYPE, 0x81B, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_Cdp_WAIT_RESTORE_SPACE_ALERT   CTL_CODE(Cdp_IOCTL_TYPE, 0x81C, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_Cdp_QUERY_DRAIN_PROGRESS       CTL_CODE(Cdp_IOCTL_TYPE, 0x81D, METHOD_BUFFERED, FILE_ANY_ACCESS)

#ifdef CDP_LICENSE
/* License control codes use a separate range to preserve existing v17 ABI. */
#define IOCTL_Cdp_SET_LICENSE      CTL_CODE(Cdp_IOCTL_TYPE, 0x81E, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_Cdp_QUERY_LICENSE    CTL_CODE(Cdp_IOCTL_TYPE, 0x81F, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_Cdp_EXPORT_RECEIPT   CTL_CODE(Cdp_IOCTL_TYPE, 0x820, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_Cdp_BUILD_APPLY_QR   CTL_CODE(Cdp_IOCTL_TYPE, 0x821, METHOD_BUFFERED, FILE_ANY_ACCESS)
#endif
#define Cdp_PHASE_GENERAL  0UL
#define Cdp_PHASE_PREVIEW  1UL
#define Cdp_PHASE_RECOVERY 2UL
#define Cdp_PHASE_DRAINING 3UL
// A protected volume whose ordinary phase is General but whose history merge
// worker is materializing and reclaiming an RR.  This is a query-only state:
// callers must wait before starting Preview.
#define Cdp_PHASE_MERGING  4UL
#define Cdp_STATUS_UNPROTECTED (-1L)

#define Cdp_DRAIN_PROGRESS_IDLE      0UL
#define Cdp_DRAIN_PROGRESS_RUNNING   1UL
#define Cdp_DRAIN_PROGRESS_COMPLETED 2UL
#define Cdp_DRAIN_PROGRESS_FAILED    3UL

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

typedef struct _Cdp_DRAIN_PROGRESS_QUERY_REQUEST
{
	GUID SourceVolumeGuid;
} Cdp_DRAIN_PROGRESS_QUERY_REQUEST,
	*PCdp_DRAIN_PROGRESS_QUERY_REQUEST;

typedef struct _Cdp_DRAIN_PROGRESS_QUERY_REPLY
{
	ULONG State;       // Cdp_DRAIN_PROGRESS_*
	LONG Status;       // STATUS_PENDING / final NTSTATUS
	UINT64 TotalBytes; // de-duplicated MetaTree coverage at drain start
	UINT64 CompletedBytes;
} Cdp_DRAIN_PROGRESS_QUERY_REPLY,
	*PCdp_DRAIN_PROGRESS_QUERY_REPLY;

C_ASSERT(sizeof(Cdp_DRAIN_PROGRESS_QUERY_REPLY) == 24);

typedef struct _Cdp_COMMAND_REPLY
{
	ULONG Command;
	ULONG Result;
	UINT64 VolumeHandle;   // CMD1 成功时有效，其余为 0
	WCHAR Message[Cdp_COMMAND_REPLY_MSG_CHARS];
} Cdp_COMMAND_REPLY, *PCdp_COMMAND_REPLY;

// TargetTime100ns fields retain their ABI name but carry UTC Unix seconds
// (same unit as persisted Journal timestamps in v17).
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
	UINT64 OldestRecord100ns; // earliest surviving UTC Unix second
	// Latest retained UTC Unix second.  Same-second records are distinguished
	// by Sequence in Cdp_JOURNAL_RECORD_INFO.
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

#define Cdp_RESTORE_SPACE_ALERT_NONE              0UL
#define Cdp_RESTORE_SPACE_ALERT_NO_COMPACTABLE_RR 1UL
#define Cdp_RESTORE_SPACE_ALERT_RESERVE_FAILED    2UL
#define Cdp_RESTORE_SPACE_ALERT_MERGE_FAILED      3UL

typedef struct _Cdp_RESTORE_SPACE_ALERT_QUERY_REQUEST
{
	GUID SourceVolumeGuid;
} Cdp_RESTORE_SPACE_ALERT_QUERY_REQUEST,
	*PCdp_RESTORE_SPACE_ALERT_QUERY_REQUEST;

typedef struct _Cdp_RESTORE_SPACE_ALERT_QUERY_REPLY
{
	ULONG RestorePointSet;
	ULONG AlertActive;
	ULONG AlertReason;
	LONG MergeStatus;
	ULONG MergeRunning;
	ULONG Reserved;
	UINT64 RecordPayloadBytesUsed;
	UINT64 RecordPayloadBytesFree;
} Cdp_RESTORE_SPACE_ALERT_QUERY_REPLY,
	*PCdp_RESTORE_SPACE_ALERT_QUERY_REPLY;

/* Inverted-call notification. LastSeenGeneration makes registration and
 * notification race-free: a stale caller completes immediately, otherwise
 * the driver holds the IRP until this source's alert state changes. */
typedef struct _Cdp_RESTORE_SPACE_ALERT_WAIT_REQUEST
{
	GUID SourceVolumeGuid;
	UINT64 LastSeenGeneration;
} Cdp_RESTORE_SPACE_ALERT_WAIT_REQUEST,
	*PCdp_RESTORE_SPACE_ALERT_WAIT_REQUEST;

typedef struct _Cdp_RESTORE_SPACE_ALERT_NOTIFICATION
{
	GUID SourceVolumeGuid;
	UINT64 Generation;
	ULONG RestorePointSet;
	ULONG AlertActive;
	ULONG AlertReason;
	LONG MergeStatus;
	UINT64 RecordPayloadBytesUsed;
	UINT64 RecordPayloadBytesFree;
} Cdp_RESTORE_SPACE_ALERT_NOTIFICATION,
	*PCdp_RESTORE_SPACE_ALERT_NOTIFICATION;

/* Duplicated, reversed merge fragment; the canonical license ABI is below.
`r`n#ifdef CDP_LICENSE

#endif
#define ERROR_CDP_LICENSE_TRIAL_USED  0x0000CD06L
#ifndef ERROR_CDP_LICENSE_TRIAL_USED
#endif
#define ERROR_CDP_LICENSE_TRIAL_USED  0x0000CD06L /* 试用证已用过 * /
#define ERROR_CDP_LICENSE_INVALID     0x0000CD05L /* 验签或绑机失败 * /
#define ERROR_CDP_LICENSE_TAMPER      0x0000CD04L /* 本地状态被篡改 * /
#define ERROR_CDP_LICENSE_EXHAUSTED   0x0000CD03L /* 次数用尽 * /
#define ERROR_CDP_LICENSE_EXPIRED     0x0000CD02L /* 已过期 * /
#define ERROR_CDP_LICENSE_REQUIRED    0x0000CD01L /* 未导入/未加载 * /
#ifndef ERROR_CDP_LICENSE_REQUIRED
 * /
 * 旧驱动若仍返回 0xC0CDxxxx，GetLastError() 会是 317（ERROR_MR_MID_NOT_FOUND）。
 * 用户态 DeviceIoControl 失败后 GetLastError() 即为此值。
 * 授权失败的 Win32 错误码。驱动将对应 NTSTATUS 包成 FACILITY_NTWIN32，
/*

} Cdp_VERSION_REPLY, *PCdp_VERSION_REPLY;
	CHAR Build[Cdp_BUILD_STRING_CHARS];
	CHAR Version[Cdp_VERSION_STRING_CHARS];
	ULONG Reserved;
	ULONG JournalVersion;
{
typedef struct _Cdp_VERSION_REPLY

C_ASSERT(sizeof(Cdp_JOURNAL_RECORD_INFO) == 40);

} Cdp_JOURNAL_RECORD_INFO, *PCdp_JOURNAL_RECORD_INFO;
	ULONG Flags; // Cdp_RECORD_FLAG_*; currently BACKFILL=0x80000000
	ULONG DataLength;
	UINT64 Sequence;
	UINT64 FileOffset;
	UINT64 VolumeOffset;
	UINT64 WallClock100ns;
{
typedef struct _Cdp_JOURNAL_RECORD_INFO

} Cdp_JOURNAL_RECORD_QUERY_REPLY, *PCdp_JOURNAL_RECORD_QUERY_REPLY;
	ULONG Reserved;
	ULONG RecordCount;
	UINT64 Generation;          // changes whenever retained records change
	UINT64 TotalRecords;
{
typedef struct _Cdp_JOURNAL_RECORD_QUERY_REPLY

} Cdp_JOURNAL_RECORD_QUERY_REQUEST, *PCdp_JOURNAL_RECORD_QUERY_REQUEST;
	ULONG Reserved;
	ULONG MaxRecords;           // capped by Cdp_JOURNAL_RECORD_QUERY_MAX_PER_CALL
	UINT64 ExpectedGeneration;  // zero for first page; later pages must echo reply
	UINT64 StartIndex;          // zero-based, oldest record first
	GUID SourceVolumeGuid;
{
typedef struct _Cdp_JOURNAL_RECORD_QUERY_REQUEST
// The returned records contain metadata only.  No payload bytes are returned.

} Cdp_JOURNAL_USAGE_QUERY_REPLY, *PCdp_JOURNAL_USAGE_QUERY_REPLY;
	UINT64 TotalRecords;                // surviving COW record count
	UINT64 RecordPayloadBytesFree;      // free payload space with current headers
	UINT64 RecordPayloadBytesUsed;      // sector-aligned payload space in use
	UINT64 JournalMetadataBytes;        // superblock + active header regions
	UINT64 JournalPartitionBytes;       // total journal partition size
{
typedef struct _Cdp_JOURNAL_USAGE_QUERY_REPLY

} Cdp_JOURNAL_USAGE_QUERY_REQUEST, *PCdp_JOURNAL_USAGE_QUERY_REQUEST;
	GUID SourceVolumeGuid;
{
typedef struct _Cdp_JOURNAL_USAGE_QUERY_REQUEST

} Cdp_TIME_RANGE_QUERY_REPLY, *PCdp_TIME_RANGE_QUERY_REPLY;
	UINT64 NewestRecord100ns; // latest WallClock100ns
	UINT64 OldestRecord100ns; // earliest surviving WallClock100ns
	ULONG Reserved;
	ULONG HasRecords; // 1 if journal has COW history; 0 if empty
{
typedef struct _Cdp_TIME_RANGE_QUERY_REPLY

} Cdp_TIME_RANGE_QUERY_REQUEST, *PCdp_TIME_RANGE_QUERY_REQUEST;
	GUID SourceVolumeGuid;
{
typedef struct _Cdp_TIME_RANGE_QUERY_REQUEST

} Cdp_RECOVERY_COMMIT_REPLY, *PCdp_RECOVERY_COMMIT_REPLY;
	UINT64 TargetTime100ns;
	ULONG Phase; // Cdp_PHASE_GENERAL after synchronous writeback completes
{
typedef struct _Cdp_RECOVERY_COMMIT_REPLY

} Cdp_RECOVERY_CONTROL_REQUEST, *PCdp_RECOVERY_CONTROL_REQUEST;
	GUID SourceVolumeGuid;
{
typedef struct _Cdp_RECOVERY_CONTROL_REQUEST

} Cdp_RECOVERY_BEGIN_REPLY, *PCdp_RECOVERY_BEGIN_REPLY;
	UINT64 NewestRecoverable100ns;
	UINT64 OldestRecoverable100ns;
	UINT64 TargetTime100ns;
	ULONG Phase; // Cdp_PHASE_RECOVERY after history view is prepared
{
typedef struct _Cdp_RECOVERY_BEGIN_REPLY

} Cdp_RECOVERY_BEGIN_REQUEST, *PCdp_RECOVERY_BEGIN_REQUEST;
	ULONG Reserved;
	ULONG Flags;
	UINT64 TargetTime100ns;
	GUID SourceVolumeGuid;
{
typedef struct _Cdp_RECOVERY_BEGIN_REQUEST

} Cdp_PHASE_QUERY_REPLY, *PCdp_PHASE_QUERY_REPLY;
	UINT64 RecoveryTargetTime100ns;
	GUID JournalPartitionGuid; // valid when Status >= 0 and protection is on
	ULONG Reserved;
	LONG Status;
	// Cdp_PHASE_GENERAL / PREVIEW / RECOVERY, or Cdp_STATUS_UNPROTECTED (-1).
{
typedef struct _Cdp_PHASE_QUERY_REPLY

} Cdp_PHASE_QUERY_REQUEST, *PCdp_PHASE_QUERY_REQUEST;
	GUID SourceVolumeGuid;
{
typedef struct _Cdp_PHASE_QUERY_REQUEST

} Cdp_PREVIEW_END_REQUEST, *PCdp_PREVIEW_END_REQUEST;
	UINT64 PreviewHandle;
{
typedef struct _Cdp_PREVIEW_END_REQUEST

} Cdp_PREVIEW_READ_REQUEST, *PCdp_PREVIEW_READ_REQUEST;
	ULONG Reserved;
	ULONG ByteLength;
	UINT64 ByteOffset;
	UINT64 PreviewHandle;
{
typedef struct _Cdp_PREVIEW_READ_REQUEST

} Cdp_PREVIEW_BEGIN_REPLY, *PCdp_PREVIEW_BEGIN_REPLY;
	UINT64 NewestRecoverable100ns;
	UINT64 OldestRecoverable100ns;
	UINT64 TargetTime100ns;
	UINT64 PreviewHandle;
{
typedef struct _Cdp_PREVIEW_BEGIN_REPLY

} Cdp_PREVIEW_BEGIN_REQUEST, *PCdp_PREVIEW_BEGIN_REQUEST;
	UINT64 TargetTime100ns;
	GUID SourceVolumeGuid;
{
typedef struct _Cdp_PREVIEW_BEGIN_REQUEST
// TargetTime100ns 使用本地时区 wall-clock（与 COW 记录 WallClock100ns 同口径）。

} Cdp_COMMAND_REPLY, *PCdp_COMMAND_REPLY;
	WCHAR Message[Cdp_COMMAND_REPLY_MSG_CHARS];
	UINT64 VolumeHandle;   // CMD1 成功时有效，其余为 0
	ULONG Result;
	ULONG Command;
{
typedef struct _Cdp_COMMAND_REPLY

} Cdp_CMD2_REQUEST, *PCdp_CMD2_REQUEST;
	GUID SourceVolumeGuid; // stop CDP for this protected source only
	ULONG Code;
{
typedef struct _Cdp_CMD2_REQUEST

} Cdp_CHANGE_PASSWORD_REQUEST, *PCdp_CHANGE_PASSWORD_REQUEST;
	UCHAR Password[Cdp_PASSWORD_MAX_UTF8_BYTES];
	ULONG PasswordLength;
{
typedef struct _Cdp_CHANGE_PASSWORD_REQUEST

} Cdp_CREDENTIAL_STATUS_REPLY, *PCdp_CREDENTIAL_STATUS_REPLY;
	UINT64 AuthEpoch;
	GUID CredentialId;
	ULONG JournalCount;
	ULONG Configured;
{
typedef struct _Cdp_CREDENTIAL_STATUS_REPLY

} Cdp_AUTH_REQUEST, *PCdp_AUTH_REQUEST;
	UCHAR Password[Cdp_PASSWORD_MAX_UTF8_BYTES];
	ULONG PasswordLength;
{
typedef struct _Cdp_AUTH_REQUEST

} Cdp_CMD1_REQUEST_V2, *PCdp_CMD1_REQUEST_V2;
	UCHAR Password[Cdp_PASSWORD_MAX_UTF8_BYTES];
	ULONG PasswordLength;
	ULONG FormatJournal;
	GUID PartitionGuid2;
	GUID PartitionGuid1;
	ULONG Code;
{
typedef struct _Cdp_CMD1_REQUEST_V2

} Cdp_CMD1_REQUEST, *PCdp_CMD1_REQUEST;
	ULONG FormatJournal;    // nonzero: initialize journal; zero: mount existing journal
	GUID PartitionGuid2;    // dedicated journal partition
	GUID PartitionGuid1;    // protected source volume
	ULONG Code;
{
typedef struct _Cdp_CMD1_REQUEST

#pragma pack(push, 8)

#define Cdp_PASSWORD_MAX_UTF8_BYTES 128u
#define Cdp_RECORD_FLAG_BACKFILL 0x80000000UL
#define Cdp_JOURNAL_RECORD_QUERY_MAX_PER_CALL 512u
#define Cdp_BUILD_STRING_CHARS 32
#define Cdp_VERSION_STRING_CHARS 32
#define Cdp_COMMAND_REPLY_MSG_CHARS 64
#define Cdp_SECTOR_SIZE_DEFAULT 512u
#define Cdp_CMD3_MAX_READ_BYTES (2u * 1024u * 1024u)

#define Cdp_CMD_2 2
#define Cdp_CMD_1 1

#define Cdp_STATUS_UNPROTECTED (-1L)
#define Cdp_PHASE_RECOVERY 2UL
#define Cdp_PHASE_PREVIEW  1UL
#define Cdp_PHASE_GENERAL  0UL

#endif
#define IOCTL_Cdp_BUILD_APPLY_QR        CTL_CODE(Cdp_IOCTL_TYPE, 0x815, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_Cdp_EXPORT_RECEIPT        CTL_CODE(Cdp_IOCTL_TYPE, 0x814, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_Cdp_QUERY_LICENSE         CTL_CODE(Cdp_IOCTL_TYPE, 0x813, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_Cdp_SET_LICENSE           CTL_CODE(Cdp_IOCTL_TYPE, 0x812, METHOD_BUFFERED, FILE_ANY_ACCESS)
/* 授权 IOCTL（设计 §11）；仅 Debug-Lic / Release 可见 * /
#ifdef CDP_LICENSE
#define IOCTL_Cdp_CHANGE_PASSWORD       CTL_CODE(Cdp_IOCTL_TYPE, 0x811, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_Cdp_QUERY_CREDENTIAL      CTL_CODE(Cdp_IOCTL_TYPE, 0x810, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_Cdp_AUTHENTICATE          CTL_CODE(Cdp_IOCTL_TYPE, 0x80F, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_Cdp_QUERY_JOURNAL_RECORDS CTL_CODE(Cdp_IOCTL_TYPE, 0x80E, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_Cdp_QUERY_JOURNAL_USAGE   CTL_CODE(Cdp_IOCTL_TYPE, 0x80D, METHOD_BUFFERED, FILE_ANY_ACCESS)
// Query the current journal payload-space accounting and record metadata.
#define IOCTL_Cdp_QUERY_VERSION   CTL_CODE(Cdp_IOCTL_TYPE, 0x80C, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_Cdp_CANCEL_RECOVERY CTL_CODE(Cdp_IOCTL_TYPE, 0x80B, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_Cdp_QUERY_TIME_RANGE CTL_CODE(Cdp_IOCTL_TYPE, 0x80A, METHOD_BUFFERED, FILE_ANY_ACCESS)
// 查询 journal 内最早/最新 COW 记录的 WallClock100ns

#define IOCTL_Cdp_COMMIT_RECOVERY CTL_CODE(Cdp_IOCTL_TYPE, 0x809, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_Cdp_BEGIN_RECOVERY CTL_CODE(Cdp_IOCTL_TYPE, 0x808, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_Cdp_QUERY_PHASE    CTL_CODE(Cdp_IOCTL_TYPE, 0x807, METHOD_BUFFERED, FILE_ANY_ACCESS)
// 卷工作阶段：查询 / 准备恢复 / 提交回填 / 取消恢复

#define IOCTL_Cdp_END_PREVIEW   CTL_CODE(Cdp_IOCTL_TYPE, 0x806, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_Cdp_READ_PREVIEW  CTL_CODE(Cdp_IOCTL_TYPE, 0x805, METHOD_OUT_DIRECT, FILE_ANY_ACCESS)
#define IOCTL_Cdp_BEGIN_PREVIEW CTL_CODE(Cdp_IOCTL_TYPE, 0x804, METHOD_BUFFERED, FILE_ANY_ACCESS)
// 文件预览：创建时间点会话、读取该时间点的卷数据、关闭会话

#define IOCTL_Cdp_SEND_COMMAND CTL_CODE(Cdp_IOCTL_TYPE, 0x802, METHOD_BUFFERED, FILE_ANY_ACCESS)
// 指令 1 / 2：METHOD_BUFFERED

#define IOCTL_Cdp_QUERY_PROTECT_STATUS CTL_CODE(Cdp_IOCTL_TYPE, 0x801, METHOD_BUFFERED, FILE_ANY_ACCESS)

#define Cdp_IOCTL_TYPE 0x8000

#endif
#define Cdp_CONTROL_SYSTEM_LINK_NAME L"\\\\.\\CdpEngineControlDevice"
#include <Windows.h>
#else
#define Cdp_CONTROL_SYSTEM_LINK_NAME L"\\DosDevices\\CdpEngineControlDevice"
#define Cdp_CONTROL_DEVICE_NAME L"\\Device\\CdpEngineControlDevice"
#include <ntddk.h>
#ifdef _KERNEL_MODE

#pragma once

 * /
 * limitations under the License.
 * See the License for the specific language governing permissions and
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * distributed under the License is distributed on an "AS IS" BASIS,
 * Unless required by applicable law or agreed to in writing, software
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * You may obtain a copy of the License at
 * you may not use this file except in compliance with the License.
 * Licensed under the Apache License, Version 2.0 (the "License");
 *
*/
#define Cdp_RECOVERY_BEGIN_FLAG_ON_REBOOT 0x00000001UL
#ifdef CDP_LICENSE
/*
 * 授权相关 IOCTL 缓冲区（与驱动 CdpLicenseGate 一一对应）。
 * 用户态需在工程中同样定义 CDP_LICENSE 才能看到这些类型。
 */
#ifndef Cdp_LICENSE_BLOB_MAX
#define Cdp_LICENSE_BLOB_MAX 2048u
#endif
#ifndef Cdp_APPLY_QR_PAYLOAD_MAX
#define Cdp_APPLY_QR_PAYLOAD_MAX 4096u
#endif
#ifndef Cdp_APPLY_QR_PREFIX_MAX
#define Cdp_APPLY_QR_PREFIX_MAX 256u
#endif
#ifndef Cdp_APPLY_CIPHERTEXT_MAX
#define Cdp_APPLY_CIPHERTEXT_MAX 3072u
#endif
#ifdef LIC_DEBUG
#ifndef Cdp_APPLY_PLAINTEXT_MAX
#define Cdp_APPLY_PLAINTEXT_MAX 1536u
#endif
#endif
#ifndef Cdp_LICENSE_FP_BYTES
#define Cdp_LICENSE_FP_BYTES 32u
#endif
#ifndef Cdp_LICENSE_MB_UUID_CHARS
#define Cdp_LICENSE_MB_UUID_CHARS 64u
#endif
#ifndef Cdp_LICENSE_DISK_SERIAL_CHARS
#define Cdp_LICENSE_DISK_SERIAL_CHARS 128u
#endif
#ifndef Cdp_LICENSE_ID_CHARS
#define Cdp_LICENSE_ID_CHARS 64u
#endif

typedef struct _Cdp_SET_LICENSE_REQUEST
{
	ULONG LicenseLength;
	ULONG Reserved;
	UCHAR LicenseBlob[Cdp_LICENSE_BLOB_MAX]; /* 服务端下载的 canonical 字节 */
} Cdp_SET_LICENSE_REQUEST, *PCdp_SET_LICENSE_REQUEST;

typedef struct _Cdp_LICENSE_QUERY_REPLY
{
	ULONG HasLicense;
	ULONG Mode;
	UINT64 T0_100ns;
	UINT64 T_EXP_100ns;
	ULONG C0;
	ULONG OPS_T;
	ULONG OPS_S;
	ULONG A_MOD;
	ULONG IsTrial;   /* 非零：当前证为试用 */
	ULONG Reserved;  /* 保持 pack(8) 下 sizeof=48，兼容旧 40 字节应答 */
} Cdp_LICENSE_QUERY_REPLY, *PCdp_LICENSE_QUERY_REPLY;

#define Cdp_LICENSE_QUERY_REPLY_V1_BYTES 40u
#ifdef _KERNEL_MODE
C_ASSERT(sizeof(Cdp_LICENSE_QUERY_REPLY) == 48);
#else
static_assert(sizeof(Cdp_LICENSE_QUERY_REPLY) == 48, "QUERY_REPLY ABI");
#endif

typedef struct _Cdp_LICENSE_RECEIPT_REPLY
{
	ULONG OPS_T;
	ULONG OPS_S;
	ULONG A_MOD;
	ULONG Reserved;
	UINT64 T_CLIENT_100ns;
	UCHAR DeviceFingerprint[Cdp_LICENSE_FP_BYTES];
	CHAR MbUuid[Cdp_LICENSE_MB_UUID_CHARS];
	CHAR DiskSerial[Cdp_LICENSE_DISK_SERIAL_CHARS];
	CHAR PrevLicenseId[Cdp_LICENSE_ID_CHARS];
} Cdp_LICENSE_RECEIPT_REPLY, *PCdp_LICENSE_RECEIPT_REPLY;

typedef struct _Cdp_BUILD_APPLY_QR_REQUEST
{
	ULONG DesiredDurationSec; /* 正式按时长/混合必须 >0；试用可为 0 */
	ULONG DesiredCredits;     /* 正式按次数/混合必须 >0；试用可为 0 */
	ULONG Mode; /* 1=time 2=counter 3=hybrid */
	ULONG Kind; /* 0=paid 1=trial；旧客户端此字段为 Reserved=0 */
	CHAR QrPrefix[Cdp_APPLY_QR_PREFIX_MAX]; /* 扫码 URI 前缀，由应用层传入，须自带 #c= / ?c= */
} Cdp_BUILD_APPLY_QR_REQUEST, *PCdp_BUILD_APPLY_QR_REQUEST;

#ifdef _KERNEL_MODE
C_ASSERT(sizeof(Cdp_BUILD_APPLY_QR_REQUEST) == 272);
#endif

typedef struct _Cdp_LICENSE_APPLY_QR_REPLY
{
	ULONG CiphertextLength;
	ULONG Reserved;
	UCHAR DeviceFingerprint[Cdp_LICENSE_FP_BYTES];
	UCHAR Ciphertext[Cdp_APPLY_CIPHERTEXT_MAX]; /* 原始混合密文，便于联网直传 */
	CHAR QrPayload[Cdp_APPLY_QR_PAYLOAD_MAX];   /* 扫码 URI；GUI 据此画二维码 */
} Cdp_LICENSE_APPLY_QR_REPLY, *PCdp_LICENSE_APPLY_QR_REPLY;

#ifdef LIC_DEBUG
/*
 * 调试扩展：接在稳定 ABI 之后。调用方（如 Debug-Lic 的 CdpConsole）
 * 若输出缓冲 >= 本结构，驱动才回填申请 JSON 原文；GUI 用不含本字段的
 * 稳定大小即可，避免 Debug-Lic 驱动与发布版 GUI 因 sizeof 不一致而
 * 在 BUILD_APPLY_QR 上返回 STATUS_BUFFER_TOO_SMALL。
 */
typedef struct _Cdp_LICENSE_APPLY_QR_REPLY_EX
{
	Cdp_LICENSE_APPLY_QR_REPLY Reply;
	CHAR Plaintext[Cdp_APPLY_PLAINTEXT_MAX];
} Cdp_LICENSE_APPLY_QR_REPLY_EX, *PCdp_LICENSE_APPLY_QR_REPLY_EX;
#endif
#endif
#pragma pack(pop)
// Persist this recovery request in the journal.  After a system restart the
// driver discovers the journal and automatically runs begin + commit.
#define Cdp_RECOVERY_BEGIN_FLAG_ON_REBOOT 0x00000001UL
