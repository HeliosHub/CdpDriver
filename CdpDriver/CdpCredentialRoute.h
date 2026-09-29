#pragma once

#include "CdpEngineDefs.h"

/* Reads the single credential mirrored by all mounted journals. */
NTSTATUS CdpCredentialGetShared(
	_In_ PCdp_DRIVER_EXTENSION DriverExt,
	_Out_ PCdp_CREDENTIAL_DESCRIPTOR Credential,
	_Out_opt_ PULONG JournalCount);

/* Handles the low-frequency credential IOCTLs outside the public dispatcher. */
BOOLEAN CdpCredentialRouteTryDispatchIoctl(
	_In_ PCdp_DRIVER_EXTENSION DriverExt,
	_In_ ULONG IoControlCode,
	_Inout_opt_ PVOID SystemBuffer,
	_In_ ULONG InputLength,
	_In_ ULONG OutputLength,
	_In_opt_ PFILE_OBJECT FileObject,
	_Out_ PNTSTATUS Status,
	_Out_ PULONG Information);
