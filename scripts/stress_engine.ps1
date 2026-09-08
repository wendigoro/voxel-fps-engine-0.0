#Requires -Version 5.1
param([switch]$SkipBuild)
$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot
$Build = Join-Path $Root "build"
$Exe = Join-Path $Build "voxel_engine.exe"
Write-Host "======== ENGINE STRESS (shotgun + max debris) ========" -ForegroundColor Magenta
if (-not $SkipBuild) {
  & (Join-Path $PSScriptRoot "build.ps1")
  if ($LASTEXITCODE -ne 0) { throw "build.ps1 failed" }
}
if (-not (Test-Path $Exe)) { throw "missing $Exe" }
Remove-Item (Join-Path $Build "stress_ok.txt") -ErrorAction SilentlyContinue
$cmdExe = Join-Path $env:SystemRoot "System32\cmd.exe"
$line = "cd /d `"$Build`" && voxel_engine.exe --stress"
Write-Host "stress_cmd=$line"
& $cmdExe /d /c $line
$code = $LASTEXITCODE
Write-Host "stress_exit=$code"
$ok = Join-Path $Build "stress_ok.txt"
if (-not (Test-Path $ok)) { throw "stress_ok.txt missing" }
Write-Host "---- stress_ok.txt ----"
Get-Content $ok
Write-Host "-----------------------"
$map = @{}
Get-Content $ok | ForEach-Object {
  if ($_ -match '^([^=]+)=(.*)$') { $map[$matches[1]] = $matches[2] }
}
function Num([string]$k) { if ($map.ContainsKey($k)) { [double]$map[$k] } else { -1 } }
$fail = @()
if ((Num 'stress') -ne 1) { $fail += 'stress flag missing' }
if ((Num 'shotgun_shots') -lt 80) { $fail += "shotgun_shots=$(Num 'shotgun_shots') < 80" }
if ((Num 'pellet_spawns') -lt 400) { $fail += "pellet_spawns low" }
if ((Num 'debris_active_peak') -lt 150) { $fail += "debris_active_peak=$(Num 'debris_active_peak') < 150" }
if ((Num 'avg_frame_ms') -gt 16.5) { $fail += "avg_frame_ms=$(Num 'avg_frame_ms') > 16.5" }
if ((Num 'max_frame_ms') -gt 50) { $fail += "max_frame_ms=$(Num 'max_frame_ms') > 50" }
if ((Num 'debris_upload_us_max') -gt 250) { $fail += "debris_upload_us_max=$(Num 'debris_upload_us_max') > 250" }
if ((Num 'stress_ok') -ne 1) { $fail += 'engine stress_ok=0' }
if ($code -ne 0) { $fail += "process exit $code" }
if ($fail.Count -gt 0) {
  Write-Host "STRESS_FAIL: $($fail -join '; ')" -ForegroundColor Red
  exit 1
}
Write-Host "STRESS_OK load=shotgun+debris stable" -ForegroundColor Green
exit 0