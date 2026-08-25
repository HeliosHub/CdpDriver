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
#include "CdpEngineDefs.h"

NTSTATUS CdpIrpDispatchDefault(_In_ PDEVICE_OBJECT DeviceObject, _Inout_ PIRP Irp);

NTSTATUS CdpIrpDispatchCreateClose(_In_ PDEVICE_OBJECT DeviceObject, _Inout_ PIRP Irp);

NTSTATUS CdpIrpDispatchRead(_In_ PDEVICE_OBJECT DeviceObject, _Inout_ PIRP Irp);

NTSTATUS CdpIrpDispatchWrite(_In_ PDEVICE_OBJECT DeviceObject, _Inout_ PIRP Irp);
NTSTATUS CdpIrpDispatchFlush(_In_ PDEVICE_OBJECT DeviceObject, _Inout_ PIRP Irp);
NTSTATUS CdpIrpDispatchShutdown(_In_ PDEVICE_OBJECT DeviceObject, _Inout_ PIRP Irp);

NTSTATUS CdpIrpDispatchPnp(_In_ PDEVICE_OBJECT DeviceObject, _Inout_ PIRP Irp);

NTSTATUS CdpIrpDispatchPower(_In_ PDEVICE_OBJECT DeviceObject, _Inout_ PIRP Irp);

NTSTATUS CdpIrpDispatchDeviceControl(_In_ PDEVICE_OBJECT DeviceObject, _Inout_ PIRP Irp);

VOID CdpDeleteFilterDevice(_In_ PDEVICE_OBJECT FilterDeviceObject);

NTSTATUS CdpCreateInternalSourceDevice(
	_In_ PCdp_DRIVER_EXTENSION DriverExt,
	_In_ PCdp_DEVICE_EXTENSION DiskExt,
	_Out_ PDEVICE_OBJECT* SourceDeviceObject,
	_Out_ PCdp_DEVICE_EXTENSION* SourceExt);

VOID CdpDeleteInternalSourceDevice(
	_In_ PCdp_DRIVER_EXTENSION DriverExt,
	_In_ PDEVICE_OBJECT SourceDeviceObject);

VOID CdpDeleteInternalSourceDevicesForDisk(
	_In_ PCdp_DRIVER_EXTENSION DriverExt,
	_In_ ULONG DiskNumber);
NTSTATUS CdpBindVolumeProtectionContext(
	_Inout_ PCdp_DEVICE_EXTENSION VolumeExt,
	_In_ PDEVICE_OBJECT SourceDeviceObject);
VOID CdpUnbindVolumeProtectionContext(
	_Inout_ PCdp_DEVICE_EXTENSION VolumeExt);
VOID CdpUnbindVolumesFromSource(
	_In_ PCdp_DRIVER_EXTENSION DriverExt,
	_In_ PDEVICE_OBJECT SourceDeviceObject);

VOID CdpCloseAllVolumeHandles(_In_ PCdp_DRIVER_EXTENSION DriverExt);

VOID CdpCloseAllPreviewSessions(_In_ PCdp_DRIVER_EXTENSION DriverExt);

NTSTATUS CdpStartCaptureWorker(_Inout_ PCdp_DEVICE_EXTENSION DevExt);

VOID CdpStopCaptureWorker(_Inout_ PCdp_DEVICE_EXTENSION DevExt);

VOID CdpDestroyDiskProtectionIndex(_Inout_ PCdp_DEVICE_EXTENSION DiskExt);

// Disable capture, wait for its worker to leave Core, then release Core.
// This routine must be used before a filter device is removed or reconfigured.
VOID CdpDisableAndDestroyCapture(_Inout_ PCdp_DEVICE_EXTENSION DevExt);
