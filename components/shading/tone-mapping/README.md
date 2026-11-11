# tone-mapping

VaM's eye camera renders LDR with no tone mapping, so light above 1.0 is cut off flat. While on, this renders the main
camera (`CameraTarget.centerTarget`, the stereo eye camera in VR) in HDR and applies bloom plus a highlight shoulder
(style 0, neutral) or a film curve (style 1) before the result goes on. With path tracing on, DLSS RR rebuilds the image
first and the tone mapping runs on the reconstructed image (the compositor binds RR's output). Depends on BepInEx and
Unity's D3D11 device; soft dependency on display-lighting (mode 1 follows it).

## Files

| File | Holds |
|---|---|
| `plugin/VaMToneMapping.cs` | BepInEx plugin `local.vam.tonemapping` 0.2.2: HDR on the camera, `OnRenderImage` hook, mode / style, test API |
| `native/tonemap.cpp` | Native library: bloom chain, shoulder, film curve, in-place composite on a UAV-capable target; render event op 1 |
| `shaders/tonemap.hlsl` | Compute programs `CSDownFirst`, `CSDown`, `CSUp`, `CSComposite`, `CSCompositeFilm`, `CSCompositeInPlace`, `CSCompositeFilmInPlace` |
| `tools/build_shaders.py` | Compiles the shaders with fxc into `native/tm_shaders.h` (generated, not committed) |
| `tests/tm_test.cpp`, `tests/build.cmd` | Offline check on a synthetic two-eye FP16 target |
| `native/build.cmd` | Native build |

## Build

`python tools/vamkit build tone-mapping` runs every step for this component (generators, native library, plugin) and
writes to `out\tone-mapping\`; the steps are listed in `manifest/components.toml`. The plugin is compiled by that
command from the `[component.managed]` recipe there (C# 3.5 compiler, against the user's VaM).

Environment: `VAMKIT_FXC`, `VAMKIT_VCVARS`, `VAMKIT_CSC`. From the repository root:

```
python -I components\shading\tone-mapping\tools\build_shaders.py
components\shading\tone-mapping\native\build.cmd
```

`build_shaders.py` compiles `shaders\tonemap.hlsl` with `fxc` into `out\gen\tone-mapping\tm_shaders.h` (listings under
`out\tone-mapping\shaders`). `native\build.cmd` writes `out\tone-mapping\VaMToneMappingNative.dll` and copies it to `.bin`
(the name the plugin loads). The managed plugin `plugin\VaMToneMapping.cs` is a BepInEx plugin against the user's VaM
(`VAMKIT_VAM`, `VAMKIT_CSC`); built by the common managed build.

## Settings

`BepInEx/config/local.vam.tonemapping.cfg`, both live and also set from the in-game menu (vr-menu):

| Section / key | Default | Meaning |
|---|---|---|
| `[General] Mode` | `1` | `0` off (VaM unchanged, nothing attached); `1` only while display-lighting lights the scene; `2` always. The launcher's install can set it |
| `[General] Style` | `0` | `0` neutral: below the knee (0.6) every value stays as VaM drew it, above it a roll-off to white plus a warm glow above the old clip point; `1` film curve with toe, more mid contrast and a print's colour (changes mids and shadows) |

## Interfaces

- Native exports: `TM_ApiVersion()` (2), `TM_Init`, and the parameter / report entry points the plugin binds with
  `GetProcAddress` (VaM's Mono resolves `[DllImport]` only from `VaM_Data\Plugins`).
- Render event op 1 (`OpTonemap`), eyes in bits 16-23: the plugin's `OnRenderImage` binds the source HDR target as the render
  target and issues the event. A target that is not a single-sampled float 2-D texture is left alone and counted
  (`Report()` says why).
- Test API for the VaM test agent: `Mode(m)`, `Style(s)`, `Params(knee, threshold, softKnee, intensity, r, g, b)`,
  `Film(...)`, `Report()`.
- With RR running, the compositor (opencomposite) binds RR's image as a UAV target and the last pass writes into it in
  place (0.2.2), so tone mapping and RR agree on who owns the HDR camera.

## Tests

- `tests\build.cmd` (after the native build) builds `out\tone-mapping\tm_test.exe` and runs it on the native DLL:
  (1) style 0 leaves pixels at or below the knee unchanged and style 1 matches the film curve on the CPU; (2) a flat area
  above the knee follows the shoulder formula; (3) a bright patch in one eye puts no light into the other; (4) NaN and
  infinity stay put and do not bleed; (5) the bound render target is restored. Optionally pass the path of a
  0.1.0 `VaMToneMappingNative.dll` as the first argument: style 0's output must then be byte for byte that build's.
- Whether it looks right is a matter for the game or headset.

## Invariants

- Style 0 below the knee returns VaM's values exactly (FP16-exact); keep it when touching the curves.
- Pipeline state touched (compute slots, output-merger targets) is put back as it was.
- With Mode 0 nothing is attached and the camera keeps its own HDR setting.
- A UAV-capable target gets the last pass in place, each texel read and written by its own thread.

## Read next

[../../../docs/knowledge/tone-and-display-lighting.md](../../../docs/knowledge/tone-and-display-lighting.md)
