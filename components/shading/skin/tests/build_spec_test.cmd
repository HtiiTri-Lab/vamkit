@echo off
rem Builds <repo>\out\skin-tests\spec_test.exe (offline test of the specular replacement programs). Needs VAMKIT_VCVARS
rem (vcvars64.bat). Run: spec_test.exe <repo>\out\gen\skin\spec_cases [hash ...] (tools\gen_spec_table.py makes them)
setlocal
if not defined VAMKIT_VCVARS (echo set VAMKIT_VCVARS to vcvars64.bat & exit /b 1)
call "%VAMKIT_VCVARS%" >NUL 2>NUL || exit /b 1
set HERE=%~dp0
for %%I in ("%HERE%..\..\..\..") do set REPO=%%~fI
set OUT=%REPO%\out\skin-tests
if not exist "%OUT%\obj" mkdir "%OUT%\obj"
cl /nologo /O2 /MT /std:c++17 /EHsc /W4 /WX /wd4100 /wd4201 /D_CRT_SECURE_NO_WARNINGS /Fo"%OUT%\obj\\" /Fe"%OUT%\spec_test.exe" ^
   "%HERE%spec_test.cpp" /link d3d11.lib dxguid.lib || exit /b 1
