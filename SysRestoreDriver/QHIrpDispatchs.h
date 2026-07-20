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
#include "QHEngineDefs.h"

NTSTATUS QHIrpDispatchDefault(_In_ PDEVICE_OBJECT DeviceObject, _Inout_ PIRP Irp);

NTSTATUS QHIrpDispatchWrite(_In_ PDEVICE_OBJECT DeviceObject, _Inout_ PIRP Irp);

NTSTATUS QHIrpDispatchPnp(_In_ PDEVICE_OBJECT DeviceObject, _Inout_ PIRP Irp);

NTSTATUS QHIrpDispatchPower(_In_ PDEVICE_OBJECT DeviceObject, _Inout_ PIRP Irp);

NTSTATUS QHIrpDispatchDeviceControl(_In_ PDEVICE_OBJECT DeviceObject, _Inout_ PIRP Irp);

VOID QHBootReinitializationRoutine(
	_In_ PDRIVER_OBJECT DriverObject,
	_In_ PVOID Context,
	_In_ ULONG Count);

VOID QHDeleteFilterDevice(_In_ PDEVICE_OBJECT FilterDeviceObject);

VOID QHCloseAllVolumeHandles(_In_ PQH_DRIVER_EXTENSION DriverExt);

VOID QHCloseAllPreviewSessions(_In_ PQH_DRIVER_EXTENSION DriverExt);

NTSTATUS QHStartCaptureWorker(_Inout_ PQH_DEVICE_EXTENSION DevExt);

VOID QHStopCaptureWorker(_Inout_ PQH_DEVICE_EXTENSION DevExt);
