@echo off
rem Builds <repo>\out\tone-mapping\VaMToneMappingNative.dll (static runtime, C++17) and copies it to .bin (BepInEx reads
rem every .dll under plugins as a .NET assembly; the plugin loads the module as .bin).
rem tm_shaders.h is read from <repo>\out\gen\tone-mapping: run python -I ..\tools\build_shaders.py first.
rem Needs VAMKIT_VCVARS (vcvars64.bat).
setlocal
if not defined VAMKIT_VCVARS (echo set VAMKIT_VCVARS & exit /b 1)
call "%VAMKIT_VCVARS%" >NUL 2>NUL || exit /b 1
set HERE=%~dp0
for %%I in ("%HERE%..\..\..\..") do set REPO=%%~fI
set GEN=%REPO%\out\gen\tone-mapping
set OUT=%REPO%\out\tone-mapping
if not exist "%GEN%\tm_shaders.h" (echo missing %GEN%\tm_shaders.h: run tools\build_shaders.py first & exit /b 1)
if not exist "%OUT%\obj" mkdir "%OUT%\obj"
cl /nologo /LD /O2 /MT /std:c++17 /EHsc /W4 /WX /wd4100 /I"%GEN%" /Fo"%OUT%\obj\\" /Fe"%OUT%\VaMToneMappingNative.dll" ^
   "%HERE%tonemap.cpp" /link d3d11.lib || exit /b 1
copy /y "%OUT%\VaMToneMappingNative.dll" "%OUT%\VaMToneMappingNative.bin" > NUL || exit /b 1
