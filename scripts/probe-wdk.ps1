# Diagnostic: locate WDK kernel headers (ntddk.h) and ntoskrnl.lib on the runner.
# Safe to run standalone; prints everything it finds, exits 0.

$roots = @(
    "${env:ProgramFiles(x86)}\Windows Kits\10",
    "${env:ProgramFiles}\Windows Kits\10",
    "${env:ProgramFiles}\Microsoft Visual Studio"
)

foreach ($root in $roots) {
    if (-not (Test-Path $root)) { continue }
    Write-Host "=== $root ==="
    Get-ChildItem $root -Directory -ErrorAction SilentlyContinue |
        ForEach-Object { Write-Host "  dir: $($_.FullName)" }
}

foreach ($root in $roots) {
    if (-not (Test-Path $root)) { continue }
    Write-Host "`n--- search ntddk.h under $root ---"
    Get-ChildItem $root -Recurse -Filter ntddk.h -ErrorAction SilentlyContinue -Depth 6 |
        ForEach-Object { Write-Host "  ntddk.h: $($_.FullName)" }
    Write-Host "`n--- search ntoskrnl.lib under $root ---"
    Get-ChildItem $root -Recurse -Filter ntoskrnl.lib -ErrorAction SilentlyContinue -Depth 6 |
        ForEach-Object { Write-Host "  ntoskrnl.lib: $($_.FullName)" }
}

$inc = "${env:ProgramFiles(x86)}\Windows Kits\10\Include"
if (Test-Path $inc) {
    Write-Host "`n=== Include tree ($inc) ==="
    Get-ChildItem $inc -Recurse -Directory -ErrorAction SilentlyContinue -Depth 2 |
        ForEach-Object { Write-Host "  $($_.FullName)" }
}
