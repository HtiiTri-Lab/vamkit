@echo off
rem Builds <repo>\out\cloth-shading\cloth_test.exe and runs it on the cases from tools\gen_cloth_programs.py
rem (<repo>\out\gen\cloth-shading\cloth_cases). Needs VAMKIT_VCVARS and a GPU. Extra arguments are program hashes.
setlocal
if not defined VAMKIT_VCVARS (echo set VAMKIT_VCVARS & exit /b 1)
call "%VAMKIT_VCVARS%" >NUL 2>NUL || exit /b 1
set HERE=%~dp0
for %%I in ("%HERE%..\..\..\..") do set REPO=%%~fI
set GEN=%REPO%\out\gen\cloth-shading
set OUT=%REPO%\out\cloth-shading
if not exist "%GEN%\cloth_cases\manifest.txt" (echo missing %GEN%\cloth_cases: run tools\gen_cloth_programs.py first & exit /b 1)
if not exist "%OUT%\obj_test" mkdir "%OUT%\obj_test"
cl /nologo /O2 /MT /std:c++17 /EHsc /W3 /D_CRT_SECURE_NO_WARNINGS /Fo"%OUT%\obj_test\\" /Fe"%OUT%\cloth_test.exe" ^
   "%HERE%cloth_test.cpp" /link d3d11.lib dxguid.lib || exit /b 1
"%OUT%\cloth_test.exe" "%GEN%" %* || exit /b 1
