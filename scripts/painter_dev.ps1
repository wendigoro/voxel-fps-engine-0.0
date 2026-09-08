#Requires -Version 5.1
param(
  [Parameter(Position = 0)]
  [ValidateSet(
    "BuildEngine", "SmokeEngine", "StressEngine", "SmokePainter", "SmokeAll",
    "BuildPainter", "RunEngine", "Ui", "Help",
    "Build", "Painter", "Engine", "Smoke"
  )]
  [string]$Action = "Help"
)

$ErrorActionPreference = "Stop"

$ScriptsDir = $PSScriptRoot
if (-not $ScriptsDir) { throw "PSScriptRoot empty" }
$Root = Split-Path -Parent $ScriptsDir
if (-not (Test-Path (Join-Path $Root "java\painter\src"))) {
  throw "Repo root invalid (missing java\painter\src): $Root"
}
if (-not (Test-Path (Join-Path $ScriptsDir "build.ps1"))) {
  throw "Missing scripts\build.ps1 under $ScriptsDir"
}

Set-Location -LiteralPath $Root
$env:VOXEL_ENGINE_ROOT = $Root

function Write-Banner([string]$msg) {
  Write-Host "======== $msg ========" -ForegroundColor Magenta
  Write-Host "ROOT=$Root" -ForegroundColor DarkGray
  Write-Host "SCRIPTS=$ScriptsDir" -ForegroundColor DarkGray
  Write-Host "PWD=$((Get-Location).Path)" -ForegroundColor DarkGray
}

function Invoke-RepoScript {
  param(
    [Parameter(Mandatory = $true)][string]$Name,
    [string[]]$ScriptArgs = @()
  )
  $path = Join-Path $ScriptsDir $Name
  if (-not (Test-Path -LiteralPath $path)) {
    throw "Missing repo script: $path"
  }
  Write-Host "==> & $path $($ScriptArgs -join ' ')" -ForegroundColor Cyan
  & $path @ScriptArgs
  if ($null -ne $LASTEXITCODE -and $LASTEXITCODE -ne 0) {
    throw "Script failed ($LASTEXITCODE): $Name"
  }
}

switch ($Action) {
  "Build"   { $Action = "BuildEngine" }
  "Painter" { $Action = "BuildPainter" }
  "Engine"  { $Action = "RunEngine" }
  "Smoke"   { $Action = "SmokeEngine" }
}

Write-Banner "PAINTER DEV  Action=$Action"

switch ($Action) {
  "Help" {
    Write-Host @"
painter_dev.ps1 actions (all paths under $Root):
  BuildEngine   - scripts/build.ps1
  BuildPainter  - scripts/build_painter.ps1
  SmokeEngine   - build + engine --smoke
  StressEngine  - shotgun + max debris load test
  SmokePainter  - scripts/smoke_painter.ps1
  SmokeAll      - SmokePainter then SmokeEngine
  RunEngine     - scripts/run.ps1
  Ui            - scripts/run_painter_ui.ps1
"@
    exit 0
  }
  "BuildEngine" {
    Invoke-RepoScript -Name "build.ps1"
  }
  "BuildPainter" {
    Invoke-RepoScript -Name "build_painter.ps1"
  }
  "SmokeEngine" {
    $env:VOXEL_SMOKE_HEADLESS = "1"
    try { Invoke-RepoScript -Name "demo.ps1" -ScriptArgs @("-SkipInteractive") }
    finally { Remove-Item Env:VOXEL_SMOKE_HEADLESS -ErrorAction SilentlyContinue }
  }
  "StressEngine" {
    Invoke-RepoScript -Name "stress_engine.ps1"
  }
  "SmokePainter" {
    Invoke-RepoScript -Name "smoke_painter.ps1"
  }
  "SmokeAll" {
    Invoke-RepoScript -Name "smoke_painter.ps1"
    $env:VOXEL_SMOKE_HEADLESS = "1"
    try { Invoke-RepoScript -Name "demo.ps1" -ScriptArgs @("-SkipInteractive") }
    finally { Remove-Item Env:VOXEL_SMOKE_HEADLESS -ErrorAction SilentlyContinue }
    Write-Host "SMOKE_ALL_OK" -ForegroundColor Green
  }
  "RunEngine" {
    Invoke-RepoScript -Name "run.ps1"
  }
  "Ui" {
    Invoke-RepoScript -Name "run_painter_ui.ps1"
  }
  default {
    throw "Unknown action: $Action"
  }
}

Write-Host "PAINTER_DEV_OK action=$Action" -ForegroundColor Green
exit 0