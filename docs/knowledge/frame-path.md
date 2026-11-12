# The path of one frame

VaM is a Unity 2018.1 game that draws a stereo image (or one window image on the desktop) with a D3D11 device. The mod
never replaces Unity's renderer. It lets Unity draw, copies what the reconstruction needs (depth, camera, motion), and
takes the frame over at four places: before Unity's opaque pass (path tracing), after its transparents (the capture), at
eye submission (the compositor) and at presentation (frame generation, or our own presenter on the desktop). A player
sees the result as a render at a fraction of the output size, reconstructed to full size by DLSS (RR preset F when path
tracing is on, SR K / L / M when it is off), cleaned by the neural denoiser, tone mapped, and then multiplied in
frame rate by DLSS Frame Generation.

This document is the map. Each stage has its own document: [d3d11on12.md](d3d11on12.md) for the device,
[motion-vectors.md](motion-vectors.md) for the guides, [upscaling.md](upscaling.md), [denoiser.md](denoiser.md),
[frame-generation.md](frame-generation.md), [path-tracing.md](path-tracing.md), [settings-and-menu.md](settings-and-menu.md).

## How it works

### Stages and owners

| # | Stage | Where it lives | Entry points |
|---|---|---|---|
| 0 | Launch: swaps VaM's own files, sets the environment, arms OFXR in VR | `components/render/launcher/launch.py` | `run_mod`, `mod_environment`, `activate_mod`, `restore_files` |
| 1 | Unity camera, jitter, depth, motion collection, post-processing hand-over | `components/render/dlss-bridge/VaMDLSSBridge.cs` (BepInEx plugin `local.vam.dlssbridge`) | `PreCull`, `PreRender`, `LateUpdate`, `SetupPostEffects` |
| 2 | Unity's D3D11 device becomes a D3D11On12 device | `components/render/d3d11-proxy/d3d11_proxy.cpp` (VaM's `d3d11.dll`) | `Proxy_D3D11CreateDevice`, `CreateOn12`, `D3DInterop_GetOn12ForDevice` |
| 3 | Compositor: guides, SR or RR, denoiser, sharpening, UI composition, tone-mapping hand-off | `components/render/opencomposite/OpenOVR/Compositor/` (VaM's `openvr_api.dll` is OpenComposite) | `dx11_vam_dlss.cpp` (`OnRenderEvent`, `Capture`, `PrepareGuides`, `Upscale`), `vam_rr.inl` (`Reconstruct`, `FinishPathFrame`, `ToneRr`, `PostRr`, `SubmitRr`), `dx11_vam_nis.cpp` (`VamNis::Process`), `dx11_nr_prepass.cpp` |
| 4 | Path tracer: scene, materials, the trace itself (called from stage 3 on the shared D3D12 queue) | `components/shading/skin/native/rt.cpp`, `components/shading/rt-core/` | `ST_RecordPathTraceV1`, `ST_FinishPathTraceV1` |
| 5 | Frame generation, VR: an OpenXR API layer between OpenComposite and the runtime | `components/render/ofxr-dlssg/` | `src/layer/openxr_layer.cpp`, `src/d3d12/d3d12_dlssg.cpp`, provider exports `OFXR_PublishD3D11GuidesV3` |
| 5' | Presentation and frame generation, desktop: our own DXGI owner inside the proxy DLL | `components/render/d3d11-proxy/desktop_presenter.cpp` | `VamDesktopInstall`, `Group` |
| 6 | Runtime and headset (Virtual Desktop's OpenXR runtime in development; any OpenXR runtime that OpenComposite can use) | not ours | |

Tone mapping is a plugin of its own (`components/shading/tone-mapping/plugin/VaMToneMapping.cs`); the compositor only
changes where it runs (below).

### Stage 0: launch

`launch.py --variant mod --display VR|Desktop --run <folder>` copies OpenComposite over VaM's `VaM_Data/Plugins/openvr_api.dll`
and the proxy next to `VaM.exe` as `d3d11.dll`, writes `opencomposite.ini` (`initUsingVulkan=false`,
`invertUsingShaders=true`, `supersampleRatio=1.0`), starts the OFXR tray in VR (it registers the OpenXR layer while
it runs), and starts `VaM.exe -vrmode OpenVR` (or `None` on the desktop). On exit it puts VaM's own files back from
`vamkit/original/`. `--variant vanilla` restores everything, removes our variables from the environment and starts VaM
with BepInEx switched off, so nothing of ours loads.

`mod_environment()` sets, and what reads each one:

| Variable | Value | Read by |
|---|---|---|
| `VAM_DISPLAY_MODE` | `VR` or `Desktop` | the bridge (`Desktop` field: one view instead of two), OpenComposite (`VamDesktopDisplay()` in `components/shading/rt-core/include/startup_display.h`), the proxy (desktop presenter only installs on Desktop), the window-camera plugin |
| `VAM_PERFORMANCE_CONFIG` | `<run>/performance.ini` | compositor, OFXR `d3d12_dlssg.cpp` (lite mode, GPU priority), the desktop presenter, the VR menu |
| `VAM_DLSS_BRIDGE` | `1` | the bridge: without it the plugin stays dormant |
| `VAM_DLSS_RUNTIME_PATH`, `VAM_NR_PREPASS_PATH`, `OFXR_DLSSG_RUNTIME_PATH` | `<kit>/bin/dlss/{sr,nr,fg}` | where NGX finds each DLSS runtime; NGX D3D12 keeps one setup per process, so SR's and DLSS-G's folders are both listed by `PrepareNgx12Folders()` |
| `VAM_DLSS_LOG`, `VAM_NR_PREPASS_LOG`, `OFXR_DLSSG_LOG` | `<log>/dlss`, `ngx-prepass`, `dlssg` | per-run logs |
| `D3DINTEROP_MODE` | `on12` | the proxy: without it every call goes to the real `D3D11CreateDevice` |
| `OFXR_DLSSG` | `1` | OFXR: enables DLSS Frame Generation (otherwise optical flow only) |
| `OFXR_FRAME_MULTIPLIER` | `4` only for a 4x launch | OFXR: sizes the staging for 4x; the live multiplier is `[ofxr] frame_multiplier` |
| `VAM_DESKTOP_CONFIG` | `<run>/desktop-presenter.ini` (Desktop only) | the presenter: `[ofxr] frame_multiplier`, `[overlay] position` |

Path tracing is deliberately not in this list: it is one in-game switch, `[RayTracing] Renderer` in
`BepInEx/config/local.vam.skintransmission.cfg`, and the compositor follows it every frame (next section). The
development tree also chose the lighting route at launch and built only that route's resources; this repository does not.

### Stage 1: the bridge (Unity side)

The bridge selects the main camera: in VR the stereo camera without a target texture and the lowest depth, rescanned
every 2 s (`SelectTarget`); on the desktop `CameraTarget.centerTarget.targetCamera`. On the desktop it also creates
a render target for that camera and a camera named "VaM Desktop Output" (depth 2.001, culls nothing) whose command
buffer fires the presentation event.

Per frame, in `PreCull` / `PreRender`:

- **Jitter.** A Halton(2,3) offset is added to the projection (stereo projections in VR). The phase count comes from the
  compositor (`VaMDLSS_GetJitterPhaseCount`, 8 x (output / render)^2, clamped 8-72). MSAA is forced off (DLSS replaces it
  and a multisampled depth buffer cannot be read). The projection is restored in `PostRender`.
- **The frame record.** A 1040-byte block (`FrameInfoV3`, version 3, magic `VDLS`; the layout is mirrored in
  `dx11_vam_dlss.cpp` and must stay in sync) in one of 4 slots, written with the unjittered and jittered matrices, the previous
  frame's, the draw lists for object motion, hair and rigid and skinned props, the UI rectangles (path tracing only) and the
  view count. It reaches the compositor through `CommandBuffer.IssuePluginEventAndData`.
- **Events** (all to `VaMDLSS_GetRenderEventFunc()`, handled by `OnRenderEvent` in `dx11_vam_dlss.cpp`):

| Event id | Camera event | Meaning |
|---|---|---|
| `0x56440001` (capture) | `AfterForwardAlpha` on the raster route; `BeforeForwardOpaque` when path tracing is on (frame flag 16) | copy the bound depth, take the colour target, run object motion, then call the registered RR callback |
| 5 | `BeforeForwardAlpha`, raster route only | depth and camera colour before the transparents (see [motion-vectors.md](motion-vectors.md)) |
| 3 | `AfterForwardAlpha`, path tracing only | late composition: UI and Unity's own effects drawn after the early hand-off are merged into the PT result (`FinishPathFrame`) |
| 4 | desktop output camera, `BeforeForwardAlpha` | desktop presentation (`DesktopHost().Present`) |
| 1, 2, 3 on `VaMDLSS_GetPostEventFunc()` | inside `PostProcessingBehaviour.OnRenderImage` | the post-processing hand-over (below); a separate entry point, separate ids |

  The bridge switches the capture event by polling `VaMDLSS_GetState()` (`StatePathRendering` = 32768): it moves its command
  buffer between the two camera events when path tracing is switched on or off, and drops its frame history.
- **Render scale.** The bridge sets `XRSettings.eyeTextureResolutionScale` to VaM's own render scale times
  `VaMDLSS_GetEyeTextureScale(percent)`; the compositor computes it from the runtime's recommended size, `OutputScale`
  and the live scale, so Unity really renders fewer pixels. 100% is DLAA, not "native". Native frames (no jitter, SR
  skipped: `FlagNative`) are rendered while `DLSSPreset = Off` and while VaM's own menus are open (`DLSSMenuNative`).
- **Post-processing hand-over (path tracing only).** Scenes grade through Unity's PostProcessing v1 component (bloom, LUT,
  vignette, grain, depth of field). A Harmony prefix/postfix on `PostProcessingBehaviour.OnRenderImage`
  (`PostEffectsBefore` / `PostEffectsAfter`) substitutes output-size render textures: the effects run on RR's
  full-size output, not on a camera image RR would then replace. Event 1 copies RR's scene into the target, 2 copies the
  result back, 3 supplies the (flipped) path-traced depth for depth of field. Depth of field is switched off for the draw in
  VR (a lens chosen for a 20-35 degree monitor view blurs nearly everything at 110 degrees; the plugin's own state is
  restored); `PostDofInVr` re-enables it. On the desktop it runs.
- **UI registry (path tracing only).** A Harmony postfix on `Graphic.OnEnable/OnDisable` keeps an event-driven list of
  UI elements (no per-frame or timed scene search). `CollectPathUi` writes each visible element's rectangle and a flag:
  1 = on top of the scene (ZTest Always shaders such as `UI/Default-Overlay`, `GUI/Text Shader Overlay`; in VR every
  world canvas while path tracing runs), 0 = depth tested.

### Stage 2: the proxy

`d3d11.dll` in VaM's folder intercepts `D3D11CreateDevice` from `UnityPlayer.dll` and returns a D3D11On12 device on
the process's single D3D12 device with one direct queue of its own. Everything after that sits on that queue.
[d3d11on12.md](d3d11on12.md) explains why and what it costs. The same DLL carries the desktop presenter.

### Stage 3: the compositor

OpenComposite is VaM's OpenVR implementation, so VaM's eye textures arrive at `Submit`. The mod's additions live in the
files listed in the table. For each eye, in submission order (`VamNis::Process` in `dx11_vam_nis.cpp`):

1. crop the eye out of the double-wide image;
2. build the eye's guides (`PrepareGuides`: depth, motion with the jitter removed, UI mask; motion is zeroed and depth set far
   where the headset's hidden-area mesh is, see [frame-generation.md](frame-generation.md));
3. NR before the upscaler only if `NRAfterUpscale = 0` (not with RR);
4. the reconstruction: `VamDlss::Upscale` -> SR K/L/M (NGX D3D12 through the 11on12 queue), or, with path tracing,
   `SubmitRr` (RR's output was already made at capture time);
5. NR after the upscaler on a copy of the output reduced to `NRAfterScale` %, its edit added back
   ([denoiser.md](denoiser.md)); NVSharpen, last or right after SR (`DLSSSharpness`, `DLSSSharpenLast`);
6. publish depth, motion and camera to OFXR (`PublishFrameGeneration`) and hand the image to the runtime.

**Which reconstruction.** `ReadVamPerformanceSettings()` (`vam_performance_config.h`) asks `VamPathRequested()`, which calls
`ST_PathTracingRequestedV1` in `VaMSkinTransmissionNative.bin` on every call. Path tracing on: RR preset F (internal
`dlssPreset = 6`), input scale `RenderScaleRR` (33-100), whatever the file holds for SR. Off: `DLSSPreset` (`Off`/`K`/`L`/`M`)
at its own `RenderScaleK/L/M`; a hand-edited `DLSSPreset = RR` is read as K at K's scale, so RR cannot run without path
tracing. A path-tracing failure switches the renderer off by itself (`ST_SetPathTracingV1(0)`) and the raster route
resumes on the next frame. The cached settings are re-parsed only when the file's stamp changes or the path flag flips.

**Path-tracing call chain.** The skin plugin's native module registers `ReconstructSkin` with `VaMDLSS_RegisterRr`.
`OnRenderEvent` calls it right after the capture; it calls `VaMDLSS_Reconstruct`, i.e. `Reconstruct()` in `vam_rr.inl`:

1. `EnsureRr` sizes RR's per-eye textures (made in D3D12 and wrapped for D3D11; see [d3d11on12.md](d3d11on12.md));
2. per eye: `PrepareGuides`, then on a D3D12 command list from `VamOn12Commands` (after a D3D11 `Flush`): the textures
   handed over, `ST_RecordPathTraceV1(list, view)` records the trace into colour, albedo, normal, specular, depth,
   hit-distance and detail-reference targets, then `NGX_D3D12_EVALUATE_DLSSD_EXT` (RR) evaluates, submit, `ST_FinishPathTraceV1`;
3. the optional detail-preservation pass, then either
   - **early** (frame flag 16): `HandoffPathTarget` writes PT's real depth into Unity's depth so raster effects and editor UI
     test against hair and alpha geometry too; the RR output stays unpublished until event 3, where `FinishPathFrame`
     composites the UI overlays (`ST_GetPathOverlayV1`: a black and a white replay, so translucent UI blends correctly)
     into RR's output. The early capture itself never takes the UI layer: no UI is drawn yet, and taking a layer marks it
     read, which sends every later UI draw of that frame to the output stage layer (below). On a headset Unity draws its
     hidden-area mesh first, with a pixel program the UI catalog lists, so the layer already exists that early; or
   - **late** (warm-up and fallback): the composite compute shader writes `rrScene` at once.

`rrScene` is the full-size output. A failure keeps the previous coherent stereo image for that frame only and falls back.

**Tone mapping position.** With K/L/M the tone-mapping plugin runs on the camera image before SR, as in VaM's own
frame order. With RR it must run on RR's HDR output at full size: the plugin wraps its render event with
`VaMDLSS_WrapToneMapEvent`; `ToneRr` binds `rrScene` as the target, runs the real tone-mapping event, then blits the
result into the camera target so Unity's remaining draws see it. When the camera must stay HDR for RR the compositor sets
state bit 16384 in `VaMDLSS_GetState()`, which the tone-mapping plugin reads (it keeps `allowHDR` on).
UI drawn after tone mapping (desktop UI cameras, the world canvases of a third-party stage) goes through the
"output stage layers" of the path-tracing overlay (`ST_GetPathOverlayV1` stages 1 and 3) and is composited by `SubmitRr`.
These layers are taken when the frame is presented. The skin plugin's frame number advances at `WaitForEndOfFrame`; the
desktop presents before that, a VR runtime takes the eyes after it. So a late layer of the frame that just ended still
counts, unless a read during an earlier frame took it (`PathOverlayCurrent` in `path_overlay.inl`); without that rule every
UI drawn by a second UI camera (and VaM's main menu inside such a stage) was lost in VR while the desktop showed it.

### Stage 5: frame generation (VR)

OFXR (an OpenXR implicit API layer, upstream project forked for this mod) sits between OpenComposite and the runtime.
`PublishFrameGeneration` hands it depth, motion, jitter and the camera of each eye with the image about to be released
(`OFXR_PublishD3D11GuidesV3`); the layer feeds DLSS-G once per real frame and eye. A pair without guides (or with a render
larger than the output) falls back to NVIDIA optical flow. Details, cadence and cost: [frame-generation.md](frame-generation.md).

### Stage 5': the desktop

VR's last two stages are replaced. `desktop_presenter.cpp` (inside the proxy) hooks the DXGI factory: Unity sees ordinary
D3D11 surfaces (wrapped D3D12 resources), the presenter owns the real flip-model swap chain, copies each finished frame,
runs DLSS-G on it and shows the real and generated frames evenly paced. OpenComposite's `DesktopRenderHost`
(`dx11_vam_dlss.cpp`) runs the same SR / RR / NR / sharpen chain with one view and writes into the surface Unity will
present, inheriting Unity's current HDR render target (the bridge's output camera fires event 4; Unity's final screen resolve then
restores orientation). The input texture is smaller than the window, so mouse and keyboard coordinates that VaM and
`UnityEngine.UI` map through `Camera.ScreenPointToRay` and friends are rescaled at 94 call sites; this is in the bridge,
not in Windows. The same chain, same settings, `viewCount = 1`; every code path that assumes two eyes must take
`frame.viewCount` and must not infer the eye count from the aspect ratio.

### What the window-camera plugin turns off

`components/render/window-camera/VaMWindowCameraVR.cs` (BepInEx `local.vam.windowcameravr`). A WindowCamera atom with "Camera On"
enables two cameras: `MainWindowCamera` (no target texture; draws the whole scene again, with every shadow map, to the
desktop window) and `MiniCamera` (a small texture on the atom's own display planes). Harmony postfixes on
`CameraControl.SyncCameraOnAndUseAudioListener` and `SyncUseAsMainCamera` disable the `Camera` components (not the
`AudioListener`, so audio stays) while VR is on, and in this mod's desktop mode too, where the output camera overwrites the
whole window anyway. `MiniCamera` is off in both modes (its display planes freeze on the last picture). A camera that VaM
treats as the main camera keeps rendering. VaM's own desktop mode, started without `VAM_DISPLAY_MODE`, is not touched.

## Settings

| File / section / key | Values | Read | Effect |
|---|---|---|---|
| `performance.ini` `[Performance] DLSSPreset` | `Off`, `K`, `L`, `M` | live, 250 ms, on file change | raster route's SR model; `RR` is read as K |
| same `RenderScaleK/L/M/RR` | 33-100 | live | input scale per model; 100 = DLAA |
| same `OutputScale` | 75-150 | live; a new value applies after 0.5 s of rest | output = runtime's size x this; rebuilds swap chains, SR and DLSS-G |
| same `NRAfterUpscale`, `NRAfterScale` | 0/1, 25-100 | live | NR position and working size (RR forces "after") |
| same `DLSSSharpness`, `DLSSSharpenLast` | 0-100, 0/1 | live | NVSharpen |
| same `DLSSMenuNative` | 0/1 | live | native 100% frames while VaM's own menus are open |
| same `FrameGeneration`, `DLSSGLite`, `GpuPriority` | see [frame-generation.md](frame-generation.md) | live | |
| same `RRStereo`, `PTReference`, `RRTransparentGuides` | 0-100, 0/1, 0/1 | live | cross-eye resolve (VR + PT, default off); PT reference view; legacy RR guide fade |
| `local.vam.skintransmission.cfg` `[RayTracing] Renderer` | `PathTracing` / `Legacy` | live, by the menu | the one path-tracing switch |
| `ofxr_bridge.ini` (VR) / `desktop-presenter.ini` (desktop) `[ofxr] frame_multiplier` | 2-4 | live | frame generation multiplier |

How a setting reaches a stage: the launcher writes `<run>/performance.ini`; the VR menu (or the panel) edits it in place;
every reader compares the file's last-write time and size (`VamProfileChanged`, about 2 microseconds) and parses
`[Performance]` again only on change. `GetPrivateProfile*` parses the whole file per key (about 45 us), so polling by
timer without the stamp costs milliseconds. New hot-changeable settings should follow the same pattern. The bridge
asks the compositor, not the file, for scale and state (`VaMDLSS_GetRenderScalePercent`, `VaMDLSS_GetState`).
The launcher keeps one preset per display mode (`vamkit/launcher/profiles/VR`, `Desktop`) and applies it to the plugin
configs at each launch (`ModeProfiles.begin`) and copies the live values back on exit (`finish`), so edit the preset or the
menu, not only a live cfg. A test launch (`--test`) restores the user's files instead of saving.

## Measured

- Per-frame GPU stage costs and budgets are in [frame-budget.md](frame-budget.md). The compositor logs
  `VAM GPU timing ...` every 10 s into `opencomposite.log`.
- Eye textures at the development machine's headset size are about 3072 x 3264 per eye.
- Device cost of the 11on12 layer: [d3d11on12.md](d3d11on12.md).

## Pitfalls

| Symptom | Cause | What to do |
|---|---|---|
| Every DLSS feature fails at once (RR, NR, DLSS-G); log shows `NvAPI_D3D12_CreateCuModule failed`, SR `0xBAD00002` | DLSS builds CUDA modules through NvAPI and needs one large contiguous range of address space; something fragmented it (the Mono collector once reserved every 4 GB boundary) | look for plugins that reserve address space; see [gc.md](gc.md) |
| NGX errors `0xbad00004` / `0xbad0000b` | NGX D3D12 has one context per process; whoever initialises first fixes the DLL directory table | `PrepareFrameGenerationNgx()` runs first, before NR; keep that order |
| Crash in `VamOn12Commands::Begin` at startup | RR assumed another stage had created the command queue | `EnsureRr` initialises it itself; new consumers must too |
| Access violation after path tracing hands motion back, only in scenes with animated props | the bone-row buffer for skinned props was released with the raster motion history; it is created once at startup and rewritten every frame | `objects.boneRows` must survive the hand-off; anything created once must not be in the "rebuild on switch" set |
| Menu or desktop UI missing in a path-traced frame | UI drawn after the early composition does not reach event 3; or a ZTest Always UI is treated as depth tested and removed where a nearer surface exists | UI shader names in `UiAlwaysOnTop`; output-stage layers; see Verify |
| Path-traced VR menus missing only on a real headset, fine in the simulated one | a UI layer was marked read before the UI was drawn (the hidden-area mesh counts as UI), so the menu went to the output stage layer; or that layer's frame number no longer matched when the VR runtime took the eyes | run the simulated headset with `SIMXR_HIDDEN_MASK=1.15`; dump the replay layers (`PathCanvasUi.Dump`) and check which stage the menu went to and whether it was taken |
| Input resize crashes in desktop mode with a post-processing UI camera | a second camera made with `Camera.CopyFrom(main)` kept the bridge's old target | the bridge re-points every scene camera still using its own target before it destroys it |
| Input resize wipes the path-traced scene | `InvalidatePathConsumer` clears materials, textures, hair | do not call it for a size change; `EnsureRr` rebuilds per size inside the same render event, after a D3D11 `Flush` then a queue drain |
| Settings polled every 250 ms cost ~6 ms | each key reparses the file | use the stamp |

## Tried and dropped

- FSR 1, NIS and optical-flow-only frame generation as selectable modes: only DLSS plus DLSS-G are offered. FSR 1 survives as
  the per-frame stand-in when a bridge capture is missing; optical flow survives as DLSS-G's fallback.
- NR + SR run on a second D3D12 queue in parallel with rendering ("AsyncPost"): not faster, uneven frame times; concurrency inside a frame cost
  0.15-0.6 ms more than serial on every queue that was tried.
- Giving menus a separate motion path or a "UI bypass" (FSR 1 over menu pixels): menu text still shimmered because the
  per-frame sub-pixel jitter itself moves glyphs. The fix is to render VaM's own menus as native frames
  (`DLSSMenuNative`).
- Scanning the scene every 2 s for UI or hair (`FindObjectsOfType`): periodic hitches in big scenes. Use event hooks.
- Forcing a native frame for the path-traced entry menu: it broke 100% resolution and interrupted PT, SR and frame generation.
- Fixed-rate variable shading (FFR): NVAPI D3D11 variable-rate shading is unavailable under 11on12; `FFREnabled` stays 0.
- Raster-route RR: RR copied the camera image at the end of the main camera, so a world canvas drawn later by another camera was
  overwritten; on the desktop SR reads its input at presentation and has no such problem. The raster route therefore
  only offers SR (the compositor still contains the raster-RR code, which is never reached).

## Open

- Menu text on the raster route can still swim (menus carry the motion of what is behind them). A design exists
  (send root-canvas corners, read them in the compositor); not built.
- VR + path tracing: all collected world UI rectangles are still sent as "on top". That rule was added before the menu
  regression's causes were found (the early capture taking the UI layer, and late layers missing the VR frame number, both
  described above); it is not needed for either and makes depth-tested world UI cover nearer objects. Whether to drop it
  needs a real headset.
- Translation of every VaM UI shader family into the path-traced composition is by name for the four known on-top shaders.

## Verify

- Build-level: the compositor compiles against NGX headers fetched by `vamkit fetch`; the frame record layout is
  mirrored by hand between `VaMDLSSBridge.cs` (`FrameSize = 1040`) and `FrameInfoV3` in `dx11_vam_dlss.cpp`; a mismatch is
  rejected at run time (`OnRenderEvent` returns if magic, size, version or view count differ).
- In game (simulated headset, `tools/lab/simxr/`): read `opencomposite.log` for `VAM DLSS bridge: first frame capture received`,
  `VAM RR frames=... model=F pathTracing=1`, `VAM PT late composition`, and `dlssg/dlssg.log` for DLSS-G pairs.
- Only a person in a real headset can judge: head-translation follow, menu legibility, hidden-area edges, encoder artefacts.
