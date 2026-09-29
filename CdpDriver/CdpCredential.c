#include "CdpEngineDefs.h"
#include "CdpCredential.h"
#include <bcrypt.h>

#include "CdpLicenseSeg.h"

static BOOLEAN CdpConstantTimeEqual(
	_In_reads_bytes_(Length) const UCHAR* Left,
	_In_reads_bytes_(Length) const UCHAR* Right,
	_In_ ULONG Length)
{
	ULONG i;
	UCHAR difference = 0;
	for (i = 0; i < Length; ++i)
		difference |= Left[i] ^ Right[i];
	return difference == 0;
}

NTSTATUS CdpCredentialDeriveVerifier(
	_In_reads_bytes_(PasswordLength) const UCHAR* Password,
	_In_ ULONG PasswordLength,
	_In_ const Cdp_CREDENTIAL_DESCRIPTOR* Credential,
	_Out_writes_bytes_(Cdp_CREDENTIAL_VERIFIER_BYTES) UCHAR* Verifier)
{
	BCRYPT_ALG_HANDLE algorithm = NULL;
	NTSTATUS status;

	if (!Password || PasswordLength == 0 || !Credential || !Verifier ||
		Credential->KdfAlgorithm != Cdp_CREDENTIAL_KDF_PBKDF2_SHA256 ||
		Credential->KdfIterations == 0)
	{
		return STATUS_INVALID_PARAMETER;
	}
	status = BCryptOpenAlgorithmProvider(
		&algorithm, BCRYPT_SHA256_ALGORITHM, NULL, BCRYPT_ALG_HANDLE_HMAC_FLAG);
	if (!NT_SUCCESS(status))
		return status;
	status = BCryptDeriveKeyPBKDF2(
		algorithm,
		(PUCHAR)Password,
		PasswordLength,
		(PUCHAR)Credential->Salt,
		Cdp_CREDENTIAL_SALT_BYTES,
		Credential->KdfIterations,
		Verifier,
		Cdp_CREDENTIAL_VERIFIER_BYTES,
		0);
	BCryptCloseAlgorithmProvider(algorithm, 0);
	return status;
}

NTSTATUS CdpCredentialCreate(
	_In_reads_bytes_(PasswordLength) const UCHAR* Password,
	_In_ ULONG PasswordLength,
	_Out_ PCdp_CREDENTIAL_DESCRIPTOR Credential)
{
	NTSTATUS status;

	if (!Password || PasswordLength == 0 || !Credential)
		return STATUS_INVALID_PARAMETER;
	RtlZeroMemory(Credential, sizeof(*Credential));
	Credential->KdfAlgorithm = Cdp_CREDENTIAL_KDF_PBKDF2_SHA256;
	Credential->KdfIterations = Cdp_CREDENTIAL_DEFAULT_ITERATIONS;
	Credential->AuthEpoch = 1;
	status = BCryptGenRandom(NULL, (PUCHAR)&Credential->CredentialId,
		sizeof(Credential->CredentialId), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
	if (NT_SUCCESS(status))
	{
		status = BCryptGenRandom(NULL, Credential->Salt,
			Cdp_CREDENTIAL_SALT_BYTES, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
	}
	if (NT_SUCCESS(status))
	{
		status = CdpCredentialDeriveVerifier(
			Password, PasswordLength, Credential, Credential->Verifier);
	}
	if (!NT_SUCCESS(status))
		RtlSecureZeroMemory(Credential, sizeof(*Credential));
	return status;
}

BOOLEAN CdpCredentialVerify(
	_In_reads_bytes_(PasswordLength) const UCHAR* Password,
	_In_ ULONG PasswordLength,
	_In_ const Cdp_CREDENTIAL_DESCRIPTOR* Credential)
{
	UCHAR verifier[Cdp_CREDENTIAL_VERIFIER_BYTES];
	NTSTATUS status;
	BOOLEAN equal = FALSE;

	RtlZeroMemory(verifier, sizeof(verifier));
	status = CdpCredentialDeriveVerifier(
		Password, PasswordLength, Credential, verifier);
	if (NT_SUCCESS(status))
		equal = CdpConstantTimeEqual(
			verifier, Credential->Verifier, sizeof(verifier));
	RtlSecureZeroMemory(verifier, sizeof(verifier));
	return equal;
}

NTSTATUS CdpCredentialGetShared(
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

#include "CdpLicenseSegEnd.h"
