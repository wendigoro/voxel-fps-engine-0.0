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
        default {
            Write-Host "Unknown Action: $Name" -ForegroundColor Red
            return 1
        }
    }
}

$result = Invoke-Action -Name "SmokePainter"
Write-Host "Result type: $($result.GetType().Name)"
if ($result -is [Array]) {
    Write-Host "Result count: $($result.Count)"
    for ($i = 0; $i -lt $result.Count; $i++) {
        Write-Host "Result[$i]: $($result[$i])"
    }
} else {
    Write-Host "Result: $result"
}