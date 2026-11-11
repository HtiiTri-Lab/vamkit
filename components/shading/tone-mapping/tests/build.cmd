@echo off
rem Builds <repo>\out\tone-mapping\tm_test.exe and runs it on <repo>\out\tone-mapping\VaMToneMappingNative.dll (build it
rem first with native\build.cmd), style 0 and style 1. If a build of 0.1.0 is given as %1 (its VaMToneMappingNative.dll),
rem style 0's output must also be byte for byte that build's. Needs VAMKIT_VCVARS and a GPU.
setlocal
if not defined VAMKIT_VCVARS (echo set VAMKIT_VCVARS & exit /b 1)
call "%VAMKIT_VCVARS%" >NUL 2>NUL || exit /b 1
set HERE=%~dp0
for %%I in ("%HERE%..\..\..\..") do set REPO=%%~fI
set B=%REPO%\out\tone-mapping
if not exist "%B%\VaMToneMappingNative.dll" (echo missing %B%\VaMToneMappingNative.dll: run native\build.cmd first & exit /b 1)
if not exist "%B%\obj" mkdir "%B%\obj"
cl /nologo /O2 /MD /std:c++17 /EHsc /W3 /D_CRT_SECURE_NO_WARNINGS /Fo"%B%\obj\\" /Fe"%B%\tm_test.exe" "%HERE%tm_test.cpp" /link d3d11.lib || exit /b 1
"%B%\tm_test.exe" "%B%\VaMToneMappingNative.dll" --style 0 --dump "%B%\tm_out_s0.bin" || exit /b 1
"%B%\tm_test.exe" "%B%\VaMToneMappingNative.dll" --style 1 || exit /b 1
if "%~1"=="" (echo no 0.1.0 build given: style 0 not compared with it & exit /b 0)
"%B%\tm_test.exe" "%~1" --dump "%B%\tm_out_010.bin" >NUL || exit /b 1
fc /b "%B%\tm_out_s0.bin" "%B%\tm_out_010.bin" >NUL && echo style 0 output identical to 0.1.0 || (echo style 0 output DIFFERS from 0.1.0 & exit /b 1)
