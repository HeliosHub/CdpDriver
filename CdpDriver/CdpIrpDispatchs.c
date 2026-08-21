#include "CdpIrpDispatchs.h"
#include "..\CdpCore\include\cdp_core.h"
#include "CdpCredential.h"
#include "..\CdpCore\include\cdp_dev_store.h"
#include <ntdddisk.h>
#include <ntddstor.h>
#include <ntstrsafe.h>

static VOID CdpDisableAllCaptureSources(_In_ PCdp_DRIVER_EXTENSION DriverExt);
static NTSTATUS CdpStartMergeThread(_Inout_ PCdp_DEVICE_EXTENSION DevExt);
static VOID CdpStopMergeThread(_Inout_ PCdp_DEVICE_EXTENSION DevExt);
static VOID CdpStartMergeIfNeeded(_Inout_ PCdp_DEVICE_EXTENSION DevExt);
static NTSTATUS CdpDrainAndDisableCapture(
	_Inout_ PCdp_DEVICE_EXTENSION DevExt);
static NTSTATUS CdpScatterReadMdlChain(
	_In_ PIRP Irp,
	_In_reads_bytes_(Length) const UCHAR* Source,
	_In_ ULONG Length,
	_Out_ PULONG MdlCount,
	_Out_ PUINT64 MdlBytes,
	_Out_ PULONG CopiedBytes);
static NTSTATUS CdpSnapshotWriteMdlChain(
	_In_ PIRP Irp,
	_In_ ULONG RequiredLength,
	_Outptr_result_bytebuffer_(RequiredLength) PUCHAR* Snapshot,
	_Out_ PULONG MdlCount,
	_Out_ PUINT64 MdlBytes);
static NTSTATUS CdpQueueDiskCaptureIrp(
	_Inout_ PCdp_DEVICE_EXTENSION DiskExt,
	_Inout_ PIRP Irp);
static NTSTATUS CdpForwardQueuedDiskIrpSynchronously(
	_Inout_ PCdp_CAPTURE_ITEM Item);
static PCdp_VOLUME_HANDLE_ENTRY CdpAcquireJournalForSource(
	_In_ PCdp_DRIVER_EXTENSION DriverExt,
	_In_ PCdp_DEVICE_EXTENSION SourceExt);
static VOID CdpReleaseVolumeHandleEntry(_In_ PCdp_VOLUME_HANDLE_ENTRY Item);
static NTSTATUS CdpCloseVolumeHandle(
	_In_ PCdp_DRIVER_EXTENSION DriverExt,
	_In_ UINT64 HandleId);
static VOID CdpAuditProtectedReadSummary(
	_In_ PCdp_DEVICE_EXTENSION DevExt,
	_In_ PCSTR Reason);
static NTSTATUS CdpForwardWriteCompletion(
	_In_ PDEVICE_OBJECT DeviceObject,
	_In_ PIRP Irp,
	_In_ PVOID Context);
static PCdp_DEVICE_EXTENSION CdpFindDiskExtensionByNumber(
	_In_ PCdp_DRIVER_EXTENSION DriverExt,
	_In_ ULONG DiskNumber);
static PCdp_DEVICE_EXTENSION CdpFindSourceExtensionByGuid(
	_In_ PCdp_DRIVER_EXTENSION DriverExt,
	_In_ const GUID* VolumeGuid);
static PCdp_DEVICE_EXTENSION CdpFindVolumeExtensionByLowerDevice(
	_In_ PCdp_DRIVER_EXTENSION DriverExt,
	_In_ PDEVICE_OBJECT LowerDevice);
static PDEVICE_OBJECT CdpReferenceVolumeLowerByPhysicalRange(
	_In_ PCdp_DRIVER_EXTENSION DriverExt,
	_In_ ULONG DiskNumber,
	_In_ UINT64 PartitionStart,
	_In_ UINT64 PartitionSize);
static UINT64 CdpFindJournalHandleBySourceGuid(
	_In_ PCdp_DRIVER_EXTENSION DriverExt,
	_In_ const GUID* SourceVolumeGuid);
static NTSTATUS CdpQueryPhysicalPartitionLayout(
	_In_ PDEVICE_OBJECT PartitionDevice,
	_Out_ PULONG DiskNumber,
	_Out_ PULONG PartitionNumber,
	_Out_ PUINT64 PartitionStart,
	_Out_ PUINT64 PartitionLength,
	_Out_ PBOOLEAN HasNextPartition,
	_Out_ PUINT64 NextPartitionStart,
	_Out_opt_ PULONG NextPartitionNumber,
	_Out_opt_ PUINT64 NextPartitionLength,
	_Out_opt_ PULONG DiskPartitionStyle,
	_Out_opt_ PULONG MbrSignature,
	_Out_opt_ GUID* DiskGuid);
static NTSTATUS CdpDiscoverJournalForStartedDisk(
	_In_ PCdp_DRIVER_EXTENSION DriverExt,
	_Inout_ PCdp_DEVICE_EXTENSION DiskExt);

static NTSTATUS CdpValidateProtectionObjectGraph(
	_In_ PCdp_DRIVER_EXTENSION DriverExt,
	_In_ PCdp_DEVICE_EXTENSION SourceExt,
	_In_ PCSTR Stage)
{
	PCdp_VOLUME_HANDLE_ENTRY journalEntry = NULL;
	PCdp_DEVICE_EXTENSION diskExt;
	PCSTR reason = NULL;

	if (!DriverExt || !SourceExt)
	{
		reason = "null-driver-or-source";
		goto failed;
	}
	journalEntry = SourceExt->RedirectJournalEntry;
	diskExt = CdpFindDiskExtensionByNumber(
		DriverExt, SourceExt->DiskNumber);
	if (SourceExt->DeviceKind != Cdp_DEVICE_KIND_VOLUME &&
		SourceExt->DeviceKind != Cdp_DEVICE_KIND_DISK)
		reason = "source-kind";
	else if (InterlockedCompareExchange(&SourceExt->Started, 0, 0) == 0)
		reason = "source-not-started";
	else if (!SourceExt->FilterDeviceObject || !SourceExt->LowerDeviceObject)
		reason = "source-device";
	else if (!SourceExt->DiskLayoutValid || SourceExt->PartitionSize == 0 ||
		SourceExt->PartitionStart > MAXUINT64 - SourceExt->PartitionSize)
		reason = "source-range";
	else if (SourceExt->SectorSize != 512 && SourceExt->SectorSize != 4096)
		reason = "source-sector";
	else if (!SourceExt->Core)
		reason = "core-null";
	else if (!diskExt || !diskExt->CaptureThreadHandle)
		reason = "worker-handle";
	else if (SourceExt->JournalHandleId == 0)
		reason = "journal-handle-id";
	else if (!journalEntry)
		reason = "redirect-entry-null";
	else if (InterlockedCompareExchange(
			&journalEntry->ReferenceCount, 0, 0) <= 0 || journalEntry->Closing)
		reason = "redirect-entry-lifetime";
	else if (!journalEntry->TargetLowerDevice)
		reason = "journal-device";
	else if (!journalEntry->Journal.Mounted)
		reason = "journal-not-mounted";
	else if (!journalEntry->Journal.TargetDevice ||
		journalEntry->Journal.TargetDevice != journalEntry->TargetLowerDevice)
		reason = "journal-target";
	else if (journalEntry->PartitionSize == 0 ||
		journalEntry->TargetBaseOffset >
			MAXUINT64 - journalEntry->PartitionSize)
		reason = "journal-range";
	else if (journalEntry->SectorSize != SourceExt->SectorSize ||
		journalEntry->Journal.SectorSize != journalEntry->SectorSize)
		reason = "journal-sector";
	else if (journalEntry->Journal.TargetBaseOffset !=
			journalEntry->TargetBaseOffset ||
		journalEntry->Journal.PartitionSize != journalEntry->PartitionSize)
		reason = "journal-backend-range";
	else if (!diskExt || !diskExt->LowerDeviceObject ||
		!diskExt->DiskLayoutValid)
		reason = "disk-extension";
	else if (diskExt->LowerDeviceObject != journalEntry->TargetLowerDevice ||
		SourceExt->DiskNumber != journalEntry->DiskNumber)
		reason = "disk-target-mismatch";
	else if (SourceExt->PartitionStart + SourceExt->PartitionSize >
			journalEntry->TargetBaseOffset)
		reason = "partition-overlap";
	else if (!SourceExt->HasNextPartition ||
		SourceExt->NextPartitionStart != journalEntry->TargetBaseOffset)
		reason = "journal-not-successor";

	if (!reason)
	{
		Cdp_LOG("[ACTIVATE-CHECK-OK] stage=%s source=%p core=%p disk=%lu part=%lu start=%llu size=%llu lower=%p journal=%p ref=%ld handle=%llu part=%lu base=%llu size=%llu lower=%p mounted=%u workers=%p/%p\n",
			Stage, SourceExt, SourceExt->Core,
			SourceExt->DiskNumber, SourceExt->PartitionNumber,
			SourceExt->PartitionStart, SourceExt->PartitionSize,
			SourceExt->LowerDeviceObject, journalEntry,
			InterlockedCompareExchange(&journalEntry->ReferenceCount, 0, 0),
			SourceExt->JournalHandleId, journalEntry->PartitionNumber,
			journalEntry->TargetBaseOffset, journalEntry->PartitionSize,
			journalEntry->TargetLowerDevice,
			journalEntry->Journal.Mounted ? 1u : 0u,
			diskExt->CaptureThreadHandle,
			NULL);
		return STATUS_SUCCESS;
	}

failed:
	Cdp_LOG("[ACTIVATE-CHECK-FAIL] stage=%s reason=%s source=%p core=%p validated=%ld enabled=%ld disk=%lu part=%lu start=%llu size=%llu next=%llu hasNext=%u sourceLower=%p journal=%p journalHandle=%llu\n",
		Stage ? Stage : "null",
		reason ? reason : "unknown",
		SourceExt,
		SourceExt ? SourceExt->Core : NULL,
		SourceExt ? InterlockedCompareExchange(
			&SourceExt->ProtectionStateValidated, 0, 0) : 0,
		SourceExt ? InterlockedCompareExchange(
			&SourceExt->CaptureEnabled, 0, 0) : 0,
		SourceExt ? SourceExt->DiskNumber : 0,
		SourceExt ? SourceExt->PartitionNumber : 0,
		SourceExt ? SourceExt->PartitionStart : 0,
		SourceExt ? SourceExt->PartitionSize : 0,
		SourceExt ? SourceExt->NextPartitionStart : 0,
		(SourceExt && SourceExt->HasNextPartition) ? 1u : 0u,
		SourceExt ? SourceExt->LowerDeviceObject : NULL,
		SourceExt ? SourceExt->RedirectJournalEntry : NULL,
		SourceExt ? SourceExt->JournalHandleId : 0);
	return STATUS_DEVICE_CONFIGURATION_ERROR;
}

/* Validate every persisted data record before CaptureEnabled can expose the
 * rebuilt MetaTree to boot I/O.  Branch records overlay VolumeOffset with
 * branch metadata and therefore are intentionally excluded. */
static NTSTATUS CdpValidateMountedSourceRecordRanges(
	_In_ PCdp_DEVICE_EXTENSION SourceExt,
	_In_ PCSTR Stage)
{
	PCdp_JOURNAL_RECORD records;
	UINT64 sourceEnd;
	UINT64 startIndex = 0;
	UINT64 totalRecords = 0;
	UINT64 generation = 0;
	UINT64 dataRecords = 0;
	UINT64 lowestOffset = 0;
	UINT64 highestEndOffset = 0;
	ULONG metaNodes = 0;
	ULONG returned;
	ULONG index;
	NTSTATUS status = STATUS_SUCCESS;

	if (!SourceExt || !SourceExt->Core || SourceExt->PartitionSize == 0 ||
		SourceExt->PartitionStart > MAXUINT64 - SourceExt->PartitionSize ||
		(SourceExt->SectorSize != 512 && SourceExt->SectorSize != 4096))
	{
		return STATUS_INVALID_DEVICE_STATE;
	}
	sourceEnd = SourceExt->PartitionStart + SourceExt->PartitionSize;
	records = (PCdp_JOURNAL_RECORD)cdpalloc(
		sizeof(*records) * Cdp_JOURNAL_RECORD_QUERY_MAX_PER_CALL);
	if (!records)
		return STATUS_INSUFFICIENT_RESOURCES;

	for (;;)
	{
		returned = 0;
		status = CdpCoreQueryRecordHeaders(
			SourceExt->Core,
			startIndex,
			generation,
			records,
			Cdp_JOURNAL_RECORD_QUERY_MAX_PER_CALL,
			&totalRecords,
			&generation,
			&returned);
		if (!NT_SUCCESS(status))
			break;
		for (index = 0; index < returned; ++index)
		{
			PCdp_JOURNAL_RECORD record = &records[index];
			if ((record->Flags & Cdp_JOURNAL_RECORD_FLAG_BRANCH) != 0)
				continue;
			dataRecords++;
			if (record->DataLength == 0 ||
				record->VolumeOffset < SourceExt->PartitionStart ||
				record->VolumeOffset >= sourceEnd ||
				record->DataLength > sourceEnd - record->VolumeOffset ||
				(record->VolumeOffset % SourceExt->SectorSize) != 0 ||
				(record->DataLength % SourceExt->SectorSize) != 0)
			{
				Cdp_LOG("[MOUNT-RANGE-FAIL] stage=%s recordIndex=%llu sequence=%llu source=[%llu,%llu) recordOffset=%llu len=%lu flags=0x%08lX\n",
					Stage ? Stage : "null",
					startIndex + index,
					record->Sequence,
					SourceExt->PartitionStart,
					sourceEnd,
					record->VolumeOffset,
					record->DataLength,
					record->Flags);
				status = STATUS_DISK_CORRUPT_ERROR;
				goto cleanup;
			}
		}
		startIndex += returned;
		if (startIndex >= totalRecords)
			break;
		if (returned == 0)
		{
			status = STATUS_DISK_CORRUPT_ERROR;
			break;
		}
	}
	if (NT_SUCCESS(status))
	{
		status = CdpCoreQueryMetaTreeStats(
			SourceExt->Core, &metaNodes, &lowestOffset, &highestEndOffset);
		if (NT_SUCCESS(status) && metaNodes != 0 &&
			(lowestOffset < SourceExt->PartitionStart ||
			 highestEndOffset > sourceEnd ||
			 lowestOffset >= highestEndOffset))
		{
			Cdp_LOG("[MOUNT-METATREE-FAIL] stage=%s nodes=%lu tree=[%llu,%llu) source=[%llu,%llu)\n",
				Stage ? Stage : "null", metaNodes,
				lowestOffset, highestEndOffset,
				SourceExt->PartitionStart, sourceEnd);
			status = STATUS_DISK_CORRUPT_ERROR;
		}
	}
	if (NT_SUCCESS(status))
	{
		Cdp_LOG("[MOUNT-RANGE-OK] stage=%s records=%llu dataRecords=%llu metaNodes=%lu tree=[%llu,%llu) source=[%llu,%llu)\n",
			Stage ? Stage : "null", totalRecords, dataRecords, metaNodes,
			lowestOffset, highestEndOffset,
			SourceExt->PartitionStart, sourceEnd);
	}

cleanup:
	cdpfree(records);
	return status;
}

static volatile LONG g_CdpMdllessSystemBufferReported;
static volatile LONG g_CdpMdllessUserBufferReported;

static VOID CdpAuditProtectedReadReset(_Inout_ PCdp_DEVICE_EXTENSION DevExt)
{
	InterlockedExchange64(&DevExt->AuditReadSeenCount, 0);
	InterlockedExchange64(&DevExt->AuditReadSeenBytes, 0);
	InterlockedExchange64(&DevExt->AuditReadCoreSuccessCount, 0);
	InterlockedExchange64(&DevExt->AuditReadCoreSuccessBytes, 0);
	InterlockedExchange64(&DevExt->AuditReadCoreFailureCount, 0);
	InterlockedExchange64(&DevExt->AuditReadSourceBypassCount, 0);
	InterlockedExchange64(&DevExt->AuditReadSourceBypassBytes, 0);
	InterlockedExchange(&DevExt->AuditReadBypassReported, 0);
}

static VOID CdpAuditProtectedReadCoreResult(
	_Inout_ PCdp_DEVICE_EXTENSION DevExt,
	_In_ ULONG Length,
	_In_ NTSTATUS Status)
{
	if (NT_SUCCESS(Status))
	{
		LONG64 count = InterlockedIncrement64(
			&DevExt->AuditReadCoreSuccessCount);
		InterlockedAdd64(&DevExt->AuditReadCoreSuccessBytes, Length);
		if (count == 1 || (count & 0xFFF) == 0)
			CdpAuditProtectedReadSummary(DevExt, "core-checkpoint");
	}
	else
	{
		InterlockedIncrement64(&DevExt->AuditReadCoreFailureCount);
	}
}

static VOID CdpAuditProtectedReadBypass(
	_Inout_ PCdp_DEVICE_EXTENSION DevExt,
	_In_ PIRP Irp,
	_In_ PCSTR Reason)
{
	PIO_STACK_LOCATION irpSp;
	ULONG length;

	if (!DevExt || !Irp ||
		InterlockedCompareExchange(&DevExt->CaptureEnabled, 0, 0) == 0 ||
		InterlockedCompareExchange(&DevExt->Phase, 0, 0) ==
			(LONG)Cdp_PHASE_DRAINING)
	{
		return;
	}
	irpSp = IoGetCurrentIrpStackLocation(Irp);
	length = irpSp->Parameters.Read.Length;
	InterlockedIncrement64(&DevExt->AuditReadSourceBypassCount);
	InterlockedAdd64(&DevExt->AuditReadSourceBypassBytes, length);
	if (InterlockedCompareExchange(
		&DevExt->AuditReadBypassReported, 1, 0) == 0)
	{
		Cdp_LOG("[PROTECTED-READ-BYPASS] FIRST reason=%s irp=%p device=%p lower=%p offset=%lld len=%lu flags=0x%08lX minor=0x%02X enabled=%ld stopping=%ld phase=%ld core=%p\n",
			Reason,
			Irp,
			DevExt->FilterDeviceObject,
			DevExt->LowerDeviceObject,
			irpSp->Parameters.Read.ByteOffset.QuadPart,
			length,
			Irp->Flags,
			irpSp->MinorFunction,
			InterlockedCompareExchange(&DevExt->CaptureEnabled, 0, 0),
			InterlockedCompareExchange(&DevExt->CaptureStopping, 0, 0),
			InterlockedCompareExchange(&DevExt->Phase, 0, 0),
			DevExt->Core);
	}
}

static VOID CdpAuditProtectedReadSummary(
	_In_ PCdp_DEVICE_EXTENSION DevExt,
	_In_ PCSTR Reason)
{
	if (!DevExt)
		return;
	Cdp_LOG("[PROTECTED-READ-AUDIT] reason=%s seen=%lld seenBytes=%lld coreOk=%lld coreBytes=%lld coreFail=%lld sourceBypass=%lld bypassBytes=%lld\n",
		Reason,
		InterlockedCompareExchange64(&DevExt->AuditReadSeenCount, 0, 0),
		InterlockedCompareExchange64(&DevExt->AuditReadSeenBytes, 0, 0),
		InterlockedCompareExchange64(&DevExt->AuditReadCoreSuccessCount, 0, 0),
		InterlockedCompareExchange64(&DevExt->AuditReadCoreSuccessBytes, 0, 0),
		InterlockedCompareExchange64(&DevExt->AuditReadCoreFailureCount, 0, 0),
		InterlockedCompareExchange64(&DevExt->AuditReadSourceBypassCount, 0, 0),
		InterlockedCompareExchange64(&DevExt->AuditReadSourceBypassBytes, 0, 0));
}

static NTSTATUS CdpBeginRecovery(
	_In_ PCdp_DRIVER_EXTENSION DriverExt,
	_In_ const Cdp_RECOVERY_BEGIN_REQUEST* Request,
	_Out_ PCdp_RECOVERY_BEGIN_REPLY Reply);

static NTSTATUS CdpCommitRecovery(
	_In_ PCdp_DRIVER_EXTENSION DriverExt,
	_In_ const Cdp_RECOVERY_CONTROL_REQUEST* Request,
	_Out_ PCdp_RECOVERY_COMMIT_REPLY Reply);

static NTSTATUS CdpCoreReadAlignedView(
	_In_ PCdp_DEVICE_EXTENSION DevExt,
	_In_ BOOLEAN Preview,
	_In_ UINT64 Offset,
	_In_ ULONG Length,
	_Out_writes_bytes_(Length) PVOID Buffer);

static VOID CdpFillReply(
	_Out_ PCdp_COMMAND_REPLY Reply,
	_In_ ULONG Command,
	_In_ ULONG Result,
	_In_ UINT64 VolumeHandle,
	_In_ PCWSTR Message)
{
	RtlZeroMemory(Reply, sizeof(*Reply));
	Reply->Command = Command;
	Reply->Result = Result;
	Reply->VolumeHandle = VolumeHandle;
	RtlStringCbCopyW(Reply->Message, sizeof(Reply->Message), Message);
}

static VOID CdpDbgGuid(_In_ PCSTR Tag, _In_ const GUID* G)
{
#if DBG
	Cdp_DBG("%s {%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}\n",
		Tag,
		G->Data1, G->Data2, G->Data3,
		G->Data4[0], G->Data4[1], G->Data4[2], G->Data4[3],
		G->Data4[4], G->Data4[5], G->Data4[6], G->Data4[7]);
#else
	UNREFERENCED_PARAMETER(Tag);
	UNREFERENCED_PARAMETER(G);
#endif
}

static NTSTATUS CdpFormatVolumeNtPath(
	_In_ const GUID* VolumeGuid,
	_Out_writes_bytes_(PathBytes) PWCHAR PathBuffer,
	_In_ SIZE_T PathBytes)
{
	return RtlStringCbPrintfW(
		PathBuffer,
		PathBytes,
		L"\\??\\Volume{%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
		VolumeGuid->Data1,
		VolumeGuid->Data2,
		VolumeGuid->Data3,
		VolumeGuid->Data4[0], VolumeGuid->Data4[1],
		VolumeGuid->Data4[2], VolumeGuid->Data4[3],
		VolumeGuid->Data4[4], VolumeGuid->Data4[5],
		VolumeGuid->Data4[6], VolumeGuid->Data4[7]);
}

static NTSTATUS CdpQueryVolumeGeometry(
	_In_ HANDLE FileHandle,
	_Out_ PUINT64 PartitionSize,
	_Out_ PULONG SectorSize)
{
	GET_LENGTH_INFORMATION lengthInfo;
	DISK_GEOMETRY geometry;
	IO_STATUS_BLOCK iosb;
	NTSTATUS status;

	RtlZeroMemory(&lengthInfo, sizeof(lengthInfo));
	RtlZeroMemory(&geometry, sizeof(geometry));
	status = ZwDeviceIoControlFile(
		FileHandle, NULL, NULL, NULL, &iosb,
		IOCTL_DISK_GET_LENGTH_INFO,
		NULL, 0, &lengthInfo, sizeof(lengthInfo));
	if (!NT_SUCCESS(status))
		return status;
	status = ZwDeviceIoControlFile(
		FileHandle, NULL, NULL, NULL, &iosb,
		IOCTL_DISK_GET_DRIVE_GEOMETRY,
		NULL, 0, &geometry, sizeof(geometry));
	if (!NT_SUCCESS(status))
		return status;
	if (lengthInfo.Length.QuadPart <= 0 ||
		(geometry.BytesPerSector != 512 && geometry.BytesPerSector != 4096))
	{
		return STATUS_NOT_SUPPORTED;
	}
	*PartitionSize = (UINT64)lengthInfo.Length.QuadPart;
	*SectorSize = geometry.BytesPerSector;
	return STATUS_SUCCESS;
}

static NTSTATUS CdpSendDeviceControlSynchronously(
	_In_ PDEVICE_OBJECT Device,
	_In_ ULONG IoControlCode,
	_Out_writes_bytes_(OutputLength) PVOID Output,
	_In_ ULONG OutputLength)
{
	KEVENT event;
	IO_STATUS_BLOCK iosb;
	PIRP irp;
	NTSTATUS status;

	KeInitializeEvent(&event, NotificationEvent, FALSE);
	RtlZeroMemory(&iosb, sizeof(iosb));
	irp = IoBuildDeviceIoControlRequest(
		IoControlCode,
		Device,
		NULL,
		0,
		Output,
		OutputLength,
		FALSE,
		&event,
		&iosb);
	if (!irp)
		return STATUS_INSUFFICIENT_RESOURCES;
	status = IoCallDriver(Device, irp);
	if (status == STATUS_PENDING)
	{
		KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, NULL);
		status = iosb.Status;
	}
	else if (NT_SUCCESS(status))
	{
		status = iosb.Status;
	}
	return status;
}

static NTSTATUS CdpQueryDeviceGeometry(
	_In_ PDEVICE_OBJECT Device,
	_Out_ PUINT64 PartitionSize,
	_Out_ PULONG SectorSize)
{
	GET_LENGTH_INFORMATION lengthInfo;
	DISK_GEOMETRY geometry;
	NTSTATUS status;

	RtlZeroMemory(&lengthInfo, sizeof(lengthInfo));
	RtlZeroMemory(&geometry, sizeof(geometry));
	status = CdpSendDeviceControlSynchronously(
		Device,
		IOCTL_DISK_GET_LENGTH_INFO,
		&lengthInfo,
		sizeof(lengthInfo));
	if (!NT_SUCCESS(status))
		return status;
	status = CdpSendDeviceControlSynchronously(
		Device,
		IOCTL_DISK_GET_DRIVE_GEOMETRY,
		&geometry,
		sizeof(geometry));
	if (!NT_SUCCESS(status))
		return status;
	if (lengthInfo.Length.QuadPart <= 0 ||
		(geometry.BytesPerSector != 512 && geometry.BytesPerSector != 4096))
	{
		return STATUS_NOT_SUPPORTED;
	}
	*PartitionSize = (UINT64)lengthInfo.Length.QuadPart;
	*SectorSize = geometry.BytesPerSector;
	return STATUS_SUCCESS;
}

static PCdp_VOLUME_HANDLE_ENTRY CdpLookupVolumeHandleLocked(
	_In_ PCdp_DRIVER_EXTENSION DriverExt,
	_In_ UINT64 HandleId)
{
	PLIST_ENTRY entry = DriverExt->VolumeHandleList.Flink;
	while (entry != &DriverExt->VolumeHandleList)
	{
		PCdp_VOLUME_HANDLE_ENTRY item = CONTAINING_RECORD(entry, Cdp_VOLUME_HANDLE_ENTRY, Entry);
		if (item->HandleId == HandleId)
			return item;
		entry = entry->Flink;
	}
	return NULL;
}

static NTSTATUS CdpGetSharedCredential(
	_In_ PCdp_DRIVER_EXTENSION DriverExt,
	_Out_ PCdp_CREDENTIAL_DESCRIPTOR Credential,
	_Out_opt_ PULONG JournalCount)
{
	PLIST_ENTRY entry;
	BOOLEAN found = FALSE;
	ULONG count = 0;
	NTSTATUS status = STATUS_NOT_FOUND;

	RtlZeroMemory(Credential, sizeof(*Credential));
	ExAcquireFastMutex(&DriverExt->VolumeHandleMutex);
	for (entry = DriverExt->VolumeHandleList.Flink;
		entry != &DriverExt->VolumeHandleList; entry = entry->Flink)
	{
		PCdp_VOLUME_HANDLE_ENTRY item =
			CONTAINING_RECORD(entry, Cdp_VOLUME_HANDLE_ENTRY, Entry);
		Cdp_CREDENTIAL_DESCRIPTOR current;
		if (item->Closing || !item->Journal.Mounted ||
			!CdpJournalGetCredential(&item->Journal, &current))
		{
			continue;
		}
		if (!found)
		{
			*Credential = current;
			found = TRUE;
			status = STATUS_SUCCESS;
		}
		else if (RtlCompareMemory(Credential, &current, sizeof(current)) !=
			sizeof(current))
		{
			status = STATUS_OBJECT_TYPE_MISMATCH;
			break;
		}
		++count;
	}
	ExReleaseFastMutex(&DriverExt->VolumeHandleMutex);
	if (JournalCount)
		*JournalCount = count;
	return status;
}

// Find our filter's LowerDeviceObject for a volume PDO / stack member.
static PDEVICE_OBJECT CdpFindTargetLowerDevice(
	_In_ PCdp_DRIVER_EXTENSION DriverExt,
	_In_ PDEVICE_OBJECT VolumeDevice)
{
	KIRQL oldIrql;
	PDEVICE_OBJECT lower = NULL;
	PDEVICE_OBJECT walk;

	if (!VolumeDevice)
		return NULL;

	// Prefer matching against devices we attached to in AddDevice.
	KeAcquireSpinLock(&DriverExt->DeviceObjectListLock, &oldIrql);
	{
		PLIST_ENTRY entry = DriverExt->DeviceObjectListHead.Flink;
		while (entry != &DriverExt->DeviceObjectListHead)
		{
			PCdp_DEVICE_LIST_NODE node = CONTAINING_RECORD(entry, Cdp_DEVICE_LIST_NODE, Entry);
			PCdp_DEVICE_EXTENSION volExt = (PCdp_DEVICE_EXTENSION)node->DeviceObject->DeviceExtension;
			if (volExt && volExt->DeviceKind == Cdp_DEVICE_KIND_VOLUME &&
				(volExt->PhysicalDeviceObject == VolumeDevice ||
				 volExt->LowerDeviceObject == VolumeDevice ||
				 node->DeviceObject == VolumeDevice))
			{
				lower = volExt->LowerDeviceObject;
				break;
			}
			entry = entry->Flink;
		}
	}
	KeReleaseSpinLock(&DriverExt->DeviceObjectListLock, oldIrql);
	if (lower)
		return lower;

	// Fallback: walk attachments above the volume until our filter appears.
	for (walk = VolumeDevice; walk; walk = walk->AttachedDevice)
	{
		if (walk->DriverObject == g_DriverObject)
		{
			PCdp_DEVICE_EXTENSION volExt = (PCdp_DEVICE_EXTENSION)walk->DeviceExtension;
			if (volExt)
				return volExt->LowerDeviceObject;
		}
	}
	return NULL;
}

static NTSTATUS CdpResolveTargetLowerDevice(
	_In_ PCdp_DRIVER_EXTENSION DriverExt,
	_In_ HANDLE VolumeFileHandle,
	_Out_ PDEVICE_OBJECT* OutLowerDevice)
{
	NTSTATUS status;
	PFILE_OBJECT fileObject = NULL;
	PDEVICE_OBJECT volumeDevice = NULL;

	*OutLowerDevice = NULL;

	status = ObReferenceObjectByHandle(
		VolumeFileHandle,
		0,
		*IoFileObjectType,
		KernelMode,
		(PVOID*)&fileObject,
		NULL);
	if (!NT_SUCCESS(status))
		return status;

	if (fileObject->Vpb && fileObject->Vpb->RealDevice)
		volumeDevice = fileObject->Vpb->RealDevice;
	else
		volumeDevice = fileObject->DeviceObject;

	*OutLowerDevice = CdpFindTargetLowerDevice(DriverExt, volumeDevice);
	ObDereferenceObject(fileObject);

	if (!*OutLowerDevice)
		return STATUS_DEVICE_DOES_NOT_EXIST;
	return STATUS_SUCCESS;
}

static PCdp_VOLUME_HANDLE_ENTRY CdpAcquireJournalForSource(
	_In_ PCdp_DRIVER_EXTENSION DriverExt,
	_In_ PCdp_DEVICE_EXTENSION SourceExt)
{
	PCdp_VOLUME_HANDLE_ENTRY item = NULL;
	UINT64 journalHandleId;

	if (!SourceExt)
		return NULL;
	journalHandleId = SourceExt->JournalHandleId;
	if (journalHandleId == 0)
		return NULL;

	ExAcquireFastMutex(&DriverExt->VolumeHandleMutex);
	item = CdpLookupVolumeHandleLocked(DriverExt, journalHandleId);
	if (item && !item->Closing)
		InterlockedIncrement(&item->ReferenceCount);
	else
		item = NULL;
	ExReleaseFastMutex(&DriverExt->VolumeHandleMutex);
	return item;
}

static PCdp_DEVICE_EXTENSION CdpFindSourceByJournalHandle(
	_In_ PCdp_DRIVER_EXTENSION DriverExt,
	_In_ UINT64 JournalHandleId)
{
	KIRQL oldIrql;
	PLIST_ENTRY entry;
	PCdp_DEVICE_EXTENSION found = NULL;

	if (JournalHandleId == 0)
		return NULL;

	KeAcquireSpinLock(&DriverExt->DeviceObjectListLock, &oldIrql);
	for (entry = DriverExt->DeviceObjectListHead.Flink;
		entry != &DriverExt->DeviceObjectListHead;
		entry = entry->Flink)
	{
		PCdp_DEVICE_LIST_NODE node =
			CONTAINING_RECORD(entry, Cdp_DEVICE_LIST_NODE, Entry);
		PCdp_DEVICE_EXTENSION ext =
			(PCdp_DEVICE_EXTENSION)node->DeviceObject->DeviceExtension;
		if (ext && ext->JournalHandleId == JournalHandleId)
		{
			found = ext;
			break;
		}
	}
	KeReleaseSpinLock(&DriverExt->DeviceObjectListLock, oldIrql);
	return found;
}

static VOID CdpReleaseVolumeHandleEntry(_In_ PCdp_VOLUME_HANDLE_ENTRY Item)
{
	if (InterlockedDecrement(&Item->ReferenceCount) == 0)
		KeSetEvent(&Item->NoReferences, IO_NO_INCREMENT, FALSE);
}

static VOID CdpCloseVolumeHandleEntry(_In_ PCdp_VOLUME_HANDLE_ENTRY Item)
{
	CdpReleaseVolumeHandleEntry(Item); // Drop the list ownership reference.
	KeWaitForSingleObject(&Item->NoReferences, Executive, KernelMode, FALSE, NULL);
	if (Item->Journal.Mounted)
		CdpJournalClose(&Item->Journal);
	if (Item->FileHandle)
		ZwClose(Item->FileHandle);
	if (Item->MetadataLowerDeviceReference)
	{
		ObDereferenceObject(Item->MetadataLowerDeviceReference);
		Item->MetadataLowerDeviceReference = NULL;
	}
	cdpfree(Item);
}

VOID CdpCloseAllVolumeHandles(_In_ PCdp_DRIVER_EXTENSION DriverExt)
{
	CdpDisableAllCaptureSources(DriverExt);
	ExAcquireFastMutex(&DriverExt->VolumeHandleMutex);
	while (!IsListEmpty(&DriverExt->VolumeHandleList))
	{
		PLIST_ENTRY entry = RemoveHeadList(&DriverExt->VolumeHandleList);
		PCdp_VOLUME_HANDLE_ENTRY item = CONTAINING_RECORD(entry, Cdp_VOLUME_HANDLE_ENTRY, Entry);
		item->Closing = TRUE;
		CdpCloseVolumeHandleEntry(item);
	}
	ExReleaseFastMutex(&DriverExt->VolumeHandleMutex);
}

static NTSTATUS CdpOpenVolumeHandle(
	_In_ PCdp_DRIVER_EXTENSION DriverExt,
	_In_ const GUID* VolumeGuid,
	_Out_ PUINT64 OutHandleId)
{
	NTSTATUS Status;
	WCHAR path[96];
	UNICODE_STRING pathStr;
	OBJECT_ATTRIBUTES oa;
	IO_STATUS_BLOCK iosb;
	HANDLE fileHandle = NULL;
	PCdp_VOLUME_HANDLE_ENTRY item;
	PDEVICE_OBJECT volumeLower = NULL;
	PCdp_DEVICE_EXTENSION volumeExt = NULL;
	PCdp_DEVICE_EXTENSION diskExt = NULL;
	ULONG refreshedDiskNumber = 0;
	ULONG refreshedPartitionNumber = 0;
	UINT64 refreshedPartitionStart = 0;
	UINT64 refreshedPartitionSize = 0;
	UINT64 refreshedNextPartitionStart = 0;
	UINT64 refreshedNextPartitionSize = 0;
	ULONG refreshedNextPartitionNumber = 0;
	ULONG refreshedPartitionStyle = PARTITION_STYLE_RAW;
	ULONG refreshedMbrSignature = 0;
	GUID refreshedDiskGuid = { 0 };
	BOOLEAN refreshedHasNextPartition = FALSE;

	*OutHandleId = 0;

	if (KeGetCurrentIrql() != PASSIVE_LEVEL)
		return STATUS_INVALID_DEVICE_STATE;

	Status = CdpFormatVolumeNtPath(VolumeGuid, path, sizeof(path));
	if (!NT_SUCCESS(Status))
		return Status;

	RtlInitUnicodeString(&pathStr, path);
	InitializeObjectAttributes(&oa, &pathStr,
		OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE,
		NULL, NULL);

	/* This handle is now used only to resolve the volume identity and query its
	 * geometry. All source/journal data I/O goes through the DiskDrive lower
	 * device with an explicit partition base offset. Requesting GENERIC_WRITE
	 * here makes an otherwise valid mounted volume open fail with
	 * STATUS_ACCESS_DENIED. */
	Status = ZwCreateFile(
		&fileHandle,
		GENERIC_READ | SYNCHRONIZE,
		&oa,
		&iosb,
		NULL,
		FILE_ATTRIBUTE_NORMAL,
		FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
		FILE_OPEN,
		FILE_NON_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT | FILE_NO_INTERMEDIATE_BUFFERING,
		NULL,
		0);
	if (!NT_SUCCESS(Status))
	{
		Cdp_LOG("open volume failed 0x%08X path=%ws\n", Status, path);
		return Status;
	}

	item = (PCdp_VOLUME_HANDLE_ENTRY)cdpalloc(sizeof(*item));
	if (!item)
	{
		ZwClose(fileHandle);
		return STATUS_INSUFFICIENT_RESOURCES;
	}

	RtlZeroMemory(item, sizeof(*item));
	item->FileHandle = fileHandle;
	item->HandleId = (UINT64)InterlockedIncrement64(&DriverExt->VolumeHandleNextId);
	item->ReferenceCount = 1;
	item->VolumeGuid = *VolumeGuid;
	item->VolumeGuidValid = TRUE;
	KeInitializeEvent(&item->NoReferences, NotificationEvent, FALSE);

	Status = CdpResolveTargetLowerDevice(DriverExt, fileHandle, &volumeLower);
	if (!NT_SUCCESS(Status))
	{
		Cdp_LOG("resolve target lower device failed 0x%08X\n", Status);
		ZwClose(fileHandle);
		cdpfree(item);
		return Status;
	}
	item->VolumeLowerDevice = volumeLower;
	Status = CdpQueryVolumeGeometry(fileHandle, &item->PartitionSize, &item->SectorSize);
	if (!NT_SUCCESS(Status))
	{
		Cdp_LOG("query volume geometry failed 0x%08X\n", Status);
		ZwClose(fileHandle);
		cdpfree(item);
		return Status;
	}
	volumeExt = CdpFindSourceExtensionByGuid(DriverExt, VolumeGuid);
	if (!volumeExt)
	{
		/* A volume GUID is not guaranteed to be available while its
		 * START_DEVICE IRP is held.  CdpResolveTargetLowerDevice already
		 * resolved this opened volume through our attachment, so the exact
		 * lower-device pointer is an unambiguous same-boot fallback. */
		volumeExt = CdpFindVolumeExtensionByLowerDevice(
			DriverExt, volumeLower);
		if (volumeExt)
		{
			volumeExt->VolumeGuid = *VolumeGuid;
			volumeExt->VolumeGuidValid = TRUE;
			Cdp_LOG("[VOLUME-MAP] GUID cache miss recovered by lower-device mapping filter=%p lower=%p\n",
				volumeExt->FilterDeviceObject, volumeLower);
		}
	}
	if (!volumeExt)
	{
		Cdp_LOG("volume physical mapping unavailable guid/lower filter lookup failed lower=%p\n",
			volumeLower);
		ZwClose(fileHandle);
		cdpfree(item);
		return STATUS_DEVICE_NOT_READY;
	}
	/* Re-query the live partition stack for every configuration operation.
	 * A volume can have been shrunk since AddDevice/START_DEVICE; cached
	 * PartitionSize/PartitionNumber values are not safe for raw disk I/O. */
	Status = CdpQueryPhysicalPartitionLayout(
		volumeLower,
		&refreshedDiskNumber,
		&refreshedPartitionNumber,
		&refreshedPartitionStart,
		&refreshedPartitionSize,
		&refreshedHasNextPartition,
		&refreshedNextPartitionStart,
		&refreshedNextPartitionNumber,
		&refreshedNextPartitionSize,
		&refreshedPartitionStyle,
		&refreshedMbrSignature,
		&refreshedDiskGuid);
	if (!NT_SUCCESS(Status) || refreshedPartitionSize == 0)
	{
		Cdp_LOG("[CMD1-LAYOUT] live partition query failed status=0x%08X\n",
			Status);
		ZwClose(fileHandle);
		cdpfree(item);
		return NT_SUCCESS(Status) ? STATUS_DEVICE_NOT_READY : Status;
	}
	if (item->PartitionSize != refreshedPartitionSize)
	{
		Cdp_LOG("[PARTITION-RANGE] live volume length differs queried=%llu layout=%llu using=%llu disk=%lu part=%lu\n",
			item->PartitionSize,
			refreshedPartitionSize,
			item->PartitionSize < refreshedPartitionSize ?
				item->PartitionSize : refreshedPartitionSize,
			refreshedDiskNumber,
			refreshedPartitionNumber);
	}
	/* The smaller independently reported length is the only safe raw-I/O
	 * boundary while a partition-layout notification is still settling. */
	if (refreshedPartitionSize < item->PartitionSize)
		item->PartitionSize = refreshedPartitionSize;
	if (item->PartitionSize == 0 ||
		refreshedPartitionStart > MAXUINT64 - item->PartitionSize)
	{
		Cdp_LOG("[CMD1-LAYOUT] invalid live range start=%llu size=%llu\n",
			refreshedPartitionStart, item->PartitionSize);
		ZwClose(fileHandle);
		cdpfree(item);
		return STATUS_INTEGER_OVERFLOW;
	}

	/* Configuration is serialized and protection is not active for this
	 * source, so publish the refreshed identity before binding the disk path. */
	volumeExt->DiskNumber = refreshedDiskNumber;
	volumeExt->PartitionNumber = refreshedPartitionNumber;
	volumeExt->PartitionStart = refreshedPartitionStart;
	volumeExt->PartitionSize = item->PartitionSize;
	volumeExt->HasNextPartition = refreshedHasNextPartition;
	volumeExt->NextPartitionStart = refreshedNextPartitionStart;
	volumeExt->NextPartitionNumber = refreshedNextPartitionNumber;
	volumeExt->NextPartitionSize = refreshedNextPartitionSize;
	volumeExt->DiskPartitionStyle = refreshedPartitionStyle;
	volumeExt->MbrSignature = refreshedMbrSignature;
	volumeExt->DiskGuid = refreshedDiskGuid;
	volumeExt->DiskLayoutValid = TRUE;

	diskExt = CdpFindDiskExtensionByNumber(DriverExt, refreshedDiskNumber);
	if (!diskExt || !diskExt->LowerDeviceObject)
	{
		Cdp_LOG("disk upper mapping unavailable disk=%lu\n",
			refreshedDiskNumber);
		ZwClose(fileHandle);
		cdpfree(item);
		return STATUS_DEVICE_NOT_READY;
	}
	item->TargetLowerDevice = diskExt->LowerDeviceObject;
	item->TargetBaseOffset = refreshedPartitionStart;
	item->DiskNumber = refreshedDiskNumber;
	item->PartitionNumber = refreshedPartitionNumber;

	ExAcquireFastMutex(&DriverExt->VolumeHandleMutex);
	InsertTailList(&DriverExt->VolumeHandleList, &item->Entry);
	ExReleaseFastMutex(&DriverExt->VolumeHandleMutex);

	*OutHandleId = item->HandleId;
	Cdp_DBG("opened volume handle id=%llu diskLower=%p base=%llu\n",
		item->HandleId, item->TargetLowerDevice, item->TargetBaseOffset);
	CdpDbgGuid("  Guid", VolumeGuid);
	return STATUS_SUCCESS;
}

static NTSTATUS CdpCloseVolumeHandle(
	_In_ PCdp_DRIVER_EXTENSION DriverExt,
	_In_ UINT64 HandleId)
{
	PCdp_VOLUME_HANDLE_ENTRY item;
	PCdp_DEVICE_EXTENSION pairedSource = NULL;

	ExAcquireFastMutex(&DriverExt->VolumeHandleMutex);
	item = CdpLookupVolumeHandleLocked(DriverExt, HandleId);
	if (item)
	{
		RemoveEntryList(&item->Entry);
		item->Closing = TRUE;
	}
	ExReleaseFastMutex(&DriverExt->VolumeHandleMutex);

	if (!item)
		return STATUS_NOT_FOUND;

	pairedSource = CdpFindSourceByJournalHandle(DriverExt, HandleId);
	if (pairedSource)
	{
		NTSTATUS drainStatus = CdpDrainAndDisableCapture(pairedSource);
		if (!NT_SUCCESS(drainStatus))
		{
			/* Restore the handle-list ownership: graceful close failed before
			 * the Journal/Core lifetime was torn down. */
			ExAcquireFastMutex(&DriverExt->VolumeHandleMutex);
			item->Closing = FALSE;
			InsertTailList(&DriverExt->VolumeHandleList, &item->Entry);
			ExReleaseFastMutex(&DriverExt->VolumeHandleMutex);
			return drainStatus;
		}
		pairedSource->JournalHandleId = 0;
		CdpDisableAndDestroyCapture(pairedSource);
		if (item->Journal.Mounted)
		{
			NTSTATUS invStatus = CdpJournalInvalidate(&item->Journal);
			Cdp_LOG("[COW] stop: invalidate journal status=0x%08X\n", invStatus);
		}
	}
	CdpCloseVolumeHandleEntry(item);
	Cdp_DBG("closed volume handle id=%llu\n", HandleId);
	return STATUS_SUCCESS;
}

static VOID CdpDisableAllCaptureSources(_In_ PCdp_DRIVER_EXTENSION DriverExt)
{
	// Do not destroy Core while holding DeviceObjectListLock: a spin lock raises
	// IRQL to DISPATCH_LEVEL and Core destruction can free locks/memory.  Take a
	// reference to one matching filter device, release the spin lock, then
	// quiesce it.  Re-scan until no capture Core remains.
	for (;;)
	{
		KIRQL oldIrql;
		PLIST_ENTRY entry;
		PDEVICE_OBJECT filterDevice = NULL;

		KeAcquireSpinLock(&DriverExt->DeviceObjectListLock, &oldIrql);
		for (entry = DriverExt->DeviceObjectListHead.Flink;
			entry != &DriverExt->DeviceObjectListHead;
			entry = entry->Flink)
		{
			PCdp_DEVICE_LIST_NODE node =
				CONTAINING_RECORD(entry, Cdp_DEVICE_LIST_NODE, Entry);
			PCdp_DEVICE_EXTENSION ext =
				(PCdp_DEVICE_EXTENSION)node->DeviceObject->DeviceExtension;
			if (ext && (InterlockedCompareExchange(&ext->CaptureEnabled, 0, 0) != 0 ||
				ext->Core != NULL))
			{
				filterDevice = node->DeviceObject;
				ObReferenceObject(filterDevice);
				break;
			}
		}
		KeReleaseSpinLock(&DriverExt->DeviceObjectListLock, oldIrql);

		if (!filterDevice)
			break;

		CdpDisableAndDestroyCapture(
			(PCdp_DEVICE_EXTENSION)filterDevice->DeviceExtension);
		ObDereferenceObject(filterDevice);
	}
}

static PCdp_DEVICE_EXTENSION CdpFindSourceExtensionByGuid(
	_In_ PCdp_DRIVER_EXTENSION DriverExt,
	_In_ const GUID* VolumeGuid)
{
	KIRQL oldIrql;
	PLIST_ENTRY entry;
	PCdp_DEVICE_EXTENSION found = NULL;

	KeAcquireSpinLock(&DriverExt->DeviceObjectListLock, &oldIrql);
	for (entry = DriverExt->DeviceObjectListHead.Flink;
		entry != &DriverExt->DeviceObjectListHead;
		entry = entry->Flink)
	{
		PCdp_DEVICE_LIST_NODE node =
			CONTAINING_RECORD(entry, Cdp_DEVICE_LIST_NODE, Entry);
		PCdp_DEVICE_EXTENSION ext =
			(PCdp_DEVICE_EXTENSION)node->DeviceObject->DeviceExtension;
		if (ext && (ext->DeviceKind == Cdp_DEVICE_KIND_VOLUME ||
			ext->DeviceKind == Cdp_DEVICE_KIND_DISK) &&
			ext->VolumeGuidValid &&
			RtlCompareMemory(
				&ext->VolumeGuid,
				VolumeGuid,
				sizeof(GUID)) == sizeof(GUID))
		{
			found = ext;
			if (ext->Core && InterlockedCompareExchange(
					&ext->CaptureEnabled, 0, 0) != 0)
				break;
		}
	}
	KeReleaseSpinLock(&DriverExt->DeviceObjectListLock, oldIrql);
	return found;
}

static PCdp_DEVICE_EXTENSION CdpFindVolumeExtensionByLowerDevice(
	_In_ PCdp_DRIVER_EXTENSION DriverExt,
	_In_ PDEVICE_OBJECT LowerDevice)
{
	KIRQL oldIrql;
	PLIST_ENTRY entry;
	PCdp_DEVICE_EXTENSION found = NULL;

	if (!DriverExt || !LowerDevice)
		return NULL;
	KeAcquireSpinLock(&DriverExt->DeviceObjectListLock, &oldIrql);
	for (entry = DriverExt->DeviceObjectListHead.Flink;
		entry != &DriverExt->DeviceObjectListHead;
		entry = entry->Flink)
	{
		PCdp_DEVICE_LIST_NODE node =
			CONTAINING_RECORD(entry, Cdp_DEVICE_LIST_NODE, Entry);
		PCdp_DEVICE_EXTENSION ext =
			(PCdp_DEVICE_EXTENSION)node->DeviceObject->DeviceExtension;
		if (ext && ext->DeviceKind == Cdp_DEVICE_KIND_VOLUME &&
			ext->LowerDeviceObject == LowerDevice)
		{
			found = ext;
			break;
		}
	}
	KeReleaseSpinLock(&DriverExt->DeviceObjectListLock, oldIrql);
	return found;
}

static PDEVICE_OBJECT CdpReferenceVolumeLowerByPhysicalRange(
	_In_ PCdp_DRIVER_EXTENSION DriverExt,
	_In_ ULONG DiskNumber,
	_In_ UINT64 PartitionStart,
	_In_ UINT64 PartitionSize)
{
	KIRQL oldIrql;
	PLIST_ENTRY entry;
	PDEVICE_OBJECT lower = NULL;

	if (!DriverExt || PartitionSize == 0)
		return NULL;
	KeAcquireSpinLock(&DriverExt->DeviceObjectListLock, &oldIrql);
	for (entry = DriverExt->DeviceObjectListHead.Flink;
		entry != &DriverExt->DeviceObjectListHead;
		entry = entry->Flink)
	{
		PCdp_DEVICE_LIST_NODE node =
			CONTAINING_RECORD(entry, Cdp_DEVICE_LIST_NODE, Entry);
		PCdp_DEVICE_EXTENSION ext =
			(PCdp_DEVICE_EXTENSION)node->DeviceObject->DeviceExtension;
		if (ext && ext->DeviceKind == Cdp_DEVICE_KIND_VOLUME &&
			InterlockedCompareExchange(&ext->Started, 0, 0) != 0 &&
			ext->DiskLayoutValid && ext->LowerDeviceObject &&
			ext->DiskNumber == DiskNumber &&
			ext->PartitionStart == PartitionStart &&
			ext->PartitionSize >= PartitionSize)
		{
			lower = ext->LowerDeviceObject;
			ObReferenceObject(lower);
			break;
		}
	}
	KeReleaseSpinLock(&DriverExt->DeviceObjectListLock, oldIrql);
	return lower;
}

static PCdp_DEVICE_EXTENSION CdpFindDiskExtensionByNumber(
	_In_ PCdp_DRIVER_EXTENSION DriverExt,
	_In_ ULONG DiskNumber)
{
	KIRQL oldIrql;
	PLIST_ENTRY entry;
	PCdp_DEVICE_EXTENSION found = NULL;

	KeAcquireSpinLock(&DriverExt->DeviceObjectListLock, &oldIrql);
	for (entry = DriverExt->DeviceObjectListHead.Flink;
		entry != &DriverExt->DeviceObjectListHead;
		entry = entry->Flink)
	{
		PCdp_DEVICE_LIST_NODE node =
			CONTAINING_RECORD(entry, Cdp_DEVICE_LIST_NODE, Entry);
		PCdp_DEVICE_EXTENSION ext =
			(PCdp_DEVICE_EXTENSION)node->DeviceObject->DeviceExtension;
		if (ext && ext->DeviceKind == Cdp_DEVICE_KIND_DISK &&
			InterlockedCompareExchange(&ext->Started, 0, 0) != 0 &&
			ext->DiskLayoutValid && ext->DiskNumber == DiskNumber)
		{
			found = ext;
			break;
		}
	}
	KeReleaseSpinLock(&DriverExt->DeviceObjectListLock, oldIrql);
	return found;
}

static BOOLEAN CdpControlHandleAuthorized(
	_In_ PIRP Irp,
	_In_ PCdp_DRIVER_EXTENSION DriverExt,
	_In_ const GUID* SourceVolumeGuid)
{
	PIO_STACK_LOCATION irpSp = IoGetCurrentIrpStackLocation(Irp);
	PCdp_CONTROL_FILE_CONTEXT context =
		(PCdp_CONTROL_FILE_CONTEXT)irpSp->FileObject->FsContext;
	Cdp_CREDENTIAL_DESCRIPTOR credential;

	if (!context || !context->Authenticated)
	{
		Cdp_LOG("[AUTH-CHECK-FAIL] reason=handle-not-authenticated\n");
		return FALSE;
	}
	if (KeQueryInterruptTime() >= context->ExpiresAt100ns)
	{
		RtlSecureZeroMemory(context, sizeof(*context));
		Cdp_LOG("[AUTH-CHECK-FAIL] reason=authorization-expired\n");
		return FALSE;
	}
	/* Authentication is deliberately global: IOCTL_Cdp_AUTHENTICATE verifies
	 * the shared protection credential, and all journals carry that credential.
	 * Do not conflate authentication with whether auto discovery happened to
	 * bind this particular source during the current boot. */
	if (!NT_SUCCESS(CdpGetSharedCredential(DriverExt, &credential, NULL)))
	{
		Cdp_LOG("[AUTH-CHECK-FAIL] reason=shared-credential-unavailable\n");
		return FALSE;
	}
	if (RtlCompareMemory(&context->CredentialId, &credential.CredentialId,
			sizeof(GUID)) != sizeof(GUID) ||
		context->AuthEpoch != credential.AuthEpoch)
	{
		Cdp_LOG("[AUTH-CHECK-FAIL] reason=credential-epoch-mismatch\n");
		return FALSE;
	}
	/* Keep an active privileged operation alive without extending an idle
	 * control handle indefinitely.  In particular, e -> mount work -> r
	 * must not lose authorization merely because the middle step is slow. */
	context->ExpiresAt100ns = KeQueryInterruptTime() +
		60ULL * 60ULL * 10000000ULL;
	UNREFERENCED_PARAMETER(SourceVolumeGuid);
	return TRUE;
}

static UINT64 CdpFindJournalHandleBySourceGuid(
	_In_ PCdp_DRIVER_EXTENSION DriverExt,
	_In_ const GUID* SourceVolumeGuid)
{
	PLIST_ENTRY entry;
	UINT64 handleId = 0;

	if (!DriverExt || !SourceVolumeGuid)
		return 0;
	ExAcquireFastMutex(&DriverExt->VolumeHandleMutex);
	for (entry = DriverExt->VolumeHandleList.Flink;
		entry != &DriverExt->VolumeHandleList;
		entry = entry->Flink)
	{
		PCdp_VOLUME_HANDLE_ENTRY item =
			CONTAINING_RECORD(entry, Cdp_VOLUME_HANDLE_ENTRY, Entry);
		if (!item->Closing && item->Journal.Mounted &&
			RtlCompareMemory(&item->Journal.SourceVolumeGuid,
				SourceVolumeGuid, sizeof(GUID)) == sizeof(GUID))
		{
			handleId = item->HandleId;
			break;
		}
	}
	ExReleaseFastMutex(&DriverExt->VolumeHandleMutex);
	return handleId;
}

static NTSTATUS CdpConfigureCaptureInternal(
	_In_ PCdp_DRIVER_EXTENSION DriverExt,
	_In_ const GUID* SourceVolumeGuid,
	_In_ const GUID* JournalPartitionGuid,
	_In_ BOOLEAN FormatJournal,
	_In_opt_ const Cdp_CREDENTIAL_DESCRIPTOR* Credential,
	_Out_ PUINT64 JournalHandleId)
{
	UINT64 sourceHandleId = 0;
	UINT64 journalHandleId = 0;
	UINT64 sourcePartitionSize = 0;
	UINT64 sourcePartitionStart = 0;
	UINT64 sourceNextPartitionStart = 0;
	ULONG sourceDiskNumber = MAXULONG;
	ULONG sourcePartitionNumber = 0;
	ULONG sourceSectorSize = 512;
	BOOLEAN sourceHasNextPartition = FALSE;
	PDEVICE_OBJECT sourceLower = NULL;
	PCdp_DEVICE_EXTENSION sourceExt = NULL;
	PCdp_VOLUME_HANDLE_ENTRY sourceEntry;
	PCdp_VOLUME_HANDLE_ENTRY journalEntry;
	NTSTATUS status;

	*JournalHandleId = 0;
	Cdp_DBG("[COW] configure begin format=%u\n", FormatJournal ? 1u : 0u);
	CdpDbgGuid("[COW] source", SourceVolumeGuid);
	CdpDbgGuid("[COW] journal", JournalPartitionGuid);
	if (RtlCompareMemory(
		SourceVolumeGuid,
		JournalPartitionGuid,
		sizeof(GUID)) == sizeof(GUID))
	{
		return STATUS_INVALID_PARAMETER;
	}
	status = CdpOpenVolumeHandle(DriverExt, SourceVolumeGuid, &sourceHandleId);
	if (!NT_SUCCESS(status))
	{
		Cdp_LOG("[CMD1-STAGE] source volume open failed status=0x%08X\n",
			status);
		return status;
	}
	ExAcquireFastMutex(&DriverExt->VolumeHandleMutex);
	sourceEntry = CdpLookupVolumeHandleLocked(DriverExt, sourceHandleId);
	if (sourceEntry)
	{
		sourceLower = sourceEntry->TargetLowerDevice;
		sourcePartitionSize = sourceEntry->PartitionSize;
		sourcePartitionStart = sourceEntry->TargetBaseOffset;
		sourceDiskNumber = sourceEntry->DiskNumber;
		sourcePartitionNumber = sourceEntry->PartitionNumber;
		sourceSectorSize = sourceEntry->SectorSize;
	}
	ExReleaseFastMutex(&DriverExt->VolumeHandleMutex);
	sourceExt = CdpFindSourceExtensionByGuid(DriverExt, SourceVolumeGuid);
	if (sourceExt)
	{
		sourceHasNextPartition = sourceExt->HasNextPartition;
		sourceNextPartitionStart = sourceExt->NextPartitionStart;
	}
	(void)CdpCloseVolumeHandle(DriverExt, sourceHandleId);
	if (!sourceExt)
	{
		Cdp_LOG("[CMD1-STAGE] source extension lookup failed\n");
		return STATUS_DEVICE_DOES_NOT_EXIST;
	}
	if (InterlockedCompareExchange(&sourceExt->CaptureEnabled, 0, 0) != 0 ||
		sourceExt->Core != NULL ||
		sourceExt->JournalHandleId != 0)
	{
		return STATUS_DEVICE_BUSY;
	}
	CdpDisableAndDestroyCapture(sourceExt);

	status = CdpOpenVolumeHandle(DriverExt, JournalPartitionGuid, &journalHandleId);
	if (!NT_SUCCESS(status))
	{
		Cdp_LOG("[CMD1-STAGE] journal volume open failed status=0x%08X\n",
			status);
		return status;
	}

	ExAcquireFastMutex(&DriverExt->VolumeHandleMutex);
	journalEntry = CdpLookupVolumeHandleLocked(DriverExt, journalHandleId);
	if (journalEntry)
		InterlockedIncrement(&journalEntry->ReferenceCount);
	ExReleaseFastMutex(&DriverExt->VolumeHandleMutex);
	if (!journalEntry)
	{
		(void)CdpCloseVolumeHandle(DriverExt, journalHandleId);
		return STATUS_INVALID_HANDLE;
	}

	/* Final fail-closed validation after both GUIDs have been resolved against
	 * the current physical layout.  The GUI may leave an alignment gap after
	 * shrinking the source, but the journal must still be the first allocated
	 * partition following it on the same disk. */
	if (sourceDiskNumber != journalEntry->DiskNumber ||
		sourceLower != journalEntry->TargetLowerDevice ||
		sourceSectorSize != journalEntry->SectorSize ||
		sourcePartitionSize == 0 || journalEntry->PartitionSize == 0 ||
		sourcePartitionStart > MAXUINT64 - sourcePartitionSize ||
		journalEntry->TargetBaseOffset >
			MAXUINT64 - journalEntry->PartitionSize ||
		sourcePartitionStart + sourcePartitionSize >
			journalEntry->TargetBaseOffset ||
		!sourceHasNextPartition ||
		sourceNextPartitionStart != journalEntry->TargetBaseOffset)
	{
		Cdp_LOG("[CMD1-LAYOUT] reject source disk=%lu part=%lu start=%llu size=%llu next=%llu hasNext=%u; journal disk=%lu part=%lu start=%llu size=%llu sector=%lu/%lu sameLower=%u\n",
			sourceDiskNumber,
			sourcePartitionNumber,
			sourcePartitionStart,
			sourcePartitionSize,
			sourceNextPartitionStart,
			sourceHasNextPartition ? 1u : 0u,
			journalEntry->DiskNumber,
			journalEntry->PartitionNumber,
			journalEntry->TargetBaseOffset,
			journalEntry->PartitionSize,
			sourceSectorSize,
			journalEntry->SectorSize,
			sourceLower == journalEntry->TargetLowerDevice ? 1u : 0u);
		CdpReleaseVolumeHandleEntry(journalEntry);
		(void)CdpCloseVolumeHandle(DriverExt, journalHandleId);
		return STATUS_DEVICE_CONFIGURATION_ERROR;
	}
	Cdp_LOG("[CMD1-LAYOUT] accepted source disk=%lu part=%lu start=%llu size=%llu end=%llu; journal part=%lu start=%llu size=%llu\n",
		sourceDiskNumber,
		sourcePartitionNumber,
		sourcePartitionStart,
		sourcePartitionSize,
		sourcePartitionStart + sourcePartitionSize,
		journalEntry->PartitionNumber,
		journalEntry->TargetBaseOffset,
		journalEntry->PartitionSize);

	Cdp_DBG("[JOURNAL-RAW] backend lowerDevice=%p\n",
		journalEntry->TargetLowerDevice);

	CdpJournalInitialize(
		&journalEntry->Journal,
		journalEntry->TargetLowerDevice,
		NULL,
		journalEntry->TargetBaseOffset,
		journalEntry->PartitionSize,
		journalEntry->SectorSize,
		SourceVolumeGuid);
	CdpJournalSetPhysicalLayout(
		&journalEntry->Journal,
		sourceExt->DiskPartitionStyle,
		sourceExt->MbrSignature,
		&sourceExt->DiskGuid,
		sourcePartitionStart,
		sourcePartitionSize,
		journalEntry->TargetBaseOffset,
		journalEntry->PartitionSize);
	if (FormatJournal)
	{
		if (!Credential)
		{
			CdpReleaseVolumeHandleEntry(journalEntry);
			(void)CdpCloseVolumeHandle(DriverExt, journalHandleId);
			return STATUS_PASSWORD_RESTRICTION;
		}
		status = CdpJournalSetCredential(&journalEntry->Journal, Credential);
		if (!NT_SUCCESS(status))
		{
			CdpReleaseVolumeHandleEntry(journalEntry);
			(void)CdpCloseVolumeHandle(DriverExt, journalHandleId);
			return status;
		}
	}
	status = FormatJournal ?
		CdpJournalFormat(&journalEntry->Journal) :
		CdpJournalMount(&journalEntry->Journal);
	if (NT_SUCCESS(status) && !journalEntry->Journal.CredentialConfigured)
		status = STATUS_PASSWORD_RESTRICTION;
	if (NT_SUCCESS(status) && !FormatJournal &&
		RtlCompareMemory(
			&journalEntry->Journal.SourceVolumeGuid,
			SourceVolumeGuid,
			sizeof(GUID)) != sizeof(GUID))
	{
		status = STATUS_OBJECT_TYPE_MISMATCH;
	}
	if (!NT_SUCCESS(status))
	{
		Cdp_LOG("[CMD1-STAGE] journal %s failed status=0x%08X base=%llu size=%llu\n",
			FormatJournal ? "format" : "mount",
			status,
			journalEntry->TargetBaseOffset,
			journalEntry->PartitionSize);
		CdpReleaseVolumeHandleEntry(journalEntry);
		(void)CdpCloseVolumeHandle(DriverExt, journalHandleId);
		return status;
	}

	sourceExt->VolumeGuid = *SourceVolumeGuid;
	sourceExt->VolumeGuidValid = TRUE;
	sourceExt->SectorSize = sourceSectorSize;
	sourceExt->PartitionSize = sourcePartitionSize;
	sourceExt->JournalHandleId = journalHandleId;

	{
		PCdp_STORE sourceStore = NULL;
		PCdp_DEVICE_EXTENSION sourceDisk =
			CdpFindDiskExtensionByNumber(DriverExt, sourceExt->DiskNumber);
		status = sourceDisk ?
			CdpDevStoreCreateAbsoluteRange(
			sourceDisk->LowerDeviceObject,
			sourceExt->PartitionStart,
			sourcePartitionSize,
			sourceSectorSize,
			&sourceStore) : STATUS_DEVICE_NOT_READY;
		if (NT_SUCCESS(status))
		{
			status = CdpCoreBind(
				sourceStore,
				&journalEntry->Journal,
				SourceVolumeGuid,
				&sourceExt->Core);
		}
		if (NT_SUCCESS(status))
			status = CdpValidateMountedSourceRecordRanges(
				sourceExt, FormatJournal ? "manual-format" : "manual-mount");
		if (!NT_SUCCESS(status))
		{
			if (sourceExt->Core)
			{
				CdpCoreDestroy(sourceExt->Core);
				sourceExt->Core = NULL;
				sourceStore = NULL;
			}
			if (sourceStore)
				CdpDevStoreDestroy(sourceStore);
			CdpReleaseVolumeHandleEntry(journalEntry);
			(void)CdpCloseVolumeHandle(DriverExt, journalHandleId);
			return status;
		}
	}
	/* Transfer the configuration reference to the source for the lifetime of
	 * protection.  The redirect fast path must not take VolumeHandleMutex. */
	NT_ASSERT(sourceExt->RedirectJournalEntry == NULL);
	sourceExt->RedirectJournalEntry = journalEntry;
	status = CdpValidateProtectionObjectGraph(
		DriverExt, sourceExt, "manual-before-enable");
	if (!NT_SUCCESS(status))
	{
		sourceExt->JournalHandleId = 0;
		CdpDisableAndDestroyCapture(sourceExt);
		(void)CdpCloseVolumeHandle(DriverExt, journalHandleId);
		return status;
	}

	CdpAuditProtectedReadReset(sourceExt);
	InterlockedExchange(&sourceExt->DiskIoAccepting, 1);
	InterlockedExchange(&sourceExt->ProtectionStateValidated, 1);
	InterlockedExchange(&sourceExt->CaptureEnabled, 1);
	Cdp_LOG("[PROTECTED-READ-AUDIT] reset protection enabled sourceExt=%p\n",
		sourceExt);
	Cdp_LOG("[PROTECTION-ACTIVE] protection enabled immediately; writes redirect to journal and MetaTree reads are active\n");

	*JournalHandleId = journalHandleId;
	Cdp_LOG("[COW] configured journalHandle=%llu size=%llu sector=%lu sourceExt=%p\n",
		journalHandleId,
		journalEntry->PartitionSize,
		journalEntry->SectorSize,
		sourceExt);
	return STATUS_SUCCESS;
}

static NTSTATUS CdpConfigureCapture(
	_In_ PCdp_DRIVER_EXTENSION DriverExt,
	_In_ const GUID* SourceVolumeGuid,
	_In_ const GUID* JournalPartitionGuid,
	_In_ BOOLEAN FormatJournal,
	_In_opt_ const Cdp_CREDENTIAL_DESCRIPTOR* Credential,
	_Out_ PUINT64 JournalHandleId)
{
	NTSTATUS status;

	status = KeWaitForSingleObject(
		&DriverExt->CaptureConfigMutex,
		Executive,
		KernelMode,
		FALSE,
		NULL);
	if (!NT_SUCCESS(status))
		return status;
	status = CdpConfigureCaptureInternal(
		DriverExt,
		SourceVolumeGuid,
		JournalPartitionGuid,
		FormatJournal,
		Credential,
		JournalHandleId);
	KeReleaseMutex(&DriverExt->CaptureConfigMutex, FALSE);
	return status;
}

static BOOLEAN CdpGuidIsZero(_In_ const GUID* Guid)
{
	static const GUID zeroGuid = { 0 };
	return RtlCompareMemory(Guid, &zeroGuid, sizeof(GUID)) == sizeof(GUID);
}

static NTSTATUS CdpActivateAutoJournal(
	_In_ PCdp_DRIVER_EXTENSION DriverExt,
	_In_ UINT64 JournalHandleId,
	_Inout_ PCdp_VOLUME_HANDLE_ENTRY JournalEntry)
{
	UINT64 sourcePartitionSize = 0;
	UINT64 livePartitionSize = 0;
	UINT64 livePartitionStart = 0;
	UINT64 liveNextPartitionStart = 0;
	ULONG sourceSectorSize = 0;
	ULONG liveDiskNumber = 0;
	ULONG livePartitionNumber = 0;
	BOOLEAN liveHasNextPartition = FALSE;
	PCdp_DEVICE_EXTENSION sourceExt;
	PCdp_DEVICE_EXTENSION sourceDisk;
	PCdp_STORE sourceStore = NULL;
	GUID sourceGuid = JournalEntry->Journal.SourceVolumeGuid;
	NTSTATUS status;

	if (CdpGuidIsZero(&sourceGuid))
		return STATUS_INVALID_PARAMETER;

	sourceExt = CdpFindSourceExtensionByGuid(DriverExt, &sourceGuid);
	if (!sourceExt ||
		InterlockedCompareExchange(&sourceExt->Started, 0, 0) == 0 ||
		!sourceExt->VolumeGuidValid ||
		!sourceExt->LowerDeviceObject)
	{
		return STATUS_DEVICE_DOES_NOT_EXIST;
	}
	if (InterlockedCompareExchange(&sourceExt->CaptureEnabled, 0, 0) != 0 ||
		sourceExt->Core != NULL ||
		sourceExt->JournalHandleId != 0)
	{
		return STATUS_DEVICE_BUSY;
	}

	/* Automatic boot activation must use the same fail-closed live-layout
	 * contract as CMD1.  Do not trust values cached before a GUI shrink or a
	 * partition re-enumeration. */
	status = CdpQueryPhysicalPartitionLayout(
		sourceExt->LowerDeviceObject,
		&liveDiskNumber,
		&livePartitionNumber,
		&livePartitionStart,
		&livePartitionSize,
		&liveHasNextPartition,
		&liveNextPartitionStart,
		&sourceExt->NextPartitionNumber,
		&sourceExt->NextPartitionSize,
		&sourceExt->DiskPartitionStyle,
		&sourceExt->MbrSignature,
		&sourceExt->DiskGuid);
	if (!NT_SUCCESS(status) || livePartitionSize == 0)
	{
		Cdp_LOG("[AUTO-LAYOUT] source refresh failed status=0x%08X\n",
			status);
		return NT_SUCCESS(status) ? STATUS_DEVICE_NOT_READY : status;
	}

	status = CdpQueryDeviceGeometry(
		sourceExt->LowerDeviceObject,
		&sourcePartitionSize,
		&sourceSectorSize);
	if (!NT_SUCCESS(status))
		return status;
	if (sourcePartitionSize != livePartitionSize)
	{
		Cdp_LOG("[PARTITION-RANGE] auto source length differs queried=%llu live=%llu using=%llu disk=%lu part=%lu\n",
			sourcePartitionSize,
			livePartitionSize,
			sourcePartitionSize < livePartitionSize ?
				sourcePartitionSize : livePartitionSize,
			liveDiskNumber,
			livePartitionNumber);
	}
	if (livePartitionSize < sourcePartitionSize)
		sourcePartitionSize = livePartitionSize;
	sourceDisk = CdpFindDiskExtensionByNumber(DriverExt, liveDiskNumber);
	if (!sourceDisk || !sourceDisk->LowerDeviceObject ||
		livePartitionStart > MAXUINT64 - sourcePartitionSize ||
		JournalEntry->TargetBaseOffset >
			MAXUINT64 - JournalEntry->PartitionSize ||
		liveDiskNumber != JournalEntry->DiskNumber ||
		sourceDisk->LowerDeviceObject != JournalEntry->TargetLowerDevice ||
		sourceSectorSize != JournalEntry->SectorSize ||
		livePartitionStart + sourcePartitionSize >
			JournalEntry->TargetBaseOffset ||
		!liveHasNextPartition ||
		liveNextPartitionStart != JournalEntry->TargetBaseOffset)
	{
		Cdp_LOG("[AUTO-LAYOUT] reject source disk=%lu part=%lu start=%llu size=%llu next=%llu hasNext=%u; journal disk=%lu part=%lu start=%llu size=%llu sector=%lu/%lu sameLower=%u\n",
			liveDiskNumber,
			livePartitionNumber,
			livePartitionStart,
			sourcePartitionSize,
			liveNextPartitionStart,
			liveHasNextPartition ? 1u : 0u,
			JournalEntry->DiskNumber,
			JournalEntry->PartitionNumber,
			JournalEntry->TargetBaseOffset,
			JournalEntry->PartitionSize,
			sourceSectorSize,
			JournalEntry->SectorSize,
			(sourceDisk && sourceDisk->LowerDeviceObject ==
				JournalEntry->TargetLowerDevice) ? 1u : 0u);
		return STATUS_DEVICE_CONFIGURATION_ERROR;
	}
	sourceExt->DiskNumber = liveDiskNumber;
	sourceExt->PartitionNumber = livePartitionNumber;
	sourceExt->PartitionStart = livePartitionStart;
	sourceExt->PartitionSize = sourcePartitionSize;
	sourceExt->HasNextPartition = liveHasNextPartition;
	sourceExt->NextPartitionStart = liveNextPartitionStart;
	sourceExt->DiskLayoutValid = TRUE;
	Cdp_LOG("[AUTO-LAYOUT] accepted source disk=%lu part=%lu start=%llu size=%llu end=%llu; journal part=%lu start=%llu size=%llu\n",
		liveDiskNumber,
		livePartitionNumber,
		livePartitionStart,
		sourcePartitionSize,
		livePartitionStart + sourcePartitionSize,
		JournalEntry->PartitionNumber,
		JournalEntry->TargetBaseOffset,
		JournalEntry->PartitionSize);

	sourceExt->VolumeGuid = sourceGuid;
	sourceExt->VolumeGuidValid = TRUE;
	sourceExt->SectorSize = sourceSectorSize;
	{
		status = sourceDisk ?
			CdpDevStoreCreateAbsoluteRange(
			sourceDisk->LowerDeviceObject,
			sourceExt->PartitionStart,
			sourcePartitionSize,
			sourceSectorSize,
			&sourceStore) : STATUS_DEVICE_NOT_READY;
	}
	if (NT_SUCCESS(status))
	{
		status = CdpCoreBind(
			sourceStore,
			&JournalEntry->Journal,
			&sourceGuid,
			&sourceExt->Core);
	}
	if (NT_SUCCESS(status))
		status = CdpValidateMountedSourceRecordRanges(
			sourceExt, "auto-mount");
	if (!NT_SUCCESS(status))
	{
		if (sourceExt->Core)
		{
			CdpCoreDestroy(sourceExt->Core);
			sourceExt->Core = NULL;
			sourceStore = NULL;
		}
		if (sourceStore)
			CdpDevStoreDestroy(sourceStore);
		return status;
	}
	sourceExt->JournalHandleId = JournalHandleId;
	/* The caller drops its discovery reference after return, so add the one
	 * reference owned by this protection session. */
	InterlockedIncrement(&JournalEntry->ReferenceCount);
	NT_ASSERT(sourceExt->RedirectJournalEntry == NULL);
	sourceExt->RedirectJournalEntry = JournalEntry;
	status = CdpValidateProtectionObjectGraph(
		DriverExt, sourceExt, "auto-before-enable");
	if (!NT_SUCCESS(status))
	{
		sourceExt->JournalHandleId = 0;
		CdpDisableAndDestroyCapture(sourceExt);
		return status;
	}
	CdpAuditProtectedReadReset(sourceExt);
	InterlockedExchange(&sourceExt->DiskIoAccepting, 1);
	InterlockedExchange(&sourceExt->ProtectionStateValidated, 1);
	InterlockedExchange(&sourceExt->CaptureEnabled, 1);
	Cdp_LOG("[PROTECTED-READ-AUDIT] reset auto protection enabled sourceExt=%p\n",
		sourceExt);
	Cdp_LOG("[DISK-UPPER] auto protection enabled: writes redirect to journal; MetaTree current-view reads active\n");
	Cdp_LOG("[AUTO-CDP] enabled journalHandle=%llu sourceExt=%p\n",
		JournalHandleId, sourceExt);
	CdpDbgGuid("[AUTO-CDP] source", &sourceGuid);
	return STATUS_SUCCESS;
}

static BOOLEAN CdpGuidIsEqual(_In_ const GUID* A, _In_ const GUID* B)
{
	return RtlCompareMemory(A, B, sizeof(GUID)) == sizeof(GUID);
}

#define Cdp_PARTITION_LAYOUT_MAX_PARTITIONS 256

static NTSTATUS CdpQueryPhysicalPartitionLayout(
	_In_ PDEVICE_OBJECT PartitionDevice,
	_Out_ PULONG DiskNumber,
	_Out_ PULONG PartitionNumber,
	_Out_ PUINT64 PartitionStart,
	_Out_ PUINT64 PartitionLength,
	_Out_ PBOOLEAN HasNextPartition,
	_Out_ PUINT64 NextPartitionStart,
	_Out_opt_ PULONG NextPartitionNumber,
	_Out_opt_ PUINT64 NextPartitionLength,
	_Out_opt_ PULONG DiskPartitionStyle,
	_Out_opt_ PULONG MbrSignature,
	_Out_opt_ GUID* DiskGuid)
{
	STORAGE_DEVICE_NUMBER deviceNumber;
	PARTITION_INFORMATION_EX partitionInfo;
	PDRIVE_LAYOUT_INFORMATION_EX layout = NULL;
	ULONG layoutBytes;
	PFILE_OBJECT diskFileObject = NULL;
	PDEVICE_OBJECT diskDevice = NULL;
	WCHAR diskPathBuffer[64];
	UNICODE_STRING diskPath;
	NTSTATUS status;
	ULONG index;
	UINT64 nextStart = MAXULONGLONG;
	UINT64 nextLength = 0;
	ULONG nextNumber = 0;
	UINT64 layoutPartitionLength = 0;
	BOOLEAN exactPartitionFound = FALSE;

	*DiskNumber = 0;
	*PartitionNumber = 0;
	*PartitionStart = 0;
	*PartitionLength = 0;
	*HasNextPartition = FALSE;
	*NextPartitionStart = 0;
	if (NextPartitionNumber)
		*NextPartitionNumber = 0;
	if (NextPartitionLength)
		*NextPartitionLength = 0;
	if (DiskPartitionStyle)
		*DiskPartitionStyle = PARTITION_STYLE_RAW;
	if (MbrSignature)
		*MbrSignature = 0;
	if (DiskGuid)
		RtlZeroMemory(DiskGuid, sizeof(*DiskGuid));
	RtlZeroMemory(&deviceNumber, sizeof(deviceNumber));
	RtlZeroMemory(&partitionInfo, sizeof(partitionInfo));

	status = CdpSendDeviceControlSynchronously(
		PartitionDevice,
		IOCTL_STORAGE_GET_DEVICE_NUMBER,
		&deviceNumber,
		sizeof(deviceNumber));
	if (!NT_SUCCESS(status))
		return status;
	status = CdpSendDeviceControlSynchronously(
		PartitionDevice,
		IOCTL_DISK_GET_PARTITION_INFO_EX,
		&partitionInfo,
		sizeof(partitionInfo));
	if (!NT_SUCCESS(status) || partitionInfo.StartingOffset.QuadPart < 0 ||
		partitionInfo.PartitionLength.QuadPart <= 0)
		return NT_SUCCESS(status) ? STATUS_DATA_ERROR : status;

	layoutBytes = FIELD_OFFSET(DRIVE_LAYOUT_INFORMATION_EX, PartitionEntry) +
		Cdp_PARTITION_LAYOUT_MAX_PARTITIONS * sizeof(PARTITION_INFORMATION_EX);
	layout = (PDRIVE_LAYOUT_INFORMATION_EX)cdpalloc(layoutBytes);
	if (!layout)
		return STATUS_INSUFFICIENT_RESOURCES;
	RtlZeroMemory(layout, layoutBytes);
	status = CdpSendDeviceControlSynchronously(
		PartitionDevice,
		IOCTL_DISK_GET_DRIVE_LAYOUT_EX,
		layout,
		layoutBytes);
	if (!NT_SUCCESS(status))
	{
		// Some storage stacks accept drive-layout requests only on Partition0.
		status = RtlStringCbPrintfW(
			diskPathBuffer,
			sizeof(diskPathBuffer),
			L"\\Device\\Harddisk%lu\\Partition0",
			deviceNumber.DeviceNumber);
		if (NT_SUCCESS(status))
		{
			RtlInitUnicodeString(&diskPath, diskPathBuffer);
			status = IoGetDeviceObjectPointer(
				&diskPath,
				FILE_READ_ATTRIBUTES,
				&diskFileObject,
				&diskDevice);
		}
		if (NT_SUCCESS(status))
		{
			RtlZeroMemory(layout, layoutBytes);
			status = CdpSendDeviceControlSynchronously(
				diskDevice,
				IOCTL_DISK_GET_DRIVE_LAYOUT_EX,
				layout,
				layoutBytes);
		}
	}
	if (NT_SUCCESS(status))
	{
		ULONG count = layout->PartitionCount;
		if (DiskPartitionStyle)
			*DiskPartitionStyle = layout->PartitionStyle;
		if (layout->PartitionStyle == PARTITION_STYLE_MBR && MbrSignature)
			*MbrSignature = layout->Mbr.Signature;
		if (layout->PartitionStyle == PARTITION_STYLE_GPT && DiskGuid)
			*DiskGuid = layout->Gpt.DiskId;
		if (count > Cdp_PARTITION_LAYOUT_MAX_PARTITIONS)
			count = Cdp_PARTITION_LAYOUT_MAX_PARTITIONS;
		for (index = 0; index < count; ++index)
		{
			PPARTITION_INFORMATION_EX candidate = &layout->PartitionEntry[index];
			UINT64 candidateStart;
			if (candidate->PartitionLength.QuadPart <= 0 ||
				candidate->StartingOffset.QuadPart < 0)
				continue;
			candidateStart = (UINT64)candidate->StartingOffset.QuadPart;
			if (candidateStart ==
				(UINT64)partitionInfo.StartingOffset.QuadPart)
			{
				UINT64 candidateLength =
					(UINT64)candidate->PartitionLength.QuadPart;
				/* Never let a stale/reused partition number select another
				 * extent. If the two live APIs briefly disagree, the smaller
				 * length is the fail-safe boundary. */
				layoutPartitionLength = candidateLength <
					(UINT64)partitionInfo.PartitionLength.QuadPart ?
					candidateLength :
					(UINT64)partitionInfo.PartitionLength.QuadPart;
				exactPartitionFound = TRUE;
			}
			if (candidateStart > (UINT64)partitionInfo.StartingOffset.QuadPart &&
				candidateStart < nextStart)
			{
				nextStart = candidateStart;
				nextLength = (UINT64)candidate->PartitionLength.QuadPart;
				nextNumber = candidate->PartitionNumber;
			}
		}
	}
	cdpfree(layout);
	if (diskFileObject)
		ObDereferenceObject(diskFileObject);
	if (!NT_SUCCESS(status))
		return status;
	/* Do not infer a partition length from the next partition's start: an
	 * alignment gap is not part of either partition. */
	if (!exactPartitionFound || layoutPartitionLength == 0)
		layoutPartitionLength =
			(UINT64)partitionInfo.PartitionLength.QuadPart;

	*DiskNumber = deviceNumber.DeviceNumber;
	*PartitionNumber = partitionInfo.PartitionNumber;
	*PartitionStart = (UINT64)partitionInfo.StartingOffset.QuadPart;
	*PartitionLength = layoutPartitionLength;
	if (nextStart != MAXULONGLONG)
	{
		*HasNextPartition = TRUE;
		*NextPartitionStart = nextStart;
		if (NextPartitionNumber)
			*NextPartitionNumber = nextNumber;
		if (NextPartitionLength)
			*NextPartitionLength = nextLength;
	}
	return STATUS_SUCCESS;
}

static BOOLEAN CdpAutoDiskIdentityMatches(
	_In_ PCdp_DEVICE_EXTENSION SourceExt,
	_In_ PCdp_JOURNAL Journal)
{
	if (Journal->DiskPartitionStyle != SourceExt->DiskPartitionStyle)
		return FALSE;
	if (SourceExt->DiskPartitionStyle == PARTITION_STYLE_GPT)
	{
		return !CdpGuidIsZero(&Journal->DiskGuid) &&
			CdpGuidIsEqual(&Journal->DiskGuid, &SourceExt->DiskGuid);
	}
	if (SourceExt->DiskPartitionStyle == PARTITION_STYLE_MBR)
	{
		return Journal->MbrSignature != 0 &&
			Journal->MbrSignature == SourceExt->MbrSignature;
	}
	return FALSE;
}

static BOOLEAN CdpAutoPhysicalLayoutMatches(
	_In_ PCdp_DEVICE_EXTENSION SourceExt,
	_In_ PCdp_JOURNAL Journal)
{
	if (!SourceExt->DiskLayoutValid || !SourceExt->HasNextPartition ||
		SourceExt->PartitionSize == 0 || SourceExt->NextPartitionSize == 0)
	{
		return FALSE;
	}
	return CdpAutoDiskIdentityMatches(SourceExt, Journal) &&
		Journal->SourcePartitionStart == SourceExt->PartitionStart &&
		Journal->SourcePartitionSize == SourceExt->PartitionSize &&
		Journal->JournalPartitionStart == SourceExt->NextPartitionStart &&
		Journal->JournalPartitionSize == SourceExt->NextPartitionSize &&
		SourceExt->PartitionStart <= MAXUINT64 - SourceExt->PartitionSize &&
		SourceExt->PartitionStart + SourceExt->PartitionSize <=
			SourceExt->NextPartitionStart;
}

/* Test path: build the protected source entirely from persisted physical
 * identity while the disk START IRP is still owned by this filter.  This
 * removes the volume-START window in which source-range reads could arrive
 * before the journal MetaTree was ready.  One protected source per disk is
 * supported by this test implementation. */
static NTSTATUS CdpDiscoverJournalForStartedDisk(
	_In_ PCdp_DRIVER_EXTENSION DriverExt,
	_Inout_ PCdp_DEVICE_EXTENSION DiskExt)
{
	PDRIVE_LAYOUT_INFORMATION_EX layout = NULL;
	ULONG layoutBytes;
	ULONG count;
	ULONG journalIndex;
	UINT64 diskSize = 0;
	ULONG sectorSize = 0;
	NTSTATUS status;

	if (!DriverExt || !DiskExt ||
		DiskExt->DeviceKind != Cdp_DEVICE_KIND_DISK ||
		!DiskExt->LowerDeviceObject ||
		InterlockedCompareExchange(&DiskExt->Started, 0, 0) == 0)
	{
		return STATUS_INVALID_DEVICE_STATE;
	}
	if (DiskExt->Core || DiskExt->JournalHandleId != 0 ||
		InterlockedCompareExchange(&DiskExt->CaptureEnabled, 0, 0) != 0)
	{
		return STATUS_DEVICE_BUSY;
	}

	status = CdpQueryDeviceGeometry(
		DiskExt->LowerDeviceObject, &diskSize, &sectorSize);
	if (!NT_SUCCESS(status) || diskSize == 0 ||
		(sectorSize != 512 && sectorSize != 4096))
	{
		return NT_SUCCESS(status) ? STATUS_DEVICE_CONFIGURATION_ERROR : status;
	}
	layoutBytes = FIELD_OFFSET(DRIVE_LAYOUT_INFORMATION_EX, PartitionEntry) +
		Cdp_PARTITION_LAYOUT_MAX_PARTITIONS * sizeof(PARTITION_INFORMATION_EX);
	layout = (PDRIVE_LAYOUT_INFORMATION_EX)cdpalloc(layoutBytes);
	if (!layout)
		return STATUS_INSUFFICIENT_RESOURCES;
	RtlZeroMemory(layout, layoutBytes);
	status = CdpSendDeviceControlSynchronously(
		DiskExt->LowerDeviceObject,
		IOCTL_DISK_GET_DRIVE_LAYOUT_EX,
		layout,
		layoutBytes);
	if (!NT_SUCCESS(status))
		goto done;
	count = layout->PartitionCount;
	if (count > Cdp_PARTITION_LAYOUT_MAX_PARTITIONS)
		count = Cdp_PARTITION_LAYOUT_MAX_PARTITIONS;

	for (journalIndex = 0; journalIndex < count; ++journalIndex)
	{
		PPARTITION_INFORMATION_EX journalPart =
			&layout->PartitionEntry[journalIndex];
		PCdp_VOLUME_HANDLE_ENTRY journalEntry = NULL;
		PCdp_STORE sourceStore = NULL;
		ULONG sourceIndex = MAXULONG;
		ULONG previousIndex = MAXULONG;
		UINT64 journalStart;
		UINT64 journalSize;
		UINT64 previousStart = 0;
		UINT64 handleId = 0;
		ULONG index;
		BOOLEAN inserted = FALSE;
		BOOLEAN sessionReference = FALSE;

		if (journalPart->StartingOffset.QuadPart < 0 ||
			journalPart->PartitionLength.QuadPart <= 0)
			continue;
		journalStart = (UINT64)journalPart->StartingOffset.QuadPart;
		journalSize = (UINT64)journalPart->PartitionLength.QuadPart;
		if (journalStart > diskSize || journalSize > diskSize - journalStart)
			continue;

		journalEntry = (PCdp_VOLUME_HANDLE_ENTRY)cdpalloc(sizeof(*journalEntry));
		if (!journalEntry)
		{
			status = STATUS_INSUFFICIENT_RESOURCES;
			goto done;
		}
		RtlZeroMemory(journalEntry, sizeof(*journalEntry));
		journalEntry->TargetLowerDevice = DiskExt->LowerDeviceObject;
		journalEntry->TargetBaseOffset = journalStart;
		journalEntry->DiskNumber = DiskExt->DiskNumber;
		journalEntry->PartitionNumber = journalPart->PartitionNumber;
		journalEntry->PartitionSize = journalSize;
		journalEntry->SectorSize = sectorSize;
		journalEntry->ReferenceCount = 1;
		KeInitializeEvent(
			&journalEntry->NoReferences, NotificationEvent, FALSE);
		CdpJournalInitialize(
			&journalEntry->Journal,
			DiskExt->LowerDeviceObject,
			NULL,
			journalStart,
			journalSize,
			sectorSize,
			&journalEntry->VolumeGuid);
		status = CdpJournalMount(&journalEntry->Journal);
		if (!NT_SUCCESS(status))
		{
			CdpJournalClose(&journalEntry->Journal);
			cdpfree(journalEntry);
			continue;
		}

		/* Locate both the persisted source extent and the immediately preceding
		 * allocated partition.  They must be the same entry. */
		for (index = 0; index < count; ++index)
		{
			PPARTITION_INFORMATION_EX part = &layout->PartitionEntry[index];
			UINT64 start;
			UINT64 size;
			if (part->StartingOffset.QuadPart < 0 ||
				part->PartitionLength.QuadPart <= 0)
				continue;
			start = (UINT64)part->StartingOffset.QuadPart;
			size = (UINT64)part->PartitionLength.QuadPart;
			if (start == journalEntry->Journal.SourcePartitionStart &&
				size == journalEntry->Journal.SourcePartitionSize)
				sourceIndex = index;
			if (start < journalStart &&
				(previousIndex == MAXULONG || start > previousStart))
			{
				previousStart = start;
				previousIndex = index;
			}
		}

		if (sourceIndex == MAXULONG || previousIndex != sourceIndex ||
			CdpGuidIsZero(&journalEntry->Journal.SourceVolumeGuid) ||
			journalEntry->Journal.JournalPartitionStart != journalStart ||
			journalEntry->Journal.JournalPartitionSize != journalSize ||
			journalEntry->Journal.SourcePartitionStart >
				MAXUINT64 - journalEntry->Journal.SourcePartitionSize ||
			journalEntry->Journal.SourcePartitionStart +
				journalEntry->Journal.SourcePartitionSize > journalStart ||
			journalEntry->Journal.DiskPartitionStyle != layout->PartitionStyle ||
			(layout->PartitionStyle == PARTITION_STYLE_MBR &&
				(journalEntry->Journal.MbrSignature == 0 ||
				 journalEntry->Journal.MbrSignature != layout->Mbr.Signature)) ||
			(layout->PartitionStyle == PARTITION_STYLE_GPT &&
				(CdpGuidIsZero(&journalEntry->Journal.DiskGuid) ||
				 !CdpGuidIsEqual(
					&journalEntry->Journal.DiskGuid, &layout->Gpt.DiskId))))
		{
			CdpJournalClose(&journalEntry->Journal);
			cdpfree(journalEntry);
			continue;
		}

		DiskExt->DiskPartitionStyle = layout->PartitionStyle;
		DiskExt->MbrSignature = layout->PartitionStyle == PARTITION_STYLE_MBR ?
			layout->Mbr.Signature : 0;
		if (layout->PartitionStyle == PARTITION_STYLE_GPT)
			DiskExt->DiskGuid = layout->Gpt.DiskId;
		else
			RtlZeroMemory(&DiskExt->DiskGuid, sizeof(DiskExt->DiskGuid));
		DiskExt->PartitionNumber =
			layout->PartitionEntry[sourceIndex].PartitionNumber;
		DiskExt->PartitionStart =
			journalEntry->Journal.SourcePartitionStart;
		DiskExt->PartitionSize = journalEntry->Journal.SourcePartitionSize;
		DiskExt->HasNextPartition = TRUE;
		DiskExt->NextPartitionNumber = journalPart->PartitionNumber;
		DiskExt->NextPartitionStart = journalStart;
		DiskExt->NextPartitionSize = journalSize;
		DiskExt->SectorSize = sectorSize;
		DiskExt->DiskLayoutValid = TRUE;
		DiskExt->VolumeGuid = journalEntry->Journal.SourceVolumeGuid;
		DiskExt->VolumeGuidValid = TRUE;

		status = CdpDevStoreCreateAbsoluteRange(
			DiskExt->LowerDeviceObject,
			DiskExt->PartitionStart,
			DiskExt->PartitionSize,
			sectorSize,
			&sourceStore);
		if (NT_SUCCESS(status))
			status = CdpCoreBind(
				sourceStore,
				&journalEntry->Journal,
				&DiskExt->VolumeGuid,
				&DiskExt->Core);
		if (NT_SUCCESS(status))
			status = CdpValidateMountedSourceRecordRanges(
				DiskExt, "disk-start-preactivate");
		if (!NT_SUCCESS(status))
		{
			if (DiskExt->Core)
			{
				CdpCoreDestroy(DiskExt->Core);
				DiskExt->Core = NULL;
				sourceStore = NULL;
			}
			if (sourceStore)
				CdpDevStoreDestroy(sourceStore);
			CdpJournalClose(&journalEntry->Journal);
			cdpfree(journalEntry);
			goto done;
		}

		handleId = (UINT64)InterlockedIncrement64(
			&DriverExt->VolumeHandleNextId);
		journalEntry->HandleId = handleId;
		ExAcquireFastMutex(&DriverExt->VolumeHandleMutex);
		InsertTailList(&DriverExt->VolumeHandleList, &journalEntry->Entry);
		InterlockedIncrement(&journalEntry->ReferenceCount);
		inserted = TRUE;
		ExReleaseFastMutex(&DriverExt->VolumeHandleMutex);

		DiskExt->JournalHandleId = handleId;
		InterlockedIncrement(&journalEntry->ReferenceCount);
		sessionReference = TRUE;
		DiskExt->RedirectJournalEntry = journalEntry;
		status = CdpValidateProtectionObjectGraph(
			DriverExt, DiskExt, "disk-start-before-enable");
		if (!NT_SUCCESS(status))
		{
			DiskExt->RedirectJournalEntry = NULL;
			DiskExt->JournalHandleId = 0;
			if (sessionReference)
				CdpReleaseVolumeHandleEntry(journalEntry);
			CdpCoreDestroy(DiskExt->Core);
			DiskExt->Core = NULL;
			CdpReleaseVolumeHandleEntry(journalEntry);
			if (inserted)
				(void)CdpCloseVolumeHandle(DriverExt, handleId);
			goto done;
		}

		CdpAuditProtectedReadReset(DiskExt);
		InterlockedExchange(&DiskExt->DiskIoAccepting, 1);
		InterlockedExchange(&DiskExt->ProtectionStateValidated, 1);
		InterlockedExchange(&DiskExt->CaptureEnabled, 1);
		if (journalEntry->Journal.RecoveryPending)
		{
			UINT64 recoveryTarget =
				journalEntry->Journal.RecoveryTargetTime100ns;
			KeWaitForSingleObject(
				&DiskExt->HistoryMutex,
				Executive, KernelMode, FALSE, NULL);
			status = CdpCorePrepareRebootRecovery(
				DiskExt->Core, recoveryTarget);
			KeReleaseMutex(&DiskExt->HistoryMutex, FALSE);
			if (!NT_SUCCESS(status))
			{
				Cdp_LOG("[DISK-PRESTART] recovery prepare failed status=0x%08X target=%llu; protection remains active\n",
					status, recoveryTarget);
				status = STATUS_SUCCESS;
			}
		}
		CdpReleaseVolumeHandleEntry(journalEntry);
		Cdp_LOG("[DISK-PRESTART] protection enabled before disk START completion disk=%lu sourcePart=%lu source=[%llu,%llu) journalPart=%lu journal=[%llu,%llu) records=%llu\n",
			DiskExt->DiskNumber,
			DiskExt->PartitionNumber,
			DiskExt->PartitionStart,
			DiskExt->PartitionStart + DiskExt->PartitionSize,
			DiskExt->NextPartitionNumber,
			DiskExt->NextPartitionStart,
			DiskExt->NextPartitionStart + DiskExt->NextPartitionSize,
			journalEntry->Journal.TotalRecords);
		status = STATUS_SUCCESS;
		goto done;
	}
	status = STATUS_NOT_FOUND;

done:
	if (layout)
		cdpfree(layout);
	return status;
}

/* START_DEVICE remains owned by this filter while this routine runs. Thus
 * Journal/Core/MetaTree are ready before the filesystem can mount the source,
 * and ordinary READ/WRITE IRPs never need a discovery gate. */
static NTSTATUS CdpDiscoverAdjacentJournalForStartedVolume(
	_In_ PCdp_DRIVER_EXTENSION DriverExt,
	_Inout_ PCdp_DEVICE_EXTENSION SourceExt)
{
	PCdp_DEVICE_EXTENSION diskExt;
	PCdp_VOLUME_HANDLE_ENTRY journalEntry;
	PDEVICE_OBJECT metadataLower = NULL;
	GUID queriedSourceGuid = { 0 };
	BOOLEAN guidAvailable;
	BOOLEAN guidMatches;
	BOOLEAN physicalMatches;
	BOOLEAN existingDiskPhysicalMatch;
	BOOLEAN existingDiskGuidMatch;
	UINT64 sourceGeometrySize = 0;
	UINT64 handleId;
	NTSTATUS status;

	if (!DriverExt || !SourceExt ||
		SourceExt->DeviceKind != Cdp_DEVICE_KIND_VOLUME ||
		!SourceExt->LowerDeviceObject)
	{
		return STATUS_INVALID_PARAMETER;
	}
	/* Cache the stable volume identity independently of the physical-layout
	 * query. Some stacks expose the GUID here but transiently reject a drive
	 * layout request; CMD1 must still be able to find this filter object. */
	status = IoVolumeDeviceToGuid(
		SourceExt->PhysicalDeviceObject ?
			SourceExt->PhysicalDeviceObject : SourceExt->FilterDeviceObject,
		&queriedSourceGuid);
	if (!NT_SUCCESS(status))
		status = IoVolumeDeviceToGuid(
			SourceExt->LowerDeviceObject, &queriedSourceGuid);
	guidAvailable = NT_SUCCESS(status) && !CdpGuidIsZero(&queriedSourceGuid);
	if (guidAvailable)
	{
		SourceExt->VolumeGuid = queriedSourceGuid;
		SourceExt->VolumeGuidValid = TRUE;
	}
	status = CdpQueryPhysicalPartitionLayout(
		SourceExt->LowerDeviceObject,
		&SourceExt->DiskNumber,
		&SourceExt->PartitionNumber,
		&SourceExt->PartitionStart,
		&SourceExt->PartitionSize,
		&SourceExt->HasNextPartition,
		&SourceExt->NextPartitionStart,
		&SourceExt->NextPartitionNumber,
		&SourceExt->NextPartitionSize,
		&SourceExt->DiskPartitionStyle,
		&SourceExt->MbrSignature,
		&SourceExt->DiskGuid);
	SourceExt->DiskLayoutValid = NT_SUCCESS(status) ? TRUE : FALSE;
	if (!NT_SUCCESS(status))
		return status;
	if (!SourceExt->HasNextPartition || SourceExt->NextPartitionSize == 0)
		return STATUS_NOT_FOUND;
	status = CdpQueryDeviceGeometry(
		SourceExt->LowerDeviceObject,
		&sourceGeometrySize,
		&SourceExt->SectorSize);
	if (!NT_SUCCESS(status))
		return status;
	if (sourceGeometrySize < SourceExt->PartitionSize)
		SourceExt->PartitionSize = sourceGeometrySize;

	diskExt = CdpFindDiskExtensionByNumber(
		DriverExt, SourceExt->DiskNumber);
	if (!diskExt || !diskExt->LowerDeviceObject ||
		InterlockedCompareExchange(&diskExt->Started, 0, 0) == 0)
	{
		return STATUS_DEVICE_NOT_READY;
	}
	existingDiskPhysicalMatch =
		(diskExt->PartitionStart == SourceExt->PartitionStart &&
		 diskExt->PartitionSize == SourceExt->PartitionSize);
	existingDiskGuidMatch =
		(diskExt->VolumeGuidValid && SourceExt->VolumeGuidValid &&
		 CdpGuidIsEqual(&diskExt->VolumeGuid, &SourceExt->VolumeGuid));
	if (diskExt->Core &&
		InterlockedCompareExchange(&diskExt->CaptureEnabled, 0, 0) != 0 &&
		InterlockedCompareExchange(
			&diskExt->ProtectionStateValidated, 0, 0) != 0 &&
		(existingDiskPhysicalMatch || existingDiskGuidMatch))
	{
		Cdp_LOG("[DISK-PRESTART] volume associated with existing disk protection disk=%lu part=%lu source=[%llu,%llu) physicalMatch=%u guidMatch=%u\n",
			SourceExt->DiskNumber,
			SourceExt->PartitionNumber,
			SourceExt->PartitionStart,
			SourceExt->PartitionStart + SourceExt->PartitionSize,
			existingDiskPhysicalMatch ? 1u : 0u,
			existingDiskGuidMatch ? 1u : 0u);
		return STATUS_SUCCESS;
	}
	journalEntry = (PCdp_VOLUME_HANDLE_ENTRY)cdpalloc(sizeof(*journalEntry));
	if (!journalEntry)
		return STATUS_INSUFFICIENT_RESOURCES;
	RtlZeroMemory(journalEntry, sizeof(*journalEntry));
	journalEntry->TargetLowerDevice = diskExt->LowerDeviceObject;
	journalEntry->TargetBaseOffset = SourceExt->NextPartitionStart;
	journalEntry->DiskNumber = SourceExt->DiskNumber;
	journalEntry->PartitionNumber = SourceExt->NextPartitionNumber;
	journalEntry->PartitionSize = SourceExt->NextPartitionSize;
	journalEntry->SectorSize = SourceExt->SectorSize;
	journalEntry->ReferenceCount = 1;
	KeInitializeEvent(&journalEntry->NoReferences, NotificationEvent, FALSE);
	metadataLower = CdpReferenceVolumeLowerByPhysicalRange(
		DriverExt,
		SourceExt->DiskNumber,
		SourceExt->NextPartitionStart,
		SourceExt->NextPartitionSize);
	if (metadataLower)
	{
		journalEntry->VolumeLowerDevice = metadataLower;
		journalEntry->MetadataLowerDeviceReference = metadataLower;
		Cdp_LOG("[AUTO-METADATA] bound volume lower=%p disk=%lu part=%lu start=%llu size=%llu; payload remains disk-absolute\n",
			metadataLower,
			SourceExt->DiskNumber,
			SourceExt->NextPartitionNumber,
			SourceExt->NextPartitionStart,
			SourceExt->NextPartitionSize);
	}
	else
	{
		Cdp_LOG("[AUTO-METADATA] adjacent volume lower unavailable disk=%lu part=%lu start=%llu size=%llu; retaining disk metadata backend\n",
			SourceExt->DiskNumber,
			SourceExt->NextPartitionNumber,
			SourceExt->NextPartitionStart,
			SourceExt->NextPartitionSize);
	}
	CdpJournalInitialize(
		&journalEntry->Journal,
		journalEntry->TargetLowerDevice,
		NULL,
		journalEntry->TargetBaseOffset,
		journalEntry->PartitionSize,
		journalEntry->SectorSize,
		&queriedSourceGuid);
	if (metadataLower)
		CdpJournalSetMetadataDevice(&journalEntry->Journal, metadataLower, 0);
	status = CdpJournalMount(&journalEntry->Journal);
	if (!NT_SUCCESS(status))
	{
		CdpJournalClose(&journalEntry->Journal);
		if (journalEntry->MetadataLowerDeviceReference)
		{
			ObDereferenceObject(
				journalEntry->MetadataLowerDeviceReference);
			journalEntry->MetadataLowerDeviceReference = NULL;
		}
		cdpfree(journalEntry);
		return status == STATUS_DISK_CORRUPT_ERROR ? STATUS_NOT_FOUND : status;
	}

	guidMatches = guidAvailable && CdpGuidIsEqual(
		&queriedSourceGuid, &journalEntry->Journal.SourceVolumeGuid);
	physicalMatches = CdpAutoPhysicalLayoutMatches(
		SourceExt, &journalEntry->Journal);
	if ((guidAvailable && !guidMatches) ||
		(!guidAvailable && !physicalMatches) ||
		(guidMatches && journalEntry->Journal.SourcePartitionSize != 0 &&
		 !physicalMatches))
	{
		Cdp_LOG("[AUTO-ADJACENT] identity mismatch disk=%lu sourcePart=%lu journalPart=%lu guidAvailable=%u guidMatch=%u physicalMatch=%u\n",
			SourceExt->DiskNumber,
			SourceExt->PartitionNumber,
			SourceExt->NextPartitionNumber,
			guidAvailable ? 1u : 0u,
			guidMatches ? 1u : 0u,
			physicalMatches ? 1u : 0u);
		CdpJournalClose(&journalEntry->Journal);
		cdpfree(journalEntry);
		return STATUS_OBJECT_TYPE_MISMATCH;
	}

	SourceExt->VolumeGuid = journalEntry->Journal.SourceVolumeGuid;
	SourceExt->VolumeGuidValid = TRUE;
	handleId = (UINT64)InterlockedIncrement64(
		&DriverExt->VolumeHandleNextId);
	journalEntry->HandleId = handleId;
	ExAcquireFastMutex(&DriverExt->VolumeHandleMutex);
	InsertTailList(&DriverExt->VolumeHandleList, &journalEntry->Entry);
	InterlockedIncrement(&journalEntry->ReferenceCount);
	ExReleaseFastMutex(&DriverExt->VolumeHandleMutex);

	status = CdpActivateAutoJournal(DriverExt, handleId, journalEntry);
	if (NT_SUCCESS(status) && journalEntry->Journal.RecoveryPending)
	{
		UINT64 recoveryTarget =
			journalEntry->Journal.RecoveryTargetTime100ns;

		/* Boot discovery is deliberately read-only. Publish the target view now,
		 * but defer the new branch RR/Superblock write until the first protected
		 * source write arrives after START_DEVICE has completed. */
		CdpStopMergeThread(SourceExt);
		KeWaitForSingleObject(
			&SourceExt->HistoryMutex,
			Executive,
			KernelMode,
			FALSE,
			NULL);
		status = CdpCorePrepareRebootRecovery(
			SourceExt->Core, recoveryTarget);
		KeReleaseMutex(&SourceExt->HistoryMutex, FALSE);
		if (!NT_SUCCESS(status))
		{
			Cdp_LOG("[RECOVERY] pre-mount deferred prepare failed status=0x%08X target=%llu; boot continues with protection mounted\n",
				status,
				recoveryTarget);
			status = STATUS_SUCCESS;
		}
		else
		{
			Cdp_LOG("[RECOVERY] pre-mount target view ready target=%llu; branch persistence deferred to first source write\n",
				recoveryTarget);
		}
	}
	CdpReleaseVolumeHandleEntry(journalEntry);
	if (!NT_SUCCESS(status))
	{
		(void)CdpCloseVolumeHandle(DriverExt, handleId);
		return status;
	}
	Cdp_LOG("[AUTO-ADJACENT] protection armed before mount disk=%lu sourcePart=%lu journalPart=%lu guidMatch=%u physicalMatch=%u\n",
		SourceExt->DiskNumber,
		SourceExt->PartitionNumber,
		SourceExt->NextPartitionNumber,
		guidMatches ? 1u : 0u,
		physicalMatches ? 1u : 0u);
	return STATUS_SUCCESS;
}

static PCdp_PREVIEW_SESSION CdpLookupPreviewSessionLocked(
	_In_ PCdp_DRIVER_EXTENSION DriverExt,
	_In_ UINT64 HandleId)
{
	PLIST_ENTRY entry = DriverExt->PreviewSessionList.Flink;
	while (entry != &DriverExt->PreviewSessionList)
	{
		PCdp_PREVIEW_SESSION session =
			CONTAINING_RECORD(entry, Cdp_PREVIEW_SESSION, Entry);
		if (session->HandleId == HandleId)
			return session;
		entry = entry->Flink;
	}
	return NULL;
}

static BOOLEAN CdpAnyPreviewSessionActive(
	_In_ PCdp_DRIVER_EXTENSION DriverExt)
{
	BOOLEAN active;

	ExAcquireFastMutex(&DriverExt->PreviewSessionMutex);
	active = !IsListEmpty(&DriverExt->PreviewSessionList);
	ExReleaseFastMutex(&DriverExt->PreviewSessionMutex);
	return active;
}

static PCdp_PREVIEW_SESSION CdpAcquirePreviewSession(
	_In_ PCdp_DRIVER_EXTENSION DriverExt,
	_In_ UINT64 HandleId)
{
	PCdp_PREVIEW_SESSION session;

	ExAcquireFastMutex(&DriverExt->PreviewSessionMutex);
	session = CdpLookupPreviewSessionLocked(DriverExt, HandleId);
	if (session && !session->Closing)
		InterlockedIncrement(&session->ReferenceCount);
	else
		session = NULL;
	ExReleaseFastMutex(&DriverExt->PreviewSessionMutex);
	return session;
}

static VOID CdpReleasePreviewSession(_In_ PCdp_PREVIEW_SESSION Session)
{
	if (InterlockedDecrement(&Session->ReferenceCount) == 0)
		KeSetEvent(&Session->NoReferences, IO_NO_INCREMENT, FALSE);
}

static VOID CdpDestroyPreviewSession(
	_In_ PCdp_DRIVER_EXTENSION DriverExt,
	_In_ PCdp_PREVIEW_SESSION Session)
{
	CdpReleasePreviewSession(Session); // Drop list ownership.
	KeWaitForSingleObject(
		&Session->NoReferences,
		Executive,
		KernelMode,
		FALSE,
		NULL);
	if (Session->SourceVolumeHandleId)
		(void)CdpCloseVolumeHandle(
			DriverExt,
			Session->SourceVolumeHandleId);
	if (Session->JournalEntry)
		CdpReleaseVolumeHandleEntry(Session->JournalEntry);
	cdpfree(Session);
}

static NTSTATUS CdpBeginPreviewSession(
	_In_ PCdp_DRIVER_EXTENSION DriverExt,
	_In_ const Cdp_PREVIEW_BEGIN_REQUEST* Request,
	_Out_ PCdp_PREVIEW_BEGIN_REPLY Reply)
{
	PCdp_VOLUME_HANDLE_ENTRY journalEntry = NULL;
	PCdp_PREVIEW_SESSION session = NULL;
	PCdp_DEVICE_EXTENSION sourceExt = NULL;
	UINT64 sourceHandleId = 0;
	UINT64 oldestTime = 0;
	UINT64 newestTime = 0;
	UINT64 targetTime = Request->TargetTime100ns;
	BOOLEAN phaseTransitioned = FALSE;
	NTSTATUS status;

		RtlZeroMemory(Reply, sizeof(*Reply));
	sourceExt = CdpFindSourceExtensionByGuid(
		DriverExt,
		&Request->SourceVolumeGuid);
	if (!sourceExt)
		return STATUS_DEVICE_DOES_NOT_EXIST;
	if (InterlockedCompareExchange(
		&sourceExt->MergeThreadRunning, 0, 0) != 0)
	{
		return STATUS_DEVICE_BUSY;
	}
	journalEntry = CdpAcquireJournalForSource(DriverExt, sourceExt);
	if (!journalEntry)
		return STATUS_DEVICE_NOT_READY;
	if (InterlockedCompareExchange(&sourceExt->Phase, 0, 0) !=
		(LONG)Cdp_PHASE_GENERAL)
	{
		CdpReleaseVolumeHandleEntry(journalEntry);
		return STATUS_INVALID_DEVICE_STATE;
	}
	if (CdpAnyPreviewSessionActive(DriverExt))
	{
		CdpReleaseVolumeHandleEntry(journalEntry);
		return STATUS_INVALID_DEVICE_STATE;
	}
	if (!sourceExt->Core)
	{
		CdpReleaseVolumeHandleEntry(journalEntry);
		return STATUS_DEVICE_NOT_READY;
	}

	if (!journalEntry->Journal.Mounted ||
		RtlCompareMemory(
			&journalEntry->Journal.SourceVolumeGuid,
			&Request->SourceVolumeGuid,
			sizeof(GUID)) != sizeof(GUID))
	{
		status = STATUS_NOT_FOUND;
		goto cleanup;
	}

	status = CdpJournalQueryTimeRange(
		&journalEntry->Journal,
		&oldestTime,
		&newestTime);
	if (status == STATUS_NOT_FOUND)
	{
		oldestTime = 0;
		newestTime = 0;
		status = STATUS_SUCCESS;
	}
	else if (!NT_SUCCESS(status))
	{
		goto cleanup;
	}
	else if (targetTime < oldestTime)
		targetTime = oldestTime;

	status = CdpOpenVolumeHandle(
		DriverExt,
		&Request->SourceVolumeGuid,
		&sourceHandleId);
	if (!NT_SUCCESS(status))
		goto cleanup;

	if (InterlockedCompareExchange(
			&sourceExt->Phase,
			(LONG)Cdp_PHASE_PREVIEW,
			(LONG)Cdp_PHASE_GENERAL) != (LONG)Cdp_PHASE_GENERAL)
	{
		status = STATUS_INVALID_DEVICE_STATE;
		goto cleanup;
	}
	phaseTransitioned = TRUE;

	if (CdpAnyPreviewSessionActive(DriverExt))
	{
		status = STATUS_INVALID_DEVICE_STATE;
		goto cleanup;
	}

	session = (PCdp_PREVIEW_SESSION)cdpalloc(sizeof(*session));
	if (!session)
	{
		status = STATUS_INSUFFICIENT_RESOURCES;
		goto cleanup;
	}
	RtlZeroMemory(session, sizeof(*session));
	session->HandleId =
		(UINT64)InterlockedIncrement64(&DriverExt->PreviewSessionNextId);
	session->TargetTime100ns = targetTime;
	session->SourceVolumeHandleId = sourceHandleId;
	session->JournalEntry = journalEntry;
	session->SourceVolumeGuid = Request->SourceVolumeGuid;
	session->ReferenceCount = 1;
	KeInitializeEvent(
		&session->NoReferences,
		NotificationEvent,
		FALSE);

	ExAcquireFastMutex(&DriverExt->PreviewSessionMutex);
	if (!IsListEmpty(&DriverExt->PreviewSessionList))
	{
		ExReleaseFastMutex(&DriverExt->PreviewSessionMutex);
		status = STATUS_INVALID_DEVICE_STATE;
		goto cleanup;
	}
	InsertTailList(&DriverExt->PreviewSessionList, &session->Entry);
	ExReleaseFastMutex(&DriverExt->PreviewSessionMutex);

	if (InterlockedCompareExchange(
		&sourceExt->MergeThreadRunning, 0, 0) != 0)
	{
		status = STATUS_DEVICE_BUSY;
		ExAcquireFastMutex(&DriverExt->PreviewSessionMutex);
		RemoveEntryList(&session->Entry);
		ExReleaseFastMutex(&DriverExt->PreviewSessionMutex);
		goto cleanup;
	}
	status = CdpCorePreviewBegin(sourceExt->Core, targetTime);
	if (!NT_SUCCESS(status))
	{
		ExAcquireFastMutex(&DriverExt->PreviewSessionMutex);
		RemoveEntryList(&session->Entry);
		ExReleaseFastMutex(&DriverExt->PreviewSessionMutex);
		goto cleanup;
	}
	session->TargetTime100ns = CdpCoreGetTargetTime100ns(sourceExt->Core);
	Reply->PreviewHandle = session->HandleId;
	Reply->TargetTime100ns = session->TargetTime100ns;
	Reply->OldestRecoverable100ns = oldestTime;
	Reply->NewestRecoverable100ns = newestTime;

	Cdp_DBG("[PREVIEW] begin handle=%llu target=%llu (Core)\n",
		session->HandleId,
		session->TargetTime100ns);
	return STATUS_SUCCESS;

cleanup:
	if (session)
	{
		cdpfree(session);
		journalEntry = NULL;
		sourceHandleId = 0;
	}
	if (phaseTransitioned && sourceExt)
	{
		if (sourceExt->Core)
			(void)CdpCorePreviewEnd(sourceExt->Core);
		InterlockedExchange(&sourceExt->Phase, (LONG)Cdp_PHASE_GENERAL);
	}
	if (sourceHandleId)
		(void)CdpCloseVolumeHandle(DriverExt, sourceHandleId);
	if (journalEntry)
		CdpReleaseVolumeHandleEntry(journalEntry);
	return status;
}

static NTSTATUS CdpEndPreviewSession(
	_In_ PCdp_DRIVER_EXTENSION DriverExt,
	_In_ UINT64 HandleId)
{
	PCdp_PREVIEW_SESSION session;
	GUID sourceGuid;
	BOOLEAN haveGuid = FALSE;

	ExAcquireFastMutex(&DriverExt->PreviewSessionMutex);
	session = CdpLookupPreviewSessionLocked(DriverExt, HandleId);
	if (session)
	{
		RemoveEntryList(&session->Entry);
		session->Closing = TRUE;
		sourceGuid = session->SourceVolumeGuid;
		haveGuid = TRUE;
	}
	ExReleaseFastMutex(&DriverExt->PreviewSessionMutex);
	if (!session)
		return STATUS_NOT_FOUND;

	CdpDestroyPreviewSession(DriverExt, session);

	if (haveGuid)
	{
		PCdp_DEVICE_EXTENSION sourceExt =
			CdpFindSourceExtensionByGuid(DriverExt, &sourceGuid);
		if (sourceExt)
		{
			if (sourceExt->Core)
				(void)CdpCorePreviewEnd(sourceExt->Core);
			InterlockedExchange(&sourceExt->Phase, (LONG)Cdp_PHASE_GENERAL);
		}
	}

	Cdp_DBG("[PREVIEW] end handle=%llu\n", HandleId);
	return STATUS_SUCCESS;
}

VOID CdpCloseAllPreviewSessions(_In_ PCdp_DRIVER_EXTENSION DriverExt)
{
	for (;;)
	{
		PCdp_PREVIEW_SESSION session = NULL;
		GUID sourceGuid;
		BOOLEAN haveGuid = FALSE;

		ExAcquireFastMutex(&DriverExt->PreviewSessionMutex);
		if (!IsListEmpty(&DriverExt->PreviewSessionList))
		{
			PLIST_ENTRY entry =
				RemoveHeadList(&DriverExt->PreviewSessionList);
			session = CONTAINING_RECORD(
				entry,
				Cdp_PREVIEW_SESSION,
				Entry);
			session->Closing = TRUE;
			sourceGuid = session->SourceVolumeGuid;
			haveGuid = TRUE;
		}
		ExReleaseFastMutex(&DriverExt->PreviewSessionMutex);
		if (!session)
			break;

		if (haveGuid)
		{
			PCdp_DEVICE_EXTENSION sourceExt =
				CdpFindSourceExtensionByGuid(DriverExt, &sourceGuid);
			if (sourceExt)
			{
				if (sourceExt->Core)
					(void)CdpCorePreviewEnd(sourceExt->Core);
				InterlockedExchange(&sourceExt->Phase, (LONG)Cdp_PHASE_GENERAL);
			}
		}

		CdpDestroyPreviewSession(DriverExt, session);
	}
}

static NTSTATUS CdpReadPreviewSession(
	_In_ PCdp_DRIVER_EXTENSION DriverExt,
	_In_ const Cdp_PREVIEW_READ_REQUEST* Request,
	_Out_writes_bytes_(Request->ByteLength) PVOID OutputBuffer)
{
	PCdp_PREVIEW_SESSION session;
	PCdp_DEVICE_EXTENSION sourceExt = NULL;
	BOOLEAN historyLocked = FALSE;
	NTSTATUS status;

	if (!Request->ByteLength ||
		Request->ByteLength > Cdp_CMD3_MAX_READ_BYTES)
	{
		return STATUS_INVALID_PARAMETER;
	}

	session = CdpAcquirePreviewSession(
		DriverExt,
		Request->PreviewHandle);
	if (!session)
		return STATUS_INVALID_HANDLE;

	sourceExt = CdpFindSourceExtensionByGuid(
		DriverExt,
		&session->SourceVolumeGuid);
	if (!sourceExt || !sourceExt->Core)
	{
		status = STATUS_DEVICE_NOT_READY;
		goto cleanup;
	}

	KeWaitForSingleObject(
		&sourceExt->HistoryMutex,
		Executive,
		KernelMode,
		FALSE,
		NULL);
	historyLocked = TRUE;

	if (Request->ByteOffset > sourceExt->PartitionSize ||
		Request->ByteLength > sourceExt->PartitionSize - Request->ByteOffset ||
		sourceExt->PartitionStart > MAXUINT64 - Request->ByteOffset)
	{
		status = STATUS_INVALID_PARAMETER;
		goto cleanup;
	}

	status = CdpCoreReadAlignedView(
		sourceExt,
		TRUE,
		sourceExt->PartitionStart + Request->ByteOffset,
		Request->ByteLength,
		OutputBuffer);
	if (NT_SUCCESS(status))
	{
		Cdp_DBG("[PREVIEW] core read handle=%llu offset=%llu len=%lu\n",
			Request->PreviewHandle,
			Request->ByteOffset,
			Request->ByteLength);
	}

cleanup:
	if (historyLocked && sourceExt)
		KeReleaseMutex(&sourceExt->HistoryMutex, FALSE);
	CdpReleasePreviewSession(session);
	return status;
}

static NTSTATUS CdpSendToNextDevice(_In_ PDEVICE_OBJECT DeviceObject, _Inout_ PIRP Irp)
{
	IoSkipCurrentIrpStackLocation(Irp);
	return IoCallDriver(DeviceObject, Irp);
}

NTSTATUS CdpIrpDispatchDefault(_In_ PDEVICE_OBJECT DeviceObject, _Inout_ PIRP Irp)
{
	PCdp_DEVICE_EXTENSION DeviceExtension = (PCdp_DEVICE_EXTENSION)DeviceObject->DeviceExtension;

	if (DeviceExtension && DeviceExtension->LowerDeviceObject)
		return CdpSendToNextDevice(DeviceExtension->LowerDeviceObject, Irp);

	return CdpCompleteIrp(Irp, STATUS_SUCCESS, 0);
}

NTSTATUS CdpIrpDispatchCreateClose(_In_ PDEVICE_OBJECT DeviceObject, _Inout_ PIRP Irp)
{
	PCdp_DRIVER_EXTENSION driverExt =
		IoGetDriverObjectExtension(g_DriverObject, &g_DriverObject);
	PIO_STACK_LOCATION irpSp = IoGetCurrentIrpStackLocation(Irp);

	if (driverExt && DeviceObject == driverExt->ControlDevice)
	{
		if (irpSp->MajorFunction == IRP_MJ_CREATE)
		{
			PCdp_CONTROL_FILE_CONTEXT context =
				(PCdp_CONTROL_FILE_CONTEXT)cdpalloc(sizeof(*context));
			if (!context)
				return CdpCompleteIrp(Irp, STATUS_INSUFFICIENT_RESOURCES, 0);
			RtlZeroMemory(context, sizeof(*context));
			irpSp->FileObject->FsContext = context;
		}
		else if (irpSp->MajorFunction == IRP_MJ_CLOSE)
		{
			PCdp_CONTROL_FILE_CONTEXT context =
				(PCdp_CONTROL_FILE_CONTEXT)irpSp->FileObject->FsContext;
			irpSp->FileObject->FsContext = NULL;
			if (context)
			{
				RtlSecureZeroMemory(context, sizeof(*context));
				cdpfree(context);
			}
		}
		return CdpCompleteIrp(Irp, STATUS_SUCCESS, 0);
	}
	return CdpIrpDispatchDefault(DeviceObject, Irp);
}

static NTSTATUS CdpBeginRecovery(
	_In_ PCdp_DRIVER_EXTENSION DriverExt,
	_In_ const Cdp_RECOVERY_BEGIN_REQUEST* Request,
	_Out_ PCdp_RECOVERY_BEGIN_REPLY Reply)
{
	PCdp_DEVICE_EXTENSION sourceExt;
	PCdp_VOLUME_HANDLE_ENTRY journalEntry;
	UINT64 oldestTime = 0;
	UINT64 newestTime = 0;
	UINT64 targetTime = Request->TargetTime100ns;
	LONG previousPhase;
	NTSTATUS status;

	RtlZeroMemory(Reply, sizeof(*Reply));
	if ((Request->Flags & ~Cdp_RECOVERY_BEGIN_FLAG_ON_REBOOT) != 0)
		return STATUS_INVALID_PARAMETER;
	sourceExt = CdpFindSourceExtensionByGuid(
		DriverExt,
		&Request->SourceVolumeGuid);
	if (!sourceExt)
		return STATUS_DEVICE_DOES_NOT_EXIST;
	if (InterlockedCompareExchange(&sourceExt->CaptureEnabled, 0, 0) == 0)
		return STATUS_DEVICE_NOT_READY;
	if (!sourceExt->Core)
		return STATUS_DEVICE_NOT_READY;
	if ((Request->Flags & Cdp_RECOVERY_BEGIN_FLAG_ON_REBOOT) != 0)
	{
		status = CdpCoreQueryTimeRange(sourceExt->Core, &oldestTime, &newestTime);
		if (status == STATUS_NOT_FOUND)
		{
			oldestTime = 0;
			newestTime = 0;
			status = STATUS_SUCCESS;
		}
		if (!NT_SUCCESS(status))
			return status;
		if (oldestTime != 0 && targetTime < oldestTime)
			targetTime = oldestTime;
		journalEntry = CdpAcquireJournalForSource(DriverExt, sourceExt);
		if (!journalEntry)
			return STATUS_DEVICE_NOT_READY;
		status = CdpJournalSetRecoveryIntent(
			&journalEntry->Journal, targetTime);
		CdpReleaseVolumeHandleEntry(journalEntry);
		if (!NT_SUCCESS(status))
			return status;
		Reply->Phase = Cdp_PHASE_GENERAL;
		Reply->TargetTime100ns = targetTime;
		Reply->OldestRecoverable100ns = oldestTime;
		Reply->NewestRecoverable100ns = newestTime;
		Cdp_LOG("[RECOVERY] reboot intent persisted target=%llu; no begin before reboot\n",
			targetTime);
		return STATUS_SUCCESS;
	}
	if (CdpAnyPreviewSessionActive(DriverExt))
		return STATUS_INVALID_DEVICE_STATE;
	Cdp_LOG(
		"[RECOVERY] begin entering recovery pagingPathCount=%ld\n",
		InterlockedCompareExchange(&sourceExt->PagingPathCount, 0, 0));
	previousPhase = InterlockedCompareExchange(
		&sourceExt->Phase,
		(LONG)Cdp_PHASE_RECOVERY,
		(LONG)Cdp_PHASE_GENERAL);
	if (previousPhase != (LONG)Cdp_PHASE_GENERAL)
		return STATUS_INVALID_DEVICE_STATE;
	// Phase closes the merge restart window first. New read/write IRPs remain
	// queued behind HistoryMutex while the existing merge worker is stopped.
	CdpStopMergeThread(sourceExt);

	status = CdpCoreQueryTimeRange(
		sourceExt->Core,
		&oldestTime,
		&newestTime);
	if (status == STATUS_NOT_FOUND)
	{
		oldestTime = 0;
		newestTime = 0;
		status = STATUS_SUCCESS;
	}
	else if (!NT_SUCCESS(status))
	{
		InterlockedExchange(&sourceExt->Phase, previousPhase);
		return status;
	}
	else if (targetTime < oldestTime)
		targetTime = oldestTime;

	KeWaitForSingleObject(
		&sourceExt->HistoryMutex,
		Executive,
		KernelMode,
		FALSE,
		NULL);
	status = CdpCoreRecoveryBegin(sourceExt->Core, targetTime);
	if (NT_SUCCESS(status))
	{
		journalEntry = CdpAcquireJournalForSource(DriverExt, sourceExt);
		if (journalEntry && journalEntry->Journal.RecoveryPending)
			status = CdpJournalCompleteRecoveryIntent(&journalEntry->Journal);
		if (journalEntry)
			CdpReleaseVolumeHandleEntry(journalEntry);
		if (NT_SUCCESS(status))
			InterlockedExchange(&sourceExt->Phase, (LONG)Cdp_PHASE_GENERAL);
	}
	KeReleaseMutex(&sourceExt->HistoryMutex, FALSE);
	if (!NT_SUCCESS(status))
	{
		InterlockedExchange(&sourceExt->Phase, previousPhase);
		return status;
	}

	Reply->Phase = Cdp_PHASE_GENERAL;
	targetTime = CdpCoreGetTargetTime100ns(sourceExt->Core);
	Reply->TargetTime100ns = targetTime;
	Reply->OldestRecoverable100ns = oldestTime;
	Reply->NewestRecoverable100ns = newestTime;
	Cdp_LOG("[PHASE] recovery branch switch complete target=%llu -> normal\n",
		targetTime);
	return STATUS_SUCCESS;
}

static NTSTATUS CdpCommitRecovery(
	_In_ PCdp_DRIVER_EXTENSION DriverExt,
	_In_ const Cdp_RECOVERY_CONTROL_REQUEST* Request,
	_Out_ PCdp_RECOVERY_COMMIT_REPLY Reply)
{
	PCdp_DEVICE_EXTENSION sourceExt;
	UINT64 targetTime;
	NTSTATUS status;
	BOOLEAN complete = FALSE;

	RtlZeroMemory(Reply, sizeof(*Reply));
	sourceExt = CdpFindSourceExtensionByGuid(
		DriverExt,
		&Request->SourceVolumeGuid);
	if (!sourceExt)
		return STATUS_DEVICE_DOES_NOT_EXIST;
	if (!sourceExt->Core)
		return STATUS_DEVICE_NOT_READY;
	if (InterlockedCompareExchange(&sourceExt->Phase, 0, 0) !=
		(LONG)Cdp_PHASE_GENERAL)
		return STATUS_INVALID_DEVICE_STATE;
	KeWaitForSingleObject(
		&sourceExt->HistoryMutex,
		Executive,
		KernelMode,
		FALSE,
		NULL);
	targetTime = CdpCoreGetTargetTime100ns(sourceExt->Core);
	status = CdpCoreRecoveryCommitStep(sourceExt->Core, &complete);
	KeReleaseMutex(&sourceExt->HistoryMutex, FALSE);
	if (!NT_SUCCESS(status) || !complete)
		return NT_SUCCESS(status) ? STATUS_UNSUCCESSFUL : status;

	/* A reboot recovery's branch switch has already been committed by Begin. */
	InterlockedExchange(&sourceExt->Phase, (LONG)Cdp_PHASE_GENERAL);
	Reply->Phase = Cdp_PHASE_GENERAL;
	Reply->TargetTime100ns = targetTime;
	Cdp_LOG("[PHASE] recovery commit acknowledged target=%llu; no writeback\n",
		targetTime);
	return STATUS_SUCCESS;
}

static NTSTATUS CdpCancelRecovery(
	_In_ PCdp_DRIVER_EXTENSION DriverExt,
	_In_ const Cdp_RECOVERY_CONTROL_REQUEST* Request)
{
	PCdp_DEVICE_EXTENSION sourceExt;
	PCdp_VOLUME_HANDLE_ENTRY journalEntry;
	LONG phase;
	NTSTATUS status;

	sourceExt = CdpFindSourceExtensionByGuid(
		DriverExt,
		&Request->SourceVolumeGuid);
	if (!sourceExt)
		return STATUS_DEVICE_DOES_NOT_EXIST;
	phase = InterlockedCompareExchange(&sourceExt->Phase, 0, 0);
	if (phase != (LONG)Cdp_PHASE_RECOVERY)
	{
		// A reboot Recovery request only persists the intent and deliberately
		// leaves the source in General phase.  CMD 'c' must be able to cancel
		// that pending intent before reboot.
		if (phase == (LONG)Cdp_PHASE_GENERAL)
		{
			journalEntry = CdpAcquireJournalForSource(DriverExt, sourceExt);
			if (journalEntry && journalEntry->Journal.RecoveryPending)
			{
				status = CdpJournalClearRecoveryIntent(&journalEntry->Journal);
				CdpReleaseVolumeHandleEntry(journalEntry);
				if (NT_SUCCESS(status))
					Cdp_LOG("[RECOVERY] pending reboot intent cancelled in General phase\n");
				return status;
			}
			if (journalEntry)
				CdpReleaseVolumeHandleEntry(journalEntry);
		}
		return STATUS_INVALID_DEVICE_STATE;
	}
	// RecoveryBegin is now a synchronous branch switch. There is no prepared
	// history state that can be cancelled while Phase is Recovery.
	return STATUS_DEVICE_BUSY;
}

static NTSTATUS CdpQueryTimeRange(
	_In_ PCdp_DRIVER_EXTENSION DriverExt,
	_In_ const Cdp_TIME_RANGE_QUERY_REQUEST* Request,
	_Out_ PCdp_TIME_RANGE_QUERY_REPLY Reply)
{
	PCdp_DEVICE_EXTENSION sourceExt;
	NTSTATUS status;

	RtlZeroMemory(Reply, sizeof(*Reply));
	sourceExt = CdpFindSourceExtensionByGuid(
		DriverExt,
		&Request->SourceVolumeGuid);
	if (!sourceExt)
		return STATUS_DEVICE_DOES_NOT_EXIST;
	if (InterlockedCompareExchange(&sourceExt->CaptureEnabled, 0, 0) == 0)
		return STATUS_DEVICE_NOT_READY;
	if (!sourceExt->Core)
		return STATUS_DEVICE_NOT_READY;

	status = CdpCoreQueryTimeRange(
		sourceExt->Core,
		&Reply->OldestRecord100ns,
		&Reply->NewestRecord100ns);
	if (status == STATUS_NOT_FOUND)
	{
		Reply->HasRecords = 0;
		Reply->OldestRecord100ns = 0;
		Reply->NewestRecord100ns = 0;
		return STATUS_SUCCESS;
	}
	if (!NT_SUCCESS(status))
		return status;

	Reply->HasRecords = 1;
	return STATUS_SUCCESS;
}

static NTSTATUS CdpQueryJournalUsage(
	_In_ PCdp_DRIVER_EXTENSION DriverExt,
	_In_ const Cdp_JOURNAL_USAGE_QUERY_REQUEST* Request,
	_Out_ PCdp_JOURNAL_USAGE_QUERY_REPLY Reply)
{
	PCdp_DEVICE_EXTENSION sourceExt;

	RtlZeroMemory(Reply, sizeof(*Reply));
	sourceExt = CdpFindSourceExtensionByGuid(
		DriverExt,
		&Request->SourceVolumeGuid);
	if (!sourceExt)
		return STATUS_DEVICE_DOES_NOT_EXIST;
	if (InterlockedCompareExchange(&sourceExt->CaptureEnabled, 0, 0) == 0 ||
		!sourceExt->Core)
	{
		return STATUS_DEVICE_NOT_READY;
	}

	return CdpCoreQueryJournalUsage(
		sourceExt->Core,
		&Reply->JournalPartitionBytes,
		&Reply->JournalMetadataBytes,
		&Reply->RecordPayloadBytesUsed,
		&Reply->RecordPayloadBytesFree,
		&Reply->TotalRecords);
}

static NTSTATUS CdpQueryJournalRecords(
	_In_ PCdp_DRIVER_EXTENSION DriverExt,
	_In_ const Cdp_JOURNAL_RECORD_QUERY_REQUEST* Request,
	_Out_ PCdp_JOURNAL_RECORD_QUERY_REPLY Reply,
	_In_ ULONG RecordCapacity)
{
	PCdp_DEVICE_EXTENSION sourceExt;
	PCdp_JOURNAL_RECORD records;

	C_ASSERT(sizeof(Cdp_JOURNAL_RECORD_INFO) ==
		sizeof(Cdp_JOURNAL_RECORD));
	RtlZeroMemory(Reply, sizeof(*Reply));
	sourceExt = CdpFindSourceExtensionByGuid(
		DriverExt,
		&Request->SourceVolumeGuid);
	if (!sourceExt)
		return STATUS_DEVICE_DOES_NOT_EXIST;
	if (InterlockedCompareExchange(&sourceExt->CaptureEnabled, 0, 0) == 0 ||
		!sourceExt->Core)
	{
		return STATUS_DEVICE_NOT_READY;
	}

	if (RecordCapacity > Request->MaxRecords)
		RecordCapacity = Request->MaxRecords;
	if (RecordCapacity > Cdp_JOURNAL_RECORD_QUERY_MAX_PER_CALL)
		RecordCapacity = Cdp_JOURNAL_RECORD_QUERY_MAX_PER_CALL;

	records = (PCdp_JOURNAL_RECORD)(Reply + 1);
	return CdpCoreQueryRecordHeaders(
		sourceExt->Core,
		Request->StartIndex,
		Request->ExpectedGeneration,
		records,
		RecordCapacity,
		&Reply->TotalRecords,
		&Reply->Generation,
		&Reply->RecordCount);
}

static NTSTATUS CdpQueryJournalBranches(
	_In_ PCdp_DRIVER_EXTENSION DriverExt,
	_In_ const Cdp_JOURNAL_BRANCH_QUERY_REQUEST* Request,
	_Out_ PCdp_JOURNAL_BRANCH_QUERY_REPLY Reply,
	_In_ ULONG BranchCapacity)
{
	PCdp_DEVICE_EXTENSION sourceExt;
	PCdp_JOURNAL_BRANCH_INFO branches;

	C_ASSERT(sizeof(Cdp_JOURNAL_BRANCH_INFO) ==
		sizeof(Cdp_JOURNAL_BRANCH_TREE_INFO));
	RtlZeroMemory(Reply, sizeof(*Reply));
	sourceExt = CdpFindSourceExtensionByGuid(
		DriverExt,
		&Request->SourceVolumeGuid);
	if (!sourceExt)
		return STATUS_DEVICE_DOES_NOT_EXIST;
	if (InterlockedCompareExchange(&sourceExt->CaptureEnabled, 0, 0) == 0 ||
		!sourceExt->Core)
	{
		return STATUS_DEVICE_NOT_READY;
	}

	if (BranchCapacity > Request->MaxBranches)
		BranchCapacity = Request->MaxBranches;
	if (BranchCapacity > Cdp_JOURNAL_BRANCH_QUERY_MAX_PER_CALL)
		BranchCapacity = Cdp_JOURNAL_BRANCH_QUERY_MAX_PER_CALL;

	branches = (PCdp_JOURNAL_BRANCH_INFO)(Reply + 1);
	return CdpCoreQueryBranches(
		sourceExt->Core,
		Request->StartIndex,
		Request->ExpectedGeneration,
		(PCdp_JOURNAL_BRANCH_TREE_INFO)branches,
		BranchCapacity,
		&Reply->TotalBranches,
		&Reply->CurrentBranchNumber,
		&Reply->Generation,
		&Reply->BranchCount);
}

static NTSTATUS CdpQueryPhase(
	_In_ PCdp_DRIVER_EXTENSION DriverExt,
	_In_ const Cdp_PHASE_QUERY_REQUEST* Request,
	_Out_ PCdp_PHASE_QUERY_REPLY Reply)
{
	PCdp_DEVICE_EXTENSION sourceExt;
	PCdp_VOLUME_HANDLE_ENTRY journalEntry = NULL;
	UINT64 journalHandleId = 0;

	RtlZeroMemory(Reply, sizeof(*Reply));
	Reply->Status = Cdp_STATUS_UNPROTECTED;

	sourceExt = CdpFindSourceExtensionByGuid(
		DriverExt,
		&Request->SourceVolumeGuid);
	if (!sourceExt ||
		InterlockedCompareExchange(&sourceExt->CaptureEnabled, 0, 0) == 0)
	{
		return STATUS_SUCCESS;
	}

	Reply->Status = (LONG)InterlockedCompareExchange(&sourceExt->Phase, 0, 0);
	if (sourceExt->Core)
		Reply->RecoveryTargetTime100ns =
			CdpCoreGetTargetTime100ns(sourceExt->Core);

	ExAcquireFastMutex(&DriverExt->VolumeHandleMutex);
	journalHandleId = sourceExt->JournalHandleId;
	if (journalHandleId != 0)
		journalEntry = CdpLookupVolumeHandleLocked(DriverExt, journalHandleId);
	if (journalEntry && journalEntry->VolumeGuidValid)
		Reply->JournalPartitionGuid = journalEntry->VolumeGuid;
	if (journalEntry)
	{
		Reply->JournalDiskNumber = journalEntry->DiskNumber;
		Reply->JournalPartitionNumber = journalEntry->PartitionNumber;
		Reply->JournalPartitionOffset = journalEntry->TargetBaseOffset;
		Reply->JournalPartitionBytes = journalEntry->PartitionSize;
	}
	ExReleaseFastMutex(&DriverExt->VolumeHandleMutex);
	return STATUS_SUCCESS;
}

/* The caller holds HistoryMutex.  CdpCore operates on sector-aligned ranges,
 * while preview/recovery clients may request an arbitrary byte subrange. */
static NTSTATUS CdpCoreReadAlignedView(
	_In_ PCdp_DEVICE_EXTENSION DevExt,
	_In_ BOOLEAN Preview,
	_In_ UINT64 Offset,
	_In_ ULONG Length,
	_Out_writes_bytes_(Length) PVOID Buffer)
{
	NTSTATUS status = STATUS_SUCCESS;
	ULONG completed = 0;
	ULONG sectorSize;

	if (!DevExt->Core)
		return STATUS_DEVICE_NOT_READY;
	sectorSize = DevExt->SectorSize;
	if (sectorSize != 512 && sectorSize != 4096)
		return STATUS_INVALID_DEVICE_REQUEST;

	while (completed < Length)
	{
		UINT64 requestOffset = Offset + completed;
		UINT64 alignedOffset = requestOffset - (requestOffset % sectorSize);
		ULONG prefix = (ULONG)(requestOffset - alignedOffset);
		ULONG chunk = Length - completed;
		ULONG span;
		PVOID alignedBuffer;
		if (chunk > Cdp_CMD3_MAX_READ_BYTES - prefix)
			chunk = Cdp_CMD3_MAX_READ_BYTES - prefix;
		span = prefix + chunk;
		span = (span + sectorSize - 1) / sectorSize * sectorSize;
		alignedBuffer = cdpalloc(span);
		if (!alignedBuffer)
		{
			status = STATUS_INSUFFICIENT_RESOURCES;
			break;
		}
		status = Preview ?
			CdpCorePreviewRead(
				DevExt->Core, alignedOffset, span, alignedBuffer) :
			CdpCoreRead(
				DevExt->Core, alignedOffset, span, alignedBuffer);
		if (NT_SUCCESS(status))
			RtlCopyMemory((PUCHAR)Buffer + completed,
				(PUCHAR)alignedBuffer + prefix, chunk);
		cdpfree(alignedBuffer);
		if (!NT_SUCCESS(status))
			break;
		completed += chunk;
	}
	return status;
}

static PCdp_DEVICE_EXTENSION CdpReferenceProtectedSourceForDiskIo(
	_In_ PCdp_DEVICE_EXTENSION DiskExt,
	_In_ UINT64 AbsoluteOffset,
	_In_ ULONG Length,
	_Out_ PDEVICE_OBJECT* SourceReference)
{
	PCdp_DRIVER_EXTENSION driverExt =
		IoGetDriverObjectExtension(g_DriverObject, &g_DriverObject);
	KIRQL oldIrql;
	PLIST_ENTRY entry;
	PCdp_DEVICE_EXTENSION found = NULL;

	*SourceReference = NULL;
	if (!driverExt || !DiskExt || Length == 0 ||
		AbsoluteOffset > MAXUINT64 - Length)
		return NULL;
	KeAcquireSpinLock(&driverExt->DeviceObjectListLock, &oldIrql);
	for (entry = driverExt->DeviceObjectListHead.Flink;
		entry != &driverExt->DeviceObjectListHead;
		entry = entry->Flink)
	{
		PCdp_DEVICE_LIST_NODE node =
			CONTAINING_RECORD(entry, Cdp_DEVICE_LIST_NODE, Entry);
		PCdp_DEVICE_EXTENSION ext =
			(PCdp_DEVICE_EXTENSION)node->DeviceObject->DeviceExtension;
		UINT64 partitionEnd;
		BOOLEAN capturePath;
		if (!ext || (ext->DeviceKind != Cdp_DEVICE_KIND_VOLUME &&
			ext->DeviceKind != Cdp_DEVICE_KIND_DISK) ||
			ext->DiskNumber != DiskExt->DiskNumber ||
			InterlockedCompareExchange(&ext->DiskIoAccepting, 0, 0) == 0 ||
			InterlockedCompareExchange(&ext->CaptureStopping, 0, 0) != 0 ||
			!ext->Core || ext->PartitionSize == 0 ||
			ext->PartitionStart > MAXUINT64 - ext->PartitionSize)
			continue;
		capturePath =
			InterlockedCompareExchange(
				&ext->ProtectionStateValidated, 0, 0) != 0 &&
			InterlockedCompareExchange(&ext->CaptureEnabled, 0, 0) != 0;
		if (!capturePath)
			continue;
		partitionEnd = ext->PartitionStart + ext->PartitionSize;
		if (AbsoluteOffset >= ext->PartitionStart &&
			AbsoluteOffset + Length <= partitionEnd)
		{
			ObReferenceObject(node->DeviceObject);
			*SourceReference = node->DeviceObject;
			found = ext;
			break;
		}
	}
	KeReleaseSpinLock(&driverExt->DeviceObjectListLock, oldIrql);
	return found;
}

static PCdp_DEVICE_EXTENSION CdpReferenceProtectedSourceForDiskFlush(
	_In_ PCdp_DEVICE_EXTENSION DiskExt,
	_Out_ PDEVICE_OBJECT* SourceReference)
{
	PCdp_DRIVER_EXTENSION driverExt =
		IoGetDriverObjectExtension(g_DriverObject, &g_DriverObject);
	KIRQL oldIrql;
	PLIST_ENTRY entry;
	PCdp_DEVICE_EXTENSION found = NULL;

	*SourceReference = NULL;
	if (!driverExt || !DiskExt)
		return NULL;
	KeAcquireSpinLock(&driverExt->DeviceObjectListLock, &oldIrql);
	for (entry = driverExt->DeviceObjectListHead.Flink;
		entry != &driverExt->DeviceObjectListHead;
		entry = entry->Flink)
	{
		PCdp_DEVICE_LIST_NODE node =
			CONTAINING_RECORD(entry, Cdp_DEVICE_LIST_NODE, Entry);
		PCdp_DEVICE_EXTENSION ext =
			(PCdp_DEVICE_EXTENSION)node->DeviceObject->DeviceExtension;
		BOOLEAN capturePath;

		if (!ext || (ext->DeviceKind != Cdp_DEVICE_KIND_VOLUME &&
			ext->DeviceKind != Cdp_DEVICE_KIND_DISK) ||
			ext->DiskNumber != DiskExt->DiskNumber ||
			InterlockedCompareExchange(&ext->DiskIoAccepting, 0, 0) == 0 ||
			InterlockedCompareExchange(&ext->CaptureStopping, 0, 0) != 0 ||
			!ext->Core)
		{
			continue;
		}
		capturePath = InterlockedCompareExchange(
			&ext->ProtectionStateValidated, 0, 0) != 0 &&
			InterlockedCompareExchange(&ext->CaptureEnabled, 0, 0) != 0;
		if (!capturePath)
			continue;
		ObReferenceObject(node->DeviceObject);
		*SourceReference = node->DeviceObject;
		found = ext;
		break;
	}
	KeReleaseSpinLock(&driverExt->DeviceObjectListLock, oldIrql);
	return found;
}

static BOOLEAN CdpAcquireDiskIoOutstanding(
	_Inout_ PCdp_DEVICE_EXTENSION SourceExt)
{
	LONG outstanding;

	if (!SourceExt || InterlockedCompareExchange(
			&SourceExt->DiskIoAccepting, 0, 0) == 0)
	{
		return FALSE;
	}
	outstanding = InterlockedIncrement(&SourceExt->DiskIoOutstanding);
	if (outstanding == 1)
		KeClearEvent(&SourceExt->DiskIoDrainedEvent);
	if (InterlockedCompareExchange(
			&SourceExt->DiskIoAccepting, 0, 0) == 0)
	{
		if (InterlockedDecrement(&SourceExt->DiskIoOutstanding) == 0)
			KeSetEvent(&SourceExt->DiskIoDrainedEvent, IO_NO_INCREMENT, FALSE);
		return FALSE;
	}
	return TRUE;
}

static VOID CdpReleaseDiskIoOutstanding(
	_Inout_ PCdp_DEVICE_EXTENSION SourceExt)
{
	if (SourceExt &&
		InterlockedDecrement(&SourceExt->DiskIoOutstanding) == 0)
	{
		KeSetEvent(&SourceExt->DiskIoDrainedEvent, IO_NO_INCREMENT, FALSE);
	}
}

typedef struct _Cdp_DISK_READ_OVERLAY_CONTEXT
{
	PIRP Irp;
	PIO_WORKITEM WorkItem;
	PDEVICE_OBJECT SourceReference;
	PDEVICE_OBJECT LowerReference;
	UINT64 AbsoluteOffset;
	ULONG Length;
} Cdp_DISK_READ_OVERLAY_CONTEXT, *PCdp_DISK_READ_OVERLAY_CONTEXT;

static NTSTATUS CdpReadDiskLowerSynchronously(
	_In_ PDEVICE_OBJECT LowerDevice,
	_In_ UINT64 AbsoluteOffset,
	_In_ ULONG Length,
	_Out_writes_bytes_(Length) PVOID Buffer)
{
	KEVENT event;
	IO_STATUS_BLOCK iosb;
	LARGE_INTEGER byteOffset;
	PIRP readIrp;
	NTSTATUS status;

	if (!LowerDevice || !Buffer || Length == 0 ||
		AbsoluteOffset > MAXLONGLONG)
	{
		return STATUS_INVALID_PARAMETER;
	}
	KeInitializeEvent(&event, NotificationEvent, FALSE);
	RtlZeroMemory(&iosb, sizeof(iosb));
	byteOffset.QuadPart = (LONGLONG)AbsoluteOffset;
	readIrp = IoBuildSynchronousFsdRequest(
		IRP_MJ_READ,
		LowerDevice,
		Buffer,
		Length,
		&byteOffset,
		&event,
		&iosb);
	if (!readIrp)
		return STATUS_INSUFFICIENT_RESOURCES;
	status = IoCallDriver(LowerDevice, readIrp);
	if (status == STATUS_PENDING)
	{
		KeWaitForSingleObject(
			&event, Executive, KernelMode, FALSE, NULL);
		status = iosb.Status;
	}
	else if (NT_SUCCESS(status))
	{
		status = iosb.Status;
	}
	if (NT_SUCCESS(status) && iosb.Information != Length)
		status = STATUS_UNEXPECTED_IO_ERROR;
	return status;
}

static VOID CdpDiskReadOverlayWorker(
	_In_ PDEVICE_OBJECT DeviceObject,
	_In_opt_ PVOID Context)
{
	PCdp_DISK_READ_OVERLAY_CONTEXT readContext =
		(PCdp_DISK_READ_OVERLAY_CONTEXT)Context;
	PCdp_DEVICE_EXTENSION sourceExt = NULL;
	PIRP irp = NULL;
	PUCHAR buffer = NULL;
	ULONG mdlCount = 0;
	UINT64 mdlBytes = 0;
	ULONG copiedBytes = 0;
	NTSTATUS status;

	UNREFERENCED_PARAMETER(DeviceObject);
	if (!readContext)
		return;
	irp = readContext->Irp;
	if (readContext->SourceReference)
		sourceExt = (PCdp_DEVICE_EXTENSION)
			readContext->SourceReference->DeviceExtension;
	status = irp ? STATUS_SUCCESS : STATUS_INVALID_PARAMETER;

	/* The application IRP never enters Disk Lower. Read the baseline with a
	 * private synchronous IRP, overlay journal hits, then touch the original
	 * MDL exactly once immediately before completing it. */
	if (NT_SUCCESS(status))
	{
		if (!sourceExt || !sourceExt->Core || !readContext->LowerReference)
		{
			status = STATUS_DEVICE_NOT_READY;
		}
		else
		{
			buffer = (PUCHAR)cdpalloc(readContext->Length);
			status = buffer ? CdpReadDiskLowerSynchronously(
				readContext->LowerReference,
				readContext->AbsoluteOffset,
				readContext->Length,
				buffer) : STATUS_INSUFFICIENT_RESOURCES;
			if (NT_SUCCESS(status))
			{
				KeWaitForSingleObject(
					&sourceExt->HistoryMutex,
					Executive, KernelMode, FALSE, NULL);
				status = CdpCoreOverlayCurrentRead(
					sourceExt->Core,
					readContext->AbsoluteOffset,
					readContext->Length,
					buffer);
				CdpAuditProtectedReadCoreResult(
					sourceExt, readContext->Length, status);
				KeReleaseMutex(&sourceExt->HistoryMutex, FALSE);
			}
			if (NT_SUCCESS(status))
			{
				status = CdpScatterReadMdlChain(
					irp, buffer, readContext->Length,
					&mdlCount, &mdlBytes, &copiedBytes);
			}
		}
	}
	if (!NT_SUCCESS(status))
	{
		Cdp_LOG("[DISK-READ-OVERLAY-FAIL] status=0x%08X absoluteOffset=%llu len=%lu mdl=%p mdlCount=%lu mdlBytes=%llu copied=%lu flags=0x%08lX\n",
			status,
			readContext->AbsoluteOffset,
			readContext->Length,
			irp ? irp->MdlAddress : NULL,
			mdlCount,
			mdlBytes,
			copiedBytes,
			irp ? irp->Flags : 0);
	}
	if (buffer)
		cdpfree(buffer);
	if (irp)
	{
		irp->IoStatus.Status = status;
		irp->IoStatus.Information = NT_SUCCESS(status) ?
			readContext->Length : 0;
		IoCompleteRequest(irp, IO_DISK_INCREMENT);
	}
	if (sourceExt)
		CdpReleaseDiskIoOutstanding(sourceExt);
	if (readContext->SourceReference)
		ObDereferenceObject(readContext->SourceReference);
	if (readContext->LowerReference)
		ObDereferenceObject(readContext->LowerReference);
	if (readContext->WorkItem)
		IoFreeWorkItem(readContext->WorkItem);
	cdpfree(readContext);
}

static NTSTATUS CdpDispatchProtectedDiskRead(
	_Inout_ PCdp_DEVICE_EXTENSION DiskExt,
	_Inout_ PIRP Irp)
{
	PIO_STACK_LOCATION irpSp = IoGetCurrentIrpStackLocation(Irp);
	UINT64 absoluteOffset;
	ULONG length;
	PDEVICE_OBJECT sourceReference = NULL;
	PCdp_DEVICE_EXTENSION sourceExt;
	PCdp_DISK_READ_OVERLAY_CONTEXT context;

	if (irpSp->Parameters.Read.ByteOffset.QuadPart < 0 ||
		irpSp->Parameters.Read.Length == 0)
	{
		return CdpSendToNextDevice(DiskExt->LowerDeviceObject, Irp);
	}
	absoluteOffset = (UINT64)irpSp->Parameters.Read.ByteOffset.QuadPart;
	length = irpSp->Parameters.Read.Length;
	sourceExt = CdpReferenceProtectedSourceForDiskIo(
		DiskExt, absoluteOffset, length, &sourceReference);
	if (!sourceExt)
		return CdpSendToNextDevice(DiskExt->LowerDeviceObject, Irp);

	if (!CdpAcquireDiskIoOutstanding(sourceExt))
	{
		ObDereferenceObject(sourceReference);
		return CdpSendToNextDevice(DiskExt->LowerDeviceObject, Irp);
	}

	context = (PCdp_DISK_READ_OVERLAY_CONTEXT)cdpalloc(sizeof(*context));
	if (!context)
	{
		CdpReleaseDiskIoOutstanding(sourceExt);
		ObDereferenceObject(sourceReference);
		return CdpCompleteIrp(Irp, STATUS_INSUFFICIENT_RESOURCES, 0);
	}
	RtlZeroMemory(context, sizeof(*context));
	context->WorkItem = IoAllocateWorkItem(DiskExt->FilterDeviceObject);
	if (!context->WorkItem)
	{
		cdpfree(context);
		CdpReleaseDiskIoOutstanding(sourceExt);
		ObDereferenceObject(sourceReference);
		return CdpCompleteIrp(Irp, STATUS_INSUFFICIENT_RESOURCES, 0);
	}
	context->Irp = Irp;
	context->SourceReference = sourceReference;
	context->LowerReference = DiskExt->LowerDeviceObject;
	ObReferenceObject(context->LowerReference);
	context->AbsoluteOffset = absoluteOffset;
	context->Length = length;

	IoMarkIrpPending(Irp);
	IoQueueWorkItem(
		context->WorkItem,
		CdpDiskReadOverlayWorker,
		DelayedWorkQueue,
		context);
	return STATUS_PENDING;
}

static NTSTATUS CdpQueueDiskCaptureIrp(
	_Inout_ PCdp_DEVICE_EXTENSION DiskExt,
	_Inout_ PIRP Irp)
{
	PIO_STACK_LOCATION irpSp = IoGetCurrentIrpStackLocation(Irp);
	UINT64 absoluteOffset = 0;
	ULONG length = 0;
	PDEVICE_OBJECT sourceReference = NULL;
	PCdp_DEVICE_EXTENSION sourceExt;
	PCdp_CAPTURE_ITEM item;
	KIRQL oldIrql;
	LONG64 auditSequence = 0;

	if (irpSp->MajorFunction == IRP_MJ_READ)
	{
		if (irpSp->Parameters.Read.ByteOffset.QuadPart < 0)
			return CdpSendToNextDevice(DiskExt->LowerDeviceObject, Irp);
		absoluteOffset = (UINT64)irpSp->Parameters.Read.ByteOffset.QuadPart;
		length = irpSp->Parameters.Read.Length;
		auditSequence = InterlockedIncrement64(
			&DiskExt->DiskReadPathEntryCount);
		if (auditSequence == 1 || (auditSequence & 0xfff) == 1)
		{
			Cdp_LOG("[DISK-READ-PATH] stage=entry seq=%lld disk=%lu absoluteOffset=%llu len=%lu mdl=%p lower=%p\n",
				auditSequence,
				DiskExt->DiskNumber,
				absoluteOffset,
				length,
				Irp->MdlAddress,
				DiskExt->LowerDeviceObject);
		}
	}
	else if (irpSp->MajorFunction == IRP_MJ_WRITE)
	{
		if (irpSp->Parameters.Write.ByteOffset.QuadPart < 0)
			return CdpSendToNextDevice(DiskExt->LowerDeviceObject, Irp);
		absoluteOffset = (UINT64)irpSp->Parameters.Write.ByteOffset.QuadPart;
		length = irpSp->Parameters.Write.Length;
	}
	else if (irpSp->MajorFunction != IRP_MJ_FLUSH_BUFFERS &&
		irpSp->MajorFunction != IRP_MJ_SHUTDOWN)
	{
		return CdpSendToNextDevice(DiskExt->LowerDeviceObject, Irp);
	}
	sourceExt = (irpSp->MajorFunction == IRP_MJ_FLUSH_BUFFERS ||
		irpSp->MajorFunction == IRP_MJ_SHUTDOWN) ?
		CdpReferenceProtectedSourceForDiskFlush(
			DiskExt, &sourceReference) :
		CdpReferenceProtectedSourceForDiskIo(
			DiskExt, absoluteOffset, length, &sourceReference);
	if (!sourceExt)
	{
		if (irpSp->MajorFunction == IRP_MJ_READ)
		{
			LONG64 missSequence = InterlockedIncrement64(
				&DiskExt->DiskReadPathNoSourceCount);
			if (missSequence == 1 || (missSequence & 0xfff) == 1)
			{
				Cdp_LOG("[DISK-READ-PATH] stage=no-source-match seq=%lld entrySeq=%lld disk=%lu absoluteOffset=%llu len=%lu\n",
					missSequence,
					auditSequence,
					DiskExt->DiskNumber,
					absoluteOffset,
					length);
			}
		}
		return CdpSendToNextDevice(DiskExt->LowerDeviceObject, Irp);
	}
	if (irpSp->MajorFunction == IRP_MJ_WRITE &&
		InterlockedCompareExchange(&DiskExt->ShutdownInProgress, 0, 0) != 0)
	{
		/* Shutdown has closed the source-write admission gate.  Never let a
		 * late protected write fall through to the original partition. */
		ObDereferenceObject(sourceReference);
		return CdpCompleteIrp(Irp, STATUS_DEVICE_NOT_READY, 0);
	}
	if (irpSp->MajorFunction == IRP_MJ_WRITE &&
		InterlockedCompareExchange(
			&sourceExt->BackfillWriteActive, 0, 0) != 0 &&
		absoluteOffset >= sourceExt->BackfillAbsoluteOffset &&
		length <= sourceExt->BackfillLength &&
		absoluteOffset - sourceExt->BackfillAbsoluteOffset <=
			sourceExt->BackfillLength - length)
	{
		PETHREAD owner = (PETHREAD)InterlockedCompareExchangePointer(
			(PVOID volatile*)&sourceExt->BackfillWriteThread, NULL, NULL);
		if (owner == PsGetCurrentThread())
		{
			/* This write originated from the volume-layer drain callback. The
			 * partition stack may have created a replacement IRP, so recognition
			 * uses only the externally published operation context. */
			ObDereferenceObject(sourceReference);
			return CdpSendToNextDevice(DiskExt->LowerDeviceObject, Irp);
		}
		Cdp_LOG("[DRAIN-VOLUME-BYPASS-MISS] disk=%lu offset=%llu len=%lu owner=%p current=%p; fail instead of queue deadlock\n",
			DiskExt->DiskNumber,
			absoluteOffset,
			length,
			owner,
			PsGetCurrentThread());
		ObDereferenceObject(sourceReference);
		return CdpCompleteIrp(Irp, STATUS_DEVICE_BUSY, 0);
	}
	if (!CdpAcquireDiskIoOutstanding(sourceExt))
	{
		ObDereferenceObject(sourceReference);
		return CdpSendToNextDevice(DiskExt->LowerDeviceObject, Irp);
	}
	if (irpSp->MajorFunction == IRP_MJ_READ)
	{
		LONG64 matchSequence = InterlockedIncrement64(
			&DiskExt->DiskReadPathSourceMatchCount);
		if (matchSequence == 1 || (matchSequence & 0xfff) == 1)
		{
			Cdp_LOG("[DISK-READ-PATH] stage=source-match seq=%lld entrySeq=%lld disk=%lu absoluteOffset=%llu len=%lu sourceStart=%llu sourceSize=%llu capture=%ld validated=%ld\n",
				matchSequence,
				auditSequence,
				DiskExt->DiskNumber,
				absoluteOffset,
				length,
				sourceExt->PartitionStart,
				sourceExt->PartitionSize,
				InterlockedCompareExchange(
					&sourceExt->CaptureEnabled, 0, 0),
				InterlockedCompareExchange(
					&sourceExt->ProtectionStateValidated, 0, 0));
		}
	}
	item = (PCdp_CAPTURE_ITEM)cdpalloc(sizeof(*item));
	if (!item)
	{
		CdpReleaseDiskIoOutstanding(sourceExt);
		ObDereferenceObject(sourceReference);
		return CdpCompleteIrp(Irp, STATUS_INSUFFICIENT_RESOURCES, 0);
	}
	item->Irp = Irp;
	item->OriginalDiskOffset = absoluteOffset;
	item->SourceReference = sourceReference;
	item->OriginLowerReference = DiskExt->LowerDeviceObject;
	ObReferenceObject(item->OriginLowerReference);
	/* Keep a disk READ IRP exactly as it arrived.  In particular, do not
	 * replace its absolute disk ByteOffset with a source-relative offset.
	 * The known-good CdpDiskFilter path treats the IRP as an opaque carrier
	 * and performs all address translation in worker-local variables. */
	/* READ and WRITE both retain their original absolute disk ByteOffset.
	 * Journal records and MetaTree keys use this same coordinate system. */

	KeAcquireSpinLock(&DiskExt->CaptureQueueLock, &oldIrql);
	if (InterlockedCompareExchange(&DiskExt->CaptureStopping, 0, 0) == 0 &&
		InterlockedCompareExchange(&DiskExt->ShutdownInProgress, 0, 0) == 0 &&
		InterlockedCompareExchange(&sourceExt->DiskIoAccepting, 0, 0) != 0 &&
		InterlockedCompareExchange(&sourceExt->CaptureEnabled, 0, 0) != 0)
	{
		IoMarkIrpPending(Irp);
		InsertTailList(&DiskExt->CaptureQueue, &item->Entry);
		InterlockedIncrement(&DiskExt->CaptureQueueDepth);
		KeSetEvent(&DiskExt->CaptureEvent, IO_NO_INCREMENT, FALSE);
		KeReleaseSpinLock(&DiskExt->CaptureQueueLock, oldIrql);
		return STATUS_PENDING;
	}
	KeReleaseSpinLock(&DiskExt->CaptureQueueLock, oldIrql);
	ObDereferenceObject(sourceReference);
	ObDereferenceObject(item->OriginLowerReference);
	cdpfree(item);
	CdpReleaseDiskIoOutstanding(sourceExt);
	if (irpSp->MajorFunction == IRP_MJ_WRITE &&
		InterlockedCompareExchange(&DiskExt->ShutdownInProgress, 0, 0) != 0)
	{
		return CdpCompleteIrp(Irp, STATUS_DEVICE_NOT_READY, 0);
	}
	return CdpSendToNextDevice(DiskExt->LowerDeviceObject, Irp);
}

NTSTATUS CdpIrpDispatchRead(_In_ PDEVICE_OBJECT DeviceObject, _Inout_ PIRP Irp)
{
	PCdp_DEVICE_EXTENSION deviceExt =
		(PCdp_DEVICE_EXTENSION)DeviceObject->DeviceExtension;

	if (!deviceExt || !deviceExt->LowerDeviceObject)
		return CdpCompleteIrp(Irp, STATUS_INVALID_DEVICE_REQUEST, 0);
	if (deviceExt->DeviceKind == Cdp_DEVICE_KIND_DISK)
	{
		/* Retain the private-read implementation for comparison while this test
		 * routes protected READ and WRITE through one ordered FIFO. */
		(void)&CdpDispatchProtectedDiskRead;
		return CdpQueueDiskCaptureIrp(deviceExt, Irp);
	}
	if (deviceExt->DeviceKind == Cdp_DEVICE_KIND_VOLUME)
		return CdpSendToNextDevice(deviceExt->LowerDeviceObject, Irp);
	return CdpCompleteIrp(Irp, STATUS_INVALID_DEVICE_REQUEST, 0);
}

static VOID CdpFailQueuedProtectedRead(
	_In_ PCdp_DEVICE_EXTENSION DevExt,
	_Inout_ PIRP Irp,
	_In_ PCSTR Reason)
{
	PIO_STACK_LOCATION irpSp = IoGetCurrentIrpStackLocation(Irp);

	CdpAuditProtectedReadBypass(DevExt, Irp, Reason);
	Cdp_LOG("[PROTECTED-READ-BLOCKED] reason=%s irp=%p offset=%lld len=%lu enabled=%ld stopping=%ld phase=%ld core=%p\n",
		Reason,
		Irp,
		irpSp->Parameters.Read.ByteOffset.QuadPart,
		irpSp->Parameters.Read.Length,
		InterlockedCompareExchange(&DevExt->CaptureEnabled, 0, 0),
		InterlockedCompareExchange(&DevExt->CaptureStopping, 0, 0),
		InterlockedCompareExchange(&DevExt->Phase, 0, 0),
		DevExt->Core);
	CdpCompleteIrp(Irp, STATUS_DEVICE_NOT_READY, 0);
}

static VOID CdpStopPreviewSessionForSource(
	_In_ PCdp_DRIVER_EXTENSION DriverExt,
	_Inout_ PCdp_DEVICE_EXTENSION SourceExt)
{
	PCdp_PREVIEW_SESSION session = NULL;
	PLIST_ENTRY entry;

	if (!DriverExt || !SourceExt)
		return;
	ExAcquireFastMutex(&DriverExt->PreviewSessionMutex);
	for (entry = DriverExt->PreviewSessionList.Flink;
		entry != &DriverExt->PreviewSessionList;
		entry = entry->Flink)
	{
		PCdp_PREVIEW_SESSION candidate = CONTAINING_RECORD(
			entry, Cdp_PREVIEW_SESSION, Entry);
		if (RtlCompareMemory(
			&candidate->SourceVolumeGuid,
			&SourceExt->VolumeGuid,
			sizeof(GUID)) == sizeof(GUID))
		{
			session = candidate;
			session->Closing = TRUE;
			RemoveEntryList(&session->Entry);
			break;
		}
	}
	ExReleaseFastMutex(&DriverExt->PreviewSessionMutex);

	InterlockedExchange(&SourceExt->Phase, (LONG)Cdp_PHASE_GENERAL);
	if (session)
	{
		Cdp_LOG("[MERGE] preview stopped at reclaimed target record handle=%llu\n",
			session->HandleId);
		CdpDestroyPreviewSession(DriverExt, session);
	}
}

static VOID CdpMergeWorker(_In_ PVOID Context)
{
	PCdp_DEVICE_EXTENSION devExt = (PCdp_DEVICE_EXTENSION)Context;
	PCdp_DRIVER_EXTENSION driverExt =
		IoGetDriverObjectExtension(g_DriverObject, &g_DriverObject);
	NTSTATUS status = STATUS_SUCCESS;
	BOOLEAN coreMergeActive = FALSE;

	status = CdpCoreSetMergeActive(devExt->Core, TRUE);
	if (!NT_SUCCESS(status))
		goto done;
	coreMergeActive = TRUE;

	for (;;)
	{
		BOOLEAN atLeast = FALSE;
		if (InterlockedCompareExchange(
				&devExt->MergeThreadStopping, 0, 0) != 0 ||
			InterlockedCompareExchange(&devExt->CaptureEnabled, 0, 0) == 0 ||
			!devExt->Core)
		{
			break;
		}
		status = CdpCoreJournalUsageAtLeast(devExt->Core, 90, &atLeast);
		if (!NT_SUCCESS(status) || !atLeast)
			break;
		KeWaitForSingleObject(
			&devExt->HistoryMutex,
			Executive,
			KernelMode,
			FALSE,
			NULL);
		status = CdpCoreCompactOldestRegion(devExt->Core);
		KeReleaseMutex(&devExt->HistoryMutex, FALSE);
		if (CdpCoreConsumePreviewStoppedByMerge(devExt->Core))
			CdpStopPreviewSessionForSource(driverExt, devExt);
		if (status == STATUS_NOT_FOUND)
			break;
		if (!NT_SUCCESS(status))
		{
			Cdp_LOG("[MERGE] compact failed status=0x%08X\n", status);
			break;
		}
		Cdp_LOG("[MERGE] oldest header region materialized and deleted\n");
	}

done:
	if (coreMergeActive)
		(void)CdpCoreSetMergeActive(devExt->Core, FALSE);
	InterlockedExchange(&devExt->MergeThreadRunning, 0);
	KeSetEvent(&devExt->MergeThreadDoneEvent, IO_NO_INCREMENT, FALSE);
	Cdp_DBG("[MERGE] thread exit status=0x%08X\n", status);
	PsTerminateSystemThread(status == STATUS_NOT_FOUND ? STATUS_SUCCESS : status);
}

static VOID CdpCloseFinishedMergeHandle(_Inout_ PCdp_DEVICE_EXTENSION DevExt)
{
	HANDLE handle = DevExt->MergeThreadHandle;
	PVOID threadObject = NULL;

	if (!handle)
		return;
	if (NT_SUCCESS(ObReferenceObjectByHandle(
		handle, THREAD_ALL_ACCESS, *PsThreadType,
		KernelMode, &threadObject, NULL)))
	{
		KeWaitForSingleObject(
			threadObject, Executive, KernelMode, FALSE, NULL);
		ObDereferenceObject(threadObject);
	}
	ZwClose(handle);
	DevExt->MergeThreadHandle = NULL;
}

static NTSTATUS CdpStartMergeThread(_Inout_ PCdp_DEVICE_EXTENSION DevExt)
{
	NTSTATUS status;

	if (!DevExt || !DevExt->Core)
		return STATUS_DEVICE_NOT_READY;
	if (InterlockedCompareExchange(
			&DevExt->MergeThreadRunning, 1, 0) != 0)
	{
		return STATUS_DEVICE_BUSY;
	}
	CdpCloseFinishedMergeHandle(DevExt);
	InterlockedExchange(&DevExt->MergeThreadStopping, 0);
	KeClearEvent(&DevExt->MergeThreadDoneEvent);
	status = PsCreateSystemThread(
		&DevExt->MergeThreadHandle,
		THREAD_ALL_ACCESS,
		NULL,
		NULL,
		NULL,
		CdpMergeWorker,
		DevExt);
	if (!NT_SUCCESS(status))
	{
		InterlockedExchange(&DevExt->MergeThreadRunning, 0);
		KeSetEvent(&DevExt->MergeThreadDoneEvent, IO_NO_INCREMENT, FALSE);
	}
	return status;
}

static VOID CdpStopMergeThread(_Inout_ PCdp_DEVICE_EXTENSION DevExt)
{
	if (!DevExt)
		return;
	InterlockedExchange(&DevExt->MergeThreadStopping, 1);
	if (InterlockedCompareExchange(
			&DevExt->MergeThreadRunning, 0, 0) != 0)
	{
		KeWaitForSingleObject(
			&DevExt->MergeThreadDoneEvent,
			Executive,
			KernelMode,
			FALSE,
			NULL);
	}
	CdpCloseFinishedMergeHandle(DevExt);
	InterlockedExchange(&DevExt->MergeThreadStopping, 0);
}

static VOID CdpStartMergeIfNeeded(_Inout_ PCdp_DEVICE_EXTENSION DevExt)
{
	BOOLEAN atLeast = FALSE;
	NTSTATUS status;

	if (!DevExt || !DevExt->Core ||
		InterlockedCompareExchange(&DevExt->Phase, 0, 0) !=
			(LONG)Cdp_PHASE_GENERAL ||
		InterlockedCompareExchange(&DevExt->CaptureEnabled, 0, 0) == 0)
	{
		return;
	}
	status = CdpCoreJournalUsageAtLeast(DevExt->Core, 90, &atLeast);
	if (!NT_SUCCESS(status))
	{
		Cdp_LOG("[MERGE] usage query failed status=0x%08X\n", status);
		return;
	}
	if (!atLeast)
		return;
	status = CdpStartMergeThread(DevExt);
	if (!NT_SUCCESS(status) && status != STATUS_DEVICE_BUSY)
		Cdp_LOG("[MERGE] start failed status=0x%08X\n", status);
}

/*
 * The only protected-write path.  Reserve its payload slot and publish its
 * in-memory mapping before handing the original IRP to the Journal volume.
 * The source stack is never called for this write.
 */
static VOID CdpReleaseRedirectWrite(
	_Inout_ PCdp_DEVICE_EXTENSION SourceExt)
{
	LONG remaining = InterlockedDecrement(&SourceExt->RedirectWritesInFlight);

	NT_ASSERT(remaining >= 0);
	if (remaining == 0)
		KeSetEvent(&SourceExt->RedirectWritesDrainedEvent,
			IO_NO_INCREMENT, FALSE);
}

static BOOLEAN CdpTryAcquireRedirectWrite(
	_Inout_ PCdp_DEVICE_EXTENSION SourceExt)
{
	LONG count;

	count = InterlockedIncrement(&SourceExt->RedirectWritesInFlight);
	if (count == 1)
		KeClearEvent(&SourceExt->RedirectWritesDrainedEvent);
	KeMemoryBarrier();
	if (InterlockedCompareExchange(&SourceExt->CaptureEnabled, 0, 0) != 0 &&
		InterlockedCompareExchange(&SourceExt->CaptureStopping, 0, 0) == 0 &&
		InterlockedCompareExchange(&SourceExt->Phase, 0, 0) !=
			(LONG)Cdp_PHASE_DRAINING)
	{
		return TRUE;
	}
	CdpReleaseRedirectWrite(SourceExt);
	return FALSE;
}

static NTSTATUS CdpCompleteFailedRedirectWrite(
	_Inout_ PCdp_DEVICE_EXTENSION SourceExt,
	_Inout_ PIRP Irp,
	_In_ NTSTATUS Status)
{
	if (SourceExt)
		CdpReleaseRedirectWrite(SourceExt);
	return CdpCompleteIrp(Irp, Status, 0);
}

static NTSTATUS CdpSnapshotWriteMdlChain(
	_In_ PIRP Irp,
	_In_ ULONG RequiredLength,
	_Outptr_result_bytebuffer_(RequiredLength) PUCHAR* Snapshot,
	_Out_ PULONG MdlCount,
	_Out_ PUINT64 MdlBytes)
{
	PMDL mdl;
	PUCHAR snapshot;
	ULONG copied = 0;
	ULONG count = 0;
	UINT64 total = 0;

	if (!Irp || !Snapshot || !MdlCount || !MdlBytes || RequiredLength == 0)
		return STATUS_INVALID_PARAMETER;
	*Snapshot = NULL;
	*MdlCount = 0;
	*MdlBytes = 0;
	if (Irp->MdlAddress == NULL)
	{
		snapshot = (PUCHAR)cdpalloc(RequiredLength);
		if (!snapshot)
			return STATUS_INSUFFICIENT_RESOURCES;
		if ((Irp->Flags & IRP_BUFFERED_IO) != 0 &&
			Irp->AssociatedIrp.SystemBuffer != NULL)
		{
			RtlCopyMemory(snapshot,
				Irp->AssociatedIrp.SystemBuffer, RequiredLength);
			*Snapshot = snapshot;
			return STATUS_SUCCESS;
		}
		if (Irp->RequestorMode == KernelMode && Irp->UserBuffer != NULL)
		{
			__try
			{
				RtlCopyMemory(snapshot, Irp->UserBuffer, RequiredLength);
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				NTSTATUS exceptionStatus = GetExceptionCode();
				cdpfree(snapshot);
				return exceptionStatus;
			}
			*Snapshot = snapshot;
			return STATUS_SUCCESS;
		}
		cdpfree(snapshot);
		return STATUS_NOT_SUPPORTED;
	}
	for (mdl = Irp->MdlAddress; mdl; mdl = mdl->Next)
	{
		ULONG bytes = MmGetMdlByteCount(mdl);
		count++;
		if (total > MAXUINT64 - bytes)
			return STATUS_INTEGER_OVERFLOW;
		total += bytes;
	}
	*MdlCount = count;
	*MdlBytes = total;
	if (count == 0 || total < RequiredLength)
		return STATUS_BUFFER_TOO_SMALL;

	snapshot = (PUCHAR)cdpalloc(RequiredLength);
	if (!snapshot)
		return STATUS_INSUFFICIENT_RESOURCES;
	for (mdl = Irp->MdlAddress; mdl && copied < RequiredLength; mdl = mdl->Next)
	{
		ULONG bytes = MmGetMdlByteCount(mdl);
		ULONG take = bytes;
		PVOID mapped;

		if (take > RequiredLength - copied)
			take = RequiredLength - copied;
		if (take == 0)
			continue;
		mapped = MmGetSystemAddressForMdlSafe(mdl, NormalPagePriority);
		if (!mapped)
		{
			cdpfree(snapshot);
			return STATUS_INSUFFICIENT_RESOURCES;
		}
		RtlCopyMemory(snapshot + copied, mapped, take);
		copied += take;
	}
	if (copied != RequiredLength)
	{
		cdpfree(snapshot);
		return STATUS_BUFFER_TOO_SMALL;
	}
	*Snapshot = snapshot;
	return STATUS_SUCCESS;
}

static NTSTATUS CdpQueryMdlChain(
	_In_opt_ PMDL FirstMdl,
	_Out_ PULONG MdlCount,
	_Out_ PUINT64 MdlBytes)
{
	PMDL mdl;
	ULONG count = 0;
	UINT64 total = 0;

	if (!MdlCount || !MdlBytes)
		return STATUS_INVALID_PARAMETER;
	*MdlCount = 0;
	*MdlBytes = 0;
	for (mdl = FirstMdl; mdl; mdl = mdl->Next)
	{
		ULONG bytes = MmGetMdlByteCount(mdl);
		count++;
		if (total > MAXUINT64 - bytes)
			return STATUS_INTEGER_OVERFLOW;
		total += bytes;
	}
	*MdlCount = count;
	*MdlBytes = total;
	return STATUS_SUCCESS;
}

static NTSTATUS CdpScatterReadMdlChain(
	_In_ PIRP Irp,
	_In_reads_bytes_(Length) const UCHAR* Source,
	_In_ ULONG Length,
	_Out_ PULONG MdlCount,
	_Out_ PUINT64 MdlBytes,
	_Out_ PULONG CopiedBytes)
{
	PMDL mdl;
	ULONG copied = 0;
	NTSTATUS status;

	if (!Irp || !Source || Length == 0 || !MdlCount || !MdlBytes ||
		!CopiedBytes)
	{
		return STATUS_INVALID_PARAMETER;
	}
	*CopiedBytes = 0;
	*MdlCount = 0;
	*MdlBytes = 0;
	/* Disk class/partition drivers can issue internal noncached reads without
	 * an MDL even though our filter device advertises DO_DIRECT_IO. Support the
	 * two buffer forms used by those kernel-originated IRPs. */
	if (Irp->MdlAddress == NULL)
	{
		if ((Irp->Flags & IRP_BUFFERED_IO) != 0 &&
			Irp->AssociatedIrp.SystemBuffer != NULL)
		{
			RtlCopyMemory(Irp->AssociatedIrp.SystemBuffer, Source, Length);
			*CopiedBytes = Length;
			if (InterlockedCompareExchange(
					&g_CdpMdllessSystemBufferReported, 1, 0) == 0)
			{
				Cdp_LOG("[CORE-READ-BUFFER] mdlless SystemBuffer supported irp=%p len=%lu flags=0x%08lX\n",
					Irp, Length, Irp->Flags);
			}
			return STATUS_SUCCESS;
		}
		if (Irp->RequestorMode == KernelMode && Irp->UserBuffer != NULL)
		{
			__try
			{
				RtlCopyMemory(Irp->UserBuffer, Source, Length);
				*CopiedBytes = Length;
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				return GetExceptionCode();
			}
			if (InterlockedCompareExchange(
					&g_CdpMdllessUserBufferReported, 1, 0) == 0)
			{
				Cdp_LOG("[CORE-READ-BUFFER] mdlless kernel UserBuffer supported irp=%p len=%lu flags=0x%08lX buffer=%p\n",
					Irp, Length, Irp->Flags, Irp->UserBuffer);
			}
			return STATUS_SUCCESS;
		}
		return STATUS_NOT_SUPPORTED;
	}
	status = CdpQueryMdlChain(Irp->MdlAddress, MdlCount, MdlBytes);
	if (!NT_SUCCESS(status))
		return status;
	if (*MdlCount == 0 || *MdlBytes < Length)
		return STATUS_BUFFER_TOO_SMALL;

	for (mdl = Irp->MdlAddress; mdl && copied < Length; mdl = mdl->Next)
	{
		ULONG bytes = MmGetMdlByteCount(mdl);
		ULONG take = bytes;
		PVOID mapped;

		if (take > Length - copied)
			take = Length - copied;
		if (take == 0)
			continue;
		mapped = MmGetSystemAddressForMdlSafe(mdl, NormalPagePriority);
		if (!mapped)
		{
			*CopiedBytes = copied;
			return STATUS_INSUFFICIENT_RESOURCES;
		}
		RtlCopyMemory(mapped, Source + copied, take);
		copied += take;
	}
	*CopiedBytes = copied;
	return copied == Length ? STATUS_SUCCESS : STATUS_BUFFER_TOO_SMALL;
}

static NTSTATUS CdpRedirectJournalWrite(
	_In_ PCdp_DEVICE_EXTENSION SourceExt,
	_Inout_ PCdp_CAPTURE_ITEM Item)
{
	PIRP Irp;
	PIO_STACK_LOCATION irpSp;
	PUCHAR snapshot = NULL;
	Cdp_JOURNAL_RECORD record;
	ULONG writeLength;
	ULONG chunkOffset;
	ULONG mdlCount = 0;
	UINT64 mdlBytes = 0;
	UINT64 writeOffset;
	NTSTATUS status;

	/* The worker only calls this routine for a fully formed queued IRP.  Keep
	 * the validation split so an unexpected bad queue item is never passed to
	 * CdpCompleteIrp as a NULL IRP. */
	if (!SourceExt || !Item || !Item->Irp)
		return STATUS_INVALID_PARAMETER;

	Irp = Item->Irp;
	if (!SourceExt->Core || !Item->OriginLowerReference)
		return CdpCompleteFailedRedirectWrite(
			SourceExt, Irp, STATUS_DEVICE_NOT_READY);

	irpSp = IoGetCurrentIrpStackLocation(Irp);
	writeLength = irpSp->Parameters.Write.Length;
	if (irpSp->Parameters.Write.ByteOffset.QuadPart < 0 || writeLength == 0)
		return CdpCompleteFailedRedirectWrite(
			SourceExt, Irp, STATUS_INVALID_PARAMETER);
	writeOffset = (UINT64)irpSp->Parameters.Write.ByteOffset.QuadPart;
	if (writeOffset > MAXUINT64 - writeLength)
		return CdpCompleteFailedRedirectWrite(
			SourceExt, Irp, STATUS_INVALID_PARAMETER);

	if (!SourceExt->RedirectJournalEntry)
		return CdpCompleteFailedRedirectWrite(
			SourceExt, Irp, STATUS_DEVICE_NOT_READY);
	status = CdpSnapshotWriteMdlChain(
		Irp, writeLength, &snapshot, &mdlCount, &mdlBytes);
	if (!NT_SUCCESS(status))
	{
		Cdp_LOG("[VERIFY-FAIL] stage=mdl status=0x%08X sourceOffset=%lld len=%lu mdlCount=%lu mdlBytes=%llu\n",
			status,
			irpSp->Parameters.Write.ByteOffset.QuadPart,
			writeLength,
			mdlCount,
			mdlBytes);
		return CdpCompleteFailedRedirectWrite(SourceExt, Irp, status);
	}
	/* A source IRP may be much larger than one on-disk record. Commit every
	 * real application-data chunk to the journal and never write the source. */
	for (chunkOffset = 0; chunkOffset < writeLength; )
	{
		ULONG chunkLength = writeLength - chunkOffset;
		PUCHAR chunkData = snapshot + chunkOffset;
		UINT64 chunkVolumeOffset =
			(UINT64)irpSp->Parameters.Write.ByteOffset.QuadPart + chunkOffset;

		if (chunkLength > Cdp_JOURNAL_MAX_RECORD_DATA)
			chunkLength = Cdp_JOURNAL_MAX_RECORD_DATA;
		status = CdpCoreAppendAfterImage(
			SourceExt->Core,
			chunkVolumeOffset,
			chunkLength,
			chunkData,
			&record);
		if (!NT_SUCCESS(status))
		{
			Cdp_LOG("[REDIRECT-WRITE-FAIL] stage=append-publish status=0x%08X offset=%lld len=%lu chunkOffset=%lu chunkLen=%lu chunksCommitted=%lu\n",
				status,
				irpSp->Parameters.Write.ByteOffset.QuadPart,
				writeLength,
				chunkOffset,
				chunkLength,
				chunkOffset / Cdp_JOURNAL_MAX_RECORD_DATA);
			cdpfree(snapshot);
			return CdpCompleteFailedRedirectWrite(SourceExt, Irp, status);
		}
		chunkOffset += chunkLength;
	}

	cdpfree(snapshot);

	CdpStartMergeIfNeeded(SourceExt);
	CdpReleaseRedirectWrite(SourceExt);
	return CdpCompleteIrp(Irp, STATUS_SUCCESS, writeLength);
}

static NTSTATUS CdpForwardWriteCompletion(
	_In_ PDEVICE_OBJECT DeviceObject,
	_In_ PIRP Irp,
	_In_ PVOID Context)
{
	UNREFERENCED_PARAMETER(DeviceObject);
	UNREFERENCED_PARAMETER(Irp);
	KeSetEvent((PKEVENT)Context, IO_NO_INCREMENT, FALSE);
	return STATUS_MORE_PROCESSING_REQUIRED;
}

static NTSTATUS CdpForwardQueuedDiskIrpSynchronously(
	_Inout_ PCdp_CAPTURE_ITEM Item)
{
	KEVENT event;
	PIO_STACK_LOCATION nextSp;

	if (!Item || !Item->Irp ||
		!Item->OriginLowerReference)
	{
		return STATUS_INVALID_PARAMETER;
	}
	KeInitializeEvent(&event, NotificationEvent, FALSE);
	IoCopyCurrentIrpStackLocationToNext(Item->Irp);
	nextSp = IoGetNextIrpStackLocation(Item->Irp);
	if (nextSp->MajorFunction == IRP_MJ_READ)
	{
		nextSp->Parameters.Read.ByteOffset.QuadPart =
			(LONGLONG)Item->OriginalDiskOffset;
	}
	else if (nextSp->MajorFunction == IRP_MJ_WRITE)
	{
		nextSp->Parameters.Write.ByteOffset.QuadPart =
			(LONGLONG)Item->OriginalDiskOffset;
	}
	IoSetCompletionRoutine(
		Item->Irp,
		CdpForwardWriteCompletion,
		&event,
		TRUE,
		TRUE,
		TRUE);
	(void)IoCallDriver(Item->OriginLowerReference, Item->Irp);
	KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, NULL);
	return Item->Irp->IoStatus.Status;
}

static NTSTATUS CdpFlushProtectedJournalsForDisk(
	_In_ PCdp_DEVICE_EXTENSION DiskExt)
{
	PCdp_DRIVER_EXTENSION driverExt;
	PDEVICE_OBJECT sources[32];
	ULONG count = 0;
	ULONG index;
	KIRQL oldIrql;
	PLIST_ENTRY entry;
	NTSTATUS firstFailure = STATUS_SUCCESS;

	if (!DiskExt)
		return STATUS_INVALID_PARAMETER;
	driverExt = IoGetDriverObjectExtension(g_DriverObject, &g_DriverObject);
	if (!driverExt)
		return STATUS_DEVICE_NOT_READY;

	KeAcquireSpinLock(&driverExt->DeviceObjectListLock, &oldIrql);
	for (entry = driverExt->DeviceObjectListHead.Flink;
		entry != &driverExt->DeviceObjectListHead && count < RTL_NUMBER_OF(sources);
		entry = entry->Flink)
	{
		PCdp_DEVICE_LIST_NODE node =
			CONTAINING_RECORD(entry, Cdp_DEVICE_LIST_NODE, Entry);
		PCdp_DEVICE_EXTENSION ext =
			(PCdp_DEVICE_EXTENSION)node->DeviceObject->DeviceExtension;
		if (ext && (ext->DeviceKind == Cdp_DEVICE_KIND_VOLUME ||
			ext->DeviceKind == Cdp_DEVICE_KIND_DISK) &&
			ext->DiskNumber == DiskExt->DiskNumber &&
			InterlockedCompareExchange(&ext->CaptureEnabled, 0, 0) != 0 &&
			ext->RedirectJournalEntry)
		{
			ObReferenceObject(node->DeviceObject);
			sources[count++] = node->DeviceObject;
		}
	}
	KeReleaseSpinLock(&driverExt->DeviceObjectListLock, oldIrql);

	for (index = 0; index < count; ++index)
	{
		PCdp_DEVICE_EXTENSION sourceExt =
			(PCdp_DEVICE_EXTENSION)sources[index]->DeviceExtension;
		NTSTATUS status;

		KeWaitForSingleObject(&sourceExt->HistoryMutex,
			Executive, KernelMode, FALSE, NULL);
		status = sourceExt->RedirectJournalEntry ?
			CdpJournalFlushBuffers(
				&sourceExt->RedirectJournalEntry->Journal) :
			STATUS_DEVICE_NOT_READY;
		KeReleaseMutex(&sourceExt->HistoryMutex, FALSE);
		if (!NT_SUCCESS(status) && NT_SUCCESS(firstFailure))
			firstFailure = status;
		ObDereferenceObject(sources[index]);
	}
	return firstFailure;
}

static VOID CdpCaptureWorker(_In_ PVOID Context)
{
	PCdp_DEVICE_EXTENSION queueExt = (PCdp_DEVICE_EXTENSION)Context;

	for (;;)
	{
		PLIST_ENTRY entry = NULL;
		KIRQL oldIrql;

		KeWaitForSingleObject(
			&queueExt->CaptureEvent,
			Executive,
			KernelMode,
			FALSE,
			NULL);
		for (;;)
		{
			KeAcquireSpinLock(&queueExt->CaptureQueueLock, &oldIrql);
			if (!IsListEmpty(&queueExt->CaptureQueue))
			{
				entry = RemoveHeadList(&queueExt->CaptureQueue);
				InterlockedDecrement(&queueExt->CaptureQueueDepth);
			}
			else
			{
				KeClearEvent(&queueExt->CaptureEvent);
				entry = NULL;
			}
			KeReleaseSpinLock(&queueExt->CaptureQueueLock, oldIrql);
			if (!entry)
				break;

			{
				PCdp_CAPTURE_ITEM item =
					CONTAINING_RECORD(entry, Cdp_CAPTURE_ITEM, Entry);
				PCdp_DEVICE_EXTENSION devExt = item->SourceReference ?
					(PCdp_DEVICE_EXTENSION)
						item->SourceReference->DeviceExtension : NULL;
				PIO_STACK_LOCATION irpSp = IoGetCurrentIrpStackLocation(item->Irp);
				UCHAR majorFunction = irpSp->MajorFunction;
				ULONG ioLength = majorFunction == IRP_MJ_WRITE ?
					irpSp->Parameters.Write.Length :
					(majorFunction == IRP_MJ_READ ?
						irpSp->Parameters.Read.Length : 0);
				BOOLEAN captureActive;
				LONG phase;

				if (majorFunction == IRP_MJ_SHUTDOWN)
				{
					NTSTATUS flushStatus =
						CdpFlushProtectedJournalsForDisk(queueExt);
					NTSTATUS lowerStatus;

					if (!NT_SUCCESS(flushStatus))
					{
						Cdp_LOG("[SHUTDOWN-BARRIER] journal flush failed disk=%lu status=0x%08X\n",
							queueExt->DiskNumber, flushStatus);
					}
					lowerStatus = CdpForwardQueuedDiskIrpSynchronously(item);
					Cdp_LOG("[SHUTDOWN-BARRIER] forwarded disk=%lu flushStatus=0x%08X lowerStatus=0x%08X\n",
						queueExt->DiskNumber, flushStatus, lowerStatus);
					IoCompleteRequest(item->Irp, IO_NO_INCREMENT);
					goto capture_item_done;
				}

				if (!devExt)
				{
					(void)CdpForwardQueuedDiskIrpSynchronously(item);
					IoCompleteRequest(item->Irp, IO_NO_INCREMENT);
					goto capture_item_release;
				}

				captureActive =
					!devExt->CaptureStopping &&
					InterlockedCompareExchange(&devExt->CaptureEnabled, 0, 0) != 0;
				phase = InterlockedCompareExchange(&devExt->Phase, 0, 0);
				if (majorFunction == IRP_MJ_WRITE &&
					captureActive && phase != (LONG)Cdp_PHASE_DRAINING &&
					CdpTryAcquireRedirectWrite(devExt))
				{
					/* Serialize exactly like the before-image implementation: the
					 * next FIFO item cannot start until this payload, header and
					 * flush have completed and the MetaTree has been published. */
					KeWaitForSingleObject(
						&devExt->HistoryMutex,
						Executive,
						KernelMode,
						FALSE,
						NULL);
					(void)CdpRedirectJournalWrite(devExt, item);
					KeReleaseMutex(&devExt->HistoryMutex, FALSE);
				}
				else if (majorFunction == IRP_MJ_READ && captureActive)
				{
					/* Disk IRPs, Core, MetaTree and record headers all use the
					 * same absolute physical-disk source address.  The legacy
					 * volume queue is translated once at this boundary. */
					UINT64 readOffset = item->OriginalDiskOffset;
					PUCHAR buffer;
					ULONG mdlCount = 0;
					UINT64 mdlBytes = 0;
					ULONG copiedBytes = 0;
					NTSTATUS readStatus;
					if (readOffset < devExt->PartitionStart ||
						devExt->PartitionStart >
							MAXUINT64 - devExt->PartitionSize ||
						readOffset >=
							devExt->PartitionStart + devExt->PartitionSize ||
						ioLength > devExt->PartitionStart +
							devExt->PartitionSize - readOffset)
					{
						Cdp_LOG("[CORE-READ-BYPASS] reason=outside-source-range offset=%llu len=%lu partitionSize=%llu originalDiskOffset=%llu\n",
							readOffset,
							ioLength,
							devExt->PartitionSize,
							item->OriginalDiskOffset);
						(void)CdpForwardQueuedDiskIrpSynchronously(item);
						IoCompleteRequest(item->Irp, IO_NO_INCREMENT);
						goto capture_item_done;
					}
					buffer = (PUCHAR)cdpalloc(ioLength);
					readStatus = buffer ? CdpReadDiskLowerSynchronously(
						item->OriginLowerReference,
						readOffset,
						ioLength,
						buffer) : STATUS_INSUFFICIENT_RESOURCES;
					if (NT_SUCCESS(readStatus))
					{
						KeWaitForSingleObject(&devExt->HistoryMutex,
							Executive, KernelMode, FALSE, NULL);
						readStatus = devExt->Core ?
							CdpCoreOverlayCurrentRead(
								devExt->Core,
								readOffset,
								ioLength,
								buffer) : STATUS_DEVICE_NOT_READY;
						CdpAuditProtectedReadCoreResult(
							devExt, ioLength, readStatus);
						KeReleaseMutex(&devExt->HistoryMutex, FALSE);
					}
					if (NT_SUCCESS(readStatus))
					{
						readStatus = CdpScatterReadMdlChain(
							item->Irp, buffer, ioLength, &mdlCount,
							&mdlBytes, &copiedBytes);
					}
					if (!NT_SUCCESS(readStatus))
					{
						Cdp_LOG("[CORE-READ-FAIL] worker=ordered status=0x%08X sourceOffset=%lld len=%lu partitionSize=%llu originalDiskOffset=%llu mdlCount=%lu mdlBytes=%llu copied=%lu\n",
							readStatus,
							(LONGLONG)readOffset,
							ioLength,
							devExt->PartitionSize,
							item->OriginalDiskOffset,
							mdlCount, mdlBytes, copiedBytes);
					}
					if (buffer)
						cdpfree(buffer);
					CdpCompleteIrp(item->Irp, readStatus,
						NT_SUCCESS(readStatus) ? ioLength : 0);
				}
				else if (majorFunction == IRP_MJ_FLUSH_BUFFERS && captureActive &&
					phase != (LONG)Cdp_PHASE_DRAINING)
				{
					NTSTATUS flushStatus;
					KeWaitForSingleObject(&devExt->HistoryMutex,
						Executive, KernelMode, FALSE, NULL);
					flushStatus = devExt->RedirectJournalEntry ?
						CdpJournalFlushBuffers(
							&devExt->RedirectJournalEntry->Journal) :
						STATUS_DEVICE_NOT_READY;
					KeReleaseMutex(&devExt->HistoryMutex, FALSE);
					if (!NT_SUCCESS(flushStatus))
						Cdp_LOG("[ORDERED-IO] journal flush failed status=0x%08X\n",
							flushStatus);
					CdpCompleteIrp(item->Irp, flushStatus, 0);
				}
				else if (majorFunction == IRP_MJ_WRITE && InterlockedCompareExchange(
						&devExt->CaptureEnabled, 0, 0) != 0 &&
					InterlockedCompareExchange(&devExt->Phase, 0, 0) ==
						(LONG)Cdp_PHASE_DRAINING)
				{
					NTSTATUS writeStatus;
					NTSTATUS punchStatus = STATUS_SUCCESS;

					KeWaitForSingleObject(
						&devExt->HistoryMutex,
						Executive,
						KernelMode,
						FALSE,
						NULL);
					writeStatus =
						CdpForwardQueuedDiskIrpSynchronously(item);
					if (NT_SUCCESS(writeStatus) && devExt->Core)
					{
						punchStatus = CdpCorePunchMetaRange(
							devExt->Core,
							(UINT64)irpSp->Parameters.Write.ByteOffset.QuadPart,
							ioLength);
						if (!NT_SUCCESS(punchStatus))
						{
							InterlockedCompareExchange(
								&devExt->DrainFailureStatus,
								(LONG)punchStatus,
								0);
							Cdp_LOG("[DRAIN] application write punch failed status=0x%08X offset=%lld len=%lu\n",
								punchStatus,
								irpSp->Parameters.Write.ByteOffset.QuadPart,
								ioLength);
						}
					}
					KeReleaseMutex(&devExt->HistoryMutex, FALSE);
					/* The source write result is the application-visible result. The
					 * drain failure is reported by CMD2 without falsely retrying an
					 * already committed application write. */
					IoCompleteRequest(item->Irp, IO_NO_INCREMENT);
				}
				else
				{
					Cdp_DBG("[ORDERED-IO] worker bypass major=0x%02X irp=%p\n",
						majorFunction, item->Irp);
					if (majorFunction == IRP_MJ_READ)
						CdpFailQueuedProtectedRead(
							devExt, item->Irp, "ordered-worker-state-change");
					else if (majorFunction == IRP_MJ_WRITE &&
						InterlockedCompareExchange(
							&devExt->CaptureEnabled, 0, 0) != 0 &&
						phase != (LONG)Cdp_PHASE_DRAINING)
					{
						Cdp_LOG("[REDIRECT-WRITE-BYPASS-BLOCKED] stage=worker offset=%lld len=%lu captureActive=%lu phase=%ld stopping=%ld\n",
							irpSp->Parameters.Write.ByteOffset.QuadPart,
							ioLength,
							captureActive ? 1UL : 0UL,
							phase,
							InterlockedCompareExchange(
								&devExt->CaptureStopping, 0, 0));
						CdpCompleteIrp(item->Irp, STATUS_DEVICE_NOT_READY, 0);
					}
					else
					{
						(void)CdpForwardQueuedDiskIrpSynchronously(item);
						IoCompleteRequest(item->Irp, IO_NO_INCREMENT);
					}
				}
			capture_item_done:
				CdpReleaseDiskIoOutstanding(devExt);
			capture_item_release:
				if (item->SourceReference)
					ObDereferenceObject(item->SourceReference);
				if (item->OriginLowerReference)
					ObDereferenceObject(item->OriginLowerReference);
				cdpfree(item);
			}
		}
		if (InterlockedCompareExchange(&queueExt->CaptureStopping, 0, 0) != 0)
			break;
	}
	PsTerminateSystemThread(STATUS_SUCCESS);
}

NTSTATUS CdpStartCaptureWorker(_Inout_ PCdp_DEVICE_EXTENSION DevExt)
{
	InterlockedExchange(&DevExt->CaptureStopping, 0);
	return PsCreateSystemThread(
		&DevExt->CaptureThreadHandle,
		THREAD_ALL_ACCESS,
		NULL,
		NULL,
		NULL,
		CdpCaptureWorker,
		DevExt);
}

VOID CdpStopCaptureWorker(_Inout_ PCdp_DEVICE_EXTENSION DevExt)
{
	HANDLE threadHandle = DevExt->CaptureThreadHandle;
	PVOID threadObject = NULL;

	if (!threadHandle)
	{
		InterlockedExchange(&DevExt->CaptureStopping, 0);
		return;
	}
	InterlockedExchange(&DevExt->CaptureStopping, 1);
	KeSetEvent(&DevExt->CaptureEvent, IO_NO_INCREMENT, FALSE);

	if (NT_SUCCESS(ObReferenceObjectByHandle(threadHandle, THREAD_ALL_ACCESS,
		*PsThreadType, KernelMode, &threadObject, NULL)))
	{
		KeWaitForSingleObject(threadObject, Executive, KernelMode, FALSE, NULL);
		ObDereferenceObject(threadObject);
	}

	ZwClose(threadHandle);
	DevExt->CaptureThreadHandle = NULL;
	// The worker has fully exited and no queued write can still reference the
	// stopping state. Clear it so ordinary writes after CMD2 do not continue
	// producing misleading "write seen/write bypass" trace messages.
	InterlockedExchange(&DevExt->CaptureStopping, 0);
}

static NTSTATUS CdpVolumeBackfillWriteAbsolute(
	_In_opt_ PVOID Context,
	_In_ UINT64 AbsoluteOffset,
	_In_ ULONG Length,
	_In_reads_bytes_(Length) const VOID* Buffer)
{
	PCdp_DEVICE_EXTENSION sourceExt = (PCdp_DEVICE_EXTENSION)Context;
	UINT64 sourceEnd;
	UINT64 volumeOffset;
	NTSTATUS status;
	PETHREAD currentThread;
	PDEVICE_OBJECT volumeLower = NULL;
	BOOLEAN volumeLowerReferenced = FALSE;

	if (!sourceExt || !Buffer || Length == 0 ||
		!sourceExt->DiskLayoutValid || sourceExt->PartitionSize == 0 ||
		sourceExt->PartitionStart > MAXUINT64 - sourceExt->PartitionSize)
	{
		return STATUS_INVALID_PARAMETER;
	}
	sourceEnd = sourceExt->PartitionStart + sourceExt->PartitionSize;
	if (AbsoluteOffset < sourceExt->PartitionStart ||
		AbsoluteOffset >= sourceEnd || Length > sourceEnd - AbsoluteOffset ||
		sourceExt->SectorSize == 0 ||
		(AbsoluteOffset % sourceExt->SectorSize) != 0 ||
		(Length % sourceExt->SectorSize) != 0)
	{
		Cdp_LOG("[DRAIN-VOLUME-WRITE-FAIL] reason=absolute-range source=[%llu,%llu) offset=%llu len=%lu sector=%lu\n",
			sourceExt->PartitionStart,
			sourceEnd,
			AbsoluteOffset,
			Length,
			sourceExt->SectorSize);
		return STATUS_INVALID_PARAMETER;
	}
	if (sourceExt->DeviceKind == Cdp_DEVICE_KIND_VOLUME)
		volumeLower = sourceExt->LowerDeviceObject;
	else if (sourceExt->DeviceKind == Cdp_DEVICE_KIND_DISK)
	{
		PCdp_DRIVER_EXTENSION driverExt =
			IoGetDriverObjectExtension(g_DriverObject, &g_DriverObject);
		volumeLower = CdpReferenceVolumeLowerByPhysicalRange(
			driverExt,
			sourceExt->DiskNumber,
			sourceExt->PartitionStart,
			sourceExt->PartitionSize);
		volumeLowerReferenced = volumeLower != NULL;
	}
	if (!volumeLower)
	{
		Cdp_LOG("[DRAIN-VOLUME-WRITE-FAIL] reason=volume-lower-unavailable disk=%lu part=%lu offset=%llu len=%lu\n",
			sourceExt->DiskNumber,
			sourceExt->PartitionNumber,
			AbsoluteOffset,
			Length);
		return STATUS_DEVICE_NOT_READY;
	}

	/* MetaTree remains in the unified absolute coordinate system. Translate
	 * exactly once at the volume-filter boundary, then submit below our source
	 * volume attachment. This is safe for a mounted volume and does not re-enter
	 * CdpDriver, so no Tail.Overlay/DriverContext marker is used. */
	volumeOffset = AbsoluteOffset - sourceExt->PartitionStart;
	if (InterlockedCompareExchange(
			&sourceExt->BackfillWriteActive, 0, 0) != 0)
	{
		Cdp_LOG("[DRAIN-VOLUME-WRITE-FAIL] reason=backfill-already-active offset=%llu len=%lu\n",
			AbsoluteOffset, Length);
		if (volumeLowerReferenced)
			ObDereferenceObject(volumeLower);
		return STATUS_INVALID_DEVICE_STATE;
	}
	currentThread = PsGetCurrentThread();
	sourceExt->BackfillAbsoluteOffset = AbsoluteOffset;
	sourceExt->BackfillLength = Length;
	InterlockedExchangePointer(
		(PVOID volatile*)&sourceExt->BackfillWriteThread, currentThread);
	KeMemoryBarrier();
	InterlockedExchange(&sourceExt->BackfillWriteActive, 1);
	status = CdpDevStoreWriteVolumeRelative(
		volumeLower,
		volumeOffset,
		Length,
		Buffer);
	InterlockedExchange(&sourceExt->BackfillWriteActive, 0);
	KeMemoryBarrier();
	InterlockedExchangePointer(
		(PVOID volatile*)&sourceExt->BackfillWriteThread, NULL);
	sourceExt->BackfillAbsoluteOffset = 0;
	sourceExt->BackfillLength = 0;
	if (volumeLowerReferenced)
		ObDereferenceObject(volumeLower);
	if (!NT_SUCCESS(status))
	{
		Cdp_LOG("[DRAIN-VOLUME-WRITE-FAIL] reason=lower-write status=0x%08X disk=%lu part=%lu absoluteOffset=%llu volumeOffset=%llu len=%lu\n",
			status,
			sourceExt->DiskNumber,
			sourceExt->PartitionNumber,
			AbsoluteOffset,
			volumeOffset,
			Length);
	}
	return status;
}

static NTSTATUS CdpDrainAndDisableCapture(
	_Inout_ PCdp_DEVICE_EXTENSION DevExt)
{
	NTSTATUS status = STATUS_SUCCESS;
	BOOLEAN complete = FALSE;
	UINT64 drainedBytes = 0;
	ULONG drainedRanges = 0;
	LONG previousPhase;

	if (!DevExt || !DevExt->Core ||
		InterlockedCompareExchange(&DevExt->CaptureEnabled, 0, 0) == 0)
	{
		return STATUS_SUCCESS;
	}
	previousPhase = InterlockedCompareExchange(
		&DevExt->Phase,
		(LONG)Cdp_PHASE_DRAINING,
		(LONG)Cdp_PHASE_GENERAL);
	if (previousPhase != (LONG)Cdp_PHASE_GENERAL)
		return STATUS_INVALID_DEVICE_STATE;

	InterlockedExchange(&DevExt->DrainFailureStatus, 0);
	CdpStopMergeThread(DevExt);
	while (InterlockedCompareExchange(
			&DevExt->RedirectWritesInFlight, 0, 0) != 0)
	{
		KeWaitForSingleObject(
			&DevExt->RedirectWritesDrainedEvent,
			Executive,
			KernelMode,
			FALSE,
			NULL);
	}
	Cdp_LOG("[DRAIN] graceful protection shutdown begin\n");

	while (!complete)
	{
		UINT64 offset = 0;
		ULONG length = 0;
		LONG asynchronousFailure;

		KeWaitForSingleObject(
			&DevExt->HistoryMutex,
			Executive,
			KernelMode,
			FALSE,
			NULL);
		asynchronousFailure = InterlockedCompareExchange(
			&DevExt->DrainFailureStatus, 0, 0);
		if (asynchronousFailure != 0)
		{
			status = (NTSTATUS)asynchronousFailure;
		}
		else if (!DevExt->Core)
		{
			status = STATUS_DEVICE_NOT_READY;
		}
		else
		{
			status = CdpCoreDrainOneMetaRangeWithWriter(
				DevExt->Core,
				CdpVolumeBackfillWriteAbsolute,
				DevExt,
				&complete,
				&offset,
				&length);
			if (NT_SUCCESS(status) && length != 0)
			{
				drainedBytes += length;
				drainedRanges++;
				Cdp_DBG("[DRAIN] range committed offset=%llu len=%lu\n",
					offset, length);
			}
		}
		if (NT_SUCCESS(status) && complete)
		{
			/* No MetaTree coverage remains. Publish pass-through before
			 * releasing HistoryMutex, so reads cannot observe a transition
			 * between the final source write and CaptureEnabled=0. */
			InterlockedExchange(&DevExt->CaptureEnabled, 0);
		}
		KeReleaseMutex(&DevExt->HistoryMutex, FALSE);

		if (!NT_SUCCESS(status))
			break;
	}

	if (!NT_SUCCESS(status))
	{
		InterlockedExchange(&DevExt->Phase, (LONG)Cdp_PHASE_GENERAL);
		InterlockedExchange(&DevExt->DrainFailureStatus, 0);
		CdpStartMergeIfNeeded(DevExt);
		Cdp_LOG("[DRAIN] graceful shutdown failed status=0x%08X ranges=%lu bytes=%llu; protection remains active\n",
			status, drainedRanges, drainedBytes);
		return status;
	}

	Cdp_LOG("[DRAIN] graceful shutdown complete ranges=%lu bytes=%llu\n",
		drainedRanges, drainedBytes);
	return STATUS_SUCCESS;
}

VOID CdpDisableAndDestroyCapture(_Inout_ PCdp_DEVICE_EXTENSION DevExt)
{
	PCdp_CORE core;
	PCdp_VOLUME_HANDLE_ENTRY redirectJournalEntry;

	if (!DevExt)
		return;
	/* Stop new Disk Upper items first. Any dispatcher that already took an
	 * outstanding reference either queues safely or observes this clear and
	 * drops the reference without touching Core. */
	InterlockedExchange(&DevExt->DiskIoAccepting, 0);
	InterlockedExchange(&DevExt->ProtectionStateValidated, 0);
	CdpAuditProtectedReadSummary(DevExt, "protection-disable");

	while (InterlockedCompareExchange(
			&DevExt->DiskIoOutstanding, 0, 0) != 0)
	{
		KeWaitForSingleObject(
			&DevExt->DiskIoDrainedEvent,
			Executive,
			KernelMode,
			FALSE,
			NULL);
	}
	InterlockedExchange(&DevExt->CaptureEnabled, 0);
	InterlockedExchange(&DevExt->Phase, (LONG)Cdp_PHASE_GENERAL);
	CdpStopMergeThread(DevExt);
	while (InterlockedCompareExchange(
			&DevExt->RedirectWritesInFlight, 0, 0) != 0)
	{
		KeWaitForSingleObject(
			&DevExt->RedirectWritesDrainedEvent,
			Executive,
			KernelMode,
			FALSE,
			NULL);
	}
	/* Finish writes queued during DRAINING before queued reads are converted to
	 * source pass-through, preserving the final source view at close return. */
	CdpStopCaptureWorker(DevExt);

	/* This is the matching release for the protection-session reference stored
	 * when capture was enabled.  Redirect writes never acquire/release it. */
	redirectJournalEntry = (PCdp_VOLUME_HANDLE_ENTRY)InterlockedExchangePointer(
		(PVOID volatile*)&DevExt->RedirectJournalEntry, NULL);
	if (redirectJournalEntry)
		CdpReleaseVolumeHandleEntry(redirectJournalEntry);

	// The capture worker holds HistoryMutex whenever it can access Core.  Take
	// ownership under that mutex only after the worker has stopped, then free
	// Core outside every spin lock and outside the mutex.
	KeWaitForSingleObject(&DevExt->HistoryMutex,
		Executive, KernelMode, FALSE, NULL);
	core = DevExt->Core;
	DevExt->Core = NULL;
	KeReleaseMutex(&DevExt->HistoryMutex, FALSE);

	if (core)
		CdpCoreDestroy(core);
}

NTSTATUS CdpIrpDispatchShutdown(
	_In_ PDEVICE_OBJECT DeviceObject,
	_Inout_ PIRP Irp)
{
	PCdp_DEVICE_EXTENSION deviceExt =
		(PCdp_DEVICE_EXTENSION)DeviceObject->DeviceExtension;

	if (!deviceExt || !deviceExt->LowerDeviceObject)
		return CdpCompleteIrp(Irp, STATUS_SUCCESS, 0);

	if (deviceExt->DeviceKind == Cdp_DEVICE_KIND_DISK)
		return CdpQueueDiskCaptureIrp(deviceExt, Irp);
	return CdpSendToNextDevice(deviceExt->LowerDeviceObject, Irp);
}

NTSTATUS CdpIrpDispatchWrite(_In_ PDEVICE_OBJECT DeviceObject, _Inout_ PIRP Irp)
{
	PCdp_DEVICE_EXTENSION deviceExt = (PCdp_DEVICE_EXTENSION)DeviceObject->DeviceExtension;

	if (!deviceExt || !deviceExt->LowerDeviceObject)
		return CdpCompleteIrp(Irp, STATUS_INVALID_DEVICE_REQUEST, 0);
	if (deviceExt->DeviceKind == Cdp_DEVICE_KIND_DISK)
		return CdpQueueDiskCaptureIrp(deviceExt, Irp);
	if (deviceExt->DeviceKind == Cdp_DEVICE_KIND_VOLUME)
		return CdpSendToNextDevice(deviceExt->LowerDeviceObject, Irp);
	return CdpCompleteIrp(Irp, STATUS_INVALID_DEVICE_REQUEST, 0);
}

static NTSTATUS PnpCompletionRoutine(PDEVICE_OBJECT DeviceObject, PIRP Irp, PVOID Context)
{
	UNREFERENCED_PARAMETER(DeviceObject);
	UNREFERENCED_PARAMETER(Irp);
	KeSetEvent((PKEVENT)Context, IO_NO_INCREMENT, FALSE);
	return STATUS_MORE_PROCESSING_REQUIRED;
}

NTSTATUS CdpIrpDispatchPnp(_In_ PDEVICE_OBJECT DeviceObject, _Inout_ PIRP Irp)
{
	PCdp_DEVICE_EXTENSION DevExt = DeviceObject->DeviceExtension;
	PIO_STACK_LOCATION IrpSp = IoGetCurrentIrpStackLocation(Irp);
	PCdp_DRIVER_EXTENSION DriverExt = NULL;

	if (!DevExt)
		return CdpIrpDispatchDefault(DeviceObject, Irp);

	DriverExt = IoGetDriverObjectExtension(g_DriverObject, &g_DriverObject);
	if (!DriverExt)
		return CdpSendToNextDevice(DevExt->LowerDeviceObject, Irp);

	switch (IrpSp->MinorFunction)
	{
	case IRP_MN_START_DEVICE:
	{
		KEVENT event;
		NTSTATUS status;

		KeInitializeEvent(&event, NotificationEvent, FALSE);
		IoCopyCurrentIrpStackLocationToNext(Irp);
		IoSetCompletionRoutine(
			Irp,
			PnpCompletionRoutine,
			&event,
			TRUE,
			TRUE,
			TRUE);
		status = IoCallDriver(DevExt->LowerDeviceObject, Irp);
		if (status == STATUS_PENDING)
			KeWaitForSingleObject(
				&event, Executive, KernelMode, FALSE, NULL);
		status = Irp->IoStatus.Status;
		if (NT_SUCCESS(status))
		{
			InterlockedExchange(&DevExt->Started, 1);
			if (DevExt->DeviceKind == Cdp_DEVICE_KIND_DISK)
			{
				STORAGE_DEVICE_NUMBER number;
				NTSTATUS discoveryStatus;
				RtlZeroMemory(&number, sizeof(number));
				if (NT_SUCCESS(CdpSendDeviceControlSynchronously(
						DevExt->LowerDeviceObject,
						IOCTL_STORAGE_GET_DEVICE_NUMBER,
						&number,
						sizeof(number))))
				{
					DevExt->DiskNumber = number.DeviceNumber;
					DevExt->DiskLayoutValid = TRUE;
				}
				discoveryStatus = KeWaitForSingleObject(
					&DriverExt->CaptureConfigMutex,
					Executive, KernelMode, FALSE, NULL);
				if (NT_SUCCESS(discoveryStatus))
				{
					discoveryStatus = CdpDiscoverJournalForStartedDisk(
						DriverExt, DevExt);
					KeReleaseMutex(
						&DriverExt->CaptureConfigMutex, FALSE);
				}
				if (!NT_SUCCESS(discoveryStatus) &&
					discoveryStatus != STATUS_NOT_FOUND)
				{
					Cdp_LOG("[DISK-PRESTART] discovery failed status=0x%08X disk=%lu; disk START continues unprotected\n",
						discoveryStatus, DevExt->DiskNumber);
				}
			}
			else if (DevExt->DeviceKind == Cdp_DEVICE_KIND_VOLUME)
			{
				NTSTATUS discoveryStatus = KeWaitForSingleObject(
					&DriverExt->CaptureConfigMutex,
					Executive, KernelMode, FALSE, NULL);
				if (NT_SUCCESS(discoveryStatus))
				{
					discoveryStatus =
						CdpDiscoverAdjacentJournalForStartedVolume(
							DriverExt, DevExt);
					KeReleaseMutex(
						&DriverExt->CaptureConfigMutex, FALSE);
				}
				if (!NT_SUCCESS(discoveryStatus) &&
					discoveryStatus != STATUS_NOT_FOUND)
				{
					Cdp_LOG("[AUTO-ADJACENT] pre-mount discovery failed status=0x%08X filter=%p; volume released without protection\n",
						discoveryStatus, DeviceObject);
				}
			}
		}
		IoCompleteRequest(Irp, IO_NO_INCREMENT);
		return status;
	}

	case IRP_MN_REMOVE_DEVICE:
	{
		KIRQL OldIrql;
		PCdp_DEVICE_LIST_NODE NodeToFree = NULL;
		PDEVICE_OBJECT LowerDevice = NULL;
		NTSTATUS Status;

		KeAcquireSpinLock(&DriverExt->DeviceObjectListLock, &OldIrql);
		PLIST_ENTRY PEntry = DriverExt->DeviceObjectListHead.Flink;
		while (PEntry != &DriverExt->DeviceObjectListHead)
		{
			PCdp_DEVICE_LIST_NODE Node = CONTAINING_RECORD(PEntry, Cdp_DEVICE_LIST_NODE, Entry);
			if (Node->DeviceObject == DeviceObject)
			{
				RemoveEntryList(&Node->Entry);
				NodeToFree = Node;
				break;
			}
			PEntry = PEntry->Flink;
		}
		KeReleaseSpinLock(&DriverExt->DeviceObjectListLock, OldIrql);

		if (NodeToFree)
			cdpfree(NodeToFree);

		InterlockedExchange(&DevExt->Started, 0);
		CdpDisableAndDestroyCapture(DevExt);
		LowerDevice = DevExt->LowerDeviceObject;

		IoSkipCurrentIrpStackLocation(Irp);
		Status = IoCallDriver(LowerDevice, Irp);

		if (LowerDevice)
		{
			IoDetachDevice(LowerDevice);
			DevExt->LowerDeviceObject = NULL;
		}

		IoDeleteDevice(DeviceObject);
		return Status;
	}

	case IRP_MN_DEVICE_USAGE_NOTIFICATION:
	{
		if (IrpSp->Parameters.UsageNotification.Type != DeviceUsageTypePaging)
			return CdpSendToNextDevice(DevExt->LowerDeviceObject, Irp);

		BOOLEAN SetPagable = FALSE;
		if (!IrpSp->Parameters.UsageNotification.InPath &&
			DevExt->PagingPathCount == 1)
		{
			if (!(DeviceObject->Flags & DO_POWER_INRUSH))
			{
				DeviceObject->Flags |= DO_POWER_PAGABLE;
				SetPagable = TRUE;
			}
		}

		KEVENT Event;
		KeInitializeEvent(&Event, NotificationEvent, FALSE);
		IoCopyCurrentIrpStackLocationToNext(Irp);
		IoSetCompletionRoutine(Irp, PnpCompletionRoutine, &Event, TRUE, TRUE, TRUE);
		NTSTATUS Status = IoCallDriver(DevExt->LowerDeviceObject, Irp);
		if (Status == STATUS_PENDING)
		{
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
		else if (SetPagable)
		{
			DeviceObject->Flags &= ~DO_POWER_PAGABLE;
		}

		return CdpCompleteIrp(Irp, Status, Irp->IoStatus.Information);
	}

	default:
		break;
	}

	return CdpSendToNextDevice(DevExt->LowerDeviceObject, Irp);
}

NTSTATUS CdpIrpDispatchPower(_In_ PDEVICE_OBJECT DeviceObject, _Inout_ PIRP Irp)
{
	PCdp_DEVICE_EXTENSION DevExt = DeviceObject->DeviceExtension;
	if (!DevExt)
		return CdpIrpDispatchDefault(DeviceObject, Irp);

#if (NTDDI_VERSION < NTDDI_VISTA)
	PoStartNextPowerIrp(Irp);
	IoSkipCurrentIrpStackLocation(Irp);
	return PoCallDriver(DevExt->LowerDeviceObject, Irp);
#else
	return CdpSendToNextDevice(DevExt->LowerDeviceObject, Irp);
#endif
}

static BOOLEAN CdpIsTrimRequest(
	_In_ PIRP Irp,
	_In_ PIO_STACK_LOCATION IrpSp)
{
	PDEVICE_MANAGE_DATA_SET_ATTRIBUTES attributes;
	ULONG inputLength;

	if (!Irp || !IrpSp ||
		IrpSp->Parameters.DeviceIoControl.IoControlCode !=
			IOCTL_STORAGE_MANAGE_DATA_SET_ATTRIBUTES)
	{
		return FALSE;
	}
	inputLength = IrpSp->Parameters.DeviceIoControl.InputBufferLength;
	if (!Irp->AssociatedIrp.SystemBuffer ||
		inputLength < sizeof(DEVICE_MANAGE_DATA_SET_ATTRIBUTES))
	{
		return FALSE;
	}
	attributes = (PDEVICE_MANAGE_DATA_SET_ATTRIBUTES)
		Irp->AssociatedIrp.SystemBuffer;
	if (attributes->Size < sizeof(DEVICE_MANAGE_DATA_SET_ATTRIBUTES) ||
		attributes->Size > inputLength)
	{
		return FALSE;
	}
	return attributes->Action == DeviceDsmAction_Trim;
}

static BOOLEAN CdpShouldSuppressTrim(
	_In_ PCdp_DEVICE_EXTENSION DevExt)
{
	PDEVICE_OBJECT sourceReference = NULL;
	PCdp_DEVICE_EXTENSION sourceExt;

	if (!DevExt)
		return FALSE;
	if (DevExt->DeviceKind == Cdp_DEVICE_KIND_VOLUME)
	{
		/* Keep suppressing while graceful disable/backfill is in progress.
		 * CaptureEnabled is cleared only after the source baseline is safe. */
		return InterlockedCompareExchange(
			&DevExt->CaptureEnabled, 0, 0) != 0;
	}
	if (DevExt->DeviceKind != Cdp_DEVICE_KIND_DISK)
		return FALSE;

	/* Disk DSM requests can contain ranges from more than one partition. If
	 * any protected source exists on this disk, suppress the whole request;
	 * losing an optimization on another partition is safer than trimming one
	 * byte of the immutable after-image baseline. */
	sourceExt = CdpReferenceProtectedSourceForDiskFlush(
		DevExt, &sourceReference);
	if (sourceReference)
		ObDereferenceObject(sourceReference);
	return sourceExt != NULL;
}

NTSTATUS CdpIrpDispatchDeviceControl(_In_ PDEVICE_OBJECT DeviceObject, _Inout_ PIRP Irp)
{
	PCdp_DEVICE_EXTENSION DevExt = (PCdp_DEVICE_EXTENSION)DeviceObject->DeviceExtension;
	PIO_STACK_LOCATION IrpSp = IoGetCurrentIrpStackLocation(Irp);
	PCdp_DRIVER_EXTENSION DriverExt = IoGetDriverObjectExtension(g_DriverObject, &g_DriverObject);
	BOOLEAN isControlDevice = (DriverExt != NULL && DeviceObject == DriverExt->ControlDevice);

	// ?????��?????????????????????? IOCTL
	if (isControlDevice)
	{
		switch (IrpSp->Parameters.DeviceIoControl.IoControlCode)
		{
		case IOCTL_Cdp_QUERY_PROTECT_STATUS:
		{
			if (IrpSp->Parameters.DeviceIoControl.OutputBufferLength < sizeof(BOOLEAN))
				return CdpCompleteIrp(Irp, STATUS_BUFFER_TOO_SMALL, 0);

			BOOLEAN isProtecting = FALSE;

			if (DriverExt)
			{
				KIRQL OldIrql;
				KeAcquireSpinLock(&DriverExt->DeviceObjectListLock, &OldIrql);
				PLIST_ENTRY Entry = DriverExt->DeviceObjectListHead.Flink;
				while (Entry != &DriverExt->DeviceObjectListHead)
				{
					PCdp_DEVICE_LIST_NODE Node = CONTAINING_RECORD(Entry, Cdp_DEVICE_LIST_NODE, Entry);
					PCdp_DEVICE_EXTENSION VolExt = (PCdp_DEVICE_EXTENSION)Node->DeviceObject->DeviceExtension;
					if (InterlockedCompareExchange(&VolExt->CaptureEnabled, 0, 0) != 0)
					{
						isProtecting = TRUE;
						break;
					}
					Entry = Entry->Flink;
				}
				KeReleaseSpinLock(&DriverExt->DeviceObjectListLock, OldIrql);
			}

			*(PBOOLEAN)Irp->AssociatedIrp.SystemBuffer = isProtecting;
			return CdpCompleteIrp(Irp, STATUS_SUCCESS, sizeof(BOOLEAN));
		}

		case IOCTL_Cdp_QUERY_CREDENTIAL:
		{
			PCdp_CREDENTIAL_STATUS_REPLY reply;
			Cdp_CREDENTIAL_DESCRIPTOR credential;
			ULONG count = 0;
			NTSTATUS status;
			if (!Irp->AssociatedIrp.SystemBuffer ||
				IrpSp->Parameters.DeviceIoControl.OutputBufferLength < sizeof(*reply))
			{
				return CdpCompleteIrp(Irp, STATUS_BUFFER_TOO_SMALL, 0);
			}
			reply = (PCdp_CREDENTIAL_STATUS_REPLY)Irp->AssociatedIrp.SystemBuffer;
			RtlZeroMemory(reply, sizeof(*reply));
			status = CdpGetSharedCredential(DriverExt, &credential, &count);
			if (status == STATUS_NOT_FOUND)
				return CdpCompleteIrp(Irp, STATUS_SUCCESS, sizeof(*reply));
			if (!NT_SUCCESS(status))
				return CdpCompleteIrp(Irp, status, 0);
			reply->Configured = 1;
			reply->JournalCount = count;
			reply->CredentialId = credential.CredentialId;
			reply->AuthEpoch = credential.AuthEpoch;
			return CdpCompleteIrp(Irp, STATUS_SUCCESS, sizeof(*reply));
		}

		case IOCTL_Cdp_AUTHENTICATE:
		{
			Cdp_AUTH_REQUEST request;
			Cdp_CREDENTIAL_DESCRIPTOR credential;
			PCdp_CONTROL_FILE_CONTEXT context =
				(PCdp_CONTROL_FILE_CONTEXT)IrpSp->FileObject->FsContext;
			NTSTATUS status;
			UINT64 now = KeQueryInterruptTime();
			if (!context || !Irp->AssociatedIrp.SystemBuffer ||
				IrpSp->Parameters.DeviceIoControl.InputBufferLength < sizeof(request))
			{
				return CdpCompleteIrp(Irp, STATUS_BUFFER_TOO_SMALL, 0);
			}
			request = *(PCdp_AUTH_REQUEST)Irp->AssociatedIrp.SystemBuffer;
			RtlSecureZeroMemory(Irp->AssociatedIrp.SystemBuffer, sizeof(request));
			if (request.PasswordLength == 0 ||
				request.PasswordLength > Cdp_PASSWORD_MAX_UTF8_BYTES)
			{
				RtlSecureZeroMemory(&request, sizeof(request));
				return CdpCompleteIrp(Irp, STATUS_INVALID_PARAMETER, 0);
			}
			if ((UINT64)InterlockedCompareExchange64(
				&DriverExt->AuthBlockedUntil100ns, 0, 0) > now)
			{
				RtlSecureZeroMemory(&request, sizeof(request));
				return CdpCompleteIrp(Irp, STATUS_ACCOUNT_LOCKED_OUT, 0);
			}
			status = CdpGetSharedCredential(DriverExt, &credential, NULL);
			if (NT_SUCCESS(status) &&
				!CdpCredentialVerify(request.Password, request.PasswordLength, &credential))
			{
				status = STATUS_ACCESS_DENIED;
			}
			RtlSecureZeroMemory(&request, sizeof(request));
			if (!NT_SUCCESS(status))
			{
				if (status == STATUS_ACCESS_DENIED &&
					InterlockedIncrement(&DriverExt->AuthFailureCount) >= 5)
				{
					InterlockedExchange(&DriverExt->AuthFailureCount, 0);
					InterlockedExchange64(&DriverExt->AuthBlockedUntil100ns,
						(LONGLONG)(now + 60ULL * 60ULL * 10000000ULL));
					status = STATUS_ACCOUNT_LOCKED_OUT;
				}
				RtlSecureZeroMemory(context, sizeof(*context));
				return CdpCompleteIrp(Irp, status, 0);
			}
			context->Authenticated = TRUE;
			InterlockedExchange(&DriverExt->AuthFailureCount, 0);
			InterlockedExchange64(&DriverExt->AuthBlockedUntil100ns, 0);
			context->CredentialId = credential.CredentialId;
			context->AuthEpoch = credential.AuthEpoch;
			context->ExpiresAt100ns = KeQueryInterruptTime() + 60ULL * 60ULL * 10000000ULL;
			return CdpCompleteIrp(Irp, STATUS_SUCCESS, 0);
		}

		case IOCTL_Cdp_CHANGE_PASSWORD:
		{
			Cdp_CHANGE_PASSWORD_REQUEST request;
			Cdp_CREDENTIAL_DESCRIPTOR oldCredential;
			Cdp_CREDENTIAL_DESCRIPTOR newCredential;
			PCdp_CONTROL_FILE_CONTEXT context =
				(PCdp_CONTROL_FILE_CONTEXT)IrpSp->FileObject->FsContext;
			PLIST_ENTRY entry;
			PCdp_VOLUME_HANDLE_ENTRY* journalEntries = NULL;
			ULONG journalCapacity = 0;
			ULONG journalCount = 0;
			ULONG changedCount = 0;
			ULONG journalIndex;
			NTSTATUS status;
			if (!context || !context->Authenticated ||
				KeQueryInterruptTime() >= context->ExpiresAt100ns ||
				!Irp->AssociatedIrp.SystemBuffer ||
				IrpSp->Parameters.DeviceIoControl.InputBufferLength < sizeof(request))
			{
				return CdpCompleteIrp(Irp, STATUS_ACCESS_DENIED, 0);
			}
			request = *(PCdp_CHANGE_PASSWORD_REQUEST)Irp->AssociatedIrp.SystemBuffer;
			RtlSecureZeroMemory(Irp->AssociatedIrp.SystemBuffer, sizeof(request));
			if (request.PasswordLength == 0 ||
				request.PasswordLength > Cdp_PASSWORD_MAX_UTF8_BYTES)
			{
				RtlSecureZeroMemory(&request, sizeof(request));
				return CdpCompleteIrp(Irp, STATUS_INVALID_PARAMETER, 0);
			}
			status = CdpGetSharedCredential(DriverExt, &oldCredential, NULL);
			if (!NT_SUCCESS(status) ||
				RtlCompareMemory(&context->CredentialId, &oldCredential.CredentialId,
					sizeof(GUID)) != sizeof(GUID) ||
				context->AuthEpoch != oldCredential.AuthEpoch)
			{
				RtlSecureZeroMemory(&request, sizeof(request));
				return CdpCompleteIrp(Irp, STATUS_ACCESS_DENIED, 0);
			}
			status = CdpCredentialCreate(
				request.Password, request.PasswordLength, &newCredential);
			RtlSecureZeroMemory(&request, sizeof(request));
			if (!NT_SUCCESS(status))
				return CdpCompleteIrp(Irp, status, 0);
			newCredential.CredentialId = oldCredential.CredentialId;
			newCredential.AuthEpoch = oldCredential.AuthEpoch + 1;
			if (newCredential.AuthEpoch == 0)
				newCredential.AuthEpoch = 1;

			/*
			 * Pin matching journal entries while holding the list mutex, then drop
			 * that mutex before writing/flushing superblocks. Raw disk I/O can re-enter
			 * this driver and must never run under VolumeHandleMutex.
			 */
			ExAcquireFastMutex(&DriverExt->VolumeHandleMutex);
			for (entry = DriverExt->VolumeHandleList.Flink;
				entry != &DriverExt->VolumeHandleList; entry = entry->Flink)
			{
				++journalCapacity;
			}
			if (journalCapacity != 0)
			{
				journalEntries = (PCdp_VOLUME_HANDLE_ENTRY*)cdpalloc(
					sizeof(*journalEntries) * journalCapacity);
			}
			if (journalCapacity != 0 && !journalEntries)
			{
				ExReleaseFastMutex(&DriverExt->VolumeHandleMutex);
				RtlSecureZeroMemory(&newCredential, sizeof(newCredential));
				RtlSecureZeroMemory(&oldCredential, sizeof(oldCredential));
				return CdpCompleteIrp(Irp, STATUS_INSUFFICIENT_RESOURCES, 0);
			}
			for (entry = DriverExt->VolumeHandleList.Flink;
				entry != &DriverExt->VolumeHandleList; entry = entry->Flink)
			{
				PCdp_VOLUME_HANDLE_ENTRY item =
					CONTAINING_RECORD(entry, Cdp_VOLUME_HANDLE_ENTRY, Entry);
				Cdp_CREDENTIAL_DESCRIPTOR current;
				if (item->Closing || !item->Journal.Mounted ||
					!CdpJournalGetCredential(&item->Journal, &current) ||
					RtlCompareMemory(&oldCredential, &current, sizeof(current)) !=
						sizeof(current))
				{
					continue;
				}
				InterlockedIncrement(&item->ReferenceCount);
				journalEntries[journalCount++] = item;
			}
			ExReleaseFastMutex(&DriverExt->VolumeHandleMutex);

			status = journalCount != 0 ? STATUS_SUCCESS : STATUS_NOT_FOUND;
			for (journalIndex = 0; journalIndex < journalCount; ++journalIndex)
			{
				status = CdpJournalSetCredential(
					&journalEntries[journalIndex]->Journal, &newCredential);
				changedCount = journalIndex + 1;
				if (!NT_SUCCESS(status))
					break;
			}
			if (!NT_SUCCESS(status))
			{
				NTSTATUS originalStatus = status;
				for (journalIndex = 0; journalIndex < changedCount; ++journalIndex)
				{
					(void)CdpJournalSetCredential(
						&journalEntries[journalIndex]->Journal, &oldCredential);
				}
				status = originalStatus;
			}
			for (journalIndex = 0; journalIndex < journalCount; ++journalIndex)
				CdpReleaseVolumeHandleEntry(journalEntries[journalIndex]);
			if (journalEntries)
				cdpfree(journalEntries);
			if (NT_SUCCESS(status))
			{
				context->CredentialId = newCredential.CredentialId;
				context->AuthEpoch = newCredential.AuthEpoch;
			}
			RtlSecureZeroMemory(&newCredential, sizeof(newCredential));
			RtlSecureZeroMemory(&oldCredential, sizeof(oldCredential));
			return CdpCompleteIrp(Irp, status, 0);
		}

		case IOCTL_Cdp_SEND_COMMAND:
		{
			PULONG pCode;
			PCdp_COMMAND_REPLY reply;
			ULONG outLen = IrpSp->Parameters.DeviceIoControl.OutputBufferLength;
			ULONG inLen = IrpSp->Parameters.DeviceIoControl.InputBufferLength;

			if (inLen < sizeof(ULONG) ||
				outLen < sizeof(Cdp_COMMAND_REPLY) ||
				!Irp->AssociatedIrp.SystemBuffer)
			{
				return CdpCompleteIrp(Irp, STATUS_BUFFER_TOO_SMALL, 0);
			}

			pCode = (PULONG)Irp->AssociatedIrp.SystemBuffer;

			switch (*pCode)
			{
			case Cdp_CMD_1:
			{
				Cdp_CMD1_REQUEST_V2 local;
				Cdp_CREDENTIAL_DESCRIPTOR credential;
				const Cdp_CREDENTIAL_DESCRIPTOR* credentialPtr = NULL;
				UINT64 handleId = 0;
				NTSTATUS Status;

				if (inLen < sizeof(Cdp_CMD1_REQUEST))
					return CdpCompleteIrp(Irp, STATUS_BUFFER_TOO_SMALL, 0);

				RtlZeroMemory(&local, sizeof(local));
				RtlCopyMemory(&local, Irp->AssociatedIrp.SystemBuffer,
					inLen < sizeof(local) ? inLen : sizeof(local));
				reply = (PCdp_COMMAND_REPLY)Irp->AssociatedIrp.SystemBuffer;
				RtlZeroMemory(&credential, sizeof(credential));
				if (local.FormatJournal != 0)
				{
					Status = CdpGetSharedCredential(DriverExt, &credential, NULL);
					if (Status == STATUS_NOT_FOUND)
					{
						if (inLen < sizeof(Cdp_CMD1_REQUEST_V2) ||
							local.PasswordLength == 0 ||
							local.PasswordLength > Cdp_PASSWORD_MAX_UTF8_BYTES)
						{
							RtlSecureZeroMemory(&local, sizeof(local));
							return CdpCompleteIrp(Irp, STATUS_PASSWORD_RESTRICTION, 0);
						}
						Status = CdpCredentialCreate(local.Password,
							local.PasswordLength, &credential);
					}
					if (!NT_SUCCESS(Status))
					{
						RtlSecureZeroMemory(&local, sizeof(local));
						return CdpCompleteIrp(Irp, Status, 0);
					}
					credentialPtr = &credential;
				}

				Cdp_DBG("CMD1 received\n");
				Cdp_LOG("version=%s journal=v%lu build=%s\n",
					Cdp_DRIVER_VERSION_STRING,
					Cdp_JOURNAL_VERSION,
					Cdp_DRIVER_BUILD_STRING);
				CdpDbgGuid("  Guid1", &local.PartitionGuid1);
				CdpDbgGuid("  Guid2", &local.PartitionGuid2);
				Status = CdpConfigureCapture(
					DriverExt,
					&local.PartitionGuid1,
					&local.PartitionGuid2,
					local.FormatJournal != 0,
					credentialPtr,
					&handleId);
				RtlSecureZeroMemory(local.Password, sizeof(local.Password));
				RtlSecureZeroMemory(Irp->AssociatedIrp.SystemBuffer, inLen);
				if (!NT_SUCCESS(Status))
				{
					Cdp_LOG("CMD1 configure failed status=0x%08X\n", Status);
					CdpFillReply(reply, Cdp_CMD_1, (ULONG)Status, 0,
						L"ERROR: capture configuration failed");
					return CdpCompleteIrp(Irp, Status, sizeof(Cdp_COMMAND_REPLY));
				}
				CdpFillReply(reply, Cdp_CMD_1, 0, handleId,
					L"OK: capture configured");
				return CdpCompleteIrp(Irp, STATUS_SUCCESS, sizeof(Cdp_COMMAND_REPLY));
			}

			case Cdp_CMD_2:
			{
				Cdp_CMD2_REQUEST local;
				PCdp_DEVICE_EXTENSION sourceExt;
				UINT64 handleId;
				NTSTATUS Status;

				if (inLen < sizeof(Cdp_CMD2_REQUEST))
					return CdpCompleteIrp(Irp, STATUS_BUFFER_TOO_SMALL, 0);

				local = *(PCdp_CMD2_REQUEST)Irp->AssociatedIrp.SystemBuffer;
				reply = (PCdp_COMMAND_REPLY)Irp->AssociatedIrp.SystemBuffer;
				if (!CdpControlHandleAuthorized(
					Irp, DriverExt, &local.SourceVolumeGuid))
				{
					CdpFillReply(reply, Cdp_CMD_2, (ULONG)STATUS_ACCESS_DENIED, 0,
						L"ERROR: password authentication required");
					return CdpCompleteIrp(Irp, STATUS_ACCESS_DENIED,
						sizeof(Cdp_COMMAND_REPLY));
				}

				Cdp_DBG("CMD2 received\n");
				CdpDbgGuid("  Source", &local.SourceVolumeGuid);
				Status = KeWaitForSingleObject(
					&DriverExt->CaptureConfigMutex,
					Executive,
					KernelMode,
					FALSE,
					NULL);
				if (!NT_SUCCESS(Status))
				{
					CdpFillReply(reply, Cdp_CMD_2, (ULONG)Status, 0,
						L"ERROR: stop capture failed");
					return CdpCompleteIrp(Irp, Status, sizeof(Cdp_COMMAND_REPLY));
				}
				sourceExt = CdpFindSourceExtensionByGuid(
					DriverExt,
					&local.SourceVolumeGuid);
				handleId = sourceExt ? sourceExt->JournalHandleId : 0;
				if (handleId == 0)
				{
					/* A failed/late boot discovery can leave a valid mounted journal
					 * without an active source binding.  CMD2 is still allowed to
					 * retire that exact journal after password authentication. */
					handleId = CdpFindJournalHandleBySourceGuid(
						DriverExt, &local.SourceVolumeGuid);
					if (handleId != 0)
						Cdp_LOG("[CMD2] source not active; closing matching unpaired journal handle=%llu\n",
							handleId);
				}
				if (handleId == 0)
				{
					KeReleaseMutex(&DriverExt->CaptureConfigMutex, FALSE);
					CdpFillReply(reply, Cdp_CMD_2, (ULONG)STATUS_NOT_FOUND, 0,
						L"ERROR: capture is not configured for source");
					return CdpCompleteIrp(Irp, STATUS_NOT_FOUND, sizeof(Cdp_COMMAND_REPLY));
				}
				Status = CdpCloseVolumeHandle(DriverExt, handleId);
				KeReleaseMutex(&DriverExt->CaptureConfigMutex, FALSE);
				if (!NT_SUCCESS(Status))
				{
					Cdp_LOG("CMD2 stop capture failed status=0x%08X\n", Status);
					CdpFillReply(reply, Cdp_CMD_2, (ULONG)Status, 0,
						L"ERROR: stop capture failed");
					return CdpCompleteIrp(Irp, Status, sizeof(Cdp_COMMAND_REPLY));
				}
				Cdp_LOG("capture stopped for source\n");
				CdpFillReply(reply, Cdp_CMD_2, 0, 0, L"OK: capture stopped");
				return CdpCompleteIrp(Irp, STATUS_SUCCESS, sizeof(Cdp_COMMAND_REPLY));
			}

			default:
				reply = (PCdp_COMMAND_REPLY)Irp->AssociatedIrp.SystemBuffer;
				Cdp_LOG("unknown command code %lu on SEND_COMMAND\n", *pCode);
				CdpFillReply(reply, *pCode, (ULONG)STATUS_INVALID_PARAMETER, 0, L"ERROR: unknown command");
				return CdpCompleteIrp(Irp, STATUS_INVALID_PARAMETER, sizeof(Cdp_COMMAND_REPLY));
			}
		}

		case IOCTL_Cdp_BEGIN_PREVIEW:
		{
			Cdp_PREVIEW_BEGIN_REQUEST request;
			PCdp_PREVIEW_BEGIN_REPLY reply;
			ULONG inLen =
				IrpSp->Parameters.DeviceIoControl.InputBufferLength;
			ULONG outLen =
				IrpSp->Parameters.DeviceIoControl.OutputBufferLength;
			NTSTATUS status;

			if (!DriverExt || !Irp->AssociatedIrp.SystemBuffer ||
				inLen < sizeof(request) ||
				outLen < sizeof(Cdp_PREVIEW_BEGIN_REPLY))
			{
				return CdpCompleteIrp(
					Irp,
					STATUS_BUFFER_TOO_SMALL,
					0);
			}

			request =
				*(PCdp_PREVIEW_BEGIN_REQUEST)Irp->AssociatedIrp.SystemBuffer;
			reply =
				(PCdp_PREVIEW_BEGIN_REPLY)Irp->AssociatedIrp.SystemBuffer;
			status = CdpBeginPreviewSession(
				DriverExt,
				&request,
				reply);
			return CdpCompleteIrp(
				Irp,
				status,
				NT_SUCCESS(status) ? sizeof(*reply) : 0);
		}

		case IOCTL_Cdp_READ_PREVIEW:
		{
			PCdp_PREVIEW_READ_REQUEST request;
			PVOID outputBuffer;
			ULONG inLen =
				IrpSp->Parameters.DeviceIoControl.InputBufferLength;
			ULONG outLen =
				IrpSp->Parameters.DeviceIoControl.OutputBufferLength;
			NTSTATUS status;

			if (!DriverExt || !Irp->AssociatedIrp.SystemBuffer ||
				inLen < sizeof(Cdp_PREVIEW_READ_REQUEST))
			{
				return CdpCompleteIrp(
					Irp,
					STATUS_BUFFER_TOO_SMALL,
					0);
			}
			request =
				(PCdp_PREVIEW_READ_REQUEST)Irp->AssociatedIrp.SystemBuffer;
			if (!request->ByteLength ||
				outLen < request->ByteLength ||
				!Irp->MdlAddress)
			{
				return CdpCompleteIrp(
					Irp,
					STATUS_BUFFER_TOO_SMALL,
					0);
			}
			outputBuffer = MmGetSystemAddressForMdlSafe(
				Irp->MdlAddress,
				NormalPagePriority);
			if (!outputBuffer)
			{
				return CdpCompleteIrp(
					Irp,
					STATUS_INSUFFICIENT_RESOURCES,
					0);
			}

			status = CdpReadPreviewSession(
				DriverExt,
				request,
				outputBuffer);
			return CdpCompleteIrp(
				Irp,
				status,
				NT_SUCCESS(status) ? request->ByteLength : 0);
		}

		case IOCTL_Cdp_END_PREVIEW:
		{
			PCdp_PREVIEW_END_REQUEST request;
			ULONG inLen =
				IrpSp->Parameters.DeviceIoControl.InputBufferLength;
			NTSTATUS status;

			if (!DriverExt || !Irp->AssociatedIrp.SystemBuffer ||
				inLen < sizeof(Cdp_PREVIEW_END_REQUEST))
			{
				return CdpCompleteIrp(
					Irp,
					STATUS_BUFFER_TOO_SMALL,
					0);
			}
			request =
				(PCdp_PREVIEW_END_REQUEST)Irp->AssociatedIrp.SystemBuffer;
			status = CdpEndPreviewSession(
				DriverExt,
				request->PreviewHandle);
			return CdpCompleteIrp(Irp, status, 0);
		}

		case IOCTL_Cdp_QUERY_PHASE:
		{
			Cdp_PHASE_QUERY_REQUEST request;
			PCdp_PHASE_QUERY_REPLY reply;
			ULONG inLen =
				IrpSp->Parameters.DeviceIoControl.InputBufferLength;
			ULONG outLen =
				IrpSp->Parameters.DeviceIoControl.OutputBufferLength;
			NTSTATUS status;

			if (!DriverExt || !Irp->AssociatedIrp.SystemBuffer ||
				inLen < sizeof(Cdp_PHASE_QUERY_REQUEST) ||
				outLen < sizeof(Cdp_PHASE_QUERY_REPLY))
			{
				return CdpCompleteIrp(
					Irp,
					STATUS_BUFFER_TOO_SMALL,
					0);
			}
			request =
				*(PCdp_PHASE_QUERY_REQUEST)Irp->AssociatedIrp.SystemBuffer;
			reply = (PCdp_PHASE_QUERY_REPLY)Irp->AssociatedIrp.SystemBuffer;
			status = CdpQueryPhase(DriverExt, &request, reply);
			return CdpCompleteIrp(
				Irp,
				status,
				NT_SUCCESS(status) ? sizeof(*reply) : 0);
		}

		case IOCTL_Cdp_BEGIN_RECOVERY:
		{
			Cdp_RECOVERY_BEGIN_REQUEST request;
			PCdp_RECOVERY_BEGIN_REPLY reply;
			ULONG inLen =
				IrpSp->Parameters.DeviceIoControl.InputBufferLength;
			ULONG outLen =
				IrpSp->Parameters.DeviceIoControl.OutputBufferLength;
			NTSTATUS status;

			if (!DriverExt || !Irp->AssociatedIrp.SystemBuffer ||
				inLen < sizeof(Cdp_RECOVERY_BEGIN_REQUEST) ||
				outLen < sizeof(Cdp_RECOVERY_BEGIN_REPLY))
			{
				return CdpCompleteIrp(
					Irp,
					STATUS_BUFFER_TOO_SMALL,
					0);
			}
			request =
				*(PCdp_RECOVERY_BEGIN_REQUEST)Irp->AssociatedIrp.SystemBuffer;
			if (!CdpControlHandleAuthorized(Irp, DriverExt,
				&request.SourceVolumeGuid))
			{
				return CdpCompleteIrp(Irp, STATUS_ACCESS_DENIED, 0);
			}
			reply =
				(PCdp_RECOVERY_BEGIN_REPLY)Irp->AssociatedIrp.SystemBuffer;
			status = CdpBeginRecovery(DriverExt, &request, reply);
			return CdpCompleteIrp(
				Irp,
				status,
				NT_SUCCESS(status) ? sizeof(*reply) : 0);
		}

		case IOCTL_Cdp_COMMIT_RECOVERY:
		{
			Cdp_RECOVERY_CONTROL_REQUEST request;
			PCdp_RECOVERY_COMMIT_REPLY reply;
			ULONG inLen =
				IrpSp->Parameters.DeviceIoControl.InputBufferLength;
			ULONG outLen =
				IrpSp->Parameters.DeviceIoControl.OutputBufferLength;
			NTSTATUS status;

			if (!DriverExt || !Irp->AssociatedIrp.SystemBuffer ||
				inLen < sizeof(Cdp_RECOVERY_CONTROL_REQUEST) ||
				outLen < sizeof(Cdp_RECOVERY_COMMIT_REPLY))
			{
				return CdpCompleteIrp(
					Irp,
					STATUS_BUFFER_TOO_SMALL,
					0);
			}
			request =
				*(PCdp_RECOVERY_CONTROL_REQUEST)Irp->AssociatedIrp.SystemBuffer;
			if (!CdpControlHandleAuthorized(Irp, DriverExt,
				&request.SourceVolumeGuid))
			{
				return CdpCompleteIrp(Irp, STATUS_ACCESS_DENIED, 0);
			}
			reply =
				(PCdp_RECOVERY_COMMIT_REPLY)Irp->AssociatedIrp.SystemBuffer;
			status = CdpCommitRecovery(DriverExt, &request, reply);
			return CdpCompleteIrp(
				Irp,
				status,
				NT_SUCCESS(status) ? sizeof(*reply) : 0);
		}

		case IOCTL_Cdp_CANCEL_RECOVERY:
		{
			Cdp_RECOVERY_CONTROL_REQUEST request;
			ULONG inLen =
				IrpSp->Parameters.DeviceIoControl.InputBufferLength;
			NTSTATUS status;

			if (!DriverExt || !Irp->AssociatedIrp.SystemBuffer ||
				inLen < sizeof(Cdp_RECOVERY_CONTROL_REQUEST))
			{
				return CdpCompleteIrp(
					Irp,
					STATUS_BUFFER_TOO_SMALL,
					0);
			}
			request =
				*(PCdp_RECOVERY_CONTROL_REQUEST)Irp->AssociatedIrp.SystemBuffer;
			if (!CdpControlHandleAuthorized(Irp, DriverExt,
				&request.SourceVolumeGuid))
			{
				return CdpCompleteIrp(Irp, STATUS_ACCESS_DENIED, 0);
			}
			status = CdpCancelRecovery(DriverExt, &request);
			return CdpCompleteIrp(Irp, status, 0);
		}

		case IOCTL_Cdp_QUERY_TIME_RANGE:
		{
			Cdp_TIME_RANGE_QUERY_REQUEST request;
			PCdp_TIME_RANGE_QUERY_REPLY reply;
			ULONG inLen =
				IrpSp->Parameters.DeviceIoControl.InputBufferLength;
			ULONG outLen =
				IrpSp->Parameters.DeviceIoControl.OutputBufferLength;
			NTSTATUS status;

			if (!DriverExt || !Irp->AssociatedIrp.SystemBuffer ||
				inLen < sizeof(Cdp_TIME_RANGE_QUERY_REQUEST) ||
				outLen < sizeof(Cdp_TIME_RANGE_QUERY_REPLY))
			{
				return CdpCompleteIrp(
					Irp,
					STATUS_BUFFER_TOO_SMALL,
					0);
			}
			request =
				*(PCdp_TIME_RANGE_QUERY_REQUEST)Irp->AssociatedIrp.SystemBuffer;
			reply =
				(PCdp_TIME_RANGE_QUERY_REPLY)Irp->AssociatedIrp.SystemBuffer;
			status = CdpQueryTimeRange(DriverExt, &request, reply);
			return CdpCompleteIrp(
				Irp,
				status,
				NT_SUCCESS(status) ? sizeof(*reply) : 0);
		}

		case IOCTL_Cdp_QUERY_JOURNAL_USAGE:
		{
			Cdp_JOURNAL_USAGE_QUERY_REQUEST request;
			PCdp_JOURNAL_USAGE_QUERY_REPLY reply;
			ULONG inLen =
				IrpSp->Parameters.DeviceIoControl.InputBufferLength;
			ULONG outLen =
				IrpSp->Parameters.DeviceIoControl.OutputBufferLength;
			NTSTATUS status;

			if (!DriverExt || !Irp->AssociatedIrp.SystemBuffer ||
				inLen < sizeof(request) ||
				outLen < sizeof(Cdp_JOURNAL_USAGE_QUERY_REPLY))
			{
				return CdpCompleteIrp(
					Irp,
					STATUS_BUFFER_TOO_SMALL,
					0);
			}

			request =
				*(PCdp_JOURNAL_USAGE_QUERY_REQUEST)Irp->AssociatedIrp.SystemBuffer;
			reply =
				(PCdp_JOURNAL_USAGE_QUERY_REPLY)Irp->AssociatedIrp.SystemBuffer;
			status = CdpQueryJournalUsage(DriverExt, &request, reply);
			return CdpCompleteIrp(
				Irp,
				status,
				NT_SUCCESS(status) ? sizeof(*reply) : 0);
		}

		case IOCTL_Cdp_QUERY_JOURNAL_RECORDS:
		{
			Cdp_JOURNAL_RECORD_QUERY_REQUEST request;
			PCdp_JOURNAL_RECORD_QUERY_REPLY reply;
			ULONG inLen =
				IrpSp->Parameters.DeviceIoControl.InputBufferLength;
			ULONG outLen =
				IrpSp->Parameters.DeviceIoControl.OutputBufferLength;
			ULONG recordCapacity;
			NTSTATUS status;

			if (!DriverExt || !Irp->AssociatedIrp.SystemBuffer ||
				inLen < sizeof(request) ||
				outLen < sizeof(Cdp_JOURNAL_RECORD_QUERY_REPLY))
			{
				return CdpCompleteIrp(
					Irp,
					STATUS_BUFFER_TOO_SMALL,
					0);
			}

			request =
				*(PCdp_JOURNAL_RECORD_QUERY_REQUEST)Irp->AssociatedIrp.SystemBuffer;
			if (!CdpControlHandleAuthorized(Irp, DriverExt,
				&request.SourceVolumeGuid))
			{
				return CdpCompleteIrp(Irp, STATUS_ACCESS_DENIED, 0);
			}
			reply =
				(PCdp_JOURNAL_RECORD_QUERY_REPLY)Irp->AssociatedIrp.SystemBuffer;
			recordCapacity =
				(outLen - sizeof(*reply)) / sizeof(Cdp_JOURNAL_RECORD_INFO);
			status = CdpQueryJournalRecords(
				DriverExt,
				&request,
				reply,
				recordCapacity);
			return CdpCompleteIrp(
				Irp,
				status,
				NT_SUCCESS(status) ?
					sizeof(*reply) +
						(ULONG_PTR)reply->RecordCount *
							sizeof(Cdp_JOURNAL_RECORD_INFO) :
					0);
		}

		case IOCTL_Cdp_QUERY_JOURNAL_BRANCHES:
		{
			Cdp_JOURNAL_BRANCH_QUERY_REQUEST request;
			PCdp_JOURNAL_BRANCH_QUERY_REPLY reply;
			ULONG inLen =
				IrpSp->Parameters.DeviceIoControl.InputBufferLength;
			ULONG outLen =
				IrpSp->Parameters.DeviceIoControl.OutputBufferLength;
			ULONG branchCapacity;
			NTSTATUS status;

			if (!DriverExt || !Irp->AssociatedIrp.SystemBuffer ||
				inLen < sizeof(request) ||
				outLen < sizeof(Cdp_JOURNAL_BRANCH_QUERY_REPLY))
			{
				return CdpCompleteIrp(Irp, STATUS_BUFFER_TOO_SMALL, 0);
			}

			request =
				*(PCdp_JOURNAL_BRANCH_QUERY_REQUEST)Irp->AssociatedIrp.SystemBuffer;
			if (!CdpControlHandleAuthorized(Irp, DriverExt,
				&request.SourceVolumeGuid))
			{
				return CdpCompleteIrp(Irp, STATUS_ACCESS_DENIED, 0);
			}
			reply =
				(PCdp_JOURNAL_BRANCH_QUERY_REPLY)Irp->AssociatedIrp.SystemBuffer;
			branchCapacity =
				(outLen - sizeof(*reply)) / sizeof(Cdp_JOURNAL_BRANCH_INFO);
			status = CdpQueryJournalBranches(
				DriverExt,
				&request,
				reply,
				branchCapacity);
			return CdpCompleteIrp(
				Irp,
				status,
				NT_SUCCESS(status) ?
					sizeof(*reply) +
						(ULONG_PTR)reply->BranchCount *
							sizeof(Cdp_JOURNAL_BRANCH_INFO) :
					0);
		}

		case IOCTL_Cdp_QUERY_VERSION:
		{
			PCdp_VERSION_REPLY reply;
			ULONG outLen =
				IrpSp->Parameters.DeviceIoControl.OutputBufferLength;

			if (!Irp->AssociatedIrp.SystemBuffer ||
				outLen < sizeof(Cdp_VERSION_REPLY))
			{
				return CdpCompleteIrp(
					Irp,
					STATUS_BUFFER_TOO_SMALL,
					0);
			}

			reply = (PCdp_VERSION_REPLY)Irp->AssociatedIrp.SystemBuffer;
			RtlZeroMemory(reply, sizeof(*reply));
			reply->JournalVersion = Cdp_JOURNAL_VERSION;
			(void)RtlStringCbCopyA(
				reply->Version,
				sizeof(reply->Version),
				Cdp_DRIVER_VERSION_STRING);
			(void)RtlStringCbCopyA(
				reply->Build,
				sizeof(reply->Build),
				Cdp_DRIVER_BUILD_STRING);
			return CdpCompleteIrp(Irp, STATUS_SUCCESS, sizeof(*reply));
		}

		default:
			Cdp_LOG("unknown IOCTL 0x%08X on control device\n",
				IrpSp->Parameters.DeviceIoControl.IoControlCode);
			return CdpCompleteIrp(Irp, STATUS_INVALID_DEVICE_REQUEST, 0);
		}
	}

	if (!DevExt)
		return CdpCompleteIrp(Irp, STATUS_INVALID_DEVICE_REQUEST, 0);
	if (CdpIsTrimRequest(Irp, IrpSp) && CdpShouldSuppressTrim(DevExt))
	{
		Cdp_LOG("[COW-TRIM] suppressed kind=%lu disk=%lu part=%lu capture=%ld phase=%ld\n",
			(ULONG)DevExt->DeviceKind,
			DevExt->DiskNumber,
			DevExt->PartitionNumber,
			InterlockedCompareExchange(&DevExt->CaptureEnabled, 0, 0),
			InterlockedCompareExchange(&DevExt->Phase, 0, 0));
		return CdpCompleteIrp(Irp, STATUS_SUCCESS, 0);
	}

	return CdpSendToNextDevice(DevExt->LowerDeviceObject, Irp);
}
