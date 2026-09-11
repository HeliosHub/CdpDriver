[CmdletBinding()]
param(
    [string]$GuiRoot = '',
    [string]$OutputDirectory = ''
)

$ErrorActionPreference = 'Stop'

$installerRoot = Split-Path -Parent $PSCommandPath
$driverRoot = Split-Path -Parent $installerRoot
if ([string]::IsNullOrWhiteSpace($GuiRoot)) {
    $GuiRoot = Join-Path (Split-Path -Parent $driverRoot) 'cdpgui'
}
if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $OutputDirectory = $installerRoot
}
$driverOutput = Join-Path $driverRoot 'x64\Release'
$guiOutput = Join-Path $GuiRoot 'bin\x64\Release'
$guiExecutableCandidates = @(
    (Join-Path $guiOutput '源点恢复.exe'),
    (Join-Path $guiOutput 'CDPCorePro.exe')
)
$guiExecutable = $guiExecutableCandidates | Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } | Select-Object -First 1
$workRoot = Join-Path $installerRoot 'work'
$payloadRoot = Join-Path $workRoot 'payload'
$archivePath = Join-Path $workRoot 'payload.zip'
$runtimeInstaller = Join-Path $installerRoot 'VC_redist.x64.exe'
$outputPath = Join-Path $OutputDirectory 'RecoverySetup-x64.exe'
$localOutputPath = Join-Path $workRoot 'out\RecoverySetup-x64.exe'
$previousOutputPath = Join-Path $OutputDirectory '源点恢复安装程序-x64.exe'
$legacyOutputPath = Join-Path $OutputDirectory 'CdpDriverSetup-x64.exe'
$makensisCandidates = @(
    (Join-Path $installerRoot 'tools\nsis\makensis.exe'),
    'C:\Program Files (x86)\NSIS\makensis.exe',
    'C:\Program Files\NSIS\makensis.exe'
)
$makensis = $makensisCandidates | Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } | Select-Object -First 1

function Require-File([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "找不到发布文件: $Path。请先构建 x64 Release。"
    }
}

function Copy-ReleaseFile([string]$Source, [string]$Destination) {
    Require-File $Source
    $parent = Split-Path -Parent $Destination
    New-Item -ItemType Directory -Force -Path $parent | Out-Null
    Copy-Item -LiteralPath $Source -Destination $Destination -Force
}

if (-not $guiExecutable) {
    throw "找不到 GUI 发布文件。已检查: $($guiExecutableCandidates -join ', ')"
}
Require-File $runtimeInstaller

foreach ($file in @(
    $guiExecutable,
    (Join-Path $guiOutput 'handle.exe'),
    (Join-Path $guiOutput 'iscsi_target_dotnet.dll'),
    (Join-Path $driverOutput 'CdpDriver.cer'),
    (Join-Path $driverOutput 'driver\CdpDriver.inf'),
    (Join-Path $driverOutput 'driver\CdpDriver.sys'),
    (Join-Path $driverOutput 'driver\cdpdriver.cat')
)) { Require-File $file }

$msbuild = 'C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe'
Require-File $msbuild
$savedProcessPath = $env:PATH
try {
    # Some launchers provide both `Path` and `PATH` in the native environment
    # block. MSBuild's ToolTask imports variables into a case-insensitive
    # dictionary and then fails before CL.exe starts. Removing the inherited
    # entry for this child build avoids that duplicate; MSBuild resolves the
    # selected VC toolchain through its own absolute installation paths.
    Remove-Item Env:Path -ErrorAction SilentlyContinue
    & $msbuild (Join-Path $driverRoot 'CdpBootService\CdpBootService.vcxproj') /m /t:Build /p:Configuration=Release /p:Platform=x64 /v:minimal /nologo
    $bootServiceBuildExitCode = $LASTEXITCODE
    & $msbuild (Join-Path $installerRoot 'CdpDriverInstallHelper.vcxproj') /m /t:Build /p:Configuration=Release /p:Platform=x64 /v:minimal /nologo
    $helperBuildExitCode = $LASTEXITCODE
} finally {
    $env:Path = $savedProcessPath
}
if ($bootServiceBuildExitCode -ne 0) {
    throw "生成启动服务失败 (exit code $bootServiceBuildExitCode)。"
}
if ($helperBuildExitCode -ne 0) {
    throw "生成驱动安装助手失败 (exit code $helperBuildExitCode)。"
}
Require-File (Join-Path $driverOutput 'CdpBootService.exe')
Require-File (Join-Path $driverOutput 'CdpDriverInstallHelper.exe')

if (-not (Test-Path -LiteralPath (Join-Path $guiOutput 'Web') -PathType Container)) {
    throw "找不到 GUI Web 资源目录: $(Join-Path $guiOutput 'Web')"
}

Remove-Item -LiteralPath $workRoot -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path $payloadRoot | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $workRoot 'out') | Out-Null

Copy-ReleaseFile $guiExecutable (Join-Path $payloadRoot '源点恢复.exe')
Copy-ReleaseFile (Join-Path $guiOutput 'handle.exe') (Join-Path $payloadRoot 'handle.exe')
Copy-ReleaseFile (Join-Path $guiOutput 'iscsi_target_dotnet.dll') (Join-Path $payloadRoot 'iscsi_target_dotnet.dll')
Copy-Item -LiteralPath (Join-Path $guiOutput 'Web') -Destination (Join-Path $payloadRoot 'Web') -Recurse -Force
$guiFileVersion = [version](Get-Item -LiteralPath $guiExecutable).VersionInfo.FileVersion
$guiDisplayVersion = "v$($guiFileVersion.Major).$($guiFileVersion.Minor).$($guiFileVersion.Build)"
$payloadIndexPath = Join-Path $payloadRoot 'Web\index.html'
# Windows PowerShell 5.1 treats UTF-8 files without a BOM as ANSI when
# Get-Content has no explicit encoding.  Reading index.html that way corrupts
# every Chinese string before the version replacement.  Use the .NET UTF-8
# APIs so packaging behaves identically in Windows PowerShell and PowerShell 7.
$utf8NoBom = New-Object System.Text.UTF8Encoding($false, $true)
$payloadIndex = [System.IO.File]::ReadAllText($payloadIndexPath, $utf8NoBom)
if ($payloadIndex -notmatch '<span class="version">v[^<]+</span>') {
    throw "GUI 页面中找不到版本标签: $payloadIndexPath"
}
$payloadIndex = $payloadIndex -replace '<span class="version">v[^<]+</span>',
    "<span class=`"version`">$guiDisplayVersion</span>"
[System.IO.File]::WriteAllText($payloadIndexPath, $payloadIndex, $utf8NoBom)
$verifiedPayloadIndex = [System.IO.File]::ReadAllText($payloadIndexPath, $utf8NoBom)
if ($verifiedPayloadIndex -notmatch '<title>源点恢复</title>') {
    throw "GUI 页面 UTF-8 校验失败，已停止生成安装包: $payloadIndexPath"
}
Write-Host "GUI 页面版本已同步: $guiDisplayVersion"
Copy-ReleaseFile (Join-Path $driverOutput 'CdpBootService.exe') (Join-Path $payloadRoot 'CdpBootService.exe')
Copy-ReleaseFile (Join-Path $driverOutput 'CdpDriverInstallHelper.exe') (Join-Path $payloadRoot 'CdpDriverInstallHelper.exe')
Copy-ReleaseFile (Join-Path $driverOutput 'CdpDriver.cer') (Join-Path $payloadRoot 'driver\CdpDriver.cer')
Copy-ReleaseFile (Join-Path $driverOutput 'driver\CdpDriver.inf') (Join-Path $payloadRoot 'driver\CdpDriver.inf')
Copy-ReleaseFile (Join-Path $driverOutput 'driver\CdpDriver.sys') (Join-Path $payloadRoot 'driver\CdpDriver.sys')
Copy-ReleaseFile (Join-Path $driverOutput 'driver\cdpdriver.cat') (Join-Path $payloadRoot 'driver\CdpDriver.cat')

$payloadItems = Get-ChildItem -LiteralPath $payloadRoot
Compress-Archive -Path $payloadItems.FullName -DestinationPath $archivePath -CompressionLevel Optimal
Copy-Item -LiteralPath (Join-Path $GuiRoot 'CDPCorePro\res\app.ico') -Destination (Join-Path $workRoot 'app.ico') -Force

New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null

if (-not $makensis) {
    throw "找不到 NSIS 编译器。请安装 NSIS，或放到 installer\tools\nsis。"
}
& $makensis /INPUTCHARSET UTF8 /V3 (Join-Path $installerRoot 'CdpDriverSetup.nsi')
if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $localOutputPath -PathType Leaf)) {
    throw "生成 NSIS 安装包失败 (exit code $LASTEXITCODE)。"
}

$copyDeadline = (Get-Date).AddSeconds(30)
do {
    try {
        Copy-Item -LiteralPath $localOutputPath -Destination $outputPath -Force
        $copyError = $null
    } catch {
        $copyError = $_
        Start-Sleep -Seconds 1
    }
} while ($copyError -and (Get-Date) -lt $copyDeadline)
if ($copyError) {
    throw "无法替换安装包；请关闭正在运行的旧安装程序后重试。$copyError"
}
if (Test-Path -LiteralPath $legacyOutputPath -PathType Leaf) {
    Remove-Item -LiteralPath $legacyOutputPath -Force
}
if (Test-Path -LiteralPath $previousOutputPath -PathType Leaf) {
    Remove-Item -LiteralPath $previousOutputPath -Force
}

$excluded = @('CdpConsole.exe', 'CdpConsole_Param.exe', 'CdpCore.Tests.exe', 'VolHexdump.exe')
$zip = [System.IO.Compression.ZipFile]::OpenRead($archivePath)
$archiveEntries = $zip.Entries.FullName
$zip.Dispose()
$presentExcluded = $archiveEntries | Where-Object { $_ -in $excluded }
if ($presentExcluded) {
    throw "安装包包含不应发布的文件: $($presentExcluded -join ', ')"
}

Write-Host "安装包已生成: $outputPath"
