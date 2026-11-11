@echo off
rem Builds <repo>\out\gc-tuning\VaMGcTuningNative.dll (static runtime; /EHa: a fault while marking is caught and
rem re-raised on the collecting thread) and copies it to .bin, the name the plugin loads. Needs VAMKIT_VCVARS.
setlocal
if not defined VAMKIT_VCVARS (echo set VAMKIT_VCVARS to vcvars64.bat & exit /b 1)
call "%VAMKIT_VCVARS%" >NUL 2>NUL || exit /b 1
set HERE=%~dp0
for %%I in ("%HERE%..\..\..\..") do set REPO=%%~fI
set OUT=%REPO%\out\gc-tuning
if not exist "%OUT%\obj" mkdir "%OUT%\obj"
cl /nologo /LD /O2 /MT /EHa /GS- /std:c++17 /Fo"%OUT%\obj\\" /Fe"%OUT%\VaMGcTuningNative.dll" "%HERE%gctuning.cpp" ^
   /link /NOLOGO kernel32.lib || exit /b 1
copy /y "%OUT%\VaMGcTuningNative.dll" "%OUT%\VaMGcTuningNative.bin" >NUL || exit /b 1
