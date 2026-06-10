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
#include "QHNtfs.h"
#include "QHOffsetHash.h"

// 前置声明（避免 QHBitmap.h 与 QHEngineDefs.h 循环依赖）
struct _QH_BITMAP;
typedef struct _QH_BITMAP* PQH_BITMAP;

// 直写放行扇区区间 — 初始化阶段一次性填充, IRP 路径上无锁只读
//
// 仅记录以下两类扇区, 它们必须直通真实磁盘而不被重定向:
//   1. _qh_protect_state.data — UI 写入后必须重启读得到, 文件大小固定(1MB),
//      不会动态扩缩, 扇区位置在保护期间永不变化
//   2. $Volume MFT 记录 #3 主区与镜像 — 维护 NTFS dirty flag, 若被重定向
//      则"清 dirty"无法落盘, 多次重启后 Windows 进入 WinRE
//
// 不收录 pagefile.sys / hiberfil.sys / bootstat.dat 等可变大小文件:
// 这些文件在保护期间可能被用户态动态扩容/缩小, 新扇区不在本表中,
// 写入会被驱动重定向 → 系统读取错位数据 → 蓝屏或更严重
#define QH_PROTECT_RANGE_MAX 8

typedef struct _QH_PROTECT_RANGE
{
	ULONGLONG StartSector;   // 起始扇区号
	ULONGLONG SectorCount;   // 扇区数量
} QH_PROTECT_RANGE, *PQH_PROTECT_RANGE;

#define QH_ALLOCATE_NO_TAG

#if (NTDDI_VERSION >= NTDDI_WIN10_VB)
#ifdef QH_ALLOCATE_NO_TAG
#define qhalloc(size) ExAllocatePool2(POOL_FLAG_NON_PAGED, size, 'NTAG')
#define qhalloc2(size) ExAllocatePool2(POOL_FLAG_PAGED, size, 'NTAG')
#define qhcalloc(size) qhalloc(size)
#else
#define qhalloc(size, tag) ExAllocatePool2(POOL_FLAG_NON_PAGED, size, tag)
#define qhalloc2(size, tag) ExAllocatePool2(POOL_FLAG_PAGED, size, tag)
#define qhcalloc(size, tag) qhalloc(size, tag)
#endif
#else
#ifdef QH_ALLOCATE_NO_TAG
#define qhalloc(size) ExAllocatePoolWithTag(NonPagedPool, size, 'NTAG')
#define qhalloc2(size) ExAllocatePoolWithTag(PagedPool, size, 'NTAG')
#define qhcalloc(size) ExAllocatePoolZero(NonPagedPool, size, 'NTAG')
#else
#define qhalloc(size, tag) ExAllocatePoolWithTag(NonPagedPool, size, tag)
#define qhalloc2(size, tag) ExAllocatePoolWithTag(PagedPool, size, tag)
#define qhcalloc(size, tag) ExAllocatePoolZero(NonPagedPool, size, tag)
#endif
#endif

#ifdef QH_ALLOCATE_NO_TAG
#define qhfree(P) ExFreePoolWithTag(P, 'NTAG')
#else
#define qhfree(P, TAG) ExFreePoolWithTag(P, TAG)
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

/* DRIVER */
typedef struct _QH_DEVICE_LIST_NODE
{
	// 过滤设备指针
	PDEVICE_OBJECT DeviceObject;
	LIST_ENTRY Entry;
}QH_DEVICE_LIST_NODE, *PQH_DEVICE_LIST_NODE;

typedef struct _QH_DRIVER_EXTENSION
{
	// 过滤设备链表
	LIST_ENTRY DeviceObjectListHead;	// PQH_DEVICE_LIST_NODE
	// 过滤设备链表自旋锁
	KSPIN_LOCK DeviceObjectListLock;
	// 通讯设备
	PDEVICE_OBJECT ControlDevice;
	// Boot 重新初始化是否已完成
	// 0 = 尚未完成，IOCTL_VOLUME_ONLINE 不得激活保护
	// 1 = 已完成，IOCTL_VOLUME_ONLINE 可正常激活保护
	volatile LONG BootReinitDone;
}QH_DRIVER_EXTENSION, *PQH_DRIVER_EXTENSION;

/* DEVICE */
typedef struct _QH_DEVICE_EXTENSION
{
	// 当前卷设备是否初始化完毕
	BOOLEAN Initialized;

	// 下级设备
	PDEVICE_OBJECT LowerDeviceObject;
	// 物理设备
	PDEVICE_OBJECT PhysicalDeviceObject;
	// 当前卷的BOOT SECTOR
	QH_NTFS_BOOT_SECTOR NtfsBootSector;
	// 扇区级空闲/占用位图（基于 RTL_BITMAP 的简单包装）
	PQH_BITMAP SectorBitmap;
	// 直写放行扇区区间表 — 仅 _qh_protect_state.data 与 $Volume MFT#3 主/镜
	// init 阶段一次性填充, IRP 路径上无锁只读 (见 QH_PROTECT_RANGE 注释)
	QH_PROTECT_RANGE ProtectRanges[QH_PROTECT_RANGE_MAX];
	ULONG ProtectRangeCount;
	// 上次扫描空闲扇区的位置（加速 RtlFindClearBits 起点提示）
	ULONG LastScanIndex;
	// 位图互斥体
	FAST_MUTEX BitmapMutex;
	// 偏移记录表（键=原始扇区索引，值=目标扇区索引）
	PQH_OFFSET_HASH OffsetHash;
	// 偏移记录表互斥体（FAST_MUTEX：RTL_GENERIC_TABLE操作会分配/释放内存，必须在APC_LEVEL执行）
	FAST_MUTEX OffsetHashMutex;

	// IRP读写队列
	KSPIN_LOCK PendingIrpQueueLock;
	LIST_ENTRY PendingIrpQueue;

	PIO_WORKITEM IoWorkItem;            // 工作项（处理高IRQL排队的读写IRP）
	LONG IoWorkItemQueued;              // 原子标志，防止重复调度工作项
	KEVENT WorkItemFinished;
	volatile LONG PagingPathCount;
}QH_DEVICE_EXTENSION, *PQH_DEVICE_EXTENSION;
