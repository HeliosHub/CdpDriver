#pragma once

#include "CdpJournal.h"

NTSTATUS CdpCredentialCreate(
	_In_reads_bytes_(PasswordLength) const UCHAR* Password,
	_In_ ULONG PasswordLength,
	_Out_ PCdp_CREDENTIAL_DESCRIPTOR Credential);

NTSTATUS CdpCredentialDeriveVerifier(
	_In_reads_bytes_(PasswordLength) const UCHAR* Password,
	_In_ ULONG PasswordLength,
	_In_ const Cdp_CREDENTIAL_DESCRIPTOR* Credential,
	_Out_writes_bytes_(Cdp_CREDENTIAL_VERIFIER_BYTES) UCHAR* Verifier);

BOOLEAN CdpCredentialVerify(
	_In_reads_bytes_(PasswordLength) const UCHAR* Password,
	_In_ ULONG PasswordLength,
	_In_ const Cdp_CREDENTIAL_DESCRIPTOR* Credential);
