@echo off
rem Builds <repo>\out\wrap-batch\VaMWrapBatchNative.dll (static runtime) and copies it to .bin, the name the plugin
rem loads. Needs VAMKIT_VCVARS (vcvars64.bat).
setlocal
if not defined VAMKIT_VCVARS (echo set VAMKIT_VCVARS to vcvars64.bat & exit /b 1)
call "%VAMKIT_VCVARS%" >NUL 2>NUL || exit /b 1
set HERE=%~dp0
for %%I in ("%HERE%..\..\..\..") do set REPO=%%~fI
set OUT=%REPO%\out\wrap-batch
if not exist "%OUT%\obj" mkdir "%OUT%\obj"
cl /nologo /LD /O2 /MT /EHsc /std:c++17 /W3 /Fo"%OUT%\obj\\" /Fe"%OUT%\VaMWrapBatchNative.dll" ^
   "%HERE%wrap_batch_native.cpp" /link d3d11.lib || exit /b 1
copy /y "%OUT%\VaMWrapBatchNative.dll" "%OUT%\VaMWrapBatchNative.bin" >NUL || exit /b 1
