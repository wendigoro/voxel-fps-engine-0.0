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

# Run the exe by absolute path. Resolving a bare "voxel_engine.exe" out of the
# current directory fails when NoDefaultCurrentDirectoryInExePath is set, which
# is the default on hardened Windows installs.
Write-Host "smoke_cmd=`"$Exe`" --smoke  (cwd=$Build)" -ForegroundColor DarkGray
& $Exe --smoke
$smokeExit = $LASTEXITCODE
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
  $cmdExe = Join-Path $env:SystemRoot "System32\cmd.exe"
  # cmd start with the absolute path: the engine loads data/ relative to its cwd.
  $launch = "start `"voxel_engine`" /D `"$Build`" `"$Exe`""
  & $cmdExe /d /c $launch
  Write-Host ("engine_launch=cmd_start cwd=" + $Build)
}
Write-Host "DEMO_OK"
exit 0