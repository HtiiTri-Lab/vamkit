# Shared by the run-*.ps1 scripts: where things are, and mono_host.exe (a small host that starts VaM's own Mono without
# the game, so that the tests run the methods exactly as VaM's runtime compiles them).
# Needs VAMKIT_VAM (the VaM folder), VAMKIT_VCVARS (vcvars64.bat) and VAMKIT_CSC (the C# 3.5 compiler).
$ErrorActionPreference = 'Stop'
foreach ($name in 'VAMKIT_VAM', 'VAMKIT_VCVARS', 'VAMKIT_CSC') {
    if (-not [Environment]::GetEnvironmentVariable($name)) { throw "set $name" }
}
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$repo = [IO.Path]::GetFullPath((Join-Path $root '..\..\..'))
$vam = $env:VAMKIT_VAM
$vcvars = $env:VAMKIT_VCVARS
$csc = $env:VAMKIT_CSC
$build = Join-Path $repo 'out\cpu-boost'
$out = Join-Path $build 'test'
[void](New-Item -ItemType Directory -Path $out -Force)
$managed = Join-Path $vam 'VaM_Data\Managed'

function Build-Native {
    & cmd /c (Join-Path $root 'native\build.cmd') | Out-Null
    if ($LASTEXITCODE -ne 0) { throw 'native build failed' }
}

function Build-MonoHost {
    $script:hostExe = Join-Path $out 'mono_host.exe'
    if (-not (Test-Path $hostExe)) {
        cmd /c "call `"$vcvars`" >nul && cl /nologo /O2 /W3 /Fo`"$out\\`" /Fe`"$hostExe`" `"$PSScriptRoot\mono_host.c`"" | Out-Null
        if ($LASTEXITCODE -ne 0) { throw 'mono_host build failed' }
    }
}
