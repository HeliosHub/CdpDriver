<#
.SYNOPSIS
    Verify that the CdpDriver obfuscated build matches its design expectations.

.DESCRIPTION
    Why this script exists
    ----------------------
    CdpDriver obfuscates code only in Release|x64, using a custom LLVM toolchain
    (clang-cl + lld-link), and the per-unit passes are passed through MSBuild
    "AdditionalOptions" metadata. Two failure modes have already happened in this
    repository, and neither is caught by anything today:

      1) MSBuild batching silently drops one unit's AdditionalOptions.
         In obf-release-build.log (2026-09-22 14:28) CdpLicenseProtect.c was
         batched into CdpLicenseGate.c's command line and LOST its -fla, so the
         produced binary did not match the project file.

      2) After switching to lld-link, the protected sections lost
         IMAGE_SCN_MEM_NOT_PAGED (MSVC link.exe /DRIVER adds it, lld-link does
         not). lld-link also does not report this.

    This script treats the BUILD ARTIFACTS as the single source of truth and
    asserts, among other things:

      - every translation unit received exactly the pass set it should have
        (read from CdpDriver.tlog\clang-cl.command.1.tlog, i.e. what actually ran)
      - .licprot / .licpr are present, not writable, .licpr not executable,
        and their sizes are sane
      - string obfuscation really took effect (license strings must NOT be
        recoverable as plaintext from the .sys)
      - the protected sections exist in the final image (a missing .licprot makes
        the license gate silently fail closed)

    Exits non-zero on any failure, so it can be wired into a post-build step.

    NOTE: this file is deliberately ASCII-only. Windows PowerShell 5.1 reads
    BOM-less .ps1 files using the ANSI code page, so non-ASCII text here would be
    corrupted on machines with a different locale.

.PARAMETER ProjectDir
    CdpDriver project directory (the one containing CdpDriver.vcxproj).
    Defaults to the parent of this script's directory.

.PARAMETER Configuration
    Build configuration to inspect. Default: Release.

.PARAMETER Platform
    Build platform to inspect. Default: x64.

.PARAMETER SysPath
    Optional explicit path to the built .sys. When omitted, the usual output
    locations are probed. Useful for checking a deployed copy.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File script\verify_obfuscation.ps1

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File script\verify_obfuscation.ps1 `
        -SysPath x64\Release\driver\CdpDriver.sys
#>
[CmdletBinding()]
param(
    [string]$ProjectDir,
    [string]$Configuration = 'Release',
    [string]$Platform = 'x64',
    [string]$SysPath
)

$ErrorActionPreference = 'Stop'
# NOTE: Set-StrictMode is intentionally NOT used. This script has to run both
# standalone and dot-invoked from another scope (CI, post-build), where strict
# mode's behavior for uninitialised properties differs and produces aborts that
# are hard to attribute. Instead every collection access goes through @(...)
# explicitly, which is strict-safe on PowerShell 5.1 and 7+.

# --------------------------------------------------------------- expectations
# Pass set every translation unit MUST have received. Keys are source file names
# (case-insensitive). Section = $true means the unit must end up contributing to
# .licprot/.licpr, i.e. it includes CdpLicenseSeg.h and CdpLicenseSegEnd.h.
#
# When you change the obfuscation config in CdpDriver.vcxproj you MUST update
# this table too. That is the whole point: a dropped flag becomes a build
# failure instead of a discovery made months later during reverse engineering.
# Every obfuscated unit must also carry the reproducible-build seed, and all of
# them must carry the SAME value: a shared seed is what makes the obfuscation
# output stable, so a unit left with a different (or missing) seed silently
# breaks reproducibility and binary diffing.
# Read the value from the project file rather than hardcoding it, so rotating
# CdpObfSeed does not require editing this script.
$SeedPattern = '-mllvm\s+-aesSeed=([0-9A-Fa-f]{32})'

$ExpectedUnits = [ordered]@{
    'CdpLocalSeal.c'       = @{ Passes = @('-string-obfus','-const-obfus','-ind-call','-fla'); Section = $true }
    'CdpLicenseTrust.c'    = @{ Passes = @('-string-obfus','-const-obfus','-ind-call','-fla'); Section = $true }
    'CdpLicenseProtect.c'  = @{ Passes = @('-string-obfus','-const-obfus','-ind-call','-fla'); Section = $true }
    'CdpLicenseGate.c'     = @{ Passes = @('-string-obfus','-const-obfus','-ind-call','-fla'); Section = $true }
    'CdpLicenseGateCall.c' = @{ Passes = @('-string-obfus','-const-obfus','-fla');            Section = $true }
    'CdpLicenseHw.c'       = @{ Passes = @('-string-obfus','-const-obfus','-ind-call','-fla'); Section = $true }
    'CdpLicenseCodec.c'    = @{ Passes = @('-string-obfus','-const-obfus','-ind-call','-fla'); Section = $true }
    # Obfuscated, but intentionally NOT in .licprot: not on a license decision point.
    'CdpCredential.c'      = @{ Passes = @('-string-obfus','-const-obfus','-ind-call','-fla'); Section = $false }
    'CdpIoctlGuard.c'      = @{ Passes = @('-string-obfus','-const-obfus','-ind-call','-fla'); Section = $false }
    'CdpJournalCodec.c'    = @{ Passes = @('-string-obfus','-const-obfus','-ind-call','-fla'); Section = $false }
    'cdp_view_policy.c'    = @{ Passes = @('-string-obfus','-const-obfus','-ind-call','-fla'); Section = $false }
    # Intentionally unobfuscated: I/O hot paths (see CdpLicenseSeg.h:16-17).
    'Driver.c'             = @{ Passes = @(); Section = $false }
    'CdpIrpDispatchs.c'    = @{ Passes = @(); Section = $false }
    'CdpJournal.c'         = @{ Passes = @(); Section = $false }
    'cdp_core.c'           = @{ Passes = @(); Section = $false }
    'cdp_dev_store.c'      = @{ Passes = @(); Section = $false }
}

# License-path strings that must NOT survive as plaintext in the loaded code/data
# when -string-obfus works:
#   cdp-cap-v1 / cdp-rk / e0-iv   <- CdpLocalSeal.c
#   'vendor blob ...'             <- CdpLicenseTrust.c
#   canonical JSON field names    <- CdpLicenseCodec.c
#
# NOTE: '.licprot' / '.licpr' are deliberately NOT in this list. They are section
# names and therefore appear verbatim in the PE section header table; that is
# unavoidable and is not a string-obfuscation failure.
$ForbiddenPlaintext = @(
    'cdp-cap-v1',
    'cdp-rk',
    'e0-iv',
    'vendor blob header invalid',
    'device_id_hash',
    'signing_key_id',
    'issued_at_server'
)

# Sections whose file-backed bytes get scanned for forbidden plaintext. PE
# headers (and thus section names) are intentionally excluded.
$ScannedSections = @('.text', '.rdata', '.data', '.pdata', '.licprot', '.licpr', 'INIT')

# ---------------------------------------------------------------- toolchain
# The OLLVM/xVMP fork at this path had a broken readAnnotate() that made every
# annotation-driven pass silently inert (including the -fla "nofla" opt-out).
# It was fixed in llvm/lib/Obfuscation/Utils.cpp and the toolchain rebuilt, but
# the toolchain is NOT a git checkout, so a naive rebuild of it would silently
# regenerate a BROKEN compiler with no visible symptom until someone relies on
# an annotation. The check below fails loudly if the built Utils.obj is older
# than Utils.cpp (i.e. the source was edited but the toolchain never rebuilt).
$ToolchainObfDir = 'E:\llvm-msvc-ex-2026-7-23\build-release-x64-obf\lib\Obfuscation'
$ToolchainUtilsCpp = 'E:\llvm-msvc-ex-2026-7-23\llvm\lib\Obfuscation\Utils.cpp'
$ToolchainClangCl = 'E:\llvm-msvc-ex-2026-7-23\build-release-x64-obf\Release\bin\clang-cl.exe'
# The pre-fix clang-cl.exe was exactly 74,411,520 bytes (measured before the fix).
# The patched build is larger; used only as a weak sanity signal.
$ToolchainMinExeBytes = 74411520

# Which units are expected to emit .licpr (the read-only constant section).
# Only these two own protected const data: the RSA public-key shards + PSS salt,
# and the PRODUCT_MAGIC / ARX constant shards. Everything else must stay out, or
# the integrity-hashed constant range grows silently.
$LicprContributors = @('CdpLocalSeal.c', 'CdpLicenseTrust.c')

# .licpr should only carry PRODUCT_MAGIC / ARX constants / public-key shards.
# Baseline measured 2026-09-22: Trust 576 B + LocalSeal 48 B = 624 B.
# The ceiling below leaves headroom; exceeding it means a new const global was
# pulled into the protected constant section (usually a new static const array).
$LicprWarnBytes  = 1024
$LicprotMinBytes = 16 * 1024   # much smaller than this means protected code is missing

# ---------------------------------------------------------------- helpers
$script:Failures = New-Object System.Collections.Generic.List[string]
$script:Warnings = New-Object System.Collections.Generic.List[string]
# seed value -> list of units that used it; must end up with exactly one key
$script:SeedSeen = @{}

function Add-Failure { param([string]$Message) $script:Failures.Add($Message) | Out-Null }
function Add-Warning { param([string]$Message) $script:Warnings.Add($Message) | Out-Null }
function Write-Head   { param([string]$Title) Write-Host ''; Write-Host "== $Title" -ForegroundColor Cyan }

function Read-TextFileAuto {
    param([string]$Path)
    $bytes = [System.IO.File]::ReadAllBytes($Path)
    $text = $null
    if ($bytes.Length -ge 2 -and $bytes[0] -eq 0xFF -and $bytes[1] -eq 0xFE) {
        $text = [System.Text.Encoding]::Unicode.GetString($bytes)
    } elseif ($bytes.Length -ge 3 -and $bytes[0] -eq 0xEF -and $bytes[1] -eq 0xBB -and $bytes[2] -eq 0xBF) {
        $text = [System.Text.Encoding]::UTF8.GetString($bytes, 3, $bytes.Length - 3)
    } else {
        $text = [System.Text.Encoding]::UTF8.GetString($bytes)
    }
    # Strip a leading BOM char if the encoding detection above did not consume it.
    return $text.TrimStart([char]0xFEFF)
}

function Get-PassesFromCommandLine {
    param([string]$CommandLine)
    $result = @()
    foreach ($m in [regex]::Matches($CommandLine, '-mllvm\s+(-[A-Za-z0-9\-]+)')) {
        $result += $m.Groups[1].Value
    }
    return @($result)
}

function Read-UInt32 {
    param([byte[]]$Bytes, [int]$Offset)
    # Cast to UInt32 BEFORE shifting: PowerShell's -shl keeps the left operand's
    # type, so a [byte] shifted by 8 would wrap to 0.
    return [uint32](
        ([uint32]$Bytes[$Offset]) -bor
        (([uint32]$Bytes[$Offset+1]) -shl 8) -bor
        (([uint32]$Bytes[$Offset+2]) -shl 16) -bor
        (([uint32]$Bytes[$Offset+3]) -shl 24))
}

function Get-PeSections {
    param([string]$Path)
    $bytes = [System.IO.File]::ReadAllBytes($Path)
    if ($bytes.Length -lt 0x40) { throw "File too small to be a PE image: $Path" }

    $peOffset = [int](Read-UInt32 -Bytes $bytes -Offset 0x3C)
    if ($peOffset -le 0 -or ($peOffset + 24) -ge $bytes.Length) { throw "Invalid e_lfanew: $peOffset" }
    if (-not ($bytes[$peOffset] -eq 0x50 -and $bytes[$peOffset+1] -eq 0x45)) { throw "Missing PE signature" }

    $numSections = [int]($bytes[$peOffset+6] -bor ($bytes[$peOffset+7] -shl 8))
    $optHdrSize  = [int]($bytes[$peOffset+20] -bor ($bytes[$peOffset+21] -shl 8))
    $secTableOff = $peOffset + 24 + $optHdrSize

    $sections = @()
    for ($i = 0; $i -lt $numSections; $i++) {
        $off = $secTableOff + ($i * 40)
        if (($off + 40) -gt $bytes.Length) { throw "Section table runs past end of file" }
        $raw = New-Object byte[] 8
        [Array]::Copy($bytes, $off, $raw, 0, 8)
        $sections += [pscustomobject]@{
            Name              = ([System.Text.Encoding]::ASCII.GetString($raw)).TrimEnd([char]0)
            VirtualSize       = Read-UInt32 -Bytes $bytes -Offset ($off + 8)
            VirtualAddress    = Read-UInt32 -Bytes $bytes -Offset ($off + 12)
            SizeOfRawData     = Read-UInt32 -Bytes $bytes -Offset ($off + 16)
            PointerToRawData  = Read-UInt32 -Bytes $bytes -Offset ($off + 20)
            Characteristics   = Read-UInt32 -Bytes $bytes -Offset ($off + 36)
        }
    }
    return [pscustomobject]@{ Bytes = $bytes; Sections = $sections }
}

function Get-CoffSections {
    <#
      Read the section headers of a COFF object file (.obj).

      This is how the protection boundary is proven end to end: the per-unit pass
      check shows which flags were passed, but only this shows which units
      actually emitted .licprot/.licpr (i.e. actually included CdpLicenseSeg.h
      before their code). A unit can receive every obfuscation flag and still
      contribute nothing to the integrity-hashed range if it forgot the segment
      include -- which is exactly the class of bug that left CdpLicenseCodec.c
      outside the protected sections.
    #>
    param([string]$Path)

    $bytes = [System.IO.File]::ReadAllBytes($Path)
    if ($bytes.Length -lt 20) { throw "File too small to be a COFF object: $Path" }

    # COFF file header: Machine(2) NumberOfSections(2) TimeDateStamp(4)
    # PointerToSymbolTable(4) NumberOfSymbols(4) SizeOfOptionalHeader(2) Characteristics(2)
    $numSections = [int]($bytes[2] -bor ($bytes[3] -shl 8))
    $optHdrSize  = [int]($bytes[16] -bor ($bytes[17] -shl 8))
    $secTableOff = 20 + $optHdrSize

    $sections = @()
    for ($i = 0; $i -lt $numSections; $i++) {
        $off = $secTableOff + ($i * 40)
        if (($off + 40) -gt $bytes.Length) { break }
        $raw = New-Object byte[] 8
        [Array]::Copy($bytes, $off, $raw, 0, 8)
        $sections += [pscustomobject]@{
            Name            = ([System.Text.Encoding]::ASCII.GetString($raw)).TrimEnd([char]0)
            # COFF section header layout (40 bytes):
            #   +0 Name(8) +8 PhysicalAddress(4) +12 VirtualAddress(4)
            #   +16 SizeOfRawData(4) +20 PointerToRawData(4) ... +36 Characteristics(4)
            # The size of an initialised data section lives at +16; there is no
            # VirtualSize in a COFF object (that field only exists in a linked
            # PE image, where it is image-relative).
            Size            = Read-UInt32 -Bytes $bytes -Offset ($off + 16)
            Characteristics = Read-UInt32 -Bytes $bytes -Offset ($off + 36)
        }
    }
    return $sections
}

function Invoke-MainStepCheckSections {
    param([string]$ObjDir, [string]$ExpectedName)

    Write-Head "4. Which objects actually emit $ExpectedName (COFF evidence)"

    if (-not (Test-Path -LiteralPath $ObjDir)) {
        Add-Failure "object directory not found: $ObjDir"
        return
    }

    $contributors = @()
    $sizes = @{}

    foreach ($entry in $ExpectedUnits.GetEnumerator()) {
        $src = [string]$entry.Key
        $exp = $entry.Value
        $obj = Join-Path $ObjDir ([System.IO.Path]::GetFileNameWithoutExtension($src) + '.obj')

        if (-not (Test-Path -LiteralPath $obj)) {
            Add-Failure "object not built, cannot verify section membership: $obj"
            continue
        }

        $sec   = @(Get-CoffSections -Path $obj | Where-Object { $_.Name -eq $ExpectedName })
        $emits = ($sec.Count -gt 0)
        $size  = 0
        if ($emits) { $size = ($sec | Measure-Object -Property Size -Sum).Sum }

        # For .licpr the expectation is a short explicit list; for .licprot it is
        # the Section flag in the expectations table above.
        $shouldEmit = if ($ExpectedName -eq '.licpr') {
            ($LicprContributors -contains $src)
        } else {
            [bool]$exp.Section
        }

        if ($shouldEmit -and -not $emits) {
            Add-Failure "$src should contribute to $ExpectedName but its .obj does not emit it (missing CdpLicenseSeg.h include?)"
            Write-Host ("  FAIL {0,-22} expected $ExpectedName, emits nothing" -f $src) -ForegroundColor Red
        } elseif ((-not $shouldEmit) -and $emits) {
            Add-Failure "$src is NOT expected to contribute to $ExpectedName but its .obj emits it ($size bytes)"
            Write-Host ("  FAIL {0,-22} unexpected $ExpectedName ($size bytes)" -f $src) -ForegroundColor Red
        } else {
            $note = if ($emits) { "$ExpectedName = $size bytes" } else { "no $ExpectedName (as designed)" }
            Write-Host ("  OK   {0,-22} {1}" -f $src, $note) -ForegroundColor Green
        }
        if ($emits) { $contributors += $src; $sizes[$src] = $size }
    }

    if ($contributors.Count -gt 0) {
        $total = ($sizes.Values | Measure-Object -Sum).Sum
        Write-Host ''
        Write-Host ("  contributors: {0}" -f ($contributors -join ', '))
        Write-Host ("  sum of their {0} sizes = {1:N0} bytes" -f $ExpectedName, $total)
    }
}

# NOTE: named Invoke-MainStep* rather than verb-noun to keep the analyzer quiet.
function Invoke-MainStepCheckToolchain {
    <#
      Verify the obfuscating toolchain was built from the PATCHED sources.

      Why this matters: the fix to readAnnotate() lives in llvm/lib/Obfuscation/
      Utils.cpp inside the LLVM fork, which is not under version control here. If
      someone edits that source and forgets to rebuild, or rebuilds from a clean
      tree that lost the patch, the resulting compiler is silently broken in a
      way that only shows up when an annotation is used. That is exactly the
      failure this whole exercise started from, so it gets a check.
    #>
    Write-Head '5. Obfuscating toolchain provenance'

    if (-not (Test-Path -LiteralPath $ToolchainUtilsCpp)) {
        Add-Warning "toolchain source not found: $ToolchainUtilsCpp (cannot verify it is patched)"
        return
    }

    $utilsObj = @(Get-ChildItem $ToolchainObfDir -Recurse -Filter 'Utils.obj' -ErrorAction SilentlyContinue) |
        Sort-Object LastWriteTime -Descending | Select-Object -First 1

    if (-not $utilsObj) {
        Add-Warning "built Utils.obj not found under $ToolchainObfDir (cannot verify toolchain freshness)"
    } else {
        $srcTime = (Get-Item -LiteralPath $ToolchainUtilsCpp).LastWriteTime
        $objTime = $utilsObj.LastWriteTime
        Write-Host ("  Utils.cpp : {0}" -f $srcTime.ToString('yyyy-MM-dd HH:mm:ss'))
        Write-Host ("  Utils.obj : {0}" -f $objTime.ToString('yyyy-MM-dd HH:mm:ss'))
        if ($objTime -lt $srcTime) {
            Add-Failure "Toolchain is STALE: Utils.obj is older than Utils.cpp. Rebuild it, or the annotation fix is not in the compiler in use."
        } else {
            Write-Host '  OK   toolchain object is newer than its source' -ForegroundColor Green
        }
    }

    $clangExe = $ToolchainClangCl
    if (Test-Path -LiteralPath $clangExe) {
        $len = (Get-Item -LiteralPath $clangExe).Length
        Write-Host ("  clang-cl.exe : {0:N0} bytes, {1}" -f $len, (Get-Item -LiteralPath $clangExe).LastWriteTime.ToString('yyyy-MM-dd HH:mm:ss'))
        if ($len -le $ToolchainMinExeBytes) {
            Add-Warning "clang-cl.exe is not larger than the known pre-fix size ($ToolchainMinExeBytes) -- confirm the readAnnotate fix is present"
        }
    } else {
        Add-Warning "clang-cl.exe not found at $clangExe"
    }
}

function Invoke-MainStepCheckFlags {
    param([string]$TlogPath)

    Write-Head '1. Per-unit obfuscation passes (from clang-cl.command.1.tlog)'
    if (-not (Test-Path -LiteralPath $TlogPath)) {
        Add-Failure "Missing compile command log: $TlogPath (build Release|$Platform once, then re-run)"
        return
    }

    $lines = (Read-TextFileAuto -Path $TlogPath) -split "`r?`n"

    # tlog layout: a source-file line followed by its argument list. The argument
    # list does NOT repeat the tool name, so it cannot be matched against 'clang';
    # it is recognised by the leading /c (compile-only) switch instead.
    $bySource = @{}
    for ($i = 0; $i -lt ($lines.Count - 1); $i++) {
        $line = $lines[$i].Trim()
        if ($line -notmatch '^\^?.+\.c$') { continue }
        if ($line.Contains(' ')) { continue }

        $src = $line.TrimStart('^')
        $slash = [Math]::Max($src.LastIndexOf('\'), $src.LastIndexOf('/'))
        if ($slash -ge 0) { $src = $src.Substring($slash + 1) }

        $next = $lines[$i + 1]
        if ($next -and ($next -match '(^|\s)/c(\s|$)')) {
            $bySource[$src.ToLowerInvariant()] = $next
        }
    }

    if (@($bySource.Keys).Count -eq 0) {
        Add-Failure "No clang command lines parsed from $TlogPath (tlog format may have changed)"
        return
    }

    foreach ($entry in $ExpectedUnits.GetEnumerator()) {
        $src = [string]$entry.Key
        $exp = $entry.Value
        $key = $src.ToLowerInvariant()

        if (-not $bySource.ContainsKey($key)) {
            Add-Failure "$src is absent from the tlog (was it compiled?)"
            Write-Host ("  FAIL {0,-22} not compiled" -f $src) -ForegroundColor Red
            continue
        }

        # Separate the seed from the pass list first: -aesSeed carries a value, so
        # it does not fit the "-mllvm -<flag>" shape used for the pass flags.
        $cmdNoSeed = $bySource[$key] -replace '-mllvm\s+-aesSeed=[0-9A-Fa-f]*', ''
        $actual  = @(Get-PassesFromCommandLine -CommandLine $cmdNoSeed)
        $missing = @($exp.Passes | Where-Object { $actual -notcontains $_ })
        $extra   = @($actual      | Where-Object { $exp.Passes -notcontains $_ })
        $nMissing = $missing.Count
        $nExtra   = $extra.Count

        # Seed: required only for obfuscated units, and must match across all of
        # them (a shared value is what makes obfuscation output stable).
        if ($exp.Passes.Count -gt 0) {
            $seedMatch = [regex]::Match($bySource[$key], $SeedPattern)
            if (-not $seedMatch.Success) {
                Add-Failure "$src has no -mllvm -aesSeed -- this unit's obfuscation output is NOT reproducible"
                Write-Host ("  FAIL {0,-22} missing -aesSeed" -f $src) -ForegroundColor Red
                continue
            }
            $seed = $seedMatch.Groups[1].Value.ToUpperInvariant()
            if (-not $script:SeedSeen.ContainsKey($seed)) {
                $script:SeedSeen[$seed] = @()
            }
            $script:SeedSeen[$seed] += $src
        }

        if ($nMissing -eq 0 -and $nExtra -eq 0) {
            $shown = if ($actual.Count -gt 0) { $actual -join ' ' } else { '(none)' }
            Write-Host ("  OK   {0,-22} {1}" -f $src, $shown) -ForegroundColor Green
        } else {
            $parts = @()
            if ($nMissing -gt 0) { $parts += "missing: $($missing -join ' ')" }
            if ($nExtra   -gt 0) { $parts += "unexpected: $($extra -join ' ')" }
            $detail = $parts -join '; '
            Add-Failure "$src pass mismatch -- $detail"
            Write-Host ("  FAIL {0,-22} {1}" -f $src, $detail) -ForegroundColor Red
        }
    }

    # Seed consistency: exactly one distinct seed across all obfuscated units.
    if ($script:SeedSeen.Keys.Count -eq 0) {
        Add-Failure 'no obfuscated unit carried -aesSeed (reproducibility lost)'
    } elseif ($script:SeedSeen.Keys.Count -gt 1) {
        foreach ($k in $script:SeedSeen.Keys) {
            Add-Failure ("seed {0} used by: {1}" -f $k, ($script:SeedSeen[$k] -join ', '))
        }
        Write-Host '  FAIL mixed obfuscation seeds across units' -ForegroundColor Red
    } else {
        $only = @($script:SeedSeen.Keys)[0]
        $n = @($script:SeedSeen[$only]).Count
        Write-Host ("  OK   consistent -aesSeed={0} across {1} obfuscated units" -f $only, $n) -ForegroundColor Green
        Write-Host '       (note: the shipped toolchain still produces byte-different output per'
        Write-Host '        build because StringObfuscation owns an unseeded CryptoUtils; see'
        Write-Host '        OBFUSCATION_ANALYSIS.md section on reproducibility)'
    }
}

function Invoke-MainStepCheckImage {
    param([string]$Path)

    Write-Head '2. Final image: protected sections'
    $pe = Get-PeSections -Path $Path
    Write-Host "  Image : $Path"
    Write-Host ("  Size  : {0:N0} bytes" -f $pe.Bytes.Length)

    # Materialise the two section objects as scalars up front. Empty arrays are
    # handled by an explicit null check rather than .Count.
    $licprotObj = @($pe.Sections | Where-Object { $_.Name -eq '.licprot' })[0]
    $licprObj   = @($pe.Sections | Where-Object { $_.Name -eq '.licpr' })[0]

    if (-not $licprotObj) {
        Add-Failure 'No .licprot section in the final image -- license integrity checks will silently fail closed'
    }
    if (-not $licprObj) {
        Add-Warning 'No .licpr section in the final image (acceptable only if no const globals remain)'
    }

    foreach ($sec in $pe.Sections) {
        Write-Host ("  {0,-9} VSize={1,-8} Raw={2,-8} Ptr={3,-8}" -f `
            $sec.Name, $sec.VirtualSize, $sec.SizeOfRawData, $sec.PointerToRawData)
    }
    Write-Host ''

    foreach ($sec in @($licprotObj, $licprObj)) {
        if (-not $sec) { continue }
        $chars      = [uint32]$sec.Characteristics
        $writable   = ($chars -band 0x80000000) -ne 0   # IMAGE_SCN_MEM_WRITE
        $executable = ($chars -band 0x20000000) -ne 0   # IMAGE_SCN_MEM_EXECUTE
        $notPaged   = ($chars -band 0x08000000) -ne 0   # IMAGE_SCN_MEM_NOT_PAGED

        Write-Host ("  {0,-9} VSize={1,-8} Raw={2,-8} Chars=0x{3:X8}" -f `
            $sec.Name, $sec.VirtualSize, $sec.SizeOfRawData, $chars)

        if ($writable) {
            Add-Failure "$($sec.Name) is writable -- obfuscation/integrity sections must never be writable"
        }
        if ($sec.Name -eq '.licpr' -and $executable) {
            Add-Failure '.licpr is executable -- a read-only constant section must not be executable'
        }
        if (-not $notPaged) {
            Add-Warning "$($sec.Name) lacks IMAGE_SCN_MEM_NOT_PAGED (lld-link does not add it, unlike link.exe /DRIVER). Checks currently run at PASSIVE_LEVEL so this is latent, but a DPC/ISR path reaching the gate would page fault."
        }
    }

    if ($licprotObj -and $licprotObj.VirtualSize -lt $LicprotMinBytes) {
        Add-Failure (".licprot is only {0:N0} bytes (< {1:N0}) -- protected units probably did not make it into the section" -f `
            $licprotObj.VirtualSize, $LicprotMinBytes)
    }
    if ($licprObj) {
        if ($licprObj.VirtualSize -gt $LicprWarnBytes) {
            Add-Warning (".licpr is {0:N0} bytes (> {1:N0}) -- a new const global landed in the protected constant section; confirm this is intended" -f `
                $licprObj.VirtualSize, $LicprWarnBytes)
        } else {
            Write-Host ("  .licpr size {0} bytes is within the expected range" -f $licprObj.VirtualSize) -ForegroundColor Green
        }
    }

    Write-Head '3. String obfuscation effectiveness (license strings must not be plaintext)'
    # Build a buffer from the file-backed bytes of the loaded sections only, so
    # that section NAMES in the PE header table are not mistaken for leaked
    # string literals.
    $scanned = New-Object System.Collections.Generic.List[byte]
    $scannedNames = @()
    foreach ($sec in $pe.Sections) {
        if ($ScannedSections -notcontains $sec.Name) { continue }
        $rawSize = [int]$sec.SizeOfRawData
        if ($rawSize -le 0) { continue }
        $ptr = [int]$sec.PointerToRawData
        if ((($ptr + $rawSize) -gt $pe.Bytes.Length) -or $ptr -lt 0) { continue }
        $scanned.AddRange([byte[]]($pe.Bytes[$ptr..($ptr + $rawSize - 1)]))
        $scannedNames += $sec.Name
    }
    Write-Host ("  Scanning {0:N0} bytes from: {1}" -f $scanned.Count, ($scannedNames -join ' '))

    $text  = [System.Text.Encoding]::ASCII.GetString($scanned.ToArray())
    $upper = $text.ToUpperInvariant()
    foreach ($needle in $ForbiddenPlaintext) {
        if ($upper.Contains($needle.ToUpperInvariant())) {
            Add-Failure "Plaintext still present: '$needle' -- -string-obfus did not take effect for its unit"
            Write-Host "  FAIL hidden: '$needle'" -ForegroundColor Red
        } else {
            Write-Host "  OK   hidden: '$needle'" -ForegroundColor Green
        }
    }
}

# ---------------------------------------------------------------- main
if (-not $ProjectDir) {
    $ProjectDir = Split-Path -Parent $PSScriptRoot
}
if (-not (Test-Path -LiteralPath $ProjectDir)) {
    Write-Host "Project directory not found: $ProjectDir" -ForegroundColor Red
    exit 2
}
$ProjectDir = (Resolve-Path -LiteralPath $ProjectDir).Path
$vcxproj    = Join-Path $ProjectDir 'CdpDriver.vcxproj'

Write-Host 'CdpDriver obfuscated-build verification' -ForegroundColor White
Write-Host "  Project : $vcxproj"
Write-Host "  Config  : $Configuration|$Platform"

if (-not (Test-Path -LiteralPath $vcxproj)) {
    Add-Failure "Project file not found: $vcxproj"
}

Invoke-MainStepCheckFlags -TlogPath (Join-Path $ProjectDir "x64\$Configuration\CdpDriver.tlog\clang-cl.command.1.tlog")

Write-Head '2. Locating final image'
if (-not $SysPath) {
    $candidates = @(
        (Join-Path $ProjectDir "x64\$Configuration\CdpDriver.sys"),
        (Join-Path $ProjectDir "..\x64\$Configuration\driver\CdpDriver.sys"),
        (Join-Path $ProjectDir "x64\$Configuration\CdpDriver\CdpDriver.sys")
    )
    $SysPath = @($candidates | Where-Object { Test-Path -LiteralPath $_ })[0]
}

if (-not $SysPath) {
    Add-Failure 'No built CdpDriver.sys found (pass -SysPath to point at one)'
} elseif (-not (Test-Path -LiteralPath $SysPath)) {
    Add-Failure "Specified .sys does not exist: $SysPath"
} else {
    Invoke-MainStepCheckImage -Path (Resolve-Path -LiteralPath $SysPath).Path
}

# Section membership has to be checked per object file, not from the linked
# image: once linked, .licprot is a single anonymous blob and we can no longer
# tell which translation unit put code there.
# Object files land in <project>\x64\<Config>\ (the tlog lives one level deeper).
$objDir = Join-Path $ProjectDir "x64\$Configuration"
Invoke-MainStepCheckSections -ObjDir $objDir -ExpectedName '.licprot'
Invoke-MainStepCheckSections -ObjDir $objDir -ExpectedName '.licpr'

# Toolchain provenance last: it is independent of the driver artifacts and only
# needs checking once per machine, but it guards against silently regressing to
# a compiler whose annotation support is broken.
Invoke-MainStepCheckToolchain

Write-Head 'Summary'
if ($script:Warnings.Count -gt 0) {
    Write-Host "Warnings: $($script:Warnings.Count)" -ForegroundColor Yellow
    foreach ($w in $script:Warnings) { Write-Host "  - $w" -ForegroundColor Yellow }
}
if ($script:Failures.Count -gt 0) {
    Write-Host "Failures: $($script:Failures.Count)" -ForegroundColor Red
    foreach ($f in $script:Failures) { Write-Host "  - $f" -ForegroundColor Red }
    Write-Host ''
    Write-Host 'Obfuscated-build verification FAILED.' -ForegroundColor Red
    exit 1
}

Write-Host ("Obfuscated-build verification PASSED ({0} units, {1} plaintext checks)." -f `
    $ExpectedUnits.Count, $ForbiddenPlaintext.Count) -ForegroundColor Green
exit 0
