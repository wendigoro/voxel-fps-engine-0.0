#Requires -Version 5.1
$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot
$SrcRoot = Join-Path $Root "java\painter\src"
$Out = Join-Path $Root "build\painter"
New-Item -ItemType Directory -Force -Path $Out | Out-Null

$files = Get-ChildItem -Path $SrcRoot -Recurse -Filter *.java | ForEach-Object { $_.FullName }
if (-not $files -or $files.Count -eq 0) { throw "No Java sources under $SrcRoot" }

Write-Host "== javac painter ==" -ForegroundColor Cyan
& javac -encoding UTF-8 -d $Out $files
if ($LASTEXITCODE -ne 0) { throw "javac failed: $LASTEXITCODE" }

Write-Host "== SmokeMain ==" -ForegroundColor Cyan
Push-Location $Root
try {
  & java -cp $Out voxel.painter.SmokeMain $Root
  if ($LASTEXITCODE -ne 0) { throw "SmokeMain failed: $LASTEXITCODE" }
} finally {
  Pop-Location
}

$ok = Join-Path $Out "painter_smoke_ok.txt"
if (-not (Test-Path $ok)) { throw "painter_smoke_ok.txt missing" }
Get-Content $ok
Write-Host "PAINTER_BUILD_OK" -ForegroundColor Green
exit 0
