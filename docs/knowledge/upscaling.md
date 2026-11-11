# Upscaling: DLSS super resolution, XeSS, FSR and Ray Reconstruction

The scene is rendered at a fraction of the size the headset (or window) shows, and a temporal upscaler rebuilds the full
image. Which one does that depends first on the in-game path tracing switch. With path tracing on, the noisy
path-traced image goes into DLSS Ray Reconstruction (RR), preset F, which denoises and upscales in one step and writes
the full-size image; nothing upscales after it. With path tracing off, VaM's own raster image goes through the model
the player chose: DLSS super resolution (SR) K, L or M on an RTX card, Intel XeSS, or AMD FSR (FSR 4 on the Radeons that
have it, FSR 3.1 on every other card), or is shown unscaled. XeSS and FSR need no RTX card and run beside DLSS on one
that has it; which cards run what, and the licences, are in [upscalers-other-vendors.md](upscalers-other-vendors.md).
A player sees: a render-scale slider whose meaning is "share of the output size", a model choice that exists only on
the raster route and lists only what the machine can run, an output-size slider in VR, and a sharpening pass at the end.

## How it works

**1. Choosing the network (every frame).** `ParseVamPerformanceSettings(pathRequested)` in
`components/render/opencomposite/OpenOVR/Compositor/vam_performance_config.h` is the only place that decides.
`pathRequested` comes from `VamPathRequested()`, which asks the skin plugin's native library
(`ST_PathTracingRequestedV1`); that follows `[RayTracing] Renderer` in `BepInEx/config/local.vam.skintransmission.cfg`
(`PathTracing` / `Legacy`) and turns false by itself when path tracing fails.

- Path tracing on: `dlssPreset = 6` (RR F) and the render scale is `[Performance] RenderScaleRR` (33-100). Whatever
  `DLSSPreset`, `RenderScale` or `RenderScaleK/L/M` hold is ignored, not erased.
- Path tracing off: `DLSSPreset` is `Off`, `K`, `L`, `M`, `XeSS` or `FSR` (the parser also accepts J). The key keeps
  its name; K / L / M set `dlssPreset`, the other two set `srBackend` (`kVamSrXess`, `kVamSrFsr`). The scale is
  `RenderScale`. A file that says `DLSSPreset=RR` is read as K at `RenderScaleK`; the launcher (`LaunchPanel.validate`)
  rewrites RR to K, the menu refuses to write it. `Off` means native frames at the output size and render percent 100.
- `Upscaler=DLSS` is fixed by the launcher and now means "the temporal upscaler fed by the bridge", whichever of the
  three it is. FSR 1 and NIS remain in the code only as the stand-in for a frame the upscaler cannot take (no capture
  yet, render larger than output, a failed feature); no setting selects them.
- **What can run is found once, with the first frame** (`ProbeUpscalers` in `dx11_vam_dlss.cpp`, on the render thread):
  DLSS SR when NGX comes up and reports it; XeSS when `libxess.dll` loads from `VAM_XESS_RUNTIME_PATH` and makes a
  context on the device; FSR when AMD's loader loads from `VAM_FSR_RUNTIME_PATH` and its upscaler lists a version for
  the device. XeSS and FSR also need Unity's device on D3D12 (the proxy). The result is logged (`VAM upscalers: ...`),
  kept in `VamSrAvailable()` and given to the menu by `VaMDLSS_GetUpscalerSupport` (bit 0 DLSS SR, 1 XeSS, 2 FSR,
  bit 3 NGX came up at all, bit 4 FSR stands in before XeSS, bit 8 once tried). Bit 3 words the line under the menu's
  path tracing row: without NGX there is no RR, and the line says so; the row itself stays.
- **One rule when the chosen model cannot run** (`VamSrChoice` in `vam_performance_config.h`): the chosen one is
  always tried first; where it cannot run, the first of XeSS, FSR, DLSS (K) that can, or FSR before XeSS on an AMD
  card, where AMD's API runs FSR 4 (the vendor of Unity's adapter; `VAMKIT_AS_VENDOR=1002` for a test). The compositor
  applies it per frame and the menu applies the same rule to what it shows (`Normalize` in `vr-menu/VaMVRMenu.cs`).
  The launcher replaces nothing: it once swapped the model before the launch, from the card's name and the files in
  the kit, and saved the swap; that decided before anything was tried and is gone (see
  [settings-and-menu.md](settings-and-menu.md)). When nothing can run (no RTX card and neither runtime installed) the frames are native, as with `Off`
  (`ReadVamLiveRender`). An upscaler that fails while it runs is dropped for the run and the next one takes over
  (`DropUpscaler`).

**2. Render size and output size.** Both are live. `VamLiveRenderSettings()` re-reads `[Performance]` at most every
250 ms. A new `OutputScale` is taken only after it has stayed the same for 0.5 s, because each one rebuilds the
swapchains, the SR/RR features and DLSS-G; the bridge settles `RenderScale` the same way
(`ScaleSettleSeconds` in `components/render/dlss-bridge/VaMDLSSBridge.cs`). The output is the runtime's recommended
eye size times `OutputScale` (75-150); in desktop mode the output is the window and `OutputScale` is not used
(`VamDesktopDisplay()`). The bridge never changes the size the game asks for; it multiplies Unity's
`XRSettings.eyeTextureResolutionScale` (`ApplyScale`) by `VaMDLSS_GetEyeTextureScale(percent)`, which clamps percent to
33..100 and, for 100, takes the largest size that fits inside the output (x0.99999) so that frame is DLAA or native.
VaM's own render-scale setting multiplies ours; if the product exceeds the output, SR pauses and FSR 1 takes the frame
until it fits again (`Eligible()` in `dx11_vam_dlss.cpp`; log "VAM DLSS SR paused").

**3. SR (raster route).** `CreateParams()` in `dx11_vam_dlss.cpp` creates one NGX DLSS feature per eye. The chosen
preset (K/L/M are the transformer presets of the 310.x runtime) is written to every quality slot, so any model runs at
any ratio. `QualityFor()` only selects DLSS's internal tuning from the input/output width ratio: equal size = DLAA,
>= .62 Quality, >= .54 Balanced, >= .42 Performance, below that Ultra Performance. So K at 33% and L at 75% both work but
sit outside what NVIDIA designed each model for (the design points are in the table below). Flags: motion vectors at
render resolution, inverted depth, HDR with auto-exposure when the target is HDR. Inputs come from the bridge
(jitter, depth, motion vectors; see [motion-vectors.md](motion-vectors.md)). The jitter phase count follows the
guide: `8 * (output/render)^2`, clamped 8..72 (`VaMDLSS_GetJitterPhaseCount`).

**XeSS and FSR (raster route).** Bindings in `.../Compositor/vam_sr_backends.h`; neither library is linked. They take
SR's place inside `Upscale()` with the same per-eye textures and guides, one context per eye:
- *XeSS* (`CreateXess`, `EvaluateXess`): `xessD3D12CreateContext`, then `xessD3D12Init` with the output size, reversed
  depth, and `XESS_INIT_FLAG_LDR_INPUT_COLOR` for the 8-bit eye image (auto-exposure for a float one). XeSS's quality
  setting only selects its tuning, like `QualityFor()` for DLSS: the setting whose ratio is nearest
  (`VamXessQuality`: Native AA 1.0, Ultra Quality Plus 1.3, Ultra Quality 1.5, Quality 1.7, Balanced 2.0, Performance
  2.3, Ultra Performance 3.0) and whose input range (`xessGetOptimalInputResolution`) holds the render size.
- *FSR* (`CreateFsr`, `EvaluateFsr`): one `ffxCreateContext` per eye through AMD's loader, with the upscaler version
  descriptor the SDK asks for; the API picks the implementation (the log names it: `version=3.1.6` on an NVIDIA card).
  Flags: reversed depth, `FFX_UPSCALE_ENABLE_NON_LINEAR_COLORSPACE` with the dispatch flag for sRGB for the 8-bit
  image; the camera's near and far plane and the eye's vertical field of view come from the guides. FSR's own
  sharpening stays off (NVSharpen follows, item 6).
- Both are evaluated like NGX in `EvaluateOn12`: the eye's textures unwrapped, the library's commands recorded into a
  list on the 11on12 queue after a `Flush` ([d3d11on12.md](d3d11on12.md)). Motion is the RG16F copy of the guides
  (render size, pixels, current to previous, jitter removed), which is each library's default convention, and the
  jitter is passed as DLSS gets it. Mip bias, the UI bypass, NR and NVSharpen are unchanged: they do not know which
  upscaler ran.

**4. RR F (path-tracing route).** `Reconstruct()` in `.../Compositor/vam_rr.inl`, called from the bridge each frame:
1. It returns false unless `dlssPreset == 6`, and false when the input is larger than the output.
2. `EnsureRr()` creates one DLSS-D (RR) feature per eye, `NGX_D3D12_CREATE_DLSSD_EXT`, with every RR quality slot set to
   preset 6 (F). Flags: HDR, low-resolution motion, inverted depth, packed roughness, hardware depth. It releases any
   K/L/M, XeSS or FSR feature and staging images first: RR owns the upscaler and no SR runs ("SR evaluations=0" in the
   `VAM RR frames=` log line).
3. The path tracer (the skin plugin's native library, see [path-tracing.md](path-tracing.md)) fills colour, diffuse
   albedo, normals (their fourth channel is read as a confidence by the cross-eye resolve), specular albedo, depth,
   motion and specular hit distance for
   the eye, in D3D12 textures built there and wrapped for D3D11 (see [d3d11on12.md](d3d11on12.md)). Exposure is a 1x1
   texture of 1.0.
4. `NGX_D3D12_EVALUATE_DLSSD_EXT` writes the full-size HDR image. It resets RR's history when the guides ask for a
   reset, the frame serial is not consecutive, or the route changed between raster and path tracing.
5. A composite pass (`Composite` in the RR shader) applies the detail-preservation factor (below), the optional
   cross-eye resolve (below), the UI layers captured by the bridge, and publishes the image. The existing tone mapper,
   then NR ([denoiser.md](denoiser.md)), then NVSharpen, then frame generation follow
   ([frame-generation.md](frame-generation.md)).

On any exception in a path-traced frame the code switches path tracing off (`ST_SetPathTracingV1(0)`), and the raster
route with its model takes over next frame ("VAM PT switched off after a failure"); a failure before path tracing started
only disables RR and retries after 1 s. This is also what a card without RTX gets: RR is an NGX feature, so path
tracing has no reconstruction there, the first path-traced frame fails, and the switch goes back to off by itself.

**5. Mip bias.** While SR or RR upscales, textures are sampled sharper: bias =
`(log2(render/output) - 1) * DLSSMipBias / 100`, 0 on native frames. On the raster route
`dx11_vam_mip_bias.cpp` hooks `PSSetSamplers` and swaps in biased sampler copies (comparison and min/max samplers and
samplers with their own bias are left alone). Only eye 0 sets it, because the two eyes read the INI on separate
schedules and would flip it. On the path-tracing route the same formula becomes `textureLodBias` of the path tracer
(`vam_rr.inl`, `p.textureLodBias`).

**6. NVSharpen.** `[Performance] DLSSSharpness` 0-100 (0 = off) runs NVIDIA's NVSharpen (NIS SDK, `NIS_SCALER=0`) in
`dx11_vam_nis.cpp`, only on 8-bit sRGB eye images (`IsSrgb(format)` gates it). Position `DLSSSharpenLast`: 1 (default)
= after NR, last stage before frame generation; 0 = right after the upscaler, before NR. Both positions share one set
of resources, one dispatch and one output per eye. DLSS's own internal sharpness parameter is ignored by current
runtimes (the RR optimal-settings call returns it as `ignoredSharpness`), which is why a separate pass exists.

**7. RR detail preservation.** `[RayTracing] RRDetail` in the skin cfg (0-100, default 75;
`ST_SetRtDetail(value/100)`). RR can smooth fine material detail (normal-map relief, texture contrast) that the path
tracer itself resolves. The path tracer therefore also writes a jittered "reference" of the current frame's material
detail. `DetailPt` in `vam_rr.inl` compares that reference, blurred 3x3, with the same band of RR's output and keeps
only the contrast RR lost with the same sign (`MissingMaterialDetail`; ratio limited to .5-2, opposing shadow edges
untouched), blends it with the previous frame's by motion vectors (75% of old when the two agree, `trust`), and
`Composite` multiplies RR's output by `lerp(1, ratio, detail)`. 0 releases the reference, the history textures and the
shader ("VAM RR detail disabled ... RR stays active") and turning it back on rebuilds them and resets history.

**8. Cross-eye consistency (VR + path tracing only).** `[Performance] RRStereo` 0-100, default 0
(`vam_rr_stereo.inl`; the menu row exists only in VR and only with path tracing on). RR reconstructs each eye alone,
so fine detail can differ between eyes. With RRStereo > 0 the composite is compiled with `RR_STEREO` and, per pixel,
finds the same surface point in the other eye geometrically: the plane through the pixel's guide normal, projected with
the other eye's matrices, then validated by depth round trip (relative 0.5%), world-plane residual, matching instance
and material ids, normal agreement (> .9), neighbour depth taps and a colour-boundary test. Where every test passes
it moves the pixel toward the other eye's RR output by at most `strength * trust`, where strength = value x 0.005
(100 = at most 50% of the other eye) and the move is capped at 15% of luminance + .002. Hair fibres, sky, mixed-coverage
pixels, surfaces dominated by specular, and clear refraction are skipped. It reads both eyes' already finished RR
output of the same frame, so it adds no ray, no NGX call, no image copy and no dependency on the previous frame. It
allocates the per-eye detail reference (about 18.6 MiB at 3072x3264 output / 35% input) even when `RRDetail` is 0.

**9. When the render size changes while playing.** Triggers: a settings change, VaM menus opening and closing
(`DLSSMenuNative`: the bridge asks for 100% native frames while a VaM menu is open, `MenusOpen()`), `OutputScale`.
- RR: `EnsureRr()` sees different sizes, flushes the D3D11 context and drains the D3D12 queue ("VAM RR resize
  synchronized ... PT scene retained"), releases and recreates the two features and their textures. The path-tracing
  scene (materials, hair caches) is kept; the bridge deliberately does not invalidate the path consumer on a size
  change (that tore down every cache and was slow).
- SR: `ReleaseFeature()` drains first; a size that fails to create is remembered (`badW/badH`) so it is not retried
  every frame.
- NR keeps the previous size's feature per eye (+82 MiB) so a menu open/close costs 24-43 ms instead of 208-288 ms.
- Writing model and scale in one menu action makes OC build the feature at the old size, then again at the new one:
  1-3 s of hitching per switch; DLSS-G also rebuilds on each output change.
- With path tracing on, the simulated headset kept the RR input at the chosen scale while a VaM menu was open and did
  not switch to native frames (VR menus are composited on top by the path-tracing composite instead). Reading
  `VaMDLSSBridge.cs` alone, `MenusOpen()` is not gated by path tracing, so check the log lines "VaM menus open" and
  "native frames on" on your build before relying on this.

## Settings

| File / section / key | Values | What it trades |
|---|---|---|
| run `performance.ini` `[Performance]` `DLSSPreset` | `Off`, `K`, `L`, `M`, `XeSS`, `FSR` | Raster route only. K cheap and steady, M sharper and costlier with input size, L for very low input; XeSS and FSR for cards without RTX, and a second choice with it (FSR is the cheapest of all, see Measured) |
| `RenderScale` | 33-100 | Raster route: input as a share of output. The menu copies it into the chosen model's own key |
| `RenderScaleK` / `L` / `M` / `XeSS` / `FSR` | 33-100, defaults 75 / 33 / 50 / 67 / 67 | What each model remembers; picking a model restores its own |
| `RenderScaleRR` | 33-100, default 33 | Path-tracing input share. Cost of path tracing and RR both scale with it |
| `OutputScale` | 75-150, default 100 | VR only: output = runtime size x this. 100 = the runtime's recommendation |
| `DLSSMipBias` | 0-100, default 50 | Texture sharpness vs shimmer; not written by the launcher, default applies |
| `DLSSSharpness` | 0-100, default 25 | NVSharpen strength |
| `DLSSSharpenLast` | 0 / 1, default 1 | 1 = after NR (last), 0 = right after the upscaler |
| `RRStereo` | 0-100, default 0 | VR + path tracing only; GPU cost small, see Measured |
| `DLSSMenuNative` | 0 / 1, default 1 | VaM menus open: render native, no SR/RR, frame generation paused |
| skin cfg `[RayTracing]` `RRDetail` | 0-100, default 75 | 0 frees the reference textures; higher restores more relief |
| `DLSSObjectMotion`, `DLSSHairMotion`, `DLSSRigidMotion`, `DLSSJitterFlipX/Y`, `DLSSHairBias`, `DLSSFGEyes`, `DLSSSREyes` | fixed by the launcher | Diagnostics and motion sources. The launcher writes these values at every launch (`FIXED` in `LaunchPanel.py`); to try another, change it there or in the run's file while it plays |

The 33 floor is DLSS Ultra Performance (1/3 per axis); NIS could only do 2x (50%). The in-game menu has the same
bounds; if you change a limit, change `LIMITS` in `launcher/LaunchPanel.py`, the `Number(...)` range in
`vr-menu/VaMVRMenu.cs`, and the `read(...)` clamp in `vam_performance_config.h` together
(see [settings-and-menu.md](settings-and-menu.md)).

## Measured

Per-eye SR cost, output 3072x3264 per eye, from 55 in-game runs on the development machine
(`frame-budget.md`) on the earlier pipeline; whether each ran in the simulated headset is not recorded:

| Render scale | K ms per eye | M ms per eye |
|---|---|---|
| 50% | 1.06-1.15 | 1.19-1.32 |
| 60% | 1.11 | 1.69-1.78 |
| 70% | 1.09-1.15 | 2.14-2.21 |
| 80-90% | 1.06-1.11 | about 3.05 |

K does not grow with input; M does. L was not measured. NVIDIA's own table for the Performance mode at 4K output on an
RTX 5090 is K .87, M 1.05, L 1.37 ms. In the same environment L at 33% on a heavy scene still left the GPU at 98%: the
render scale is no longer the main lever there.

NVIDIA's design points (programming guide, per axis): DLAA 1.0, Quality 1/1.5, Balanced 1/1.724, Performance 1/2,
Ultra Performance 1/3; its automatic mode picks Quality up to 1440p output, Performance to 4K, Ultra Performance above,
which means the input never goes below about 1280x720. Per eye at 3072x3264 that points to L33; 2688x2880 and
2112x2304 point to M50. These tables are for flat displays; that VR needs one tier higher is an inference, never tested.

Suggestions that were given for the raster route: M 50% for heavy scenes, K 60-70% for ordinary ones, K 80-90% for
light ones; raising M above 60% costs +1.3 / +2.2 / +3.9 ms per real frame (60 / 70 / 80-90%) and is only worth it
when hair or motion ghosting is objectionable. M is sharper with less ghosting but can moire on fine repeating textures.

XeSS and FSR against DLSS K, one session on the desktop (RTX 5090 D, window 3840x2160, input 67% = 2573x1447, one
view, a still person in front of black, NR and sharpening off, the model switched inside the session):

| Model | Upscale stage per view | Distance from the native frame (PSNR of luminance) | Same, jitter sign flipped in x / y |
|---|---|---|---|
| DLSS K | 1.08 ms | 39.5 dB | 37.8 / 37.0 |
| XeSS 2.0.2 (cross-vendor kernel), setting Ultra Quality | 1.45 ms | 38.9 dB | 38.1 / 36.6 |
| FSR 3.1.6 | 0.50 ms | 39.4 dB | 38.3 / 37.9 |

The stage times are 10 s averages of the compositor's `upscale` stage (`VAM GPU timing eye submission`) over spans
with one model. The PSNR column compares one captured frame with the native frame of the same still scene, which has
no anti-aliasing, so it mostly measures how the edges were resolved: it shows the three agree, not which looks better.
The last column is the check that the jitter convention is right for each library: every model is closest to native
with the jitter as passed and further with either sign flipped. At 33% input XeSS (Ultra Performance) and FSR gave
38.2 and 38.0 dB, at 100% 39.9 and 39.4. No judgement of shimmer in motion was made.

With NGX refusing (`VAMKIT_NO_DLSS=1`, the same card, desktop, one session of 100 s): the launcher's run held XeSS in
place of K; XeSS ran 5400 evaluations, FSR was switched in and out live, and K written into the file while it played
was run as XeSS (`the one chosen cannot run here`); NR, switched on, logged once that it cannot run and was not tried
again; path tracing asked for through the skin plugin gave one RR failure and raster frames went on; the menu listed
XeSS and FSR and had no path tracing row. This is the fallback on an NVIDIA card, not another vendor's driver.

Per eye on the simulated headset (144 Hz, 1528 x 1624 in, 3072 x 3264 out, the same scene, one session with the
model switched live, the same stage timer): K 0.96-0.97 ms, XeSS (setting Performance) 1.24-1.26 ms, FSR 3.1.6
0.40-0.43 ms. K agrees with the table above (1.06-1.15 on the earlier pipeline).

Mip bias (simulated headset): 50 gave +17-43% detail and -2..+11% shimmer over 0; 100 (NVIDIA's value) turned thin
wood seams into dotted lines that change every frame. Other levers measured and left unchanged: NR intensity 50 gave
+16% detail and -40% skin shimmer; K at 50% +20% still detail but fixed dot patterns on hair; render scale 67% +15%
detail; hair bias 100 made hair noisier. Motion costs about 17% of detail (-8% real frames, a further ~15% softer on
generated frames).

Sharpening was first an RCAS pass (on a dancing character 25: +20% detail, +12% shimmer when still; 50: +25/+32%,
+19%; 75: +58/+72%, +59%). NVSharpen replaced it; the NVSharpen numbers were not measured.

Cross-eye consistency, one-character scene, input 35%, output 3072x3264 per eye, `RRDetail` 0, a single captured frame
replayed offline: the difference between eyes over the shared person area fell about 36% (face about 27%). That is a
metric before NR; it does not show the headset is more comfortable. Fused GPU cost +0.15-0.16 ms per real frame
(both eyes); a full in-game window with it off/on was 52.3 -> 51.9 fps, and 4x short runs reached 36 fps (simulated
headset, 144 Hz). Not looked at by a person in a headset.

## Pitfalls

- **Render scale does nothing under path tracing.** Cause: `RenderScale` and `RenderScaleK/L/M` are ignored while path
  tracing is on. Edit `RenderScaleRR`.
- **`DLSSPreset=RR` gives K.** Intended since the raster route has no RR (see Tried and dropped).
- **The chosen model is not the one running.** It cannot run on this machine and `VamSrChoice` put another in its
  place; the log says `VAM upscaler: XeSS (the one chosen cannot run here)` and why the chosen one was refused
  (`VAM upscalers: ... cannot run here: ...`). A missing runtime folder (`vamkit/bin/xess`, `vamkit/bin/fsr`) is the
  usual cause: the runtimes are optional items of `vamkit fetch`.
- **"FSR" on an NVIDIA or Intel card is FSR 3.1.** AMD's API runs FSR 4 only on Radeons; the `version=` in the log
  line `VAM FSR feature created` is what runs; the menu's label says FSR, not which version.
- **XeSS and FSR do nothing for path tracing.** RR is the only reconstruction the path-traced route has here. Without
  NGX the menu's row stays and the line under it says what is missing (`pathOffered` in `VaMVRMenu.cs`); switching it
  on gets one failed attempt (`VAM RR device/resource failure`), the native request is switched off (item 4) and the
  raster route with XeSS or FSR goes on. The cfg value itself still says `PathTracing`. What it would take to show
  path tracing without RR is a denoiser that is not NVIDIA's between the tracer and the upscaler; the tracer's own
  outputs (colour, albedo, normals, specular, depth) are what `VaMDLSS_Reconstruct` already receives.
- **Switching model and scale together builds the feature twice** for XeSS and FSR as for DLSS (item 9): first at the
  old size, then at the new one.
- **VaM died with an access violation at the first XeSS or FSR frame, only where DLSS could not run.** The 11on12
  command ring (`VamOn12Commands`) was brought up inside NGX's set-up alone; with NGX refusing, the first evaluation
  recorded on a ring without a device. `ProbeUpscalers` now brings the ring up itself (`CommandRing`) before it offers
  XeSS or FSR. Rule: whatever records on the ring calls `on12Commands.Init` on its own path; never rely on NGX having
  run. Found with `VAMKIT_NO_DLSS=1`, which is why that switch must be part of any change to this code.
- **The denoiser (NR) retried once a second for ever on a desktop without NGX.** Its retry was meant for resource
  failures. NGX not coming up is now `unavailable`: one log line (`NR prepass cannot run on this machine`), no retry.
- **SR pauses and the image goes soft.** VaM's own render scale times ours exceeds the output; set VaM's to 1.0.
- **Small menu text shakes with SR on.** The upscaler plus jitter smears thin glyphs on head turns; menus therefore
  render as native frames (`DLSSMenuNative`). A UI mask with FSR 1 for those pixels was tried and still shook.
- **Ghosting behind non-depth-writing transparents** (world UI panels, glowing signs, hair alpha edges) on head turns:
  they do not write depth, so the upscaler reprojects what is behind them. Not a motion-vector bug.
- **Hair ghosting at 50%.** It comes from the hair strands themselves flickering at low input, not from wrong motion
  vectors; measured: captured vectors and camera-only vectors give the same error. Lever: raise the scale or change NR/K.
- **Dark spots that grow on still skin under RR, gone when the camera moves.** The path tracer's input is heavy-tailed
  (one shadow ray per pixel; rare samples 200-300x the mean), which RR cannot converge when still. Not an RR setting;
  see [path-tracing.md](path-tracing.md).
- **Switching size or model hitches 1-3 s**, see item 9. Avoid bulk changes during play.
- **Hang at the moment a menu opens or closes (once, twice in thousands)**: see Open.

## Tried and dropped

- **RR on the raster route.** RR copies its input after the main camera's transparents, so UI drawn later by another
  camera (the desktop world-canvas menus of a third-party stage) was overwritten by RR's output; SR reads its input at
  present time and has no such problem. Raster-route RR was removed from every setting; the code paths for it remain in
  `Reconstruct()` and are unreachable.
- **SR for both eyes in one image or on two queues.** Offline, two eyes in one queue / two queues / one packed image:
  L35 2.24 / 2.26 / 2.30 ms, M50 2.55 / 2.60 / 2.74, K75 1.98 / 2.10 / 2.11. SR time follows output pixels and has no
  fixed per-evaluation cost to save (NR's packing wins because its regions are small). The GPU is full, so two queues do
  not help either.
- **Mip bias 100**, see Measured. **RCAS** sharpening was dropped for NVSharpen. **A DLSS-internal sharpness** does not
  exist in current runtimes.
- **C model (frame generation lite) as a way to save time** was rejected on quality on the development machine as a
  default; it stays a setting, `DLSSGLite` ([frame-generation.md](frame-generation.md)).
- **Response mask for RR to fix still-skin spots**, **a per-pixel "stable guide" from nearest-hit rays**: both removed;
  the fix was in what the path tracer feeds RR ([path-tracing.md](path-tracing.md)).

## Open

- XeSS and FSR were run on one NVIDIA card. On Radeon and Arc cards nothing was run: whether NGX fails quietly there
  everywhere the compositor calls it, what XeSS's own kernel and FSR 4 cost, and how they look are unknown. On the
  development machine `VAMKIT_NO_DLSS=1` in the launcher's environment makes NGX refuse (no DLSS SR, no RR, no NR),
  which tests the fallback (see Measured) but not another vendor's driver.
- XeSS and FSR per eye in a headset: built and started on the simulated headset, never judged by a person; the vendors
  document neither for stereo.
- No L-model cost measurement; no side-by-side of the K/L/M choice on a real headset at the 1/3 floor.
- Whether VR wants one quality tier above NVIDIA's flat-display table is unmeasured.
- Cross-eye consistency is a prototype, off by default; no person has judged it in a headset. NR after it can reintroduce
  differences and has no joint two-eye constraint yet.
- A GPU hang (driver reset after about 2 s) happened twice in a multi-day session, both exactly at a VaM menu
  open/close that swaps the eye texture between 50% and 100% (about 3 GB of render targets each time). It is not memory
  exhaustion, not a leak, not settings polling. Which GPU task stalls is unknown; D3D12 DRED or Nsight Aftermath would
  tell. Workaround: turn `DLSSMenuNative` off. A different, fixed crash at size changes (a desktop post-processing UI
  camera still pointing at the destroyed input texture) is handled by retargeting cameras before release.
- Under path tracing, the exact behaviour of `DLSSMenuNative` (item 9).

## Verify

- Offline: the path tracer and the composite have probe tools inside the skin and rt-core components (see
  [../protocol/testing.md](../protocol/testing.md)); nothing offline covers the NGX calls.
- Simulated headset (`tools/lab/simxr/`, see [frame-budget.md](frame-budget.md) for what it lacks): start a run with
  the settings above, then grep the OpenComposite log for `VAM RR F settings`, `VAM RR created eye=`, `VAM RR frames=`,
  `VAM DLSS SR eye=... input=... output=...`, `VAM native frames on/off`, `VAM RR resize synchronized`, and the bridge log
  for `render scale N% of the output`. Change a key in the run's `performance.ini` while it plays; the compositor follows
  within 250 ms.
- What was asked for and what runs, for a program: `running.json` in the run's `dlss` log folder (`WriteRunning` in
  `dx11_vam_dlss.cpp`): `upscaler` {`wanted`, `running`, `reason`}, `can_run` and `why_not` per backend,
  `stands_in_first`, `path_tracing` {`wanted`, `running`, `can`, `reason`}. `wanted` is the setting as chosen (`K`,
  `L`, `M`, `XeSS`, `FSR`, `Off`, or `RR` while path tracing is asked for), `running` what the frames went through
  (`FSR1` when none of the three can). It is rewritten when an upscaled frame finds it changed; while VaM's menus
  are open the frames are native and the file keeps what it said. Read it rather than the log's wording.
- XeSS and FSR: `VAM upscalers:` (one line per backend: can run, or why not), `VAM upscaler: <model>` at every change,
  `VAM XeSS feature created: eye=... setting=...`, `VAM FSR feature created: eye=... version=...`, and the evaluation
  counts `VAM XeSS eye=0: N evaluations`. On the desktop `VaMDLSSBridge.CaptureDesktop(path)` (through the test agent)
  writes the compositor's output as a `.ppm`; a still scene captured with each model and with `DLSSPreset=Off` gives
  the comparison in Measured.
- Only a person in a headset can judge shimmer, ghosting, cross-eye comfort and sharpening taste.
