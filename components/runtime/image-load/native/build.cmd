@echo off
rem Builds <repo>\out\image-load\ImageLoadWorkNative.dll and copies it to .bin, the name the plugin loads.
rem Needs VAMKIT_VCVARS (vcvars64.bat). Intermediate files go to out\image-load\obj.
setlocal
if not defined VAMKIT_VCVARS (echo set VAMKIT_VCVARS to vcvars64.bat & exit /b 1)
call "%VAMKIT_VCVARS%" >NUL 2>NUL || exit /b 1
set HERE=%~dp0
for %%I in ("%HERE%..\..\..\..") do set REPO=%%~fI
set OUT=%REPO%\out\image-load
if not exist "%OUT%\obj" mkdir "%OUT%\obj"
cd /d "%HERE%"
cl /nologo /O2 /EHsc /std:c++17 /LD imagework.cpp sourcework.cpp mipwork.cpp /Fo:"%OUT%\obj\\" /Fe:"%OUT%\ImageLoadWorkNative.dll" /link /NOLOGO kernel32.lib gdiplus.lib ole32.lib || exit /b 1
copy /y "%OUT%\ImageLoadWorkNative.dll" "%OUT%\ImageLoadWorkNative.bin" >NUL || exit /b 1
