# cpu-boost

VaM's character hot paths (CPU skinning, morph deltas, post-skin normals, soft-vertex morphs) run in native code with
results bit-identical to VaM 1.22.0.3's own C#, so the frame spends less main-thread and worker time on the character.
The same native library is the one worker pool every native plugin of this repo shares (`CB_Task` / `CB_Run` /
`CB_PinThread`). Depends on BepInEx + Harmony; no other component is required, but hair-solver, cloth-physics,
wrap-batch and others look it up by name.

## Files

| File | Holds |
|---|---|
| `plugin/VaMCpuBoost.cs` | BepInEx plugin `local.vam.cpuboost`: Harmony patches, IL fingerprint checks, config, verify mode, stats log |
| `plugin/CpuBoostSkin.cs`, `plugin/CpuBoostPaths.cs` | Marshalling for the skinning path and for the normals / soft-vertex / morph paths |
| `native/cpuboost_native.cpp` | The native library: worker pool, `CB_Skin*`, `CB_NormalsSubset`, `CB_SoftMorph`, `CB_MorphApply` |
| `native/x87.asm` | `cb_fsin` / `cb_fcos` / `cb_fsincos` and x87 control word access (Mono's `Math.Sin/Cos` are x87 instructions) |
| `native/build.cmd` | Native build |
| `harness/` | Offline checks run inside VaM's own Mono: `mono_host.c`, `SkinTest.cs`, `PathsTest.cs`, `PatchTest.cs`, `Worlds.cs`, `floattest/` |

## Build

`python tools/vamkit build cpu-boost` runs every step for this component (native library, plugin) and writes to
`out\cpu-boost\`; the steps are listed in `manifest/components.toml`. The plugin is compiled by that command from the
`[component.managed]` recipe there (C# 3.5 compiler, against the user's VaM).
- Native: `components\runtime\cpu-boost\native\build.cmd` with `VAMKIT_VCVARS` set to `vcvars64.bat`. Output
  `out\cpu-boost\VaMCpuBoostNative.dll`, copied to `VaMCpuBoostNative.bin` (BepInEx loads every `.dll` under `plugins`
  as a .NET assembly, so the plugin loads the module as `.bin`). `/fp:strict` is required, see Invariants.
- Managed: `plugin\*.cs` is compiled as a BepInEx plugin against `VaM_Data\Managed` and `BepInEx\core` of the user's
  VaM (`VAMKIT_VAM`, `VAMKIT_CSC`).
- The `harness/*.ps1` scripts use `VAMKIT_VAM`, `VAMKIT_VCVARS` and `VAMKIT_CSC` (`harness/common.ps1`); they build
  the native library, the plugin and the test programs themselves and write under `out\cpu-boost\`.

## Settings

`BepInEx/config/local.vam.cpuboost.cfg`:

| Section / key | Default | Meaning |
|---|---|---|
| `[General] Enabled` | `true` | Off leaves VaM's C# untouched |
| `[General] Threads` | `8` | Upper bound of native worker threads, capped by the processor count; `0` = calling thread only |
| `[General] WorkerCpus` | `Cores` | `Cores` = one worker per physical core on the cores Windows prefers least; `Any` = Windows places them |
| `[Paths] Skinning`, `Morphs`, `Normals`, `SoftVertices` | `true` | Each path on or off |
| `[Diagnostics] VerifyEvery` | `0` | N > 0: every Nth call also runs VaM's C#, keeps its result, logs any bit difference |
| `[Diagnostics] LogStats` | `true` | Calls and native time per path every 30 s |

## Interfaces

- Exports (`native/cpuboost_native.cpp`, `CB_ApiVersion()` = 5): `CB_Task(name, priority)`, `CB_Run(task, fn, ctx, begin,
  end, chunk)`, `CB_PinThread(k)`, `CB_SetThreads`, `CB_SetWorkerCpus`, `CB_TaskStats`, `CB_CoreMasks`, `CB_L3Mask`,
  `CB_X87ControlWord`. Other native plugins call them after `GetModuleHandleW(L"VaMCpuBoostNative.bin")` and require
  `CB_ApiVersion() >= 5`; without the module they run on their own thread.
- Pinned-thread slots (`CB_PinThread(k)`) are registered per caller; do not reuse a number another plugin uses.
- Patched methods: `DAZSkinV2.SkinMeshPart`, `DAZMorphBank.ApplyMorphsThreadedFast`,
  `RecalculateNormals.recalculateNormals(int[], int[])`, `DAZPhysicsMeshSoftVerticesGroup.MorphVerticesThreadedFast`.

## Tests

- `harness/run-skin.ps1`, `run-paths.ps1`: build the native library and run the skinning / normals / soft-vertex /
  Math.Pow comparisons bit for bit against VaM's methods inside VaM's own Mono (offline, no GPU, needs the user's VaM).
- `harness/run-patch.ps1`: runs the plugin's own Harmony patches against the stock methods in that Mono.
- `harness/floattest/run.ps1`: the experiment that fixed the float rules (compares `MonoFloatTest.exe` output).
- In game: `[Diagnostics] VerifyEvery` > 0 reports any difference at run time. Frame-time effect is only visible in game.

## Invariants

- Results bit-identical to VaM's C#: operands widened to double, rounded to float only where the IL stores, comparisons
  in double, `Math.Sin/Cos` as x87 `fsin/fcos`, no FMA contraction (`/fp:strict`).
- A method is patched only when its IL FNV-1a 64 fingerprint (`SkinIL`, `MorphIL`, `NormalsIL`, `NormalsCoreIL`,
  `SoftIL` in `VaMCpuBoost.cs`) matches the VaM 1.22.0.3 code; any other body is left to VaM and logged. On another VaM
  build, re-run the harness against it and update the fingerprints only after the comparisons pass.
- `Assembly-CSharp.dll` is never modified. Only our own worker threads get CPU masks; never VaM's, Unity's, drivers'
  threads or the process.
- Keep the exported `CB_*` signatures; other plugins bind to them by name.

## Read next

[../../../docs/knowledge/cpu.md](../../../docs/knowledge/cpu.md)
