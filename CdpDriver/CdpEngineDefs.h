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

#define Cdp_DRIVER_VERSION_STRING "1.5.47-test1"
#define Cdp_DRIVER_BUILD_STRING   "20260817.47-dead-code-cleanup"

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

	WORK_QUEUE_ITEM AutoDiscoveryWorkItem;
	volatile LONG AutoDiscoveryQueued;
	volatile LONG AutoDiscoveryStopping;
	volatile LONG AutoDiscoverySuppressed;
	// Set only from the boot-driver reinitialization callback, which the I/O
	// manager invokes after the boot PnP enumeration/start pass completes.
	// No volume gate may be opened before this becomes nonzero.
	volatile LONG BootEnumerationComplete;
	// 0 until every started volume is classified and no journal is waiting
	// on a not-yet-started source (or CDP has been enabled).
	volatile LONG AutoDiscoverySettled;
	// Boot-driver reinitialization can run before late data/journal volumes
	// receive START_DEVICE.  Count consecutive quiet discovery passes before
	// opening unmatched volume gates; every new START_DEVICE resets the count.
	volatile LONG AutoDiscoveryStablePasses;
	volatile LONG AutoDiscoveryRunning;
	/* START_DEVICE can arrive while the single work item is already running.
	 * Preserve that edge so the current worker loops or queues another pass. */
	volatile LONG AutoDiscoveryRescanRequested;
	KEVENT AutoDiscoveryIdle;
	KEVENT AutoDiscoverySettledEvent;
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
	Cdp_DEVICE_KIND_DISK = 2
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
	volatile LONG Phase;
	// Auto discovery may run concurrently with PnP removal.  Started prevents
	// raw I/O before IRP_MN_START_DEVICE has completed; rundown keeps the lower
	// device attachment stable for the duration of a probe.
	volatile LONG Started;
	// Auto-discovery classification: 0=unknown, 1=source, 2=journal.
	volatile LONG AutoKind;
	BOOLEAN VolumeGuidValid;
	// Physical partition identity captured after START_DEVICE.  The complete
	// disk layout lets discovery wait for the physically adjacent successor
	// instead of delaying every volume for a global quiet period.
	BOOLEAN DiskLayoutValid;
	BOOLEAN HasNextPartition;
	ULONG DiskNumber;
	ULONG PartitionNumber;
	UINT64 PartitionStart;
	UINT64 PartitionSize;
	UINT64 NextPartitionStart;
	EX_RUNDOWN_REF AutoDiscoveryRundown;
	// Each newly started volume is held until automatic discovery determines
	// whether it is a recovery source.  This closes the source-identification
	// window before CaptureEnabled/Recovery Phase can be established.
	volatile LONG AutoDiscoveryGateActive;
	// A persisted reboot recovery keeps the discovery gate closed while the
	// driver synchronously attempts Recovery Begin (e) and Commit (r). Success
	// or failure ends the attempt; boot is allowed to continue in either case.
	volatile LONG RebootRecoveryGateRequired;
	KEVENT AutoDiscoveryGateEvent;
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
	KSPIN_LOCK RecoveryReadQueueLock;
	LIST_ENTRY RecoveryReadQueue;
	KEVENT RecoveryReadEvent;
	HANDLE RecoveryReadThreadHandle;
	volatile LONG RecoveryReadStopping;
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

typedef struct _Cdp_RECOVERY_READ_ITEM
{
	LIST_ENTRY Entry;
	PIRP Irp;
} Cdp_RECOVERY_READ_ITEM, *PCdp_RECOVERY_READ_ITEM;
