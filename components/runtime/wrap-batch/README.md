# wrap-batch

Every clothing item VaM wraps onto the skin runs `DAZSkinWrap.LateUpdate` each frame: about a dozen small dependent
compute dispatches. On D3D11On12 each waits for all earlier GPU work, so a character in many garments pays for the
dispatch count, not the work. This plugin records those dispatches (also the render-frame work of simulated clothing) and
replays them phase by phase across all wraps, so the GPU runs them side by side. Results are bit-identical to VaM's.
Depends on BepInEx + Harmony and Unity's D3D11 device; optionally cooperates with cloth-physics.

## Files

| File | Holds |
|---|---|
| `VaMWrapBatch.cs` | BepInEx plugin `local.vam.wrapbatch`: Harmony transpilers that turn the `ComputeShader` calls into recorded ones, the replay at the "before anything reads" points, Verify |
| `native/wrap_batch_native.cpp` | Native library: render-event entry that flushes the D3D11 context between phases (`WB_Init`, `WB_RenderEventFunc`, `WB_Flushes`) |
| `native/build.cmd` | Native build |

## Build

`python tools/vamkit build wrap-batch` runs every step for this component (native library, plugin) and writes to
`out\wrap-batch\`; the steps are listed in `manifest/components.toml`. The plugin is compiled by that command from the
`[component.managed]` recipe there (C# 3.5 compiler, against the user's VaM).
- Native: `components\runtime\wrap-batch\native\build.cmd` with `VAMKIT_VCVARS` (vcvars64.bat). Output
  `out\wrap-batch\VaMWrapBatchNative.dll`, copied to `VaMWrapBatchNative.bin`.

## Settings

`BepInEx/config/local.vam.wrapbatch.cfg`:

| Section / key | Default | Meaning |
|---|---|---|
| `[General] Enabled` | `true` | Off: VaM unchanged. Read at startup |

## Interfaces

- Transpiled methods: `DAZSkinWrap.LateUpdate`, `UpdateVertsGPU`, the `MeshSmoothGPU` / `MapVerticesGPU` /
  `RecalculateNormalsGPU` / `RecalculateTangentsGPU` helpers they call, and for simulated clothing GPUTools'
  `ClothSettings.LateUpdate`, `KernelBase.Dispatch` / `BindAttributes` and the wrap's `Dispatch` (with `SetInt`,
  `SetVector`, `SetBool`, `SetFloats`, `SetTexture`).
- Replay points ("before anything reads or rewrites what the recorded work touches"): `Application.onBeforeRender`, any
  camera's PreCull, the Update / LateUpdate / cleanup of a skin a waiting wrap reads, `DAZSkinWrap.Dispatch` /
  `PostProcessDispatch` / `GPUCleanup` of a waiting wrap.
- Test-agent entry points: `VaMWrapBatch.SetEnabled(bool)`, `SetCloth(bool)`, `Stats()`, `ResetStats()`,
  `Verify(int frames)`, `VerifyResult()`. Agent: `tools/lab/test-agent`.
- Loads `VaMWrapBatchNative.bin` from the plugin's own folder.

## Tests

- None offline: only the game shows it. `Verify(frames)` reruns VaM's own `LateUpdate` on the same inputs for every wrap
  and compares vertices, normals, tangents and the delayed copies; for simulated clothing also the wrap targets, particles,
  vertex-only data and drawn vertices (its frame counter and last-frame targets are put back afterwards). Drive it with
  the test agent in a scene with clothing.

## Invariants

- Same result as VaM bit for bit: each dispatch reads and writes what it did before, in the same order within its wrap; a
  wrap reads only its skin and its own buffers. Do not add a replay point that is later than a reader of the results.
- Not recorded, run as VaM wrote them: wraps that provide matrices to hair or cloth, wraps not yet initialised, and cloth
  with accessories (`BuildClothAccessories` reads the particles back mid-chain).
- GPUTools rebinds every buffer and value before each dispatch, which is why interleaving items that share a shader is safe;
  a recorded call that did not re-bind would break that.
- On D3D11On12 a `Flush` between phases starts a new command list so buffers start in COMMON and need no barriers.

## Read next

[../../../docs/knowledge/clothing.md](../../../docs/knowledge/clothing.md)
