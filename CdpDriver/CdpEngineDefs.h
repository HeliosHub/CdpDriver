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

#define Cdp_DRIVER_VERSION_STRING "1.0.0"
#define Cdp_DRIVER_BUILD_STRING   "20260918.109-release"

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
	// Journal payload I/O target below the mounted volume filter. Metadata may
	// use a separately referenced volume-lower object during auto discovery.
	PDEVICE_OBJECT TargetLowerDevice;
	PDEVICE_OBJECT VolumeLowerDevice;
	// Referenced only by the auto-discovered journal. It keeps the volume-lower
	// object valid while RR/Header/Superblock I/O uses that stack.
	PDEVICE_OBJECT MetadataLowerDeviceReference;
	/* Physical identity used only for discovery and adjacency validation. */
	UINT64 PartitionStart;
	/* Offset understood by TargetLowerDevice: zero for a volume lower device,
	 * physical partition start for the raw-disk discovery backend. */
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
	// Per-boot gate set by the preview UI. It is never persisted.
	volatile LONG AutoDiscoveryDisabled;

	// 按时间点读取的文件预览会话
	LIST_ENTRY PreviewSessionList;
	FAST_MUTEX PreviewSessionMutex;
	// Serializes BEGIN/END replacement so a new BEGIN cannot free a session
	// while another BEGIN is still constructing and publishing its PreviewTree.
	KMUTEX PreviewOperationMutex;
	volatile LONGLONG PreviewSessionNextId;

	volatile LONG AuthFailureCount;
	volatile LONGLONG AuthBlockedUntil100ns;
	// Pending inverted-call IOCTLs from CdpBootService. The I/O manager cancel
	// spin lock serializes this list with each IRP's cancel routine.
	LIST_ENTRY RestoreSpaceAlertWaitList;
} Cdp_DRIVER_EXTENSION, *PCdp_DRIVER_EXTENSION;

NTSTATUS CdpPinMountedJournals(_In_ PCdp_DRIVER_EXTENSION DriverExt,
	_Outptr_result_buffer_(*JournalCount) PCdp_VOLUME_HANDLE_ENTRY** Journals,
	_Out_ PULONG JournalCount);
VOID CdpReleaseVolumeHandleEntry(_In_ PCdp_VOLUME_HANDLE_ENTRY Item);

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
	// Retained until END_PREVIEW after emergency compaction stopped its view.
	BOOLEAN StoppedByMerge;
	KEVENT NoReferences;
} Cdp_PREVIEW_SESSION, *PCdp_PREVIEW_SESSION;

typedef enum _Cdp_DEVICE_KIND
{
	Cdp_DEVICE_KIND_UNKNOWN = 0,
	Cdp_DEVICE_KIND_VOLUME = 1
} Cdp_DEVICE_KIND;

typedef struct _Cdp_DEVICE_EXTENSION
{
	Cdp_DEVICE_KIND DeviceKind;
	volatile LONG CaptureEnabled;
	// Set only after the complete volume/journal/Core object graph has passed
	// fail-closed activation validation.
	volatile LONG ProtectionStateValidated;
	// The protected Volume FIFO accepts new references only while this is set.
	// Disable clears it before waiting for outstanding requests to drain.
	volatile LONG VolumeIoAccepting;
	volatile LONG VolumeIoOutstanding;
	KEVENT VolumeIoDrainedEvent;
	/* Normal current-view reads pin immutable Journal payload locations while
	 * performing slow source/Journal I/O without HistoryMutex. Merge, drain and
	 * recovery transitions wait for this count to reach zero before changing or
	 * reclaiming the view. */
	volatile LONG CurrentViewReadsInFlight;
	KEVENT CurrentViewReadsDrainedEvent;
	/* Protected source objects use 0=normal, 1=publishing the terminal durable
	 * barrier, 2=terminal durable I/O. State 2 keeps capture admission open;
	 * every later redirected write is flushed before it completes. The state is
	 * deliberately lock-free: shutdown must not depend on a second dispatcher
	 * object that could have been invalidated during volume teardown. */
	volatile LONG ShutdownInProgress;
	/* Per-filter-hop accounting. Every logged shutdown entry must eventually
	 * increment completion after the lower stack finishes that same IRP. */
	volatile LONG64 ShutdownIrpEntryCount;
	volatile LONG64 ShutdownIrpCompletionCount;
	volatile LONG64 PowerIrpEntryCount;
	volatile LONG64 PowerIrpCompletionCount;
	volatile LONG Phase;
	// START_DEVICE publishes this before pre-mount discovery uses the lower
	// device stack.
	volatile LONG Started;
	/* Published only after IOCTL_VOLUME_ONLINE has completed successfully in
	 * the lower volume stack. Cleared after a successful OFFLINE transition. */
	volatile LONG VolumeOnline;
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
	/* Auto discovery can mount the Journal through a physical-disk handle
	 * before the adjacent Journal volume has received START_DEVICE. Protected
	 * source I/O remains in CaptureQueue until that volume-lower backend is
	 * published. */
	volatile LONG JournalBackendReady;
	KEVENT JournalBackendReadyEvent;
	/* Set by CaptureWorker only after the startup FIFO is empty.  Ordinary
	 * writes may then redirect in their dispatch path instead of joining it. */
	volatile LONG DirectRedirectReady;
	HANDLE CaptureThreadHandle;
	volatile LONG CaptureStopping;
	volatile LONG RedirectWritesInFlight;
	KEVENT RedirectWritesDrainedEvent;
	// First failure observed while graceful disable is writing/punching the
	// current MetaTree. Zero means the drain may continue.
	volatile LONG DrainFailureStatus;
	// Queryable graceful-disable progress. Byte fields are atomically published
	// and remain available after Core is destroyed until protection starts again.
	volatile LONG DrainProgressState;
	volatile LONG DrainProgressStatus;
	volatile LONG64 DrainProgressTotalBytes;
	volatile LONG64 DrainProgressCompletedBytes;
	HANDLE MergeThreadHandle;
	volatile LONG MergeThreadRunning;
	volatile LONG MergeThreadStopping;
	// Set only by an authenticated manual-merge request. The worker consumes it
	// to compact one normal oldest RR without the automatic 90% usage gate.
	// That Core pass still reclaims any branch-invalidated tombstone RRs.
	volatile LONG MergeIgnoreUsageThreshold;
	KEVENT MergeThreadDoneEvent;
	// Serializes the one write that releases HistoryMutex while waiting for a
	// running merge. Later writes wait here so they cannot overtake its retry.
	volatile LONG MergeSpaceRetryOwner;
	KEVENT MergeSpaceRetryDoneEvent;
	// Raised only for restore-point mode after the 80-percent automatic merge
	// proves that no RR can currently be reclaimed.
	volatile LONG RestorePointSpaceAlertActive;
	volatile LONG RestorePointSpaceAlertReason;
	volatile LONG RestorePointSpaceAlertStatus;
	volatile LONG64 RestorePointSpaceAlertGeneration;
	EX_PUSH_LOCK HistoryLock;
	// Preview reads share this gate. Preview teardown/automatic compaction takes
	// it exclusively, while ordinary protected writes intentionally do not.
	// This keeps slow history/source reads from blocking Journal appends.
	EX_PUSH_LOCK PreviewAccessLock;
	PCdp_CORE Core;
	// Journal VolumeHandleList entry used while CaptureEnabled is set.
	UINT64 JournalHandleId;
	// One journal-entry reference is retained for the protection session.
	// Dispatch reads this cached entry without serializing every I/O on
	// VolumeHandleMutex.
	PCdp_VOLUME_HANDLE_ENTRY RedirectJournalEntry;
	volatile LONG CaptureQueueDepth;
} Cdp_DEVICE_EXTENSION, *PCdp_DEVICE_EXTENSION;

typedef struct _Cdp_CAPTURE_ITEM
{
	LIST_ENTRY Entry;
	PIRP Irp;
	/* Source-volume-relative offset used by Core, MetaTree and Journal records. */
	UINT64 SourceVolumeOffset;
	/* Offset understood by OriginLowerReference; also volume-relative. */
	UINT64 OriginLowerOffset;
	PDEVICE_OBJECT SourceReference;
	PDEVICE_OBJECT OriginLowerReference;
	/* Captured before queueing so current-view read timing includes FIFO delay. */
} Cdp_CAPTURE_ITEM, *PCdp_CAPTURE_ITEM;
