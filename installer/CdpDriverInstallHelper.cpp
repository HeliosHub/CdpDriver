#include "../CdpConsole/cdp_driver_install.h"
#include "../CdpDriver/CdpIoctl.h"

#include <stdio.h>

static int Fail(const wchar_t* stage) {
    DWORD error = GetLastError();
    fwprintf(stderr, L"%s failed (Win32=%lu).\n", stage, error);
    return error ? static_cast<int>(error) : 1;
}

enum ProtectionCheckExitCode {
    ProtectionCheckNone = 0,
    ProtectionCheckActive = 10,
    ProtectionCheckUnavailable = 11,
};

static int CheckProtectionStatus() {
    HANDLE device = CreateFileW(
        Cdp_CONTROL_SYSTEM_LINK_NAME,
        GENERIC_READ | GENERIC_WRITE,
        0,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (device == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)
            return ProtectionCheckNone;
        fwprintf(stderr, L"Open control device failed (Win32=%lu).\n", error);
        return ProtectionCheckUnavailable;
    }

    BOOLEAN active = FALSE;
    DWORD bytesReturned = 0;
    const BOOL ok = DeviceIoControl(
        device,
        IOCTL_Cdp_QUERY_PROTECT_STATUS,
        nullptr,
        0,
        &active,
        sizeof(active),
        &bytesReturned,
        nullptr);
    const DWORD error = ok ? ERROR_SUCCESS : GetLastError();
    CloseHandle(device);

    if (!ok || bytesReturned < sizeof(active)) {
        fwprintf(stderr, L"Query protection status failed (Win32=%lu).\n", error);
        return ProtectionCheckUnavailable;
    }
    return active ? ProtectionCheckActive : ProtectionCheckNone;
}

int wmain(int argc, wchar_t** argv) {
    if (argc != 2) {
        fwprintf(stderr, L"Usage: CdpDriverInstallHelper.exe --install|--uninstall|--check-protection\n");
        return ERROR_INVALID_PARAMETER;
    }

    if (_wcsicmp(argv[1], L"--check-protection") == 0)
        return CheckProtectionStatus();

    if (_wcsicmp(argv[1], L"--uninstall") == 0) {
        if (!CdpUninstallDriverPackage())
            return Fail(CdpGetInstallFailureStage());
        wprintf(L"CdpDriver removal completed; reboot is required.\n");
        return 0;
    }

    if (_wcsicmp(argv[1], L"--install") != 0) {
        fwprintf(stderr, L"Usage: CdpDriverInstallHelper.exe --install|--uninstall|--check-protection\n");
        return ERROR_INVALID_PARAMETER;
    }

    wchar_t infPath[MAX_PATH] = {};
    if (!CdpResolveDriverInfPath(infPath, _countof(infPath)))
        return Fail(L"Resolve driver package");
    if (!CdpInstallDriverFromInf(infPath))
        return Fail(L"Install driver service");
    if (!CdpRegisterVolumeUpperFilter())
        return Fail(L"Register Volume UpperFilters");
    if (!CdpRegisterDiskUpperFilter())
        return Fail(L"Register DiskDrive UpperFilters");
    if (!CdpInstallBootConfirmService())
        return Fail(L"Install CdpBootService");

    wprintf(L"CdpDriver installation completed; reboot is required.\n");
    return 0;
}
