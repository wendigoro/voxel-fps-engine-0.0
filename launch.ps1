#Requires -Version 5.1
<#
.SYNOPSIS
  Root entry → scripts/launch_dev.ps1

  Backward-compatible switches (-Build / -Run / -Demo / -SmokeOnly) still work
  when the classic scripts exist; otherwise prefer -Action.
#>
param(
  # Must stay a superset of scripts/launch_dev.ps1's own set: this root entry
  # forwards -Action straight through, so a value missing here fails parameter
  # binding before launch_dev.ps1 ever runs.
  [ValidateSet("Build", "Painter", "Engine", "SmokeEngine", "SmokeMovement", "SmokePainter", "SmokeAll", "Stress", "Ui", "Help", "")]
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
