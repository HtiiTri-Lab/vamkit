@echo off
rem Builds <repo>\out\cloth-physics\VaMClothPhysicsNative.dll: the fused solver's compute shaders
rem (..\..\solver\cloth_fused.hlsl) compiled into headers, then the DLL (static runtime), copied to .bin, the name the
rem plugin loads. Shader options (cs_5_0, /O3, JOINTS_IN_FLIGHT 1, COLLIDE_THREADS 256) are the ones the bit-identity
rem checks were run with. Needs VAMKIT_VCVARS (vcvars64.bat) and VAMKIT_FXC (the directory of fxc.exe).
setlocal
if not defined VAMKIT_VCVARS (echo set VAMKIT_VCVARS to vcvars64.bat & exit /b 1)
call "%VAMKIT_VCVARS%" >NUL 2>NUL || exit /b 1
set HERE=%~dp0
if not defined VAMKIT_FXC (echo set VAMKIT_FXC to the Windows SDK bin\^<version^>\x64 directory & exit /b 1)
for %%I in ("%HERE%..\..\..\..\..") do set REPO=%%~fI
set OUT=%REPO%\out\cloth-physics
set SRC="%HERE%..\..\solver\cloth_fused.hlsl"
set FXC="%VAMKIT_FXC%\fxc.exe" /nologo /T cs_5_0 /O3
if not exist "%OUT%\gen" mkdir "%OUT%\gen"
if not exist "%OUT%\obj" mkdir "%OUT%\obj"
%FXC% /E CSImport /Vn g_CSImport /Fh "%OUT%\gen\cloth_fused_import.h" %SRC% >NUL || exit /b 1
%FXC% /E CSIterate /D SHARED=1 /Vn g_CSIterateShared /Fh "%OUT%\gen\cloth_fused_iterate_shared.h" %SRC% >NUL || exit /b 1
%FXC% /E CSIterate /Vn g_CSIterate /Fh "%OUT%\gen\cloth_fused_iterate.h" %SRC% >NUL || exit /b 1
%FXC% /E CSIterateMulti /D MULTI=1 /Vn g_CSIterateMulti /Fh "%OUT%\gen\cloth_fused_iterate_multi.h" %SRC% >NUL || exit /b 1
%FXC% /E CSCollide /Vn g_CSCollide /Fh "%OUT%\gen\cloth_fused_collide.h" %SRC% >NUL || exit /b 1
cl /nologo /LD /O2 /MT /EHsc /std:c++17 /W3 /I"%OUT%\gen" /Fo"%OUT%\obj\\" /Fe"%OUT%\VaMClothPhysicsNative.dll" ^
   "%HERE%cloth_physics_native.cpp" /link d3d11.lib || exit /b 1
copy /y "%OUT%\VaMClothPhysicsNative.dll" "%OUT%\VaMClothPhysicsNative.bin" >NUL || exit /b 1
