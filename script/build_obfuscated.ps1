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

.PARAMETER EnableReleaseLogging
    Compile Cdp_LOG into a diagnostic Release image. Omit this for the default
    production image, which removes Release log calls and format strings.

.PARAMETER EnableKernelVm
    After a successful Release|x64 build, replace only the isolated
    capability-token object with the xollvm kernel-profile VM object and
    relink/sign CdpDriver.sys. This preserves the normal compiler pipeline for
    the rest of the driver. It requires the locally built xollvm LLVM 22
    toolchain at -KernelVmToolchain.

.PARAMETER KernelVmToolchain
    Directory that contains xollvm's clang-cl.exe, opt.exe and lld-link.exe.
    The default matches the locally built kernel-safe LLVM 22 toolchain.

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
    [switch]$EnableReleaseLogging,
    [switch]$EnableKernelVm,
    [string]$KernelVmToolchain = 'E:\xollvm-experiment\llvm22-kernel-build-fixed\bin',
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
if ($EnableReleaseLogging) {
    $msbuildArgs += '/p:CdpReleaseLogging=true'
    Write-Host '  Release logging: enabled (diagnostic image only)'
}

if ($EnableKernelVm -and ($Configuration -ne 'Release' -or $Platform -ne 'x64')) {
    throw '-EnableKernelVm is supported only for Release|x64.'
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

# --------------------------------------------------------- optional kernel VM
if ($EnableKernelVm) {
    $vmRequired = @('clang-cl.exe', 'opt.exe', 'lld-link.exe') |
        ForEach-Object { Join-Path $KernelVmToolchain $_ }
    $vmMissing = $vmRequired | Where-Object { -not (Test-Path -LiteralPath $_) }
    if ($vmMissing) {
        throw "xollvm kernel toolchain is incomplete: $($vmMissing -join ', ')"
    }

    $projectDir = Join-Path $rootDir 'CdpDriver'
    $objDir = Join-Path $projectDir "x64\$Configuration"
    $mainSys = Join-Path $objDir 'CdpDriver.sys'
    $deploySys = Join-Path $rootDir "x64\$Configuration\driver\CdpDriver.sys"
    # The WDK packaging target also leaves this release-root mirror; the
    # verifier and ad-hoc deployment workflows consume it directly.
    $releaseMirrorSys = Join-Path $rootDir "x64\$Configuration\CdpDriver.sys"
    $packageDir = Join-Path $objDir 'CdpDriver'
    if (-not (Test-Path -LiteralPath $mainSys)) {
        throw "Normal driver image was not produced: $mainSys"
    }
    if (-not (Test-Path -LiteralPath $packageDir)) {
        throw "WDK package directory was not produced: $packageDir"
    }

    # Sign with the certificate that MSBuild just used for the normal image.
    # This avoids hard-coding a machine-specific thumbprint in source control.
    $normalSignature = Get-AuthenticodeSignature -FilePath $mainSys
    $signThumbprint = $normalSignature.SignerCertificate.Thumbprint
    if (-not $signThumbprint) {
        throw 'Cannot obtain the signing certificate thumbprint from the normal driver image.'
    }
    $signTool = Join-Path $kitsRoot "bin\$sdkVer\x64\signtool.exe"
    if (-not (Test-Path -LiteralPath $signTool)) {
        throw "signtool.exe was not found: $signTool"
    }
    $inf2cat = Join-Path $kitsRoot "bin\$sdkVer\x86\Inf2Cat.exe"
    if (-not (Test-Path -LiteralPath $inf2cat)) {
        throw "Inf2Cat.exe was not found: $inf2cat"
    }

    $vmTemp = Join-Path ([System.IO.Path]::GetTempPath()) ("CdpDriver-kernel-vm-" + [guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $vmTemp | Out-Null
    try {
        $vmSource = Join-Path $projectDir 'CdpLicenseVmRecomputeCap.c'
        $vmBitcode = Join-Path $vmTemp 'CdpLicenseVmRecomputeCap.bc'
        $vmObfBitcode = Join-Path $vmTemp 'CdpLicenseVmRecomputeCap.obf.bc'
        $vmObject = Join-Path $vmTemp 'CdpLicenseVmRecomputeCap.obj'
        $vmCandidate = Join-Path $vmTemp 'CdpDriver.kernel-vm.sys'

        Write-Host ''
        Write-Host 'Applying xollvm kernel VM to authentication boundaries...' -ForegroundColor White
        Push-Location $vmTemp
        & (Join-Path $KernelVmToolchain 'clang-cl.exe') /nologo /c /Od /Zl /kernel `
            /DCDP_LICENSE /DCDP_LICENSE_OBFUSCATE /DCDP_XOLLVM_BUILD `
            /D_WIN64 /D_AMD64_ /DAMD64 /D_WIN32_WINNT=0x0A00 /DWINVER=0x0A00 /DWINNT=1 /DNTDDI_VERSION=0xA000010 `
            "/I$projectDir" "/I$(Join-Path $rootDir 'CdpCore\include')" /clang:-emit-llvm $vmSource
        if ($LASTEXITCODE -ne 0) { throw "xollvm bitcode compilation failed ($LASTEXITCODE)." }
        Pop-Location

        if (-not (Test-Path -LiteralPath $vmBitcode)) {
            throw "xollvm did not produce bitcode: $vmBitcode"
        }
        & (Join-Path $KernelVmToolchain 'opt.exe') -passes=obfuscation $vmBitcode -o $vmObfBitcode `
            -obf-seed=239 -obf-deterministic -obf-verify -obf-vm-kernel -obf-verbose
        if ($LASTEXITCODE -ne 0) { throw "xollvm VM transformation failed ($LASTEXITCODE)." }
        & (Join-Path $KernelVmToolchain 'clang-cl.exe') /nologo /c /Zl /kernel $vmObfBitcode "/Fo$vmObject"
        if ($LASTEXITCODE -ne 0) { throw "xollvm VM object compilation failed ($LASTEXITCODE)." }

        $vmFunctions = @('CdpLicenseVmRecomputeCap', 'CdpLicenseVmArmEvidenceValid', 'CdpLicenseVmCommitEvidenceValid')
        $vmSymbols = & (Join-Path $msvcDir 'bin\Hostx64\x64\dumpbin.exe') /symbols $vmObject
        foreach ($vmFunction in $vmFunctions) {
            if (-not ($vmSymbols -match [regex]::Escape("$vmFunction.vm.ctor"))) {
                throw "xollvm did not virtualise $vmFunction."
            }
        }

        $objectNames = @(
            'CdpCredential.obj', 'CdpIoctlGuard.obj', 'CdpIrpDispatchs.obj',
            'CdpJournal.obj', 'CdpJournalCodec.obj', 'CdpLicenseCodec.obj',
            'CdpLicenseGate.obj', 'CdpLicenseGateCall.obj', 'CdpLicenseHw.obj',
            'CdpLicenseProtect.obj', 'CdpLicenseTrust.obj', 'CdpLocalSeal.obj',
            'cdp_core.obj', 'cdp_dev_store.obj', 'cdp_view_policy.obj', 'Driver.obj'
        )
        # Build the argument vector one item at a time.  In Windows PowerShell,
        # mixing '+' and commas inside an array literal can accidentally turn
        # multiple linker switches into one output-path argument.
        $linkArgs = @()
        $linkArgs += ('/OUT:' + $vmCandidate)
        $linkArgs += '/VERSION:10.0'
        $linkArgs += '/INCREMENTAL:NO'
        $linkArgs += '/NOLOGO'
        $linkArgs += '/WX'
        $linkArgs += '/SECTION:INIT,d'
        foreach ($objectName in $objectNames) {
            $objectPath = Join-Path $objDir $objectName
            if (-not (Test-Path -LiteralPath $objectPath)) { throw "Build object missing: $objectPath" }
            $linkArgs += $objectPath
        }
        $kmLibDir = Join-Path $kitsRoot "Lib\$sdkVer\km\x64"
        $linkArgs += $vmObject
        $linkArgs += (Join-Path $kmLibDir 'cng.lib')
        $linkArgs += (Join-Path $kmLibDir 'bufferoverflowfastfailk.lib')
        $linkArgs += (Join-Path $kmLibDir 'ntoskrnl.lib')
        $linkArgs += (Join-Path $kmLibDir 'hal.lib')
        $linkArgs += (Join-Path $kmLibDir 'wmilib.lib')
        $linkArgs += @('/NODEFAULTLIB', '/MANIFEST:NO', '/SUBSYSTEM:NATIVE,10.00', '/Driver',
            '/OPT:REF', '/OPT:ICF', '/ENTRY:GsDriverEntry', '/RELEASE', '/MACHINE:X64',
            '/PROFILE', '/guard:cf', '/kernel',
            '/IGNORE:4198,4010,4037,4039,4065,4070,4078,4087,4089,4221,4108,4088,4218,4235',
            '/osversion:10.0', '/debugtype:pdata')
        & (Join-Path $KernelVmToolchain 'lld-link.exe') @linkArgs
        if ($LASTEXITCODE -ne 0) { throw "xollvm VM relink failed ($LASTEXITCODE)." }

        $forbiddenImports = 'fmod|IsDebuggerPresent|CheckRemoteDebuggerPresent|GetCurrentProcess|GetModuleHandle|GetProcAddress|_fltused|__stdio'
        $candidateImports = & (Join-Path $msvcDir 'bin\Hostx64\x64\dumpbin.exe') /imports $vmCandidate
        if ($candidateImports -match $forbiddenImports) {
            throw 'Kernel VM candidate has a forbidden user-mode, floating-point, or CRT import.'
        }
        & $signTool sign /sha1 $signThumbprint /fd SHA256 $vmCandidate
        if ($LASTEXITCODE -ne 0) { throw "Kernel VM signing failed ($LASTEXITCODE)." }
        Copy-Item -LiteralPath $vmCandidate -Destination $mainSys -Force
        Copy-Item -LiteralPath $vmCandidate -Destination $deploySys -Force
        Copy-Item -LiteralPath $vmCandidate -Destination $releaseMirrorSys -Force
        Copy-Item -LiteralPath $vmCandidate -Destination (Join-Path $packageDir 'CdpDriver.sys') -Force

        # MSBuild generated the original CAT before the VM object was swapped.
        # Regenerate it against the final signed SYS, then sign that catalog and
        # copy the coherent INF/CAT/SYS set to the deployment package directory.
        & $inf2cat ("/driver:$packageDir") '/os:10_X64'
        if ($LASTEXITCODE -ne 0) { throw "Inf2Cat catalog generation failed ($LASTEXITCODE)." }
        $catalog = Get-ChildItem -LiteralPath $packageDir -Filter '*.cat' -File | Select-Object -First 1
        if (-not $catalog) { throw "Inf2Cat did not create a catalog in $packageDir" }
        & $signTool sign /sha1 $signThumbprint /fd SHA256 $catalog.FullName
        if ($LASTEXITCODE -ne 0) { throw "Catalog signing failed ($LASTEXITCODE)." }
        Copy-Item -LiteralPath (Join-Path $packageDir 'CdpDriver.inf') -Destination (Join-Path (Split-Path -Parent $deploySys) 'CdpDriver.inf') -Force
        Copy-Item -LiteralPath (Join-Path $packageDir 'CdpDriver.sys') -Destination $deploySys -Force
        Copy-Item -LiteralPath $catalog.FullName -Destination (Join-Path (Split-Path -Parent $deploySys) 'CdpDriver.cat') -Force
        Write-Host '  Kernel VM : 3 authentication boundaries virtualised' -ForegroundColor Green
        Write-Host '  Package   : INF/CAT regenerated and signed for the final VM driver' -ForegroundColor Green
    }
    finally {
        if ((Get-Location).Path -eq $vmTemp) { Pop-Location }
        Remove-Item -LiteralPath $vmTemp -Recurse -Force -ErrorAction SilentlyContinue
    }
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
        $verifyArgs = @{
            ProjectDir = (Join-Path $rootDir 'CdpDriver')
            Configuration = $Configuration
            Platform = $Platform
        }
        if ($EnableReleaseLogging) { $verifyArgs.AllowReleaseLogging = $true }
        & $verifyScript @verifyArgs
        exit $LASTEXITCODE
    }
    Write-Host "Verifier not found: $verifyScript" -ForegroundColor Yellow
}

exit 0
