#pragma once

#include "CdpEngineDefs.h"

typedef enum _Cdp_IOCTL_GUARD_CONTEXT_RESULT
{
    Cdp_IOCTL_GUARD_CONTEXT_READY = 0,
    Cdp_IOCTL_GUARD_CONTEXT_NOT_AUTHENTICATED,
    Cdp_IOCTL_GUARD_CONTEXT_EXPIRED
} Cdp_IOCTL_GUARD_CONTEXT_RESULT;

Cdp_IOCTL_GUARD_CONTEXT_RESULT CdpIoctlGuardValidateContext(
    _Inout_opt_ PCdp_CONTROL_FILE_CONTEXT Context,
    _In_ UINT64 Now100ns);

BOOLEAN CdpIoctlGuardCredentialMatchesAndRenew(
    _Inout_ PCdp_CONTROL_FILE_CONTEXT Context,
    _In_ const Cdp_CREDENTIAL_DESCRIPTOR* Credential,
    _In_ UINT64 Now100ns);

BOOLEAN CdpIoctlGuardGuidIsZero(_In_ const GUID* Guid);
BOOLEAN CdpIoctlGuardGuidIsEqual(_In_ const GUID* Left,
    _In_ const GUID* Right);
BOOLEAN CdpIoctlGuardAutoDiskIdentityMatches(
    _In_ PCdp_DEVICE_EXTENSION SourceExtension,
    _In_ PCdp_JOURNAL Journal);
BOOLEAN CdpIoctlGuardAutoPhysicalLayoutMatches(
    _In_ PCdp_DEVICE_EXTENSION SourceExtension,
    _In_ PCdp_JOURNAL Journal);
VOID CdpIoctlGuardApplyRestorePointTimeLowerBound(
    UINT64 RestorePointTime100ns, PUINT64 OldestTime100ns,
    PUINT64 NewestTime100ns);

