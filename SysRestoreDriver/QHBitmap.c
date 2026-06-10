#include <ntifs.h>
#include "QHBitmap.h"

// ============================================================================
// 位图实现 — 基于 Windows 内核 RTL_BITMAP API 的薄包装
//
// RTL_BITMAP 一次性分配整块缓冲区, 索引类型为 ULONG, 因此单张位图最多支持
// 2^32 - 1 位 (~4G 位). 在 512B 扇区下对应约 2 TB 卷, 在 4 KB 扇区下对应
// 约 16 TB 卷, 覆盖绝大多数实际场景. 若需要支持更大卷, 可在此层之上再加
// 一层稀疏分槽, 而上层调用接口保持不变.
//
// 缓冲区从 NonPagedPool 分配, 因为 IRP 派遣可能在 DISPATCH_LEVEL 触达;
// 位图操作本身在持有 FAST_MUTEX 时被调用, 已确保在 PASSIVE_LEVEL.
// ============================================================================

NTSTATUS QHBitmapCreate(
	_Out_ PQH_BITMAP* Bitmap,
	_In_ ULONGLONG SizeInBits)
{
	PQH_BITMAP New;
	PULONG BitBuffer;
	SIZE_T BufferBytes;

	*Bitmap = NULL;

	// RTL_BITMAP 用 ULONG 索引, 位数上限即 ULONG 上限
	if (SizeInBits == 0 || SizeInBits > MAXULONG)
		return STATUS_INVALID_PARAMETER;

	New = (PQH_BITMAP)qhalloc(sizeof(QH_BITMAP));
	if (!New)
		return STATUS_INSUFFICIENT_RESOURCES;

	// 缓冲区按 ULONG 对齐分配, 字节数 = ceil(SizeInBits / 32) * sizeof(ULONG)
	BufferBytes = ((SIZE_T)((SizeInBits + 31) / 32)) * sizeof(ULONG);
	BitBuffer = (PULONG)qhalloc(BufferBytes);
	if (!BitBuffer)
	{
		qhfree(New);
		return STATUS_INSUFFICIENT_RESOURCES;
	}

	RtlZeroMemory(BitBuffer, BufferBytes);
	// RtlInitializeBitMap 会把 BitBuffer 和位数都存入 Header
	RtlInitializeBitMap(&New->Header, BitBuffer, (ULONG)SizeInBits);

	*Bitmap = New;
	return STATUS_SUCCESS;
}

VOID QHBitmapFree(_In_ PQH_BITMAP Bitmap)
{
	if (!Bitmap)
		return;

	if (Bitmap->Header.Buffer)
		qhfree(Bitmap->Header.Buffer);

	qhfree(Bitmap);
}

VOID QHBitmapSet(
	_In_ PQH_BITMAP Bitmap,
	_In_ ULONGLONG Index,
	_In_ BOOLEAN Value)
{
	if (Index >= Bitmap->Header.SizeOfBitMap)
		return;

	if (Value)
		RtlSetBit(&Bitmap->Header, (ULONG)Index);
	else
		RtlClearBit(&Bitmap->Header, (ULONG)Index);
}

BOOLEAN QHBitmapTest(
	_In_ PQH_BITMAP Bitmap,
	_In_ ULONGLONG Index)
{
	if (Index >= Bitmap->Header.SizeOfBitMap)
		return FALSE;

	// RtlCheckBit 返回 0 或 1
	return RtlCheckBit(&Bitmap->Header, (ULONG)Index) ? TRUE : FALSE;
}

ULONGLONG QHBitmapFindNextClear(
	_In_ PQH_BITMAP Bitmap,
	_In_ ULONGLONG HintIndex)
{
	ULONG Found;
	ULONG Hint;

	if (HintIndex >= Bitmap->Header.SizeOfBitMap)
		Hint = 0;
	else
		Hint = (ULONG)HintIndex;

	// RtlFindClearBits 在找不到时会自动从位图头部回绕一次搜索
	// 单 bit 场景下传 1 即可
	Found = RtlFindClearBits(&Bitmap->Header, 1, Hint);
	if (Found == 0xFFFFFFFF)
		return (ULONGLONG)-1;

	return (ULONGLONG)Found;
}

// ============================================================================
// 范围批量 API — RTL_BITMAP 自带 SIMD 友好的位字扫描, 比逐位 Test 快约 64x
// 主要用途: IRP 路径上"整段命中 ProtectRanges" / "整段空闲" / "整段无重定向"
// 的快速短路判定, 一次锁 + 一次范围扫描替代 N 次单点锁 + N 次 Test
// ============================================================================

BOOLEAN QHBitmapAreBitsSet(
	_In_ PQH_BITMAP Bitmap,
	_In_ ULONGLONG StartIndex,
	_In_ ULONGLONG Count)
{
	if (Count == 0)
		return TRUE;

	// 越界视为否定 (与单点 API 越界静默处理同款风格, 上层不必额外校验)
	if (StartIndex >= Bitmap->Header.SizeOfBitMap)
		return FALSE;
	if (Count > Bitmap->Header.SizeOfBitMap - StartIndex)
		return FALSE;

	return RtlAreBitsSet(&Bitmap->Header, (ULONG)StartIndex, (ULONG)Count);
}

BOOLEAN QHBitmapAreBitsClear(
	_In_ PQH_BITMAP Bitmap,
	_In_ ULONGLONG StartIndex,
	_In_ ULONGLONG Count)
{
	if (Count == 0)
		return TRUE;

	if (StartIndex >= Bitmap->Header.SizeOfBitMap)
		return FALSE;
	if (Count > Bitmap->Header.SizeOfBitMap - StartIndex)
		return FALSE;

	return RtlAreBitsClear(&Bitmap->Header, (ULONG)StartIndex, (ULONG)Count);
}

VOID QHBitmapSetRange(
	_In_ PQH_BITMAP Bitmap,
	_In_ ULONGLONG StartIndex,
	_In_ ULONGLONG Count)
{
	if (Count == 0)
		return;

	if (StartIndex >= Bitmap->Header.SizeOfBitMap)
		return;

	// 截断越界部分, 与 RTL_BITMAP 单点写入静默忽略越界的语义一致
	if (Count > Bitmap->Header.SizeOfBitMap - StartIndex)
		Count = Bitmap->Header.SizeOfBitMap - StartIndex;

	RtlSetBits(&Bitmap->Header, (ULONG)StartIndex, (ULONG)Count);
}

// NTFS 位图读取(扇区读写 非API)
NTSTATUS QHGetBootSector(
	_In_ PDEVICE_OBJECT FilterDeviceObject,
	_Out_ PQH_NTFS_BOOT_SECTOR BootSector
)
{
	return QHReadVolumeData(
		FilterDeviceObject,
		0,
		sizeof(QH_NTFS_BOOT_SECTOR),
		BootSector
	);
}

// 对 MFT 记录缓冲区执行 Update Sequence Array 修复
static NTSTATUS QHApplyUpdateSequenceArray(
	_Inout_ PQH_NTFS_MFT_RECORD RecordBuffer,
	_In_ UINT32 RecordSize,
	_In_ UINT32 BytesPerSector
)
{
	UINT16 UsaOffset;
	UINT16 UsaWordCount;
	UINT16 Usn;
	UINT32 localSectorCount;
	UINT32 UsaArrayByteOffset;
	UINT32 i;

	if (RecordSize < BytesPerSector || RecordSize % BytesPerSector != 0)
		return STATUS_DISK_CORRUPT_ERROR;

	UsaOffset = RecordBuffer->UpdateSequenceOffset;
	UsaWordCount = RecordBuffer->WordsSizeOfUpdateSequence;
	localSectorCount = RecordSize / BytesPerSector;

	if (UsaWordCount != (UINT16)(1 + localSectorCount))
		return STATUS_DISK_CORRUPT_ERROR;

	if ((UINT32)UsaOffset + (UINT32)UsaWordCount * sizeof(UINT16) > RecordSize)
		return STATUS_DISK_CORRUPT_ERROR;

	UsaArrayByteOffset = UsaOffset;
	RtlCopyMemory(&Usn, (PUCHAR)RecordBuffer + UsaArrayByteOffset, sizeof(UINT16));

	for (i = 0; i < localSectorCount; i++)
	{
		UINT32 LastWordPos = (i + 1) * BytesPerSector - 2;
		UINT16 SectorTailValue;
		UINT16 OriginalValue;

		RtlCopyMemory(&SectorTailValue, (PUCHAR)RecordBuffer + LastWordPos, sizeof(UINT16));

		if (SectorTailValue != Usn)
			return STATUS_DISK_CORRUPT_ERROR;

		RtlCopyMemory(&OriginalValue,
			(PUCHAR)RecordBuffer + UsaArrayByteOffset + (i + 1) * sizeof(UINT16),
			sizeof(UINT16));

		RtlCopyMemory((PUCHAR)RecordBuffer + LastWordPos, &OriginalValue, sizeof(UINT16));
	}

	return STATUS_SUCCESS;
}

static NTSTATUS _QHGetBitmapRecord(
	_In_ PDEVICE_OBJECT FilterDeviceObject,
	_In_ PQH_NTFS_BOOT_SECTOR BootSector,
	_Inout_ PQH_NTFS_MFT_RECORD BitmapRecord
)
{
	NTSTATUS Status;
	UINT32 RecordOffset = QH_MFT_NEXT_BYTE_OFFSET(BootSector);
	UINT64 StartOffset = BootSector->ExtendedBPB.MFT_LCN * BootSector->BPB.SectorsPerCluster * BootSector->BPB.BytesPerSector;

	StartOffset += RecordOffset * 6;

	Status = QHReadVolumeData(
		FilterDeviceObject,
		StartOffset,
		RecordOffset,
		BitmapRecord
	);
	if (!NT_SUCCESS(Status))
		return Status;

	Status = QHApplyUpdateSequenceArray(BitmapRecord, RecordOffset,
		BootSector->BPB.BytesPerSector);

	return Status;
}

// 异步 IRP 完成例程：释放 MDL、回收 IRP、通知等待者
static NTSTATUS QHIssueVolumeIOCompletion(
	_In_ PDEVICE_OBJECT DeviceObject,
	_In_ PIRP Irp,
	_In_ PVOID Context
)
{
	PMDL mdl;

	UNREFERENCED_PARAMETER(DeviceObject);

	// 释放系统缓冲区（若有）
	if (Irp->AssociatedIrp.SystemBuffer && (Irp->Flags & IRP_DEALLOCATE_BUFFER))
	{
		qhfree(Irp->AssociatedIrp.SystemBuffer);
	}

	// 释放 MDL 链
	while (Irp->MdlAddress)
	{
		mdl = Irp->MdlAddress;
		Irp->MdlAddress = mdl->Next;
		MmUnlockPages(mdl);
		IoFreeMdl(mdl);
	}

	// 通知等待线程
	if (Irp->PendingReturned && Context != NULL)
	{
		*Irp->UserIosb = Irp->IoStatus;
		KeSetEvent((PKEVENT)Context, IO_DISK_INCREMENT, FALSE);
	}

	IoFreeIrp(Irp);

	// IRP 已释放，不可再访问
	return STATUS_MORE_PROCESSING_REQUIRED;
}

// 同步发起一次卷 I/O
// 使用 IoBuildAsynchronousFsdRequest 构建异步 IRP，写操作设置 SL_FORCE_DIRECT_WRITE
static NTSTATUS QHIssueVolumeIO(
	_In_ UCHAR MajorFunction,
	_In_ PDEVICE_OBJECT FilterDeviceObject,
	_In_ LONGLONG Offset,
	_In_ UINT32 ByteSize,
	_Inout_ PVOID ByteBuffer
)
{
	PQH_DEVICE_EXTENSION DeviceExtension = (PQH_DEVICE_EXTENSION)FilterDeviceObject->DeviceExtension;
	IO_STATUS_BLOCK Ios;
	KEVENT WaitEvent;
	PIRP Irp;
	NTSTATUS Status;

	// 构建异步 IRP（不关联事件，由完成例程通知）
	Irp = IoBuildAsynchronousFsdRequest(
		MajorFunction,
		DeviceExtension->LowerDeviceObject,
		ByteBuffer,
		ByteSize,
		(PLARGE_INTEGER)&Offset,
		&Ios
	);

	if (!Irp)
	{
		return STATUS_INSUFFICIENT_RESOURCES;
	}

	// Vista+ 内核写卷必须设置此标志，否则直接写入会被卷管理器拒绝
	// 踩坑之一, 如果没有这个标志, 复制文件会卡死, 但是不知道为什么修改文件是可以的
	if (IRP_MJ_WRITE == MajorFunction)
	{
		IoGetNextIrpStackLocation(Irp)->Flags |= SL_FORCE_DIRECT_WRITE;
	}

	// 设置完成例程：释放 IRP 资源 + 通知等待者
	KeInitializeEvent(&WaitEvent, NotificationEvent, FALSE);
	IoSetCompletionRoutine(Irp, QHIssueVolumeIOCompletion, &WaitEvent, TRUE, TRUE, TRUE);

	Status = IoCallDriver(DeviceExtension->LowerDeviceObject, Irp);
	if (Status == STATUS_PENDING)
	{
		KeWaitForSingleObject(&WaitEvent, Executive, KernelMode, FALSE, NULL);
		Status = Ios.Status;
	}

	return Status;
}

NTSTATUS QHReadVolumeData(
	_In_ PDEVICE_OBJECT FilterDeviceObject,
	_In_ LONGLONG Offset,
	_In_ UINT32 ByteSize,
	_Inout_ PVOID ByteBuffer)
{
	return QHIssueVolumeIO(IRP_MJ_READ, FilterDeviceObject, Offset, ByteSize, ByteBuffer);
}

NTSTATUS QHWriteVolumeData(
	_In_ PDEVICE_OBJECT FilterDeviceObject,
	_In_ LONGLONG Offset,
	_In_ UINT32 ByteSize,
	_Inout_ PVOID ByteBuffer
)
{
	return QHIssueVolumeIO(IRP_MJ_WRITE, FilterDeviceObject, Offset, ByteSize, ByteBuffer);
}

NTSTATUS QHGetBitmapData(
	_In_ PDEVICE_OBJECT FilterDeviceObject,
	_In_ PQH_NTFS_BOOT_SECTOR BootSector,
	_Out_ PVOID* BitmapData,
	_Out_ PUINT64 BitmapSize
)
{
	NTSTATUS Status = STATUS_SUCCESS;
	PQH_NTFS_MFT_RECORD BitmapRecord = NULL;
	PQH_NTFS_MFT_ATTRIBUTE BitmapAttr = NULL;
	PUINT8 PRecordEnd = NULL;
	PUINT8 PRun = NULL;
	PUINT8 PRunEnd = NULL;
	UINT64 CurrentBaseLCN = 0;
	UINT64 ReadLCNByteSize = 0;
	PUINT8 PBitmap = NULL;

	UINT32 recordSize = QH_MFT_NEXT_BYTE_OFFSET(BootSector);

	BitmapRecord = qhalloc(recordSize);
	if (!BitmapRecord)
	{
		Status = STATUS_MEMORY_NOT_ALLOCATED;
		goto cleanup;
	}

	Status = _QHGetBitmapRecord(FilterDeviceObject, BootSector, BitmapRecord);
	if (!NT_SUCCESS(Status))
		goto cleanup;

	if (BitmapRecord->Magic != 0x454C4946) // 'FILE'
	{
		Status = STATUS_DISK_CORRUPT_ERROR;
		goto cleanup;
	}

	BitmapAttr = (PQH_NTFS_MFT_ATTRIBUTE)((PUINT8)BitmapRecord + BitmapRecord->FirstAttributeOffset);
	PRecordEnd = (PUINT8)BitmapRecord + BitmapRecord->FileRecordRealSize;

	while ((PUINT8)BitmapAttr < PRecordEnd && BitmapAttr->Length > 0) {
		if (BitmapAttr->AttributeType == 0x80)
			break;
		BitmapAttr = (PQH_NTFS_MFT_ATTRIBUTE)((PUINT8)BitmapAttr + BitmapAttr->Length);
	}

	if ((PUINT8)BitmapAttr >= PRecordEnd || BitmapAttr->AttributeType != 0x80) {
		Status = STATUS_UNSUCCESSFUL;
		goto cleanup;
	}

	if (BitmapAttr->ResidentFlag != 1)
	{
		Status = STATUS_NOT_SUPPORTED;
		goto cleanup;
	}

	PRun = (PUINT8)BitmapAttr + BitmapAttr->NoResident.DataRunOffset;
	*BitmapSize = BitmapAttr->NoResident.AttributeRealSize;

	UINT64 allocSize = *BitmapSize + BootSector->BPB.BytesPerSector;
	PBitmap = qhalloc(allocSize);
	if (!PBitmap) {
		Status = STATUS_MEMORY_NOT_ALLOCATED;
		goto cleanup;
	}

	PRunEnd = (PUINT8)BitmapAttr + BitmapAttr->Length;
	while (PRun < PRunEnd && *PRun != 0x00)
	{
		UINT8 Header = *PRun;
		UINT32 SizeBytes = Header & 0x0F;
		UINT32 OffsetBytes = Header >> 4;
		UINT64 Length = 0;
		INT64 LcnOffset = 0;
		UINT32 ReadBytes = 0;

		if (SizeBytes == 0 || SizeBytes > 8 || OffsetBytes > 8) {
			Status = STATUS_INVALID_PARAMETER;
			goto cleanup;
		}

		++PRun;

		if (PRun + SizeBytes > PRunEnd || PRun + SizeBytes + OffsetBytes > PRunEnd) {
			Status = STATUS_INVALID_PARAMETER;
			goto cleanup;
		}

		for (UINT32 i = 0; i < SizeBytes; i++) {
			Length |= (UINT64)(*PRun) << (i * 8);
			PRun++;
		}

		for (UINT32 i = 0; i < OffsetBytes; i++) {
			LcnOffset |= (UINT64)(*PRun) << (i * 8);
			PRun++;
		}

		if (OffsetBytes == 0)
		{
			Status = STATUS_UNSUCCESSFUL;
			goto cleanup;
		}

		if (OffsetBytes > 0 && (LcnOffset >> (OffsetBytes * 8 - 1)) & 1) {
			for (UINT32 i = OffsetBytes; i < 8; i++)
				LcnOffset |= ((UINT64)0xFF) << (i * 8);
		}

		CurrentBaseLCN += LcnOffset;

		ReadBytes = (UINT32)QH_LCN_TO_BYTE_OFFSET(BootSector, Length);

		if (ReadLCNByteSize + ReadBytes > *BitmapSize) {
			ReadBytes = (UINT32)(*BitmapSize - ReadLCNByteSize);
			ReadBytes = (UINT32)(((UINT64)ReadBytes + BootSector->BPB.BytesPerSector - 1) &
				~((UINT64)BootSector->BPB.BytesPerSector - 1));
		}

		if (ReadBytes == 0)
			break;

		Status = QHReadVolumeData(
			FilterDeviceObject,
			QH_LCN_TO_BYTE_OFFSET(BootSector, CurrentBaseLCN),
			ReadBytes,
			PBitmap + ReadLCNByteSize
		);
		if (!NT_SUCCESS(Status))
		{
			goto cleanup;
		}

		ReadLCNByteSize += ReadBytes;
	}

cleanup:
	if (!NT_SUCCESS(Status))
	{
		if (PBitmap)
		{
			qhfree(PBitmap);
			PBitmap = NULL;
		}
	}

	if (BitmapRecord)
	{
		qhfree(BitmapRecord);
	}

	*BitmapData = PBitmap;

	return Status;
}

// ============================================================================
// 扇区级位图构建
// ============================================================================

// 辅助：检查簇级位图中指定位是否为 1
static BOOLEAN QHIsClusterBitmapBitSet(
	_In_ PVOID ClusterBitmapData,
	_In_ UINT64 ClusterIndex
)
{
	PUINT8 p = (PUINT8)ClusterBitmapData;
	UINT64 byteIndex = ClusterIndex / 8;
	UINT8 bitPos = (UINT8)(ClusterIndex % 8);
	return (p[byteIndex] >> bitPos) & 1 ? TRUE : FALSE;
}

NTSTATUS QHBuildSectorBitmap(
	_In_ PQH_NTFS_BOOT_SECTOR BootSector,
	_In_ PVOID ClusterBitmapData,
	_In_ UINT64 ClusterBitmapSize,
	_Out_ PQH_BITMAP* SectorBitmap
)
{
	NTSTATUS Status;
	UINT32 sectorsPerCluster = BootSector->BPB.SectorsPerCluster;
	ULONGLONG totalSectors = BootSector->ExtendedBPB.TotalSectors;
	ULONGLONG totalClusters = totalSectors / sectorsPerCluster;
	ULONGLONG clusterIdx;
	PQH_BITMAP pSectorBitmap = NULL;

	// clusterBitmapSize 的位数即为 NTFS 报告的簇数，取其与 totalClusters 的较小值
	ULONGLONG bitmapClusterCount = ClusterBitmapSize * 8;
	if (bitmapClusterCount > totalClusters)
		bitmapClusterCount = totalClusters;

	// 创建扇区级位图
	Status = QHBitmapCreate(SectorBitmap, totalSectors);
	if (!NT_SUCCESS(Status))
	{
		return Status;
	}
	pSectorBitmap = *SectorBitmap;

	// 将簇级位图扩展到扇区级
	for (clusterIdx = 0; clusterIdx < bitmapClusterCount; clusterIdx++)
	{
		if (QHIsClusterBitmapBitSet(ClusterBitmapData, clusterIdx))
		{
			// 此簇已占用，标记其所有扇区为占用
			ULONGLONG baseSector = clusterIdx * sectorsPerCluster;
			for (UINT32 j = 0; j < sectorsPerCluster; j++)
			{
				QHBitmapSet(pSectorBitmap, baseSector + j, TRUE);
			}
		}
	}

	// 簇空间之外的尾部扇区（如果有）标记为占用
	// 这些扇区不属于任何簇，不应被分配为重定向目标
	{
		ULONGLONG leadingSectors = totalClusters * sectorsPerCluster;
		for (ULONGLONG s = leadingSectors; s < totalSectors; s++)
		{
			QHBitmapSet(pSectorBitmap, s, TRUE);
		}
	}

	return STATUS_SUCCESS;
}

// ============================================================================
// ProtectRanges 填充：将必须直通磁盘的扇区记录到 DevExt->ProtectRanges
// 使用 ZwCreateFile + FSCTL_GET_RETRIEVAL_POINTERS 获取文件簇映射
//
// 仅收录两类静态扇区:
//   * _qh_protect_state.data — 文件大小固定(1MB), 保护期间扇区位置不变
//   * $Volume MFT 记录 #3 主/镜 — NTFS 元数据, 物理位置固定
// 不收录 pagefile.sys/hiberfil.sys/bootstat.dat — 用户态可能动态扩容,
// 新扇区不在表中会被误重定向 → 系统读取错位数据 → 蓝屏
// ============================================================================

// 辅助：获取卷的 NT 路径名（如 "\??\C:"），用于 ZwCreateFile 打开文件
//
// IoVolumeDeviceToDosName 返回的是 DOS 形式 "C:", 但 ZwCreateFile 必须用 NT
// 原生路径 "\??\C:\..." 才能被 Object Manager 解析, 否则 STATUS_OBJECT_PATH_NOT_FOUND
//
// 历史 bug: 旧实现忘了加 "\??\" 前缀, 导致 QHCollectFileProtectRanges 对所有系统
// 文件的打开请求全部失败但返回值未被检查, ProtectRanges 实际为空, 这也是原代码
// "放行 bootstat.dat 等仍进诊断模式" 的真正原因
static NTSTATUS QHGetVolumeDosName(
	_In_ PDEVICE_OBJECT PhysicalDeviceObject,
	_Out_ PUNICODE_STRING DosName)
{
	UNICODE_STRING volumeMountPoint = { 0 };
	NTSTATUS Status;
	DECLARE_CONST_UNICODE_STRING(NtPrefix, L"\\??\\");

	Status = IoVolumeDeviceToDosName(PhysicalDeviceObject, &volumeMountPoint);
	if (!NT_SUCCESS(Status))
	{
		return Status;
	}

	DosName->Length = 0;
	DosName->MaximumLength = NtPrefix.Length + volumeMountPoint.Length + 8 * sizeof(WCHAR);
	DosName->Buffer = (PWCH)qhalloc(DosName->MaximumLength);
	if (!DosName->Buffer)
	{
		ExFreePool(volumeMountPoint.Buffer);
		return STATUS_INSUFFICIENT_RESOURCES;
	}

	RtlAppendUnicodeStringToString(DosName, (PUNICODE_STRING)&NtPrefix);
	RtlAppendUnicodeStringToString(DosName, &volumeMountPoint);
	ExFreePool(volumeMountPoint.Buffer);
	return STATUS_SUCCESS;
}

// 辅助：将指定文件的所有扇区追加到 DevExt->ProtectRanges
static NTSTATUS QHCollectFileProtectRanges(
	_In_ PDEVICE_OBJECT FilterDeviceObject,
	_In_ PQH_DEVICE_EXTENSION DevExt,
	_In_ PCWSTR FilePath)  // 如 L"\\_qh_protect_state.data"
{
	UNREFERENCED_PARAMETER(FilterDeviceObject);

	NTSTATUS Status;
	HANDLE fileHandle = NULL;
	UNICODE_STRING fullpath = { 0 };
	OBJECT_ATTRIBUTES oa;
	IO_STATUS_BLOCK iosb;
	ULONG sectorsPerCluster = DevExt->NtfsBootSector.BPB.SectorsPerCluster;
	UNICODE_STRING filePathStr;
	RtlInitUnicodeString(&filePathStr, FilePath);

	// 构造完整路径 "\??\C:\_qh_protect_state.data"
	{
		UNICODE_STRING dosName = { 0 };
		Status = QHGetVolumeDosName(DevExt->PhysicalDeviceObject, &dosName);
		if (!NT_SUCCESS(Status))
		{
			return Status;
		}

		fullpath.MaximumLength = dosName.Length + filePathStr.Length + sizeof(WCHAR);
		fullpath.Buffer = (PWCH)qhalloc(fullpath.MaximumLength);
		if (!fullpath.Buffer)
		{
			ExFreePool(dosName.Buffer);
			return STATUS_INSUFFICIENT_RESOURCES;
		}
		RtlCopyUnicodeString(&fullpath, &dosName);
		RtlAppendUnicodeStringToString(&fullpath, &filePathStr);
		ExFreePool(dosName.Buffer);
	}

	// 打开文件
	InitializeObjectAttributes(&oa, &fullpath, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
	Status = ZwCreateFile(&fileHandle,
		GENERIC_READ | SYNCHRONIZE,
		&oa, &iosb, NULL,
		FILE_ATTRIBUTE_NORMAL,
		FILE_SHARE_READ | FILE_SHARE_WRITE,
		FILE_OPEN,
		FILE_SYNCHRONOUS_IO_NONALERT,
		NULL, 0);
	if (!NT_SUCCESS(Status))
	{
		qhfree(fullpath.Buffer);
		return Status;
	}
	qhfree(fullpath.Buffer);

	// 获取文件簇映射
	{
		STARTING_VCN_INPUT_BUFFER startingVcn = { 0 };
		startingVcn.StartingVcn.QuadPart = 0;
		ULONG outputSize = sizeof(RETRIEVAL_POINTERS_BUFFER) + 1024 * sizeof(LARGE_INTEGER);
		PRETRIEVAL_POINTERS_BUFFER pVcnPairs = NULL;

		do {
			if (pVcnPairs) qhfree(pVcnPairs);
			pVcnPairs = (PRETRIEVAL_POINTERS_BUFFER)qhalloc(outputSize);
			if (!pVcnPairs)
			{
				ZwClose(fileHandle);
				return STATUS_INSUFFICIENT_RESOURCES;
			}

			Status = ZwFsControlFile(fileHandle, NULL, NULL, NULL, &iosb,
				FSCTL_GET_RETRIEVAL_POINTERS,
				&startingVcn, sizeof(startingVcn),
				pVcnPairs, outputSize);

			outputSize += 1024 * sizeof(LARGE_INTEGER);
		} while (Status == STATUS_BUFFER_OVERFLOW);

		if (!NT_SUCCESS(Status))
		{
			qhfree(pVcnPairs);
			ZwClose(fileHandle);
			return Status;
		}

		// 遍历所有 extent, 每段以 {baseSector, sectorCount} 形式追加到 ProtectRanges
		// init 阶段无并发, 不加锁
		LARGE_INTEGER prevVcn = pVcnPairs->StartingVcn;
		for (ULONG r = 0; r < pVcnPairs->ExtentCount; r++)
		{
			LARGE_INTEGER lcn = pVcnPairs->Extents[r].Lcn;
			ULONGLONG clusterCount = pVcnPairs->Extents[r].NextVcn.QuadPart - prevVcn.QuadPart;
			prevVcn = pVcnPairs->Extents[r].NextVcn;

			if (lcn.QuadPart == -1) // 稀疏/未分配的簇, 跳过
				continue;

			ULONGLONG baseSector = (ULONGLONG)lcn.QuadPart * sectorsPerCluster;
			ULONGLONG sectorCount = clusterCount * sectorsPerCluster;
			QHAddProtectRange(DevExt, baseSector, sectorCount);
		}

		qhfree(pVcnPairs);
	}

	ZwClose(fileHandle);
	return STATUS_SUCCESS;
}

// 辅助：将 MFT 中指定记录覆盖的所有扇区追加到 DevExt->ProtectRanges
// UseMirror = FALSE 取 $MFT 主区 (ExtendedBPB.MFT_LCN)
// UseMirror = TRUE  取 $MFTMirr 镜像区 (ExtendedBPB.MFTMirr_LCN)
// 不打开任何文件, 纯算术, 可在 PASSIVE_LEVEL 调用
static VOID _QHCollectMftRecordProtectRange(
	_In_ PQH_DEVICE_EXTENSION DevExt,
	_In_ UINT32 RecordIndex,
	_In_ BOOLEAN UseMirror)
{
	PQH_NTFS_BOOT_SECTOR BootSector = &DevExt->NtfsBootSector;
	UINT32 BytesPerSector = BootSector->BPB.BytesPerSector;
	UINT32 RecordSize = QH_MFT_NEXT_BYTE_OFFSET(BootSector);
	UINT64 MftBaseLCN = UseMirror ? BootSector->ExtendedBPB.MFTMirr_LCN
		: BootSector->ExtendedBPB.MFT_LCN;

	UINT64 MftStartByte = MftBaseLCN * QH_CLUSTER_BYTES(BootSector);
	UINT64 RecordStartByte = MftStartByte + (UINT64)RecordIndex * RecordSize;
	UINT64 StartSector = RecordStartByte / BytesPerSector;
	UINT32 SectorsForRecord = RecordSize / BytesPerSector;
	if (SectorsForRecord == 0)
		SectorsForRecord = 1;

	QHAddProtectRange(DevExt, StartSector, SectorsForRecord);
}

// 填充 ProtectRanges: 仅收录扇区位置静态不变的实体
VOID QHPopulateProtectRanges(
	_In_ PDEVICE_OBJECT FilterDeviceObject,
	_In_ PQH_DEVICE_EXTENSION DevExt)
{
	DevExt->ProtectRangeCount = 0;

	// 保护状态文件 _qh_protect_state.data
	// UI 关闭保护时对它的写入必须直通真实磁盘, 否则重启后状态丢失;
	// 文件大小固定 1MB (强制非驻留), 保护期间扇区位置不会变化
	QHCollectFileProtectRanges(FilterDeviceObject, DevExt,
		QH_PROTECT_STATE_FILE_NAME);

	// $Volume (MFT #3): 维护 NTFS dirty flag
	// 此记录的写入若被重定向, 真实磁盘 dirty bit 永远停留在保护开启时的状态,
	// 关机时的"清 dirty"无法落盘, 多次重启后 Windows 累计判定未干净关机进入 WinRE
	// 同时放行 $MFTMirr 的对应镜像, 避免 MFT 与 MFTMirr 校验不一致
	_QHCollectMftRecordProtectRange(DevExt, 3, FALSE);
	_QHCollectMftRecordProtectRange(DevExt, 3, TRUE);
}

// ============================================================================
// ProtectRanges 操作 API (init 阶段写, IRP 阶段读)
// ============================================================================

VOID QHAddProtectRange(
	_Inout_ PQH_DEVICE_EXTENSION DevExt,
	_In_ ULONGLONG StartSector,
	_In_ ULONGLONG SectorCount)
{
	if (SectorCount == 0)
		return;

	if (DevExt->ProtectRangeCount >= QH_PROTECT_RANGE_MAX)
	{
		KdPrint(("QHAddProtectRange: ProtectRanges 已满 (%u), 丢弃 [%llu, +%llu)\n",
			QH_PROTECT_RANGE_MAX, StartSector, SectorCount));
		return;
	}

	DevExt->ProtectRanges[DevExt->ProtectRangeCount].StartSector = StartSector;
	DevExt->ProtectRanges[DevExt->ProtectRangeCount].SectorCount = SectorCount;
	DevExt->ProtectRangeCount++;
}

BOOLEAN QHIsSectorProtected(
	_In_ PQH_DEVICE_EXTENSION DevExt,
	_In_ ULONGLONG SectorIndex)
{
	ULONG count = DevExt->ProtectRangeCount;
	for (ULONG i = 0; i < count; i++)
	{
		PQH_PROTECT_RANGE r = &DevExt->ProtectRanges[i];
		if (SectorIndex >= r->StartSector &&
			SectorIndex <  r->StartSector + r->SectorCount)
		{
			return TRUE;
		}
	}
	return FALSE;
}

BOOLEAN QHAreSectorsProtected(
	_In_ PQH_DEVICE_EXTENSION DevExt,
	_In_ ULONGLONG StartSector,
	_In_ ULONGLONG Count)
{
	if (Count == 0)
		return TRUE;

	// 整段必须落在同一个 range 内: 因为本表只用作 fast-path 短路,
	// 写 IRP 通常不会跨越多个 ProtectRange (它们物理上彼此远离),
	// 不需要处理跨 range 拼接的复杂情况
	ULONG count = DevExt->ProtectRangeCount;
	for (ULONG i = 0; i < count; i++)
	{
		PQH_PROTECT_RANGE r = &DevExt->ProtectRanges[i];
		if (StartSector >= r->StartSector &&
			StartSector + Count <= r->StartSector + r->SectorCount)
		{
			return TRUE;
		}
	}
	return FALSE;
}

// ============================================================================
// 保护状态文件读取
// 在 PASSIVE_LEVEL 调用, 用于初始化时判断该卷是否需要激活保护
// ============================================================================

INT32 QHReadProtectStateFromFile(
	_In_ PDEVICE_OBJECT PhysicalDeviceObject)
{
	NTSTATUS Status;
	UNICODE_STRING dosName = { 0 };
	UNICODE_STRING fullpath = { 0 };
	UNICODE_STRING fileNameStr;
	OBJECT_ATTRIBUTES oa;
	IO_STATUS_BLOCK iosb;
	HANDLE fileHandle = NULL;
	UCHAR firstByte = 0;
	INT32 result = -1;

	RtlInitUnicodeString(&fileNameStr, QH_PROTECT_STATE_FILE_NAME);

	// 构造完整路径 (与 QHCollectFileProtectRanges 同款做法)
	Status = QHGetVolumeDosName(PhysicalDeviceObject, &dosName);
	if (!NT_SUCCESS(Status))
	{
		return -1;
	}

	fullpath.Length = 0;
	fullpath.MaximumLength = dosName.Length + fileNameStr.Length + sizeof(WCHAR);
	fullpath.Buffer = (PWCH)qhalloc(fullpath.MaximumLength);
	if (!fullpath.Buffer)
	{
		qhfree(dosName.Buffer);
		return -1;
	}
	RtlAppendUnicodeStringToString(&fullpath, &dosName);
	RtlAppendUnicodeStringToString(&fullpath, &fileNameStr);
	qhfree(dosName.Buffer);

	// 打开文件 (只读)
	InitializeObjectAttributes(&oa, &fullpath,
		OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE,
		NULL, NULL);

	Status = ZwCreateFile(&fileHandle,
		GENERIC_READ | SYNCHRONIZE,
		&oa, &iosb, NULL,
		FILE_ATTRIBUTE_NORMAL,
		FILE_SHARE_READ | FILE_SHARE_WRITE,
		FILE_OPEN,
		FILE_SYNCHRONOUS_IO_NONALERT,
		NULL, 0);

	qhfree(fullpath.Buffer);

	if (!NT_SUCCESS(Status))
	{
		return -1;
	}

	// 读首字节
	LARGE_INTEGER ByteOffset = { 0 };
	Status = ZwReadFile(fileHandle, NULL, NULL, NULL, &iosb,
		&firstByte, sizeof(firstByte), &ByteOffset, NULL);
	ZwClose(fileHandle);

	if (!NT_SUCCESS(Status) || iosb.Information < 1)
	{
		return -1;
	}

	result = (firstByte != 0) ? 1 : 0;
	return result;
}
