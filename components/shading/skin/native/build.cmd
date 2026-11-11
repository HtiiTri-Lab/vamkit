@echo off
rem Builds <repo>\out\skin\VaMSkinTransmissionNative.dll (static runtime, C++17, CINTERFACE; rt.cpp: C++ COM, D3D12 +
rem 11on12). RtCore (..\..\rt-core\src\rtcore.cpp, its shaders in <repo>\out\gen\rt-core\rtc_shaders.h: python -I
rem ..\..\rt-core\tools\gen_shaders.py) is compiled on its own and linked in. The generated headers are read from
rem <repo>\out\gen\skin (run the generators first). Needs VAMKIT_VCVARS (vcvars64.bat).
setlocal
if not defined VAMKIT_VCVARS (echo set VAMKIT_VCVARS & exit /b 1)
call "%VAMKIT_VCVARS%" >NUL 2>NUL || exit /b 1
set HERE=%~dp0
for %%I in ("%HERE%..\..\..\..") do set REPO=%%~fI
set NV=%REPO%\third_party\nvapi
set RTC=%HERE%..\..\rt-core
set GEN=%REPO%\out\gen\skin
set GENRTC=%REPO%\out\gen\rt-core
set OUT=%REPO%\out\skin
if not exist "%OUT%\obj" mkdir "%OUT%\obj"
if not exist "%OUT%\obj_rtc" mkdir "%OUT%\obj_rtc"
cl /nologo /c /O2 /MT /std:c++17 /utf-8 /EHsc /W4 /WX /wd4100 /D_CRT_SECURE_NO_WARNINGS /I"%RTC%\include" /I"%GENRTC%" /I"%NV%" /Fo"%OUT%\obj_rtc\\" ^
   "%RTC%\src\rtcore.cpp" || exit /b 1
cl /nologo /LD /O2 /MT /std:c++17 /utf-8 /EHsc /W4 /WX /wd4100 /wd4201 /D_CRT_SECURE_NO_WARNINGS /I"%GEN%" /I"%NV%" /Fo"%OUT%\obj\\" ^
   /Fe"%OUT%\VaMSkinTransmissionNative.dll" "%HERE%skin_transmission.cpp" "%HERE%bake.cpp" "%HERE%mouth_bake.cpp" "%HERE%mouth_gpu.cpp" "%HERE%sss.cpp" "%HERE%rt.cpp" ^
   "%OUT%\obj_rtc\rtcore.obj" ^
   /link /LIBPATH:"%NV%\amd64" nvapi64.lib d3d11.lib d3d12.lib dxguid.lib bcrypt.lib psapi.lib || exit /b 1
rem BepInEx reads every .dll under plugins as a .NET assembly: the plugin loads the module as .bin
copy /y "%OUT%\VaMSkinTransmissionNative.dll" "%OUT%\VaMSkinTransmissionNative.bin" > NUL || exit /b 1
