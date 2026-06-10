#include "QHIrpDispatchs.h"
#include "QHNtfs.h"
#include "QHBitmap.h"
#include <ntddvol.h>

// 构建扇区级位图并填充 ProtectRanges
// 由 QHInitializeVolumeProtection 在确认要激活保护后调用
//
// 命名遗留: 此函数原本设计是在 IRP 处理中动态刷新位图(担心初始化时硬盘位图与处理 IRP 时不一致)
// 实际项目中并未启用动态刷新, 函数名保留 "Safe" 字样仅为历史遗留
static NTSTATUS _QHUpdateSectorBitmapSafe(PDEVICE_OBJECT DeviceObject)
{
	NTSTATUS Status;
	PQH_DEVICE_EXTENSION DevExt = DeviceObject->DeviceExtension;
	PVOID clusterBitmapData = NULL;
	UINT64 clusterBitmapSize = 0;

	// 读取 NTFS 簇级位图
	Status = QHGetBitmapData(DeviceObject, &DevExt->NtfsBootSector,
		&clusterBitmapData, &clusterBitmapSize);
	if (!NT_SUCCESS(Status))
		goto cleanup;

	// 从簇级位图构建扇区级位图
	// windows大概是1个簇=8个扇区, 1个扇区为512字节
	// 当然, 不能写死, 这些数据在引导里可以读取的到
	Status = QHBuildSectorBitmap(
		&DevExt->NtfsBootSector,
		clusterBitmapData,
		clusterBitmapSize,
		&DevExt->SectorBitmap);
	if (!NT_SUCCESS(Status))
		goto cleanup;

	DevExt->LastScanIndex = 0;

	// 填充 ProtectRanges: 记录直写放行扇区 (_qh_protect_state.data + $Volume MFT#3)
	// 这些扇区写入时直通真实磁盘, 不被本驱动重定向
	QHPopulateProtectRanges(DeviceObject, DevExt);

cleanup:
	// 释放临时簇级位图数据
	if (clusterBitmapData)
	{
		qhfree(clusterBitmapData);
	}

	if (!NT_SUCCESS(Status))
	{
		// 失败回滚: QHBitmapFree 内部已防 NULL
		QHBitmapFree(DevExt->SectorBitmap);
		DevExt->SectorBitmap = NULL;
	}
	return Status;
}

// ============================================================================
// IRP 分发辅助函数
// ============================================================================

static NTSTATUS QHSendToNextDevice(_In_ PDEVICE_OBJECT DeviceObject, _Inout_ PIRP Irp)
{
	IoSkipCurrentIrpStackLocation(Irp);
	return IoCallDriver(DeviceObject, Irp);
}

NTSTATUS QHIrpDispatchDefault(_In_ PDEVICE_OBJECT DeviceObject, _Inout_ PIRP Irp)
{
	PQH_DEVICE_EXTENSION DeviceExtension = (PQH_DEVICE_EXTENSION)DeviceObject->DeviceExtension;

	if (DeviceExtension)
	{
		return QHSendToNextDevice(DeviceExtension->LowerDeviceObject, Irp);
	}

	return QHCompleteIrp(Irp, STATUS_SUCCESS, 0);
}

NTSTATUS QHIrpDispatchCreateCloseCleanup(_In_ PDEVICE_OBJECT DeviceObject, _Inout_ PIRP Irp)
{
	NTSTATUS Status = STATUS_SUCCESS;
	PQH_DEVICE_EXTENSION DeviceExtension = (PQH_DEVICE_EXTENSION)DeviceObject->DeviceExtension;

	if (DeviceExtension)
	{
		Status = QHSendToNextDevice(DeviceExtension->LowerDeviceObject, Irp);
	}
	else
	{
		Status = QHCompleteIrp(Irp, Status, 0);
	}

	return Status;
}

// ============================================================================
// 工作项（处理高 IRQL 排队的读写 IRP）
// ============================================================================

VOID QHIoWorkItemRoutine(_In_ PDEVICE_OBJECT DeviceObject, _In_opt_ PVOID Context)
{
	UNREFERENCED_PARAMETER(Context);
	PQH_DEVICE_EXTENSION DevExt = (PQH_DEVICE_EXTENSION)DeviceObject->DeviceExtension;
	KIRQL OldIrql;
	PLIST_ENTRY Entry;
	PIRP Irp;

	while (TRUE)
	{
		KeAcquireSpinLock(&DevExt->PendingIrpQueueLock, &OldIrql);
		if (IsListEmpty(&DevExt->PendingIrpQueue))
		{
			InterlockedExchange(&DevExt->IoWorkItemQueued, 0);
			KeReleaseSpinLock(&DevExt->PendingIrpQueueLock, OldIrql);
			break;
		}
		// 从队列中挨个取出IRP处理
		Entry = RemoveHeadList(&DevExt->PendingIrpQueue);
		KeReleaseSpinLock(&DevExt->PendingIrpQueueLock, OldIrql);

		Irp = CONTAINING_RECORD(Entry, IRP, Tail.Overlay.ListEntry);
		PIO_STACK_LOCATION Stack = IoGetCurrentIrpStackLocation(Irp);

		if (!DevExt->Initialized)
		{
			QHCompleteIrp(Irp, STATUS_DEVICE_NOT_READY, 0);
			continue;
		}

		if (Stack->MajorFunction == IRP_MJ_READ)
			QHIrpDispatchRead(DeviceObject, Irp);
		else if (Stack->MajorFunction == IRP_MJ_WRITE)
			QHIrpDispatchWrite(DeviceObject, Irp);
		else
		{
			QHCompleteIrp(Irp, STATUS_INVALID_DEVICE_REQUEST, 0);
		}
	}

	KeSetEvent(&DevExt->WorkItemFinished, 0, FALSE);
}

// 插入IRP处理队列函数
static NTSTATUS QHQueueIrpForWorkItem(_In_ PQH_DEVICE_EXTENSION DevExt, _In_ PIRP Irp)
{
	IoMarkIrpPending(Irp);
	KIRQL OldIrql;
	KeAcquireSpinLock(&DevExt->PendingIrpQueueLock, &OldIrql);
	BOOLEAN NeedSchedule = IsListEmpty(&DevExt->PendingIrpQueue);
	InsertTailList(&DevExt->PendingIrpQueue, &Irp->Tail.Overlay.ListEntry);
	KeReleaseSpinLock(&DevExt->PendingIrpQueueLock, OldIrql);

	if (NeedSchedule && InterlockedCompareExchange(&DevExt->IoWorkItemQueued, 1, 0) == 0)
	{
		KeClearEvent(&DevExt->WorkItemFinished);
		IoQueueWorkItem(DevExt->IoWorkItem, QHIoWorkItemRoutine, CriticalWorkQueue, NULL);
	}
	return STATUS_PENDING;
}

// ============================================================================
// 扇区级读分发
// 逐扇区计算物理偏移，连续扇区合并为单次 I/O
// 修改：使用独立缓冲区操作，避免 PFN_LIST_CORRUPT
// ============================================================================

// 映射 IRP 的 MDL 缓冲区，失败时完成 IRP 并返回错误状态
static PUCHAR QHMapIrpBuffer(_In_ PIRP Irp, _Out_ PNTSTATUS pStatus)
{
	PUCHAR Buffer = NULL;
	// 必定是MDL, 因为过滤设备有DO_DIRECT_IO属性
	if (Irp->MdlAddress)
	{
		Buffer = (PUCHAR)MmGetSystemAddressForMdlSafe(Irp->MdlAddress, NormalPagePriority);
		if (!Buffer)
		{
			*pStatus = QHCompleteIrp(Irp, STATUS_INSUFFICIENT_RESOURCES, 0);
			return NULL;
		}
	}
	else
	{
		*pStatus = QHCompleteIrp(Irp, STATUS_INVALID_PARAMETER, 0);
		return NULL;
	}
	*pStatus = STATUS_SUCCESS;
	return Buffer;
}

// 读IRP操作相对简单
// 1. 先检查 ProtectRanges, 判断是否是直写放行扇区, 如果是则跳过
// 2. 后查 OffsetHash, 如果整段都无重定向 (即都是空扇区或保护扇区) 则直接透传
// 3. 进入while循环 循环查找重定向. 因为重定向记录是一个扇区对应一个偏移记录 所以得循环
//		比如说, 用户向编号为1000的扇区写了1024个字节, 那么这个while循环就会循环两遍
// 4. 检查重定向表, 如果无重定向记录则跳过
//		至于为什么无重定向记录跳过, 因为$Bitmap文件也被重定向了, 如果向空扇区中写数据,没必要重定向
//		重启后$Bitmap会被复原, 自然没什么问题
//		但是如果是有数据的扇区则不同, 必须重定向, 因为不能修改原始数据, 如果被修改了就爆炸了
//		所以先查 OffsetHash, 没有重定向记录(意味着该扇区写入时是空扇区) 则按原始位置读取
//
NTSTATUS QHIrpDispatchRead(_In_ PDEVICE_OBJECT DeviceObject, _Inout_ PIRP Irp)
{
	NTSTATUS Status;
	PIO_STACK_LOCATION Stack = IoGetCurrentIrpStackLocation(Irp);
	PQH_DEVICE_EXTENSION DevExt = (PQH_DEVICE_EXTENSION)DeviceObject->DeviceExtension;

	if (!DevExt->Initialized)
		return QHIrpDispatchDefault(DeviceObject, Irp);

	UINT64 Offset = Stack->Parameters.Read.ByteOffset.QuadPart;
	UINT32 Length = Stack->Parameters.Read.Length;
	UINT32 BytesPerSector = DevExt->NtfsBootSector.BPB.BytesPerSector;

	if (Length == 0)
	{
		return QHCompleteIrp(Irp, STATUS_SUCCESS, 0);
	}

	// 映射缓冲区
	NTSTATUS mapStatus;
	PUCHAR Buffer = QHMapIrpBuffer(Irp, &mapStatus);
	if (!Buffer)
		return mapStatus;

	// FAST_MUTEX 要求 PASSIVE_LEVEL
	if (KeGetCurrentIrql() != PASSIVE_LEVEL)
		return QHQueueIrpForWorkItem(DevExt, Irp);


	// 快速扫描, 直接查 OffsetHash 判断是否存在任意重定向
	// ProtectRanges 中的扇区直接放行, 不需要查表 (无锁判断)
	UINT64 startSector = Offset / BytesPerSector;
	UINT64 endSector = (Offset + Length - 1) / BytesPerSector;
	BOOLEAN HasRedirect = FALSE;

	for (UINT64 s = startSector; s <= endSector; s++)
	{
		// 保护区间无锁判断, 命中即直接当作无重定向
		if (QHIsSectorProtected(DevExt, s))
			continue;

		UINT64 dummyTarget;
		ExAcquireFastMutex(&DevExt->OffsetHashMutex);
		NTSTATUS hashStatus = QHHashGet(DevExt->OffsetHash, s, &dummyTarget);
		ExReleaseFastMutex(&DevExt->OffsetHashMutex);
		if (NT_SUCCESS(hashStatus))
		{
			HasRedirect = TRUE;
			break;
		}
	}

	// 若无重定向，直接下发原始 IRP
	if (!HasRedirect)
	{
		IoSkipCurrentIrpStackLocation(Irp);
		return IoCallDriver(DevExt->LowerDeviceObject, Irp);
	}

	// 有重定向，逐扇区处理并合并连续 I/O
	// 使用独立缓冲区操作，避免原始 MDL 与自建 IRP 的 MDL 同时锁定同一物理页
	// 按照原理来讲 直接使用MDL地址应该也没什么问题 这里只是上一道保险, 后期为了性能可能删除
	PUCHAR newBuf = (PUCHAR)qhalloc(Length);
	if (!newBuf)
	{
		return QHCompleteIrp(Irp, STATUS_INSUFFICIENT_RESOURCES, 0);
	}

	ULONGLONG  currOffset = Offset;
	ULONG      remain = Length;
	PUCHAR     currBuf = newBuf;  // ★ 操作独立缓冲区

	// I/O 合并状态
	BOOLEAN     isFirstBlock = TRUE;
	ULONGLONG   prevPhysicalOffset = 0;
	PUCHAR      prevBuffer = NULL;
	ULONG       mergedLength = 0;

	Status = STATUS_SUCCESS;

	while (remain > 0)
	{
		ULONGLONG sectorIndex = currOffset / BytesPerSector;

		// 首先检查 ProtectRanges: 受保护扇区直接读原始位置 (无锁)
		BOOLEAN isProtected = QHIsSectorProtected(DevExt, sectorIndex);

		ULONGLONG physicalOffset;
		if (isProtected)
		{
			physicalOffset = currOffset;
		}
		else
		{
			// 查询重定向 (查哈希表获取目标扇区, 未命中则按原位置读)
			UINT64 targetSector;
			ExAcquireFastMutex(&DevExt->OffsetHashMutex);
			NTSTATUS hashStatus = QHHashGet(DevExt->OffsetHash, sectorIndex, &targetSector);
			ExReleaseFastMutex(&DevExt->OffsetHashMutex);

			if (NT_SUCCESS(hashStatus))
			{
				physicalOffset = targetSector * BytesPerSector;
			}
			else
			{
				physicalOffset = currOffset;
			}
		}
	__readReInit:
		if (isFirstBlock)
		{
			prevPhysicalOffset = physicalOffset;
			prevBuffer = currBuf;
			mergedLength = BytesPerSector;
			isFirstBlock = FALSE;
			goto __readNext;
		}

		if (physicalOffset == prevPhysicalOffset + mergedLength)
		{
			mergedLength += BytesPerSector;
		}
		else
		{
			Status = QHReadVolumeData(DeviceObject, (LONGLONG)prevPhysicalOffset, mergedLength, prevBuffer);
			if (!NT_SUCCESS(Status))
			{
				qhfree(newBuf);
				return QHCompleteIrp(Irp, Status, 0);
			}
			isFirstBlock = TRUE;
			goto __readReInit;
		}

	__readNext:
		if (BytesPerSector >= remain)
		{
			Status = QHReadVolumeData(DeviceObject, (LONGLONG)prevPhysicalOffset, mergedLength, prevBuffer);
			break;
		}

		currOffset += BytesPerSector;
		currBuf += BytesPerSector;
		remain -= BytesPerSector;
	}

	// 读操作：从独立缓冲区拷贝回原始 Buffer
	if (NT_SUCCESS(Status))
	{
		RtlCopyMemory(Buffer, newBuf, Length);
	}
	qhfree(newBuf);

	if (!NT_SUCCESS(Status))
		return QHCompleteIrp(Irp, Status, 0);

	return QHCompleteIrp(Irp, STATUS_SUCCESS, Length);
}

// 扇区级写分发
// 无 COW：扇区对齐写入，直接写入重定向目标
// 含独立缓冲区、引导扇区写保护
// 实话说, 原本我想写COW(写时复制)来着, 你们看偏移记录表现在是扇区对应扇区偏移
// 但是一开始可是簇对应簇偏移, 如果簇对应簇, 则会造成一种情况, 我说一下你就明白了
// 比如说一个'-'代表一个空扇区, 一个'*'代表一个被使用的扇区 例子如下
// --*-**--
// 如果要重定向, 我需要对应整个簇进行重定向(8个扇区), 我势必要拷贝原始簇数据到偏移簇中, 然后将更改写入偏移簇中
// 这样会多一次IO, 别小看, 这一次IO性能影响可大了去了, 那就相当于性能直接砍了一半!这简直就是灾难性的性能事故
NTSTATUS QHIrpDispatchWrite(_In_ PDEVICE_OBJECT DeviceObject, _Inout_ PIRP Irp)
{
	NTSTATUS Status;
	PIO_STACK_LOCATION Stack = IoGetCurrentIrpStackLocation(Irp);
	PQH_DEVICE_EXTENSION DevExt = (PQH_DEVICE_EXTENSION)DeviceObject->DeviceExtension;

	if (!DevExt->Initialized)
		return QHIrpDispatchDefault(DeviceObject, Irp);

	// 写操作必须在 PASSIVE_LEVEL
	if (KeGetCurrentIrql() != PASSIVE_LEVEL)
		return QHQueueIrpForWorkItem(DevExt, Irp);

	UINT64 Offset = Stack->Parameters.Write.ByteOffset.QuadPart;
	UINT32 Length = Stack->Parameters.Write.Length;
	UINT32 BytesPerSector = DevExt->NtfsBootSector.BPB.BytesPerSector;

	if (Length == 0)
	{
		return QHCompleteIrp(Irp, STATUS_SUCCESS, 0);
	}

	// 引导扇区写保护：拒绝写入卷的第一个扇区（偏移 0 ~ BytesPerSector-1）
	if (Offset < BytesPerSector)
	{
		return QHCompleteIrp(Irp, STATUS_ACCESS_DENIED, 0);
	}

	// 范围短路 — 在缓冲区分配/拷贝之前先尝试整段直通, 避免无谓的 qhalloc + memcpy
	//
	// 两种短路条件 (任一成立都可走原生透传):
	//   1. 整段都在 ProtectRanges 内 -> 直接写原位置 (等同慢路径每扇区 IsProtected==TRUE)
	//   2. 整段在 SectorBitmap 内全 0 (空闲) -> 标 SectorBitmap 后直写原位置
	//      (等同慢路径每扇区 IsFreeSector==TRUE 的逐位处理结果)
	//
	// 关键: "判空 + 占用" 必须在同一把锁内完成, 否则两个并发 IRP 都判定为空,
	// 都直写到同一物理位置, 造成数据相互覆盖
	{
		UINT64 wStartSector = Offset / BytesPerSector;
		UINT64 wEndSector = (Offset + Length - 1) / BytesPerSector;
		UINT64 wSectorCount = wEndSector - wStartSector + 1;

		// 全段保护无锁判断 (ProtectRanges 在 init 后只读)
		BOOLEAN allProtected = QHAreSectorsProtected(DevExt, wStartSector, wSectorCount);

		BOOLEAN allFree = FALSE;
		if (!allProtected)
		{
			ExAcquireFastMutex(&DevExt->BitmapMutex);
			allFree = QHBitmapAreBitsClear(DevExt->SectorBitmap, wStartSector, wSectorCount);
			if (allFree)
			{
				// 原子地把整段标记为已用, 防止后续并发写再次判空走直通
				// (语义与慢路径单扇区 "判空 -> 立即 QHBitmapSet" 一致)
				QHBitmapSetRange(DevExt->SectorBitmap, wStartSector, wSectorCount);
			}
			ExReleaseFastMutex(&DevExt->BitmapMutex);
		}

		if (allProtected || allFree)
		{
			IoSkipCurrentIrpStackLocation(Irp);
			return IoCallDriver(DevExt->LowerDeviceObject, Irp);
		}
	}

	// 映射缓冲区
	NTSTATUS mapStatus;
	PUCHAR Buffer = QHMapIrpBuffer(Irp, &mapStatus);
	if (!Buffer)
		return mapStatus;

	// 分配独立缓冲区，拷贝用户数据后操作，避免 PFN_LIST_CORRUPT
	// 其实我认为没什么用, 为了保险
	PUCHAR newBuf = (PUCHAR)qhalloc(Length);
	if (!newBuf)
	{
		return QHCompleteIrp(Irp, STATUS_INSUFFICIENT_RESOURCES, 0);
	}
	RtlCopyMemory(newBuf, Buffer, Length);

	// 逐扇区处理
	{
		ULONGLONG  currOffset = Offset;
		ULONG      remain = Length;
		PUCHAR     currBuf = newBuf;  // 操作独立缓冲区

		// I/O 合并状态
		BOOLEAN     isFirstBlock = TRUE;
		ULONGLONG   prevPhysicalOffset = 0;
		PUCHAR      prevBuffer = NULL;
		ULONG       mergedLength = 0;

		Status = STATUS_SUCCESS;

		while (remain > 0)
		{
			ULONGLONG sectorIndex = currOffset / BytesPerSector;
			ULONGLONG physicalOffset;

			// 首先检查 ProtectRanges: 受保护扇区直接写原始位置, 不拦截不重定向 (无锁)
			BOOLEAN IsProtected = QHIsSectorProtected(DevExt, sectorIndex);

			if (IsProtected)
			{
				// 受保护文件扇区，直接写入原始位置
				physicalOffset = currOffset;
			}
			else
			{
				// 检查当前扇区是否为空闲
				ExAcquireFastMutex(&DevExt->BitmapMutex);
				BOOLEAN IsFreeSector = !QHBitmapTest(DevExt->SectorBitmap, sectorIndex);
				if (IsFreeSector)
				{
					QHBitmapSet(DevExt->SectorBitmap, sectorIndex, TRUE);
				}
				ExReleaseFastMutex(&DevExt->BitmapMutex);

				if (IsFreeSector)
				{
					// 空闲扇区直接写入原始位置
					physicalOffset = currOffset;
				}
				else
				{
					// 已占用扇区, 在 OffsetHashMutex 内原子完成 "查询 → 必要时分配 → 写表"
					//
					// 必须把整段保护在同一把锁内, 否则两个并发写同一扇区的 IRP 都会:
					//   1) 各自查 hash 都未命中
					//   2) 各自分一个新空闲扇区 (例如 100, 101)
					//   3) 各自写 hash, 后者覆盖前者
					// 结果: 一份数据落到 100 (永久泄露, 引用丢失), hash 表只指向 101,
					//      前者的写入永久消失
					//
					// 锁顺序约定: OffsetHashMutex 外, BitmapMutex 内
					// (全代码库无反向嵌套, 无死锁风险)
					ExAcquireFastMutex(&DevExt->OffsetHashMutex);

					UINT64 targetSector;
					NTSTATUS hashStatus = QHHashGet(DevExt->OffsetHash, sectorIndex, &targetSector);

					if (NT_SUCCESS(hashStatus))
					{
						// 已有重定向, 直接用目标扇区
						ExReleaseFastMutex(&DevExt->OffsetHashMutex);
						physicalOffset = targetSector * BytesPerSector;
					}
					else
					{
						// 首次写入已占用扇区, 分配新的空闲扇区作为重定向目标
						// QHBitmapFindNextClear 内部已带 wrap-around 语义,
						// 返回 -1 即代表整张位图已无清零位
						ExAcquireFastMutex(&DevExt->BitmapMutex);
						ULONGLONG freeSector = QHBitmapFindNextClear(DevExt->SectorBitmap, DevExt->LastScanIndex);
						if (freeSector == (ULONGLONG)-1)
						{
							ExReleaseFastMutex(&DevExt->BitmapMutex);
							ExReleaseFastMutex(&DevExt->OffsetHashMutex);
							qhfree(newBuf);
							return QHCompleteIrp(Irp, STATUS_DISK_FULL, 0);
						}
						DevExt->LastScanIndex = (ULONG)(freeSector + 1);
						QHBitmapSet(DevExt->SectorBitmap, freeSector, TRUE);
						ExReleaseFastMutex(&DevExt->BitmapMutex);

						// 仍持 OffsetHashMutex, 此处写入对其他线程的下一次 HashGet 立即可见
						QHHashSet(DevExt->OffsetHash, sectorIndex, freeSector);
						ExReleaseFastMutex(&DevExt->OffsetHashMutex);

						physicalOffset = freeSector * BytesPerSector;
					}
				}
			} // end else IsProtected

		__writeReInit:
			if (isFirstBlock)
			{
				prevPhysicalOffset = physicalOffset;
				prevBuffer = currBuf;
				mergedLength = BytesPerSector;
				isFirstBlock = FALSE;
				goto __writeNext;
			}

			// 检查是否与上一块连续
			if (physicalOffset == prevPhysicalOffset + mergedLength)
			{
				mergedLength += BytesPerSector;
			}
			else
			{
				// 不连续，先下发已合并的 I/O
				Status = QHWriteVolumeData(DeviceObject, (LONGLONG)prevPhysicalOffset, mergedLength, prevBuffer);
				if (!NT_SUCCESS(Status))
				{
					qhfree(newBuf);
					return QHCompleteIrp(Irp, Status, 0);
				}
				isFirstBlock = TRUE;
				goto __writeReInit;
			}

		__writeNext:
			if (BytesPerSector >= remain)
			{
				// 最后一块，下发
				Status = QHWriteVolumeData(DeviceObject, (LONGLONG)prevPhysicalOffset, mergedLength, prevBuffer);
				break;
			}

			currOffset += BytesPerSector;
			currBuf += BytesPerSector;
			remain -= BytesPerSector;
		}
	}

	qhfree(newBuf);

	if (!NT_SUCCESS(Status))
		return QHCompleteIrp(Irp, Status, 0);

	return QHCompleteIrp(Irp, STATUS_SUCCESS, Length);
}

// ============================================================================
// 卷保护初始化
// 新方案: 完全去掉注册表/自定义扇区, 改为读取卷根目录的保护状态文件
//   文件存在 + 首字节 = 1 -> 用户开启了保护, 激活
//   文件存在 + 首字节 = 0 -> 用户已关闭保护, 跳过
//   文件不存在            -> 此卷未配置保护, 跳过
// 文件 _qh_protect_state.data 由 UI 在开启保护时创建, 由 ProtectRanges 保护
// 其写入不被重定向, 故 UI 关闭保护时对它的写入会直通真实磁盘, 重启后生效
// ============================================================================
static NTSTATUS QHInitializeVolumeProtection(
	_In_ PDEVICE_OBJECT DeviceObject,
	_In_ PQH_DEVICE_EXTENSION DevExt)
{
	NTSTATUS Status;

	// 1. 读 boot sector
	//    此时 Initialized=FALSE, QHReadVolumeData 走默认透传, 不经过本驱动重定向
	Status = QHGetBootSector(DeviceObject, &DevExt->NtfsBootSector);
	if (!NT_SUCCESS(Status))
	{
		return STATUS_SUCCESS;
	}

	// 2. 读保护状态文件首字节
	//    同样此时 Initialized=FALSE, ZwReadFile 经下层设备直读真实磁盘
	INT32 state = QHReadProtectStateFromFile(DevExt->PhysicalDeviceObject);

	if (state != 1)
	{
		// 0  = 用户已关闭保护
		// -1 = 文件不存在/读取失败 -> 此卷未配置保护
		return STATUS_SUCCESS;
	}

	// 3. 构建位图并填充 ProtectRanges (其中包含状态文件本身)
	Status = _QHUpdateSectorBitmapSafe(DeviceObject);
	if (!NT_SUCCESS(Status))
	{
		return STATUS_SUCCESS;
	}

	// 4. 标记该设备已初始化完毕, 正式开始拦截读写
	InterlockedExchange8((volatile CHAR*)&DevExt->Initialized, 1);

	return STATUS_SUCCESS;
}

// 在所有 Boot 驱动加载完成后执行，此时 NTFS 挂载流程已完成
// 遍历所有已添加但尚未初始化的过滤设备，执行保护初始化
VOID QHBootReinitializationRoutine(
	_In_ PDRIVER_OBJECT DriverObject,
	_In_ PVOID Context,
	_In_ ULONG Count)
{
	UNREFERENCED_PARAMETER(Context);
	UNREFERENCED_PARAMETER(Count);

	PQH_DRIVER_EXTENSION DriverExt = IoGetDriverObjectExtension(DriverObject, &g_DriverObject);
	if (!DriverExt)
		return;

	// 遍历所有已添加的过滤设备
	KIRQL OldIrql;
	KeAcquireSpinLock(&DriverExt->DeviceObjectListLock, &OldIrql);
	PLIST_ENTRY Entry = DriverExt->DeviceObjectListHead.Flink;
	while (Entry != &DriverExt->DeviceObjectListHead)
	{
		PQH_DEVICE_LIST_NODE Node = CONTAINING_RECORD(Entry, QH_DEVICE_LIST_NODE, Entry);
		PQH_DEVICE_EXTENSION DevExt = (PQH_DEVICE_EXTENSION)Node->DeviceObject->DeviceExtension;

		// 只处理尚未初始化的设备
		if (!DevExt->Initialized)
		{
			// 释放锁后执行初始化（初始化可能阻塞，不能在自旋锁内执行）
			KeReleaseSpinLock(&DriverExt->DeviceObjectListLock, OldIrql);
			QHInitializeVolumeProtection(Node->DeviceObject, DevExt);

			KeAcquireSpinLock(&DriverExt->DeviceObjectListLock, &OldIrql);
			Entry = Entry->Flink;
			continue;
		}

		Entry = Entry->Flink;
	}
	KeReleaseSpinLock(&DriverExt->DeviceObjectListLock, OldIrql);

	// 初始化完毕
	InterlockedExchange(&DriverExt->BootReinitDone, 1);
}

// ============================================================================
// PnP / Power / DeviceControl（适配扇区级字段）
// ============================================================================

static NTSTATUS PnpCompletionRoutine(PDEVICE_OBJECT DeviceObject, PIRP Irp, PVOID Context)
{
	UNREFERENCED_PARAMETER(DeviceObject);
	UNREFERENCED_PARAMETER(Irp);
	KeSetEvent((PKEVENT)Context, IO_NO_INCREMENT, FALSE);
	return STATUS_MORE_PROCESSING_REQUIRED;
}

NTSTATUS QHIrpDispatchPnp(_In_ PDEVICE_OBJECT DeviceObject, _Inout_ PIRP Irp)
{
	PQH_DEVICE_EXTENSION DevExt = DeviceObject->DeviceExtension;
	PIO_STACK_LOCATION IrpSp = IoGetCurrentIrpStackLocation(Irp);
	PQH_DRIVER_EXTENSION DriverExt = NULL;

	if (!DevExt)
		return QHIrpDispatchDefault(DeviceObject, Irp);

	DriverExt = IoGetDriverObjectExtension(g_DriverObject, &g_DriverObject);
	if (!DriverExt) {
		IoSkipCurrentIrpStackLocation(Irp);
		return IoCallDriver(DevExt->LowerDeviceObject, Irp);
	}

	switch (IrpSp->MinorFunction)
	{
	case IRP_MN_REMOVE_DEVICE:	// 删除的控制码, 书中说要处理这个, 具体这些代码我还未测试
	{
		KIRQL OldIrql;
		PQH_DEVICE_LIST_NODE NodeToFree = NULL;

		KeAcquireSpinLock(&DriverExt->DeviceObjectListLock, &OldIrql);
		PLIST_ENTRY PEntry = DriverExt->DeviceObjectListHead.Flink;
		while (PEntry != &DriverExt->DeviceObjectListHead)
		{
			PQH_DEVICE_LIST_NODE Node = CONTAINING_RECORD(PEntry, QH_DEVICE_LIST_NODE, Entry);
			// 要删除的设备是否是过滤设备
			if (Node->DeviceObject == DeviceObject)
			{
				// 如果是 在链表中删除此节点
				RemoveEntryList(&Node->Entry);
				NodeToFree = Node;
				break;
			}
			PEntry = PEntry->Flink;
		}
		KeReleaseSpinLock(&DriverExt->DeviceObjectListLock, OldIrql);

		if (NodeToFree)
			qhfree(NodeToFree);

		QHCompleteIrp(Irp, STATUS_SUCCESS, 0);

		// 无论链表中是否移除了此设备 都要进行设备删除操作
		QHDeleteFilterDevice(DeviceObject);
		return STATUS_SUCCESS;
	}

	// 物理设备如果下线
	case IRP_MN_DEVICE_USAGE_NOTIFICATION:
	{
		// 是不是分页
		if (IrpSp->Parameters.UsageNotification.Type != DeviceUsageTypePaging)
		{
			IoSkipCurrentIrpStackLocation(Irp);
			return IoCallDriver(DevExt->LowerDeviceObject, Irp);
		}

		// 如果不是分页
		BOOLEAN SetPagable = FALSE;
		// InPath = FALSE 表示移除分页文件路径，且当前分页路径计数为 1（即将变为 0）
		if (!IrpSp->Parameters.UsageNotification.InPath &&
			DevExt->PagingPathCount == 1)
		{
			// 此设备不是大浪涌电流设备
			if (!(DeviceObject->Flags & DO_POWER_INRUSH))
			{
				// 则设置 DO_POWER_PAGABLE 标志
				// 在电源转换期间，设备可以被分页（即其内存可以换出）
				DeviceObject->Flags |= DO_POWER_PAGABLE;
				SetPagable = TRUE;
			}
		}

		// 初始化等待事件
		KEVENT Event;
		KeInitializeEvent(&Event, NotificationEvent, FALSE);
		// 向下传递IRP
		IoCopyCurrentIrpStackLocationToNext(Irp);
		IoSetCompletionRoutine(Irp, PnpCompletionRoutine, &Event, TRUE, TRUE, TRUE);
		NTSTATUS Status = IoCallDriver(DevExt->LowerDeviceObject, Irp);
		// 如果传递为等待
		if (Status == STATUS_PENDING)
		{
			// 通过等待事件 等待完成
			KeWaitForSingleObject(&Event, Executive, KernelMode, FALSE, NULL);
			Status = Irp->IoStatus.Status;
		}

		if (NT_SUCCESS(Status))
		{
			IoAdjustPagingPathCount(&DevExt->PagingPathCount,
				IrpSp->Parameters.UsageNotification.InPath);
			if (IrpSp->Parameters.UsageNotification.InPath &&
				DevExt->PagingPathCount == 1)
			{
				DeviceObject->Flags &= ~DO_POWER_PAGABLE;
			}
		}
		else
		{
			if (SetPagable)
			{
				DeviceObject->Flags &= ~DO_POWER_PAGABLE;
			}
		}

		QHCompleteIrp(Irp, Status, Irp->IoStatus.Information);
		return Status;
	}

	default:
		break;
	}

	IoSkipCurrentIrpStackLocation(Irp);
	return IoCallDriver(DevExt->LowerDeviceObject, Irp);
}

NTSTATUS QHIrpDispatchPower(_In_ PDEVICE_OBJECT DeviceObject, _Inout_ PIRP Irp)
{
	PQH_DEVICE_EXTENSION DevExt = DeviceObject->DeviceExtension;
	if (!DevExt)
		return QHIrpDispatchDefault(DeviceObject, Irp);

#if (NTDDI_VERSION < NTDDI_VISTA)
	PoStartNextPowerIrp(Irp);
	IoSkipCurrentIrpStackLocation(Irp);
	return PoCallDriver(DevExt->LowerDeviceObject, Irp);
#else
	IoSkipCurrentIrpStackLocation(Irp);
	return IoCallDriver(DevExt->LowerDeviceObject, Irp);
#endif
}

static NTSTATUS QHVolumeOnlineCompletionRoutine(
	_In_ PDEVICE_OBJECT DeviceObject,
	_In_ PIRP Irp,
	_In_ PVOID Context)
{
	UNREFERENCED_PARAMETER(DeviceObject);
	UNREFERENCED_PARAMETER(Irp);
	KeSetEvent((PKEVENT)Context, IO_NO_INCREMENT, FALSE);
	return STATUS_MORE_PROCESSING_REQUIRED;
}

NTSTATUS QHIrpDispatchDeviceControl(_In_ PDEVICE_OBJECT DeviceObject, _Inout_ PIRP Irp)
{
	PQH_DEVICE_EXTENSION DevExt = DeviceObject->DeviceExtension;
	PIO_STACK_LOCATION IrpSp = IoGetCurrentIrpStackLocation(Irp);

	// 控制设备：处理自定义通讯 IOCTL
	if (!DevExt)
	{
		switch (IrpSp->Parameters.DeviceIoControl.IoControlCode)
		{
		case IOCTL_QH_QUERY_PROTECT_STATUS:	// 查询保护状态
		{
			if (IrpSp->Parameters.DeviceIoControl.OutputBufferLength < sizeof(BOOLEAN))
			{
				return QHCompleteIrp(Irp, STATUS_BUFFER_TOO_SMALL, 0);
			}

			PQH_DRIVER_EXTENSION DriverExt = IoGetDriverObjectExtension(g_DriverObject, &g_DriverObject);
			BOOLEAN isProtecting = FALSE;

			if (DriverExt)
			{
				KIRQL OldIrql;
				KeAcquireSpinLock(&DriverExt->DeviceObjectListLock, &OldIrql);
				PLIST_ENTRY Entry = DriverExt->DeviceObjectListHead.Flink;
				while (Entry != &DriverExt->DeviceObjectListHead)
				{
					PQH_DEVICE_LIST_NODE Node = CONTAINING_RECORD(Entry, QH_DEVICE_LIST_NODE, Entry);
					PQH_DEVICE_EXTENSION VolExt = (PQH_DEVICE_EXTENSION)Node->DeviceObject->DeviceExtension;
					if (VolExt->Initialized)
					{
						isProtecting = TRUE;
					}
					Entry = Entry->Flink;
				}
				KeReleaseSpinLock(&DriverExt->DeviceObjectListLock, OldIrql);
			}

			*(PBOOLEAN)Irp->AssociatedIrp.SystemBuffer = isProtecting;
			return QHCompleteIrp(Irp, STATUS_SUCCESS, sizeof(BOOLEAN));
		}

		default:
			break;
		}

		return QHCompleteIrp(Irp, STATUS_INVALID_DEVICE_REQUEST, 0);
	}

	switch (IrpSp->Parameters.DeviceIoControl.IoControlCode)
	{
	case IOCTL_VOLUME_ONLINE:
	{
		if (DevExt->Initialized) {
			break;
		}

		// QHBootReinitializationRoutine初始化未完成前，不允许激活保护
		// 此时 NTFS 挂载流程尚未完成，dirty flag 等关键写入必须直接落盘
		{
			PQH_DRIVER_EXTENSION DriverExt = IoGetDriverObjectExtension(g_DriverObject, &g_DriverObject);
			if (DriverExt && !DriverExt->BootReinitDone)
			{
				// 初始化尚未完成，直接下发，不激活保护
				IoSkipCurrentIrpStackLocation(Irp);
				return IoCallDriver(DevExt->LowerDeviceObject, Irp);
			}
		}

		// 什么时候代码能走到这一步?
		// 动态插拔介质(例如移动硬盘或者U盘等)
		KEVENT Event;
		KeInitializeEvent(&Event, NotificationEvent, FALSE);

		IoCopyCurrentIrpStackLocationToNext(Irp);
		IoSetCompletionRoutine(Irp, QHVolumeOnlineCompletionRoutine, &Event, TRUE, TRUE, TRUE);

		NTSTATUS Status = IoCallDriver(DevExt->LowerDeviceObject, Irp);
		if (Status == STATUS_PENDING)
		{
			KeWaitForSingleObject(&Event, Executive, KernelMode, FALSE, NULL);
			Status = Irp->IoStatus.Status;
		}

		if (!NT_SUCCESS(Status))
		{
			return QHCompleteIrp(Irp, Status, Irp->IoStatus.Information);
		}

		// 使用公共初始化函数
		QHInitializeVolumeProtection(DeviceObject, DevExt);

		return QHCompleteIrp(Irp, STATUS_SUCCESS, Irp->IoStatus.Information);
	}

	default:
		break;
	}

	IoSkipCurrentIrpStackLocation(Irp);
	return IoCallDriver(DevExt->LowerDeviceObject, Irp);
}
