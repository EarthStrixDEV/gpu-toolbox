@echo off
REM ===========================================================================
REM  Build GpuToolbox.exe with MSVC
REM
REM  vcvars64.bat sets up the compiler environment. It is hardcoded to the
REM  VS install found on this machine (E:\Visual Studio); adjust VSPATH if
REM  Visual Studio moves.
REM ===========================================================================

setlocal

set "VSPATH=E:\Visual Studio"
set "VCVARS=%VSPATH%\VC\Auxiliary\Build\vcvars64.bat"

if not exist "%VCVARS%" (
    echo [error] vcvars64.bat not found at:
    echo         %VCVARS%
    echo Edit VSPATH in this file to point at your Visual Studio install.
    exit /b 1
)

call "%VCVARS%" >nul
if errorlevel 1 (
    echo [error] failed to initialise the MSVC environment.
    exit /b 1
)

cd /d "%~dp0"

REM  /W4      high warning level
REM  /EHsc    standard C++ exception model
REM  /O2      optimise for speed
REM  /std:c++17 for the std::thread / atomic usage
REM  /DUNICODE /D_UNICODE  wide-char Win32 API throughout
REM  /SUBSYSTEM:WINDOWS  GUI app, no console window
REM  GpuCore.cpp holds the native ports of the three PowerShell scripts.
REM
REM  /MANIFEST:EMBED + /MANIFESTINPUT embeds GpuToolbox.manifest directly into
REM  the exe. That manifest requests requireAdministrator, so Windows prompts
REM  for elevation before the process starts. Embedding matters: a loose
REM  .exe.manifest file next to the binary is ignored once the exe carries its
REM  own, and is easy to lose when copying the exe elsewhere.
REM  Compile the resource script first: it carries the application icon and the
REM  version info shown in the file properties dialog.
rc /nologo /fo GpuToolbox.res GpuToolbox.rc
if errorlevel 1 (
    echo [error] resource compile failed.
    exit /b 1
)

REM  /MANIFESTUAC:NO is required, not optional: without it the linker emits its
REM  own trustInfo block defaulting to asInvoker, which collides with the
REM  requireAdministrator level in our manifest and fails the build with
REM  "manifest authoring error c1010001: Values of attribute level not equal".
cl /nologo /W4 /EHsc /O2 /std:c++17 /DUNICODE /D_UNICODE ^
   GpuToolbox.cpp GpuCore.cpp GpuToolbox.res ^
   /Fe:GpuToolbox.exe ^
   /link /SUBSYSTEM:WINDOWS ^
   /MANIFEST:EMBED /MANIFESTUAC:NO /MANIFESTINPUT:GpuToolbox.manifest

if errorlevel 1 (
    echo.
    echo [error] build failed.
    exit /b 1
)

REM  Clean up the intermediate object files.
if exist GpuToolbox.obj del GpuToolbox.obj
if exist GpuCore.obj del GpuCore.obj
if exist GpuToolbox.res del GpuToolbox.res

echo.
echo [ok] built: %~dp0GpuToolbox.exe
endlocal
