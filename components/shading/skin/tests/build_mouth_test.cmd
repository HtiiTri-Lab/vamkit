@echo off
rem Builds <repo>\out\skin-tests\mouth_test.exe. Needs VAMKIT_VCVARS (vcvars64.bat) and the generated
rem <repo>\out\gen\skin\mouth_shaders.h (tools\gen_mouth.py).
setlocal
if not defined VAMKIT_VCVARS (echo set VAMKIT_VCVARS to vcvars64.bat & exit /b 1)
call "%VAMKIT_VCVARS%" >NUL 2>NUL || exit /b 1
set HERE=%~dp0
for %%I in ("%HERE%..\..\..\..") do set REPO=%%~fI
set OUT=%REPO%\out\skin-tests
if not exist "%OUT%\obj" mkdir "%OUT%\obj"
cl /nologo /O2 /MT /std:c++17 /EHsc /W4 /WX /wd4100 /wd4201 /D_CRT_SECURE_NO_WARNINGS /I"%REPO%\out\gen\skin" /Fo"%OUT%\obj\\" /Fe"%OUT%\mouth_test.exe" "%HERE%mouth_test.cpp" "%HERE%..\native\mouth_bake.cpp" "%HERE%..\native\mouth_gpu.cpp" /link d3d11.lib dxguid.lib || exit /b 1
