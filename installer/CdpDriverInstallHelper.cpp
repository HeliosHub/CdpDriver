#include "../CdpConsole/cdp_driver_install.h"
#include "../CdpDriver/CdpIoctl.h"

#include <commctrl.h>
#include <stdio.h>

#pragma comment(lib, "comctl32.lib")

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
    ProtectionCheckActiveWithRestorePoint = 13,
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
    Cdp_RESTORE_POINT_QUERY_REQUEST restorePointRequest = {};
    Cdp_RESTORE_POINT_QUERY_REPLY restorePointReply = {};
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

    if (!ok || bytesReturned < sizeof(reply)) {
        CloseHandle(device);
        fwprintf(stderr, L"Query protection status failed (Win32=%lu).\n", error);
        return ProtectionCheckUnavailable;
    }
    if (reply.Status < 0) {
        CloseHandle(device);
        return ProtectionCheckNone;
    }

    restorePointRequest.SourceVolumeGuid = sourceVolumeGuid;
    const BOOL restorePointOk = DeviceIoControl(
        device,
        IOCTL_Cdp_QUERY_RESTORE_POINT,
        &restorePointRequest,
        sizeof(restorePointRequest),
        &restorePointReply,
        sizeof(restorePointReply),
        &bytesReturned,
        nullptr);
    const DWORD restorePointError = restorePointOk ? ERROR_SUCCESS : GetLastError();
    CloseHandle(device);
    if (!restorePointOk || bytesReturned < sizeof(restorePointReply)) {
        fwprintf(stderr, L"Query system restore point failed (Win32=%lu).\n", restorePointError);
        return ProtectionCheckActive;
    }
    return restorePointReply.IsSet ? ProtectionCheckActiveWithRestorePoint : ProtectionCheckActive;
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

/* The graceful-stop IOCTL does the writeback synchronously.  Keep it on a
 * worker thread so the installer can query the driver's published drain
 * counters while it is running. */
struct StopProgressState {
    HANDLE CommandDevice;
    HANDLE QueryDevice;
    GUID SourceVolumeGuid;
    HWND Window;
    HWND Detail;
    HWND Bar;
    HWND Percent;
    HANDLE Worker;
    DWORD Result;
    HFONT BodyFont;
};

static const UINT CdpStopProgressTimer = 1;
static const UINT CdpStopProgressCloseTimer = 2;
static const UINT CdpStopProgressCompleteMessage = WM_APP + 42;

static void LayoutStopProgress(HWND window, StopProgressState* state) {
    RECT client = {};
    const int contentHeight = 78;
    int top;
    int width;

    if (!state || !state->Detail || !state->Bar || !state->Percent)
        return;
    GetClientRect(window, &client);
    top = (client.bottom - client.top - contentHeight) / 2;
    if (top < 12)
        top = 12;
    width = client.right - client.left - 44;
    SetWindowPos(state->Detail, nullptr, 22, top, width, 22,
        SWP_NOZORDER | SWP_NOACTIVATE);
    SetWindowPos(state->Bar, nullptr, 22, top + 30, width, 14,
        SWP_NOZORDER | SWP_NOACTIVATE);
    SetWindowPos(state->Percent, nullptr, 22, top + 56, width, 22,
        SWP_NOZORDER | SWP_NOACTIVATE);
}

static void UpdateStopProgress(StopProgressState* state) {
    Cdp_DRAIN_PROGRESS_QUERY_REQUEST request = {};
    Cdp_DRAIN_PROGRESS_QUERY_REPLY reply = {};
    DWORD bytesReturned = 0;
    wchar_t text[192] = {};

    if (!state || !state->Detail)
        return;
    request.SourceVolumeGuid = state->SourceVolumeGuid;
    if (state->QueryDevice == INVALID_HANDLE_VALUE || !DeviceIoControl(
            state->QueryDevice, IOCTL_Cdp_QUERY_DRAIN_PROGRESS,
            &request, sizeof(request), &reply, sizeof(reply),
            &bytesReturned, nullptr) || bytesReturned < sizeof(reply)) {
        SetWindowTextW(state->Detail, L"正在准备回填数据，请稍候…");
        SetWindowTextW(state->Percent, L"准备中");
        return;
    }

    if (reply.TotalBytes != 0) {
        const UINT64 completed = reply.CompletedBytes > reply.TotalBytes ?
            reply.TotalBytes : reply.CompletedBytes;
        const ULONG percent = static_cast<ULONG>(
            (completed * 100ULL) / reply.TotalBytes);
        _snwprintf_s(text, _countof(text), _TRUNCATE,
            L"%lu%%  ·  %llu / %llu MiB",
            percent,
            completed / (1024ULL * 1024ULL),
            reply.TotalBytes / (1024ULL * 1024ULL));
        SetWindowTextW(state->Percent, text);
        SendMessageW(state->Bar, PBM_SETPOS, percent, 0);
    } else if (reply.State == Cdp_DRAIN_PROGRESS_COMPLETED) {
        SetWindowTextW(state->Detail, L"回填完成，正在继续安装…");
        SetWindowTextW(state->Percent, L"100%");
        SendMessageW(state->Bar, PBM_SETPOS, 100, 0);
    } else {
        SetWindowTextW(state->Detail, L"正在准备回填数据，请稍候…");
        SetWindowTextW(state->Percent, L"准备中");
    }
}

static DWORD WINAPI StopProtectionWorker(void* context) {
    StopProgressState* state = static_cast<StopProgressState*>(context);
    Cdp_CMD2_REQUEST request = {};
    Cdp_COMMAND_REPLY reply = {};
    DWORD bytesReturned = 0;

    request.Code = Cdp_CMD_2;
    request.SourceVolumeGuid = state->SourceVolumeGuid;
    if (!DeviceIoControl(state->CommandDevice, IOCTL_Cdp_SEND_COMMAND,
            &request, sizeof(request), &reply, sizeof(reply),
            &bytesReturned, nullptr) || bytesReturned < sizeof(reply) ||
        reply.Result != 0) {
        const DWORD error = GetLastError();
        state->Result = error ? error : ERROR_GEN_FAILURE;
    } else {
        state->Result = ERROR_SUCCESS;
    }
    SecureZeroMemory(&request, sizeof(request));
    PostMessageW(state->Window, CdpStopProgressCompleteMessage,
        0, reinterpret_cast<LPARAM>(state));
    return 0;
}

static LRESULT CALLBACK StopProgressWindowProc(
    HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    StopProgressState* state = reinterpret_cast<StopProgressState*>(
        GetWindowLongPtrW(window, GWLP_USERDATA));

    if (message == WM_NCCREATE) {
        const CREATESTRUCTW* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        state = static_cast<StopProgressState*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
        return TRUE;
    }
    switch (message) {
    case WM_CREATE:
    {
        state->Window = window;
        state->BodyFont = CreateFontW(-15, 0, 0, 0, FW_NORMAL,
            FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
            CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
            L"Segoe UI");
        state->Detail = CreateWindowW(L"STATIC", L"正在回填系统盘数据…",
            WS_CHILD | WS_VISIBLE, 22, 20, 360, 22, window, nullptr, nullptr, nullptr);
        state->Bar = CreateWindowExW(0, PROGRESS_CLASSW, L"",
            WS_CHILD | WS_VISIBLE | PBS_SMOOTH, 22, 50, 376, 14,
            window, nullptr, nullptr, nullptr);
        state->Percent = CreateWindowW(L"STATIC", L"准备中",
            WS_CHILD | WS_VISIBLE, 22, 76, 376, 22, window, nullptr, nullptr, nullptr);
        SendMessageW(state->Detail, WM_SETFONT, reinterpret_cast<WPARAM>(state->BodyFont), TRUE);
        SendMessageW(state->Percent, WM_SETFONT, reinterpret_cast<WPARAM>(state->BodyFont), TRUE);
        SendMessageW(state->Bar, PBM_SETRANGE32, 0, 100);
        LayoutStopProgress(window, state);
        return 0;
    }

    case WM_SIZE:
        LayoutStopProgress(window, state);
        return 0;

    case WM_CTLCOLORSTATIC:
        SetBkMode(reinterpret_cast<HDC>(wParam), TRANSPARENT);
        SetTextColor(reinterpret_cast<HDC>(wParam), RGB(70, 70, 70));
        return reinterpret_cast<LRESULT>(GetStockObject(WHITE_BRUSH));

    case WM_TIMER:
        if (wParam == CdpStopProgressTimer) {
            UpdateStopProgress(state);
            return 0;
        }
        if (wParam == CdpStopProgressCloseTimer) {
            DestroyWindow(window);
            return 0;
        }
        break;

    case CdpStopProgressCompleteMessage:
        if (reinterpret_cast<StopProgressState*>(lParam) == state) {
            KillTimer(window, CdpStopProgressTimer);
            UpdateStopProgress(state);
            SetTimer(window, CdpStopProgressCloseTimer, 350, nullptr);
            return 0;
        }
        break;

    case WM_CLOSE:
        /* The driver cannot safely cancel a writeback part way through. */
        return 0;

    case WM_DESTROY:
        KillTimer(window, CdpStopProgressTimer);
        KillTimer(window, CdpStopProgressCloseTimer);
        if (state->BodyFont) DeleteObject(state->BodyFont);
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

static BOOL StopProtectionWithProgress(HANDLE device, const GUID& sourceVolumeGuid) {
    static const wchar_t className[] = L"CdpDriverStopProgress";
    static ATOM windowClass = 0;
    WNDCLASSW windowClassInfo = {};
    StopProgressState state = { device, INVALID_HANDLE_VALUE, sourceVolumeGuid,
        nullptr, nullptr, nullptr, nullptr, nullptr, ERROR_GEN_FAILURE, nullptr };
    HWND window;
    MSG message;

    if (!windowClass) {
        windowClassInfo.lpfnWndProc = StopProgressWindowProc;
        windowClassInfo.hInstance = GetModuleHandleW(nullptr);
        windowClassInfo.hCursor = LoadCursorW(nullptr, IDC_WAIT);
        windowClassInfo.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        windowClassInfo.lpszClassName = className;
        windowClass = RegisterClassW(&windowClassInfo);
        if (!windowClass && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
            return FALSE;
    }
    InitCommonControls();
    state.QueryDevice = OpenControlDevice();
    window = CreateWindowExW(WS_EX_DLGMODALFRAME, className, L"正在关闭系统盘保护",
        WS_CAPTION | WS_POPUP | WS_VISIBLE,
        (GetSystemMetrics(SM_CXSCREEN) - 420) / 2,
        (GetSystemMetrics(SM_CYSCREEN) - 135) / 2,
        420, 135, nullptr, nullptr, GetModuleHandleW(nullptr), &state);
    if (!window) {
        if (state.QueryDevice != INVALID_HANDLE_VALUE) CloseHandle(state.QueryDevice);
        return FALSE;
    }
    state.Worker = CreateThread(nullptr, 0, StopProtectionWorker, &state, 0, nullptr);
    if (!state.Worker) {
        DestroyWindow(window);
        if (state.QueryDevice != INVALID_HANDLE_VALUE) CloseHandle(state.QueryDevice);
        return FALSE;
    }
    SetTimer(window, CdpStopProgressTimer, 200, nullptr);
    while (IsWindow(window) && GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    WaitForSingleObject(state.Worker, INFINITE);
    CloseHandle(state.Worker);
    if (state.QueryDevice != INVALID_HANDLE_VALUE) CloseHandle(state.QueryDevice);
    if (state.Result != ERROR_SUCCESS) {
        SetLastError(state.Result);
        return FALSE;
    }
    return TRUE;
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
    DWORD bytesReturned = 0;
    HANDLE device;
    Cdp_PHASE_QUERY_REQUEST phaseRequest = {};
    Cdp_PHASE_QUERY_REPLY phaseReply = {};
    wchar_t windowsDirectory[MAX_PATH] = {};
    wchar_t systemMountPoint[MAX_PATH] = {};
    const int state = CheckSystemProtectionStatus();

    if (state != ProtectionCheckActive && state != ProtectionCheckActiveWithRestorePoint)
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

    const BOOL ok = StopProtectionWithProgress(device, sourceVolumeGuid);
    const DWORD error = ok ? ERROR_SUCCESS : GetLastError();
    CloseHandle(device);
    if (!ok) {
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
