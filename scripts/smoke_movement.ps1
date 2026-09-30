#Requires -Version 5.1
<#
.SYNOPSIS
  Movement system headless smoke test.
.DESCRIPTION
  Drives the real movement seam (src/movement.hpp) with scripted SimInput
  against a local grid and asserts the body/camera contract from RULES.md:
  stance is a body height, edges are edges not latches, collision is the
  authoritative grid, and the camera offset can never be load-bearing.
.PARAMETER SkipBuild
  Reuse the existing build/voxel_engine.exe instead of rebuilding first.
#>
param([switch]$SkipBuild)

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot
if (-not $Root) { $Root = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path }

$Build = Join-Path $Root "build"
$Exe = Join-Path $Build "voxel_engine.exe"

Write-Host "======== MOVEMENT SYSTEM SMOKE ========" -ForegroundColor Magenta
Write-Host "Cubic unit grid: VOXEL_SIZE=0.001" -ForegroundColor DarkGray

if (-not $SkipBuild) {
    & (Join-Path $PSScriptRoot "build.ps1")
    if ($LASTEXITCODE -ne 0) { throw "build.ps1 failed" }
}

$Marker = Join-Path $Build "movement_smoke_ok.txt"
Write-Host "== movement smoke test (--smoke-movement) ==" -ForegroundColor Cyan
Remove-Item $Marker -ErrorAction SilentlyContinue

$p = Start-Process -FilePath $Exe -ArgumentList "--smoke-movement" -WorkingDirectory $Build -PassThru -Wait
Write-Host ("smoke_exit=" + $p.ExitCode)

if (-not (Test-Path $Marker)) {
    throw "movement_smoke_ok.txt missing - engine did not complete the movement smoke path (exit $($p.ExitCode))"
}

Write-Host "---- movement_smoke_ok.txt ----" -ForegroundColor DarkGray
Get-Content $Marker
Write-Host "-------------------------------" -ForegroundColor DarkGray

# The engine already exits 2 when any movement assertion fails, but check the
# marker too so a stale file can never read as a pass.
$all = @{}
Get-Content $Marker | ForEach-Object {
    if ($_ -match '^(\w+)=(.*)$') { $all[$Matches[1]] = $Matches[2] }
}
$failed = @()
foreach ($k in $all.Keys) {
    if ($k -eq 'move_ok') { continue }
    if ($k -match '_ok$|_(enters|exhausts|detects|times_out|moves|not_solid|presentation_only)$') {
        if ($all[$k] -ne '1') { $failed += "$k=$($all[$k])" }
    }
}
if ($all['move_ok'] -ne '1') { $failed += "move_ok=$($all['move_ok'])" }
if ($failed.Count -gt 0) {
    $failed | ForEach-Object { Write-Host ("  FAIL " + $_) -ForegroundColor Red }
    throw ("movement smoke test failed: " + ($failed -join ", "))
}

Write-Host "MOVEMENT_SMOKE_OK" -ForegroundColor Green
exit 0
