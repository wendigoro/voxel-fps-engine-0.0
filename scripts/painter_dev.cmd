@echo off
REM Voxel Painter → engine/painter build & smoke bridge.
REM Always rooted at this scripts\ folder. Java should launch THIS file via cmd.exe.
setlocal EnableExtensions
set "SCRIPTS=%~dp0"
REM strip trailing backslash for nicer logs
if "%SCRIPTS:~-1%"=="\" set "SCRIPTS=%SCRIPTS:~0,-1%"
for %%I in ("%SCRIPTS%\..") do set "ROOT=%%~fI"

cd /d "%ROOT%" || (
  echo ERROR: cannot cd to repo root "%ROOT%"
  exit /b 1
)

echo PAINTER_DEV.CMD ROOT=%ROOT%
echo PAINTER_DEV.CMD SCRIPTS=%SCRIPTS%
echo PAINTER_DEV.CMD ARGS=%*

set "PS_EXE=%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe"
if not exist "%PS_EXE%" set "PS_EXE=powershell.exe"

REM Prefer -File with absolute script path. ExecutionPolicy RemoteSigned is enough for local scripts;
REM fall back to Bypass only if needed via env VOXEL_PS_BYPASS=1.
set "PS_POLICY=RemoteSigned"
if /I "%VOXEL_PS_BYPASS%"=="1" set "PS_POLICY=Bypass"

"%PS_EXE%" -NoLogo -NoProfile -ExecutionPolicy %PS_POLICY% -File "%SCRIPTS%\painter_dev.ps1" %*
set "EC=%ERRORLEVEL%"
echo PAINTER_DEV.CMD EXIT=%EC%
exit /b %EC%
