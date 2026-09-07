@echo off
setlocal

set "SCRIPT_DIR=%~dp0"
fltmc >nul 2>&1
if errorlevel 1 (
    set "CDP_INSTALLER_BAT=%~f0"
    echo Requesting administrator privileges...
    powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "$p = Start-Process -FilePath $env:CDP_INSTALLER_BAT -Verb RunAs -Wait -PassThru; exit $p.ExitCode"
    exit /b %ERRORLEVEL%
)

pushd "%SCRIPT_DIR%" || exit /b 1

powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%SCRIPT_DIR%Build-Installer.ps1"
set "BUILD_EXIT=%ERRORLEVEL%"

if not "%BUILD_EXIT%"=="0" (
    echo.
    echo Installer build failed. Exit code: %BUILD_EXIT%
) else (
    echo.
    echo Installer generated: %SCRIPT_DIR%RecoverySetup-x64.exe
)

popd
exit /b %BUILD_EXIT%
