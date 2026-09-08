Unicode True
ManifestDPIAware True
RequestExecutionLevel admin

!include "MUI2.nsh"
!include "LogicLib.nsh"
!include "x64.nsh"

!define PRODUCT_NAME "源点恢复"
!define PRODUCT_VERSION "1.6.8.44"
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

Section "安装源点恢复" SEC_MAIN
    SectionIn RO
    SetShellVarContext all

    DetailPrint "正在复制图形管理工具..."
    SetOutPath "$INSTDIR"
    File /r "work\payload\*.*"

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
    Delete "$INSTDIR\CdpDriverInstallHelper.exe"

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
    ExecWait '"$SYSDIR\rundll32.exe" setupapi.dll,InstallHinfSection DefaultUninstall 132 "$INSTDIR\driver\CdpDriver.inf"'

    Delete "$DESKTOP\源点恢复.lnk"
    RMDir /r "$SMPROGRAMS\源点恢复"
    DeleteRegKey HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\源点恢复"
    DeleteRegKey HKLM "Software\源点恢复"
    RMDir /r "$INSTDIR"
    SetRebootFlag true
SectionEnd
