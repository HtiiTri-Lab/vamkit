@echo off
rem Builds <repo>\out\d3d11-proxy\d3d11.dll: the D3D11On12 proxy with the desktop presenter. It compiles the OFXR
rem layer's d3d12_dlssg.cpp (..\ofxr-dlssg) and links the NGX SDK from <repo>\third_party\ngx-sdk.
rem Needs VAMKIT_VCVARS (vcvars64.bat).
setlocal
if not defined VAMKIT_VCVARS (echo set VAMKIT_VCVARS to vcvars64.bat & exit /b 1)
call "%VAMKIT_VCVARS%" >NUL 2>NUL || exit /b 1
set "HERE=%~dp0"
for %%I in ("%HERE%..\..\..") do set "REPO=%%~fI"
set "OFXR=%HERE%..\ofxr-dlssg"
set "NGX=%REPO%\third_party\ngx-sdk"
if not exist "%NGX%\include" (echo missing %NGX%\include & exit /b 1)
set "OUT=%REPO%\out\d3d11-proxy"
if not exist "%OUT%\obj" mkdir "%OUT%\obj"
ml64 /nologo /c /Fo"%OUT%\obj\d3d11_thunks.obj" "%HERE%d3d11_thunks.asm" >NUL || exit /b 1
cl /nologo /O2 /MT /std:c++20 /EHsc /W3 /LD /DNOMINMAX /DWIN32_LEAN_AND_MEAN /D_CRT_SECURE_NO_WARNINGS ^
   /Fo"%OUT%\obj\\" /I"%OFXR%\include" /I"%NGX%\include" /Fe"%OUT%\d3d11.dll" ^
   "%HERE%d3d11_proxy.cpp" "%HERE%desktop_presenter.cpp" "%OFXR%\src\d3d12\d3d12_dlssg.cpp" "%OUT%\obj\d3d11_thunks.obj" ^
   /link /DEF:"%HERE%d3d11.def" d3d12.lib dxgi.lib user32.lib advapi32.lib "%NGX%\lib\Windows_x86_64\x64\nvsdk_ngx_s.lib" || exit /b 1
echo built %OUT%\d3d11.dll
