#include "../CdpConsole/cdp_driver_install.h"

#include <stdio.h>

static int Fail(const wchar_t* stage) {
    DWORD error = GetLastError();
    fwprintf(stderr, L"%s failed (Win32=%lu).\n", stage, error);
    return error ? static_cast<int>(error) : 1;
}

int wmain(int argc, wchar_t** argv) {
    if (argc != 2 || _wcsicmp(argv[1], L"--install") != 0) {
        fwprintf(stderr, L"Usage: CdpDriverInstallHelper.exe --install\n");
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
