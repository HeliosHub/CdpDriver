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

#include "QHIoctl.h"
#include "QHJournal.h"

#define QH_DRIVER_VERSION_STRING "1.1.0"
#define QH_DRIVER_BUILD_STRING   "20260721.9"

#if (NTDDI_VERSION >= NTDDI_WIN10_VB)
#define qhalloc(size) ExAllocatePool2(POOL_FLAG_NON_PAGED, size, 'NTAG')
#define qhfree(P) ExFreePoolWithTag(P, 'NTAG')
#else
#define qhalloc(size) ExAllocatePoolWithTag(NonPagedPool, size, 'NTAG')
#define qhfree(P) ExFreePoolWithTag(P, 'NTAG')
#endif

extern PDRIVER_OBJECT g_DriverObject;

static __forceinline NTSTATUS QHCompleteIrp(
	_In_ PIRP Irp,
	_In_ NTSTATUS Status,
	_In_ ULONG_PTR Information)
{
	Irp->IoStatus.Status = Status;
	Irp->IoStatus.Information = Information;
	IoCompleteRequest(Irp, IO_NO_INCREMENT);
	return Status;
}

typedef struct _QH_DEVICE_LIST_NODE
{
	PDEVICE_OBJECT DeviceObject;
	LIST_ENTRY Entry;
} QH_DEVICE_LIST_NODE, *PQH_DEVICE_LIST_NODE;

typedef struct _QH_VOLUME_HANDLE_ENTRY
{
	LIST_ENTRY Entry;
	UINT64 HandleId;
	HANDLE FileHandle;
	// Volume stack below our filter. Capture writes go here to bypass the
	// mounted filesystem's DASD write denial (STATUS_ACCESS_DENIED).
	PDEVICE_OBJECT TargetLowerDevice;
	UINT64 PartitionSize;
	ULONG SectorSize;
	QH_JOURNAL Journal;
	// One reference is held while the entry is in VolumeHandleList.  Capture
	// operations take an extra reference so CMD5 cannot close a handle while a
	// write callback is using it.
	volatile LONG ReferenceCount;
	BOOLEAN Closing;
	KEVENT NoReferences;
} QH_VOLUME_HANDLE_ENTRY, *PQH_VOLUME_HANDLE_ENTRY;

typedef struct _QH_DRIVER_EXTENSION
{
	LIST_ENTRY DeviceObjectListHead;
	KSPIN_LOCK DeviceObjectListLock;
	PDEVICE_OBJECT ControlDevice;

	// 指令4 打开的卷句柄表（内核 HANDLE，用户态只持有 HandleId）
	LIST_ENTRY VolumeHandleList;
	FAST_MUTEX VolumeHandleMutex;
	volatile LONGLONG VolumeHandleNextId;
	UINT64 CaptureTargetHandleId;

	// 按时间点读取的文件预览会话
	LIST_ENTRY PreviewSessionList;
	FAST_MUTEX PreviewSessionMutex;
	volatile LONGLONG PreviewSessionNextId;
} QH_DRIVER_EXTENSION, *PQH_DRIVER_EXTENSION;

typedef struct _QH_CORE QH_CORE, *PQH_CORE;

typedef struct _QH_PREVIEW_SESSION
{
	LIST_ENTRY Entry;
	UINT64 HandleId;
	UINT64 TargetTime100ns;
	UINT64 SourceVolumeHandleId;
	PQH_VOLUME_HANDLE_ENTRY JournalEntry;
	GUID SourceVolumeGuid;
	volatile LONG ReferenceCount;
	BOOLEAN Closing;
	KEVENT NoReferences;
} QH_PREVIEW_SESSION, *PQH_PREVIEW_SESSION;

typedef struct _QH_DEVICE_EXTENSION
{
	volatile LONG CaptureEnabled;
	volatile LONG Phase;
	GUID VolumeGuid;
	PDEVICE_OBJECT LowerDeviceObject;
	PDEVICE_OBJECT PhysicalDeviceObject;
	volatile LONG PagingPathCount;
	KSPIN_LOCK CaptureQueueLock;
	LIST_ENTRY CaptureQueue;
	KEVENT CaptureEvent;
	HANDLE CaptureThreadHandle;
	volatile LONG CaptureStopping;
	KMUTEX HistoryMutex;
	PQH_CORE Core;
} QH_DEVICE_EXTENSION, *PQH_DEVICE_EXTENSION;

typedef struct _QH_CAPTURE_ITEM
{
	LIST_ENTRY Entry;
	PIRP Irp;
} QH_CAPTURE_ITEM, *PQH_CAPTURE_ITEM;
