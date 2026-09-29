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
    ProtectionCheckCancelled = 12,
};

static HANDLE OpenControlDevice() {
    HANDLE device = CreateFileW(
        Cdp_CONTROL_SYSTEM_LINK_NAME,
        GENERIC_READ | GENERIC_WRITE,
        0,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    return device;
}

static BOOL GetSystemVolumeGuid(GUID* volumeGuid) {
    wchar_t windowsDirectory[MAX_PATH] = {};
    wchar_t mountPoint[MAX_PATH] = {};
    wchar_t volumeName[MAX_PATH] = {};
    wchar_t guidText[39] = {};
    const wchar_t* brace;

    if (!volumeGuid || !GetWindowsDirectoryW(windowsDirectory, _countof(windowsDirectory)) ||
        !GetVolumePathNameW(windowsDirectory, mountPoint, _countof(mountPoint)) ||
        !GetVolumeNameForVolumeMountPointW(mountPoint, volumeName, _countof(volumeName)))
        return FALSE;

    brace = wcschr(volumeName, L'{');
    if (!brace) {
        SetLastError(ERROR_INVALID_DATA);
        return FALSE;
    }
    wcsncpy_s(guidText, _countof(guidText), brace, 38);
    if (FAILED(CLSIDFromString(guidText, volumeGuid))) {
        SetLastError(ERROR_INVALID_DATA);
        return FALSE;
    }
    return TRUE;
}

static int CheckProtectionStatus() {
    BOOLEAN active = FALSE;
    DWORD bytesReturned = 0;
    HANDLE device = OpenControlDevice();

    if (device == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)
            return ProtectionCheckNone;
        fwprintf(stderr, L"Open control device failed (Win32=%lu).\n", error);
        return ProtectionCheckUnavailable;
    }

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

static int CheckSystemProtectionStatus() {
    GUID sourceVolumeGuid = {};
    Cdp_PHASE_QUERY_REQUEST request = {};
    Cdp_PHASE_QUERY_REPLY reply = {};
    DWORD bytesReturned = 0;
    HANDLE device;

    if (!GetSystemVolumeGuid(&sourceVolumeGuid)) {
        fwprintf(stderr, L"Resolve system volume failed (Win32=%lu).\n", GetLastError());
        return ProtectionCheckUnavailable;
    }

    device = OpenControlDevice();
    if (device == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)
            return ProtectionCheckNone;
        fwprintf(stderr, L"Open control device failed (Win32=%lu).\n", error);
        return ProtectionCheckUnavailable;
    }

    request.SourceVolumeGuid = sourceVolumeGuid;
    const BOOL ok = DeviceIoControl(
        device,
        IOCTL_Cdp_QUERY_PHASE,
        &request,
        sizeof(request),
        &reply,
        sizeof(reply),
        &bytesReturned,
        nullptr);
    const DWORD error = ok ? ERROR_SUCCESS : GetLastError();
    CloseHandle(device);

    if (!ok || bytesReturned < sizeof(reply)) {
        fwprintf(stderr, L"Query protection status failed (Win32=%lu).\n", error);
        return ProtectionCheckUnavailable;
    }
    return reply.Status >= 0 ? ProtectionCheckActive : ProtectionCheckNone;
}

struct PasswordPromptState {
    wchar_t* Password;
    size_t Capacity;
    HWND Edit;
    BOOL Accepted;
    HFONT TitleFont;
    HFONT BodyFont;
};

static LRESULT CALLBACK ProtectionPasswordPromptProc(
    HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    PasswordPromptState* state = reinterpret_cast<PasswordPromptState*>(
        GetWindowLongPtrW(window, GWLP_USERDATA));

    if (message == WM_NCCREATE) {
        const CREATESTRUCTW* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(window, GWLP_USERDATA,
            reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        return TRUE;
    }

    switch (message) {
    case WM_CREATE:
    {
        HWND title;
        HWND description;
        HWND label;
        HWND hint;
        HWND cancel;
        HWND confirm;
        state = reinterpret_cast<PasswordPromptState*>(
            GetWindowLongPtrW(window, GWLP_USERDATA));
        state->TitleFont = CreateFontW(
            -22, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
            DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        state->BodyFont = CreateFontW(
            -15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
            DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        title = CreateWindowW(L"STATIC", L"关闭系统盘保护",
            WS_CHILD | WS_VISIBLE, 24, 20, 400, 32, window, nullptr, nullptr, nullptr);
        description = CreateWindowW(L"STATIC",
            L"系统盘当前受到保护。请输入保护密码以继续安装。",
            WS_CHILD | WS_VISIBLE, 24, 58, 410, 22, window, nullptr, nullptr, nullptr);
        label = CreateWindowW(L"STATIC", L"保护密码",
            WS_CHILD | WS_VISIBLE, 24, 92, 160, 22, window, nullptr, nullptr, nullptr);
        state->Edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_PASSWORD | ES_AUTOHSCROLL,
            24, 116, 410, 28, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(1001)),
            nullptr, nullptr);
        hint = CreateWindowW(L"STATIC", L"密码仅用于本次验证，不会被保存。",
            WS_CHILD | WS_VISIBLE, 24, 151, 410, 20, window, nullptr, nullptr, nullptr);
        confirm = CreateWindowW(L"BUTTON", L"关闭保护并继续",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
            274, 188, 160, 32, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDOK)),
            nullptr, nullptr);
        cancel = CreateWindowW(L"BUTTON", L"取消安装",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP,
            174, 188, 92, 32, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDCANCEL)),
            nullptr, nullptr);
        SendMessageW(title, WM_SETFONT, reinterpret_cast<WPARAM>(state->TitleFont), TRUE);
        SendMessageW(description, WM_SETFONT, reinterpret_cast<WPARAM>(state->BodyFont), TRUE);
        SendMessageW(label, WM_SETFONT, reinterpret_cast<WPARAM>(state->BodyFont), TRUE);
        SendMessageW(state->Edit, WM_SETFONT, reinterpret_cast<WPARAM>(state->BodyFont), TRUE);
        SendMessageW(hint, WM_SETFONT, reinterpret_cast<WPARAM>(state->BodyFont), TRUE);
        SendMessageW(confirm, WM_SETFONT, reinterpret_cast<WPARAM>(state->BodyFont), TRUE);
        SendMessageW(cancel, WM_SETFONT, reinterpret_cast<WPARAM>(state->BodyFont), TRUE);
        SetFocus(state->Edit);
        return 0;
    }

    case WM_COMMAND:
        if (LOWORD(wParam) == IDOK && state) {
            if (GetWindowTextW(state->Edit, state->Password,
                    static_cast<int>(state->Capacity)) == 0) {
                MessageBoxW(window, L"保护密码不能为空。", L"关闭系统盘保护",
                    MB_OK | MB_ICONEXCLAMATION);
                SetFocus(state->Edit);
                return 0;
            }
            state->Accepted = TRUE;
            DestroyWindow(window);
            return 0;
        }
        if (LOWORD(wParam) == IDCANCEL) {
            DestroyWindow(window);
            return 0;
        }
        break;

    case WM_CLOSE:
        DestroyWindow(window);
        return 0;

    case WM_CTLCOLORSTATIC:
        SetBkMode(reinterpret_cast<HDC>(wParam), TRANSPARENT);
        SetTextColor(reinterpret_cast<HDC>(wParam), RGB(45, 45, 45));
        return reinterpret_cast<LRESULT>(GetStockObject(WHITE_BRUSH));

    case WM_DESTROY:
        if (state) {
            if (state->TitleFont) {
                DeleteObject(state->TitleFont);
                state->TitleFont = nullptr;
            }
            if (state->BodyFont) {
                DeleteObject(state->BodyFont);
                state->BodyFont = nullptr;
            }
        }
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

static BOOL PromptForProtectionPassword(wchar_t* password, size_t capacity) {
    static const wchar_t className[] = L"CdpDriverProtectionPasswordPrompt";
    static ATOM windowClass = 0;
    WNDCLASSW windowClassInfo = {};
    PasswordPromptState state = { password, capacity, nullptr, FALSE, nullptr, nullptr };
    MSG message;
    HWND window;
    int x;
    int y;

    if (!password || capacity < 2) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    password[0] = L'\0';
    if (!windowClass) {
        windowClassInfo.lpfnWndProc = ProtectionPasswordPromptProc;
        windowClassInfo.hInstance = GetModuleHandleW(nullptr);
        windowClassInfo.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        windowClassInfo.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        windowClassInfo.lpszClassName = className;
        windowClass = RegisterClassW(&windowClassInfo);
        if (!windowClass && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
            return FALSE;
    }
    x = (GetSystemMetrics(SM_CXSCREEN) - 460) / 2;
    y = (GetSystemMetrics(SM_CYSCREEN) - 275) / 2;
    window = CreateWindowExW(WS_EX_DLGMODALFRAME, className, L"关闭系统盘保护",
        WS_CAPTION | WS_SYSMENU | WS_POPUP | WS_VISIBLE,
        x, y, 460, 275, nullptr, nullptr, GetModuleHandleW(nullptr), &state);
    if (!window)
        return FALSE;
    while (IsWindow(window) && GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    if (!state.Accepted)
        SetLastError(ERROR_CANCELLED);
    return state.Accepted;
}

static BOOL AuthenticateWithPrompt(HANDLE device) {
    wchar_t password[Cdp_PASSWORD_MAX_UTF8_BYTES + 1] = {};
    Cdp_AUTH_REQUEST request = {};
    DWORD bytesReturned = 0;
    int passwordBytes;
    BOOL ok = FALSE;

    if (!PromptForProtectionPassword(password, _countof(password)))
        goto cleanup;

    passwordBytes = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, password, -1,
        reinterpret_cast<char*>(request.Password),
        Cdp_PASSWORD_MAX_UTF8_BYTES, nullptr, nullptr);
    if (passwordBytes <= 1) {
        SetLastError(ERROR_INVALID_PASSWORD);
        goto cleanup;
    }
    request.PasswordLength = static_cast<ULONG>(passwordBytes - 1);
    ok = DeviceIoControl(device, IOCTL_Cdp_AUTHENTICATE,
        &request, sizeof(request), nullptr, 0, &bytesReturned, nullptr);

cleanup:
    SecureZeroMemory(&request, sizeof(request));
    SecureZeroMemory(password, sizeof(password));
    return ok;
}

static BOOL ReclaimSystemJournal(const Cdp_PHASE_QUERY_REPLY& phase, wchar_t systemDrive) {
    wchar_t volumePath[] = L"\\\\.\\C:";
    VOLUME_DISK_EXTENTS extents = {};
    DWORD bytesReturned = 0;
    HANDLE volume;
    wchar_t temporaryDirectory[MAX_PATH] = {};
    wchar_t scriptPath[MAX_PATH] = {};
    char script[256] = {};
    DWORD written = 0;
    HANDLE scriptFile;
    STARTUPINFOW startupInfo = { sizeof(startupInfo) };
    PROCESS_INFORMATION processInfo = {};
    wchar_t commandLine[MAX_PATH + 64] = {};
    DWORD exitCode = ERROR_GEN_FAILURE;

    if (systemDrive < L'A' || systemDrive > L'Z' ||
        phase.JournalPartitionNumber == 0 || phase.JournalPartitionBytes == 0) {
        SetLastError(ERROR_INVALID_DATA);
        return FALSE;
    }
    volumePath[4] = systemDrive;
    volume = CreateFileW(volumePath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING, 0, nullptr);
    if (volume == INVALID_HANDLE_VALUE)
        return FALSE;
    const BOOL queried = DeviceIoControl(volume, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS,
        nullptr, 0, &extents, sizeof(extents), &bytesReturned, nullptr);
    CloseHandle(volume);
    if (!queried || bytesReturned < sizeof(extents) || extents.NumberOfDiskExtents != 1 ||
        extents.Extents[0].DiskNumber != phase.JournalDiskNumber ||
        static_cast<UINT64>(extents.Extents[0].StartingOffset.QuadPart) +
            static_cast<UINT64>(extents.Extents[0].ExtentLength.QuadPart) !=
            phase.JournalPartitionOffset) {
        SetLastError(ERROR_NOT_SUPPORTED);
        return FALSE;
    }

    if (!GetTempPathW(_countof(temporaryDirectory), temporaryDirectory) ||
        !GetTempFileNameW(temporaryDirectory, L"cdp", 0, scriptPath))
        return FALSE;
    const int scriptLength = _snprintf_s(script, sizeof(script), _TRUNCATE,
        "select disk %lu\r\nselect partition %lu\r\ndelete partition override\r\n"
        "select volume %c\r\nextend\r\nexit\r\n",
        phase.JournalDiskNumber, phase.JournalPartitionNumber, static_cast<char>(systemDrive));
    if (scriptLength <= 0) {
        DeleteFileW(scriptPath);
        SetLastError(ERROR_INVALID_DATA);
        return FALSE;
    }
    scriptFile = CreateFileW(scriptPath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
        FILE_ATTRIBUTE_TEMPORARY, nullptr);
    if (scriptFile == INVALID_HANDLE_VALUE) {
        DeleteFileW(scriptPath);
        return FALSE;
    }
    const BOOL writeOk = WriteFile(scriptFile, script, static_cast<DWORD>(scriptLength), &written, nullptr);
    CloseHandle(scriptFile);
    if (!writeOk || written != static_cast<DWORD>(scriptLength)) {
        DeleteFileW(scriptPath);
        SetLastError(ERROR_WRITE_FAULT);
        return FALSE;
    }
    _snwprintf_s(commandLine, _countof(commandLine), _TRUNCATE,
        L"diskpart.exe /s \"%s\"", scriptPath);
    startupInfo.dwFlags = STARTF_USESHOWWINDOW;
    startupInfo.wShowWindow = SW_HIDE;
    const BOOL started = CreateProcessW(nullptr, commandLine, nullptr, nullptr, FALSE,
        CREATE_NO_WINDOW, nullptr, nullptr, &startupInfo, &processInfo);
    if (started) {
        WaitForSingleObject(processInfo.hProcess, INFINITE);
        GetExitCodeProcess(processInfo.hProcess, &exitCode);
        CloseHandle(processInfo.hThread);
        CloseHandle(processInfo.hProcess);
    }
    DeleteFileW(scriptPath);
    if (!started || exitCode != 0) {
        if (started) SetLastError(exitCode);
        return FALSE;
    }
    return TRUE;
}

static int StopSystemProtection() {
    GUID sourceVolumeGuid = {};
    Cdp_CMD2_REQUEST request = {};
    Cdp_COMMAND_REPLY reply = {};
    DWORD bytesReturned = 0;
    HANDLE device;
    Cdp_PHASE_QUERY_REQUEST phaseRequest = {};
    Cdp_PHASE_QUERY_REPLY phaseReply = {};
    wchar_t windowsDirectory[MAX_PATH] = {};
    wchar_t systemMountPoint[MAX_PATH] = {};
    const int state = CheckSystemProtectionStatus();

    if (state != ProtectionCheckActive)
        return state;
    if (!GetSystemVolumeGuid(&sourceVolumeGuid))
        return ProtectionCheckUnavailable;
    device = OpenControlDevice();
    if (device == INVALID_HANDLE_VALUE)
        return ProtectionCheckUnavailable;
    phaseRequest.SourceVolumeGuid = sourceVolumeGuid;
    if (!DeviceIoControl(device, IOCTL_Cdp_QUERY_PHASE, &phaseRequest, sizeof(phaseRequest),
            &phaseReply, sizeof(phaseReply), &bytesReturned, nullptr) ||
        bytesReturned < sizeof(phaseReply)) {
        CloseHandle(device);
        return ProtectionCheckUnavailable;
    }
    if (!AuthenticateWithPrompt(device)) {
        const DWORD error = GetLastError();
        CloseHandle(device);
        return error == ERROR_CANCELLED ? ProtectionCheckCancelled : ProtectionCheckUnavailable;
    }

    request.Code = Cdp_CMD_2;
    request.SourceVolumeGuid = sourceVolumeGuid;
    const BOOL ok = DeviceIoControl(device, IOCTL_Cdp_SEND_COMMAND,
        &request, sizeof(request), &reply, sizeof(reply), &bytesReturned, nullptr);
    const DWORD error = ok ? ERROR_SUCCESS : GetLastError();
    SecureZeroMemory(&request, sizeof(request));
    CloseHandle(device);
    if (!ok || bytesReturned < sizeof(reply) || reply.Result != 0) {
        SetLastError(error ? error : ERROR_GEN_FAILURE);
        return ProtectionCheckUnavailable;
    }
    if (CheckSystemProtectionStatus() != ProtectionCheckNone ||
        !GetWindowsDirectoryW(windowsDirectory, _countof(windowsDirectory)) ||
        !GetVolumePathNameW(windowsDirectory, systemMountPoint, _countof(systemMountPoint)) ||
        !ReclaimSystemJournal(phaseReply, systemMountPoint[0]))
        return ProtectionCheckUnavailable;
    return ProtectionCheckNone;
}

int wmain(int argc, wchar_t** argv) {
    if (argc != 2) {
        fwprintf(stderr, L"Usage: CdpDriverInstallHelper.exe --install|--uninstall|--check-protection|--check-system-protection|--stop-system-protection\n");
        return ERROR_INVALID_PARAMETER;
    }

    if (_wcsicmp(argv[1], L"--check-protection") == 0)
        return CheckProtectionStatus();

    if (_wcsicmp(argv[1], L"--check-system-protection") == 0)
        return CheckSystemProtectionStatus();

    if (_wcsicmp(argv[1], L"--stop-system-protection") == 0)
        return StopSystemProtection();

    if (_wcsicmp(argv[1], L"--uninstall") == 0) {
        if (!CdpUninstallDriverPackage())
            return Fail(CdpGetInstallFailureStage());
        wprintf(L"CdpDriver removal completed; reboot is required.\n");
        return 0;
    }

    if (_wcsicmp(argv[1], L"--install") != 0) {
        fwprintf(stderr, L"Usage: CdpDriverInstallHelper.exe --install|--uninstall|--check-protection|--check-system-protection|--stop-system-protection\n");
        return ERROR_INVALID_PARAMETER;
    }

    wchar_t infPath[MAX_PATH] = {};
    if (!CdpResolveDriverInfPath(infPath, _countof(infPath)))
        return Fail(L"Resolve driver package");
    if (!CdpInstallDriverFromInf(infPath))
        return Fail(L"Install driver service");
    if (!CdpRegisterVolumeUpperFilter())
        return Fail(L"Register Volume UpperFilters");
    if (!CdpRemoveLegacyDiskUpperFilter())
        return Fail(L"Remove legacy DiskDrive UpperFilters");
    if (!CdpInstallBootConfirmService())
        return Fail(L"Install CdpBootService");

    wprintf(L"CdpDriver installation completed; reboot is required.\n");
    return 0;
}
