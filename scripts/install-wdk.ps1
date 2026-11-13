# Install the WDK kernel headers/libs in the CI runner.
#
# The windows-latest image ships the DriverKit VS extension but not the km
# headers/lib payload, so a kernel driver cannot compile out of the box.  This
# downloads the matching WDK 26100 bootstrapper and installs it quietly.
# Exit codes: 0 success, 3010 success-reboot-required (accepted on CI).

$ErrorActionPreference = "Stop"

$wdk = Join-Path $env:TEMP "wdksetup.exe"
if (-not (Test-Path $wdk)) {
    Write-Host "Downloading WDK installer..."
    Invoke-WebRequest -Uri "https://go.microsoft.com/fwlink/?linkid=2324617" -OutFile $wdk
}

Write-Host "Installing WDK (quiet, no reboot)..."
$p = Start-Process -FilePath $wdk -ArgumentList "/features", "+", "/quiet", "/norestart" -Wait -PassThru
Write-Host "WDK installer exit code: $($p.ExitCode)"

if ($p.ExitCode -notin 0, 3010) {
    throw "WDK install failed with exit code $($p.ExitCode)"
}

# Verify the kernel headers landed.
$kitsRoot = "${env:ProgramFiles(x86)}\Windows Kits\10"
$km = Get-ChildItem "$kitsRoot\Include" -Directory -ErrorAction SilentlyContinue |
    Where-Object { Test-Path (Join-Path $_.FullName "km\ntddk.h") } |
    Select-Object -First 1
if (-not $km) {
    throw "WDK installed but km\ntddk.h still not found under $kitsRoot"
}
Write-Host "WDK kernel headers present: $($km.FullName)\km"
