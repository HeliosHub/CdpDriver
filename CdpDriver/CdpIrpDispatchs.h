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

NTSTATUS CdpIrpDispatchRead(_In_ PDEVICE_OBJECT DeviceObject, _Inout_ PIRP Irp);

NTSTATUS CdpIrpDispatchWrite(_In_ PDEVICE_OBJECT DeviceObject, _Inout_ PIRP Irp);

NTSTATUS CdpIrpDispatchPnp(_In_ PDEVICE_OBJECT DeviceObject, _Inout_ PIRP Irp);

NTSTATUS CdpIrpDispatchPower(_In_ PDEVICE_OBJECT DeviceObject, _Inout_ PIRP Irp);

NTSTATUS CdpIrpDispatchDeviceControl(_In_ PDEVICE_OBJECT DeviceObject, _Inout_ PIRP Irp);

VOID CdpDeleteFilterDevice(_In_ PDEVICE_OBJECT FilterDeviceObject);

VOID CdpCloseAllVolumeHandles(_In_ PCdp_DRIVER_EXTENSION DriverExt);

VOID CdpCloseAllPreviewSessions(_In_ PCdp_DRIVER_EXTENSION DriverExt);

VOID CdpInitializeAutoDiscovery(_Inout_ PCdp_DRIVER_EXTENSION DriverExt);

// Queue an immediate scan (safe at DISPATCH_LEVEL). Prefer calling from
// IRP_MN_START_DEVICE so CDP arms before volume traffic.
VOID CdpQueueAutoDiscovery(_Inout_ PCdp_DRIVER_EXTENSION DriverExt);

VOID CdpScheduleAutoDiscovery(_Inout_ PCdp_DRIVER_EXTENSION DriverExt);

VOID CdpStopAutoDiscovery(_Inout_ PCdp_DRIVER_EXTENSION DriverExt);

NTSTATUS CdpStartCaptureWorker(_Inout_ PCdp_DEVICE_EXTENSION DevExt);

VOID CdpStopCaptureWorker(_Inout_ PCdp_DEVICE_EXTENSION DevExt);

NTSTATUS CdpStartRecoveryReadWorker(_Inout_ PCdp_DEVICE_EXTENSION DevExt);

VOID CdpStopRecoveryReadWorker(_Inout_ PCdp_DEVICE_EXTENSION DevExt);

// Disable capture, wait for its worker to leave Core, then release Core.
// This routine must be used before a filter device is removed or reconfigured.
VOID CdpDisableAndDestroyCapture(_Inout_ PCdp_DEVICE_EXTENSION DevExt);
