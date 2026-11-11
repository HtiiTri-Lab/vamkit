# Frame budget: cadence, where the time goes, what was gained

The mod's target is not "as many frames as possible". With frame generation the game renders "real" frames and the
generator fills the slots between them, so a real frame must be finished on a fixed beat. This document gives the rule,
the cost of each stage on each lighting route, every optimisation with its measured gain, how a heavy character costs,
how to read the pacing logs, and what the simulated headset used for testing does not include. It also owns the
description of the development machine; every number elsewhere says "the development machine" and means this.

## The development machine and headset

| Part | What the notes record |
|---|---|
| GPU | an NVIDIA RTX 5090-class card (170 streaming multiprocessors); about 31.4 GB of graphics memory budget as the driver reported it; hardware GPU scheduling on (frame generation needs it) |
| CPU | a 16-core AMD Ryzen X3D: two 8-core chiplets, both with stacked cache. VaM's own threads end up on the best four cores of chiplet 0; the OS parks most of the rest, and the mod's workers are pinned to chiplet 1 |
| Memory | 128 GB |
| OS | Windows 11 |
| Headset | Meta Quest 3 streamed by Virtual Desktop (its VDXR OpenXR runtime). The runtime's recommended eye size, taken as `OutputScale` 100, is 3072x3264 per eye |
| Headset refresh | 144 Hz with 4x frame generation (36 real frames per second) in the later record; earlier measurements were taken at 120 Hz. Always read the cadence from the run's flight log (the display period), never assume |
| Desktop mode | a 3840x2160 monitor; tests ran frame generation at 2x-4x against a 160 Hz display |
| DLSS runtimes | 310.9.16 for SR, RR, NR and frame generation |

Not recorded: GPU driver version, memory speed, the monitor model, Virtual Desktop's bitrate and codec in normal use
(a side note: an 8-bit H.264 stream showed fixed blocks in dark areas on the headset; a 10-bit codec chosen in the
headset's own Virtual Desktop settings is the owner's side of it), Wi-Fi hardware. Everything measured below was
measured on this machine; transfer ratios, not milliseconds, to another GPU.

## The cadence rule

Frame generation fills a fixed number of slots per real frame (2x, 3x or 4x; the OFXR layer inserts at most three).
Each slot is one headset refresh. So the real-frame budget is a step function of refresh and multiplier:

| Refresh | 2x | 3x | 4x |
|---|---|---|---|
| 120 Hz | 16.7 ms (60 real fps) | 25.0 ms (40) | 33.3 ms (30) |
| 144 Hz | 13.9 ms (72) | 20.8 ms (48) | 27.8 ms (36) |

A real frame that takes longer than its n slots occupies n+1. With 4x only three frames are generated, so the extra
slot is empty and the headset shows a repeated frame or a gap: the counter says "high" and the picture is jerky.
Consequences for tuning:
- The target is "finish inside the slot", not "faster". Spare time is spent on quality (render scale, NR, path-tracing
  bounces), shortfall is paid by dropping one rung (input scale, multiplier, background savings).
- A gap of four or more slots, or a 12-38 ms hitch, is one real frame missing its beat. Near the limit any frame can do
  it; no separate cause needs to be hunted. Give the real frame headroom to make hitches rarer.
- Frame-rate counters are not smoothness: a run can fall short of the refresh yet be smooth, or reach it and hitch.
  Judge by slot counts from the flight log, and for localised deformation judge the actual trajectory, not the average.
- "Real fps" in the OpenComposite log is averaged over 10 s windows that include menu pauses; it is not play-time fps.
- Extra cost that does not lower normal-play (menus closed) frame rate is acceptable; savings from a stage go to frame
  rate, not to a denser setting on the same preset.
- Desktop mode follows the same arithmetic against the monitor refresh (for example 3x at 160 Hz needs 53.3 real fps).

## Where the time goes

Order of a real frame: Unity scripts and physics on the CPU -> Unity's raster pass (opaque, shadow maps, skin and hair
programs) -> on the path-tracing route the path tracer (per eye) and RR, on the raster route SR (per eye) -> composite and
tone mapping -> NR -> NVSharpen -> hand-off -> DLSS frame generation on its own D3D12 queue
([frame-path.md](frame-path.md), [d3d11on12.md](d3d11on12.md)). The GPU is the limit in VR (98-100% busy); the CPU's busiest
two threads each use only 35-57% of a core.

GPU shares (simulated headset, development machine):
- The three NVIDIA networks (RR or SR, NR, frame generation) and the output-size passes are the bulk. Unity's raster is only
  1-2.4 ms, so a rewritten renderer or a full D3D12 port would gain at most 2-3 ms and was not pursued.
- Raster route, heavy two-character dance scene, when that route could still use RR at 50% input (not available any more;
  use as a magnitude): about 19-21 ms per real frame with generation off: RR stage about 6, eye submission about 3 per
  eye (mostly the reduced NR), Unity opaque about 2, work before the camera about 3.5 (of it: script `LateUpdate` 1.63,
  frame-start send 1.09, a post-camera call .57); GPU idle only .65-.69 ms per frame. With K/L/M instead of RR, the SR
  per eye is 1.1-3 ms at 3072x3264 output ([upscaling.md](upscaling.md)).
- Path-tracing route, same scene, RR input 33%: path tracing replaces Unity's lit raster, shadow maps and the texture-space
  atlas (about 3.7 ms) and costs about 5 ms itself; that is why PT looks cheaper than the raster route at equal settings:
  the measurement bases differed (33% vs 50%). Different sessions' absolute fps differ by up to 10; compare only within
  one script and session.
- Desktop, path tracing, RR input 75%, 3x, heavy scene, generation off, GPU always full, 51.8 fps: main camera 11.4 ms (path
  tracing 7.2 + RR 3.9), desktop output (NR and the rest) 3.2, work before the camera 3.0, `MainWindowCamera` 1.24, UI .13.
  Other desktop items measured at under .1 ms each (an unnecessary clip under RR .017, two hand-offs about .08, an extra
  presenter copy about .05, a texture borrow/return about .06-.09) were left alone.
- Path-tracing stage per eye (simulated headset, VR, after the analytic hair change of item 6): a close-up on one face at
  RR input 50%: 4.54 / 3.78 ms (two measurements of the same change; the notes do not say whether they are the two
  eyes); a heavy scene at RR input 33%: 1.51 / 1.29 ms.
  The path tracer's time is mostly per-vertex shading and the light loop, not BVH traversal (traversal-optimised build
  flags did not help: 46.27 -> 46.11 / 45.0 fps).
- Frame generation, per real frame beyond generation off (quality model, simulated headset): 2x +2.8, 3x +4.4, 4x +5.7 ms;
  offline both eyes at 4x 4.5 ms. The lite model C is cheaper (+.9 / +2.1 / +2.5) and was rejected on quality.
- NR (new model, packed): about 1.5 ms both eyes before the upscaler; after RR in reduced mode 4.4-4.6 ms for both eyes at
  the time it was measured ([denoiser.md](denoiser.md)).

CPU: the character path (skin, morphs, normals) was reworked natively, 4.1 -> 2.2 ms on the character thread
([cpu.md](cpu.md)). Physics runs at 90 Hz with at most three steps per real frame; freezing an animation does not save
the cost since physics and skinning still run. A periodic stall is the Mono garbage collector marking the whole managed
heap (0.66-0.95 s per full collection on a heavy scene before the parallel marker; 0.16 s after; [gc.md](gc.md)).

## Optimisations made, with measured gain

All on the development machine, simulated headset unless stated, a heavy two-character dance scene with the dance playing
(a static scene gives inflated numbers), RR input 33%, `MiniCamera` off, real frames per second with generation off ("off")
and with 4x ("4x", capped at 36 by a 144 Hz refresh).

1. **Path-tracing material textures resident in D3D12.** Each eye used to borrow about 83 material textures from D3D11 one
   at a time (`UnwrapUnderlyingResource` / `ReturnUnderlyingResource`); 11on12 queues a synchronisation after each, giving
   about 1800 sync events and 2.5 ms of render thread per frame and idle GPU before each eye's trace. Removing only the fences
   did not help. Textures D3D11 cannot render into are copied once to a D3D12 copy; seven write methods of the immediate
   context are hooked so a write to a copied texture re-copies it. Result: off 41.99 -> 47.57 (borrow once per frame) -> 51.80
   (resident); 4x 32.49 -> 35.87 -> 36.00; GPU idle per frame 3.09 -> 2.14 -> .88 ms; cost +1286 MiB of graphics memory
   (about 14.3 GB used of 31.4). Skipped when less than 2 GB of budget would remain.
2. **RR textures built in D3D12 and wrapped** instead of borrowed per frame. Raster route (when it used RR): sync events 125-128 -> 36.5
   per frame, idle .65-.69 -> .36 ms, RR stage 6.2-6.6 -> 5.78 ms, 54.55 fps vs 48.3-53.1 earlier. Path tracing: events 137 -> 40,
   idle .88 -> .37 ms, RR stage 9.2-9.7 -> 7.9 ms, off 55.59 vs 51.80.
3. **Four whole-image copies at output size removed** on the raster route (byte-identical output): tone mapping .57 -> .38 ms,
   eye submission about -.1 ms per eye, about -.35 ms per real frame in total (fps within the session's noise).
4. **NR packing and scaling**: NR after RR for both eyes in one evaluation, saves .6-.78 ms per real frame; NR on a reduced
   image 6.0 -> 3.3 ms (DLAA input); new NR model -45% per eye; NR packed before the upscaler 15.01 -> 13.70 ms per frame.
5. **Cameras never seen turned off.** `MiniCamera` (455x256, draws a display plane of a window-camera atom): path tracing 4x
   30.56 -> 35.07, off 36.83 -> 44.49, GPU per frame 27.10 -> 22.44 ms. `MainWindowCamera` in desktop mode (it renders the
   whole scene at 4K and is then covered by the desktop output): path tracing 49.8 / 46.5 / 53.2 fps, raster route 71.5 / 63.4 /
   70.7 (off / on / off), GPU -1.4-2.7 / -1.7 ms. Only effective in scenes that use these cameras.
6. **Path-tracing hair: analytic multiple scattering instead of tracing between strands**: path stage per eye 2.04 / 1.73 ->
   1.51 / 1.29 ms, off 57.1-58.0 -> 61.82 fps, 4x 36.00; with hair filling the view (face scene, RR 50%) 6.5-6.9 -> 4.5 ms.
7. **Path-tracing background savings** (`[RayTracing] PathBackground`, see [path-tracing.md](path-tracing.md)): background does
   no diffuse bounce and rough reflections read the environment cube. VR with "All": 54.71 / 51.62 fps vs 47.30 (off) and
   47.02 ("Far") (one-character scene, RR 35%, 3 bounces, off); path stage 2.7-2.9 vs 3.9 / 3.8 ms. Desktop, an indoor
   close-up scene: path time 10.46 (Far), 10.45 (Off), 3.71 ms (All).
8. **Hair, cloth and wrap solvers** ([hair.md](hair.md), [clothing.md](clothing.md)): a heavy character's total, +7 fps with
   generation off at RR 33%: cloth physics in multiple thread groups (GPU 2.78 -> 1.84 ms per frame), wrap-cloth batching (about
   -.2-.25 ms), hair solved once per frame even when the body steps twice (solves 5.9 -> 1.3 per frame; GPU 18.83-19.67 ->
   17.87-18.23 ms).
9. **Opacity micromaps for path tracing**: small; a cloth-heavy character -.19 ms (about 4%) path time per frame, fps inside noise.
10. **Collector tuning**: parallel mark (16 threads) 0.66-0.72 -> 0.16 s per collection; first-load collections 8-56 -> 3
    ([gc.md](gc.md)). Not a frame rate gain, a spike removal.

## What a heavy character costs

A character with 12 hair pieces, 12 simulated clothing pieces and 52 wrap-cloth pieces ran 10+ fps below a character with
less hair and clothing in the same scene with generation off. The extra is on the GPU and render thread: simulated cloth,
hair solving (about 2.97 vs 1.88 ms per frame) and wrap cloth, not the path tracer. Main-thread waits in `LateUpdate` are
the main thread waiting on the render thread; removing them only moves the wait to frame start, so read where the wait
went before concluding anything. Path-tracing hair geometry (strand refit/rebuild) averages 0.36 ms and splitting rebuilds
over frames cost more on average, so there is nothing to save there. Hair is costly at close range: every rendering
pass and every shadow-casting light redraws it.

## Session-to-session variation

- The same configuration differs by 2-3 fps between sessions (path tracing, heavy scene: 29.8-30.6 / 34.8-36.4 across three
  candidates), and different scripts by up to 10. Compare candidates inside one session by switching the setting live
  (settings are re-read within 250 ms), against a reference window next to it.
- fps cannot resolve a .3 ms change; use the per-stage GPU timers (below) with windows of at least 30-40 s.
- Anything else on the GPU counts. On the development machine, a desktop application rendering in software put the GPU at
  about 25% with VaM closed (the window manager's share of the 3D engine was 23-26%). Check the idle GPU load first.
- The first path-traced frame after a shader change waits about 26 s for the driver to compile pipelines (the second run
  was 0.6 s); that is not a hang.
- Scripts must unfreeze the animation explicitly: a freeze set for one scene can leak into the next and silently make the
  dance static, which made some early path-tracing numbers too high.

## Reading the pacing logs

- Flight log: `ofxr-bridge-flight-*.log` of the OFXR layer, enabled by `[diagnostics] logging_enabled=1` in the layer's
  `ofxr_bridge.ini` (the tray has a "Bridge flight recorder" item; the launcher writes `diagnostics=0` into the run's
  `tray.ini`). Bounded: the first records and the last ~90 s at the default 32 MB. Cost about .1-.2 ms CPU per frame. Fields: `seq
  ms tid phase op result dur_us a b c`. For `presenter_submission`, `c` is 2 = generated frame, 1 = real frame, 0 = repeated
  frame; `internal_wait_frame`'s `b` is the display period (read the refresh from it); `synthesis_gpu_span` is each pair's
  frame-generation time per eye. The `nvidia_gpu_stages` records are wrong on the DLSS-G path; ignore them.
- `tools/lab/simxr/flightlog.py LOG_DIR` prints per-cycle and per-frame duration distributions, release phase, late
  releases, GPU-gated waits, frames still in flight, and an estimate of what would be displayed.
- Frame generation is running where frames with `c = 2` exist. Count 4-slot and 5-slot cadences and empty slots to find
  missed beats.
- OpenComposite log (`opencomposite.log`): every 10 s `VAM GPU timing eye submission eye=N: ... avg ms: crop guides nr
  upscale denoise sharpen output = total (max ...)`, `VAM GPU timing RR F HDR upscaling, both eyes ...`, `VAM PT split
  eye=N samples=... path_ms=... rr_ms=...`, `VAM RR frames=...`. Stage times are elapsed GPU time on that queue and include
  whatever other queues ran meanwhile.
- GPU idle and synchronisation need event tracing (ETW) of VaM's own queues; the notes used it without administrator rights.
- Per-thread CPU: the census tools in `tools/lab/census/` (see [cpu.md](cpu.md)).
- Before trusting a 10 s sample, check the VaM log for "VaM menus open" and the compositor log for "native frames on".
  Menus can open by themselves 30-40 s into a simulated run.

## What the simulated headset does not include

The simulated headset replaces only the last link (the streaming runtime and the headset); VaM, the compositor, the frame
generation layer and the DLSS runtimes are the real ones. It does not model: video encoding, the Wi-Fi link, timewarp or
space warp, Virtual Desktop's own GPU use (about .7-1 ms per displayed frame, 3-4 ms per real frame at 4x), or the headset's
hidden-area stencil (bugs that depend on it show only on a real headset; an example is a black band on pan with frame
generation). On a real headset the same scene took about 30 ms per real frame against about 19.5 ms simulated (144 Hz, 4x,
model M at 70%); with Virtual Desktop streaming, the quality frame-generation model went from 2.1-2.6 to 4.4 ms per eye
(conjecture: the layer's high-priority queue and the streamer time-slice the GPU), and about 4.5 ms is unexplained. To
estimate headset headroom from a simulated run, add 3-4 ms and count the quality frame-generation model at about double.
The simulated display's own numbers (displayed fps, repeats, latency) depend on the stand-in's rhythm: they are printed,
never used as a pass criterion. Its default refresh is 120 Hz (environment `SIMXR_REFRESH`). Use only the pipeline's own
output: frame times, the frames handed to the runtime, screenshots. Tests keep the GPU queue limit at 1.

## Open

- On the real headset, a heavy scene's 3x and 4x both ran at only 90-115 frames/s; the simulated headset did not reproduce it.
  A controlled headset A/B (same position, menu closed, switching 3x/4x by script) is needed and needs the owner.
- Loading a character or scene with the menu closed dropped to 2-13 fps for 10-30 s on the headset; whose cost is unknown.
- After 100%-input at a menu or a model switch, one scene dropped to about 10 fps and did not recover. The best guess is
  paging when the scene's own use sits against the 31.4 GB budget (unproven; needs the graphics-memory log).
- Community frame-generation kernel optimisations (an optimised-kernel build for older architectures) are untested on
  this GPU, may be slower, and run through JIT; a static look at the shipped frame-generation library found 27 tensor-core
  network kernels compiled for an older architecture in FP16 only. Nothing was measured or adopted.

## Verify

Offline probes cover correctness, not frame time. Time stages with the timers above in the simulated headset
(`tools/lab/simxr/`), one change per run, live toggle against an adjacent reference window; a real-headset number needs
the owner ([../protocol/performance.md](../protocol/performance.md), [settings-and-menu.md](settings-and-menu.md)).
