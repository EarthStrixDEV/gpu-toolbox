@echo off
REM ===========================================================================
REM  dist.bat - build everything and package it for end users
REM
REM  Produces a dist\ folder holding just the runnable pieces: the executables,
REM  the guide, and a desktop shortcut. Nothing in dist\ needs Visual Studio,
REM  the CUDA Toolkit, or the PowerShell scripts - the binaries depend only on
REM  Windows system DLLs and nvml.dll, which ships with the NVIDIA driver.
REM
REM  Copy dist\ to any Windows machine with an NVIDIA card and it runs as-is.
REM ===========================================================================

setlocal
cd /d "%~dp0"

REM  Each sub-script calls vcvars64.bat, which changes the working directory.
REM  Call them by absolute path and cd back afterwards so relative paths below
REM  keep resolving.
set "HERE=%~dp0"

echo === Building ===
call "%HERE%build.bat"
if errorlevel 1 goto :failed
cd /d "%HERE%"

call "%HERE%build-test.bat"
if errorlevel 1 goto :failed
cd /d "%HERE%"

call "%HERE%build-demo.bat"
if errorlevel 1 goto :failed
cd /d "%HERE%"

echo.
echo === Packaging ===

if not exist dist mkdir dist

copy /y GpuToolbox.exe      dist\ >nul
copy /y CoreTest.exe        dist\ >nul
copy /y GpuThrottleDemo.exe dist\ >nul
copy /y "..\GPU-Guide.txt"  dist\ >nul

REM  Shortcut creation needs a real scripting host; the one-liner below is the
REM  smallest reliable way to get one from a .bat file.
powershell -NoProfile -ExecutionPolicy Bypass -Command ^
  "$s=(New-Object -ComObject WScript.Shell).CreateShortcut((Join-Path (Resolve-Path 'dist') 'GPU Toolbox.lnk'));" ^
  "$s.TargetPath=(Join-Path (Resolve-Path 'dist') 'GpuToolbox.exe');" ^
  "$s.WorkingDirectory=(Resolve-Path 'dist').Path;" ^
  "$s.IconLocation=(Join-Path (Resolve-Path 'dist') 'GpuToolbox.exe')+',0';" ^
  "$s.Description='GPU Toolbox - NVIDIA GPU diagnostics (requires admin)';" ^
  "$s.Save()"

echo.
echo [ok] dist\ ready:
dir /b dist
echo.
echo Copy the dist folder anywhere. GpuToolbox.exe prompts for admin on launch.
goto :eof

:failed
echo.
echo [error] build failed - dist not updated.
exit /b 1
