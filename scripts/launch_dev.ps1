#Requires -Version 5.1
<#
.SYNOPSIS
  Development launcher: Build | Painter | Engine | smokes | UI.
.DESCRIPTION
  Single entry for voxel engine + painter workflows. Painter bitcrush 32x is
  display/export only; occupancy stays cubic unit cells (VOXEL_SIZE=0.001).

.PARAMETER Action
  Build         - scripts/build.ps1 (engine)
  Painter       - scripts/build_painter.ps1 (compile + SmokeMain)
  Engine        - scripts/run.ps1
  SmokeEngine   - scripts/demo.ps1 -SkipInteractive
  SmokePainter  - scripts/smoke_painter.ps1 (-> build_painter.ps1)
  SmokeAll      - painter smoke then engine smoke
  Ui            - scripts/run_painter_ui.ps1 if present, else Painter
  Help          - print usage
  (empty)       - interactive menu
#>
param(
  [ValidateSet("Build", "Painter", "Engine", "SmokeEngine", "SmokePainter", "SmokeMovement", "SmokeAll", "Ui", "Help", "")]
  [string]$Action = "",

  [Parameter(ValueFromRemainingArguments = $true)]
  [string[]]$PassThru
)

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot
if (-not $Root) { $Root = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path }
$Scripts = $PSScriptRoot

function Invoke-RepoScript {
  param(
    [Parameter(Mandatory = $true)][string]$Name,
    [string[]]$ScriptArgs = @()
  )
  $path = Join-Path $Scripts $Name
  if (-not (Test-Path $path)) {
    throw "Missing script: $path"
  }
  Write-Host "== $Name $($ScriptArgs -join ' ') ==" -ForegroundColor Cyan
  # Use Start-Process to avoid capturing script stdout (which breaks $LASTEXITCODE capture in callers)
  $proc = Start-Process -FilePath "powershell.exe" -ArgumentList "-ExecutionPolicy Bypass -File `"$path`" $($ScriptArgs -join ' ')" -Wait -PassThru -NoNewWindow
  return $proc.ExitCode
}

function Show-Help {
  Write-Host @"
launch_dev.ps1 -Action <Build|Painter|Engine|SmokeEngine|SmokePainter|SmokeMovement|SmokeAll|Ui>
  Build           - engine build.ps1
  Painter         - build_painter.ps1 (core smoke)
  Engine          - run.ps1 interactive
  SmokeEngine     - demo.ps1 -SkipInteractive
  SmokePainter    - smoke_painter.ps1 / build_painter.ps1
  SmokeMovement   - smoke_movement.ps1 (movement system smoke)
  SmokeAll        - painter + engine + movement smokes
  Ui              - run_painter_ui.ps1 if present else Painter
  Help            - this text

Cubic unit voxels only (VOXEL_SIZE=0.001). Bitcrush 32x is display/export only.
"@
}

function Show-Menu {
  Write-Host ""
  Write-Host "======== VOXEL DEV LAUNCHER ========" -ForegroundColor Magenta
  Write-Host " 1  Build           engine (scripts/build.ps1)"
  Write-Host " 2  Painter         build_painter.ps1 (core smoke)"
  Write-Host " 3  Engine          interactive engine (scripts/run.ps1)"
  Write-Host " 4  SmokeEngine     demo.ps1 -SkipInteractive"
  Write-Host " 5  SmokePainter    smoke_painter.ps1"
  Write-Host " 6  SmokeMovement   smoke_movement.ps1 (movement system)"
  Write-Host " 7  SmokeAll        painter + engine + movement smokes"
  Write-Host " 8  Ui              run_painter_ui.ps1 (or Painter)"
  Write-Host " h  Help"
  Write-Host " q  Quit"
  Write-Host "------------------------------------"
  Write-Host "RULES: cubic unit voxels only (VOXEL_SIZE=0.001); bitcrush 32x is display-only." -ForegroundColor DarkGray
  $choice = Read-Host "Select"
  switch -Regex ($choice) {
    "^1$" { return "Build" }
    "^2$" { return "Painter" }
    "^3$" { return "Engine" }
    "^4$" { return "SmokeEngine" }
    "^5$" { return "SmokePainter" }
    "^6$" { return "SmokeMovement" }
    "^7$" { return "SmokeAll" }
    "^8$" { return "Ui" }
    "^[hH]$" { return "Help" }
    "^[qQ]$" { return "Quit" }
    default {
      Write-Host "Unknown selection: $choice" -ForegroundColor Yellow
      return "Quit"
    }
  }
}

function Invoke-Action {
  param([string]$Name)
  switch ($Name) {
    "Help" {
      Show-Help
      return 0
    }
    "Build" {
      return Invoke-RepoScript -Name "build.ps1"
    }
    "Painter" {
      return Invoke-RepoScript -Name "build_painter.ps1"
    }
    "Engine" {
      if ($PassThru -and $PassThru.Count -gt 0) {
        return Invoke-RepoScript -Name "run.ps1" -ScriptArgs $PassThru
      }
      return Invoke-RepoScript -Name "run.ps1"
    }
    "SmokeEngine" {
      return Invoke-RepoScript -Name "demo.ps1" -ScriptArgs @("-SkipInteractive")
    }
    "SmokePainter" {
      $smoke = Join-Path $Scripts "smoke_painter.ps1"
      if (Test-Path $smoke) {
        return Invoke-RepoScript -Name "smoke_painter.ps1"
      }
      return Invoke-RepoScript -Name "build_painter.ps1"
    }
    "SmokeMovement" {
      $smoke = Join-Path $Scripts "smoke_movement.ps1"
      if (Test-Path $smoke) {
        return Invoke-RepoScript -Name "smoke_movement.ps1"
      }
      Write-Host "smoke_movement.ps1 missing" -ForegroundColor Red
      return 1
    }
    "SmokeAll" {
      $p = Invoke-Action -Name "SmokePainter"
      if ($p -ne 0) { return $p }
      $e = Invoke-Action -Name "SmokeEngine"
      if ($e -ne 0) { return $e }
      $m = Invoke-Action -Name "SmokeMovement"
      if ($m -ne 0) { return $m }
      Write-Host "SMOKE_ALL_OK" -ForegroundColor Green
      return 0
    }
    "Ui" {
      $ui = Join-Path $Scripts "run_painter_ui.ps1"
      if (Test-Path $ui) {
        return Invoke-RepoScript -Name "run_painter_ui.ps1"
      }
      Write-Host "run_painter_ui.ps1 missing; falling back to Painter" -ForegroundColor Yellow
      return Invoke-Action -Name "Painter"
    }
    "Quit" { return 0 }
    default {
      Write-Host "Unknown Action: $Name" -ForegroundColor Red
      Show-Help
      return 1
    }
  }
}

if (-not $Action) {
  $Action = Show-Menu
}

$code = Invoke-Action -Name $Action
if ($null -eq $code) { $code = 0 }
exit $code
