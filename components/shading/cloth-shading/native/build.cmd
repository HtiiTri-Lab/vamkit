@echo off
rem Builds <repo>\out\cloth-shading\VaMClothShadingNative.dll (static runtime, C++17, CINTERFACE) and copies it to .bin
rem (BepInEx reads every .dll under plugins as a .NET assembly; the plugin loads the module as .bin).
rem cloth_programs.h is read from <repo>\out\gen\cloth-shading: run python -I ..\tools\gen_cloth_programs.py first.
rem Needs VAMKIT_VCVARS (vcvars64.bat).
setlocal
if not defined VAMKIT_VCVARS (echo set VAMKIT_VCVARS & exit /b 1)
call "%VAMKIT_VCVARS%" >NUL 2>NUL || exit /b 1
set HERE=%~dp0
for %%I in ("%HERE%..\..\..\..") do set REPO=%%~fI
set GEN=%REPO%\out\gen\cloth-shading
set OUT=%REPO%\out\cloth-shading
if not exist "%GEN%\cloth_programs.h" (echo missing %GEN%\cloth_programs.h: run tools\gen_cloth_programs.py first & exit /b 1)
if not exist "%OUT%\obj" mkdir "%OUT%\obj"
cl /nologo /LD /O2 /MT /std:c++17 /EHsc /W4 /WX /wd4100 /wd4201 /I"%GEN%" /Fo"%OUT%\obj\\" /Fe"%OUT%\VaMClothShadingNative.dll" ^
   "%HERE%cloth_shading.cpp" /link d3d11.lib dxguid.lib bcrypt.lib psapi.lib || exit /b 1
copy /y "%OUT%\VaMClothShadingNative.dll" "%OUT%\VaMClothShadingNative.bin" > NUL || exit /b 1
