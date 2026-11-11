@echo off
rem Builds <repo>\out\scene-load\VaMSceneLoadNative.dll and copies it to .bin, the name the plugin loads.
rem Contents: one .var entry read + inflate (miniz tinfl); asset bundles prepared in memory (bundlecache.cpp);
rem DecompressOnLoad Vorbis clips -> PCM16 (vorbisdec.cpp + libvorbis 1.3.7 / libogg 1.3.5: the decoder, and the
rem encoder that remakes FMOD's setup headers); memstate.cpp: video memory in use.
rem Needs VAMKIT_VCVARS (vcvars64.bat). Intermediate files go to out\scene-load\obj.
setlocal
if not defined VAMKIT_VCVARS (echo set VAMKIT_VCVARS to vcvars64.bat & exit /b 1)
call "%VAMKIT_VCVARS%" >NUL 2>NUL || exit /b 1
set HERE=%~dp0
for %%I in ("%HERE%..\..\..\..") do set REPO=%%~fI
set OUT=%REPO%\out\scene-load
if not exist "%OUT%\obj" mkdir "%OUT%\obj"
cd /d "%HERE%"
set OGG=third_party\libogg-1.3.5
set VOR=third_party\libvorbis-1.3.7
set VSRC=%VOR%\lib\analysis.c %VOR%\lib\bitrate.c %VOR%\lib\block.c %VOR%\lib\codebook.c %VOR%\lib\envelope.c %VOR%\lib\floor0.c %VOR%\lib\floor1.c %VOR%\lib\info.c %VOR%\lib\lookup.c %VOR%\lib\lpc.c %VOR%\lib\lsp.c %VOR%\lib\mapping0.c %VOR%\lib\mdct.c %VOR%\lib\psy.c %VOR%\lib\registry.c %VOR%\lib\res0.c %VOR%\lib\sharedbook.c %VOR%\lib\smallft.c %VOR%\lib\synthesis.c %VOR%\lib\vorbisenc.c %VOR%\lib\window.c %OGG%\src\bitwise.c %OGG%\src\framing.c
cl /nologo /utf-8 /O2 /EHsc /GS- /std:c++17 /wd4244 /wd4267 /wd4305 /wd4996 /I"%OGG%\include" /I"%VOR%\include" /I"%VOR%\lib" /LD sceneload.cpp bundlecache.cpp vorbisdec.cpp memstate.cpp third_party\miniz\miniz.c %VSRC% /Fe:"%OUT%\VaMSceneLoadNative.dll" /Fo:"%OUT%\obj\\" /link /NOLOGO kernel32.lib dxgi.lib || exit /b 1
copy /y "%OUT%\VaMSceneLoadNative.dll" "%OUT%\VaMSceneLoadNative.bin" >NUL || exit /b 1
