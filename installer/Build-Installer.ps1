[CmdletBinding()]
param(
    [string]$GuiRoot = 'C:\Users\Administrator\Desktop\cdpgui',
    [string]$OutputDirectory = 'C:\Users\Administrator\Desktop\cdpgui\bin'
)

$ErrorActionPreference = 'Stop'

$installerRoot = Split-Path -Parent $PSCommandPath
$driverRoot = Split-Path -Parent $installerRoot
$driverOutput = Join-Path $driverRoot 'x64\Release'
$guiOutput = Join-Path $GuiRoot 'bin\x64\Release'
$workRoot = Join-Path $installerRoot 'work'
$payloadRoot = Join-Path $workRoot 'payload'
$archivePath = Join-Path $workRoot 'payload.zip'
$outputPath = Join-Path $OutputDirectory 'CdpDriverSetup-x64.exe'
$localOutputPath = Join-Path $workRoot 'out\CdpDriverSetup-x64.exe'
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

foreach ($file in @(
    (Join-Path $guiOutput 'CDPCorePro.exe'),
    (Join-Path $guiOutput 'handle.exe'),
    (Join-Path $guiOutput 'iscsi_target_dotnet.dll'),
    (Join-Path $driverOutput 'CdpBootService.exe'),
    (Join-Path $driverOutput 'CdpDriver.cer'),
    (Join-Path $driverOutput 'driver\CdpDriver.inf'),
    (Join-Path $driverOutput 'driver\CdpDriver.sys'),
    (Join-Path $driverOutput 'driver\cdpdriver.cat')
)) { Require-File $file }

$msbuild = 'C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe'
Require-File $msbuild
& $msbuild (Join-Path $installerRoot 'CdpDriverInstallHelper.vcxproj') /m /t:Build /p:Configuration=Release /p:Platform=x64 /v:minimal /nologo
if ($LASTEXITCODE -ne 0) {
    throw "生成驱动安装助手失败 (exit code $LASTEXITCODE)。"
}
Require-File (Join-Path $driverOutput 'CdpDriverInstallHelper.exe')

if (-not (Test-Path -LiteralPath (Join-Path $guiOutput 'Web') -PathType Container)) {
    throw "找不到 GUI Web 资源目录: $(Join-Path $guiOutput 'Web')"
}

Remove-Item -LiteralPath $workRoot -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path $payloadRoot | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $workRoot 'out') | Out-Null

Copy-ReleaseFile (Join-Path $guiOutput 'CDPCorePro.exe') (Join-Path $payloadRoot 'CDPCorePro.exe')
Copy-ReleaseFile (Join-Path $guiOutput 'handle.exe') (Join-Path $payloadRoot 'handle.exe')
Copy-ReleaseFile (Join-Path $guiOutput 'iscsi_target_dotnet.dll') (Join-Path $payloadRoot 'iscsi_target_dotnet.dll')
Copy-Item -LiteralPath (Join-Path $guiOutput 'Web') -Destination (Join-Path $payloadRoot 'Web') -Recurse -Force
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

$excluded = @('CdpConsole.exe', 'CdpConsole_Param.exe', 'CdpCore.Tests.exe', 'VolHexdump.exe')
$zip = [System.IO.Compression.ZipFile]::OpenRead($archivePath)
$archiveEntries = $zip.Entries.FullName
$zip.Dispose()
$presentExcluded = $archiveEntries | Where-Object { $_ -in $excluded }
if ($presentExcluded) {
    throw "安装包包含不应发布的文件: $($presentExcluded -join ', ')"
}

Write-Host "安装包已生成: $outputPath"
