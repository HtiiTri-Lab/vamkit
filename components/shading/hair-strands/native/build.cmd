@echo off
rem Builds <repo>\out\hair-strands\VaMHairOnePassNative.dll (static runtime, C++17, CINTERFACE) and copies it to .bin
rem (BepInEx reads every .dll under plugins as a .NET assembly; the plugin loads the module as .bin).
rem The generated headers are read from <repo>\out\gen\hair-strands: run tools\gen_programs.py,
rem tools\build_onepass_shaders.py and tools\build_strands_shaders.py first. Needs VAMKIT_VCVARS (vcvars64.bat).
setlocal
if not defined VAMKIT_VCVARS (echo set VAMKIT_VCVARS & exit /b 1)
call "%VAMKIT_VCVARS%" >NUL 2>NUL || exit /b 1
set HERE=%~dp0
for %%I in ("%HERE%..\..\..\..") do set REPO=%%~fI
set GEN=%REPO%\out\gen\hair-strands
set OUT=%REPO%\out\hair-strands
if not exist "%GEN%\hair_programs.h" (echo missing %GEN%\hair_programs.h: run the generators in tools first & exit /b 1)
if not exist "%OUT%\obj" mkdir "%OUT%\obj"
cl /nologo /LD /O2 /MT /std:c++17 /EHsc /W4 /WX /wd4100 /wd4201 /I"%GEN%" /Fo"%OUT%\obj\\" /Fe"%OUT%\VaMHairOnePassNative.dll" ^
   "%HERE%hair_onepass.cpp" /link d3d11.lib dxguid.lib bcrypt.lib psapi.lib || exit /b 1
copy /y "%OUT%\VaMHairOnePassNative.dll" "%OUT%\VaMHairOnePassNative.bin" > NUL || exit /b 1
