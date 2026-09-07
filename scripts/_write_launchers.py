from pathlib import Path

root = Path(__file__).resolve().parents[1]
scripts = root / "scripts"

demo = r'''#Requires -Version 5.1
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
'''

run = r'''#Requires -Version 5.1
param(
  [switch]$Rebuild,
  [Parameter(ValueFromRemainingArguments = $true)]
  [string[]]$EngineArgs
)
$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot
$Build = Join-Path $Root "build"
$Exe = Join-Path $Build "voxel_engine.exe"
if ($Rebuild -or -not (Test-Path $Exe)) {
  Write-Host "== rebuild required =="
  & (Join-Path $PSScriptRoot "build.ps1")
  if ($LASTEXITCODE -ne 0) { throw "build failed" }
}
if (-not (Test-Path (Join-Path $Build "projectiles.json"))) {
  Copy-Item (Join-Path $Root "data\projectiles.json") (Join-Path $Build "projectiles.json") -Force -ErrorAction SilentlyContinue
}
if (-not (Test-Path (Join-Path $Build "shaders\voxel.vert.spv"))) {
  throw "Missing SPIR-V shaders in build/shaders - run scripts/build.ps1"
}
Write-Host ("Starting: " + $Exe + " " + ($EngineArgs -join " "))
Write-Host ("CWD: " + $Build)
if ($EngineArgs -and $EngineArgs.Count -gt 0) {
  $p = Start-Process -FilePath $Exe -ArgumentList $EngineArgs -WorkingDirectory $Build -PassThru
} else {
  $p = Start-Process -FilePath $Exe -WorkingDirectory $Build -PassThru
}
Write-Host ("engine_pid=" + $p.Id)
exit 0
'''

launch = r'''#Requires -Version 5.1
param(
  [switch]$Build,
  [switch]$Run,
  [switch]$Demo,
  [switch]$SmokeOnly
)
$ErrorActionPreference = "Stop"
$Scripts = Join-Path $PSScriptRoot "scripts"
if ($Build) { & (Join-Path $Scripts "build.ps1"); exit $LASTEXITCODE }
if ($Run) { & (Join-Path $Scripts "run.ps1") -Rebuild:$false; exit $LASTEXITCODE }
if ($SmokeOnly) { & (Join-Path $Scripts "demo.ps1") -SkipInteractive; exit $LASTEXITCODE }
& (Join-Path $Scripts "demo.ps1")
exit $LASTEXITCODE
'''

(scripts / "demo.ps1").write_text(demo, encoding="utf-8", newline="\n")
(scripts / "run.ps1").write_text(run, encoding="utf-8", newline="\n")
(root / "launch.ps1").write_text(launch, encoding="utf-8", newline="\n")
print("launchers written")
