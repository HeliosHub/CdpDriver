#include "CdpIoctlGuard.h"

#include <ntdddisk.h>

#define Cdp_IOCTL_GUARD_LEASE_100NS (60ULL * 60ULL * 10000000ULL)

Cdp_IOCTL_GUARD_CONTEXT_RESULT CdpIoctlGuardValidateContext(
    _Inout_opt_ PCdp_CONTROL_FILE_CONTEXT Context,
    _In_ UINT64 Now100ns)
{
    if (!Context || !Context->Authenticated)
        return Cdp_IOCTL_GUARD_CONTEXT_NOT_AUTHENTICATED;

    if (Now100ns >= Context->ExpiresAt100ns)
    {
        RtlSecureZeroMemory(Context, sizeof(*Context));
        return Cdp_IOCTL_GUARD_CONTEXT_EXPIRED;
    }

    return Cdp_IOCTL_GUARD_CONTEXT_READY;
}

BOOLEAN CdpIoctlGuardCredentialMatchesAndRenew(
    _Inout_ PCdp_CONTROL_FILE_CONTEXT Context,
    _In_ const Cdp_CREDENTIAL_DESCRIPTOR* Credential,
    _In_ UINT64 Now100ns)
{
    if (CdpIoctlGuardValidateContext(Context, Now100ns) !=
        Cdp_IOCTL_GUARD_CONTEXT_READY ||
        !Credential)
    {
        return FALSE;
    }

    if (RtlCompareMemory(&Context->CredentialId, &Credential->CredentialId,
            sizeof(GUID)) != sizeof(GUID) ||
        Context->AuthEpoch != Credential->AuthEpoch)
    {
        return FALSE;
    }

    Context->ExpiresAt100ns = Now100ns + Cdp_IOCTL_GUARD_LEASE_100NS;
    return TRUE;
}

BOOLEAN CdpIoctlGuardGuidIsZero(_In_ const GUID* Guid)
{
    static const GUID zeroGuid = { 0 };

    return RtlCompareMemory(Guid, &zeroGuid, sizeof(GUID)) == sizeof(GUID);
}

BOOLEAN CdpIoctlGuardGuidIsEqual(_In_ const GUID* Left,
    _In_ const GUID* Right)
{
    return RtlCompareMemory(Left, Right, sizeof(GUID)) == sizeof(GUID);
}

BOOLEAN CdpIoctlGuardAutoDiskIdentityMatches(
    _In_ PCdp_DEVICE_EXTENSION SourceExtension,
    _In_ PCdp_JOURNAL Journal)
{
    if (Journal->DiskPartitionStyle != SourceExtension->DiskPartitionStyle)
        return FALSE;
    if (SourceExtension->DiskPartitionStyle == PARTITION_STYLE_GPT)
    {
        return !CdpIoctlGuardGuidIsZero(&Journal->DiskGuid) &&
            CdpIoctlGuardGuidIsEqual(&Journal->DiskGuid,
                &SourceExtension->DiskGuid);
    }
    if (SourceExtension->DiskPartitionStyle == PARTITION_STYLE_MBR)
    {
        return Journal->MbrSignature != 0 &&
            Journal->MbrSignature == SourceExtension->MbrSignature;
    }
    return FALSE;
}

BOOLEAN CdpIoctlGuardAutoPhysicalLayoutMatches(
    _In_ PCdp_DEVICE_EXTENSION SourceExtension,
    _In_ PCdp_JOURNAL Journal)
{
    if (!SourceExtension->DiskLayoutValid || !SourceExtension->HasNextPartition ||
        SourceExtension->PartitionSize == 0 ||
        SourceExtension->NextPartitionSize == 0)
    {
        return FALSE;
    }
    return CdpIoctlGuardAutoDiskIdentityMatches(SourceExtension, Journal) &&
        Journal->SourcePartitionStart == SourceExtension->PartitionStart &&
        Journal->SourcePartitionSize == SourceExtension->PartitionSize &&
        Journal->JournalPartitionStart == SourceExtension->NextPartitionStart &&
        Journal->JournalPartitionSize == SourceExtension->NextPartitionSize &&
        SourceExtension->PartitionStart <= MAXUINT64 -
            SourceExtension->PartitionSize &&
        SourceExtension->PartitionStart + SourceExtension->PartitionSize <=
            SourceExtension->NextPartitionStart;
}

VOID CdpIoctlGuardApplyRestorePointTimeLowerBound(
    UINT64 RestorePointTime100ns, PUINT64 OldestTime100ns,
    PUINT64 NewestTime100ns)
{
    if (RestorePointTime100ns == 0 || !OldestTime100ns || !NewestTime100ns)
        return;
    if (*OldestTime100ns == 0 || *OldestTime100ns < RestorePointTime100ns)
        *OldestTime100ns = RestorePointTime100ns;
    if (*NewestTime100ns == 0 || *NewestTime100ns < *OldestTime100ns)
        *NewestTime100ns = *OldestTime100ns;
}
