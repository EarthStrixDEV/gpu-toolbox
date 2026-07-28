@echo off
setlocal
set "VCVARS=E:\Visual Studio\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" ( echo [error] vcvars64 not found & exit /b 1 )
call "%VCVARS%" >nul
cd /d "%~dp0"
cl /nologo /W4 /EHsc /O2 /std:c++17 GpuThrottleDemo.cpp /Fe:GpuThrottleDemo.exe
if errorlevel 1 ( echo [error] build failed & exit /b 1 )
if exist GpuThrottleDemo.obj del GpuThrottleDemo.obj
echo [ok] built GpuThrottleDemo.exe
endlocal
