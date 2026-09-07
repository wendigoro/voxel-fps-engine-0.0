#Requires -Version 5.1
<#
.SYNOPSIS
  Compile Java painter sources and run voxel.painter.SmokeMain (headless).
.NOTES
  Bitcrush/upscale is display-only; cubic unit occupancy (VOXEL_SIZE=0.001) is unchanged.
#>
param(
  [string]$JavaHome = $env:JAVA_HOME
)

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot
if (-not $Root) { $Root = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path }

$SrcRoot = Join-Path $Root "java\painter\src"
$OutDir = Join-Path $Root "build\painter"
$SmokeOk = Join-Path $Root "build\painter_smoke\smoke_painter_ok.txt"

function Resolve-Javac {
  param([string]$HomeHint)
  if ($HomeHint -and (Test-Path (Join-Path $HomeHint "bin\javac.exe"))) {
    return (Join-Path $HomeHint "bin\javac.exe")
  }
  $cmd = Get-Command javac -ErrorAction SilentlyContinue
  if ($cmd) { return $cmd.Source }
  throw "javac not found on PATH (need JDK 25+). JAVA_HOME=$HomeHint"
}

function Resolve-Java {
  param([string]$HomeHint)
  if ($HomeHint -and (Test-Path (Join-Path $HomeHint "bin\java.exe"))) {
    return (Join-Path $HomeHint "bin\java.exe")
  }
  $cmd = Get-Command java -ErrorAction SilentlyContinue
  if ($cmd) { return $cmd.Source }
  throw "java not found on PATH (need JDK 25+). JAVA_HOME=$HomeHint"
}

Write-Host "======== PAINTER SMOKE ========" -ForegroundColor Magenta
Write-Host "Cubic unit grid: VOXEL_SIZE=0.001 (bitcrush is display-only)" -ForegroundColor DarkGray

if (-not (Test-Path $SrcRoot)) {
  throw "Painter sources missing: $SrcRoot"
}

$javac = Resolve-Javac -HomeHint $JavaHome
$java = Resolve-Java -HomeHint $JavaHome
Write-Host "== javac: $javac ==" -ForegroundColor Cyan
Write-Host "== java:  $java ==" -ForegroundColor Cyan

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
Remove-Item $SmokeOk -ErrorAction SilentlyContinue

$sources = Get-ChildItem -Path $SrcRoot -Filter *.java -Recurse | ForEach-Object { $_.FullName }
if (-not $sources -or $sources.Count -eq 0) {
  throw "No .java files under $SrcRoot"
}

Write-Host ("== compile $($sources.Count) source(s) -> $OutDir ==") -ForegroundColor Cyan
& $javac -encoding UTF-8 -d $OutDir $sources
if ($LASTEXITCODE -ne 0) { throw "javac failed: $LASTEXITCODE" }

# Prefer SmokeMain if present; otherwise fail clearly.
$smokeClass = "voxel.painter.SmokeMain"
$smokePath = Join-Path $OutDir ($smokeClass.Replace(".", "\") + ".class")
if (-not (Test-Path $smokePath)) {
  throw "SmokeMain class not found after compile: expected $smokePath"
}

Write-Host "== run $smokeClass ==" -ForegroundColor Cyan
Push-Location $Root
try {
  & $java -cp $OutDir $smokeClass
  $code = $LASTEXITCODE
} finally {
  Pop-Location
}
if ($code -ne 0) { throw "SmokeMain exited $code" }

if (-not (Test-Path $SmokeOk)) {
  # SmokeMain writes under build/painter_smoke relative to CWD=Root
  throw "smoke_painter_ok.txt missing - SmokeMain did not complete"
}

Write-Host "---- smoke_painter_ok.txt ----" -ForegroundColor DarkGray
Get-Content $SmokeOk
Write-Host "------------------------------" -ForegroundColor DarkGray
Write-Host "SMOKE_PAINTER_OK" -ForegroundColor Green
exit 0
