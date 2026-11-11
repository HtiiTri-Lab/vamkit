@echo off
rem Builds <repo>\out\hair-solver\VaMHairSolverNative.dll: the solver's compute shaders (..\..\solver\fused_strand.hlsl)
rem compiled into headers, then the DLL (static runtime), copied to .bin, the name the plugin loads.
rem Needs VAMKIT_VCVARS (vcvars64.bat) and VAMKIT_FXC (the directory of fxc.exe).
setlocal
if not defined VAMKIT_VCVARS (echo set VAMKIT_VCVARS to vcvars64.bat & exit /b 1)
call "%VAMKIT_VCVARS%" >NUL 2>NUL || exit /b 1
set HERE=%~dp0
if not defined VAMKIT_FXC (echo set VAMKIT_FXC to the Windows SDK bin\^<version^>\x64 directory & exit /b 1)
for %%I in ("%HERE%..\..\..\..\..") do set REPO=%%~fI
set OUT=%REPO%\out\hair-solver
set SRC="%HERE%..\..\solver\fused_strand.hlsl"
set FXC="%VAMKIT_FXC%\fxc.exe" /nologo /T cs_5_0 /O3 /WX
if not exist "%OUT%\gen" mkdir "%OUT%\gen"
if not exist "%OUT%\obj" mkdir "%OUT%\obj"
%FXC% /E CSStrand /Vn g_CSStrand /Fh "%OUT%\gen\fused_strand_strand.h" %SRC% >NUL || exit /b 1
%FXC% /E CSStep /Vn g_CSStep /Fh "%OUT%\gen\fused_strand_step.h" %SRC% >NUL || exit /b 1
cl /nologo /LD /O2 /MT /EHsc /std:c++17 /W3 /I"%OUT%\gen" /Fo"%OUT%\obj\\" /Fe"%OUT%\VaMHairSolverNative.dll" ^
   "%HERE%hair_solver_native.cpp" /link d3d11.lib || exit /b 1
copy /y "%OUT%\VaMHairSolverNative.dll" "%OUT%\VaMHairSolverNative.bin" >NUL || exit /b 1
