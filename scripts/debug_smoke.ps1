#Requires -Version 5.1
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
    if ($ScriptArgs.Count -gt 0) {
        & $path @ScriptArgs
    } else {
        & $path
    }
    return $LASTEXITCODE
}

function Invoke-Action {
    param([string]$Name)
    Write-Host "Invoke-Action called with: $Name"
    switch ($Name) {
        "SmokePainter" {
            Write-Host "Running SmokePainter..."
            $smoke = Join-Path $Scripts "smoke_painter.ps1"
            if (Test-Path $smoke) {
                return Invoke-RepoScript -Name "smoke_painter.ps1"
            }
            return Invoke-RepoScript -Name "build_painter.ps1"
        }
        "SmokeEngine" {
            Write-Host "Running SmokeEngine..."
            return Invoke-RepoScript -Name "demo.ps1" -ScriptArgs @("-SkipInteractive")
        }
        "SmokeMovement" {
            Write-Host "Running SmokeMovement..."
            $smoke = Join-Path $Scripts "smoke_movement.ps1"
            if (Test-Path $smoke) {
                return Invoke-RepoScript -Name "smoke_movement.ps1"
            }
            Write-Host "smoke_movement.ps1 missing" -ForegroundColor Red
            return 1
        }
        "SmokeAll" {
            Write-Host "Running SmokeAll..."
            $p = Invoke-Action -Name "SmokePainter"
            Write-Host "SmokePainter returned: $p"
            if ($p -ne 0) { return $p }
            $e = Invoke-Action -Name "SmokeEngine"
            Write-Host "SmokeEngine returned: $e"
            if ($e -ne 0) { return $e }
            $m = Invoke-Action -Name "SmokeMovement"
            Write-Host "SmokeMovement returned: $m"
            if ($m -ne 0) { return $m }
            Write-Host "SMOKE_ALL_OK" -ForegroundColor Green
            return 0
        }
        default {
            Write-Host "Unknown Action: $Name" -ForegroundColor Red
            return 1
        }
    }
}

$code = Invoke-Action -Name "SmokeAll"
Write-Host "Final code: $code"
exit $code