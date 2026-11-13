# Build kmbugcheck.sys (x64, kernel mode) with the preinstalled WDK.
#
# Assumes the MSVC environment is already loaded (ilammy/msvc-dev-cmd) and the
# WDK 10.0.* is installed under "C:\Program Files (x86)\Windows Kits\10".
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

$incDir = Get-ChildItem "$kitsRoot\Include" -Directory |
    Where-Object { $_.Name -match '^10\.0\.\d+\.0$' } |
    Sort-Object Name -Descending |
    Select-Object -First 1
if (-not $incDir) { throw "No WDK Include version directory found" }

$sdkVer  = $incDir.Name
$kmInc   = Join-Path $incDir.FullName "km"
$crtInc  = Join-Path $incDir.FullName "crt"
$shInc   = Join-Path $incDir.FullName "shared"
$kmLib   = Join-Path $kitsRoot "Lib\$sdkVer\km\x64"

Write-Host "WDK SDK version : $sdkVer"
Write-Host "km include      : $kmInc"
Write-Host "km lib          : $kmLib"

$outDir = Split-Path -Parent $Out
if (-not (Test-Path $outDir)) { New-Item -ItemType Directory -Force $outDir | Out-Null }
$obj = Join-Path $outDir "kmbugcheck.obj"

$clArgs = @(
    "/nologo", "/c", "/kernel", "/GS-", "/Gy", "/W3",
    "/D_AMD64_", "/D_X86_AMD64_", "/D_KERNEL_MODE",
    "/I`"$kmInc`"", "/I`"$crtInc`"", "/I`"$shInc`"",
    "`"$Source`"", "/Fo`"$obj`""
)
Write-Host "cl $($clArgs -join ' ')"
& cl @clArgs
if ($LASTEXITCODE -ne 0) { throw "cl failed with $LASTEXITCODE" }

$linkArgs = @(
    "/NOLOGO", "/DRIVER", "/SUBSYSTEM:NATIVE", "/ENTRY:DriverEntry",
    "/MACHINE:X64", "/NODEFAULTLIB",
    "`"$obj`"", "/OUT:`"$Out`"", "/LIBPATH:`"$kmLib`"", "ntoskrnl.lib"
)
Write-Host "link $($linkArgs -join ' ')"
& link @linkArgs
if ($LASTEXITCODE -ne 0) { throw "link failed with $LASTEXITCODE" }

if (-not (Test-Path $Out)) { throw "driver output missing: $Out" }
Write-Host "driver built: $Out ($((Get-Item $Out).Length) bytes)"
