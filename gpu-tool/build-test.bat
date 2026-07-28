@echo off
setlocal
call "E:\Visual Studio\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d "%~dp0"
cl /nologo /W4 /EHsc /O2 /std:c++17 /DUNICODE /D_UNICODE CoreTest.cpp GpuCore.cpp /Fe:CoreTest.exe
if errorlevel 1 ( echo [error] build failed & exit /b 1 )
if exist CoreTest.obj del CoreTest.obj
if exist GpuCore.obj del GpuCore.obj
echo [ok] built CoreTest.exe
endlocal
