@echo off
rem Builds <repo>\out\cpu-boost\VaMCpuBoostNative.dll (static runtime) and copies it to .bin, the name the plugin loads
rem (BepInEx reads every .dll under plugins as a .NET assembly). /fp:strict keeps every float operation as written (no
rem FMA contraction), which the bit-identical rules in cpuboost_native.cpp depend on. Needs VAMKIT_VCVARS (vcvars64.bat).
setlocal
if not defined VAMKIT_VCVARS (echo set VAMKIT_VCVARS to vcvars64.bat & exit /b 1)
call "%VAMKIT_VCVARS%" >NUL 2>NUL || exit /b 1
set HERE=%~dp0
for %%I in ("%HERE%..\..\..\..") do set REPO=%%~fI
set OUT=%REPO%\out\cpu-boost
if not exist "%OUT%\obj" mkdir "%OUT%\obj"
ml64 /nologo /c /Fo"%OUT%\obj\x87.obj" "%HERE%x87.asm" >NUL || exit /b 1
cl /nologo /LD /O2 /MT /EHsc /std:c++17 /fp:strict /W4 /Fo"%OUT%\obj\\" /Fe"%OUT%\VaMCpuBoostNative.dll" ^
   "%HERE%cpuboost_native.cpp" "%OUT%\obj\x87.obj" || exit /b 1
copy /y "%OUT%\VaMCpuBoostNative.dll" "%OUT%\VaMCpuBoostNative.bin" >NUL || exit /b 1
