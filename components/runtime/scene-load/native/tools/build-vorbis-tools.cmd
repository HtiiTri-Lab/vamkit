@echo off
rem Offline tool on libogg 1.3.5 + libvorbis 1.3.7 (native\third_party): vorbis_setups.exe (encoder setup headers by
rem CRC32, to match FMOD's FSB5 setup CRCs; its output goes through setup_index.py to native\vorbis_setup_index.h).
rem Needs VAMKIT_VCVARS (vcvars64.bat). Writes to <repo>\out\scene-load.
setlocal
if not defined VAMKIT_VCVARS (echo set VAMKIT_VCVARS to vcvars64.bat & exit /b 1)
call "%VAMKIT_VCVARS%" >NUL 2>NUL || exit /b 1
cd /d "%~dp0"
for %%I in ("..\..\..\..\..") do set REPO=%%~fI
set OUT=%REPO%\out\scene-load
if not exist "%OUT%\tools-obj" mkdir "%OUT%\tools-obj"
set OGG=..\third_party\libogg-1.3.5
set VOR=..\third_party\libvorbis-1.3.7
set VSRC=%VOR%\lib\analysis.c %VOR%\lib\bitrate.c %VOR%\lib\block.c %VOR%\lib\codebook.c %VOR%\lib\envelope.c %VOR%\lib\floor0.c %VOR%\lib\floor1.c %VOR%\lib\info.c %VOR%\lib\lookup.c %VOR%\lib\lpc.c %VOR%\lib\lsp.c %VOR%\lib\mapping0.c %VOR%\lib\mdct.c %VOR%\lib\psy.c %VOR%\lib\registry.c %VOR%\lib\res0.c %VOR%\lib\sharedbook.c %VOR%\lib\smallft.c %VOR%\lib\synthesis.c %VOR%\lib\vorbisenc.c %VOR%\lib\window.c
cl /nologo /O2 /wd4244 /wd4267 /wd4305 /wd4996 /I"%OGG%\include" /I"%VOR%\include" /I"%VOR%\lib" vorbis_setups.c %VSRC% %OGG%\src\bitwise.c %OGG%\src\framing.c /Fe:"%OUT%\vorbis_setups.exe" /Fo:"%OUT%\tools-obj\\" || exit /b 1
