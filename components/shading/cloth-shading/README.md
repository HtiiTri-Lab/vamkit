# cloth-shading

VaM draws every custom clothing item with one shader, `Custom/Subsurface/TransparentGlossNMNoCullSeparateAlpha`
(ComputeBuff): the skin's lighting code (one Blinn-Phong lobe, a wrap diffuse) plus a separate alpha. This component
replaces that shader's ForwardBase and ForwardAdd pixel programs for garments with a cloth BRDF (GGX that turns into
Charlie sheen as the fabric gets rough, specular anti-aliasing), and tells the path tracer which meshes are real garments
and what their cloth parameters are. The player sees fabric highlights and sheen instead of a plastic Blinn-Phong lobe.
Depends on BepInEx + Harmony; the replacement programs are generated from the user's VaM (`from_vam = true`) and use
skin's and shader-lift's readers; skin (`rt.cpp`) calls `CS_GetPathMaterialV1`.

## Files

| File | Holds |
|---|---|
| `VaMClothShading.cs` | BepInEx plugin `local.vam.clothshading` 0.1.0: Harmony postfix on `DAZSkinWrap.DrawMeshGPU` (no scene searches), garment rule (not on the skin, not small), config, test API |
| `native/cloth_shading.cpp` | Native module (D3D11): hashes every pixel shader as it is created (patched `CreatePixelShader`), creates a replacement next to each clothing program, redirects registered garment draws, `CS_GetPathMaterialV1` |
| `tools/gen_cloth_programs.py` | Lifts each clothing pixel program from its bytecode, inserts the cloth BRDF with a mode in constant buffer b12, compiles, writes `native/cloth_programs.h` and test cases |
| `tests/cloth_test.cpp`, `tests/build_test.cmd` | Offline check of the replacement programs |
| `native/build.cmd` | Native build |

## Build

`python tools/vamkit build cloth-shading` runs every step for this component (generators, native library, plugin) and
writes to `out\cloth-shading\`; the steps are listed in `manifest/components.toml`. The plugin is compiled by that
command from the `[component.managed]` recipe there (C# 3.5 compiler, against the user's VaM).

Environment (see the repository README): `VAMKIT_VAM`, `VAMKIT_FXC`, `VAMKIT_VCVARS`, `VAMKIT_CSC`. From the repository root:

```
python -I components\shading\cloth-shading\tools\gen_cloth_programs.py
components\shading\cloth-shading\native\build.cmd
```

1. `gen_cloth_programs.py` reads the clothing shader from VaM (through `components\shading\skin\tools\skin_shaders.py`,
   unchanged, and the skin generator `gen_spec_table.py`, imported) and writes `out\gen\cloth-shading\cloth_programs.h`,
   `cloth_programs.json` and the test cases `cloth_cases\`. These are derived from VaM's programs and are never committed.
2. `native\build.cmd` writes `out\cloth-shading\VaMClothShadingNative.dll` and copies it to `.bin` (the name the plugin loads).
3. The managed plugin `VaMClothShading.cs`: BepInEx plugin against the user's VaM (`VAMKIT_VAM`, `VAMKIT_CSC`); built by the
   common managed build.

## Settings

`BepInEx/config/local.vam.clothshading.cfg`:

| Section / key | Default | Meaning |
|---|---|---|
| `[General] Mode` | `1` | `0` = VaM's own clothing shading, `1` = cloth |
| `[Cloth] SheenFrom` | `0.25` | GGX roughness (alpha) where sheen starts to replace the highlight |
| `[Cloth] SheenFull` | `0.5` | Roughness from which fabric is all sheen |
| `[Cloth] SheenGain` | `2` | Sheen brightness (2 is about VaM's highlight light at a 45 degree view) |
| `[Cloth] SpecularGain` | `2` | Highlight brightness (2 = the peak of VaM's own lobe) |
| `[Cloth] AntiAliasing`, `AntiAliasingCap` | `0.159`, `0.18` | Screen-space normal variance widening of the highlight (Tokuyoshi and Kaplanyan), and its cap on alpha squared |

## Interfaces

- Native exports (`CS_ApiVersion()` = 1): `CS_RenderEventFunc` (ops 1 install, 2 frame), `CS_Init`, `CS_SetParams`,
  `CS_SetDraws(ibs, starts, n)` (the registered garment submeshes, by index buffer and first index), `CS_Stats`, `CS_LastError`,
  `CS_GetPathMaterialV1(ib, first, params)`: returns 1 and the live cloth parameters for a registered garment draw, used by the skin
  component's path-tracer scene so no second classifier exists.
- Hooks the immediate context's function table with the same template-plus-live-table chain as skin and hair-strands, so it
  works whichever patches first.
- Test API (test agent): `SetMode`, `SetLook`, `Status`.

## Tests

- `tests\build_test.cmd` (after the generator) builds `out\cloth-shading\cloth_test.exe` and runs it with the generated
  `out\gen\cloth-shading\cloth_cases\`: for each VaM clothing pixel program the original and the replacement draw the same
  32 x 32 grid of random inputs; mode 0 must reproduce the original, mode 1 must change pixels and give no NaN or infinity
  where the original has none; the D3D debug layer, if installed, checks linking and bindings. Needs a GPU and the user's VaM.
  Result with the 30 programs of this VaM: 30 cases, 0 failed, PASSED.
- Look: only the game or headset shows it.

## Invariants

- Mode 0 and every draw that is not a registered garment keep VaM's own program. Layers on the skin (makeup, wet layers,
  decals: 95% of vertices within 1 mm of the skin) and small items (under 0.15 m on every side: eye overlays, jewellery) are not garments.
- Replacement programs are matched by the SHA-1 of the original bytecode; an unknown program stays VaM's.
- VaM's specular colour, diffuse wrap, alpha, decal and environment light are kept; only the specular lobe changes.
- Nothing derived from VaM's shaders is committed.

## Read next

[../../../docs/knowledge/clothing.md](../../../docs/knowledge/clothing.md)
