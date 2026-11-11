@echo off
rem Builds OpenComposite with the VaM compositor work: <repo>\out\opencomposite\openvr_api.dll (the product vrclient_x64.dll,
rem installed as openvr_api.dll). Needs VAMKIT_VCVARS (vcvars64.bat) and VAMKIT_FXC (Windows SDK bin\<version>\x64, holds
rem fxc.exe). Third-party SDKs come from <repo>\third_party. Optional: VAMKIT_ATL_ROOT, the atlmfc folder, for a Visual
rem Studio without the ATL component. The Visual Studio generator is used (the project references a C# project).
setlocal
if not defined VAMKIT_VCVARS goto novc
if not defined VAMKIT_FXC goto nofxc
call "%VAMKIT_VCVARS%" >NUL 2>NUL || exit /b 1
set "HERE=%~dp0"
for %%I in ("%HERE%..\..\..") do set "REPO=%%~fI"
rem vcvars64.bat sits in <install>\VC\Auxiliary\Build; the generator instance is <install>.
for %%I in ("%VAMKIT_VCVARS%\..\..\..\..") do set "VSINST=%%~fI"
set "VSINST_CM=%VSINST:\=/%"
set "OUT=%REPO%\out\opencomposite"
if not defined VAMKIT_JOBS set VAMKIT_JOBS=4
cmake -S "%HERE%." -B "%OUT%\build" -G "Visual Studio 17 2022" -A x64 "-DCMAKE_GENERATOR_INSTANCE=%VSINST_CM%" || exit /b 1
cmake --build "%OUT%\build" --config Release --target OCOVR --parallel %VAMKIT_JOBS% -- /nodeReuse:false || exit /b 1
if not exist "%OUT%\build\bin\Release\vrclient_x64.dll" goto nodll
copy /y "%OUT%\build\bin\Release\vrclient_x64.dll" "%OUT%\openvr_api.dll" >NUL || exit /b 1
echo built %OUT%\openvr_api.dll
exit /b 0
:novc
echo set VAMKIT_VCVARS to vcvars64.bat
exit /b 1
:nofxc
echo set VAMKIT_FXC to the Windows SDK bin\^<version^>\x64 folder holding fxc.exe
exit /b 1
:nodll
echo vrclient_x64.dll was not produced
exit /b 1
