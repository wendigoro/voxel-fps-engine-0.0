#Requires -Version 5.1
<#
.SYNOPSIS
  Movement system headless smoke test.
.NOTES
  Runs engine with --smoke-movement flag to exercise stance, gait, slide, wallrun, dash.
  Writes build/movement_smoke_ok.txt with movement state summary.
#>
param()

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot
if (-not $Root) { $Root = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path }

$Build = Join-Path $Root "build"
$Exe = Join-Path $Build "voxel_engine.exe"

Write-Host "======== MOVEMENT SYSTEM SMOKE ========" -ForegroundColor Magenta
Write-Host "Cubic unit grid: VOXEL_SIZE=0.001" -ForegroundColor DarkGray

# Ensure engine is built
& (Join-Path $PSScriptRoot "build.ps1")
if ($LASTEXITCODE -ne 0) { throw "build.ps1 failed" }

Write-Host "== movement smoke test (--smoke-movement) ==" -ForegroundColor Cyan
Remove-Item (Join-Path $Build "movement_smoke_ok.txt") -ErrorAction SilentlyContinue

$p = Start-Process -FilePath $Exe -ArgumentList "--smoke-movement" -WorkingDirectory $Build -PassThru -Wait
Write-Host ("smoke_exit=" + $p.ExitCode)
if ($p.ExitCode -ne 0) { throw ("movement smoke test failed with exit " + $p.ExitCode) }

$ok = Join-Path $Build "movement_smoke_ok.txt"
if (-not (Test-Path $ok)) { throw "movement_smoke_ok.txt missing - engine did not complete movement smoke path" }

Write-Host "---- movement_smoke_ok.txt ----" -ForegroundColor DarkGray
Get-Content $ok
Write-Host "-------------------------------" -ForegroundColor DarkGray
Write-Host "MOVEMENT_SMOKE_OK" -ForegroundColor Green
exit 0