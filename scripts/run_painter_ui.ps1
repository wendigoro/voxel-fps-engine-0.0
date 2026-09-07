#Requires -Version 5.1
$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot
& (Join-Path $PSScriptRoot "build_painter.ps1")
if ($LASTEXITCODE -ne 0) { throw "build_painter failed" }
$Out = Join-Path $Root "build\painter"
Write-Host "Starting PainterApp UI..." -ForegroundColor Cyan
Start-Process -FilePath "java" -ArgumentList @("-cp", $Out, "voxel.painter.ui.PainterApp", $Root) -WorkingDirectory $Root
Write-Host "UI_LAUNCHED"
exit 0
