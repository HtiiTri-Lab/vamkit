# cloth-physics

GPUTools' simulated-cloth step runs as outer x (iterations x (3 + colour groups) + ~6) small dispatches per item. This
plugin runs it on a fused compute solver, `1 + 2 x outer` dispatches per item and step, all items of a step interleaved
so the GPU runs them side by side, with results bit-identical to GPUTools. Large items iterate on several thread groups.
The player gets the same cloth for less GPU time on characters wearing many simulated garments. Depends on BepInEx +
Harmony and Unity's D3D11 device; uses cpu-boost's pool when present.

## Files

| File | Holds |
|---|---|
| `plugin/VaMClothPhysics.cs` | BepInEx plugin `local.vam.clothphysics`: Harmony prefix on `ClothPhysicsWorld.DispatchPhysicsImpl`, per-scene assignment of items to the solver, queue and flush points, verify, diagnostics |
| `plugin/native/cloth_physics_native.cpp` | Native library: item create / queue / solve, phases, multi-group iteration, GPU timers, checks |
| `solver/cloth_fused.hlsl` | Compute shaders `CSImport`, `CSIterate` (+ `SHARED`), `CSIterateMulti` (`MULTI`), `CSCollide` |
| `plugin/native/build.cmd` | Native build, compiles the shaders into headers |

## Build

`python tools/vamkit build cloth-physics` runs every step for this component (native library, plugin) and writes to
`out\cloth-physics\`; the steps are listed in `manifest/components.toml`. The plugin is compiled by that command from
the `[component.managed]` recipe there (C# 3.5 compiler, against the user's VaM).
- Native: `components\runtime\cloth-physics\plugin\native\build.cmd` with `VAMKIT_VCVARS` (vcvars64.bat) and `VAMKIT_FXC`
  (Windows SDK `bin\<version>\x64`). Shaders `cs_5_0 /O3` go to `out\cloth-physics\gen\*.h`; the DLL
  `out\cloth-physics\VaMClothPhysicsNative.dll` is copied to `.bin`. The shader options (`JOINTS_IN_FLIGHT 1`,
  `COLLIDE_THREADS 256`, `/O3`) are the ones the bit-identity checks were run with: do not change them without re-verifying.

## Settings

`BepInEx/config/local.vam.clothphysics.cfg` (the install enables it; the plugin's own default is `false`):

| Section / key | Default | Meaning |
|---|---|---|
| `[General] Enabled` | `false` (installed `true`) | Off: VaM unchanged. Read at startup |
| `[Diagnostics] Diagnostics` | `false` | Load even when disabled, time cloth steps and solves (main thread, GPU), let the test agent switch the solver |
| `[Diagnostics] VerifyEverySteps` | `0` | With Diagnostics: every N steps per item run GPUTools' step on the same particles and compare (stalls the render thread) |

## Interfaces

- Native exports (`CP_ApiVersion()` = 4): `CP_CreateItem`, `CP_QueueStep`, `CP_QueueSolve`, `CP_QueueRelease`,
  `CP_GetRenderEventFunc` (through `GL.IssuePluginEvent`), `CP_SetPhases`, `CP_SetMulti`, `CP_ItemStatus`, `CP_ItemMulti`,
  `CP_QueueCheck`, `CP_ItemVerify`, `CP_GpuTime`, `CP_GetLastError`.
- Test-agent entry points: `VaMClothPhysics.SetSolver(bool)`, `SetPhases(bool)`, `SetMulti(bool)`, `SetVerify(int)`,
  `Stats()`, `ResetStats()`. Agent: `tools/lab/test-agent`.
- GPUTools' renderer reads the particle buffer as before, so the queue must have run before a camera renders.
- wrap-batch records the cloth's render-frame work (`ClothSettings.LateUpdate`), which reads this solver's particles after the physics step.
- Loads `VaMClothPhysicsNative.bin` from the plugin's own folder.

## Tests

- None offline in this repository: the replay harness (`cloth_harness.cpp`, four recorded scenes) of the development
  tree was not moved here.
- In game: `Diagnostics = true`, `VerifyEverySteps = N` compares one step in N per item with GPUTools' on the same
  particles; the log lists items that stay with GPUTools and why. Frame cost only shows in game.

## Invariants

- Bit-identical to GPUTools' cloth step. `CSIterateMulti` uses a counter in the item's sync buffer as a barrier between
  thread groups; a thread group that never gets its peers gives up after a bounded poll, sets a flag, and the item runs
  single-group from then on. Keep that escape.
- The solver takes an item only if it makes the step shorter: items run side by side, so a solve costs about its slowest
  item. GPUTools keeps the first 20 frames after a reset, neighbour collision, items without skin-wrap targets, more than
  65536 particles and anything that fails. Do not remove these guards.
- Anything that would touch a queued item's inputs or results (shared collider update, the cloth's render-frame work, a
  second step of the same item) runs the queue first.

## Read next

[../../../docs/knowledge/clothing.md](../../../docs/knowledge/clothing.md)
