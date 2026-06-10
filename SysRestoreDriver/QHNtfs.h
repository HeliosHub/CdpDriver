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

#include <ntddk.h>

#pragma pack(push)
#pragma pack(1)

// NTFS 引导扇区完整结构 (512 字节)
// 物理布局: 跳转指令 + OEM ID (0x00-0x0A) | BPB + 扩展 BPB (0x0B-0x53)
//           | 引导代码 (0x54-0x1FD) | 结束标志 (0x1FE)
// 格式化时会分配前 16 个扇区给 $Boot 元数据文件，第一个扇区为引导扇区，后 15 个为 IPL
// 分区的最后一个扇区存放引导扇区的备用副本，以提高文件系统可靠性
typedef struct _QH_NTFS_BOOT_SECTOR
{
    UINT8  JumpInstruction[3];             // 偏移 0x00 | 3 字节 | 跳转指令，跳过 BPB 区域，通常为 EB 52 90
    UINT64 OEM_ID;                         // 偏移 0x03 | 8 字节 | OEM 标识字符串，NTFS 固定为 "NTFS    "（含 4 个尾部空格）

    // 字节偏移: 0x0B | 字段长度: 25 字节 | BPB (BIOS 参数块)
    // 存储卷的逻辑几何参数与介质信息，供引导代码和文件系统驱动使用
    struct
    {
        UINT16  BytesPerSector;       // 偏移 0x0B | 2 字节 | 每扇区字节数，通常为 512
        UINT8   SectorsPerCluster;    // 偏移 0x0D | 1 字节 | 每簇扇区数，必须是 2 的幂次
        UINT16  ReservedSectors;      // 偏移 0x0E | 2 字节 | 保留扇区数，NTFS 中通常为 0
        UINT8   Reserved_1[3];        // 偏移 0x10 | 3 字节 | 保留字段，必须为 0
        UINT16  NotUsed_1;            // 偏移 0x13 | 2 字节 | 未使用，NTFS 通常设为 0
        UINT8   MediaDescriptor;      // 偏移 0x15 | 1 字节 | 介质描述符，固定硬盘为 0xF8
        UINT16  Reserved_2;           // 偏移 0x16 | 2 字节 | 保留字段，必须为 0
        UINT16  SectorsPerTrack;      // 偏移 0x18 | 2 字节 | 每磁道扇区数（用于 INT 13h 兼容）
        UINT16  NumberOfHeads;        // 偏移 0x1A | 2 字节 | 磁头数（用于 INT 13h 兼容）
        UINT32  HiddenSectors;        // 偏移 0x1C | 4 字节 | 该卷之前的隐藏扇区数（即分区起始 LBA）
        UINT32  NotUsed_2;            // 偏移 0x20 | 4 字节 | 未使用，NTFS 通常设为 0
    } BPB;

    // 字节偏移: 0x24 | 字段长度: 48 字节 | 扩展 BPB
    // 存储 NTFS 特有的文件系统参数，帮助 Ntldr 在启动时定位主文件表 (MFT)
    // 与 FAT 不同，NTFS 的 MFT 没有固定的预定义位置，可在必要时进行重定位
    // 若此区域数据损坏，Windows NT/2000 将认为卷未格式化
    struct
    {
        UINT32  NotUsed_1;                          // 偏移 0x24 | 4 字节 | 未使用字段
        UINT64  TotalSectors;                       // 偏移 0x28 | 8 字节 | 卷上的总扇区数
        // MFT_LCN -> QH_NTFP_MFT_FILE_RECORD_HEADER
        UINT64  MFT_LCN;                            // 偏移 0x30 | 8 字节 | $MFT 的起始逻辑簇号 (LCN)
        UINT64  MFTMirr_LCN;                        // 偏移 0x38 | 8 字节 | $MFTMirr（MFT 镜像）的起始逻辑簇号
        INT32   ClustersPerFileRecordSegment;       // 偏移 0x40 | 4 字节 | 每个文件记录段所占簇数，以负数补码形式表示
        // 例如 0xF6 (-10) 表示记录大小为 2^10 = 1024 字节
        INT8    ClustersPerIndexBuffer;             // 偏移 0x44 | 1 字节 | 每个索引缓冲区所占簇数，同样为负数补码形式
        UINT8   NotUsed_2[3];                       // 偏移 0x45 | 3 字节 | 保留字段
        UINT64  VolumeSerialNumber;                 // 偏移 0x48 | 8 字节 | 卷序列号
        UINT32  Checksum;                           // 偏移 0x50 | 4 字节 | 引导扇区校验和
    } ExtendedBPB;

    UINT8  BootstrapCode[426];                      // 偏移 0x54 | 426 字节 | 引导代码（初始程序加载器 IPL），负责定位并加载启动管理器
    UINT16 End;                                     // 偏移 0x1FE | 2 字节 | 引导扇区结束标志，必须为 0xAA55
} QH_NTFS_BOOT_SECTOR, * PQH_NTFS_BOOT_SECTOR;

// MFT
// 根据QH_NTFS_BOOT_SECTOR.ExtendedBPB.ClustersPerFileRecordSegment查找下一条MFT记录
typedef struct _QH_NTFS_MFT_RECORD
{
    UINT32  Magic;
    UINT16  UpdateSequenceOffset;
    UINT16  WordsSizeOfUpdateSequence;
    UINT64  LSN;
    UINT16  SequenceNumber;
    UINT16  HardLinkCount;
    UINT16  FirstAttributeOffset;
    UINT16  Flags;
    UINT32  FileRecordRealSize;
    UINT32  FileRecordAllocatedSize;
    UINT64  BaseFileRecordFileReference;
    UINT16  NextAttributeID;
    UINT16  Align4ByteBoundary;
    UINT32  MFTRecordNumber;
}QH_NTFS_MFT_RECORD, *PQH_NTFS_MFT_RECORD;

#pragma warning(disable: 4201)
typedef struct _QH_NTFS_MFT_ATTRIBUTE
{
    UINT32  AttributeType;
    UINT32  Length;
    UINT8   ResidentFlag;   // 0 == Resident, 1 == NoResident
    UINT8   NameLength;
    UINT16  NameOffset;
    UINT16  Flags;
    UINT16  AttributeID;

    union
    {
        // 驻留属性
        // 类似文件名的时间戳、短文件名等等属性占用空间不大的属性 常设置为驻留属性
        struct
        {
            UINT32  AttributeLength;
            UINT16  AttributeOffset;
            UINT8   IndexFlag;
            UINT8   Padding;
        } Resident;

        // 非驻留属性
        // 类似文件内容、大型目录索引 设置非驻留属性 
        // 非驻留属性请查看 QH_MFT_DATARUN
        struct
        {
            UINT64  StartVCN;
            UINT64  LastVCN;
            UINT16  DataRunOffset;
            UINT16  CompressionUnitSize;
            UINT32  Padding;
            UINT64  AttributeAllocatedSize;
            UINT64  AttributeRealSize;
            UINT64  StreamDataInitializedSize;
        }NoResident;
    };
}QH_NTFS_MFT_ATTRIBUTE, *PQH_NTFS_MFT_ATTRIBUTE;

#pragma pack(pop)

#define QH_CLUSTER_BYTES(pBootSector) (UINT64)((pBootSector)->BPB.SectorsPerCluster * (pBootSector)->BPB.BytesPerSector)
#define QH_LCN_TO_BYTE_OFFSET(pBootSector, LCN) (UINT64)((LCN) * QH_CLUSTER_BYTES(pBootSector))
#define QH_BYTE_OFFSET_TO_LCN(pBootSector, Offset)  (UINT64)((Offset) / QH_CLUSTER_BYTES(pBootSector))

static inline UINT32 QH_MFT_NEXT_BYTE_OFFSET(const PQH_NTFS_BOOT_SECTOR pBoot)
{
    // 必须取低字节 NTFS给出的官方示例中 此字段为0x000000F6 为246
    // 如果按照单字节有符号算 为-10
    // 如果用4字节算 并非0xFFFFFFF6 所以只能是246
    // 因为windows读取时一般都是-10 所以我猜测windows在读取此字段时只读了一个低字节
    INT8 clustersPerRecord = (INT8)pBoot->ExtendedBPB.ClustersPerFileRecordSegment; // 取低字节有符号
    UINT32 advance;
    if (clustersPerRecord >= 0) {
        advance = (UINT32)clustersPerRecord * pBoot->BPB.BytesPerSector * pBoot->BPB.SectorsPerCluster;
    }
    else {
        advance = 1ULL << (-clustersPerRecord);   // 2^(-clustersPerRecord)
    }
    return advance;
}
