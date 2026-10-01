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
# A failed compile must fail the build: otherwise the previous .spv stays in
# build/shaders and the engine silently runs the old shader.
function Compile-Shader([string]$Name) {
  & $glslc (Join-Path $Root "shaders\$Name") -o (Join-Path $Build "shaders\$Name.spv")
  if ($LASTEXITCODE -ne 0) { throw "glslc failed for $Name ($LASTEXITCODE)" }
}
Compile-Shader "voxel.vert"
Compile-Shader "voxel.frag"
if (Test-Path (Join-Path $Root "shaders\post.vert")) {
  Compile-Shader "post.vert"
  Compile-Shader "post.frag"
}

Write-Host "== compile engine (Clang + VS env) ==" -ForegroundColor Cyan
if (-not (Test-Path $VCVARS)) { throw "vcvars64.bat missing: $VCVARS" }
if (-not (Test-Path (Join-Path $LLVM "clang++.exe"))) { throw "clang++ missing under $LLVM" }

# Dear ImGui (third_party/imgui, MIT) is compiled once into build/imgui/*.o and
# only recompiled when a source is newer than its object, so the engine build
# does not pay for ~2 MB of third-party source every time.
$ImguiDir = Join-Path $Root "third_party\imgui"
$ImguiObjDir = Join-Path $Build "imgui"
New-Item -ItemType Directory -Force -Path $ImguiObjDir | Out-Null
$imguiSources = @("imgui.cpp", "imgui_draw.cpp", "imgui_tables.cpp", "imgui_widgets.cpp",
                  "backends\imgui_impl_vulkan.cpp", "backends\imgui_impl_win32.cpp")
$imguiCompile = @()
$imguiObjects = @()
foreach ($rel in $imguiSources) {
  $srcFile = Join-Path $ImguiDir $rel
  $objFile = Join-Path $ImguiObjDir ([IO.Path]::GetFileNameWithoutExtension($rel) + ".o")
  $imguiObjects += "`"$objFile`""
  if (-not (Test-Path $objFile) -or (Get-Item $srcFile).LastWriteTime -gt (Get-Item $objFile).LastWriteTime) {
    $imguiCompile += "clang++.exe -std=c++17 -O2 -D_CRT_SECURE_NO_WARNINGS -I`"$ImguiDir`" -I`"$VK\Include`" -c `"$srcFile`" -o `"$objFile`" || exit /b 1"
  }
}
$imguiCompileLines = $imguiCompile -join "`r`n"
$imguiObjectList = $imguiObjects -join " "

$bat = @"
@echo off
REM vcvars64.bat probes for vswhere.exe and prints a benign "not recognized"
REM warning on stderr when it is absent. That must not abort the build, so
REM both streams are discarded and the real clang exit code is what counts.
call "$VCVARS" >nul 2>&1
set "PATH=$LLVM;%PATH%"
$imguiCompileLines
clang++.exe -std=c++17 -O2 -g -D_CRT_SECURE_NO_WARNINGS -I"$Src" -I"$ImguiDir" -I"$VK\Include" "$Src\main.cpp" $imguiObjectList -o "$Build\voxel_engine.exe" -L"$VK\Lib" -lvulkan-1 -luser32 -lgdi32 -lshell32 -ldwmapi
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
# voxfmt MAP fixtures (authored in painter map mode, loaded by map_vox.hpp)
$voxfmtSrc = Join-Path $Root "data\voxfmt"
$voxfmtDst = Join-Path $Build "voxfmt"
if (Test-Path $voxfmtSrc) {
  New-Item -ItemType Directory -Force -Path $voxfmtDst | Out-Null
  Copy-Item (Join-Path $voxfmtSrc "*") $voxfmtDst -Force -Recurse
}
$weaponsSrc = Join-Path $Root "data\weapons"
$weaponsDst = Join-Path $Build "weapons"
if (Test-Path $weaponsSrc) {
  New-Item -ItemType Directory -Force -Path $weaponsDst | Out-Null
  Copy-Item (Join-Path $weaponsSrc "*") $weaponsDst -Force -Recurse
}
# Inventory item defs (RULES.md rule 12: unit=1, voxel_size=0.001)
$itemsSrc = Join-Path $Root "data\items"
$itemsDst = Join-Path $Build "items"
if (Test-Path $itemsSrc) {
  New-Item -ItemType Directory -Force -Path $itemsDst | Out-Null
  Copy-Item (Join-Path $itemsSrc "*") $itemsDst -Force -Recurse
}

$exe = Get-Item (Join-Path $Build "voxel_engine.exe")
Write-Host "BUILD_OK $($exe.FullName) ($([math]::Round($exe.Length/1KB)) KB)" -ForegroundColor Green
exit 0
