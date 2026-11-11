# hair-strands

VaM's physics hair (GPUTools `MeshedVR/HairOpt`) is drawn by Unity's forward renderer once in ForwardBase and once more,
whole, for every further pixel light that reaches it. This component recognises those draws in the eye camera's opaque
pass and draws the hair once through a visibility buffer instead: strand points computed once per frame, the segments'
quads rasterised in compute, ForwardBase and all ForwardAdd lights shaded in full-target batches (mode 3, "strands"),
with a Marschner transmission lobe so hair lit from behind glows. For the path tracer it also builds the LSS strand
geometry from the same hair. Depends on BepInEx; its hair program table is generated from the user's VaM
(`from_vam = true`, uses shader-lift); rt-core headers describe the hair hand-over to the path tracer.

## Files

| File | Holds |
|---|---|
| `VaMHairOnePass.cs` | BepInEx plugin `local.vam.haironepass` 2.2.0: loads the native module early (before the game creates hair shaders), marks the opaque pass with render events, mode switches, test API |
| `native/hair_onepass.cpp` | Native module: context and device hooks, SHA-1 tagging of hair programs, modes 1 observe / 2 merge / 3 strands, `HOP_*` exports |
| `native/hs_item.h`, `hop_item.h` | Per-item data of the strands and merge paths |
| `native/path_hair_registry.inl` | `HOP_BuildPathHairV2`: strands for the path tracer's LSS hair |
| `shaders/hair_strands.hlsl`, `hs_domain.hlsl`, `hs_dispatch.hlsl` | Strands build / raster (depth, id) / resolve passes, isoline domain read-back, `HairTransmission` |
| `shaders/hair_onepass.hlsl` | The merge (fallback) pixel path |
| `tools/gen_programs.py` | Writes `out/gen/hair-strands/hair_programs.h`: every `HairOpt` program keyed by SHA-1 prefix, with pass, stage, keywords, constant buffer layouts |
| `tools/build_strands_shaders.py` | Compiles the strands shaders with fxc into `out/gen/hair-strands/hs_shaders.h` |
| `tools/build_onepass_shaders.py` | Compiles the merge shaders with fxc into `out/gen/hair-strands/hop_shaders.h` |

## Build

`python tools/vamkit build hair-strands` runs every step for this component (generators, native library, plugin) and
writes to `out\hair-strands\`; the steps are listed in `manifest/components.toml`. The plugin is compiled by that
command from the `[component.managed]` recipe there (C# 3.5 compiler, against the user's VaM).

Environment: `VAMKIT_VAM`, `VAMKIT_FXC`, `VAMKIT_VCVARS`, `VAMKIT_CSC`. From the repository root:

```
python -I components\shading\hair-strands\tools\gen_programs.py
python -I components\shading\hair-strands\tools\build_onepass_shaders.py
python -I components\shading\hair-strands\tools\build_strands_shaders.py
components\shading\hair-strands\native\build.cmd
```

- `gen_programs.py` reads `GPUTools/MeshedVR/HairOpt` straight from `VaM_Data\resources.assets` (through
  `components\shading\skin\tools\skin_shaders.py`) and writes `out\gen\hair-strands\hair_programs.h`. Derived from VaM:
  never commit it.
- `build_onepass_shaders.py` compiles `shaders\hair_onepass.hlsl` into `out\gen\hair-strands\hop_shaders.h`;
  `build_strands_shaders.py` compiles `hair_strands.hlsl` and `hs_domain.hlsl` into `out\gen\hair-strands\hs_shaders.h`
  (listings under `out\hair-strands\shaders`).
- `native\build.cmd` writes `out\hair-strands\VaMHairOnePassNative.dll` and copies it to `.bin` (the name the plugin loads).
  The native source finds `startup_display.h` and `path_hair_bridge.h` in `components\shading\rt-core\include`.
- Managed plugin `VaMHairOnePass.cs`: BepInEx plugin against the user's VaM, referencing `BepInEx\core\BepInEx.dll` and
  `VaM_Data\Managed\UnityEngine.dll`, `UnityEngine.CoreModule.dll`, `Assembly-CSharp.dll`; C# 3 (`VAMKIT_CSC`). Built by
  the common managed build.

## Settings

`BepInEx/config/local.vam.haironepass.cfg`, read at startup:

| Section / key | Default | Meaning |
|---|---|---|
| `[General] Enabled` | `true` | Strands path on. `false`: nothing is loaded, VaM unchanged |

Run-time switches are test-API only: `Strands(on)`, `Merge(on)`, `Transmission(on)` (on by default; off = the earlier
lighting). The environment variable `VAM_DISPLAY_MODE=Desktop` (set by the launcher) selects the desktop camera path: a
non-stereo constant layout and position mapping; VR keeps the two-view layout.

## Interfaces

- Native exports (`HOP_ApiVersion()` = 5): `HOP_Init`, `HOP_RenderEventFunc` (ops 1 install, 2 begin, 3 end, 4 frame),
  `HOP_SetMode`, `HOP_SetTransmission`, `HOP_StrandsReady`, `HOP_ResetTiming`, `HOP_Snapshot`, `HOP_Capture`, `HOP_Read`,
  `HOP_LastError`, `HOP_PathHairSourceV1`, `HOP_BuildPathHairV2`.
- The skin component's path tracer calls `HOP_BuildPathHairV2` (found with `GetModuleHandleW(L"VaMHairOnePassNative.bin")`)
  to get strand points and indices for LSS hair; structures in `components/shading/rt-core/include/path_hair_bridge.h`.
- What strands cannot take is drawn as merge mode does (each item's ForwardAdd lights in one draw) or as Unity draws it.
- Test API (`Report()`, `Snapshot(path)`, `Capture(path)`, `Observe(true)`, `ResetTiming()`) for the test agent in `tools/lab/test-agent`.

## Tests

- None offline in this repository (the development tree also had a hook test for the context-table patching). The game
  or the simulated headset show it: `Report()` returns merge and strands statistics and the opaque pass's GPU time;
  `Snapshot` writes the eye target after the opaque pass so strands and Unity's draws can be compared.

## Invariants

- Same picture as Unity's draws for what strands take (apart from the transmission lobe, which is intended); what they cannot
  take falls back, never disappears.
- Draws are recognised by the SHA-1 of the program bytecode, never by name; unknown programs are not touched.
- Context hooks patch both the runtime's template table and the live table, chained, so they work whichever of skin,
  cloth-shading and this module patches first. Keep that when adding hooks.
- A strand quad too large for one thread (camera close to the hair) is rasterised by a thread group sharing its lines;
  quads wholly behind the eye are dropped. Keep both for close-ups.

## Read next

[../../../docs/knowledge/hair.md](../../../docs/knowledge/hair.md)
