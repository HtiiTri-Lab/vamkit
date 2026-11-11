@echo off
rem shadow_probe.exe into <repo>\out\rt-core-tests; run build.cmd first (it generates the shaders and fiber_check.h).
setlocal
if not defined VAMKIT_VCVARS (echo set VAMKIT_VCVARS & exit /b 1)
call "%VAMKIT_VCVARS%" >NUL 2>NUL || exit /b 1
set HERE=%~dp0
set CORE=%HERE%..
for %%I in ("%HERE%..\..\..\..") do set REPO=%%~fI
set NV=%REPO%\third_party\nvapi
set GEN=%REPO%\out\gen\rt-core
set OUT=%REPO%\out\rt-core-tests
pushd "%OUT%"
cl /nologo /O2 /MT /std:c++17 /utf-8 /EHsc /W4 /WX /wd4100 /D_CRT_SECURE_NO_WARNINGS /I"%CORE%\include" /I"%GEN%" /I"%OUT%" /I"%NV%" /Fe"shadow_probe.exe" "%HERE%shadow_probe.cpp" "%CORE%\src\rtcore.cpp" /link /LIBPATH:"%NV%\amd64" nvapi64.lib d3d12.lib d3dcompiler.lib dxgi.lib dxguid.lib
set RESULT=%errorlevel%
popd
exit /b %RESULT%
