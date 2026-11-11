@echo off
rem The PT offline probes: probe.exe (and shadow_probe.exe, build_shadow_probe.cmd) into <repo>\out\rt-core-tests.
rem Needs VAMKIT_VCVARS (vcvars64.bat) and VAMKIT_DXC (the Windows SDK bin\<version>\x64 directory).
rem Run: <repo>\out\rt-core-tests\probe.exe <output dir>
setlocal
if not defined VAMKIT_VCVARS (echo set VAMKIT_VCVARS & exit /b 1)
if not defined VAMKIT_DXC (echo set VAMKIT_DXC & exit /b 1)
call "%VAMKIT_VCVARS%" >NUL 2>NUL || exit /b 1
set HERE=%~dp0
set CORE=%HERE%..
for %%I in ("%HERE%..\..\..\..") do set REPO=%%~fI
set NV=%REPO%\third_party\nvapi
set GEN=%REPO%\out\gen\rt-core
set OUT=%REPO%\out\rt-core-tests
python -I "%CORE%\tools\gen_shaders.py" || exit /b 1
if not exist "%OUT%" mkdir "%OUT%"
"%VAMKIT_DXC%\dxc.exe" -nologo -T cs_6_5 -E FiberCheck -HV 2021 -O3 -WX -I "%NV%" -Fh "%OUT%\fiber_check.h" -Vn kFiberCheck "%HERE%fiber_check.hlsl" || exit /b 1
pushd "%OUT%"
cl /nologo /O2 /MT /std:c++17 /utf-8 /EHsc /W4 /WX /wd4100 /D_CRT_SECURE_NO_WARNINGS /I"%CORE%\include" /I"%GEN%" /I"%OUT%" /I"%NV%" /Fe"probe.exe" "%HERE%probe.cpp" "%CORE%\src\rtcore.cpp" /link /LIBPATH:"%NV%\amd64" nvapi64.lib d3d12.lib d3dcompiler.lib dxgi.lib dxguid.lib
set RESULT=%errorlevel%
popd
exit /b %RESULT%
