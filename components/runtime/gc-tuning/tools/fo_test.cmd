@echo off
rem Builds and runs <repo>\out\gc-tuning\fo_test.exe: mono.dll's own finalizer test and the replacement, executed on
rem random values, must agree with the two formulas. Run native\build.cmd first (it makes VaMGcTuningNative.bin).
rem Needs VAMKIT_VCVARS and VAMKIT_VAM (the VaM folder).
setlocal
if not defined VAMKIT_VCVARS (echo set VAMKIT_VCVARS to vcvars64.bat & exit /b 1)
if not defined VAMKIT_VAM (echo set VAMKIT_VAM to the VaM folder & exit /b 1)
call "%VAMKIT_VCVARS%" >NUL 2>NUL || exit /b 1
set HERE=%~dp0
for %%I in ("%HERE%..\..\..\..") do set REPO=%%~fI
set OUT=%REPO%\out\gc-tuning
if not exist "%OUT%\obj" mkdir "%OUT%\obj"
cl /nologo /O2 /MT /EHa /std:c++17 /Fo"%OUT%\obj\\" /Fe"%OUT%\fo_test.exe" "%HERE%fo_test.cpp" /link /NOLOGO kernel32.lib >"%OUT%\fo_test-build.txt" || (type "%OUT%\fo_test-build.txt" & exit /b 1)
"%OUT%\fo_test.exe" "%OUT%\VaMGcTuningNative.bin" "%VAMKIT_VAM%\Mono\EmbedRuntime\mono.dll" "%OUT%"
