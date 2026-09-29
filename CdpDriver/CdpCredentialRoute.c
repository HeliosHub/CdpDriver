#include "CdpCredentialRoute.h"
#include "CdpCredential.h"

#include "CdpLicenseSeg.h"

BOOLEAN CdpCredentialRouteTryDispatchIoctl(
	_In_ PCdp_DRIVER_EXTENSION DriverExt,
	_In_ ULONG IoControlCode,
	_Inout_opt_ PVOID SystemBuffer,
	_In_ ULONG InputLength,
	_In_ ULONG OutputLength,
	_In_opt_ PFILE_OBJECT FileObject,
	_Out_ PNTSTATUS Status,
	_Out_ PULONG Information)
{
	if (!Status || !Information)
		return FALSE;

	*Information = 0;
	switch (IoControlCode)
	{
	case IOCTL_Cdp_QUERY_CREDENTIAL:
	{
		PCdp_CREDENTIAL_STATUS_REPLY reply;
		Cdp_CREDENTIAL_DESCRIPTOR credential;
		ULONG count = 0;
		if (!SystemBuffer || OutputLength < sizeof(*reply))
		{
			*Status = STATUS_BUFFER_TOO_SMALL;
			return TRUE;
		}
		reply = (PCdp_CREDENTIAL_STATUS_REPLY)SystemBuffer;
		RtlZeroMemory(reply, sizeof(*reply));
		*Status = CdpCredentialGetShared(DriverExt, &credential, &count);
		if (*Status == STATUS_NOT_FOUND)
		{
			*Status = STATUS_SUCCESS;
			*Information = sizeof(*reply);
			return TRUE;
		}
		if (!NT_SUCCESS(*Status))
			return TRUE;
		reply->Configured = 1;
		reply->JournalCount = count;
		reply->CredentialId = credential.CredentialId;
		reply->AuthEpoch = credential.AuthEpoch;
		*Status = STATUS_SUCCESS;
		*Information = sizeof(*reply);
		return TRUE;
	}

	case IOCTL_Cdp_AUTHENTICATE:
	{
		Cdp_AUTH_REQUEST request;
		Cdp_CREDENTIAL_DESCRIPTOR credential;
		PCdp_CONTROL_FILE_CONTEXT context;
		UINT64 now = KeQueryInterruptTime();
		if (!FileObject || !SystemBuffer || InputLength < sizeof(request))
		{
			*Status = STATUS_BUFFER_TOO_SMALL;
			return TRUE;
		}
		context = (PCdp_CONTROL_FILE_CONTEXT)FileObject->FsContext;
		if (!context)
		{
			*Status = STATUS_BUFFER_TOO_SMALL;
			return TRUE;
		}
		request = *(PCdp_AUTH_REQUEST)SystemBuffer;
		RtlSecureZeroMemory(SystemBuffer, sizeof(request));
		if (request.PasswordLength == 0 ||
			request.PasswordLength > Cdp_PASSWORD_MAX_UTF8_BYTES)
		{
			RtlSecureZeroMemory(&request, sizeof(request));
			*Status = STATUS_INVALID_PARAMETER;
			return TRUE;
		}
		if ((UINT64)InterlockedCompareExchange64(
			&DriverExt->AuthBlockedUntil100ns, 0, 0) > now)
		{
			RtlSecureZeroMemory(&request, sizeof(request));
			*Status = STATUS_ACCOUNT_LOCKED_OUT;
			return TRUE;
		}
		*Status = CdpCredentialGetShared(DriverExt, &credential, NULL);
		if (NT_SUCCESS(*Status) &&
			!CdpCredentialVerify(request.Password, request.PasswordLength, &credential))
		{
			*Status = STATUS_ACCESS_DENIED;
		}
		RtlSecureZeroMemory(&request, sizeof(request));
		if (!NT_SUCCESS(*Status))
		{
			if (*Status == STATUS_ACCESS_DENIED &&
				InterlockedIncrement(&DriverExt->AuthFailureCount) >= 5)
			{
				InterlockedExchange(&DriverExt->AuthFailureCount, 0);
				InterlockedExchange64(&DriverExt->AuthBlockedUntil100ns,
					(LONGLONG)(now + 60ULL * 60ULL * 10000000ULL));
				*Status = STATUS_ACCOUNT_LOCKED_OUT;
			}
			RtlSecureZeroMemory(context, sizeof(*context));
			return TRUE;
		}
		context->Authenticated = TRUE;
		InterlockedExchange(&DriverExt->AuthFailureCount, 0);
		InterlockedExchange64(&DriverExt->AuthBlockedUntil100ns, 0);
		context->CredentialId = credential.CredentialId;
		context->AuthEpoch = credential.AuthEpoch;
		context->ExpiresAt100ns = KeQueryInterruptTime() +
			60ULL * 60ULL * 10000000ULL;
		*Status = STATUS_SUCCESS;
		return TRUE;
	}

	default:
		return FALSE;
	}
}

#include "CdpLicenseSegEnd.h"
