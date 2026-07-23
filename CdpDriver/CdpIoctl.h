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

// 查询 journal 内最早/最新 COW 记录的 WallClock100ns
#define IOCTL_Cdp_QUERY_TIME_RANGE CTL_CODE(Cdp_IOCTL_TYPE, 0x80A, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_Cdp_CANCEL_RECOVERY CTL_CODE(Cdp_IOCTL_TYPE, 0x80B, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_Cdp_QUERY_VERSION   CTL_CODE(Cdp_IOCTL_TYPE, 0x80C, METHOD_BUFFERED, FILE_ANY_ACCESS)

#define Cdp_PHASE_GENERAL  0UL
#define Cdp_PHASE_PREVIEW  1UL
#define Cdp_PHASE_RECOVERY 2UL
#define Cdp_STATUS_UNPROTECTED (-1L)

#define Cdp_CMD_1 1
#define Cdp_CMD_2 2

#define Cdp_CMD3_MAX_READ_BYTES (2u * 1024u * 1024u)
#define Cdp_SECTOR_SIZE_DEFAULT 512u
#define Cdp_COMMAND_REPLY_MSG_CHARS 64
#define Cdp_VERSION_STRING_CHARS 32
#define Cdp_BUILD_STRING_CHARS 32

#pragma pack(push, 8)

typedef struct _Cdp_CMD1_REQUEST
{
	ULONG Code;
	GUID PartitionGuid1;    // protected source volume
	GUID PartitionGuid2;    // dedicated journal partition
	ULONG FormatJournal;    // nonzero: initialize journal; zero: mount existing journal
} Cdp_CMD1_REQUEST, *PCdp_CMD1_REQUEST;

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
	// Cdp_PHASE_GENERAL / PREVIEW / RECOVERY, or Cdp_STATUS_UNPROTECTED (-1).
	LONG Status;
	ULONG Reserved;
	GUID JournalPartitionGuid; // valid when Status >= 0 and protection is on
	UINT64 RecoveryTargetTime100ns;
} Cdp_PHASE_QUERY_REPLY, *PCdp_PHASE_QUERY_REPLY;

typedef struct _Cdp_RECOVERY_BEGIN_REQUEST
{
	GUID SourceVolumeGuid;
	UINT64 TargetTime100ns;
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
	ULONG HasRecords; // 1 if journal has COW history; 0 if empty
	ULONG Reserved;
	UINT64 OldestRecord100ns; // earliest surviving WallClock100ns
	UINT64 NewestRecord100ns; // latest WallClock100ns
} Cdp_TIME_RANGE_QUERY_REPLY, *PCdp_TIME_RANGE_QUERY_REPLY;

typedef struct _Cdp_VERSION_REPLY
{
	ULONG JournalVersion;
	ULONG Reserved;
	CHAR Version[Cdp_VERSION_STRING_CHARS];
	CHAR Build[Cdp_BUILD_STRING_CHARS];
} Cdp_VERSION_REPLY, *PCdp_VERSION_REPLY;

#pragma pack(pop)
