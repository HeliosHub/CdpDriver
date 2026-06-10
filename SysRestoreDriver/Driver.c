#include "QHEngineDefs.h"
#include "QHIrpDispatchs.h"
#include "QHBitmap.h"
#include <ntstrsafe.h>

PDRIVER_OBJECT g_DriverObject = NULL;

VOID QHDeleteFilterDevice(_In_ PDEVICE_OBJECT FilterDeviceObject)
{
	PQH_DEVICE_EXTENSION DevExt = FilterDeviceObject->DeviceExtension;
	KIRQL OldIrql;
	PLIST_ENTRY Entry;

	if (!DevExt) return;

	// 先禁止新的保护读写, 阻止新IRP处理 自然不会有新的IRP被插入工作项队列
	InterlockedExchange8((volatile CHAR*)&DevExt->Initialized, 0);

	// 排空队列中的IRP, 使工作项不会持续循环
	KeAcquireSpinLock(&DevExt->PendingIrpQueueLock, &OldIrql);
	while (!IsListEmpty(&DevExt->PendingIrpQueue))
	{
		Entry = RemoveHeadList(&DevExt->PendingIrpQueue);
		PIRP Irp = CONTAINING_RECORD(Entry, IRP, Tail.Overlay.ListEntry);
		KeReleaseSpinLock(&DevExt->PendingIrpQueueLock, OldIrql);

		// 直接完成该IRP返回失败 一般来说QHDeleteFilterDevice被调用 就等于关机或者重启了
		QHCompleteIrp(Irp, STATUS_DEVICE_NOT_READY, 0);

		KeAcquireSpinLock(&DevExt->PendingIrpQueueLock, &OldIrql);
	}
	KeReleaseSpinLock(&DevExt->PendingIrpQueueLock, OldIrql);

	// 等待工作项完成：排空队列后工作项会快速结束并 SetEvent
	// 必须在 IoDetachDevice 之前等待，否则工作项中访问 LowerDeviceObject 会空指针崩溃
	if (DevExt->IoWorkItem) {
		KeWaitForSingleObject(&DevExt->WorkItemFinished, Executive, KernelMode, FALSE, NULL);
		IoFreeWorkItem(DevExt->IoWorkItem);
		DevExt->IoWorkItem = NULL;
	}

	// 工作项已结束，现在安全地断开设备栈
	if (DevExt->LowerDeviceObject) {
		IoDetachDevice(DevExt->LowerDeviceObject);
		DevExt->LowerDeviceObject = NULL;
	}

	// 释放资源
	// 此时 Initialized=0, IRP 队列已排空, 工作项已 SetEvent, LowerDeviceObject 已断栈
	// → 不可能再有线程访问这些字段, 直接释放即可
	if (DevExt->SectorBitmap)
	{
		QHBitmapFree(DevExt->SectorBitmap);
		DevExt->SectorBitmap = NULL;
	}

	if (DevExt->OffsetHash)
	{
		QHHashDelete(DevExt->OffsetHash);
		DevExt->OffsetHash = NULL;
	}

	// 安全销毁过滤设备：已无引用、无队列、无资源. 删除设备本身
	IoDeleteDevice(FilterDeviceObject);
}

// 创建通讯设备
NTSTATUS QHCreateControlDevice(_In_ PDRIVER_OBJECT DriverObject)
{
	NTSTATUS Status = STATUS_SUCCESS;
	PQH_DRIVER_EXTENSION DriverExtension = NULL;
	DECLARE_CONST_UNICODE_STRING(ControlDeviceName, QH_CONTROL_DEVICE_NAME);
	DECLARE_CONST_UNICODE_STRING(ControlSystemLinkName, QH_CONTROL_SYSTEM_LINK_NAME);

	if (!(DriverExtension = IoGetDriverObjectExtension(DriverObject, &g_DriverObject)))
	{
		Status = STATUS_UNSUCCESSFUL;
		goto cleanup;
	}

	// 创建通讯设备
	Status = IoCreateDevice(
		DriverObject,
		0,
		(PUNICODE_STRING)&ControlDeviceName,
		FILE_DEVICE_UNKNOWN,
		FILE_DEVICE_SECURE_OPEN,
		FALSE,
		&DriverExtension->ControlDevice
	);
	if (!NT_SUCCESS(Status))
		goto cleanup;

	// 创建通讯设备与用户态的符号链接
	Status = IoCreateSymbolicLink((PUNICODE_STRING)&ControlSystemLinkName, (PUNICODE_STRING)&ControlDeviceName);
	if (!NT_SUCCESS(Status))
		goto cleanup;

cleanup:
	if (!NT_SUCCESS(Status))
	{
		if (DriverExtension)
		{
			if (DriverExtension->ControlDevice)
			{
				IoDeleteDevice(DriverExtension->ControlDevice);
				DriverExtension->ControlDevice = NULL;
			}
		}
	}
	return Status;
}

// 添加设备
NTSTATUS QHAddDevice(_In_ PDRIVER_OBJECT DriverObject, _In_ PDEVICE_OBJECT PhysicalDeviceObject)
{
	NTSTATUS Status = STATUS_SUCCESS;
	PQH_DRIVER_EXTENSION DriverExtension = NULL;
	PQH_DEVICE_EXTENSION DeviceExtension = NULL;
	PDEVICE_OBJECT FilterDeviceObject = NULL;
	PQH_DEVICE_LIST_NODE DeviceListNode = NULL;

	if (!(DriverExtension = IoGetDriverObjectExtension(DriverObject, &g_DriverObject)))
	{
		Status = STATUS_UNSUCCESSFUL;
		goto cleanup;
	}

	// 每个卷在上线后 都创建一个过滤设备
	Status = IoCreateDevice(
		DriverObject,
		sizeof(QH_DEVICE_EXTENSION),
		NULL,
		FILE_DEVICE_DISK,
		FILE_DEVICE_SECURE_OPEN,
		FALSE,
		&FilterDeviceObject
	);
	if (!NT_SUCCESS(Status))
		goto cleanup;

	// 设备扩展初始化
	DeviceExtension = (PQH_DEVICE_EXTENSION)FilterDeviceObject->DeviceExtension;

	RtlZeroMemory(DeviceExtension, sizeof(QH_DEVICE_EXTENSION));
	DeviceExtension->PhysicalDeviceObject = PhysicalDeviceObject;

	Status = IoAttachDeviceToDeviceStackSafe(FilterDeviceObject, PhysicalDeviceObject, &DeviceExtension->LowerDeviceObject);
	if (!NT_SUCCESS(Status))
		goto cleanup;

	// 位图锁
	ExInitializeFastMutex(&DeviceExtension->BitmapMutex);
	// 偏移记录表锁
	ExInitializeFastMutex(&DeviceExtension->OffsetHashMutex);
	// 这个EVENT主要是用来检测IRP处理是否正在进行 卸载时
	KeInitializeEvent(&DeviceExtension->WorkItemFinished, NotificationEvent, TRUE);
	// 队列锁
	KeInitializeSpinLock(&DeviceExtension->PendingIrpQueueLock);
	// 队列 
	// 在IRP处理中 只要IRQL > PASSIVE_LEVEL 都排到队列中 由系统工作项执行
	InitializeListHead(&DeviceExtension->PendingIrpQueue);
	// 工作项
	DeviceExtension->IoWorkItem = IoAllocateWorkItem(FilterDeviceObject);
	DeviceExtension->IoWorkItemQueued = 0;

	DeviceExtension->SectorBitmap = NULL;
	DeviceExtension->ProtectRangeCount = 0;
	DeviceExtension->LastScanIndex = 0;
	FilterDeviceObject->Flags = DeviceExtension->LowerDeviceObject->Flags | DO_POWER_PAGABLE | DO_DIRECT_IO;

	// 偏移记录表 看起来是hash, 其实不是hash. 后期考虑替换为hash
	// 原本的确是hash, 但是我将klib的khashl移植过来使用出现严重的卡死问题 我就删除了 也避免第三方库的开源协议污染
	DeviceExtension->OffsetHash = QHHashCreate();
	if (!DeviceExtension->OffsetHash)
	{
		Status = STATUS_MEMORY_NOT_ALLOCATED;
		goto cleanup;
	}

	// 创建好的过滤驱动, 放到node里
	DeviceListNode = qhalloc(sizeof(QH_DEVICE_LIST_NODE));
	if (!DeviceListNode)
	{
		Status = STATUS_MEMORY_NOT_ALLOCATED;
		goto cleanup;
	}
	DeviceListNode->DeviceObject = FilterDeviceObject;
	// 由Driver的扩展结构保存
	ExInterlockedInsertHeadList(&DriverExtension->DeviceObjectListHead,
		&DeviceListNode->Entry,
		&DriverExtension->DeviceObjectListLock);

	// 此过滤设备正式初始化完毕
	// 但是也只是过滤设备初始化 只有设备肯定不行
	// 剩余的初始化由IoRegisterBootDriverReinitialization注册的回调函数QHBootReinitializationRoutine继续初始化
	FilterDeviceObject->Flags &= ~DO_DEVICE_INITIALIZING;

cleanup:
	if (!NT_SUCCESS(Status))
	{
		if (FilterDeviceObject)
		{
			IoDeleteDevice(FilterDeviceObject);
			FilterDeviceObject = NULL;
		}
	}
	return Status;
}

// 驱动退出函数
// 实际上这个函数实现我都不怎么想写 只有关机的时候才会调用这个
// 如果在计算机运行时调用 那直接就是灾难级别事故
// 一瞬间所有的硬盘数据全丢回上一次关机时的数据 你猜会发生什么事情
// 为了看起来合理 有健壮性 我就写了
VOID QHDriverUnload(_In_ PDRIVER_OBJECT DriverObject)
{
	PQH_DRIVER_EXTENSION DriverExtension = NULL;
	PLIST_ENTRY FilterNodeEntry = NULL;
	DECLARE_CONST_UNICODE_STRING(ControlSystemLinkName, QH_CONTROL_SYSTEM_LINK_NAME);

	if (!(DriverExtension = IoGetDriverObjectExtension(DriverObject, &g_DriverObject)))
		return;

	while ((FilterNodeEntry = ExInterlockedRemoveHeadList(&DriverExtension->DeviceObjectListHead, &DriverExtension->DeviceObjectListLock)))
	{
		PQH_DEVICE_LIST_NODE DeviceListNode = CONTAINING_RECORD(FilterNodeEntry, QH_DEVICE_LIST_NODE, Entry);
		if (DeviceListNode->DeviceObject)
		{
			QHDeleteFilterDevice(DeviceListNode->DeviceObject);
		}
		qhfree(DeviceListNode);
	}

	if (DriverExtension->ControlDevice)
	{
		IoDeleteSymbolicLink((PUNICODE_STRING)&ControlSystemLinkName);
		IoDeleteDevice(DriverExtension->ControlDevice);
		DriverExtension->ControlDevice = NULL;
	}
}

// 驱动入口
// 此驱动任何(变量名、函数名、参数名、结构名、宏定义等等)命名尽量靠近Windows命名规范
// 此函数内只做基本的IRP回调填写 还有一些额外的填写操作 不做任何处理
// 真正第一步是在QHAddDevice中

// 作者的话：写在这里也显眼, 这是我第一个驱动程序 此驱动耗费了我近乎一个多月的时间才写了个初版 也就是现在的版本
// 让我吐血的是, 直到我写完后我才知道windows有个驱动的官方例子集合?懵逼了都
// 我第一个bug就差点没让我放弃编写此驱动. 你们都想不到是什么问题,栈溢出, 我就申请了几个2048个字节的WCHAR变量 栈内存就没了 然后莫名其妙崩溃
// 直至写完此驱动 我都不知道有个调试工具叫做WinDbg 全都是出现蓝屏死盯着代码 不行就查资料
// 网上只有商业软件 没有此类驱动的开源实现 查资料一度查到绝望
NTSTATUS DriverEntry(_In_ PDRIVER_OBJECT DriverObject, _In_ PUNICODE_STRING RegistryPath)
{
	NTSTATUS Status = STATUS_SUCCESS;
	PQH_DRIVER_EXTENSION DriverExtension = NULL;

	UNREFERENCED_PARAMETER(RegistryPath);

	g_DriverObject = DriverObject;
	DriverObject->DriverUnload = QHDriverUnload;

	// 创建 Driver 的扩展结构
	Status = IoAllocateDriverObjectExtension(
		DriverObject,
		&g_DriverObject,
		sizeof(QH_DRIVER_EXTENSION),
		&DriverExtension
	);

	if (!NT_SUCCESS(Status))
		goto cleanup;

	RtlZeroMemory(DriverExtension, sizeof(QH_DRIVER_EXTENSION));
	InitializeListHead(&DriverExtension->DeviceObjectListHead);
	KeInitializeSpinLock(&DriverExtension->DeviceObjectListLock);

	// 创建通讯设备
	Status = QHCreateControlDevice(DriverObject);
	if (!NT_SUCCESS(Status))
		goto cleanup;

	// 默认IRP处理
	for (ULONG i = 0; i < IRP_MJ_MAXIMUM_FUNCTION; ++i)
	{
		DriverObject->MajorFunction[i] = QHIrpDispatchDefault;
	}

	// 其实这三个和默认IRP处理区别不是很大
	DriverObject->MajorFunction[IRP_MJ_CREATE] = QHIrpDispatchCreateCloseCleanup;
	DriverObject->MajorFunction[IRP_MJ_CLOSE] = QHIrpDispatchCreateCloseCleanup;
	DriverObject->MajorFunction[IRP_MJ_CLEANUP] = QHIrpDispatchCreateCloseCleanup;

	// 读IRP处理
	DriverObject->MajorFunction[IRP_MJ_READ] = QHIrpDispatchRead;
	// 写IRP处理
	DriverObject->MajorFunction[IRP_MJ_WRITE] = QHIrpDispatchWrite;
	// PNP处理
	DriverObject->MajorFunction[IRP_MJ_PNP] = QHIrpDispatchPnp;
	// 电源管理 这个函数也没什么太大用
	// 此过滤驱动直接将硬盘的数据定格 重启就复原了 关机时直接不处理
	// 至于休眠什么的更不考虑 无论如何过滤设备不能够暂停或者停止 否则会引发灾难性后果
	DriverObject->MajorFunction[IRP_MJ_POWER] = QHIrpDispatchPower;
	// 主要处理通讯设备和过滤设备的初始化
	// 过滤设备的初始化主要是在QHBootReinitializationRoutine中初始化
	// 至于在IRP_MJ_DEVICE_CONTROL中处理IOCTL_VOLUME_ONLINE 主要是因为有可能有想要保护的动态插拔介质(例如U盘)
	DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = QHIrpDispatchDeviceControl;
	// 每当某个卷上线 都会触发此函数
	DriverObject->DriverExtension->AddDevice = QHAddDevice;
	// 此回调在文件系统加载后会被执行 正合适初始化过滤设备
	// 原本我的初始化都是在IRP_MJ_DEVICE_CONTROL的IOCTL_VOLUME_ONLINE中处理的 但是有多重考虑就放在了回调函数中处理了
	// 考虑1. 我需要将某些文件剔除重定向 例如pagefile.sys等, 这些虚拟内存文件属实没必要重定向 否则需要记录重定向 浪费内存
	//		内核内存非常宝贵 不能浪费内存 但是还是有一层顾虑. 万一用户手动修改了虚拟内存文件大小 这个就很坑爹了
	// 考虑2. 我一开始写了位图的获取, 是直接从NTFS卷格式解析出来, 但是我考虑到多格式(FAT32等), 看看后期有没有那个必要直接调用API
	//		但是用API获取位图数据需要文件系统的支持, IOCTL_VOLUME_ONLINE此时大概率文件系统还没有上线, 用不了
	IoRegisterBootDriverReinitialization(DriverObject, QHBootReinitializationRoutine, NULL);

cleanup:
	if (!NT_SUCCESS(Status))
	{
		QHDriverUnload(DriverObject);
	}
	return Status;
}
