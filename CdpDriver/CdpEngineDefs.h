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

#include "CdpIoctl.h"
#include "CdpJournal.h"

#define Cdp_DRIVER_VERSION_STRING "1.6.6-test1"
#define Cdp_DRIVER_BUILD_STRING   "20260824.86-multi-source-per-disk"

#define Cdp_COW_BATCH_MAX_ITEMS 16UL
#define Cdp_COW_BATCH_MAX_BYTES (16UL * 1024UL * 1024UL)

// Cdp_LOG: always (Release+Debug) — version / errors / rare lifecycle.
// Cdp_DBG: Debug builds only — verbose I/O and path tracing.
#define Cdp_LOG(fmt, ...) \
	DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, \
		"CdpDriver: " fmt, ##__VA_ARGS__)
#if DBG
#define Cdp_DBG(fmt, ...) Cdp_LOG(fmt, ##__VA_ARGS__)
#else
#define Cdp_DBG(fmt, ...) ((void)0)
#endif

#if (NTDDI_VERSION >= NTDDI_WIN10_VB)
#define cdpalloc(size) ExAllocatePool2(POOL_FLAG_NON_PAGED, size, 'NTAG')
#define cdpfree(P) ExFreePoolWithTag(P, 'NTAG')
#else
#define cdpalloc(size) ExAllocatePoolWithTag(NonPagedPool, size, 'NTAG')
#define cdpfree(P) ExFreePoolWithTag(P, 'NTAG')
#endif

extern PDRIVER_OBJECT g_DriverObject;

static __forceinline NTSTATUS CdpCompleteIrp(
	_In_ PIRP Irp,
	_In_ NTSTATUS Status,
	_In_ ULONG_PTR Information)
{
	Irp->IoStatus.Status = Status;
	Irp->IoStatus.Information = Information;
	IoCompleteRequest(Irp, IO_NO_INCREMENT);
	return Status;
}

typedef struct _Cdp_DEVICE_LIST_NODE
{
	PDEVICE_OBJECT DeviceObject;
	LIST_ENTRY Entry;
} Cdp_DEVICE_LIST_NODE, *PCdp_DEVICE_LIST_NODE;

typedef struct _Cdp_VOLUME_HANDLE_ENTRY
{
	LIST_ENTRY Entry;
	UINT64 HandleId;
	HANDLE FileHandle;
	// Volume stack below our filter. Capture writes go here to bypass the
	// mounted filesystem's DASD write denial (STATUS_ACCESS_DENIED).
	PDEVICE_OBJECT TargetLowerDevice;
	PDEVICE_OBJECT VolumeLowerDevice;
	// Referenced only by the auto-discovered journal. It keeps the volume-lower
	// object valid while RR/Header/Superblock I/O uses that stack.
	PDEVICE_OBJECT MetadataLowerDeviceReference;
	UINT64 TargetBaseOffset;
	ULONG DiskNumber;
	ULONG PartitionNumber;
	UINT64 PartitionSize;
	ULONG SectorSize;
	Cdp_JOURNAL Journal;
	// One reference is held while the entry is in VolumeHandleList.  Capture
	// operations take an extra reference so close cannot drop a handle while a
	// write callback is using it.
	volatile LONG ReferenceCount;
	BOOLEAN Closing;
	BOOLEAN VolumeGuidValid;
	GUID VolumeGuid;
	KEVENT NoReferences;
} Cdp_VOLUME_HANDLE_ENTRY, *PCdp_VOLUME_HANDLE_ENTRY;

typedef struct _Cdp_DRIVER_EXTENSION
{
	LIST_ENTRY DeviceObjectListHead;
	KSPIN_LOCK DeviceObjectListLock;
	PDEVICE_OBJECT ControlDevice;

	// 指令4 打开的卷句柄表（内核 HANDLE，用户态只持有 HandleId）
	LIST_ENTRY VolumeHandleList;
	FAST_MUTEX VolumeHandleMutex;
	volatile LONGLONG VolumeHandleNextId;
	// KMUTEX (not FastMutex): configure/auto-discover issue sync IoBuild*
	// IRPs and must stay at PASSIVE_LEVEL for the whole critical section.
	KMUTEX CaptureConfigMutex;

	// 按时间点读取的文件预览会话
	LIST_ENTRY PreviewSessionList;
	FAST_MUTEX PreviewSessionMutex;
	volatile LONGLONG PreviewSessionNextId;

	volatile LONG AuthFailureCount;
	volatile LONGLONG AuthBlockedUntil100ns;
} Cdp_DRIVER_EXTENSION, *PCdp_DRIVER_EXTENSION;

typedef struct _Cdp_CONTROL_FILE_CONTEXT
{
	BOOLEAN Authenticated;
	GUID CredentialId;
	UINT64 AuthEpoch;
	UINT64 ExpiresAt100ns;
} Cdp_CONTROL_FILE_CONTEXT, *PCdp_CONTROL_FILE_CONTEXT;

typedef struct _Cdp_CORE Cdp_CORE, *PCdp_CORE;

typedef struct _Cdp_PREVIEW_SESSION
{
	LIST_ENTRY Entry;
	UINT64 HandleId;
	UINT64 TargetTime100ns;
	UINT64 SourceVolumeHandleId;
	PCdp_VOLUME_HANDLE_ENTRY JournalEntry;
	GUID SourceVolumeGuid;
	volatile LONG ReferenceCount;
	BOOLEAN Closing;
	KEVENT NoReferences;
} Cdp_PREVIEW_SESSION, *PCdp_PREVIEW_SESSION;

typedef enum _Cdp_DEVICE_KIND
{
	Cdp_DEVICE_KIND_UNKNOWN = 0,
	Cdp_DEVICE_KIND_VOLUME = 1,
	Cdp_DEVICE_KIND_DISK = 2,
	/* Unattached per-partition protection context created during disk START.
	 * A disk can own any number of these contexts. */
	Cdp_DEVICE_KIND_SOURCE = 3
} Cdp_DEVICE_KIND;

typedef struct _Cdp_DEVICE_EXTENSION
{
	Cdp_DEVICE_KIND DeviceKind;
	volatile LONG CaptureEnabled;
	// Set only after the complete source/disk/journal/Core object graph has
	// passed fail-closed activation validation. Disk hot paths require both.
	volatile LONG ProtectionStateValidated;
	volatile LONG64 DiskJournalMirrorWriteCount;
	volatile LONG64 DiskJournalMirrorWriteBytes;
	volatile LONG64 DiskJournalMirrorFailureCount;
	volatile LONG64 DiskJournalAuditReadCount;
	volatile LONG64 DiskJournalAuditReadBytes;
	volatile LONG64 DiskJournalAuditReadHitCount;
	volatile LONG64 DiskJournalAuditReadFailureCount;
	// Sparse diagnostics for proving whether Disk Upper reads reach source
	// matching and the journal-audit branch.  Logged at 1 and every 4096 reads.
	volatile LONG64 DiskReadPathEntryCount;
	volatile LONG64 DiskReadPathNoSourceCount;
	volatile LONG64 DiskReadPathSourceMatchCount;
	// The source context accepts Disk Upper FIFO references only while this is
	// set. Disable clears it before waiting for DiskIoOutstanding to drain.
	volatile LONG DiskIoAccepting;
	volatile LONG DiskIoOutstanding;
	KEVENT DiskIoDrainedEvent;
	/* Set permanently once IRP_MJ_SHUTDOWN reaches the disk filter.  No new
	 * protected write may bypass to the source while the ordered queue and
	 * journal cache are being drained. */
	volatile LONG ShutdownInProgress;
	volatile LONG Phase;
	// START_DEVICE publishes this before pre-mount discovery uses the lower
	// device stack.
	volatile LONG Started;
	BOOLEAN VolumeGuidValid;
	// Physical partition identity captured after START_DEVICE.  The complete
	// disk layout lets discovery identify the physically adjacent successor.
	BOOLEAN DiskLayoutValid;
	BOOLEAN HasNextPartition;
	ULONG DiskNumber;
	ULONG PartitionNumber;
	ULONG DiskPartitionStyle;
	ULONG MbrSignature;
	GUID DiskGuid;
	UINT64 PartitionStart;
	UINT64 PartitionSize;
	ULONG NextPartitionNumber;
	UINT64 NextPartitionStart;
	UINT64 NextPartitionSize;
	GUID VolumeGuid;
	PDEVICE_OBJECT FilterDeviceObject;
	PDEVICE_OBJECT LowerDeviceObject;
	PDEVICE_OBJECT PhysicalDeviceObject;
	volatile LONG PagingPathCount;
	ULONG SectorSize;
	KSPIN_LOCK CaptureQueueLock;
	LIST_ENTRY CaptureQueue;
	KEVENT CaptureEvent;
	HANDLE CaptureThreadHandle;
	volatile LONG CaptureStopping;
	volatile LONG RedirectWritesInFlight;
	KEVENT RedirectWritesDrainedEvent;
	// First failure observed while graceful disable is writing/punching the
	// current MetaTree. Zero means the drain may continue.
	volatile LONG DrainFailureStatus;
	/* Out-of-band identity for the one synchronous volume-layer backfill write.
	 * The partition stack may replace the IRP, so identity must never live in
	 * Tail.Overlay.DriverContext. Disk Upper matches owner thread + range. */
	volatile LONG BackfillWriteActive;
	PETHREAD volatile BackfillWriteThread;
	UINT64 BackfillAbsoluteOffset;
	ULONG BackfillLength;
	HANDLE MergeThreadHandle;
	volatile LONG MergeThreadRunning;
	volatile LONG MergeThreadStopping;
	KEVENT MergeThreadDoneEvent;
	KMUTEX HistoryMutex;
	PCdp_CORE Core;
	// Journal VolumeHandleList entry used while CaptureEnabled is set.
	UINT64 JournalHandleId;
	// Direct-redirect test path keeps one reference for the entire protection
	// session.  Write dispatch must only read this cached entry; acquiring it for
	// every I/O serializes on VolumeHandleMutex and destroys large-I/O throughput.
	PCdp_VOLUME_HANDLE_ENTRY RedirectJournalEntry;
	volatile LONG CaptureQueueDepth;
	/* Low-volume protected-read audit. Reset when protection is enabled and
	 * summarized when it is disabled. All byte counters are monotonic for one
	 * protection session. */
	volatile LONG64 AuditReadSeenCount;
	volatile LONG64 AuditReadSeenBytes;
	volatile LONG64 AuditReadCoreSuccessCount;
	volatile LONG64 AuditReadCoreSuccessBytes;
	volatile LONG64 AuditReadCoreFailureCount;
	volatile LONG64 AuditReadSourceBypassCount;
	volatile LONG64 AuditReadSourceBypassBytes;
	volatile LONG AuditReadBypassReported;
} Cdp_DEVICE_EXTENSION, *PCdp_DEVICE_EXTENSION;

typedef struct _Cdp_CAPTURE_ITEM
{
	LIST_ENTRY Entry;
	PIRP Irp;
	UINT64 OriginalDiskOffset;
	PDEVICE_OBJECT SourceReference;
	PDEVICE_OBJECT OriginLowerReference;
} Cdp_CAPTURE_ITEM, *PCdp_CAPTURE_ITEM;
