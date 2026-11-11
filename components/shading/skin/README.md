# skin

Skin shading and the host of the path tracer. For skin it replaces VaM's skin pixel programs (skin-like GGX highlights,
light through thin parts, sub-surface scattering, teeth, eyes, an occluded mouth) by lifting VaM's own programs to HLSL
and editing them. For path tracing it is the 11on12 host of `rt-core`: it records scene meshes, skinned characters,
clothing, materials, UI and effects into RtCore every frame and hands the traced frame to the compositor
(opencomposite) through `ST_*PathV1` exports. `[RayTracing] Renderer` in its cfg is the one path tracing switch. Depends on
BepInEx + Harmony, `rt-core` (compiled in), `shader-lift` (to read VaM's shader data), the NVAPI headers and libs in
`third_party/nvapi`, and the DXC / FXC of the Windows SDK. Dormant unless the launcher set its environment.

## Files

| File | Holds |
|---|---|
| `VaMSkinTransmission.cs` | BepInEx plugin `local.vam.skintransmission`: config, native load, per-skin bake, skin gloss levels, menu-facing state, `Renderer` follow |
| `RtScene.cs`, `PathSkinned.cs`, `PathSubmitted.cs`, `PathCanvasUi.cs` | The path tracer's scene capture: materials per shader family, skinned and submitted meshes, UI and effect hand-over |
| `TongueSurface.cs`, `RegionalSurface.cs` | Tongue and regional skin looks; they need assets derived from VaM that are not in the repository, without them VaM's tongue and the uniform regional look stay |
| `native/skin_transmission.cpp` | Native module (D3D11): shader recognition, replaced skin draws, `ST_*` exports, context hooks |
| `native/rt.cpp`, `path_overlay.inl`, `path_textures.inl` | The 11on12 / D3D12 host of RtCore: buffer copies, per-frame record, path trace, overlay and texture hand-over |
| `native/bake.cpp`, `mouth_bake.cpp`, `mouth_gpu.cpp`, `sss.cpp` | Thickness bake, mouth geometry and occlusion, sub-surface scattering passes |
| `shaders/transmission.hlsli`, `mouth.hlsl`, `sss.hlsl` | Shaders written here (the lifted VaM programs are generated, never stored) |
| `tools/gen_*.py`, `asm_lift.py`, `skin_shaders.py`, `vs_emu.py` | Generators: read VaM's shaders, lift DXBC to HLSL, patch, compile; write only under `out/gen/skin/` |
| `tests/` | Offline tests of the specular programs, scattering and mouth |

## Build

`python tools/vamkit build skin` runs every step for this component (generators, native library, plugin) and writes to
`out\skin\`; the steps are listed in `manifest/components.toml`. The plugin is compiled by that command from the
`[component.managed]` recipe there (C# 3.5 compiler, against the user's VaM).

Three generated C# files and several generated headers come from the user's VaM, so the order matters (all need
`VAMKIT_VAM`; the compilers `VAMKIT_FXC` / `VAMKIT_DXC` are the Windows SDK `bin\<version>\x64` directory):

1. Generators into `out\gen\skin\` and `out\gen\rt-core\`: `python -I components\shading\skin\tools\gen_skin_table.py`,
   `gen_spec_table.py`, `gen_sss_shaders.py`, `gen_mouth.py`; `python -I components\shading\rt-core\tools\build_program_sets.py`,
   `build_overlay_registry.py`, `shader_contracts.py`, `shadow_casters.py`, `gen_shaders.py` (the last one needs `VAMKIT_DXC`).
2. `components\shading\skin\native\build.cmd` (`VAMKIT_VCVARS`; needs `third_party\nvapi`): compiles
   `rt-core\src\rtcore.cpp` and the skin sources into `out\skin\VaMSkinTransmissionNative.dll` and copies it to `.bin`.

Some generator docstrings mention checked-in copies of `PathShaderStates.cs` and `PathShadowCasters.cs`; there are none, the build reads
the ones in `out\gen\skin`. The first path-traced frame after a shader change waits for the driver to compile the pipelines (about 26 s once).

## Settings

`BepInEx/config/local.vam.skintransmission.cfg` (the in-game menu writes most of these; the launcher writes the per-mode preset at launch):

| Section / key | Default | Meaning |
|---|---|---|
| `[General] Enabled` | `true` | Off: nothing loaded, VaM unchanged (read at startup) |
| `[General] Strength` | `1.0` | Light through thin parts (0 to 4): 1 as measured for skin, 0 none. Also drives path tracing's thin-part transmission |
| `[RayTracing] Renderer` | `Legacy` | `PathTracing` or `Legacy`; followed live; the compositor follows it every frame |
| `[RayTracing] PathBounces` | `0` | 0 to 3 diffuse bounces in path tracing; mirrors, glass, gloss and hair reflect at every value |
| `[RayTracing] PathBackground` | `Far` | `Off` / `Far` (no diffuse bounce past 8 m, whole past 16 m, reflections read the sky cube) / `All` |
| `[RayTracing] RRDetail` | `75` | RR material-detail retention, 0 to 100 |
| `[Subsurface] Strength` | `1.0` | Sub-surface scattering 0 to 1 (both routes) |
| `[Specular] Normalize` | `true` | Skin-like highlights at one gloss level per skin (level kept in `local.vam.skintransmission.tiers.txt`); off = VaM's own |
| `[Teeth] Look`, `[Eyes] Look` | `Natural` | `VaM`, `Even`, `Natural`, `Deep` for teeth; `VaM`, `Subtle`, `Natural`, `Deep` for eyes |
| `[Mouth] Enabled`, `Strength`, `TongueMaterial` | `true`, `0.95`, `true` | Geometry-driven mouth occlusion and the tongue material |
| `[Shadows] ShadowMapSize` | `2048` | Spot and point light shadow map size (0, 2048, 4096); 0 = Unity's own |

## Interfaces

- Native exports (`ST_ApiVersion()` = 45): the `ST_Set*` setters the plugin calls, `ST_Bake`, `ST_Read`, `ST_LastError`, and the
  path tracer hand-over the compositor binds with `GetProcAddress` on `VaMSkinTransmissionNative.bin`:
  `ST_PathTracingRequestedV1`, `ST_PathTracingEnabledV1`, `ST_SetPathTracingV1`, `ST_RecordPathTraceV1`,
  `ST_FinishPathTraceV1`, `ST_WritePathSceneV1`, `ST_PathSceneRevisionV1`, `ST_PathFrameValidV1`, `ST_GetPathOverlayV1`,
  `ST_SetPathConsumerReadyV1`, `ST_SetPathDepthReadyV1`, `ST_PathBufferBytesV1`, `ST_SetPathUiTexturesV1`,
  `ST_GetPathObservedV1`. They are used in `components/render/opencomposite/OpenOVR/Compositor/vam_rr.inl`.
- Calls other plugins' natives by module name: `CS_GetPathMaterialV1` (cloth-shading), `HOP_BuildPathHairV2`
  (hair-strands), cpu-boost's pool.
- Environment: `VAM_DISPLAY_MODE` (`VR` / `Desktop`, from the launcher). Diagnostics: `VAM_PT_DUMP` (dump directory), `VAM_PT_TEXTURE_LOG`,
  `VAM_PT_CPU_PROFILE`, `RTC_MARKERS=1`, `RTC_PATH_COMPUTE`.
- Expects the 11on12 proxy (`d3d11-proxy`) in front of Unity's device for path tracing.

## Tests

- `tests/build_spec_test.cmd`, `build_sss_test.cmd`, `build_mouth_test.cmd` (`VAMKIT_VCVARS`) build `out\skin-tests\*.exe`.
  `spec_test.exe out\gen\skin\spec_cases` draws each replaced skin program with random inputs and checks that mode 0 gives VaM's
  result and the changed mode gives finite output; `sss_test.exe` checks the sub-surface passes (colour difference equals
  albedo times light, depth and distance valid) on a synthetic eye camera; `mouth_test.exe` the mouth occlusion. They need a GPU and
  the generated `spec_cases`.
- The path tracer's own offline probes are in `rt-core` (`tests/build.cmd`).
- Look and speed: only the game or the simulated headset show them.

## Invariants

- Nothing derived from VaM goes into git: generated programs, `PathShaderPrograms.cs`, the tongue and regional assets.
- Mode "off" paths give VaM's own program; replaced programs are matched by bytecode hash, an unknown program is not touched.
- Path tracing never rejects a scene because of one object: unsupported shaders fall back to the standard-surface path,
  effects are replayed on top, and only a device error pauses it (the frames are VaM's raster meanwhile).
- Bump `ST_API_VERSION` (`native/skin_transmission.cpp`) and `rtc::kApiVersion` (rt-core) when structures change; the host
  refuses an RtCore whose API differs from its own (`rt.cpp`). Keep the `V1` export names stable for opencomposite.

## Read next

[../../../docs/knowledge/skin.md](../../../docs/knowledge/skin.md), [../../../docs/knowledge/path-tracing.md](../../../docs/knowledge/path-tracing.md)
