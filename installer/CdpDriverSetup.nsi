Unicode True
ManifestDPIAware True
RequestExecutionLevel admin

!include "MUI2.nsh"
!include "LogicLib.nsh"
!include "WordFunc.nsh"
!include "x64.nsh"

!insertmacro VersionCompare

!define PRODUCT_NAME "源点恢复"
!define PRODUCT_VERSION "1.0.2.0"
!define PRODUCT_PUBLISHER "源点恢复"
!define PRODUCT_EXE "源点恢复.exe"

Name "${PRODUCT_NAME}"
OutFile "work\out\RecoverySetup-x64.exe"
InstallDir "$PROGRAMFILES64\源点恢复"
InstallDirRegKey HKLM "Software\源点恢复" "InstallDir"
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
!define MUI_WELCOMEPAGE_TEXT "安装向导将安装源点恢复、卷筛选驱动和启动服务。$\r$\n$\r$\n继续前请关闭正在运行的源点恢复程序。"
!define MUI_DIRECTORYPAGE_TEXT_TOP "请选择源点恢复的安装位置。"
!define MUI_FINISHPAGE_TITLE "源点恢复安装完成"
!define MUI_FINISHPAGE_TEXT "源点恢复已成功安装。必须重启 Windows 才能加载卷筛选驱动。"
!define MUI_FINISHPAGE_REBOOTLATER_DEFAULT

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_DIRECTORY
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
    MessageBox MB_RETRYCANCEL|MB_ICONEXCLAMATION "暂时无法检测系统盘保护状态。请关闭正在运行的源点恢复后重试。" IDRETRY system_protection_check IDCANCEL system_protection_cancel

system_protection_stop:
    DetailPrint "正在关闭系统盘保护..."
    ; The helper shows its own password window. nsExec runs child processes
    ; hidden, which would make that prompt invisible and leave setup waiting.
    ExecWait '"$PLUGINSDIR\CdpDriverInstallHelper.exe" --stop-system-protection' $0
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

    DetailPrint "正在复制图形管理工具..."
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

    DetailPrint "正在停止并删除 CdpBootService..."
    ExecWait '"$INSTDIR\CdpBootService.exe" --uninstall'

    DetailPrint "正在注销 CdpDriver 卷筛选驱动..."
    nsExec::ExecToLog '"$INSTDIR\CdpDriverInstallHelper.exe" --uninstall'
    Pop $0
    ${If} $0 != 0
        DetailPrint "警告：驱动注销未完全完成，错误码：$0。文件卸载将继续。"
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
