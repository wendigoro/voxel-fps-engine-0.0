#Requires -Version 5.1
<#
.SYNOPSIS
  Compile src/view_isolation_check.cpp to prove the view/sim split is structural.
.DESCRIPTION
  view_isolation_check.cpp is a translation unit that includes ONLY view_chunk.hpp.
  Its value is in what it must NOT be able to do: if someone adds
  `#include "sim_world.hpp"` to the view header, or otherwise gives a view
  client a path to the authoritative grid, the #error directives fire and this
  compile fails.

  The file is never linked into the engine, so -fsyntax-only is sufficient and
  faster than a full build. RULES.md "Authority and the view/sim split" makes
  the split a mandatory contract, and a contract that nothing checks is a
  convention; this is the check that makes it load-bearing.
#>
param([switch]$SkipToolchainProbe)

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot
if (-not $Root) { $Root = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path }
$Src = Join-Path $Root "src"
$Build = Join-Path $Root "build"
$VK = "C:\VulkanSDK\1.4.357.0"
$LLVM = "C:\Program Files\LLVM\bin"
$VCVARS = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"

$SrcFile = Join-Path $Src "view_isolation_check.cpp"
if (-not (Test-Path $SrcFile)) { throw "missing $SrcFile" }

New-Item -ItemType Directory -Force -Path $Build | Out-Null
$okPath = Join-Path $Build "view_isolation_ok.txt"
Remove-Item $okPath -ErrorAction SilentlyContinue

if (-not $SkipToolchainProbe) {
    if (-not (Test-Path $VCVARS)) { throw "vcvars64.bat missing: $VCVARS" }
    if (-not (Test-Path (Join-Path $LLVM "clang++.exe"))) { throw "clang++ missing under $LLVM" }
}

$bat = @"
@echo off
REM vcvars64.bat probes for vswhere.exe and prints a benign "not recognized"
REM warning on stderr when absent; discard both streams and trust the exit code.
call "$VCVARS" >nul 2>&1
set "PATH=$LLVM;%PATH%"
clang++.exe -std=c++17 -fsyntax-only -D_CRT_SECURE_NO_WARNINGS -I"$Src" -I"$VK\Include" "$SrcFile"
echo CLANG_EXIT=%ERRORLEVEL%
exit /b %ERRORLEVEL%
"@
$batPath = Join-Path $Build "_view_isolation.bat"
Set-Content -Path $batPath -Value $bat -Encoding ASCII

# Native tools are chatty on stderr; decide success on the exit code alone.
$prevErrorAction = $ErrorActionPreference
$ErrorActionPreference = "Continue"
try {
    cmd /c $batPath
    $clangExit = $LASTEXITCODE
} finally {
    $ErrorActionPreference = $prevErrorAction
}

if ($clangExit -ne 0) {
    Write-Host "VIEW_ISOLATION_FAIL: the view TU can no longer compile standalone." -ForegroundColor Red
    Write-Host "A view client must not be able to name sim_world.hpp (see src/view_isolation_check.cpp)." -ForegroundColor Red
    exit 1
}

Set-Content -Path $okPath -Value "view_isolation=1`n" -Encoding ASCII
Write-Host "VIEW_ISOLATION_OK view TU compiles without reaching the sim grid" -ForegroundColor Green
Write-Host "view_isolation_ok.txt written" -ForegroundColor DarkGray
exit 0
