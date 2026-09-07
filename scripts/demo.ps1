#Requires -Version 5.1
param([switch]$SkipInteractive)
$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot
$Build = Join-Path $Root "build"
$Exe = Join-Path $Build "voxel_engine.exe"
Write-Host "======== VOXEL FPS ENGINE DEMO ========" -ForegroundColor Magenta
Write-Host "Cubic unit grid rule: see RULES.md (VOXEL_SIZE=0.001)" -ForegroundColor DarkGray
& (Join-Path $PSScriptRoot "build.ps1")
if ($LASTEXITCODE -ne 0) { throw "build.ps1 failed" }
Write-Host "== smoke test (--smoke) ==" -ForegroundColor Cyan
Remove-Item (Join-Path $Build "smoke_ok.txt") -ErrorAction SilentlyContinue
$p = Start-Process -FilePath $Exe -ArgumentList "--smoke" -WorkingDirectory $Build -PassThru -Wait
Write-Host ("smoke_exit=" + $p.ExitCode)
if ($p.ExitCode -ne 0) { throw ("smoke test failed with exit " + $p.ExitCode) }
$ok = Join-Path $Build "smoke_ok.txt"
if (-not (Test-Path $ok)) { throw "smoke_ok.txt missing - engine did not complete smoke path" }
Write-Host "---- smoke_ok.txt ----"
Get-Content $ok
Write-Host "----------------------"
if (-not $SkipInteractive) {
  Write-Host "== interactive warehouse demo =="
  Write-Host "Controls: WASD fly | LMB look | RMB/F fire | 1-4 ammo | R cycle | Esc quit"
  $live = Start-Process -FilePath $Exe -WorkingDirectory $Build -PassThru
  Write-Host ("interactive_pid=" + $live.Id)
} else {
  Write-Host "SkipInteractive set - headless demo only."
}
Write-Host "DEMO_OK"
exit 0
