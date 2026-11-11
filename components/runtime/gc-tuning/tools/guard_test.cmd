@echo off
rem Offline check of the 4 GB boundary guard against the user's own mono.dll: builds and runs
rem <repo>\out\gc-tuning\guard_test.exe. Needs VAMKIT_VCVARS and VAMKIT_VAM (the VaM folder).
setlocal
if not defined VAMKIT_VCVARS (echo set VAMKIT_VCVARS to vcvars64.bat & exit /b 1)
call "%VAMKIT_VCVARS%" >NUL 2>NUL || exit /b 1
set HERE=%~dp0
if not defined VAMKIT_VAM (echo set VAMKIT_VAM to the VaM folder & exit /b 1)
for %%I in ("%HERE%..\..\..\..") do set REPO=%%~fI
set OUT=%REPO%\out\gc-tuning
if not exist "%OUT%\obj" mkdir "%OUT%\obj"
cl /nologo /O2 /MT /EHa /GS- /std:c++17 /Fo"%OUT%\obj\\" /Fe"%OUT%\guard_test.exe" "%HERE%guard_test.cpp" ^
   /link /NOLOGO kernel32.lib >"%OUT%\guard_test-build.txt" || (type "%OUT%\guard_test-build.txt" & exit /b 1)
"%OUT%\guard_test.exe" "%VAMKIT_VAM%\Mono\EmbedRuntime\mono.dll"
