#Requires -Version 5.1
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
# Launch via cmd start to avoid WDAC blocks on Start-Process -FilePath <exe>.
$cmdExe = Join-Path $env:SystemRoot "System32\cmd.exe"
$argLine = if ($EngineArgs -and $EngineArgs.Count -gt 0) { ($EngineArgs -join " ") } else { "" }
if ($argLine) {
  & $cmdExe /d /c "start `"voxel_engine`" /D `"$Build`" `"$Exe`" $argLine"
} else {
  & $cmdExe /d /c "start `"voxel_engine`" /D `"$Build`" `"$Exe`""
}
Write-Host "engine_launch=cmd_start"
exit 0
