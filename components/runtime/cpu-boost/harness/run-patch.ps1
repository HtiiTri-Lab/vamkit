# Builds the plugin (not installed) and PatchTest.exe, then runs the plugin's own Harmony patches against the stock
# methods inside VaM's own Mono: every patched method must give the unpatched method's bits. This is the check to run
# on a VaM build you have not seen before.
#     run-patch.ps1 [-Bench]
param([switch]$Bench)
. (Join-Path $PSScriptRoot 'common.ps1')

Build-Native
$plugin = Join-Path $build 'VaMCpuBoost.dll'
& $csc /nologo /target:library /optimize+ /warn:4 /unsafe ('/out:' + $plugin) `
    ('/reference:' + (Join-Path $vam 'BepInEx\core\BepInEx.dll')) ('/reference:' + (Join-Path $vam 'BepInEx\core\0Harmony.dll')) `
    ('/reference:' + (Join-Path $managed 'Assembly-CSharp.dll')) ('/reference:' + (Join-Path $managed 'UnityEngine.CoreModule.dll')) `
    ('/reference:' + (Join-Path $managed 'UnityEngine.dll')) `
    (Join-Path $root 'plugin\VaMCpuBoost.cs') (Join-Path $root 'plugin\CpuBoostSkin.cs') (Join-Path $root 'plugin\CpuBoostPaths.cs')
if ($LASTEXITCODE -ne 0) { throw 'VaMCpuBoost build failed' }
Build-MonoHost
$exe = Join-Path $out 'PatchTest.exe'
& $csc /nologo /target:exe /optimize+ /warn:4 ('/out:' + $exe) `
    ('/reference:' + (Join-Path $vam 'BepInEx\core\0Harmony.dll')) ('/reference:' + (Join-Path $managed 'Assembly-CSharp.dll')) `
    ('/reference:' + (Join-Path $managed 'UnityEngine.CoreModule.dll')) ('/reference:' + (Join-Path $managed 'UnityEngine.dll')) `
    (Join-Path $PSScriptRoot 'PatchTest.cs') (Join-Path $PSScriptRoot 'Worlds.cs')
if ($LASTEXITCODE -ne 0) { throw 'PatchTest build failed' }
$extra = @()
if ($Bench) { $extra = @('bench') }
& $hostExe $vam $exe $build $vam @extra
exit $LASTEXITCODE
