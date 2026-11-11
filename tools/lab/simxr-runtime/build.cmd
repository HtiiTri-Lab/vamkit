@echo off
rem Builds the simulated headset into <repo>\out\lab\simxr\:
rem   simxr-openxr.dll    the OpenXR runtime (VirtualDesktop-OpenXR 1.1.0 built with SIMXR), with simxr-openxr.json
rem   LibOVRRT64_1.dll    the stand-in headset the runtime loads from its own folder
rem   simxr_selftest.exe  a small D3D11 OpenXR application that checks the two above
rem Needs VAMKIT_VCVARS (vcvars64.bat) and VAMKIT_FXC (the Windows SDK bin\<version>\x64 folder holding fxc.exe).
rem Takes from <repo>\third_party: libovr, vdxr-meta, vulkan-sdk, wil, gameinput, detours, ngx-sdk (see THIRD_PARTY.md).
rem No msbuild, no NuGet, no network. VAMKIT_JOBS sets the number of parallel compiles (default 4).
rem The compiler and linker options are the ones the upstream project files give for Release|x64.
setlocal
if not defined VAMKIT_VCVARS (echo set VAMKIT_VCVARS to vcvars64.bat & exit /b 1)
if not defined VAMKIT_FXC (echo set VAMKIT_FXC to the Windows SDK bin\^<version^>\x64 folder holding fxc.exe & exit /b 1)
if not exist "%VAMKIT_FXC%\fxc.exe" goto nofxc
call "%VAMKIT_VCVARS%" >NUL 2>NUL || exit /b 1
set "FXC=%VAMKIT_FXC%\fxc.exe"
set "HERE=%~dp0"
for %%I in ("%HERE%..\..\..") do set "REPO=%%~fI"
if not defined VAMKIT_JOBS set VAMKIT_JOBS=4
set "TP=%REPO%\third_party"
set "EXT=%HERE%external"
set "OUT=%REPO%\out\lab\simxr"
set "B=%OUT%\build"

set "NEED=%TP%\libovr\Include\OVR_CAPI.h"
if not exist "%NEED%" goto missing
set "NEED=%TP%\libovr\Shim\OVR_CAPI_Util.cpp"
if not exist "%NEED%" goto missing
set "NEED=%TP%\vdxr-meta\virtualdesktop-openxr\OVR_CAPIShim.c"
if not exist "%NEED%" goto missing
set "NEED=%TP%\vdxr-meta\virtualdesktop-openxr\meta_body_tracking_full_body.h"
if not exist "%NEED%" goto missing
set "NEED=%TP%\vulkan-sdk\include\vulkan\vulkan.h"
if not exist "%NEED%" goto missing
set "NEED=%TP%\vulkan-sdk\lib\vulkan-1.lib"
if not exist "%NEED%" goto missing
set "NEED=%TP%\wil\include\wil\resource.h"
if not exist "%NEED%" goto missing
set "NEED=%TP%\gameinput\include\GameInput.h"
if not exist "%NEED%" goto missing
set "NEED=%TP%\gameinput\lib\x64\GameInput.lib"
if not exist "%NEED%" goto missing
set "NEED=%TP%\detours\include\detours.h"
if not exist "%NEED%" goto missing
set "NEED=%TP%\detours\libs\x64\detours.lib"
if not exist "%NEED%" goto missing
set "NEED=%TP%\ngx-sdk\include\nvsdk_ngx.h"
if not exist "%NEED%" goto missing
set "NEED=%TP%\ngx-sdk\lib\Windows_x86_64\x64\nvsdk_ngx_s.lib"
if not exist "%NEED%" goto missing

if not exist "%B%\hmd" mkdir "%B%\hmd"
if not exist "%B%\runtime" mkdir "%B%\runtime"
if not exist "%B%\selftest" mkdir "%B%\selftest"
del /q "%B%\hmd\*.obj" "%B%\runtime\*.obj" "%B%\selftest\*.obj" 2>NUL

rem ---------------------------------------------------------------- the stand-in headset: LibOVRRT64_1.dll
set "O=%B%\hmd"
set "S=%HERE%hmd"
for %%N in (ReprojectVS CaptureVS) do "%FXC%" /nologo /E main /T vs_5_0 /D HLSL /Fh "%O%\%%N.h" /Vn k_%%N "%S%\%%N.hlsl" >NUL || goto failed
for %%N in (ReprojectPS CapturePS) do "%FXC%" /nologo /E main /T ps_5_0 /D HLSL /Fh "%O%\%%N.h" /Vn k_%%N "%S%\%%N.hlsl" >NUL || goto failed

set "R=%O%\cl.rsp"
>  "%R%" echo /c /Zi /FS /nologo /W3 /WX- /diagnostics:column /O2 /Oi /GL
>> "%R%" echo /D OVR_DLL_BUILD /D _SILENCE_CXX17_CODECVT_HEADER_DEPRECATION_WARNING /D _CRT_SECURE_NO_WARNINGS
>> "%R%" echo /D NDEBUG /D _WINDOWS /D _USRDLL /D _WINDLL /D _UNICODE /D UNICODE
>> "%R%" echo /Gm- /EHsc /MT /GS /Gy /fp:precise /Zc:wchar_t /Zc:forScope /Zc:inline /permissive-
>> "%R%" echo /external:W3 /Gd /FC /utf-8
>> "%R%" echo /I"%O%" /I"%S%"
>> "%R%" echo /I"%TP%\libovr\Include" /I"%TP%\libovr\Include\Extras"
>> "%R%" echo /I"%EXT%\cJSON" /I"%TP%\wil\include"
>> "%R%" echo /Fo"%O%\\" /Fd"%O%\vc143.pdb"

pushd "%S%"
cl @"%R%" /sdl /TP /std:c++17 /Yc"pch.h" /Fp"%O%\pch.pch" pch.cpp || goto failedpop
cl @"%R%" /sdl /TP /std:c++17 /Yu"pch.h" /Fp"%O%\pch.pch" /MP%VAMKIT_JOBS% ^
   api.cpp control.cpp dllmain.cpp driver.cpp log.cpp sim.cpp stubs.cpp || goto failedpop
cl @"%R%" /sdl /TP /std:c++17 /MP%VAMKIT_JOBS% ^
   "%TP%\libovr\Shim\OVR_CAPI_Util.cpp" "%TP%\libovr\Shim\OVR_StereoProjection.cpp" || goto failedpop
cl @"%R%" /sdl- /TC "%EXT%\cJSON\cJSON.c" || goto failedpop
popd

link /OUT:"%OUT%\LibOVRRT64_1.dll" /NOLOGO ^
   RuntimeObject.lib dxgi.lib dxguid.lib d3d11.lib winmm.lib ws2_32.lib windowscodecs.lib ole32.lib shell32.lib ^
   kernel32.lib user32.lib gdi32.lib winspool.lib comdlg32.lib advapi32.lib oleaut32.lib uuid.lib odbc32.lib odbccp32.lib ^
   /MANIFEST /MANIFESTUAC:NO /manifest:embed /DEBUG /PDB:"%OUT%\LibOVRRT64_1.pdb" /MAP:"%O%\LibOVRRT64_1.map" ^
   /IMPLIB:"%O%\LibOVRRT64_1.lib" /SUBSYSTEM:WINDOWS /OPT:REF /OPT:ICF ^
   /LTCG:incremental /LTCGOUT:"%O%\LibOVRRT64_1.iobj" /TLBID:1 /DYNAMICBASE /NXCOMPAT ^
   /MACHINE:X64 /DLL "%O%\*.obj" || goto failed

rem ---------------------------------------------------------------- the runtime: simxr-openxr.dll
set "O=%B%\runtime"
set "S=%HERE%runtime"
set "FXI=/I "%EXT%\FidelityFX-FSR\ffx-fsr" /I "%EXT%\FidelityFX-CAS\ffx-cas""
for %%N in (AlphaBlendingCS SharpeningCS UpscalingCS) do "%FXC%" /nologo /E main /T cs_5_0 %FXI% /Fh "%O%\%%N.h" /Vn g_%%N "%S%\%%N.hlsl" >NUL || goto failed
for %%N in (DlssnrFeatherPS ResolveMultisampledDepthPS) do "%FXC%" /nologo /E main /T ps_5_0 %FXI% /Fh "%O%\%%N.h" /Vn g_%%N "%S%\%%N.hlsl" >NUL || goto failed
"%FXC%" /nologo /E main /T vs_5_0 %FXI% /Fh "%O%\FullScreenQuadVS.h" /Vn g_FullScreenQuadVS "%S%\FullScreenQuadVS.hlsl" >NUL || goto failed

rc /D _UNICODE /D UNICODE /l"0x0409" /nologo /fo"%O%\resource.res" "%S%\resource.rc" || goto failed

set "R=%O%\cl.rsp"
>  "%R%" echo /c /Zi /FS /nologo /W3 /WX- /diagnostics:column /sdl /O2 /Oi /GL
>> "%R%" echo /D USING_GAMEINPUT /D RUNTIME_NAMESPACE=virtualdesktop_openxr /D SIMXR /D LOAD_LOCAL_LIBOVR
>> "%R%" echo /D NDEBUG /D _WINDOWS /D _USRDLL /D _CRT_SECURE_NO_WARNINGS /D _WINDLL /D _UNICODE /D UNICODE
>> "%R%" echo /Gm- /EHsc /MT /GS /Gy /fp:precise /Zc:wchar_t /Zc:forScope /Zc:inline /permissive-
>> "%R%" echo /external:W3 /Gd /FC
>> "%R%" echo /I"%TP%\gameinput\include" /I"%TP%\detours\include" /I"%TP%\wil\include"
>> "%R%" echo /I"%O%" /I"%S%" /I"%TP%\vdxr-meta\virtualdesktop-openxr"
>> "%R%" echo /I"%EXT%\OpenXR-SDK\include"
>> "%R%" echo /I"%EXT%\OpenXR-MixedReality\shared" /I"%EXT%\OpenXR-MixedReality\shared\XrUtility"
>> "%R%" echo /I"%EXT%\OpenXR-MixedReality\shared\SampleShared"
>> "%R%" echo /I"%TP%\libovr\Include" /I"%TP%\libovr\Include\Extras" /I"%TP%\libovr\Shim"
>> "%R%" echo /I"%TP%\vulkan-sdk\include" /I"%EXT%\OpenGL" /I"%EXT%\fmt\include"
>> "%R%" echo /I"%EXT%\openvr\samples\drivers\drivers\handskeletonsimulation\src" /I"%EXT%\openvr\headers"
>> "%R%" echo /I"%EXT%\openvr\samples\drivers\utils\vrmath"
>> "%R%" echo /I"%EXT%\FidelityFX-FSR\ffx-fsr" /I"%EXT%\FidelityFX-CAS\ffx-cas"
>> "%R%" echo /I"%EXT%\cJSON" /I"%EXT%\d3dx12" /I"%TP%\ngx-sdk\include"
>> "%R%" echo /Fo"%O%\\" /Fd"%O%\vc143.pdb"

pushd "%S%"
cl @"%R%" /TP /std:c++17 /Yc"pch.h" /Fp"%O%\pch.pch" pch.cpp || goto failedpop
cl @"%R%" /TP /std:c++17 /Yu"pch.h" /Fp"%O%\pch.pch" /MP%VAMKIT_JOBS% ^
   accessibility.cpp action.cpp audio.cpp body_tracking.cpp d3d11_native.cpp d3d12_interop.cpp ^
   display_refresh_rate.cpp dlssnr.cpp eye_tracking.cpp face_tracking.cpp frame.cpp ^
   framework\dispatch.cpp framework\dispatch.gen.cpp framework\entry.cpp hand_tracking.cpp instance.cpp ^
   log.cpp mappings.cpp opengl_interop.cpp perf_counter.cpp mirror_window.cpp precompositor.cpp session.cpp ^
   space.cpp swapchain.cpp system.cpp visibility_mask.cpp vulkan_interop.cpp simxr.cpp || goto failedpop
cl @"%R%" /TP /std:c++17 /MP%VAMKIT_JOBS% ^
   "%TP%\libovr\Shim\OVR_CAPI_Util.cpp" "%TP%\libovr\Shim\OVR_StereoProjection.cpp" || goto failedpop
cl @"%R%" /TP /std:c++17 /wd4244 /wd4305 ^
   "%EXT%\openvr\samples\drivers\drivers\handskeletonsimulation\src\hand_simulation.cpp" || goto failedpop
cl @"%R%" /TC /MP%VAMKIT_JOBS% "%EXT%\cJSON\cJSON.c" "%TP%\vdxr-meta\virtualdesktop-openxr\OVR_CAPIShim.c" || goto failedpop
popd

link /OUT:"%OUT%\simxr-openxr.dll" /NOLOGO ^
   /LIBPATH:"%TP%\gameinput\lib\x64" /LIBPATH:"%TP%\detours\libs\x64" /LIBPATH:"%TP%\vulkan-sdk\lib" ^
   /LIBPATH:"%TP%\ngx-sdk\lib\Windows_x86_64\x64" ^
   detours.lib dxgi.lib dxguid.lib d3d11.lib d3d12.lib vulkan-1.lib opengl32.lib ntdll.lib nvsdk_ngx_s.lib ^
   kernel32.lib user32.lib gdi32.lib winspool.lib comdlg32.lib advapi32.lib shell32.lib ole32.lib oleaut32.lib ^
   uuid.lib odbc32.lib odbccp32.lib GameInput.lib ^
   /DEF:"%S%\virtualdesktop-openxr.def" /MANIFEST /MANIFESTUAC:NO /manifest:embed ^
   /DEBUG /PDB:"%OUT%\simxr-openxr.pdb" /MAP:"%O%\simxr-openxr.map" /IMPLIB:"%O%\simxr-openxr.lib" ^
   /SUBSYSTEM:WINDOWS /OPT:REF /OPT:ICF /LTCG:incremental /LTCGOUT:"%O%\simxr-openxr.iobj" ^
   /TLBID:1 /DYNAMICBASE /NXCOMPAT /MACHINE:X64 /DLL ^
   "%O%\*.obj" "%O%\resource.res" || goto failed
copy /y "%S%\simxr-openxr.json" "%OUT%\" >NUL || goto failed

rem ---------------------------------------------------------------- the self-test: simxr_selftest.exe
set "O=%B%\selftest"
set "S=%HERE%selftest"
cl /c /Zi /FS /nologo /W3 /WX- /diagnostics:column /sdl /O2 /D _CRT_SECURE_NO_WARNINGS /D NDEBUG /D _UNICODE /D UNICODE ^
   /Gm- /EHsc /MT /GS /fp:precise /Zc:wchar_t /Zc:forScope /Zc:inline /std:c++17 /permissive- /external:W3 /Gd /TP /FC ^
   /I"%EXT%\OpenXR-SDK\include" /Fo"%O%\\" /Fd"%O%\vc143.pdb" "%S%\simxr_selftest.cpp" || goto failed
link /OUT:"%OUT%\simxr_selftest.exe" /NOLOGO d3d11.lib dxgi.lib ^
   kernel32.lib user32.lib gdi32.lib winspool.lib comdlg32.lib advapi32.lib shell32.lib ole32.lib oleaut32.lib ^
   uuid.lib odbc32.lib odbccp32.lib ^
   /MANIFEST "/MANIFESTUAC:level='asInvoker' uiAccess='false'" /manifest:embed /DEBUG:FULL /INCREMENTAL:NO ^
   /PDB:"%O%\simxr_selftest.pdb" /SUBSYSTEM:CONSOLE /TLBID:1 /DYNAMICBASE /NXCOMPAT /MACHINE:X64 ^
   "%O%\simxr_selftest.obj" || goto failed

echo built %OUT%
exit /b 0

:failedpop
popd
:failed
echo simxr-runtime build failed
exit /b 1
:missing
echo missing %NEED%
echo the simulated headset needs these folders in third_party: libovr, vdxr-meta, vulkan-sdk, wil, gameinput, detours, ngx-sdk
exit /b 1
:nofxc
echo fxc.exe not found in VAMKIT_FXC
exit /b 1
