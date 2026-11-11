# Builds the native library and PathsTest.exe, then runs the post-skin normals / soft-body vertices / Math.Pow
# bit-for-bit checks inside VaM's own Mono.
#     run-paths.ps1 [-Seed n] [-Cases n] [-Workers n] [-Bench]
param([uint32]$Seed = 2654435769, [int]$Cases = 8, [int]$Workers = 0, [switch]$Bench)
. (Join-Path $PSScriptRoot 'common.ps1')

Build-Native
Build-MonoHost
$exe = Join-Path $out 'PathsTest.exe'
& $csc /nologo /target:exe /optimize+ /unsafe /warn:4 ('/out:' + $exe) `
    ('/reference:' + (Join-Path $managed 'Assembly-CSharp.dll')) ('/reference:' + (Join-Path $managed 'UnityEngine.CoreModule.dll')) `
    ('/reference:' + (Join-Path $managed 'UnityEngine.dll')) `
    (Join-Path $PSScriptRoot 'PathsTest.cs') (Join-Path $PSScriptRoot 'Worlds.cs') (Join-Path $root 'plugin\CpuBoostSkin.cs') (Join-Path $root 'plugin\CpuBoostPaths.cs')
if ($LASTEXITCODE -ne 0) { throw 'PathsTest build failed' }
$extra = @()
if ($Bench) { $extra = @('bench') }
& $hostExe $vam $exe $build $Seed $Cases $Workers @extra
exit $LASTEXITCODE
