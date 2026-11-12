# Frame generation (DLSS-G, FSR) in VR and on the desktop

The real frame rate of a path-traced or heavily shaded VaM scene is a fraction of what a headset or a monitor can
show. DLSS Frame Generation (DLSS-G) makes the in-between pictures from the last two real frames, depth, motion vectors and
camera, so a scene rendered at 30-40 real frames a second can fill a 120-160 Hz display with 2x, 3x or 4x the frames. What a player
sees is smoothness; what an engineer must keep in mind is that every real frame now has to fit in "multiplier" display periods, or the
display shows an empty slot and the scene stutters although the displayed frame count looks high.

In VR the generator runs inside an OpenXR API layer (OFXR, `components/render/ofxr-dlssg/`) that sits between OpenComposite
and the runtime. On the desktop there is no OpenXR runtime, so our own presenter inside the 11on12 proxy
(`components/render/d3d11-proxy/desktop_presenter.cpp`) owns the swap chain and compiles the same generator sources
(`ofxr-dlssg/src/d3d12/d3d12_dlssg.cpp`, `d3d12_fsrfg.cpp`). Where the picture and its guides come from:
[frame-path.md](frame-path.md), [motion-vectors.md](motion-vectors.md).

DLSS-G needs an RTX 40 (2x) or RTX 50 (up to 4x). AMD's FSR frame generation is the second generator: it runs on any
card, takes the same inputs, and makes one frame per pair (2x). It gives RTX 30 and older cards generated frames from
the engine's depth and motion for the first time, and a second choice on RTX 40 / 50. Which cards run what, and why
Intel's XeSS frame generation is not here: [upscalers-other-vendors.md](upscalers-other-vendors.md).

## How it works

### What the compositor publishes

`PublishFrameGeneration` (`dx11_vam_dlss.cpp`) runs once per eye and real frame, just before the compositor releases the image:

- per-eye depth (R32F, render size, the headset's hidden-area pixels set far) and motion vectors (RG16F, jitter removed, in render
  pixels, current to previous), the jitter offset, `frameTimeDeltaMs`, near and far plane, field of view and aspect from the projection,
  the view-to-clip and clip-to-view matrices and the clip-to-previous-clip matrix (converted from OpenGL to D3D conventions),
  a `reset` flag (camera jump, scene change, size change), and a serial;
- it publishes only if the render size is not larger than the output size (otherwise that pair goes to optical flow);
- it finds the consumer once: VR looks for the layer module `XR_APILAYER_XRFrameBridge_diagnostic.dll` and the exports
  `OFXR_GetProviderApiVersionV3` / `OFXR_PublishD3D11GuidesV3`; on the desktop the proxy exports `V4`. The version export returns 3 (or 4) only when
  `OFXR_DLSSG=1`; otherwise nothing is published and the layer keeps optical flow only. `DLSSFGEyes` (bit 0 left, bit 1 right; fixed 3 by the
  launcher) is a diagnostic: an eye left out gets optical flow.

### The generator

`D3D12DlssgFrameGenerator` (`d3d12_dlssg.cpp`), NGX D3D12 (runtime 310.x), one instance per eye as NVIDIA documents for stereo, on the layer's own
HIGH-priority D3D12 queue (VR) or the presenter's HIGH queue (desktop). Rules that follow from DLSS-G keeping its own previous frame:

- every real frame is fed exactly once and in order; a gap resets it, and a reset pair carries a copy of B that must be
  shown with B's pose;
- a pair goes through DLSS-G only if the capture carries depth, motion vectors and camera; otherwise through optical flow, frame by frame;
- eight consecutive evaluation failures make DLSS-G give up for the session (`kFailureLimit`); the desktop presenter instead retries once a second
  (`retryAt`);
- NGX D3D12 allows one setup per process, so SR's and DLSS-G's runtime folders must both be listed before the first feature is created. The compositor
  calls `OFXR_PrepareFrameGenerationV3` (`VamDlss::PrepareFrameGenerationNgx`) before the NR pre-pass starts its own NGX; otherwise NGX D3D12 came up
  between the two eyes' SR features and DLSS-G reported itself unavailable.
- **In place.** DLSS-G reads B where the layer captured it and writes its frames straight into the layer's staging images when those allow it
  (`readable_directly`, `writable_directly`); copying instead costs four 40 MB eye copies per 4x pair at 3072 x 3264. `OFXR_DLSSG_COPIES=1` restores
  the copies for comparison. A generator log line says which mode it is in.
- The layer interpolates the head pose between A and B for each generated frame.

### Two generators behind one interface

`D3D12GuidedFrameGenerator` (`ofxr-dlssg/include/xrfg/d3d12_frame_generator.hpp`) is the contract DLSS-G set, as an
interface: `can_generate`, `record` (B, B's guides, the destinations), `invalidate_history`, `usable`, `max_outputs`.
`D3D12DlssgFrameGenerator` implements it; so does `D3D12FsrFrameGenerator` (`d3d12_fsrfg.cpp`):

- It talks to AMD's FSR API (`amd_fidelityfx_loader_dx12.dll` from `VAM_FSR_RUNTIME_PATH`, which loads the frame
  generation DLL beside it); nothing of AMD's is linked. `fsrfg_available` asks the API which frame generation
  versions it offers for the device (3.1.6 on an NVIDIA card).
- **No FSR swap chain.** AMD's own integration wraps the game's swap chain and presents by itself. Here the context is
  created alone and dispatched by hand, per frame in AMD's order: `ffxConfigure` with
  `FFX_FRAMEGENERATION_FLAG_NO_SWAPCHAIN_CONTEXT_NOTIFY` and no callbacks, the prepare dispatch
  (`ffxDispatchDescFrameGenerationPrepareV2`: depth, motion, jitter, camera planes, field of view, position and axes),
  the generation dispatch (`ffxDispatchDescFrameGeneration`: B and one output texture). AMD calls this route possible
  but discouraged; it is what a layer without a swap chain and a presenter with its own pacing need.
- FSR keeps the frame before B itself, like DLSS-G. Its own frame counter rises by one per dispatch; a gap in our
  serials is passed as `reset`, and a reset pair gets a copy of B (shown with B's pose), as DLSS-G's does.
- It works on its own typed copies: B is copied into an 8-bit UNORM image and the generated frame is copied out of a
  UAV image (two copies per pair, about 0.03 ms each for an eye image). The caller's images may be typeless, sRGB-typed
  or not writable as a UAV; DLSS-G's in-place path is not used here.
- The FSR context is made with the first pair (the guide size is known then), which takes about 0.6 s once.

`make_guided_frame_generator` makes the generator `[Performance] FrameGenModel` names (`DLSS`, the default, or `FSR`)
when it can run, else the other: a card without DLSS-G gets FSR without anyone choosing it. The choice is followed
while the game runs:

- VR: `follow_generator_model` in `d3d12_frame_synthesizer.cpp` looks at every pair, waits for the synthesis queue and
  makes the other generator in place of the old one. While FSR is the generator the layer runs 2X whatever
  `[ofxr] frame_multiplier` says (`guided_frames_most`, applied in `apply_live_frame_multiplier`), and returns to the
  multiplier asked for when DLSS-G is chosen again.
- Desktop: `Settings()` in the presenter drops the generator when the choice changes; the next one is made after a
  frame is shown, like the first. The frames shown per real frame are the smaller of the multiplier and
  `max_outputs() + 1` (FSR: 2; DLSS-G: what the card's does).
- `OFXR_GetFrameGeneratorsV1` (layer and presenter) tells the menu what can run: bit 0 DLSS-G, bit 1 FSR, bit 4 while
  FSR generates. The menu shows a model row only when both can, and replaces the multiplier row by a line "2x" while
  FSR generates.

**Without NVIDIA optical flow** (no `nvofapi64.dll` on a Radeon or Arc card, an NVIDIA card without the hardware) the
layer used not to start synthesis at all. Now, when a guided generator is asked for (`OFXR_DLSSG=1`) and
`create_nvidia_resources` fails, `initialize` in `d3d12_frame_synthesizer.cpp` sets `nvidia_flow_missing`, releases
what the optical-flow set-up had made and goes on: the work slots keep their NVIDIA shape with nothing behind the flow
descriptors, and the session runs on the guided generator alone (`NVIDIA optical flow is not available here ...: this
swapchain runs on FSR alone` in dlssg.log). A pair the generator cannot take goes to the game-motion route where its
guides allow, and otherwise `record_hold_pair` copies the previous real frame into every in-between image (counted in
the log). If neither generator can be made either, `initialize` fails as before and the frames pass through.
`VAMKIT_NO_NVOF=1` makes the optical-flow set-up fail on a card that has it; with `VAMKIT_NO_DLSS=1` it gives the path
of a Radeon or Arc card in VR. The desktop presenter never used optical flow.

**The launcher does not choose for the machine.** What was chosen goes into the run as it is; the layer and the
presenter try it and only then put the other in its place. (A first version replaced DLSS by FSR before the launch,
from the card's name and from whether `nvngx_dlssg.dll` lay in the kit, and saved the replacement: an RTX 50 with the
file missing got FSR at 2x and no word about it. Removed.) What the launcher does instead, `runtimes.py`: an NVIDIA
library the kit lacks (`nvngx_dlss.dll`, `nvngx_dlssd.dll`, `nvngx_dlssg.dll`) is taken from the graphics driver's
model store (`%ProgramData%/NVIDIA/NGX/models/<dlss|dlssd|dlssg>/versions/<major<<16|minor<<8|patch>/files/*.bin`,
the release the mod was run against first, else the newest), its signature checked, copied to
`<VaM>/vamkit/cache/ngx/` under its DLL name and passed to the game; what is found nowhere is printed, written to
`runtimes.txt` in the run's log folder and shown in the panel with what is lost and how to get it (`launch_notes`).

**FSR's GPU time per pair** is measured like DLSS-G's (two timestamp queries around what `record` puts on the list,
B's copy and the generated frame's copy included) and reported in the line every 1800 pairs.

### Multipliers and cadence

`[ofxr] frame_multiplier` 2, 3 or 4 (menu, live). The layer tells the game, through `xrWaitFrame`, to run at the display rate divided by the
multiplier, so a 120 Hz headset gives the game 60, 40 or 30 real frames a second. A change drains the presenter queue, re-phases once, and the
game sees one frame without synthetics and its period step by one display period. 3x and 4x run the "deep pipeline" (`[ofxr] deep_pipeline=1`,
one display period of extra depth bought with about one frame of latency); the shallow pipeline could not receive a frame from VaM in time for 3x. A 4x
session is created with staging for four frames whatever the launch multiplier (`OFXR_FRAME_MULTIPLIER=4` at launch; `frames_most`), so
2x/3x/4x switch without remaking swap chains. The layer generates at most 3 frames per real frame; the NGX feature itself reports 5 (6x): see "Not built here".

Budget per real frame: display period x multiplier.

| Display | 2x | 3x | 4x |
|---|---|---|---|
| 120 Hz | 16.7 ms | 25 ms | 33.3 ms |
| 144 Hz | 13.9 ms | 20.8 ms | 27.8 ms |
| 160 Hz (desktop monitor) | 12.5 ms | 18.75 ms | 25 ms |

A real frame that runs past n periods takes n+1: the slot is empty and one picture is held longer ("high frame count but stutter").
A 12-38 ms hitch in a log is therefore one real frame that overran its budget. Reading logs: a frame with `c=2` in the layer's flight log
(`presenter_submission`) is a generated frame, 1 a real one, 0 a repeat; the flight log exists only when diagnostics are enabled (`[diagnostics] logging_enabled`;
`diagnostics` in `tray.ini`).

### Quality or lite mode

`[Performance] DLSSGLite` (0/1). 1 sets NGX's create-time flag `DLSSG.LiteCandidateBlendingEnabled` (NVIDIA's lighter "Preset C": a candidate blend replaces
the second network); it is not in the 310.7 SDK headers, 310.9.16's runtime reads it, older runtimes ignore it. Followed live (`lite_wanted`): on a change
the layer waits for an empty queue and remakes each eye's feature (about 0.2 s per eye in the simulated headset), and
a switch back to quality remakes it with fresh parameters so no stale key stays. Default 0. The mode was judged too low in quality for normal use; it is a
cost lever for when the quality model cannot make the budget (below).

### GPU scheduling priority

`[Performance] GpuPriority` (`default` / `high` / `realtime` / `cycle`), read when the file changes (`d3d12_dlssg.cpp`, `GpuPriorityControl`). Windows time-slices
the GPU between processes by scheduling class (`D3DKMTSetProcessSchedulingPriorityClass`). VaM runs at NORMAL; the streaming app of a wireless headset runs elevated and
calls the same API, which is the leading explanation for DLSS-G taking 4.4 ms per eye on the headset against 2.1-2.6 ms in the simulated headset. `high` = class 4, `realtime` = 5
(an ordinary process may set its own class; checked on the development machine), `cycle` = 20 s each of as-found, high and realtime, logging pairs per second and DLSS-G
GPU time per eye for each phase so one session compares them. `default` leaves the class and logs it. This has not been run on a headset; see Open.

### Pausing

The VR menu's frame-generation switch (`[Performance] FrameGeneration`) and VaM's own menus (while `DLSSMenuNative` is on) set the OFXR tray's pause event
(`Local\OFXRBridgePause-<n>`, number taken from the layer ini's `control_event`, polled in `components/render/vr-menu/VaMVRMenu.FrameGen.cs`). The layer then passes frames through
and paces VaM at the headset's own rate. The menu clears the event only after the menus have stayed closed for 0.6 s: resuming in the very frame the
button closed them made the layer restart its presenter while the button was still held, and VaM read a second press (13 of 19 closes bounced back open).

### The optical-flow fallback

OFXR's original backend: NVIDIA optical flow (`[ofxr] backend=nvidia`, `nvidia_preset=medium`, `nvidia_input_scale=50` percent per axis, `nvidia_bidirectional=0`), used
for any pair without DLSS-G guides (an eye left out, a render larger than the output, DLSS-G unavailable). The launcher writes these into `tray.ini` (`flow_ini` in
`LaunchPanel.py`). The mod does not offer optical flow as a mode of its own; it is a safety net, and one the layer can now do without (above).

### The hidden-area mask and black bars on head translation

A headset's hidden area (outside the lenses) is drawn by Unity as black at the near plane (reversed Z = 1). It is fixed to the display. With the camera's own
motion vectors, a 1 cm near plane gives it hundreds of pixels of apparent parallax on a few millimetres of head translation, and generated frames drew it across the
view as black bars perpendicular to the motion. The guide shader (`guideShader` in `dx11_vam_dlss.cpp`) treats depth >= 0.9999 as hidden: motion 0 and depth set to the far value
(the flag `pad < 2` excludes the path-tracing pre-pass, which keeps the raw value until the composite has consumed it). The simulated headset
returns an empty stencil from `ovr_GetFovStencil` and cannot show the problem; this fix was only compiled and checked offline in the shader, and the notes do not record a
confirmation in a real headset. Any "only on the headset" artefact: first ask what the stand-in lacks (hidden-area mesh, video encoder, runtime time warp).

### Double images when turning the head

Cause: the hand-over copy and the next pair's DLSS-G wrote the same staging slot with no GPU synchronisation. Fix: `synchronize_producer_queue`
(`d3d12_frame_synthesizer.cpp`) in the hand-over; confirmed in a real headset. Motion vectors had been suspected first and were proved right
(per-frame MV comparison with `tests/mv_check.py`); reopening that needs new evidence. The stock layer's 3x mode used a shallow pipeline that could
not wait for VaM's frames; 3x and 4x therefore take the deep pipeline.

### The desktop presenter

`desktop_presenter.cpp`. Unity draws to wrapped D3D12 surfaces that the proxy hands out as the swap chain's buffers; the presenter owns a real flip-model chain (3 buffers, waitable,
`SetMaximumFrameLatency(1)`) on its own HIGH queue, so DLSS-G, the copies into the swap chain and the waits of `Present` never sit on Unity's queue.

- **One real frame** (`Group`): wait for Unity's ready fence; copy the frame into a private image ("shown"); record DLSS-G from the frame and the guides
  published by the compositor (`OFXR_PublishD3D11GuidesV4`); submit, so that Unity renders the next frame while this one is shown; then show the generated
  frames followed by the real one.
- **Unity's frames go through a ring of three inputs** (frame, depth, motion and their description). Unity writes an input that is neither taken by the
  presenter nor still being read by its queue, which it checks on the CPU (`Claim`); the presenter has at most one input in flight, so one is always free.
  Neither queue waits on the GPU for the other. Up to v2 there was one input, and Unity's queue waited on the GPU for the presenter's copy of the last
  frame. That copy sits on the presenter's queue behind the copies into the swap chain's buffers, and the GPU holds a write to a swap-chain buffer until the
  presentation engine has let go of it (Microsoft's flip-model guidance: the first GPU write to the back buffer is where the GPU waits for it). So a stall
  anywhere in presentation stopped Unity's queue, then Unity's render thread inside 11on12 (which waits for that queue to recycle its memory), then the main
  thread. The one report of a desktop hang (another machine, RTX 5090 with driver 621.14, Windows 10 22H2) ended in that state: GPU nearly idle, the
  presenter in a fence wait, Unity's thread in 11on12. With v3 the window kept answering and its logs named the stop: Unity's own queue, not
  presentation. Unity's ready fence never advanced past the first frame the compositor handed to DLSS SR, and the presenter's queue was empty. The cause
  was the compositor naming 11on12's own queue to `UnwrapUnderlyingResource`. On that machine's older `d3d11on12.dll` this puts a `Wait` ahead of its own
  `Signal` on that queue ([d3d11on12.md](d3d11on12.md), "Unwrap / return"; reproduced here with Windows 11's first 24H2 build of the DLL beside `VaM.exe`).
  The compositor and the path tracer now name 11on12's queue only after the proxy's probe has shown that the DLL does
  not stop on it, and a queue that runs no work otherwise.
- **Waits are bounded or logged.** Unity's thread still waits until the presenter has taken its last frame (the pacing), but for at most a second
  (`kTakeMs`); then it writes over the frame not yet taken and stops waiting until the presenter takes one again, so the window keeps taking messages.
  Every CPU wait on a fence goes through an event; one that lasts more than 2 s is logged in `dlssg.log` (`desktop wait: ...`) with the fence, the value
  waited for, how far it got and the presenter's current step, and so are latency-object timeouts (`desktop presenter: the swap chain's latency object
  stayed unsignaled ...`), a `Present` of 250 ms or more, and a frame not taken for a second. Those lines say which side stopped.
- **Even pacing.** The real-frame period is estimated from frames the presenter had to wait for (a smoothed 0.75 / 0.25 average; shrinks when the
  presenter is the slower side so it never holds Unity back); each picture is shown `period / frames` apart, never closer than one refresh, never further than 50 ms;
  there are no repeat frames. The first version ran everything on Unity's queue, drained it after each step and inserted repeats, which
  quantised the real frame rate to 160/k (3x came out at exactly 40.0 and 26.5 on a 160 Hz display) and let the vsync waits of the presents stall Unity's GPU work.
- **Same-size `ResizeBuffers` keeps everything.** Unity calls it with the buffers' own size when its window is activated again. The first version
  tore buffers and DLSS-G down (0.15-0.4 s to rebuild, with no image on screen for that time). Now a same-size or 0 x 0 call returns at once; the log says
  `same size, buffers and DLSS-G kept`.
- **The "not responding" ghost window.** The flashes to the desktop were Windows' desktop window manager replacing VaM's window with a `Ghost` window for about 0.1 s.
  Windows judges a window "not responding" when its thread has taken no real message for 5 seconds. Unity peeks the message queue every frame, but only a
  peek that returns a message counts, and with no input there are none. A flip-model window has nothing to show in the ghost, so the user sees the desktop;
  when VaM is back the swap chain gets a same-size `ResizeBuffers`. Fix, both in the presenter: a timer thread posts `WM_NULL` to the game window once a second
  (Unity's per-frame peek takes it) and `DisableProcessWindowsGhosting()` is called, so even a main thread blocked for a long scene load gets no ghost. `CheckHung`
  logs every judgement ("judged NOT RESPONDING" / "responding again") with the time since the last input and whether a sent `WM_NULL` was answered.
- **Settings.** `Settings()` re-reads every 250 ms by file stamp: `desktop-presenter.ini` `[ofxr] frame_multiplier` (2-4), `[overlay] position`; the run's `performance.ini`
  `[Performance] FrameGeneration`, `DLSSGLite`. The presenter thread is pinned with `CB_PinThread(1)` when CpuBoost is present.
- The proxy also exports `OFXR_GetFrameMultiplierV1`, `OFXR_PrepareFrameGenerationV3` and `VAM_DesktopCapture` (a test capture of the final back buffer including the overlay).

## Settings

| File / section / key | Values | Effect |
|---|---|---|
| `ofxr_bridge.ini` (VR) or `desktop-presenter.ini` (desktop) `[ofxr] frame_multiplier` | 2, 3, 4 | frames per real frame, live |
| `performance.ini` `[Performance] FrameGeneration` | 0/1 | on = generate; 0 pauses through the tray's pause event (VR) or presents unpaced (desktop) |
| `[Performance] FrameGenModel` | `DLSS`, `FSR` | which generator; live. The other one stands in where the chosen one cannot run. FSR is 2x whatever the multiplier |
| `[Performance] DLSSGLite` | 0/1 | NVIDIA lighter candidate blend; quality is lower. DLSS-G only |
| `[Performance] GpuPriority` | `default`/`high`/`realtime`/`cycle` | process GPU scheduling class (VR layer) |
| `[Performance] DLSSFGEyes` | 0-3 | diagnostic: eyes that get DLSS-G (others optical flow) |
| `ofxr_bridge.ini` `[ofxr] deep_pipeline` | 0/1 | one display period of extra depth; needed for 3x and 4x |
| `[ofxr] backend`, `nvidia_preset`, `nvidia_input_scale`, `nvidia_bidirectional` | `nvidia`, `medium`, 50, 0 | optical-flow fallback |
| environment `OFXR_DLSSG`, `OFXR_DLSSG_COPIES`, `OFXR_DLSSG_CAPTURE`, `OFXR_FRAME_MULTIPLIER` | 1 / 1 / dir / 4 | enable guided generation (either generator); DLSS-G's copy mode; capture bursts of DLSS-G inputs for `tests/mv_check.py`; start size |
| environment `VAM_FSR_RUNTIME_PATH` | `<kit>/bin/fsr` | where the FSR API's DLLs are; empty or absent: no FSR |
| environment `VAMKIT_NO_DLSS` | 1 | a test: NGX refuses, so DLSS-G is unavailable and FSR generates (and the compositor has no DLSS SR and no RR). Run on the desktop: `FrameGenModel=DLSS` in the file, `Frame generation model: FSR (the one chosen ... cannot run here)` in dlssg.log, 3600 pairs with 3 resets, real frame rate 72-78 at a 160 Hz output |

## Measured

Simulated headset unless stated; costs per real frame added to the frame (frame generation off = baseline); heavy two-character dance scene.

| What | Result |
|---|---|
| DLSS-G cost, quality model, 2x / 3x / 4x | +2.8 / +4.4 / +5.7 ms |
| DLSS-G cost, lite mode, 2x / 3x / 4x | +0.9 / +2.1 / +2.5 ms |
| Offline, 4x, both eyes at 3072 x 3264 | quality 4.5 ms, lite 1.6 ms |
| Copy elimination (in place), offline, 2 eyes | 4.70 -> 4.46 ms, output byte-identical |
| Whole-image 3072 x 3264 copy | about 0.03 ms |
| Quality model per eye, simulated headset vs the development machine's headset streamed over Wi-Fi | 2.1-2.6 ms vs 4.4 ms (only while streaming) |
| Real frame time, 144 Hz, 4x, one scene | about 30 ms on the headset vs about 19.5 ms simulated; the streaming app itself takes 0.7-1 ms per displayed frame (3-4 ms per real frame at 4x), about 4.5 ms is unexplained |
| Desktop presenter v2, 160 Hz, path tracing with RR, scene playing | 3x RR input 50%: 51.2 real fps; 3x RR 75% with lite: 52.3; 4x RR 50%: 39.4 (caps 53.3 and 40); present interval median 6.4 ms, p95 6.8-6.9 ms; repeated frames 0 |
| Same scene, v1 presenter | real frame rate quantised to 160/k |
| FSR frame generation 3.1.6 on the desktop, one session (RTX 5090 D, window 3840 x 2160, input 67%, one person standing, the model switched live) | real frame rate 37 with DLSS-G at 4x, 71-73 with FSR (2x whatever the multiplier says), 37 again with DLSS-G; 1800 FSR pairs with 1 reset; a generated frame differs from the real frame it follows by 65-71 dB PSNR in this near-still scene (DLSS-G: 61-65 dB): no artefact, and no test of motion |
| FSR frame generation in VR, simulated headset (144 Hz, 3072 x 3264 per eye, input 50%, the same scene) | one generator per eye; VaM renders 36 fps with DLSS-G at 4x, 72-73 with FSR, 36 again with DLSS-G; 144 frames a second handed to the headset each time, no repeated refresh |
| No NVIDIA optical flow and no DLSS (`VAMKIT_NO_NVOF=1`, `VAMKIT_NO_DLSS=1`), simulated headset, 144 Hz, 3072 x 3264 per eye, two sessions | `K` and `DLSS` stay in the run's file; XeSS stands in for K, FSR for DLSS-G, each eye's swapchain "runs on FSR alone"; VaM 73 fps, 144 frames a second handed over, no repeated refresh in 4609; one pair per eye held the previous frame (the first, without guides); head turn at 20 degrees a second: shift per frame within 0.35 px of the pose |
| FSR 3.1.6 frame generation, GPU time per pair, copies included | 0.98-1.04 ms per eye at 3072 x 3264 (render 1528 x 1624); 0.88-0.90 ms at 3840 x 2160 on the desktop (render 2573 x 1447). DLSS-G's quality model takes several times that (frame-budget.md) |
| `nvngx_dlssg.dll` removed from the kit, desktop | the launcher takes 310.9.16 from the graphics driver's store (`runtimes.txt`); DLSS-G 2x 72 fps, 4x 37 fps as with the kit's file |
| FSR asked for two generated frames per pair (`VAMKIT_FSR_FRAMES=2`, FSR 3.1.6, NVIDIA card) | no error, the same 0.90 ms, and the second output comes back untouched: equal to the real frame pixel for pixel. This provider makes one frame per pair |
| Head turn at 20 degrees a second in that session: content shift between consecutive frames handed to the headset (real and generated), half-size captures, the pose calls for 2.27 px | DLSS-G 4x: 2.53-2.62 px; FSR 2x: 2.52-2.60 px (with FSR upscaling too: 2.53-2.64). The same constant offset from the pose with both and the same evenness: FSR's frames sit at their in-between poses like DLSS-G's |

## Pitfalls

| Symptom | Cause | What to do |
|---|---|---|
| 3x or 4x chosen, 2x shown | FSR is the generator (chosen, or DLSS-G cannot run on this card): one frame per pair | `dlssg.log`: `Frame generation model: FSR`, `Frame multiplier switched live to 2X (...; FSR frame generation makes one frame per pair)` |
| `FSR frame generation not offered` or `cannot run here` | the loader or `amd_fidelityfx_framegeneration_dx12.dll` is not in `vamkit/bin/fsr` (optional items of `vamkit fetch`) | fetch and install them |
| A short stall when the generator model is switched | the queue is drained and the new generator's feature or context is made (0.2-0.6 s) | expected; it is a menu action |
| DLSS-G "feature creation failed" together with RR / NR failures | NGX could not get address space (CUDA module); see [frame-path.md](frame-path.md) | find the reserver of address space |
| DLSS-G unavailable though the runtime is present | NGX D3D12 was initialised before the DLSS-G folder was known | `OFXR_PrepareFrameGenerationV3` first, then NR |
| Stutter with a high displayed frame count | a real frame passed its n-period budget | give the real frame margin; count 4+ period gaps in the flight log |
| Black bars that follow head translation | hidden-area pixels given camera motion | keep the `depth >= 0.9999` rule |
| Doubled image on head turn | missing GPU sync between hand-over copy and next write | `synchronize_producer_queue` |
| Menu closes and re-opens by itself | frame generation resumed in the frame the menu button was released | keep the 0.6 s hold in the menu plugin |
| Desktop flashes to the desktop for a fraction of a second | ghost window after "not responding" | check `dlssg.log` for "judged NOT RESPONDING": answered `WM_NULL` means the keep-alive did not take, unanswered means the main thread really stopped |
| `Present` stalls Unity | presenting on Unity's queue | keep the presenter's own queue |
| Real fps at exactly 160/k on the desktop | the old presenter design | not in this code |

## Tried and dropped

- **NVIDIA lighter model as a default**: rejected on quality, kept as `DLSSGLite`.
- **One DLSS-G evaluation for both eyes** (side by side): saves only about 0.5 ms (a fixed ~0.3 ms per call, the rest scales with pixels) and each evaluation accepts one camera, so two
  asymmetric frusta have no correct matrix. **One queue per eye** was worse (the GPU is already full; the pair finished 0.1-0.3 ms later); one shared queue is right.
- **Community-optimised DLSS-G kernels** (rewritten kernels with cross-kernel fusion): its author has not measured RTX 40 / 50 cards against the official kernels and it may be
  slower; they are built for an older architecture and the driver would JIT them. Not integrated, not measured here.
@@@ END

- **FSR's own swap chain** (AMD's recommended integration): VR has no swap chain, and on the desktop it would replace
  the presenter's pacing. The context is dispatched by hand instead (above).
@@@ END

- **The layer's FidelityFX optical-flow backend** as the way to frame generation without NVIDIA: it needs AMD's SDK
  1.1.4 built from source and gives optical flow without the engine's depth and motion; FSR frame generation uses both.
- **Optical flow as the default**: only DLSS-G has the engine's depth, motion and camera.

## Not built here, and what it takes

Nothing below is ruled out. Each entry says where to start and what a session must show.

- **DLSS Frame Generation at 5x and 6x.** `nvngx_dlssg.dll` 310.9.16 on an RTX 50 reports `MultiFrameCountMax=5`
  (dlssg.log); the code stops at three generated frames. To raise it: `kGuidedMaxOutputs`
  (`d3d12_frame_generator.hpp`), `kMaxExtraSynthetics` and the per-output fractions (`d3d12_frame_synthesizer.hpp`),
  the layer's `frames_most` and swapchain budget in `openxr_layer.cpp` ("four for 3X and six for 4X"), the presenter's
  clamp to 2-4 and its flip buffers (`desktop_presenter.cpp`), `(2, 3, 4)` in `LaunchPanel.py`, the multiplier row of
  `VaMVRMenu.cs`, `OFXR_FRAME_MULTIPLIER`. Show on the simulated headset: the cadence (144 Hz needs 28.8 real fps at
  5x, 24 at 6x), no repeated refresh, DLSS-G's time per pair.
- **XeSS frame generation on the desktop** (never VR: its only entry points wrap a DXGI swap chain).
  `xefgSwapChainD3D12InitFromSwapChain` can take the presenter's swap chain; XeSS then presents and paces by itself, so
  the presenter hands it real frames only, tags depth and motion (`xefgSwapChainD3D12TagFrameResource`) and the camera
  (`xefgSwapChainTagFrameConstants`) and calls `xefgSwapChainSetPresentId` before `Present`. It needs XeLL on every
  card (`xellD3D12CreateContext`, `xefgSwapChainSetLatencyReduction`, sleep and markers each frame), `libxess_fg.dll`
  and `libxell.dll` from Intel's repository at the commit the manifest pins, and a generator kind of its own (it
  returns no texture, so it does not fit `D3D12GuidedFrameGenerator`). One generated frame off Intel cards, up to
  `maxSupportedInterpolations` on Arc. The owner of the development machine was asked on 2026-11-11 and did not want
  it for that machine, where FSR already gives 2x; on an Arc card it is the way to more than 2x on a flat screen.
- **FSR with more than one generated frame.** The dispatch has four outputs; the provider this ran on (3.1.6, an
  NVIDIA card) fills one (Measured). `VAMKIT_FSR_FRAMES=2` repeats the probe on any card: the shown generated frame is
  then the second output, and a capture equal to the real frame means it was not written. If a provider fills more
  (AMD's ML generator on a Radeon was not run), extend `max_outputs()` and the single `output` texture of
  `D3D12FsrFrameGenerator`; the layer and the presenter already follow `max_outputs()`.
- **The graphics drivers' own frame generation on the desktop** (NVIDIA Smooth Motion, AMD Fluid Motion Frames). They
  double what a swap chain presents, from colour alone: the presenter's output, never a headset. Nothing in the mod
  stops them; stacked on the mod's own generation they would interpolate interpolated frames, so one of the two is switched off (`[Performance] FrameGeneration=0`). AMD's can be
  switched by a program through ADLX (`GetAMDFluidMotionFrames`, `SetEnabled`); NVIDIA's is a per-program setting of
  the NVIDIA app. Not tried: whether Smooth Motion engages on the presenter's swap chain (a D3D12 queue of its own
  under a D3D11On12 game). One launch finds out.
- **Lossless Scaling** captures a window and shows interpolated frames in its own; it has no interface for a game. It
  can run over the desktop window with the mod's generation off.
- **The headset runtimes' own synthesis** (Virtual Desktop's SSW, SteamVR motion smoothing, Meta's ASW) runs at half
  rate in the runtime, and the launcher asks for SSW off while the mod generates. The compositor submits no depth
  layer (`XR_KHR_composition_layer_depth`), which some runtimes use to reproject position: that would be added where
  `components/render/opencomposite` builds the projection layer. Whether a PC runtime takes motion vectors from the
  application (`XR_FB_space_warp`) was not established.
- **Optical flow on cards without NVIDIA's.** Upstream's FidelityFX backend is compiled out (`external/ffx-stub`);
  AMD's FidelityFX SDK 1.1.4 (MIT) built and given as `XRFG_FIDELITYFX_SDK_ROOT` brings it back. It would serve only
  pairs without guides, which now hold the previous frame.

## Open

- FSR frame generation was run on one NVIDIA card, with a near-still scene on the desktop and a scripted head turn on
  the simulated headset. Nobody judged it in motion on a monitor or in a headset, and nothing was run on a Radeon or an
  Arc card (where AMD's API may select its ML generator for the context; documented only with AMD's swap chain).
- VR on Radeon and Arc: the layer's mode without NVIDIA optical flow was run only on an NVIDIA card with
  `VAMKIT_NO_NVOF=1` (see Measured); what else of the layer or its tray assumes an NVIDIA card is unknown.
- DLSS-G alone without optical flow (`VAMKIT_NO_NVOF=1`, 4x): 9 repeated refreshes in 4608 (32 s) where the same
  build with optical flow had none in 1152 (8 s). Not attributed; no pair was held.
- `GpuPriority` has not been run on a headset; whether it moves the quality model back from 4.4 ms toward 2.3 ms is unknown.
- Heavy scenes in a real headset reached only 90-115 displayed frames a second at 3x and 4x; the simulated headset did not reproduce it. A controlled
  headset A/B (same position, menus closed, script alternating 3x and 4x) is needed.
- Loading a character or scene with the menu closed in a real headset dropped to 2-13 fps for 10-30 s; the share of each component was not separated.
- Changing the render scale while a menu writes model and scale rebuilds the SR feature at the old size first, then again; DLSS-G rebuilds each time; each
  switch costs 1-3 s. The layer's `creation_allocators` are never cleared (small).
- Whether the "not responding" fix holds for a foregrounded window is not visible to a background test.

## Verify

- `components/render/ofxr-dlssg/tests/` contains the upstream layer tests (CMake, `ctest`) plus `mv_check.py`: run the game with `OFXR_DLSSG_CAPTURE=<dir>` and
  `python -I tests/mv_check.py <burst dir> [<image dir>]`. It fits sampling of A at B's motion for a range of scales (1.0 is correct), per tile, and
  recomputes motion from depth and the camera matrix. Misfits are luminance differences over textured pixels only.
- `dlssg.log` (`OFXR_DLSSG_LOG`) shows `DLSS-G[n] generator ready ... in place`, mode switches, `desktop ResizeBuffers ... kept` and foreground changes.
  For FSR: `FSR frame generation can run here; versions ...`, `Frame generation model: FSR`, `FSR-FG[n] context created: ...
  version 3.1.6, dispatched by hand (no FSR swap chain)`, `FSR-FG[n] first pair recorded`, and a count every 1800 pairs.
- What was asked for and what generates, for a program: `running.json` in the run's `dlssg` log folder
  (`write_running` in `d3d12_fsrfg.cpp`, written whenever a generator is made, by the layer and by the presenter):
  `frame_generation` {`wanted`, `running`, `reason`, `frames_most`}, `can_run` and `why_not` for `DLSS` and `FSR`.
  `running` is `none` where neither could be made. Read it rather than the log's wording.
- Switches for the paths this machine does not take by itself, set in the environment of the script that starts the
  session: `VAMKIT_NO_DLSS=1` (NGX refuses), `VAMKIT_NO_NVOF=1` (no NVIDIA optical flow), `VAMKIT_AS_VENDOR=1002`
  (the compositor orders the stand-ins as on an AMD card), `VAMKIT_FSR_FRAMES=2` (the probe above).
- On the desktop `VaMDLSSBridge.CaptureGenerated(path)` (through the test agent) writes the next generated frame, the
  real frame it follows (`.real.ppm`) and what was presented (`.presented.ppm`).
- Display cadence and hitches can be read from the flight log, but only if diagnostics were on for the run.
- Only a person in a headset can confirm head-turn artefacts, the hidden-area bars, encoder blockiness and the feeling of latency.
