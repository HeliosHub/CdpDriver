Unicode True
ManifestDPIAware True
RequestExecutionLevel admin

!include "MUI2.nsh"
!include "LogicLib.nsh"
!include "WordFunc.nsh"
!include "x64.nsh"

!insertmacro VersionCompare

!define PRODUCT_NAME "源点恢复"
!define PRODUCT_VERSION "1.0.16.0"
!define PRODUCT_PUBLISHER "源点恢复"
!define PRODUCT_EXE "源点恢复.exe"

Var MaintenanceAuthorized

Name "${PRODUCT_NAME}"
OutFile "work\out\RecoverySetup-x64.exe"
InstallDir "$PROGRAMFILES64\源点恢复"
BrandingText "源点恢复安装向导"
Icon "work\app.ico"
UninstallIcon "work\app.ico"
ShowInstDetails show
ShowUninstDetails show
VIProductVersion "${PRODUCT_VERSION}"
VIAddVersionKey /LANG=2052 "ProductName" "${PRODUCT_NAME}"
VIAddVersionKey /LANG=2052 "FileDescription" "源点恢复安装程序"
VIAddVersionKey /LANG=2052 "CompanyName" "${PRODUCT_PUBLISHER}"
VIAddVersionKey /LANG=2052 "LegalCopyright" "Copyright (C) 2026 源点恢复"
VIAddVersionKey /LANG=2052 "FileVersion" "${PRODUCT_VERSION}"
VIAddVersionKey /LANG=2052 "ProductVersion" "${PRODUCT_VERSION}"

!define MUI_ABORTWARNING
!define MUI_ICON "work\app.ico"
!define MUI_UNICON "work\app.ico"
!define MUI_WELCOMEPAGE_TITLE "欢迎安装源点恢复"
!define MUI_WELCOMEPAGE_TEXT "安装向导将把源点恢复安装到固定目录，并安装卷筛选驱动、自保护驱动和启动服务。$\r$\n$\r$\n安装位置不能修改。继续前请关闭正在运行的源点恢复程序。"
!define MUI_FINISHPAGE_TITLE "源点恢复安装完成"
!define MUI_FINISHPAGE_TEXT "源点恢复已成功安装。必须重启 Windows 才能加载卷筛选驱动。"
!define MUI_FINISHPAGE_REBOOTLATER_DEFAULT

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_UNPAGE_FINISH

!insertmacro MUI_LANGUAGE "SimpChinese"

Function .onInit
    ${IfNot} ${RunningX64}
        MessageBox MB_OK|MB_ICONSTOP "此安装包仅支持 64 位 Windows。"
        Abort
    ${EndIf}
    SetRegView 64
    ; Keep the product in its fixed default location, including upgrades from
    ; older installers that may have remembered a custom directory.
    StrCpy $INSTDIR "$PROGRAMFILES64\源点恢复"
FunctionEnd

Function un.onInit
    SetShellVarContext all
    SetRegView 64

protection_check:
    nsExec::ExecToLog '"$INSTDIR\CdpDriverInstallHelper.exe" --check-protection'
    Pop $0
    ${If} $0 == 10
        MessageBox MB_RETRYCANCEL|MB_ICONEXCLAMATION "检测到仍有分区处于保护状态。$\r$\n$\r$\n请先打开“源点恢复”，关闭所有分区保护并等待操作完成，然后点击“重试”。" IDRETRY protection_check IDCANCEL cancel_uninstall
    ${ElseIf} $0 != 0
        MessageBox MB_RETRYCANCEL|MB_ICONEXCLAMATION "暂时无法检查分区保护状态。$\r$\n$\r$\n请关闭正在运行的“源点恢复”，然后点击“重试”。" IDRETRY protection_check IDCANCEL cancel_uninstall
    ${EndIf}
    Return

cancel_uninstall:
    Abort
FunctionEnd

Function EnsureSystemProtectionDisabled
system_protection_check:
    nsExec::ExecToLog '"$PLUGINSDIR\CdpDriverInstallHelper.exe" --check-system-protection'
    Pop $0
    ${If} $0 == 0
        Return
    ${EndIf}
    ${If} $0 == 10
        MessageBox MB_YESNO|MB_ICONEXCLAMATION "检测到系统盘仍受源点恢复保护。必须先关闭系统盘保护，才能继续安装。$\r$\n$\r$\n点击“是”立即关闭系统盘保护（随后需要输入保护密码）；点击“否”取消安装。" IDYES system_protection_stop IDNO system_protection_cancel
    ${EndIf}
    ${If} $0 == 13
        MessageBox MB_YESNO|MB_ICONEXCLAMATION "检测到系统盘仍受源点恢复保护，且已设置还原点。必须先关闭系统盘保护，才能继续安装。$\r$\n$\r$\n建议先重启系统，再关闭系统盘保护。$\r$\n$\r$\n点击“是”立即关闭系统盘保护（随后需要输入保护密码）；点击“否”取消安装。" IDYES system_protection_stop IDNO system_protection_cancel
    ${EndIf}
    MessageBox MB_RETRYCANCEL|MB_ICONEXCLAMATION "暂时无法检测系统盘保护状态。请关闭正在运行的源点恢复后重试。" IDRETRY system_protection_check IDCANCEL system_protection_cancel

system_protection_stop:
    DetailPrint "正在关闭系统盘保护..."
    ; The helper shows its own password window. nsExec runs child processes
    ; hidden, which would make that prompt invisible and leave setup waiting.
    ; 已安装的 Helper 位于 CdpProtect 允许的固定路径，可在口令验证成功后
    ; 把维护会话签发给安装器。没有已安装 Helper 时才使用临时副本。
    IfFileExists "$INSTDIR\CdpDriverInstallHelper.exe" 0 system_protection_stop_plugin
    ExecWait '"$INSTDIR\CdpDriverInstallHelper.exe" --stop-system-protection' $0
    Goto system_protection_stop_result
system_protection_stop_plugin:
    ExecWait '"$PLUGINSDIR\CdpDriverInstallHelper.exe" --stop-system-protection' $0
system_protection_stop_result:
    ${If} $0 == 14
        StrCpy $MaintenanceAuthorized 1
        StrCpy $0 0
    ${EndIf}
    ${If} $0 == 0
        DetailPrint "系统盘保护已关闭。"
        Goto system_protection_check
    ${EndIf}
    ${If} $0 == 12
        MessageBox MB_RETRYCANCEL|MB_ICONEXCLAMATION "已取消输入保护密码。必须关闭系统盘保护才能继续安装。" IDRETRY system_protection_stop IDCANCEL system_protection_cancel
    ${EndIf}
    MessageBox MB_RETRYCANCEL|MB_ICONSTOP "关闭系统盘保护失败。请确认保护密码正确且没有恢复或预览任务正在执行，然后重试。" IDRETRY system_protection_stop IDCANCEL system_protection_cancel

system_protection_cancel:
    Abort
FunctionEnd

Function CloseRunningGui
close_gui_current:
    DetailPrint "正在关闭已运行的源点恢复..."
    nsExec::ExecToLog '"$WINDIR\Sysnative\taskkill.exe" /F /T /IM "源点恢复.exe"'
    Pop $0
    ${If} $0 != 0
    ${AndIf} $0 != 128
        MessageBox MB_RETRYCANCEL|MB_ICONEXCLAMATION "无法关闭正在运行的源点恢复（错误码：$0）。请手动关闭后重试。" IDRETRY close_gui_current IDCANCEL close_gui_cancel
    ${EndIf}

close_gui_legacy:
    nsExec::ExecToLog '"$WINDIR\Sysnative\taskkill.exe" /F /T /IM "CDPCorePro.exe"'
    Pop $0
    ${If} $0 != 0
    ${AndIf} $0 != 128
        MessageBox MB_RETRYCANCEL|MB_ICONEXCLAMATION "无法关闭正在运行的源点恢复（错误码：$0）。请手动关闭后重试。" IDRETRY close_gui_legacy IDCANCEL close_gui_cancel
    ${EndIf}
    Return

close_gui_cancel:
    Abort
FunctionEnd

Section "安装源点恢复" SEC_MAIN
    SectionIn RO
    SetShellVarContext all
    StrCpy $MaintenanceAuthorized 0

    ; Extract only the helper needed for the pre-install protection check;
    ; the final payload is not copied to the selected install directory yet.
    InitPluginsDir
    SetOutPath "$PLUGINSDIR"
    File /oname=CdpDriverInstallHelper.exe "work\payload\CdpDriverInstallHelper.exe"
    Call CloseRunningGui
    Call EnsureSystemProtectionDisabled

    DetailPrint "正在检查 Microsoft Visual C++ x64 运行库..."
    StrCpy $2 1
    ReadRegDWORD $0 HKLM "SOFTWARE\Microsoft\VisualStudio\14.0\VC\Runtimes\x64" "Installed"
    ${If} $0 == 1
        ReadRegStr $1 HKLM "SOFTWARE\Microsoft\VisualStudio\14.0\VC\Runtimes\x64" "Version"
        ${If} $1 != ""
            StrCpy $1 $1 "" 1
            ${VersionCompare} $1 "14.42.34438.0" $3
            ${If} $3 != 2
                StrCpy $2 0
            ${EndIf}
        ${EndIf}
    ${EndIf}

    ${If} $2 == 1
        DetailPrint "未检测到所需版本的运行库，正在安装..."
        SetOutPath "$PLUGINSDIR"
        File /oname=VC_redist.x64.exe "VC_redist.x64.exe"
        nsExec::ExecToLog '"$PLUGINSDIR\VC_redist.x64.exe" /install /quiet /norestart'
        Pop $1
        ${If} $1 == 3010
            SetRebootFlag true
        ${ElseIf} $1 == 1641
            SetRebootFlag true
        ${ElseIf} $1 == 1638
            DetailPrint "系统中已安装兼容的更新版本。"
        ${ElseIf} $1 != 0
            MessageBox MB_OK|MB_ICONSTOP "Microsoft Visual C++ x64 运行库安装失败，错误码：$1。"
            Abort
        ${EndIf}
    ${Else}
        DetailPrint "已检测到兼容的 Microsoft Visual C++ x64 运行库，跳过安装。"
    ${EndIf}

    IfFileExists "$INSTDIR\CdpDriverInstallHelper.exe" 0 copy_payload
    ${If} $MaintenanceAuthorized == 1
        DetailPrint "已沿用关闭系统盘保护时完成的维护授权。"
        Goto copy_payload
    ${EndIf}
    DetailPrint "正在请求维护授权..."
    DetailPrint "请在弹出的“源点恢复维护”窗口中输入保护密码。"
    System::Call 'user32::AllowSetForegroundWindow(i -1)'
    ; 升级时首先运行的是已安装的旧 Helper。由新包内的 Helper 在后台修正
    ; 旧口令窗口的位置并隐藏旧版“尚未设密”按钮。
    ExecShell "open" "$PLUGINSDIR\CdpDriverInstallHelper.exe" "--adjust-maintenance-window" SW_HIDE
    ExecWait '"$INSTDIR\CdpDriverInstallHelper.exe" --authorize-maintenance' $0
    ${If} $0 != 0
        MessageBox MB_OK|MB_ICONSTOP "维护授权失败，错误码：$0。已取消更新。"
        Abort
    ${EndIf}

copy_payload:
    DetailPrint "正在复制程序文件..."
    SetOutPath "$INSTDIR"
    File /r "work\payload\*.*"

    DetailPrint "正在启用 Windows 测试模式..."
    ; NSIS is a 32-bit process. Sysnative bypasses WOW64 redirection so this
    ; launches the 64-bit bcdedit.exe; SysWOW64 does not include bcdedit.exe.
    nsExec::ExecToLog '"$WINDIR\Sysnative\bcdedit.exe" /set testsigning on'
    Pop $0
    ${If} $0 != 0
        MessageBox MB_OK|MB_ICONSTOP "无法启用 Windows 测试模式，错误码：$0。请确认以管理员身份运行安装程序；若已启用安全启动，请先关闭安全启动后再试。"
        Abort
    ${EndIf}
    DetailPrint "Windows 测试模式将在重启后生效。"
    SetRebootFlag true

    DetailPrint "正在安装 CdpDriver 测试签名证书..."
    nsExec::ExecToLog '"$SYSDIR\certutil.exe" -addstore -f Root "$INSTDIR\driver\CdpDriver.cer"'
    Pop $0
    ${If} $0 != 0
        MessageBox MB_OK|MB_ICONSTOP "驱动证书安装失败，错误码：$0。"
        Abort
    ${EndIf}
    nsExec::ExecToLog '"$SYSDIR\certutil.exe" -addstore -f TrustedPublisher "$INSTDIR\driver\CdpDriver.cer"'
    Pop $0
    ${If} $0 != 0
        MessageBox MB_OK|MB_ICONSTOP "驱动发布者证书安装失败，错误码：$0。"
        Abort
    ${EndIf}

    DetailPrint "正在安装 CdpDriver 卷筛选驱动和启动服务..."
    nsExec::ExecToLog '"$INSTDIR\CdpDriverInstallHelper.exe" --install'
    Pop $0
    ${If} $0 != 0
        MessageBox MB_OK|MB_ICONSTOP "卷筛选驱动安装失败，错误码：$0。请查看安装进度中的详细信息。"
        Abort
    ${EndIf}

    DetailPrint "正在安装 CdpProtect 自保护驱动..."
    nsExec::ExecToLog '"$INSTDIR\CdpDriverInstallHelper.exe" --install-protect'
    Pop $0
    ${If} $0 != 0
        MessageBox MB_OK|MB_ICONSTOP "自保护驱动安装失败，错误码：$0。请查看安装进度中的详细信息。"
        Abort
    ${EndIf}

    DetailPrint "正在创建开始菜单和桌面快捷方式..."
    CreateDirectory "$SMPROGRAMS\源点恢复"
    CreateShortcut "$SMPROGRAMS\源点恢复\源点恢复.lnk" "$INSTDIR\${PRODUCT_EXE}" "" "$INSTDIR\${PRODUCT_EXE}" 0
    CreateShortcut "$DESKTOP\源点恢复.lnk" "$INSTDIR\${PRODUCT_EXE}" "" "$INSTDIR\${PRODUCT_EXE}" 0

    WriteUninstaller "$INSTDIR\Uninstall.exe"
    WriteRegStr HKLM "Software\源点恢复" "InstallDir" "$INSTDIR"
    WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\源点恢复" "DisplayName" "源点恢复"
    WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\源点恢复" "DisplayVersion" "${PRODUCT_VERSION}"
    WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\源点恢复" "Publisher" "${PRODUCT_PUBLISHER}"
    WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\源点恢复" "DisplayIcon" "$INSTDIR\${PRODUCT_EXE}"
    WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\源点恢复" "UninstallString" '"$INSTDIR\Uninstall.exe"'
    WriteRegDWORD HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\源点恢复" "NoModify" 1
    WriteRegDWORD HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\源点恢复" "NoRepair" 1

    DetailPrint "安装完成，等待重启后加载驱动。"
    SetRebootFlag true
SectionEnd

Section "Uninstall"
    SetShellVarContext all
    SetRegView 64

    DetailPrint "正在卸载驱动..."
    ExecWait '"$INSTDIR\CdpDriverInstallHelper.exe" --uninstall-all' $0
    ${If} $0 == 10
        MessageBox MB_OK|MB_ICONSTOP "仍有分区处于保护状态，已取消卸载。"
        Abort
    ${ElseIf} $0 != 0
        MessageBox MB_OK|MB_ICONSTOP "卸载驱动失败，错误码：$0。"
        Abort
    ${EndIf}

    Delete "$DESKTOP\源点恢复.lnk"
    RMDir /r "$SMPROGRAMS\源点恢复"
    ; 许可证仅用于本产品的本机恢复；卸载时必须一并移除，避免下一次安装继承旧授权。
    DeleteRegKey HKLM "Software\CDPCorePro\License"
    DeleteRegKey HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\源点恢复"
    DeleteRegKey HKLM "Software\源点恢复"
    RMDir /r "$INSTDIR"
    SetRebootFlag true
SectionEnd
