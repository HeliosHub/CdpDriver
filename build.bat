@echo off
setlocal EnableExtensions

rem Usage:
rem   build.bat
rem   build.bat Release x64
rem   build.bat Debug x64

set "CONFIGURATION=%~1"
if not defined CONFIGURATION set "CONFIGURATION=Release"

set "PLATFORM=%~2"
if not defined PLATFORM set "PLATFORM=x64"

set "ROOT_DIR=%~dp0"
set "SOLUTION=%ROOT_DIR%CdpDriver.sln"
set "MSBUILD_EXE="

if not exist "%SOLUTION%" (
    echo [ERROR] Solution not found: "%SOLUTION%"
    exit /b 2
)

rem Prefer Visual Studio Installer's discovery utility so the script also works
rem when it is launched outside a Developer Command Prompt.
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if exist "%VSWHERE%" (
    for /f "usebackq tokens=*" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.Component.MSBuild -property installationPath`) do (
        if exist "%%I\MSBuild\Current\Bin\MSBuild.exe" set "MSBUILD_EXE=%%I\MSBuild\Current\Bin\MSBuild.exe"
    )
)

rem Fall back to MSBuild already available in PATH.
if not defined MSBUILD_EXE (
    for /f "delims=" %%I in ('where MSBuild.exe 2^>nul') do (
        if not defined MSBUILD_EXE set "MSBUILD_EXE=%%I"
    )
)

if not defined MSBUILD_EXE (
    echo [ERROR] MSBuild was not found.
    echo         Install Visual Studio 2022 with Desktop development with C++
    echo         and the Windows Driver Kit, then run this script again.
    exit /b 3
)

echo [BUILD] Solution      : "%SOLUTION%"
echo [BUILD] Configuration : %CONFIGURATION%
echo [BUILD] Platform      : %PLATFORM%
echo [BUILD] MSBuild       : "%MSBUILD_EXE%"
echo.

pushd "%ROOT_DIR%" >nul
"%MSBUILD_EXE%" "%SOLUTION%" /nologo /m /t:Build /p:Configuration="%CONFIGURATION%" /p:Platform="%PLATFORM%" /v:minimal
set "BUILD_RESULT=%ERRORLEVEL%"
popd >nul

if not "%BUILD_RESULT%"=="0" (
    echo.
    echo [ERROR] Build failed with exit code %BUILD_RESULT%.
    exit /b %BUILD_RESULT%
)

echo.
echo [OK] Build completed successfully.
echo [OK] Output directory: "%ROOT_DIR%%PLATFORM%\%CONFIGURATION%"
pause
