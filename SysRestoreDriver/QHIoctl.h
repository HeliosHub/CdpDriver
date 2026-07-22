/*
 * Copyright 2026 Xuhui Jiang
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
#define QH_CONTROL_DEVICE_NAME L"\\Device\\QHEngineControlDevice"
#define QH_CONTROL_SYSTEM_LINK_NAME L"\\DosDevices\\QHEngineControlDevice"
#else
#include <Windows.h>
#define QH_CONTROL_SYSTEM_LINK_NAME L"\\\\.\\QHEngineControlDevice"
#endif

#define QH_IOCTL_TYPE 0x8000

#define IOCTL_QH_QUERY_PROTECT_STATUS CTL_CODE(QH_IOCTL_TYPE, 0x801, METHOD_BUFFERED, FILE_ANY_ACCESS)

// 指令 1 / 2 / 4 / 5：METHOD_BUFFERED
#define IOCTL_QH_SEND_COMMAND CTL_CODE(QH_IOCTL_TYPE, 0x802, METHOD_BUFFERED, FILE_ANY_ACCESS)

// 指令 3：按驱动侧句柄读扇区（METHOD_OUT_DIRECT）
#define IOCTL_QH_READ_SECTORS CTL_CODE(QH_IOCTL_TYPE, 0x803, METHOD_OUT_DIRECT, FILE_ANY_ACCESS)

// 文件预览：创建时间点会话、读取该时间点的卷数据、关闭会话
#define IOCTL_QH_BEGIN_PREVIEW CTL_CODE(QH_IOCTL_TYPE, 0x804, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_QH_READ_PREVIEW  CTL_CODE(QH_IOCTL_TYPE, 0x805, METHOD_OUT_DIRECT, FILE_ANY_ACCESS)
#define IOCTL_QH_END_PREVIEW   CTL_CODE(QH_IOCTL_TYPE, 0x806, METHOD_BUFFERED, FILE_ANY_ACCESS)

// 卷工作阶段：查询 / 准备恢复 / 提交回填 / 取消恢复
#define IOCTL_QH_QUERY_PHASE    CTL_CODE(QH_IOCTL_TYPE, 0x807, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_QH_BEGIN_RECOVERY CTL_CODE(QH_IOCTL_TYPE, 0x808, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_QH_COMMIT_RECOVERY CTL_CODE(QH_IOCTL_TYPE, 0x809, METHOD_BUFFERED, FILE_ANY_ACCESS)

// 查询 journal 内最早/最新 COW 记录的 WallClock100ns
#define IOCTL_QH_QUERY_TIME_RANGE CTL_CODE(QH_IOCTL_TYPE, 0x80A, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_QH_CANCEL_RECOVERY CTL_CODE(QH_IOCTL_TYPE, 0x80B, METHOD_BUFFERED, FILE_ANY_ACCESS)

#define QH_PHASE_GENERAL  0UL
#define QH_PHASE_PREVIEW  1UL
#define QH_PHASE_RECOVERY 2UL
#define QH_STATUS_UNPROTECTED (-1L)

#define QH_CMD_1 1
#define QH_CMD_2 2
#define QH_CMD_3 3
#define QH_CMD_4 4  // 按 GUID 打开卷，返回 VolumeHandle
#define QH_CMD_5 5  // 关闭 VolumeHandle

#define QH_CMD3_MAX_READ_BYTES (2u * 1024u * 1024u)
#define QH_SECTOR_SIZE_DEFAULT 512u
#define QH_COMMAND_REPLY_MSG_CHARS 64

#pragma pack(push, 8)

typedef struct _QH_CMD1_REQUEST
{
	ULONG Code;
	GUID PartitionGuid1;    // protected source volume
	GUID PartitionGuid2;    // dedicated journal partition
	ULONG FormatJournal;    // nonzero: initialize journal; zero: mount existing journal
} QH_CMD1_REQUEST, *PQH_CMD1_REQUEST;

typedef struct _QH_CMD2_REQUEST
{
	ULONG Code;
} QH_CMD2_REQUEST, *PQH_CMD2_REQUEST;

// 指令4：GUID → 驱动内打开并缓存，返回句柄 ID
typedef struct _QH_CMD4_REQUEST
{
	ULONG Code;          // = QH_CMD_4
	GUID PartitionGuid;
} QH_CMD4_REQUEST, *PQH_CMD4_REQUEST;

// 指令5：关闭句柄
typedef struct _QH_CMD5_REQUEST
{
	ULONG Code;            // = QH_CMD_5
	UINT64 VolumeHandle;
} QH_CMD5_REQUEST, *PQH_CMD5_REQUEST;

// 指令3：用句柄读扇区（不再带 GUID）
typedef struct _QH_CMD3_REQUEST
{
	ULONG Code;            // = QH_CMD_3
	UINT64 VolumeHandle;   // 指令4 返回的句柄
	UINT64 ByteOffset;
	ULONG ByteLength;
} QH_CMD3_REQUEST, *PQH_CMD3_REQUEST;

typedef struct _QH_COMMAND_REPLY
{
	ULONG Command;
	ULONG Result;
	UINT64 VolumeHandle;   // 指令4 成功时有效，其余为 0
	WCHAR Message[QH_COMMAND_REPLY_MSG_CHARS];
} QH_COMMAND_REPLY, *PQH_COMMAND_REPLY;

// TargetTime100ns 使用本地时区 wall-clock（与 COW 记录 WallClock100ns 同口径）。
typedef struct _QH_PREVIEW_BEGIN_REQUEST
{
	GUID SourceVolumeGuid;
	UINT64 TargetTime100ns;
} QH_PREVIEW_BEGIN_REQUEST, *PQH_PREVIEW_BEGIN_REQUEST;

typedef struct _QH_PREVIEW_BEGIN_REPLY
{
	UINT64 PreviewHandle;
	UINT64 TargetTime100ns;
	UINT64 OldestRecoverable100ns;
	UINT64 NewestRecoverable100ns;
} QH_PREVIEW_BEGIN_REPLY, *PQH_PREVIEW_BEGIN_REPLY;

typedef struct _QH_PREVIEW_READ_REQUEST
{
	UINT64 PreviewHandle;
	UINT64 ByteOffset;
	ULONG ByteLength;
	ULONG Reserved;
} QH_PREVIEW_READ_REQUEST, *PQH_PREVIEW_READ_REQUEST;

typedef struct _QH_PREVIEW_END_REQUEST
{
	UINT64 PreviewHandle;
} QH_PREVIEW_END_REQUEST, *PQH_PREVIEW_END_REQUEST;

typedef struct _QH_PHASE_QUERY_REQUEST
{
	GUID SourceVolumeGuid;
} QH_PHASE_QUERY_REQUEST, *PQH_PHASE_QUERY_REQUEST;

typedef struct _QH_PHASE_QUERY_REPLY
{
	// QH_PHASE_GENERAL / PREVIEW / RECOVERY, or QH_STATUS_UNPROTECTED (-1).
	LONG Status;
	ULONG Reserved;
	GUID JournalPartitionGuid; // valid when Status >= 0 and protection is on
	UINT64 RecoveryTargetTime100ns;
} QH_PHASE_QUERY_REPLY, *PQH_PHASE_QUERY_REPLY;

typedef struct _QH_RECOVERY_BEGIN_REQUEST
{
	GUID SourceVolumeGuid;
	UINT64 TargetTime100ns;
} QH_RECOVERY_BEGIN_REQUEST, *PQH_RECOVERY_BEGIN_REQUEST;

typedef struct _QH_RECOVERY_BEGIN_REPLY
{
	ULONG Phase; // QH_PHASE_RECOVERY after history view is prepared
	UINT64 TargetTime100ns;
	UINT64 OldestRecoverable100ns;
	UINT64 NewestRecoverable100ns;
} QH_RECOVERY_BEGIN_REPLY, *PQH_RECOVERY_BEGIN_REPLY;

typedef struct _QH_RECOVERY_CONTROL_REQUEST
{
	GUID SourceVolumeGuid;
} QH_RECOVERY_CONTROL_REQUEST, *PQH_RECOVERY_CONTROL_REQUEST;

typedef struct _QH_RECOVERY_COMMIT_REPLY
{
	ULONG Phase; // QH_PHASE_GENERAL after synchronous writeback completes
	UINT64 TargetTime100ns;
} QH_RECOVERY_COMMIT_REPLY, *PQH_RECOVERY_COMMIT_REPLY;

typedef struct _QH_TIME_RANGE_QUERY_REQUEST
{
	GUID SourceVolumeGuid;
} QH_TIME_RANGE_QUERY_REQUEST, *PQH_TIME_RANGE_QUERY_REQUEST;

typedef struct _QH_TIME_RANGE_QUERY_REPLY
{
	ULONG HasRecords; // 1 if journal has COW history; 0 if empty
	ULONG Reserved;
	UINT64 OldestRecord100ns; // earliest surviving WallClock100ns
	UINT64 NewestRecord100ns; // latest WallClock100ns
} QH_TIME_RANGE_QUERY_REPLY, *PQH_TIME_RANGE_QUERY_REPLY;

#pragma pack(pop)
