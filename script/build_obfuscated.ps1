<#
.SYNOPSIS
    Build CdpDriver Release|x64 with the obfuscating LLVM toolchain.

.DESCRIPTION
    Why this script exists
    ----------------------
    CdpDriver's Release|x64 configuration overrides CLToolPath/LinkToolPath to a
    locally built LLVM fork (clang-cl + lld-link) and passes the OLLVM/xVMP
    obfuscation passes per translation unit.

    Invoking MSBuild on CdpDriver.vcxproj *directly* does not work for that
    configuration: overriding CLToolPath makes the WDK property evaluation take a
    path where the WDK/MSVC include directories end up missing, and every file
    fails with:

        fatal error : 'ntddk.h' file not found

    (this is exactly what happened in CdpDriver\x64\Release\CdpDriver.log).
    The same project builds fine when the environment provides INCLUDE/LIB, which
    is what a Visual Studio Developer Command Prompt does and what this script
    reproduces by locating the WDK and MSVC toolchain itself.

    It also makes the obfuscated build *reproducible*: the obfuscation seed is
    passed via -mllvm -aesSeed (see CdpObfSeed in CdpDriver.vcxproj). Without a
    fixed seed the toolchain's CryptoUtils PRNG falls back to CryptGenRandom and
    every build produces different machine code.

    NOTE: ASCII-only on purpose - Windows PowerShell 5.1 reads BOM-less .ps1
    files using the ANSI code page.

.PARAMETER Configuration
    Build configuration. Default: Release. Only Release|x64 uses the
    obfuscating toolchain.

.PARAMETER Platform
    Build platform. Default: x64.

.PARAMETER Rebuild
    Pass /t:Rebuild instead of /t:Build (full clean rebuild). Use this when
    checking build reproducibility.

.PARAMETER Verify
    After a successful build, run script\verify_obfuscation.ps1 automatically.

.PARAMETER Seed
    Override the obfuscation seed (32 hex chars) instead of the CdpObfSeed
    default from the project file.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File script\build_obfuscated.ps1 -Verify

.EXAMPLE
    :: build twice and compare, proving reproducibility
    powershell -ExecutionPolicy Bypass -File script\build_obfuscated.ps1 -Rebuild
    $a = (Get-FileHash x64\Release\driver\CdpDriver.sys -Algorithm SHA256).Hash
    powershell -ExecutionPolicy Bypass -File script\build_obfuscated.ps1 -Rebuild
    $b = (Get-FileHash x64\Release\driver\CdpDriver.sys -Algorithm SHA256).Hash
    if ($a -eq $b) { 'reproducible' } else { "DIFFERS: $a vs $b" }
#>
[CmdletBinding()]
param(
    [string]$Configuration = 'Release',
    [string]$Platform      = 'x64',
    [switch]$Rebuild,
    [switch]$Verify,
    [string]$Seed
)

$ErrorActionPreference = 'Stop'

# --------------------------------------------------------------- locate inputs
$scriptDir  = $PSScriptRoot
if (-not $scriptDir) { $scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path }
$rootDir    = Split-Path -Parent $scriptDir
$project    = Join-Path $rootDir 'CdpDriver\CdpDriver.vcxproj'

if (-not (Test-Path -LiteralPath $project)) {
    Write-Host "Project not found: $project" -ForegroundColor Red
    exit 2
}

Write-Host 'CdpDriver obfuscated build' -ForegroundColor White
Write-Host "  Project : $project"
Write-Host "  Config  : $Configuration|$Platform"

# ------------------------------------------------------- WDK / MSVC environment
# The WDK version is discovered from the "Windows Kits\10\Include" children so
# the script keeps working after a WDK upgrade. The newest one wins.
$kitsRoot = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10'
if (-not (Test-Path -LiteralPath $kitsRoot)) {
    Write-Host "Windows Kits not found at $kitsRoot" -ForegroundColor Red
    exit 3
}

$sdkVersion = Get-ChildItem (Join-Path $kitsRoot 'Include') -Directory -ErrorAction SilentlyContinue |
    Where-Object { $_.Name -match '^10\.' -and (Test-Path (Join-Path $_.FullName 'km\ntddk.h')) } |
    Sort-Object { [version]$_.Name } -Descending |
    Select-Object -First 1
if (-not $sdkVersion) {
    Write-Host "No WDK (km\ntddk.h) found under $(Join-Path $kitsRoot 'Include')" -ForegroundColor Red
    exit 3
}
$sdkVer = $sdkVersion.Name

# Newest MSVC toolset under the newest Visual Studio install.
$vsRoot = Join-Path ${env:ProgramFiles} 'Microsoft Visual Studio\2022'
$msvcRoot = Get-ChildItem $vsRoot -Directory -ErrorAction SilentlyContinue |
    ForEach-Object { Join-Path $_.FullName 'VC\Tools\MSVC' } |
    Where-Object { Test-Path -LiteralPath $_ } |
    ForEach-Object { Get-ChildItem $_ -Directory -ErrorAction SilentlyContinue } |
    Sort-Object { [version]$_.Name } -Descending |
    Select-Object -First 1
if (-not $msvcRoot) {
    Write-Host 'No MSVC toolset found under Visual Studio 2022' -ForegroundColor Red
    exit 3
}
$msvcDir = $msvcRoot.FullName

$env:INCLUDE = @(
    (Join-Path $msvcDir 'include'),
    (Join-Path $kitsRoot "Include\$sdkVer\km"),
    (Join-Path $kitsRoot "Include\$sdkVer\shared"),
    (Join-Path $kitsRoot "Include\$sdkVer\ucrt")
) -join ';'

$env:LIB = @(
    (Join-Path $msvcDir 'lib\x64'),
    (Join-Path $kitsRoot "Lib\$sdkVer\km\x64"),
    (Join-Path $kitsRoot "Lib\$sdkVer\ucrt\x64")
) -join ';'

Write-Host "  WDK     : $sdkVer"
Write-Host "  MSVC    : $($msvcRoot.Name)"

# --------------------------------------------------------------- locate MSBuild
$msbuild = Join-Path $vsRoot 'Community\MSBuild\Current\Bin\MSBuild.exe'
if (-not (Test-Path -LiteralPath $msbuild)) {
    $found = Get-ChildItem $vsRoot -Directory -ErrorAction SilentlyContinue |
        ForEach-Object { Join-Path $_.FullName 'MSBuild\Current\Bin\MSBuild.exe' } |
        Where-Object { Test-Path -LiteralPath $_ } |
        Select-Object -First 1
    $msbuild = $found
}
if (-not $msbuild) {
    Write-Host 'MSBuild.exe not found' -ForegroundColor Red
    exit 3
}

# --------------------------------------------------------------- build
# Single-node / single-CL on purpose: the obfuscation passes are stateful and the
# toolchain is a custom build; parallel cl.exe instances have historically been a
# source of flakiness here (see xxbuild.bat, which disables the same settings).
$target = if ($Rebuild) { 'Rebuild' } else { 'Build' }
$msbuildArgs = @(
    $project
    "/t:$target"
    '/m:1'
    "/p:Configuration=$Configuration"
    "/p:Platform=$Platform"
    '/p:MultiProcessorCompilation=false'
    '/p:UseMultiToolTask=false'
    '/p:CL_MPCount=1'
    '/v:minimal'
    '/nologo'
)
if ($Seed) {
    $msbuildArgs += "/p:CdpObfSeed=$Seed"
    Write-Host "  Seed    : $Seed (override)"
}

Write-Host ''
& $msbuild @msbuildArgs
$result = $LASTEXITCODE

if ($result -ne 0) {
    Write-Host ''
    Write-Host "Build FAILED (exit $result)." -ForegroundColor Red
    Write-Host 'If the failure is "ntddk.h file not found", the INCLUDE/LIB setup above did not take effect.' -ForegroundColor Yellow
    exit $result
}

# --------------------------------------------------------------- artifacts
$sysPath = Join-Path $rootDir "x64\$Configuration\driver\CdpDriver.sys"
if (-not (Test-Path -LiteralPath $sysPath)) {
    $sysPath = Join-Path $rootDir "CdpDriver\x64\$Configuration\CdpDriver.sys"
}
if (Test-Path -LiteralPath $sysPath) {
    $hash = (Get-FileHash -LiteralPath $sysPath -Algorithm SHA256).Hash
    $size = (Get-Item -LiteralPath $sysPath).Length
    Write-Host ''
    Write-Host 'Build OK' -ForegroundColor Green
    Write-Host "  Image : $sysPath"
    Write-Host ("  Size  : {0:N0} bytes" -f $size)
    Write-Host "  SHA256: $hash"
} else {
    Write-Host ''
    Write-Host 'Build reported success but CdpDriver.sys was not found.' -ForegroundColor Yellow
}

if ($Verify) {
    $verifyScript = Join-Path $scriptDir 'verify_obfuscation.ps1'
    if (Test-Path -LiteralPath $verifyScript) {
        Write-Host ''
        & $verifyScript -ProjectDir (Join-Path $rootDir 'CdpDriver') -Configuration $Configuration -Platform $Platform
        exit $LASTEXITCODE
    }
    Write-Host "Verifier not found: $verifyScript" -ForegroundColor Yellow
}

exit 0
