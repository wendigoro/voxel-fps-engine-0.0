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

# Invoke the engine by explicit path. `cd` into the build folder does NOT put the
# exe on PATH, so `cmd /c "cd ... && voxel_engine.exe"` fails to resolve it.
# Start-Process -Wait is required: voxel_engine.exe is a WinMain app, and PowerShell
# does not block on `&` for GUI-subsystem executables, which races the marker file.
# The engine finds its own shaders and JSON relative to its own directory, so it
# is launched with $Build as the working directory.
Write-Host "smoke_cmd=cd /d `"$Build`" && `"$Exe`" --smoke" -ForegroundColor DarkGray
$proc = Start-Process -FilePath $Exe -ArgumentList "--smoke" -WorkingDirectory $Build -Wait -PassThru
$smokeExit = $proc.ExitCode
Write-Host ("smoke_exit=" + $smokeExit)
if ($smokeExit -ne 0) { throw ("smoke test failed with exit " + $smokeExit) }
$ok = Join-Path $Build "smoke_ok.txt"
if (-not (Test-Path $ok)) { throw "smoke_ok.txt missing - engine did not complete smoke path" }
Write-Host "---- smoke_ok.txt ----"
# Write-Host explicitly: piping Get-Content through an outer capture loses the
# report on some hosts, leaving the launcher looking like it printed nothing.
Get-Content $ok | Write-Host
Write-Host "----------------------"

$headless = $SkipInteractive -or ($env:VOXEL_SMOKE_HEADLESS -eq "1")
if ($headless) {
  Write-Host "SkipInteractive/headless set - headless demo only."
} else {
  Write-Host "== interactive warehouse demo =="
  Write-Host "Controls: WASD walk | Space jump | Q/E lean | LMB look | RMB/F fire | 1-4 ammo | R cycle | Esc quit"
  Write-Host "          TAB pack | G take pickup | pack: LMB lift/place, RMB stow, R rotate" -ForegroundColor DarkGray
  Start-Process -FilePath $Exe -WorkingDirectory $Build | Out-Null
  Write-Host ("engine_launch=exe cwd=" + $Build)
}
Write-Host "DEMO_OK"
exit 0