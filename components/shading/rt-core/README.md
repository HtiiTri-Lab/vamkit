# rt-core

RtCore is the path tracer: a DXR 1.1 scene (bottom and top level acceleration structures for skinned characters,
clothing, scene meshes and hair strands as LSS fibres) and a full-scene input-pixel path integrator that gives DLSS Ray
Reconstruction its noisy colour plus guides (diffuse and specular albedo, normal, depth, hit distance, motion). With path
tracing switched on in game, this is what draws the picture. It has no Unity or VaM code: the host (the `skin` component)
supplies D3D12 resources and a command list and RtCore records into it. Built as part of `skin`
(`build = "none"` in the manifest); needs the Windows SDK DXC and the NVAPI headers in `third_party/nvapi`
(opacity micromaps and shader execution reordering on NVIDIA).

## Files

| File | Holds |
|---|---|
| `include/rtcore.h` | The scene API: `Init`, geometry / material / light descriptions, `Frame`, `Record`, `Status`, `SetTestLayers`; `kApiVersion` |
| `include/path_tracing.h` | The integrator API: `PathMaterial`, `PathEnvironment` (and the `kPathAuthor*` rule bits), `PathView`, `RecordPathTrace`, eye rigs, hair |
| `include/path_hair_bridge.h`, `startup_display.h` | Hair hand-over structs shared with hair-strands; `VamDesktopDisplay()` / `VamViewCount()` from `VAM_DISPLAY_MODE` |
| `src/rtcore.cpp` | Resources, acceleration structures, pipelines, per-frame record, the `Pipe` order shared with the generated shader blobs |
| `src/path_*.inl` | Path tracer parts: hair, lighting, motion, opacity micromaps, skinning, CPU profile |
| `shaders/path_trace.hlsl`, `path_lighting.hlsli`, `path_hair*.hlsl*`, `path_skin_bssrdf.hlsli`, `path_scatter.hlsl`, `path_omm.hlsl`, `path_scene.hlsl`, `path_skinning.hlsl`, `rtc_common.hlsli`, `rtc_omm.hlsli` | The shaders (DXIL, shader model 6.5) |
| `third_party/rtxcr/`, `third_party/pbrt/` | NVIDIA's RTXCR material library (local adaptation, see `LOCAL-ADAPTER.txt`; licences in each file) and pbrt's licence |
| `tools/` | Generators (`gen_shaders.py`, `build_program_sets.py`, `build_overlay_registry.py`, `shader_contracts.py`, `shadow_casters.py`) and read-only audits (`family_groups.py`, `lift_family.py`, `material_coverage.py`) over the user's VaM shaders |
| `tests/` | Offline GPU probes |

## Build

`python tools/vamkit build rt-core` runs every step for this component (generators) and writes to `out\rt-core\`; the
steps are listed in `manifest/components.toml`.
- Shaders: `python -I components\shading\rt-core\tools\gen_shaders.py` with `VAMKIT_DXC` (the Windows SDK
  `bin\<version>\x64` directory or `dxc.exe`, with a signed `dxil.dll`) writes `out\gen\rt-core\rtc_shaders.h`, one DXIL blob per pass.
- RtCore itself is compiled by the host: `components\shading\skin\native\build.cmd` compiles `src\rtcore.cpp` and links it into
  `VaMSkinTransmissionNative.dll` (`VAMKIT_VCVARS`, `third_party\nvapi`). The other generators write under `out\gen\skin\`
  from the user's VaM (`VAMKIT_VAM`); see the skin README for the order.
- `tools\build_composite_shaders.py` (`VAMKIT_FXC`) compiles the compositor's embedded shaders in
  `components/render/opencomposite/OpenOVR/Compositor/vam_rr.inl` ahead of a game run, into `out\gen\rt-core\composite-check`.
- Probes: `components\shading\rt-core\tests\build.cmd` (`VAMKIT_VCVARS`, `VAMKIT_DXC`; it runs `gen_shaders.py` and compiles
  `fiber_check.hlsl`), then `build_shadow_probe.cmd`; output `out\rt-core-tests\`.

## Settings

None of its own. Behaviour is chosen by the host through the structs: `PathView::maxBounces`, `PathEnvironment::diffuseBounces`
(`[RayTracing] PathBounces` in the skin cfg), `PathEnvironment::authorRules` and the background distances
(`kPathAuthorBgBounce` / `kPathAuthorBgRough`, from `[RayTracing] PathBackground`), RR detail reference and responsivity.
`SetTestLayers(mask)` switches individual features off for A/B tests (the bit meanings are in `src/rtcore.cpp` and the probes, for example
bit 19 = opacity micromaps).

## Interfaces

- `rtc::kApiVersion` (27) is checked by the host (`skin/native/rt.cpp`): a mismatch refuses RtCore and the frames stay VaM's raster.
- RtCore records on the host's queue only: it never submits, never waits on a fence, keeps every resource alive until the host reports
  `Frame::completedFrame`, and expects inputs in COMMON / NON_PIXEL_SHADER_RESOURCE.
- `PathView` outputs (colour, diffuse, normal, specular, depth, hit distance, motion; optional diffuse / specular radiance for a
  denoiser route) are UAVs with the input dimensions, consumed by the compositor in opencomposite.
- Reads `VAM_DISPLAY_MODE` (`startup_display.h`), `RTC_MARKERS=1` (GPU markers), `RTC_PATH_COMPUTE` (disables shader execution reordering).

## Tests

`out\rt-core-tests\probe.exe <output dir>` runs bounded GPU checks with the D3D12 debug layer on analytic fixtures; they reject wrong
cross-stage data and cannot approve appearance:

- `contract_probe.inl`: material transport and reconstruction contracts; `lighting_probe.inl`: fixed energy and visibility cases;
  `material_probe.inl`: texture and material inputs; `motion_size_probe.inl`: motion history buffer sizes;
  `omm_probe.inl`: with and without opacity micromaps the outputs must be byte-identical; `split_probe.inl`: diffuse / specular split;
  `fiber_probe.inl` with `fiber_check.hlsl`: fibre intersection. `shadow_probe.exe`: shadow rays against a mesh and a fibre blocker.
- They need a DXR-capable GPU. How the scene looks, speed and the in-game behaviour are only visible in the game.

## Invariants

- No VaM or Unity dependency; no file read from VaM. Everything derived from VaM goes through the host's generators into `out/gen`.
- With the author-rule bits at 0 the tracer is the physical one; each rule is one named bit so it can be cleared for A/B.
- Opacity micromaps are marked only where the result equals the shader's (the omm probe proves byte identity); animated alpha stays on the shader.
- Keep third-party licence headers in `third_party/` and record local changes in `LOCAL-ADAPTER.txt`.

## Read next

[../../../docs/knowledge/path-tracing.md](../../../docs/knowledge/path-tracing.md),
[../../../docs/knowledge/path-tracing-authored-lighting.md](../../../docs/knowledge/path-tracing-authored-lighting.md)
