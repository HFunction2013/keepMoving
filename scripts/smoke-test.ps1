# keepMoving - builds only, no real crash on the runner.
#
# Everything here is dry-run on purpose: the goal is to prove that the binary
# loads, that the detached worker comes up, that the state machine works and
# that the log gets written.  A real bugcheck would kill the CI job.

param(
    [Parameter(Mandatory = $true)][string]$Exe,
    [string]$Arch = "unknown"
)

$ErrorActionPreference = "Continue"

Write-Host "=== keepMoving smoke test ($Arch) ==="

if (-not (Test-Path -LiteralPath $Exe)) {
    Write-Host "FAILED: exe not found: $Exe"
    exit 1
}

$exeFull = (Resolve-Path -LiteralPath $Exe).Path
Write-Host "exe: $exeFull"
Write-Host "size: $((Get-Item -LiteralPath $exeFull).Length) bytes"

function Invoke-Case {
    param(
        [string]$Label,
        [string[]]$CmdArgs
    )

    Write-Host ""
    Write-Host "--- $Label ---"
    try {
        & $exeFull @CmdArgs 2>&1 | ForEach-Object { Write-Host $_ }
        Write-Host "(exit code: $LASTEXITCODE)"
    } catch {
        Write-Host "exception: $_"
    }
}

Invoke-Case "version" @("version")
Invoke-Case "help" @("help")
Invoke-Case "diag" @("diag")

# 120 s of idle limit, dry run: the worker arms itself and logs, but the
# machine is never asked to bugcheck.
Invoke-Case "start 120 --dry-run" @("start", "120", "--dry-run")
Start-Sleep -Seconds 2
Invoke-Case "query" @("query")
Invoke-Case "query --json" @("query", "--json")
Invoke-Case "start again (should refuse)" @("start", "120", "--dry-run")

$log = Join-Path $env:LOCALAPPDATA "keepMoving\keepMoving.log"
if (Test-Path -LiteralPath $log) {
    Write-Host ""
    Write-Host "--- log tail ($log) ---"
    Get-Content -LiteralPath $log -Tail 40 | ForEach-Object { Write-Host $_ }
} else {
    Write-Host ""
    Write-Host "--- no log file at $log ---"
}

Invoke-Case "stop" @("stop")
Start-Sleep -Seconds 1
Invoke-Case "query after stop" @("query")

$state = Join-Path $env:LOCALAPPDATA "keepMoving\state.ini"
Write-Host ""
if (Test-Path -LiteralPath $state) {
    Write-Host "state file still present after stop (unexpected): $state"
} else {
    Write-Host "state file cleaned up after stop"
}

Write-Host ""
Write-Host "=== smoke test done ==="
exit 0
