# Build kmbugcheck.sys (x64, kernel mode) with the preinstalled WDK.
#
# Assumes the MSVC environment is already loaded (ilammy/msvc-dev-cmd) and the
# WDK 10.0.* is installed under "C:\Program Files (x86)\Windows Kits\10".
#
# Include/lib paths are passed through the INCLUDE/LIB environment variables so
# the spaces in "Program Files (x86)" do not have to survive cl/link argument
# quoting.
#
# Usage:
#   pwsh scripts/build-driver.ps1 -Source driver\kmbugcheck.c -Out build\kmbugcheck.sys

param(
    [Parameter(Mandatory = $true)][string]$Source,
    [Parameter(Mandatory = $true)][string]$Out
)

$ErrorActionPreference = "Stop"

$kitsRoot = "C:\Program Files (x86)\Windows Kits\10"
if (-not (Test-Path $kitsRoot)) {
    throw "Windows Kits root not found: $kitsRoot"
}

# Pick the Include version dir that actually ships the kernel headers; the
# highest-numbered SDK dir may only carry user-mode headers.
$candidates = Get-ChildItem "$kitsRoot\Include" -Directory |
    Where-Object { $_.Name -match '^10\.0\.\d+\.0$' } |
    Sort-Object Name -Descending

$incDir = $null
foreach ($d in $candidates) {
    if (Test-Path (Join-Path $d.FullName "km\ntddk.h")) { $incDir = $d; break }
}
if (-not $incDir) {
    Write-Host "Include candidates:"
    $candidates | ForEach-Object { Write-Host "  $($_.FullName)" }
    throw "WDK kernel headers (km\ntddk.h) not found under $kitsRoot"
}

$sdkVer = $incDir.Name
$kmInc  = Join-Path $incDir.FullName "km"
$crtInc = Join-Path $incDir.FullName "crt"
$shInc  = Join-Path $incDir.FullName "shared"
$kmLib  = Join-Path $kitsRoot "Lib\$sdkVer\km\x64"

if (-not (Test-Path $kmLib)) {
    throw "WDK kernel lib dir not found: $kmLib"
}

Write-Host "WDK SDK version : $sdkVer"
Write-Host "km include      : $kmInc"
Write-Host "km lib          : $kmLib"

$outDir = Split-Path -Parent $Out
if (-not (Test-Path $outDir)) { New-Item -ItemType Directory -Force $outDir | Out-Null }
$obj = Join-Path $outDir "kmbugcheck.obj"

$env:INCLUDE = "$kmInc;$crtInc;$shInc"
$env:LIB = $kmLib

# Paths here are relative and contain no spaces; pass bare tokens so pwsh does
# not leave literal double quotes in the cl/link command line.
& cl /nologo /c /kernel /GS- /Gy /W3 `
    /D_AMD64_ /D_X86_AMD64_ /D_KERNEL_MODE `
    $Source /Fo$obj
if ($LASTEXITCODE -ne 0) { throw "cl failed with $LASTEXITCODE" }

& link /NOLOGO /DRIVER /SUBSYSTEM:NATIVE /ENTRY:DriverEntry `
    /MACHINE:X64 /NODEFAULTLIB $obj /OUT:$Out ntoskrnl.lib
if ($LASTEXITCODE -ne 0) { throw "link failed with $LASTEXITCODE" }

if (-not (Test-Path $Out)) { throw "driver output missing: $Out" }
Write-Host "driver built: $Out ($((Get-Item $Out).Length) bytes)"
