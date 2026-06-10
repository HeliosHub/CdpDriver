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

#include "QHEngineDefs.h"
#include "QHNtfs.h"

// ============================================================================
// 位图 — 基于 Windows 内核 RTL_BITMAP 的简单包装
//
// 设计取舍:
//   一次性分配整块位图缓冲区。1 TB 卷需要 256 MB（每扇区 1 位），对常见的
//   系统盘/数据盘量级完全可接受。若未来需要支持超大容量场景，可在此包装
//   层之上再加稀疏分槽，而不改变上层调用者代码。
//
//   选用 RTL_BITMAP 是 Microsoft 内核公开 API, 行为有官方文档保证, 无需
//   自行验证位操作算法的正确性与稳定性。
// ============================================================================

typedef struct _QH_BITMAP
{
	RTL_BITMAP  Header;       // RTL_BITMAP 头（描述位图大小与缓冲区位置）
	// 位缓冲区由 QHBitmapCreate 单独 qhalloc 分配, 由 Header.Buffer 指向
} QH_BITMAP, *PQH_BITMAP;

// 创建位图 — 全部位初始化为 0
NTSTATUS QHBitmapCreate(
	_Out_ PQH_BITMAP* Bitmap,
	_In_ ULONGLONG SizeInBits
);

// 释放位图
VOID QHBitmapFree(_In_ PQH_BITMAP Bitmap);

// 置位（Value = TRUE 设为 1，FALSE 清 0）
// 索引越界视为静默忽略，与 RTL_BITMAP 行为一致
VOID QHBitmapSet(
	_In_ PQH_BITMAP Bitmap,
	_In_ ULONGLONG Index,
	_In_ BOOLEAN Value
);

// 测试某位是否为 1
BOOLEAN QHBitmapTest(
	_In_ PQH_BITMAP Bitmap,
	_In_ ULONGLONG Index
);

// 从 HintIndex 开始查找下一个清零位（值为 0 的位），返回其索引
// 找不到时返回 (ULONGLONG)-1
// HintIndex 仅作为搜索起点提示；若该位置之后没有，会自动从 0 重新搜索一次
ULONGLONG QHBitmapFindNextClear(
	_In_ PQH_BITMAP Bitmap,
	_In_ ULONGLONG HintIndex
);

// 范围批量测试 — 区间内是否所有位都为 1
// 越界（StartIndex + Count 超出位图大小）返回 FALSE
// 用于在 IRP 路径上一次性判定整段是否全部命中 SectorBitmap 等场景,
// 避免对 [StartIndex, StartIndex+Count) 范围内逐位 QHBitmapTest 的 N 次锁切换
BOOLEAN QHBitmapAreBitsSet(
	_In_ PQH_BITMAP Bitmap,
	_In_ ULONGLONG StartIndex,
	_In_ ULONGLONG Count
);

// 范围批量测试 — 区间内是否所有位都为 0
// 越界返回 FALSE
// 用于一次性判定整段是否全部空闲 / 无重定向
BOOLEAN QHBitmapAreBitsClear(
	_In_ PQH_BITMAP Bitmap,
	_In_ ULONGLONG StartIndex,
	_In_ ULONGLONG Count
);

// 范围批量置位 — 把 [StartIndex, StartIndex+Count) 全部设为 1
// 越界静默忽略, 与 QHBitmapSet 同款风格
// 注意: 调用者必须自行持有上层互斥体以保证 "测试 + 占用" 的原子性
VOID QHBitmapSetRange(
	_In_ PQH_BITMAP Bitmap,
	_In_ ULONGLONG StartIndex,
	_In_ ULONGLONG Count
);

// ============================================================================
// NTFS 位图读取（保留，从 $Bitmap 读取簇级位图数据）
// ============================================================================

// 获得该 NTFS 卷的 BootSector
NTSTATUS QHGetBootSector(
	_In_ PDEVICE_OBJECT FilterDeviceObject,
	_Out_ PQH_NTFS_BOOT_SECTOR BootSector
);

// 从该卷指定位置读取数据
NTSTATUS QHReadVolumeData(
	_In_ PDEVICE_OBJECT FilterDeviceObject,
	_In_ LONGLONG Offset,
	_In_ UINT32 ByteSize,
	_Inout_ PVOID ByteBuffer
);

// 向该卷指定位置写入数据
NTSTATUS QHWriteVolumeData(
	_In_ PDEVICE_OBJECT FilterDeviceObject,
	_In_ LONGLONG Offset,
	_In_ UINT32 ByteSize,
	_Inout_ PVOID ByteBuffer
);

// 获取 $Bitmap 所有数据（簇级位图）
NTSTATUS QHGetBitmapData(
	_In_ PDEVICE_OBJECT FilterDeviceObject,
	_In_ PQH_NTFS_BOOT_SECTOR BootSector,
	_Out_ PVOID* BitmapData,
	_Out_ PUINT64 BitmapSize
);

// ============================================================================
// 扇区级位图构建
// ============================================================================

// 从 NTFS 簇级位图构建扇区级空闲位图
// 每个占用簇 → 该簇内所有扇区标记为占用
NTSTATUS QHBuildSectorBitmap(
	_In_ PQH_NTFS_BOOT_SECTOR BootSector,
	_In_ PVOID ClusterBitmapData,
	_In_ UINT64 ClusterBitmapSize,
	_Out_ PQH_BITMAP* SectorBitmap
);

// ============================================================================
// ProtectRanges 填充（直写放行扇区记录）
// ============================================================================

// 将 _qh_protect_state.data 和 $Volume MFT#3 主/镜的扇区追加到 DevExt->ProtectRanges
//
// 仅收录"保护期间扇区位置不会变化"的实体:
//   * _qh_protect_state.data: 文件大小固定 1MB, UI 创建后不改变, 物理扇区静态
//   * $Volume MFT 记录 #3 (主区 + 镜像): NTFS 元数据, 位置由文件系统固定分配
//
// 不收录 pagefile.sys / hiberfil.sys / bootstat.dat 等 — 这些文件可能被
// 用户态动态扩容/缩小, 新扇区不在表中会被误重定向 → 系统读取错位数据 → 蓝屏
VOID QHPopulateProtectRanges(
	_In_ PDEVICE_OBJECT FilterDeviceObject,
	_In_ PQH_DEVICE_EXTENSION DevExt
);

// 追加一段保护扇区区间到 DevExt->ProtectRanges
// 超出 QH_PROTECT_RANGE_MAX 时丢弃并 KdPrint 一行告警 (理论上不会发生)
// init 阶段调用, 无并发, 无需加锁
VOID QHAddProtectRange(
	_Inout_ PQH_DEVICE_EXTENSION DevExt,
	_In_ ULONGLONG StartSector,
	_In_ ULONGLONG SectorCount
);

// 单扇区: 是否落在任一保护区间内
// 无锁: ProtectRanges 在 init 阶段填充, IRP 路径只读
BOOLEAN QHIsSectorProtected(
	_In_ PQH_DEVICE_EXTENSION DevExt,
	_In_ ULONGLONG SectorIndex
);

// 范围: [StartSector, StartSector+Count) 是否整段都落在某一个保护区间内
// 用于写 IRP 的 fast-path 短路判定
BOOLEAN QHAreSectorsProtected(
	_In_ PQH_DEVICE_EXTENSION DevExt,
	_In_ ULONGLONG StartSector,
	_In_ ULONGLONG Count
);

// ============================================================================
// 保护状态文件
// ============================================================================

// 从卷根目录的保护状态文件读取首字节
// 返回值:
//   1  : 文件存在且首字节非零 → 保护开启
//   0  : 文件存在且首字节为零 → 用户已关闭保护
//   -1 : 文件不存在或读取失败 → 此卷未配置保护
//
// 此函数在 PASSIVE_LEVEL 调用, 在 QHBootReinitializationRoutine 或
// IOCTL_VOLUME_ONLINE 路径上读取, 此时驱动尚未激活保护(Initialized=FALSE),
// 走默认 IRP 透传到下层设备, 不经过本驱动重定向
INT32 QHReadProtectStateFromFile(
	_In_ PDEVICE_OBJECT PhysicalDeviceObject
);
