# Frame generation (DLSS-G) in VR and on the desktop

The real frame rate of a path-traced or heavily shaded VaM scene is a fraction of what a headset or a monitor can
show. DLSS Frame Generation (DLSS-G) makes the in-between pictures from the last two real frames, depth, motion vectors and
camera, so a scene rendered at 30-40 real frames a second can fill a 120-160 Hz display with 2x, 3x or 4x the frames. What a player
sees is smoothness; what an engineer must keep in mind is that every real frame now has to fit in "multiplier" display periods, or the
display shows an empty slot and the scene stutters although the displayed frame count looks high.

In VR the generator runs inside an OpenXR API layer (OFXR, `components/render/ofxr-dlssg/`) that sits between OpenComposite
and the runtime. On the desktop there is no OpenXR runtime, so our own presenter inside the 11on12 proxy
(`components/render/d3d11-proxy/desktop_presenter.cpp`) owns the swap chain and compiles the same generator source
(`ofxr-dlssg/src/d3d12/d3d12_dlssg.cpp`). Where the picture and its guides come from: [frame-path.md](frame-path.md),
[motion-vectors.md](motion-vectors.md).

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

### Multipliers and cadence

`[ofxr] frame_multiplier` 2, 3 or 4 (menu, live). The layer tells the game, through `xrWaitFrame`, to run at the display rate divided by the
multiplier, so a 120 Hz headset gives the game 60, 40 or 30 real frames a second. A change drains the presenter queue, re-phases once, and the
game sees one frame without synthetics and its period step by one display period. 3x and 4x run the "deep pipeline" (`[ofxr] deep_pipeline=1`,
one display period of extra depth bought with about one frame of latency); the shallow pipeline could not receive a frame from VaM in time for 3x. A 4x
session is created with staging for four frames whatever the launch multiplier (`OFXR_FRAME_MULTIPLIER=4` at launch; `frames_most`), so
2x/3x/4x switch without remaking swap chains. The layer generates at most 3 frames per real frame; the NGX feature itself reports 5 but 5x is not built.

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
`LaunchPanel.py`). The mod does not offer optical flow as a mode of its own; it is a safety net.

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
(per-frame MV comparison with `tests/mv_check.py`); do not suspect them again without new evidence. The stock layer's 3x mode used a shallow pipeline that could
not wait for VaM's frames; 3x and 4x therefore take the deep pipeline.

### The desktop presenter

`desktop_presenter.cpp`. Unity draws to wrapped D3D12 surfaces that the proxy hands out as the swap chain's buffers; the presenter owns a real flip-model chain (3 buffers, waitable,
`SetMaximumFrameLatency(1)`) on its own HIGH queue, so DLSS-G, the copies into the swap chain and the waits of `Present` never sit on Unity's queue.

- **One real frame** (`Group`): wait for Unity's ready fence; copy the frame into a private image ("shown"); record DLSS-G from the frame and the guides
  published by the compositor (`OFXR_PublishD3D11GuidesV4`); submit and release the inputs so Unity's queue only waits, on the GPU, until the copy is taken
  and Unity renders the next frame while this one is shown; then show the generated frames followed by the real one.
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
| `[Performance] DLSSGLite` | 0/1 | NVIDIA lighter candidate blend; quality is lower |
| `[Performance] GpuPriority` | `default`/`high`/`realtime`/`cycle` | process GPU scheduling class (VR layer) |
| `[Performance] DLSSFGEyes` | 0-3 | diagnostic: eyes that get DLSS-G (others optical flow) |
| `ofxr_bridge.ini` `[ofxr] deep_pipeline` | 0/1 | one display period of extra depth; needed for 3x and 4x |
| `[ofxr] backend`, `nvidia_preset`, `nvidia_input_scale`, `nvidia_bidirectional` | `nvidia`, `medium`, 50, 0 | optical-flow fallback |
| environment `OFXR_DLSSG`, `OFXR_DLSSG_COPIES`, `OFXR_DLSSG_CAPTURE`, `OFXR_FRAME_MULTIPLIER` | 1 / 1 / dir / 4 | enable; copy mode; capture bursts of DLSS-G inputs for `tests/mv_check.py`; start size |

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

## Pitfalls

| Symptom | Cause | What to do |
|---|---|---|
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
- **5x**: the feature reports a maximum of 5, the layer's staging is built for 4.
- **Optical flow as the default**: only DLSS-G has the engine's depth, motion and camera.

## Open

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
- Display cadence and hitches can be read from the flight log, but only if diagnostics were on for the run.
- Only a person in a headset can confirm head-turn artefacts, the hidden-area bars, encoder blockiness and the feeling of latency.
