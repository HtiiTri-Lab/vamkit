# Builds the plugin (not installed) and PatchTest.exe, then runs the plugin's own Harmony patches against the stock
# methods inside VaM's own Mono: every patched method must give the unpatched method's bits. This is the check to run
# on a VaM build you have not seen before.
#     run-patch.ps1 [-Bench]
param([switch]$Bench)
. (Join-Path $PSScriptRoot 'common.ps1')

# BepInEx 5.4 carries HarmonyX, whose own dependencies (Mono.Cecil, MonoMod) the stand-alone Mono host cannot load, so
# the patches cannot be applied here. run-paths.ps1 still says whether this VaM's methods are the code the patches
# were written for; in the game, the plugin's log line ("on: skinning, morphs, normals, soft vertices") and its verify
# mode ([Diagnostics] VerifyEvery) are the check.
if (Test-Path (Join-Path $vam 'BepInEx\core\HarmonyXInterop.dll')) {
    'skipped: this BepInEx has HarmonyX, which cannot be loaded outside the game (see the comment in run-patch.ps1)'
    exit 0
}

Build-Native
$plugin = Join-Path $build 'VaMCpuBoost.dll'
& $csc /nologo /target:library /optimize+ /warn:4 /unsafe ('/out:' + $plugin) `
    ('/reference:' + (Join-Path $vam 'BepInEx\core\BepInEx.dll')) ('/reference:' + (Join-Path $vam 'BepInEx\core\0Harmony.dll')) `
    ('/reference:' + (Join-Path $managed 'Assembly-CSharp.dll')) ('/reference:' + (Join-Path $managed 'UnityEngine.CoreModule.dll')) `
    ('/reference:' + (Join-Path $managed 'UnityEngine.dll')) `
    (Join-Path $root 'plugin\VaMCpuBoost.cs') (Join-Path $root 'plugin\CpuBoostSkin.cs') (Join-Path $root 'plugin\CpuBoostPaths.cs') `
    (Join-Path $root '..\..\shared\harmony-compat\HarmonyCompat.cs')   # as `vamkit build` compiles it: see that file
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
