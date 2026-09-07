#Requires -Version 5.1
<#
.SYNOPSIS
  Painter headless smoke - delegates to build_painter.ps1 (javac + SmokeMain).
.NOTES
  Bitcrush/upscale is display-only; cubic unit occupancy (VOXEL_SIZE=0.001) is unchanged.
  SmokeMain writes build/painter/painter_smoke_ok.txt
#>
param(
  [string]$JavaHome = $env:JAVA_HOME
)

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot
if (-not $Root) { $Root = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path }

$BuildPainter = Join-Path $PSScriptRoot "build_painter.ps1"
if (-not (Test-Path $BuildPainter)) {
  throw "Missing $BuildPainter"
}

Write-Host "======== PAINTER SMOKE ========" -ForegroundColor Magenta
Write-Host "Cubic unit grid: VOXEL_SIZE=0.001 (bitcrush is display-only)" -ForegroundColor DarkGray

if ($JavaHome) {
  $bin = Join-Path $JavaHome "bin"
  if (Test-Path $bin) {
    $env:PATH = "$bin;$env:PATH"
  }
}

& $BuildPainter
if ($LASTEXITCODE -ne 0) {
  throw "build_painter.ps1 failed: $LASTEXITCODE"
}

$ok = Join-Path $Root "build\painter\painter_smoke_ok.txt"
if (-not (Test-Path $ok)) {
  throw "painter_smoke_ok.txt missing at $ok"
}

Write-Host "---- painter_smoke_ok.txt ----" -ForegroundColor DarkGray
Get-Content $ok
Write-Host "-----------------------------" -ForegroundColor DarkGray
Write-Host "SMOKE_PAINTER_OK" -ForegroundColor Green
exit 0
