# hair-solver

GPUTools' hair physics (VaM's physics hair) normally issues 30 to 190 small compute dispatches per hair item and step.
This plugin runs the same step on one fused compute shader, one to five dispatches per item, and runs all hair items of
a frame side by side instead of waiting for each other (which matters on D3D11On12). Hair moves as in VaM, for less
GPU and driver time. Depends on BepInEx + Harmony and on Unity's D3D11 device; uses cpu-boost's worker pool when present.

## Files

| File | Holds |
|---|---|
| `plugin/VaMHairSolver.cs` | BepInEx plugin `local.vam.hairsolver`: Harmony prefix on `HairPhysicsWorld.DispatchPhysicsImpl`, per-item decision, queueing, flush points, diagnostics and verify |
| `plugin/HairSolverData.cs` | What is handed to the native solver for one item, built from GPUTools' joint data, and the checks that the data is what the solver models (pure functions) |
| `plugin/HairBodyContact.cs` | Collider index mask over `GPUCollidersManager`'s lists; breast colliders can be replaced by spheres published by a soft body plugin |
| `plugin/native/hair_solver_native.cpp` | Native library: item create / queue / solve, phases, GPU timers |
| `solver/fused_strand.hlsl` | The fused compute shader: `CSStrand` (per substep dispatches) and `CSStep` (one dispatch per step), cling modes 0 / 1 / 2 |
| `plugin/native/build.cmd` | Native build, compiles the shader into headers |

## Build

`python tools/vamkit build hair-solver` runs every step for this component (native library, plugin) and writes to
`out\hair-solver\`; the steps are listed in `manifest/components.toml`. The plugin is compiled by that command from
the `[component.managed]` recipe there (C# 3.5 compiler, against the user's VaM).
- Native: `components\runtime\hair-solver\plugin\native\build.cmd` with `VAMKIT_VCVARS` (vcvars64.bat) and `VAMKIT_FXC`
  (the Windows SDK `bin\<version>\x64` directory holding `fxc.exe`). Shaders are compiled `cs_5_0 /O3 /WX` into
  `out\hair-solver\gen\fused_strand_{strand,step}.h`, then `out\hair-solver\VaMHairSolverNative.dll`, copied to `.bin`.

## Settings

`BepInEx/config/local.vam.hairsolver.cfg` (the install enables it; the plugin's own default is `false`):

| Section / key | Default | Meaning |
|---|---|---|
| `[General] Enabled` | `false` (installed `true`) | Off: VaM unchanged. Read at startup |
| `[General] Cling` | `Synchronous` | `Synchronous`: cling joints evaluated inside the solver (mean difference to GPUTools 0.3-0.6 mm on the test hair). `Original`: GPUTools' own cling kernel between the solver's dispatches (same result as GPUTools, more dispatches) |
| `[Diagnostics] Diagnostics` | `false` | Load even when disabled, time every hair step (main thread and GPU), let the test agent switch the solver at run time |
| `[Diagnostics] VerifyEverySteps` | `0` | With Diagnostics: every N steps per item run GPUTools' step on the same particles too and compare (stalls the render thread) |

## Interfaces

- Native exports (`HS_ApiVersion()` = 5): `HS_CreateItem`, `HS_QueueStep`, `HS_QueueSolve`, `HS_QueueRelease`,
  `HS_GetRenderEventFunc` (invoked through `GL.IssuePluginEvent`), `HS_SetOptions`, `HS_ItemStatus`, `HS_QueueTimer`,
  `HS_GpuTime`, `HS_ItemVerify`, `HS_BatchStats`, `HS_GetLastError`.
- Test-agent entry points (class `VaMHairSolver`): `SetSolver(bool)`, `SetVerify(int)`, `SetCling(bool)`,
  `SetOverlap(bool)`, `SetRepeat(bool)`, `SetTiming(bool)`, `Stats()`, `ResetStats()`. The test agent is `tools/lab/test-agent`.
- Uses hair-budget's own hair clock when that plugin gives hair its own physics rate: a frame with two hair steps keeps
  one solve (each item's second step runs in the phases after its first).
- Optional: a soft body plugin may publish the drawn breast's spheres (`BreastSpheres`); the soft body is not in this
  repository, so breast colliders stay as VaM's (mask 1).
- Loads `VaMHairSolverNative.bin` from the plugin's own folder.

## Tests

- None offline in this repository: the offline harness of the development tree (an on-12 benchmark and a data test) was not
  moved here.
- In game: `Diagnostics = true` plus `VerifyEverySteps = N` compares the solver's step with GPUTools' on the same particles
  and logs the difference; the plugin logs why an item stays with GPUTools. Only the game or the simulated headset shows it.

## Invariants

- Same rules, parameters and order as GPUTools: where strands do not interact (no cling joints, or cling 0) one dispatch per
  step and results equal GPUTools'. With cling `Synchronous` is an approximation (above); `Original` is exact.
- Per item GPUTools keeps the step for style mode, joint data the solver does not model, more than 256 points per strand,
  and any error. Never make an unsupported item fail silently; fall back and log.
- Before anything reads or writes what a queued step touches (the item's next step, the renderer's tessellation, the
  skin the item hangs on, collider rewrite, rebuild or dispose) the queue is flushed. Keep every flush point.
- Take worker threads from cpu-boost (`CB_Task` / `CB_Run`); do not add a pool.

## Read next

[../../../docs/knowledge/hair.md](../../../docs/knowledge/hair.md)
