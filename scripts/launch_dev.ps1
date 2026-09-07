#Requires -Version 5.1
param(
  [ValidateSet("Build","Painter","Engine","SmokeEngine","SmokePainter","SmokeAll","Ui","Help")]
  [string]$Action = "Help"
)
$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot
$Scripts = $PSScriptRoot

switch ($Action) {
  "Help" {
    Write-Host @"
launch_dev.ps1 -Action <Build|Painter|Engine|SmokeEngine|SmokePainter|SmokeAll|Ui>
  Build         - engine build.ps1
  Painter       - build_painter.ps1 (core smoke)
  Engine        - run.ps1 interactive
  SmokeEngine   - demo.ps1 -SkipInteractive
  SmokePainter  - build_painter.ps1
  SmokeAll      - engine + painter smokes
  Ui            - run_painter_ui.ps1 if present else Painter
"@
  }
  "Build" { & (Join-Path $Scripts "build.ps1"); if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE } }
  "Painter" { & (Join-Path $Scripts "build_painter.ps1"); if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE } }
  "Engine" { & (Join-Path $Scripts "run.ps1"); if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE } }
  "SmokeEngine" {
    & (Join-Path $Scripts "demo.ps1") -SkipInteractive
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
  }
  "SmokePainter" { & (Join-Path $Scripts "build_painter.ps1"); if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE } }
  "SmokeAll" {
    & (Join-Path $Scripts "build_painter.ps1"); if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    & (Join-Path $Scripts "demo.ps1") -SkipInteractive; if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    Write-Host "SMOKE_ALL_OK" -ForegroundColor Green
  }
  "Ui" {
    $ui = Join-Path $Scripts "run_painter_ui.ps1"
    if (Test-Path $ui) { & $ui; if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE } }
    else { & (Join-Path $Scripts "build_painter.ps1"); if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE } }
  }
}
exit 0
