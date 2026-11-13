# Generate src/sys_bytes.h embedding the built kmbugcheck.sys.
#
# If -Sys is omitted (or the file is empty), an empty driver blob is emitted so
# the x86 / local build still compiles; the launch path then reports that no
# driver payload is embedded.
#
# Usage:
#   pwsh scripts/gen-sys-header.ps1 -Out src/sys_bytes.h [-Sys build/kmbugcheck.sys]

param(
    [Parameter(Mandatory = $true)][string]$Out,
    [string]$Sys = ""
)

$ErrorActionPreference = "Stop"

$bytes = @()
if ($Sys -ne "" -and (Test-Path $Sys)) {
    $bytes = [System.IO.File]::ReadAllBytes((Resolve-Path $Sys))
}

$sb = New-Object System.Text.StringBuilder
[void]$sb.AppendLine("#pragma once")
[void]$sb.AppendLine("#include <stddef.h>")
[void]$sb.AppendLine("")

if ($bytes.Count -eq 0) {
    [void]$sb.AppendLine("static const unsigned char kmbugcheck_sys[] = { 0 };")
    [void]$sb.AppendLine("static const unsigned int kmbugcheck_sys_len = 0;")
} else {
    [void]$sb.AppendLine("static const unsigned char kmbugcheck_sys[] = {")
    for ($i = 0; $i -lt $bytes.Count; $i += 16) {
        $line = ($bytes[$i..([Math]::Min($i + 15, $bytes.Count - 1))] |
            ForEach-Object { "0x{0:x2}" -f $_ }) -join ", "
        [void]$sb.AppendLine("    $line,")
    }
    [void]$sb.AppendLine("};")
    [void]$sb.AppendLine("static const unsigned int kmbugcheck_sys_len =")
    [void]$sb.AppendLine("    (unsigned int)sizeof(kmbugcheck_sys);")
}

$outDir = Split-Path -Parent $Out
if (-not (Test-Path $outDir)) { New-Item -ItemType Directory -Force $outDir | Out-Null }
[System.IO.File]::WriteAllText($Out, $sb.ToString(), (New-Object System.Text.UTF8Encoding($false)))

Write-Host "sys_bytes.h written: $($bytes.Count) driver bytes embedded"
