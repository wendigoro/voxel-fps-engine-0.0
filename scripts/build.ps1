#Requires -Version 5.1
<#
.SYNOPSIS
  Export Python projectile/material defs and compile voxel_engine.exe
#>
$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot
if (-not $Root) { $Root = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path }

$Build = Join-Path $Root "build"
$Src = Join-Path $Root "src"
$VK = "C:\VulkanSDK\1.4.357.0"
$LLVM = "C:\Program Files\LLVM\bin"
$VCVARS = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"

New-Item -ItemType Directory -Force -Path $Build, (Join-Path $Build "shaders") | Out-Null

Write-Host "== check cross-language constants ==" -ForegroundColor Cyan
$py = Get-Command python -ErrorAction SilentlyContinue
if (-not $py) { throw "python not found on PATH" }
# Fails the build if the C++/Python/Java material tables or the unit scale drift.
# The Python voxel-mass scale was once 1000x off the C++ one; nothing caught it.
& python (Join-Path $Root "scripts\check_constants.py")
if ($LASTEXITCODE -ne 0) { throw "check_constants.py failed: $LASTEXITCODE" }

Write-Host "== export projectiles/materials (Python) ==" -ForegroundColor Cyan
& python (Join-Path $Root "python\export_projectiles.py")
if ($LASTEXITCODE -ne 0) { throw "export_projectiles.py failed: $LASTEXITCODE" }

Write-Host "== compile shaders ==" -ForegroundColor Cyan
$glslc = Join-Path $VK "Bin\glslc.exe"
if (-not (Test-Path $glslc)) { throw "glslc missing: $glslc" }
& $glslc (Join-Path $Root "shaders\voxel.vert") -o (Join-Path $Build "shaders\voxel.vert.spv")
& $glslc (Join-Path $Root "shaders\voxel.frag") -o (Join-Path $Build "shaders\voxel.frag.spv")
if (Test-Path (Join-Path $Root "shaders\post.vert")) {
  & $glslc (Join-Path $Root "shaders\post.vert") -o (Join-Path $Build "shaders\post.vert.spv")
  & $glslc (Join-Path $Root "shaders\post.frag") -o (Join-Path $Build "shaders\post.frag.spv")
}

Write-Host "== compile engine (Clang + VS env) ==" -ForegroundColor Cyan
if (-not (Test-Path $VCVARS)) { throw "vcvars64.bat missing: $VCVARS" }
if (-not (Test-Path (Join-Path $LLVM "clang++.exe"))) { throw "clang++ missing under $LLVM" }

$bat = @"
@echo off
REM vcvars64.bat probes for vswhere.exe and prints a benign "not recognized"
REM warning on stderr when it is absent. That must not abort the build, so
REM both streams are discarded and the real clang exit code is what counts.
call "$VCVARS" >nul 2>&1
set "PATH=$LLVM;%PATH%"
clang++.exe -std=c++17 -O2 -g -D_CRT_SECURE_NO_WARNINGS -I"$Src" -I"$VK\Include" "$Src\main.cpp" -o "$Build\voxel_engine.exe" -L"$VK\Lib" -lvulkan-1 -luser32 -lgdi32 -lshell32
echo CLANG_EXIT=%ERRORLEVEL%
exit /b %ERRORLEVEL%
"@
$batPath = Join-Path $Build "_build.bat"
Set-Content -Path $batPath -Value $bat -Encoding ASCII
# Native tools are chatty on stderr (linker notes, vcvars probes). Keep them from
# tripping ErrorActionPreference=Stop and decide success on the exit code alone.
$prevErrorAction = $ErrorActionPreference
$ErrorActionPreference = "Continue"
try {
    cmd /c $batPath
    $clangExit = $LASTEXITCODE
} finally {
    $ErrorActionPreference = $prevErrorAction
}
if ($clangExit -ne 0) { throw "clang++ build failed: $clangExit" }
if (-not (Test-Path (Join-Path $Build "voxel_engine.exe"))) { throw "voxel_engine.exe not produced" }

# Ensure runtime JSON sits beside exe
Copy-Item (Join-Path $Root "data\projectiles.json") (Join-Path $Build "projectiles.json") -Force
$weaponsSrc = Join-Path $Root "data\weapons"
$weaponsDst = Join-Path $Build "weapons"
if (Test-Path $weaponsSrc) {
  New-Item -ItemType Directory -Force -Path $weaponsDst | Out-Null
  Copy-Item (Join-Path $weaponsSrc "*") $weaponsDst -Force -Recurse
}

$exe = Get-Item (Join-Path $Build "voxel_engine.exe")
Write-Host "BUILD_OK $($exe.FullName) ($([math]::Round($exe.Length/1KB)) KB)" -ForegroundColor Green
exit 0
