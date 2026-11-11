# Builds and runs the float-rule experiment: MonoFloatTest.exe inside VaM's own Mono (through mono_host.exe), then
# float_check.exe on its output. It shows how this Mono evaluates float arithmetic (widening, where it rounds, x87
# sin / cos): the rules cpuboost_native.cpp is written to. No game, no GPU.
# Needs VAMKIT_VAM, VAMKIT_VCVARS, VAMKIT_CSC.
$ErrorActionPreference = 'Stop'
foreach ($name in 'VAMKIT_VAM', 'VAMKIT_VCVARS', 'VAMKIT_CSC') {
    if (-not [Environment]::GetEnvironmentVariable($name)) { throw "set $name" }
}
$here = $PSScriptRoot
$repo = [IO.Path]::GetFullPath((Join-Path $here '..\..\..\..\..'))
$vam = $env:VAMKIT_VAM
$vcvars = $env:VAMKIT_VCVARS
$csc = $env:VAMKIT_CSC
$out = Join-Path $repo 'out\cpu-boost\floattest'
[void](New-Item -ItemType Directory -Path $out -Force)
$managed = Join-Path $vam 'VaM_Data\Managed'

$hostExe = Join-Path $out 'mono_host.exe'
$hostSrc = Join-Path $here '..\mono_host.c'
cmd /c "call `"$vcvars`" >nul && cl /nologo /O2 /W3 /Fo`"$out\\`" /Fe`"$hostExe`" `"$hostSrc`"" | Out-Host
if ($LASTEXITCODE -ne 0) { throw 'mono_host build failed' }
$check = Join-Path $out 'float_check.exe'
cmd /c "call `"$vcvars`" >nul && ml64 /nologo /c /Fo`"$out\x87.obj`" `"$here\x87.asm`" && cl /nologo /O2 /fp:precise /EHsc /W3 /D_CRT_SECURE_NO_WARNINGS /Fo`"$out\\`" /Fe`"$check`" `"$here\float_check.cpp`" `"$out\x87.obj`"" | Out-Host
if ($LASTEXITCODE -ne 0) { throw 'float_check build failed' }

$exe = Join-Path $out 'MonoFloatTest.exe'
& $csc /nologo /target:exe /optimize+ /warn:4 ('/out:' + $exe) ('/reference:' + (Join-Path $managed 'UnityEngine.CoreModule.dll')) (Join-Path $here 'MonoFloatTest.cs')
if ($LASTEXITCODE -ne 0) { throw 'MonoFloatTest build failed' }
Copy-Item (Join-Path $managed 'UnityEngine.CoreModule.dll') $out -Force

$data = Join-Path $out 'float.bin'
& $hostExe $vam $exe $data
if ($LASTEXITCODE -ne 0) { throw "MonoFloatTest exit $LASTEXITCODE" }
& $check $data
