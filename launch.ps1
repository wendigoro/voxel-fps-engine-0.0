#Requires -Version 5.1
<#
.SYNOPSIS
  Root entry → scripts/launch_dev.ps1

  Backward-compatible switches (-Build / -Run / -Demo / -SmokeOnly) still work
  when the classic scripts exist; otherwise prefer -Action.
#>
param(
  [ValidateSet("Build", "Painter", "Engine", "SmokeEngine", "SmokePainter", "SmokeAll", "Ui", "")]
  [string]$Action = "",

  # Legacy switches (engine-era root launch.ps1)
  [switch]$Build,
  [switch]$Run,
  [switch]$Demo,
  [switch]$SmokeOnly,

  [Parameter(ValueFromRemainingArguments = $true)]
  [string[]]$PassThru
)

$ErrorActionPreference = "Stop"
$Scripts = Join-Path $PSScriptRoot "scripts"
$LaunchDev = Join-Path $Scripts "launch_dev.ps1"

if (-not (Test-Path $LaunchDev)) {
  throw "Missing $LaunchDev"
}

# Map legacy switches onto Action when -Action not set.
if (-not $Action) {
  if ($Build) { $Action = "Build" }
  elseif ($Run) { $Action = "Engine" }
  elseif ($SmokeOnly) { $Action = "SmokeEngine" }
  elseif ($Demo) { $Action = "SmokeEngine" }
}

if ($Action) {
  & $LaunchDev -Action $Action @PassThru
  exit $LASTEXITCODE
}

# Interactive menu
& $LaunchDev @PassThru
exit $LASTEXITCODE
