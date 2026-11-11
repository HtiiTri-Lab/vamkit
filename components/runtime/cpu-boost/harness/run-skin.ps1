# Builds the native library and SkinTest.exe, then runs the SkinMeshPart bit-for-bit check inside VaM's own Mono.
#     run-skin.ps1 [-Seed n] [-Cases n] [-Workers n]   (native pool size; 0 = calling thread only)
#     run-skin.ps1 -Workers 8 -Bench | -Ccd            (timings; -Ccd: caller pinned to each cache domain x worker placements)
param([uint32]$Seed = 2654435769, [int]$Cases = 6, [int]$Workers = 0, [switch]$Bench, [switch]$Ccd)
. (Join-Path $PSScriptRoot 'common.ps1')

Build-Native
Build-MonoHost
$exe = Join-Path $out 'SkinTest.exe'
& $csc /nologo /target:exe /optimize+ /unsafe /warn:4 ('/out:' + $exe) `
    ('/reference:' + (Join-Path $managed 'Assembly-CSharp.dll')) ('/reference:' + (Join-Path $managed 'UnityEngine.CoreModule.dll')) `
    ('/reference:' + (Join-Path $managed 'UnityEngine.dll')) `
    (Join-Path $PSScriptRoot 'SkinTest.cs') (Join-Path $root 'plugin\CpuBoostSkin.cs')
if ($LASTEXITCODE -ne 0) { throw 'SkinTest build failed' }
$extra = @()
if ($Bench) { $extra = @('bench') }
if ($Ccd) { $extra = @('ccd') }
& $hostExe $vam $exe $build $Seed $Cases $Workers @extra
exit $LASTEXITCODE
