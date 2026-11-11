# Directions not taken, and problems still open

Two lists so that nobody spends a day rediscovering them. First, directions that were evaluated and not taken, each with the
reason and the condition under which the answer would change. Second, problems that are unsolved: what is known, what was ruled
out, and how to continue. Items that belong to one subsystem are kept short here and detailed in that subsystem's document.

## Not taken

### Stereo frame generation (a "StereoFG" style model)

- What it is: a model that generates the *other eye* from one eye, not an interpolation in time: each frame only one eye is
  shaded (left and right alternating) and the model needs both eyes' G-buffers, depth and stereo motion vectors.
- Why not: VaM is forward-rendered in a single double-wide pass. There is no G-buffer, and it cannot render one eye per frame. The
  model's weights are trained on game-engine scenes and have not seen skin and hair. Compute: at a Quest 3 panel's resolution the
  full model costs about 5.6 TFLOP per frame (at least 27 ms even in the ideal case), the "mini" variant about 1.5 TFLOP (at least
  7 ms) - more than the roughly 6 ms the second eye's rendering would have cost.
- If cross-eye reuse is ever asked for again, the only workable form found: one eye runs the denoiser or frame generation, the other
  is a depth reprojection with hole filling. Measure the reprojection error on same-frame stereo screenshots offline before building
  anything. (A smaller idea was built instead: an optional cross-eye consistency blend of the ray-reconstructed result, off by
  default, VR with path tracing only; see [denoiser](denoiser.md).)

### An own PC-to-headset streaming channel

- Motive: true alpha passthrough. The streaming app's passthrough is chroma-key only (its OpenXR runtime enumerates only the
  opaque blend mode), another open-source streamer is chroma-key too, and the headset vendor's cable/Wi-Fi link passthrough was
  still an unstable beta when this was evaluated (autumn 2026).
- Why not: the hardware ceiling is the same as the streaming app's (NVENC, the headset's decoder, the router, the vendor's
  compositor). The streaming app leads on tuning: Wi-Fi loss recovery, automatic bitrate, latency and pacing. Its foveated
  streaming only helps headsets with eye tracking, which a Quest 3 does not have, so nothing is lost there. A home-made channel would
  likely be a regression. It would only be worth building if it measured equal to the app on motion-to-photon latency, pacing, SSIM at
  equal bitrate and loss recovery, and the riskiest parts (network and decode latency) should be measured first.
- Zero-regression alternative noted: do the matting before the chroma key.

### Rebuilding for newer runtime versions

- Virtual Desktop 1.34.24 / its OpenXR runtime 1.1: no rebuild needed. The OpenComposite layer asks for OpenXR API 1.0, and the
  runtime answers a 1.0 request with 1.0 behaviour while still reporting itself. The frame-generation layer takes its pacing from
  `predictedDisplayPeriod`, not a fixed 120 Hz; only a few hint strings assume 120 or 90 Hz.
- DLSS frame generation needs hardware-accelerated GPU scheduling (HAGS) turned on, and **our code does not check for it**. If
  frame generation suddenly cannot be created, check HAGS first. Forcing HAGS on or off as a "fix" for other symptoms was not
  pursued.
- A stall about every 15 minutes in streamed VR is the streaming app's own workaround for Quest 3 frame drops (its release notes
  say so), not this mod's pipeline. Its streamer settings are in `C:\ProgramData\Virtual Desktop\StreamerSettings.json`.

### A native replacement for VaM's hair renderer

An offline rewrite of the `HairOpt` geometry stage matched VaM's output triangle for triangle in 38 shader variants over 5 scenes (largest
relative error 1.2e-4). It was not integrated: the geometry stage is only 10-45% of the hair pass; the rest is rasterisation and
pixel shading; the best measured replacement ran at 0.91-1.16x, and a compute pre-pass was slower. Tessellation plus the geometry shader
is not the bottleneck. The branch was based on an old pipeline version. The hair gain that did arrive came from drawing strands
differently ([hair](hair.md)). Facts about the hair pipeline: [vam-facts](vam-facts.md).

### Render-budget plugins that were never built

Two ideas from a static study of VaM's renderer, not done: a passive audit plugin (cameras, lights, probes, GPU timestamps per camera
event) and a "render budget" plugin (turn off shadows for lights with `shadowStrength < 0.25`, cap point-light shadows, cast hair
shadows from the main light only, turn `generateDepthTexture` off). The first would be the right start if someone wants to attack
VaM's own 18-22 ms; see [frame-budget](frame-budget.md).

### The community CPU performance patch

Analysed offline (VaM 1.22.0.3; versions 12 and 13 beta of the patch exist). It recompiles whole classes (612 and 1120 methods) and its real
gain is about twelve native internal calls: morph application, skin mesh parts, normals, collider data, per-character scheduling. Its native
library calls `SetProcessAffinityMask` for the whole process, confining the render thread, the OpenComposite layer and every native plugin to
the cores it picked. Not installed: the same paths were reimplemented as [cpu](cpu.md) (bit-identical, no whole-class recompilation, masks
only on our own workers). Hooks of this mod remain usable under that patch, except that `DAZMorph.appliedValue` becomes a property there.

### Community rendering plugins (read-only analysis)

- A post-processing package with a "path tracing" global-illumination mode is not ray tracing: it is a visibility-bitmask
  screen-space GI (one slice direction per pixel, 16 steps) using the already-lit colour of this frame, reprojecting the previous GI by
  camera matrices only (no object motion), 0.95 temporal blend, added with `Blend One One` times a pseudo-albedo, and in its Quality
  mode drawing depth and normals again per eye. In VR the two eyes disagree, moving people smear, noise goes into DLSS, and its
  screen-space occlusion would stack on ours. Body-to-body bounce, the one thing worth borrowing, is done by the traced bounce.
- A community skin-SSS plugin redraws the skin into a mask target and blurs the whole lit colour (specular included) with four
  full-resolution ARGBFloat directional blurs: waxy and expensive. It showed that shaders compiled for Unity 2018.1 can be loaded
  as asset bundles in VaM.
- A skin micro-detail plugin swaps the skin to VaM's own tessellated mapped shader and sets detail maps, tessellation and Phong
  smoothing; it brings no new SSS or transmission algorithm, its detail maps are loaded without mips (distant sampling shimmers), and
  it re-applies its shader every frame unless told not to. Its compatibility with this skin pipeline was **not** verified.
- Scene survey of 753 scenes: Timeline in 416, Embody in 300, TittyMagic in 110, BootyMagic in 84, Naturalis in 79; skin-shading plugins
  are rare (micro-detail 8, SSS 5).

### Tried and lost in the shading work (details in the topic documents)

| Idea | Result and reason | Where |
|---|---|---|
| Extra nearest-hit ray per skin pixel to steady the reconstruction guides | cost too high; removed | [path-tracing](path-tracing.md) |
| Reconstruction response mask against dark spots on still skin | no effect; removed. The cause was heavy-tailed path-traced input (one shadow ray per pixel picking blocked lights); visibility-weighted light sampling and a sample clamp reduced it (in one test scene, frames with zero input on the person fell from 47% to 24%) | [path-tracing](path-tracing.md) |
| Unity's own motion vectors for props and animated meshes in VR | strong artefacts, hair worst, on the headset; rolled back to the bridge's own motion vectors (cause not found) | [motion-vectors](motion-vectors.md) |
| Corneal curvature for the eye highlight | highlight position unchanged, one eye's got smaller; removed | [path-tracing](path-tracing.md) |
| A 2 mm real thickness for eyelids | no visible effect; the eyelid shading term of the raster route was ported instead | [path-tracing](path-tracing.md) |
| A traversal-optimised acceleration structure | no gain: path tracing time is in shading; reverted | [path-tracing](path-tracing.md) |
| Sheer cloth's raster-style shadow and single alpha multiply in path tracing | ugly projected shadows; rolled back | [clothing](clothing.md) |
| A plugin that shifts all clothing Alpha Adjust | made parts the author hid with Alpha Adjust -1 appear (microphones etc.); uninstalled | [clothing](clothing.md) |
| Terminator-line correction and cast-shadows-off for the hard bench shadow | neither changed it; the shadow was an author-excluded object leaking in a test script | [path-tracing-authored-lighting](path-tracing-authored-lighting.md) |
| Giving the world-canvas menus a depth rectangle in path tracing | picture unchanged; reverted | [path-tracing](path-tracing.md) |
| "No ambient from the author means no ambient bounce for the background" | written but not installed: a more open path-traced picture is an accepted advantage; choose "background reduction = all" for dead black | [path-tracing-authored-lighting](path-tracing-authored-lighting.md) |

### Loading-time ideas that were refused or lost (the loaders are not in this repository)

Loading voice-over in the background or not preloading it: refused, because the feature would not be usable right after load.
A hard-disk cache of asset bundles: refused for write wear; they live in RAM. Deferring `UnloadUnusedAssets` on character switch (it never triggered in the test scene) and
raising `backgroundLoadingPriority` (no change in load time): dropped. Decoding two images at a time through the shared pool: slower (a limit of that
design's two-slot refill and synchronous batch waits, not a verdict on parallel decoding). Remaining large pieces: the plugins' own
`Init` and UI building (no single hot spot, about 0.9 s of JIT per load), the reference scan in `UnloadUnusedAssets` (about 1.65 s a load; could be
ported to a parallel native walk, risky), and engine-native work.

### Memory-layout ideas in the collector

Divisor 1, reserving every 4 GB boundary up front, and marking on CpuBoost's workers: see [gc](gc.md).

## Open

### Rare Mono heap corruption during startup (unsolved, parked)

- Symptom: about 6-7% of VaM starts crash early, 20-60 s after launch, usually right after the log line announcing frame generation
  (only a time marker) and before or around VR input initialisation. Inside Mono: `mono_add_process_object` on an object whose
  vtable is 0; a `mono_gc_out_of_memory` on an ordinary allocation; an assertion in `mono_array_element_size` during `Instantiate`; an
  access violation inside `UnityPlayer` during `Instantiate`. All say the managed heap or Mono's metadata is already damaged.
  Reports are written to `%TEMP%\MeshedVR\VaM\Crashes\`. It was first noticed within a window of about eleven hours of VR-layer changes (frame generation
  layer versions 8-10, OpenComposite versions 11-15, menu 4-6, bridge 2.2-2.4); evidence before the window is weak. Later tests
  hit the same signature now and then; a retry passes.
- Ruled out: the hair solver (crashes without it), the asynchronous post-processing, the hair budget's rate patch, a GPU reset (timing
  does not fit), the denoiser and super resolution (not yet running when it crashes), session operations (four rounds of 10 scene
  loads, 40 menu toggles, 88 frame-generation toggles, 80 render-scale changes: 0 crashes), and an audit of the bridge's unsafe
  writes, the menu pause, the frame multiplier query and the OpenComposite bone input.
- Hypothesis, unconfirmed: the 4 GB block-header defect of the collector ([gc](gc.md)): a header whose address lies within 0xfff bytes
  after a 4 GB boundary is misread, and the second outcome (live objects left unmarked and freed) looks like these crashes
  (vtable 0, bad array element size, out-of-memory). gc-tuning moves mono's new regions off the boundaries; blocks that exist before the
  plugin starts are not protected. If these crashes disappear after it is installed, that was it.
- How to continue: repeat starts 20-30 times with a full-memory dump on the first access violation (a dump helper loaded only during
  tests so installed files are untouched), look in the dump for the referrers of the damaged object, or bisect with older versions of
  the frame-generation layer and OpenComposite. These crash tools are not in this repository; the simulated-headset launcher in
  `tools/lab/simxr` can repeat starts. Test-agent pitfalls found: a delayed `wait_frames` reply carries no request id, so the
  client never receives it (sleep and poll `status` instead); kill helper processes by PID, not by matching a command line (that kills your own shell).
- Treat it as a known flake: when a test run hits this signature, retry once before blaming the change just made.

### GPU hang (driver reset) when the menu toggles the render size (unsolved)

- Symptom: a crash in mid-session that is a **GPU hang of about 2 s reset by the driver** (the system log shows the vendor driver's
  "Error occurred on GPUID" followed by a TDR; the OpenXR call fails with `887a0005`; the 11on12 layer reports device removed and the
  process ends). Two occurrences in five days of logs, both at the moment VaM's menu opened or closed and the render size switched between 100%
  and 50% (the layer logs the eye size change, for example 3057x3248 <-> 1528x1624), direction menu-open 100% native -> close back to 50%
  with DLSS, in the first one or two frames. Frame generation was paused at that time (it resumes 0.6 s after the menu closes) and the
  denoiser was off in one of the two, so neither is the trigger.
- Known: in one case, 1.5 s before the hang the GPU was at 100% load with power rising 470 -> 517 W and memory bandwidth falling 20% -> 0,
  as if one GPU task kept computing; it is not paging. Process video memory 24 GB (limit 31.4 GB), steady at 19-23 GB all night: not memory
  exhaustion, not a leak. Not polling of settings (that only cost CPU).
- Why it can recur: every settings change in the VR menu needs VaM's menu open; with `[Performance] DLSSMenuNative = 1` (default, menu
  "VaM menu -> no upscaling or interpolation while the menu is open") each opening swaps Unity's eye textures from 50% to 100% and
  back: about 3 GB of render targets allocated and freed per open/close. At least 3000 such switches over those days against two hangs:
  probabilistic.
- Checked and safe on resize: the OpenComposite denoiser (swap back to the old size, drain before release), DLSS super resolution
  (`ReleaseFeature` drains first), OpenComposite's own shaders (all loops bounded), the frame-generation pause (waits for presenter and
  compositor idle), the ray-tracing host (deferred release), the hair rasteriser (bounds inside the drawable area).
- Not known: which GPU task runs away. Next step: enable D3D12 DRED (before the 11on12 proxy creates its device) or Nsight Aftermath
  and capture a hang.
- Workaround: `DLSSMenuNative = 0` (menu text is then upscaled and interpolated).
- Separate: almost every normal VaM exit crashes after "OpenVR Shutdown" (BEX64 `c0000005` in `ntdll`) and leaves a crash dump of about
  110 MB each time in the user's local crash-dump folder. Unrelated to the mid-session hang. When looking at a crash first see whether
  the log ends in "OpenVR Shutdown" (exit crash) before matching system event times to the driver reset.
- A different, **fixed** crash (desktop mode, after changing the super-resolution input size): the post-processing UI camera, copied
  from the main camera, still pointed at the input render texture the bridge had just destroyed, and Unity's render thread dereferenced
  it. Fixed by re-pointing every camera that shares the old target before release (five consecutive size changes passed). It did not
  show a GPU TDR, so it does not close the hang above. See [d3d11on12](d3d11on12.md) and [frame-path](frame-path.md).

### Other open items stated in the topic documents

- About 2.3-2.6 MB/s of managed allocation remains in the skin plugin's scene record, and an unexplained 2.2 MB/s with path tracing
  off ([gc](gc.md)).
- A family of "`_AlphaAdjust` float" warnings, about 46 per scene load, comes from a source other than the material-property reader
  that was fixed ([vam-facts](vam-facts.md)); not traced.
- The CpuBoost IL fingerprints and the collector's `mono.dll` byte checks cover VaM 1.22.0.3 only; a newer VaM build turns those
  paths off until they are re-derived ([cpu](cpu.md), [gc](gc.md), [adapt-vam-version](../agent/adapt-vam-version.md)).
- Most VR-specific changes were verified in the simulated headset, not in a real one; each topic document says which. A person in a
  headset is the only check for comfort-type judgements (cross-eye consistency, motion artefacts, menu behaviour).

## Verify

Nothing here has an automated test except as noted. The startup crash and the GPU hang can only be investigated with the game running:
repeat starts (startup crash) or open/close the menu repeatedly with the render size switching (hang), then read the game log's last
lines and the Windows system log for the driver reset event. `tools/lab/simxr` drives a simulated headset for repetition; the real
trigger conditions (real headset, streaming app) were never reproduced there.
