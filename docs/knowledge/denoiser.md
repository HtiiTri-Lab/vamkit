# The neural denoiser (NR)

After the upscaler has produced the full-size eye image, NVIDIA's neural denoiser (the "DLSS NR" model,
`nvngx_dlssnr.dll`, version 310.9.16) can be run over a part of it. In this project it is an image-quality pass for
skin: it removes remaining noise and shimmer and sharpens local tone and structure (pores, lines, shading), at the
cost of a few percent of the frame. It is not the path tracer's denoiser; that is RR ([upscaling.md](upscaling.md)).
NR runs after RR on the path-tracing route, and before or after SR on the raster route. A player sees a "noise
reduction" page in the menu with strengths, a region size and a position; without the runtime file the page does
nothing and the rest of the mod works.

## How it works

**Loading.** There is no public SDK or download for NR; NVIDIA publishes none. The file comes inside some games.
`vamkit fetch --file <path>` (see `manifest/thirdparty.toml`, id `dlss-nr`) checks a copy the user points it to for the
pinned version and an NVIDIA signature, and installs it as `<VaM>/vamkit/bin/dlss/nr/nvngx_dlssnr.dll`; the launcher
passes that folder as `VAM_NR_PREPASS_PATH` (`launch.py`, `mod_environment`). The agent must not fetch it from an
unverified source and cannot ship it. If the file is missing, wrong or fails to load,
`components/render/opencomposite/OpenOVR/Compositor/dx11_nr_prepass.cpp` throws in `LoadManualNr()`, logs "NR prepass
disabled after failure; OFXR continues with original real frames", writes `failure.txt` into `VAM_NR_PREPASS_LOG`, and
NR stays off for the run. A new version of the DLL may break the manual loader; the failure mode is NR off, nothing else.

The loader is manual because NGX keeps the first D3D12 setup of a process, and DLSS Frame Generation (OFXR) sets NGX
up first with a path that has no NR library. NR therefore initialises its own NGX D3D12 instance with the author's
direct DLL loader, with a Detours hook on `GetModuleFileNameW` that reports the NR module as `nvngx.dll` to itself
(`NrGetModuleFileNameW`).

**Per frame** (`dx11_nr_prepass.cpp`, driven from `dx11_vam_nis.cpp`, class `VamNis`):
1. Settings: `ReadSettings()` reads the `[NR]` section and a few `[Performance]` keys of the run's `performance.ini`.
   It runs again only when the file's stamp changed, checked every 250 ms (`ReadSettingsIfDue`). Values from the INI
   override defaults that come, if present, from registry values `DLSSNR_*` and `FoveationSize` of the Virtual Desktop
   streamer (`HKLM\SOFTWARE\Virtual Desktop, Inc.\OpenXR`), and then from the code defaults below.
2. Region: a rectangle of `FoveationSize` percent of the eye image around the projection centre, moved up by
   `RegionShiftY` percent of the eye image height (negative = down), with an edge feather of `NRFeatherPercent` percent
   (`featherShader`: NR's result blended into the original inside the feather).
3. Guides: depth and motion resampled to NR's image size (`PrepareOutputGuides`) when `NRUseDepth` / `NRUseMotion` are 1
   (fixed by the launcher).
4. NR input is RGBA8 (gamma-encoded) or RGBA16F; anything else throws "NR prepass requires RGBA8 or RGBA16F eye images".
   NR itself gets `UseAutoMask = true`, `Style`, the strengths, and never a `ControlMask` (see Pitfalls).
5. Position (`NRAfterUpscale`): 0 = before the upscaler, at render resolution (cheap when the render is small); 1 = after,
   on the output. Under RR the position is forced to after (`after = nrAfterUpscale || dlssPreset == 6`).
6. Reduced mode (`NRAfterScale` < 100, after the upscaler) in `DenoiseReduced()`: reduce the output to that percent per
   axis (box taps, up to 4), run NR on the reduced image, then write `output + bilinear_upscale(NR(reduced) - reduced)`.
   The reason: NR's convolutions and attention windows have fixed pixel sizes and the network runs at exactly the size it
   is given (offline: requested = network size, scale 1.00). So NR on the output sees skin at a pixel scale that moves
   with the render scale and the headset's size; reducing to a fixed percent gives the same look whatever SR does, and
   costs (output/render)^2 less than NR at full output size. Adding the edit instead of upscaling the NR image keeps full
   resolution detail.
7. Packing both eyes into one evaluation (`ProcessPair`, `[NR] PackEyes`, default 1): each evaluation has a fixed cost of
   about 0.75-0.95 ms and only 0.34-0.47 ms per megapixel on top, so the two eyes' regions are copied side by side onto
   one canvas and evaluated once. Eye 0's submission also prepares eye 1 ahead of time (before the upscaler:
   `PackAhead`; after RR: `UpscaledAhead` fetches eye 1's RR output, `PackReducedAhead`, `ReducedAheadEye`), and eye 1's
   submission only applies its part. With K/L/M after the upscaler, or with sharpening before NR, eye 1's input does not
   exist yet when eye 0 runs, so those combinations run NR per eye.
8. Sharpening and frame generation come after ([upscaling.md](upscaling.md), [frame-generation.md](frame-generation.md)).

**The seam width.** NR's result depends on the canvas width, in steps, not on the other eye leaking in. Gaps found
offline (black columns between the two regions), implemented in `PackGap(regionWidth)`:
- region width <= 760: gap 96; <= 1100: gap 64. (L33, 608 wide: gap 0 gave visibly warmer images and deeper eye sockets;
  gap 80-112 matched separate evaluation; >= 224 got worse again. M50, 916 wide: any gap 0-128 was fine.)
- region width 1840-1852 (NR after RR at 75% of a 3072-wide output, 80% region = 1844 wide): the canvas is fixed at
  3704 wide (gap = 3704 - 2 x width). Canvas 3688-3704 matched separate evaluation; 3720-3832 shifted tone by about 4
  levels (a third to a half of NR's own effect); >= 3848 about 1-1.5 levels.
- any other width: not packed ("NR prepass not packed ... each eye on its own"). If you change `FoveationSize`,
  `NRAfterScale`, the render scale or the output size so a region lands outside these windows, packing silently stops
  and NR costs more; measure a new gap with the offline harness before adding a window.

**Model choice.** `[NR] RenderPreset` is `DLSSNR.Hint.Render.Preset`, a creation parameter: 2 = the small FP8 model of
310.9.16 (also the DLL default, so 0 means the same), 1 = the 310.8.0 model (output bit-identical to that DLL, same
cost). Changing it recreates the feature.

## Settings

All in the run's `performance.ini`; the menu's "noise reduction" page writes them live (see
[settings-and-menu.md](settings-and-menu.md)).

| Key | Range | Code default (launcher seed) | Meaning |
|---|---|---|---|
| `[NR] Enabled` | 0/1 | 1 | Off = no NR work |
| `[NR] RenderPreset` | 1-2 (0 = DLL default) | 0 (2) | 2 new model, 1 old model |
| `[NR] Style` | 0-2 | 0 | Passed to `DLSSNR.Style`. Menu labels 0 (default), 1, 2; what 1 and 2 look like is not recorded |
| `[NR] Intensity` | 0-100 | 100 | Overall strength (scaled to 0..1) |
| `[NR] LocalToneStrength` | 0-200 | 20 (80) | Local tone / shading. The first thing to raise if NR looks weak |
| `[NR] LocalStructureStrength` | 0-200 | 70 (100) | Local structure |
| `[NR] SkinStructureStrength` | 0-200 | 50 (85) | Structure on skin found by NR's automatic skin mask |
| `[NR] FoveationSize` | 10-100 | 66 | Region size, percent of the eye image |
| `[NR] RegionShiftY` | -30..30 | 0 | Region moved up (positive) by that percent of the image height; each change resets NR history |
| `[NR] PackEyes` | 0/1 | 1 | Both eyes in one evaluation; not in the launcher or menu |
| `[Performance] NRFeatherPercent` | 1-45 | 10 | Region edge feather |
| `[Performance] NRAfterUpscale` | 0/1 | 0 (1) | Before / after the upscaler; ignored (always after) under RR |
| `[Performance] NRAfterScale` | 25-100 | 100 (50) | Reduced mode percent, after-upscaler only. 100 = NR on the full output |

The "code default" column is what `ReadSettings()` falls back to; the "launcher seed" in brackets is what a fresh
profile gets from `initial_settings()` in `launcher/LaunchPanel.py` (registry values, if present, replace it). The two
differ; the profile's value is what runs. The ranges are enforced in three places
that must change together: `ReadSettings()`, `LIMITS` in `LaunchPanel.py`, and the `Number(...)` rows in
`vr-menu/VaMVRMenu.cs`.

## Measured

New model vs old, offline, one eye at 1224x1300: 2.81 -> 1.48 ms GPU (-45-47%), video memory -25%. Simulated headset,
frame generation off: NR 2.82 -> 1.81 ms per eye, real frames +21-24%. The new model acts about 80% as strongly as the
old (keeps more detail, brighter, fewer deep shadows); it pulls skin structure toward 100 only a little; and it also
touches pink-lit background walls (NR takes them for skin).

Packing, offline, both eyes: L33 2.235 -> 1.29 ms, M50 2.657 -> 1.58 ms. Simulated headset, frame generation off, NR before
the upscaler: VaM frame 15.01 -> 13.70 ms, NR 2.54 -> 1.52 ms; skin looked the same, a 6x-amplified difference image
showed a local tone difference of 2.5-2.9 levels on one eye. Packing the reduced images after RR (measured when the
raster route could still use RR; simulated headset, heavy two-character scene, RR input 50%, region reduced as described
above): NR for both eyes
5.04 -> 4.44 ms; the same idea under path tracing at input 33%: 5.22 -> 4.45-4.62 ms; real frames with frame generation
off 55.83 -> 58.32 / 57.89 and 55.64 -> 59.38 / 58.29 fps. Cost: with guides, packing makes NR darken bright skin
pixels slightly more (offline: NR itself darkens 25-31 levels, packing adds 2.1-2.4; in game mostly the left eye: path
tracing -2.6 / -2.85, raster route -0.6 / -1.7 levels). Region averages hide this; compare skin pixels with a signed
difference.

Reduced mode, simulated headset, DLAA input (render = output), NR before the upscaler as the baseline, region 70%, 50%
reduction: VaM frame 24.1 -> 21.4 ms with frame generation off, NR for both eyes 6.0 -> 3.3 ms. Skin fine lines were
clearer (high-pass x1.2-1.45, since half-size NR cannot touch detail under 2 pixels) and still-frame temporal jitter a
little higher (< 0.4 level). Motion and a real headset were not measured. It only saves time when the render is at
output size; with real upscaling NR before the upscaler is already cheap.

Intensity: 50 gave +16% detail and -40% skin shimmer (earlier measurement, model and route not recorded). NR is
deterministic (repeat runs differ by 0). Blending NR's output back toward its input by 60% is about the same as
multiplying tone, structure and skin strengths by 0.4.

## Pitfalls

- **Never pass a `DLSSNR.ControlMask`.** Any mask, whatever its content, makes the result bit-identical to
  `UseAutoMask = 0` and disables skin structure strength. To weaken NR in an area, blend its output back yourself.
- **`GlobalToneStrength` and `ScalingRatio` do nothing.** Output and time did not change; do not pass them.
- **Do not try to make NR scale internally.** The library has `ScalingRatio`, backbuffer, UI inputs and a quality-based
  scaling callback, which suggests it was designed to run at final size; but the callback only accepts Quality 1.0,
  UltraQuality .85 and DLAA 1.0, other values report "not supported", creation or evaluation with a quality value or ratio
  leaves the network unchanged, and a colour image smaller than the output is rejected (colour and output must be the
  same size). The reduced mode is the substitute.
- **Packed result looks warmer or darker than unpacked.** The canvas width is outside a known-good window (see seam width).
- **A one-frame spike of 20-38 ms GPU about 10 s after any feature creation, and a 122-309 ms stall during creation.** NR's
  own behaviour, both DLL versions. The menu keeps the previous size's feature per eye so open/close does not recreate it
  (+82 MiB). A new render scale or region size still creates one.
- **NR must run on native frames too.** An earlier version skipped them to avoid the rebuild, so NR vanished whenever a
  VaM menu was open. Now it runs on them in either position; with NR before the upscaler, opening or closing a menu
  rebuilds NR for the native size.
- **A different NR DLL can fail in the manual loader.** Symptom: NR off, `failure.txt`. Check signature and version first.

## Tried and dropped

- **Nasolabial-fold reduction** (blend NR back toward its input inside a band along the fold of female faces, from a
  per-vertex base-face buffer drawn by the skin pass): NR deepens those folds on VaM faces. Removed as unwanted; the
  `NasolabialReduction` key is ignored (`nrFold` is always 0). Reducing NR over the whole face was also refused. Remnants of
  the guide code (`kStateNrFold`) exist; do not rebuild the feature without asking.
- **Guided-filter style transfer of NR's edit** in reduced mode: halos at hair edges; adding the edit directly won.
- **Idea from a public project (Prism-NR, read only):** evaluate every N frames, warp NR's edit along motion vectors in
  between, NR on an async compute queue. Not adopted: whole-frame alternation conflicts with frame generation (real frames
  occupy 4 of 5 slots, so alternating means visible hitches); an async queue gained nothing because the GPU is full (the
  earlier "async post" experiment showed no benefit); tiling and HDR exposure encoding do not fit (VaM is LDR). Two-eye
  alternation with warping the other eye's edit was not done: motion and eye-difference risks, needs a motion side-by-side
  first. Region/resolution reduction plus carrying the edit was adopted (reduced mode).
- **Packing as a way to make the two eyes' tone agree.** An early estimate said it would; measurement showed no
  improvement.

## Open

- Eye-to-eye tone agreement after NR is not constrained (see the cross-eye note in [upscaling.md](upscaling.md)).
- What `Style` 1 and 2 do visually, and which is best for which skin, is not recorded.
- Under motion and in a real headset, reduced mode and packing were not measured.
- Whether the extra darkening of packed skin matters in a headset is not judged.
- The 10 s spike after creation is not removable from the application side.

## Verify

- Simulated headset: grep the OpenComposite log for `NR prepass packed evaluation submitted successfully`, `NR prepass:
  both eyes packed`, `NR prepass not packed`, `VAM NR after the upscaler eye=... on the output reduced to N%`, `VAM NR
  eye=1: image from eye 0's packed NR evaluation (PackEyes)`, and the per-stage `VAM GPU timing eye submission eye=N ...
  nr ... denoise ...`. A `failure.txt` in the NR log folder means NR is off.
- Offline: the original investigation used a standalone NR harness with canvas-width sweeps; it is not in this repository.
  To re-measure a seam, evaluate the same pair of crops at several canvas widths and compare skin pixels with a signed
  difference ([../protocol/testing.md](../protocol/testing.md)).
- A person has to judge taste: show a side-by-side with NR on, since NR changes skin appearance more than any other pass.
