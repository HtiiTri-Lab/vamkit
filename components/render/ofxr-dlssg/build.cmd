@echo off
rem Builds the OFXR Bridge layer and tray (with DLSS Frame Generation) into <repo>\out\ofxr-dlssg\:
rem OFXRBridgeTray.exe and the ofxr\ folder. Needs VAMKIT_VCVARS (vcvars64.bat) and VAMKIT_FXC (Windows SDK bin\<ver>\x64).
rem OpenVR headers and the NGX SDK come from <repo>\third_party. Set VAMKIT_JOBS to change the parallel build count.
setlocal
if not defined VAMKIT_VCVARS (echo set VAMKIT_VCVARS to vcvars64.bat & exit /b 1)
if not defined VAMKIT_FXC (echo set VAMKIT_FXC to the Windows SDK bin\^<version^>\x64 folder holding fxc.exe & exit /b 1)
if not exist "%VAMKIT_FXC%\fxc.exe" goto nofxc
call "%VAMKIT_VCVARS%" >NUL 2>NUL || exit /b 1
set "HERE=%~dp0"
for %%I in ("%HERE%..\..\..") do set "REPO=%%~fI"
if not defined VAMKIT_JOBS set VAMKIT_JOBS=4
set "OUT=%REPO%\out\ofxr-dlssg"
set "B=%OUT%\build"
cmake -S "%HERE%." -B "%B%" -G Ninja -DCMAKE_BUILD_TYPE=Release -DXRFG_BUILD_TESTS=OFF || exit /b 1
cmake --build "%B%" --target OFXRBridgeTray --parallel %VAMKIT_JOBS% || exit /b 1
for %%F in ("%B%\OFXRBridgeTray.exe" "%B%\Release\OFXRBridgeTray.exe") do if exist "%%~F" set "TRAY=%%~F"
if not defined TRAY goto notray
for %%F in ("%TRAY%") do set "BIN=%%~dpF"
copy /y "%TRAY%" "%OUT%\" >NUL || exit /b 1
if not exist "%OUT%\ofxr" mkdir "%OUT%\ofxr"
xcopy /y /q "%BIN%ofxr\*" "%OUT%\ofxr\" >NUL || exit /b 1
echo built %OUT%
exit /b 0
:notray
echo OFXRBridgeTray.exe not found in the build folder
exit /b 1
:nofxc
echo fxc.exe not found in VAMKIT_FXC
exit /b 1
