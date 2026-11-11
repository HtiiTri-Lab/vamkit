# opencomposite (vamkit additions)

`README.md` beside this file is OpenComposite's own, unchanged. This file describes only what this repository adds: the VaM
compositor work inside OpenComposite's DX11 compositor. Built as `vrclient_x64.dll` and installed as `vamkit/bin/openvr_api.dll`
(VaM loads it as `VaM_Data/Plugins/openvr_api.dll` while the mod runs; the launcher swaps it in and puts VaM's own back). It renders VR
frames for VaM through a temporal upscaler: DLSS super resolution (K / L / M), Intel XeSS or AMD FSR on the raster route, Ray
Reconstruction (preset F) with path tracing; then the neural denoiser (NR) before or after the upscaler, NVSharpen, FSR 1 fallback,
UI handling, and hands finished frames and guides on to OFXR's frame generation. Depends on NVIDIA NGX (`ngx-sdk`), Detours, NIS and
FSR 1 SDK headers, NVAPI, glm, the OpenXR SDK and the headers of XeSS (`xess-sdk`) and of AMD's FSR API (`fsr-sdk`) (see
`manifest/thirdparty.toml`); uses `components/shading/rt-core/include` headers only (not linked). XeSS's and FSR's libraries are
loaded at run time and not linked.

## Files

All additions live in `OpenOVR/Compositor/` (upstream files are otherwise untouched apart from the build and hook-in lines):

| File | Holds |
|---|---|
| `dx11_vam_dlss.cpp`, `dx11_vam_dlss.h` | The VaM DLSS bridge in the compositor: capture event handling, per-eye guide build from the plugin's depth / motion data, object / hair / rigid / skinned motion redraw, UI rectangles and the FSR 1 UI bypass, SR evaluation (NGX D3D11), RR hand-in, live render scale and native frames, `VaMDLSS_*` exports |
| `vam_rr.inl` | Included inside `VamDlss::Impl`: Ray Reconstruction. `Reconstruct()`, RR textures and guides (`UpdatePathGuides`), path-traced frame hand-over (`GetPathOverlay`, `HandoffPathTarget`, `FinishPathFrame`), tone-mapping and post-processing events on RR's output (`ToneRr`, `PostRr`), detail reference, `SubmitRr` |
| `vam_rr_stereo.inl` | Optional same-frame cross-eye fusion of RR inputs (`[Performance] RRStereo`), a separate shader variant; disabled frames use the original shader |
| `vam_sr_backends.h` | Run-time bindings to Intel XeSS (`libxess.dll`) and AMD's FSR API (`amd_fidelityfx_loader_dx12.dll`): loading from `VAM_XESS_RUNTIME_PATH` / `VAM_FSR_RUNTIME_PATH`, version queries, error text, XeSS's quality setting for a ratio. Their use is in `dx11_vam_dlss.cpp`: `ProbeUpscalers`, `CreateXess` / `CreateFsr`, `EvaluateXess` / `EvaluateFsr`, `DropUpscaler` |
| `vam_on12.h` | D3D11On12 helpers: handing textures to D3D12 (`UnwrapUnderlyingResource` / wrapped resources), `VamWriteConstants` (DYNAMIC buffers with `WRITE_DISCARD`) |
| `vam_performance_config.h` | `[Performance]` / `[NR]` ini reading (`ReadVamPerformanceSettings`, re-read when the file changes), `VamPathRequested()` (the in-game path tracing switch read from the skin native module), the upscaler choice (`kVamSr*`, `VamSrChoice`: the chosen one or the one that stands in, `VamSrAvailable`) |
| `dx11_nr_prepass.cpp/.h` | Neural denoiser pre-pass (adapted from an MIT-licensed project, notice kept in the file) |
| `dx11_vam_nis.cpp/.h`, `vam_fsr1.hlsl`, `dx11_vam_mip_bias.cpp/.h`, `dx11_vam_ffr.cpp/.h` | NVSharpen, the FSR 1 passes (EASU, RCAS, UI bypass), texture mip bias while DLSS upscales, fixed-foveated rendering (kept off under 11on12) |
| `vam_nr_region.h`, `vam_render_host.h`, `dx11_eye_blit.h`, `dx11_gpu_stage_timer.h`, `dx12_rr_stage_timer.h` | NR region geometry, host start-up order, eye blit, GPU stage timers |
| `../../vam_eye_blit_test.cpp`, `../../tests/vam_nr_region_test.cpp` | Offline tests |

## Build

`python tools/vamkit build opencomposite` runs every step for this component (native library) and writes to
`out\opencomposite\`; the steps are listed in `manifest/components.toml`.

`build.cmd` in this folder: needs `VAMKIT_VCVARS` (path of `vcvars64.bat`) and `VAMKIT_FXC` (the Windows SDK `bin\<version>d` folder holding `fxc.exe`). It
configures into `out\opencomposite\build` with the Visual Studio generator (the instance is derived from the location of `vcvars64.bat`; the Visual Studio
generator is needed because `CMakeLists.txt` references a C# project), builds target `OCOVR` (Release) and copies `vrclient_x64.dll` to
`out\opencomposite\openvr_api.dll`. `CMakeLists.txt` reads the SDKs (NGX, Detours, NIS, FSR 1, NVAPI, glm, OpenXR SDK, the XeSS and FSR API headers) from `VAMKIT_THIRD_PARTY`
(default `<repo>\third_party`) and stops with a message when one is missing. ATL: a Visual Studio with the ATL component needs nothing; otherwise set
`VAMKIT_ATL_ROOT` to the `atlmfc` folder. `libs/vulkan` (Vulkan headers and import library) is upstream OpenComposite content kept in the component because the
Vulkan compositor is compiled in (`SUPPORT_VK`); VaM itself uses D3D11 only. The CMake file compiles the NIS and FSR shaders with fxc into `<build>/vam-shaders/`
(`vam_nis_*.h`, `vam_fsr_*.h`); the RR, guide and tone shaders are embedded as source strings in `vam_rr.inl` and `dx11_vam_dlss.cpp` and compiled at run time.
OpenComposite is built with `/MD` and NGX's `nvsdk_ngx_d.lib`. `python -I components\shading\rt-core\tools\build_composite_shaders.py` (`VAMKIT_FXC`) checks that
every embedded shader of `vam_rr.inl` compiles, without a game run.

## Settings

Run's `performance.ini` (path in `VAM_PERFORMANCE_CONFIG`; re-read when the file changes), `[Performance]` unless noted; the launcher and the in-game menu write them:

| Key | Default | Meaning |
|---|---|---|
| `DLSSPreset` | launcher writes `K` (a model the machine has: FSR or XeSS without an RTX card) | `Off`, `K`, `L`, `M` (DLSS super resolution models), `XeSS`, `FSR`: the raster route's upscaler; ignored while path tracing is requested (RR F is used at `RenderScaleRR`). `RR` is read as K. A model that cannot run here is replaced by `VamSrChoice` |
| `RenderScale` (what the renderer reads) and `RenderScaleK`, `L`, `M`, `XeSS`, `FSR`, `RR` (what each model remembers; the menu copies) | launcher: 75, 33, 50, 67, 67, 33 | Percent of output, 33 to 100 |
| `OutputScale` | 100 | 75 to 150 (swap chains follow) |
| `DLSSSharpness`, `DLSSSharpenLast` | 25, 1 | NVSharpen strength; position after SR or after post-SR NR |
| `DLSSMipBias` | 50 | Percent of the DLSS-guide texture mip bias (100 made thin seams shimmer) |
| `DLSSMenuNative` | 1 | Native-resolution, no-SR frames while VaM's own menus are open |
| `NRAfterUpscale`, `NRAfterScale` | 0, 100 | Run the denoiser after the upscaler, and at what share of the output |
| `RRStereo` | 0 | Cross-eye RR input fusion, 0 to 100 (VR + path tracing) |
| `RRTransparentGuides`, `PTReference` | 1, 0 | Transparent-layer RR guides; path-traced reference accumulation view (also Ctrl+Alt+R) |
| `DLSSObjectMotion`, `DLSSHairMotion`, `DLSSRigidMotion`, `DLSSHairBias` | 1, 1, 1, 0 | Motion sources and hair bias mask |
| `[NR] ...` | | Denoiser keys (`RegionShiftY`, `FoveationSize`, `Style`, `Intensity`, ...) |

## Interfaces

- Exports of `openvr_api.dll` bound by `components/render/dlss-bridge`: `VaMDLSS_GetApiVersion`, `GetRenderEventFunc`, `GetState`, `GetRenderScalePercent`,
  `GetEyeTextureScale`, `GetJitterPhaseCount`, `GetDesktopSize`, `CaptureDesktop`, `CaptureGenerated`, `GetPostEventFunc`, `GetPostSize`; also `VaMDLSS_Reconstruct`,
  `VaMDLSS_RegisterRr`, `VaMDLSS_WrapToneMapEvent`, `VaMDLSS_InvalidatePathConsumerV1`. Shared layouts (`FrameInfoV2` 1040 bytes, `ObjectDraw` 64 bytes,
  `kDraw*` flags, `kOpaqueDepthEvent`) must equal the plugin's constants.
- `VaMDLSS_GetUpscalerSupport` (bound by `components/render/vr-menu`): the upscalers this machine can run, bit 0 DLSS SR, bit 1 XeSS, bit 2 FSR, bit 3 NGX
  came up (Ray Reconstruction can be tried; without it the menu's path tracing row says what is missing), bit 4 FSR stands in before XeSS (an AMD
  card; `VamSrChoice`), bit 8 once the compositor has tried them (with its first frame).
- `running.json` in the folder that `VAM_DLSS_LOG` names says what upscaler was asked for, what runs and why they differ, what can run, and the same for path
  tracing (`WriteRunning`). Tools read this file; the wording of the log lines `VAM DLSS SR feature created`, `VAM XeSS feature created`,
  `VAM FSR feature created` is also read by `tools/lab` and must not change without telling them.
- XeSS and FSR record on the 11on12 command ring (`on12Commands`); `ProbeUpscalers` brings it up itself (`CommandRing`), because on a card without
  NGX nothing else does. Run a session with `VAMKIT_NO_DLSS=1` after any change to the upscaler or NR code: it is the only test here of that path.
- Loads `libxess.dll` and `amd_fidelityfx_loader_dx12.dll` (which loads `amd_fidelityfx_upscaler_dx12.dll` beside it) from the folders the launcher names;
  both record into a command list on the 11on12 queue, so both need the proxy.
- Binds by `GetModuleHandleW(L"VaMSkinTransmissionNative.bin")` the skin component's `ST_*PathV1` exports (record, finish, scene revision, overlay, consumer ready,
  UI textures, observed draws): the path tracer is recorded on the 11on12 queue and consumed here by RR.
- Binds the proxy's `D3DInterop_GetOn12*` and, in desktop mode, the presenter's `OFXR_*` provider exports (`d3d11-proxy`), and publishes depth and motion guides for DLSS
  frame generation (`ofxr-dlssg`).
- Environment: `VAM_PERFORMANCE_CONFIG`, `VAM_DLSS_RUNTIME_PATH`, `VAM_XESS_RUNTIME_PATH`, `VAM_FSR_RUNTIME_PATH`, `VAM_DLSS_LOG`, `VAM_NR_PREPASS_PATH`,
  `VAM_NR_PREPASS_LOG`, `VAM_DISPLAY_MODE`, `D3DINTEROP_MODE`; `VAMKIT_NO_DLSS=1` (a test: NGX refuses, as on a card without RTX, so XeSS or FSR upscale and
  path tracing switches itself off).

## Tests

- `tests/vam_nr_region_test.cpp` (header-only `CalculateVamNrRegion`: region centre from the eye's asymmetric frustum, sizes, clamping); `vam_eye_blit_test.cpp`
  (`D3D11EyeBlitter` on a WARP device; target `vam_eye_blit_test` in `CMakeLists.txt`). Both run offline; the region test has no CMake target in this tree yet, compile it by hand.
- Image quality, ghosting, timing and everything with NGX need a game, the simulated headset or a headset; there is no offline probe for `vam_rr.inl`.

## Invariants

- Keep OpenComposite's and the vendored libraries' licences and headers (`LICENSE.txt`, `LICENSE_*.txt`, the notice in `dx11_nr_prepass.cpp`); this component is GPL-3.0-only.
- Path tracing is one switch in game: this code follows `VamPathRequested()` every frame (on = RR F at `RenderScaleRR` straight from input to output with no SR after it;
  off = `DLSSPreset` at its own scale). Do not add a second path-tracing switch here.
- NGX initialisation order matters (NR retained its order; the D3D12 side is prepared before the producer's own SR features so both eyes start from the same NGX state).
- The three upscalers share one path: the same per-eye textures, guides, mip bias, NR and sharpening, and one rule for which runs (`VamSrChoice`). A fourth one is a
  backend there, not a second pipeline. XeSS and FSR are never linked, never redistributed, and XeSS's library is never hooked or patched (its licence forbids
  altering its operation while it runs).
- D3D11On12 costs: textures handed over every frame are created in D3D12 and wrapped; constant buffers written per draw are DYNAMIC with `WRITE_DISCARD`; no per-frame
  unwrap and return of many textures.
- Transparent objects carry no motion or depth of their own into the guides; people's skin, clothing and hair match against either captured depth.

## Read next

[../../../docs/knowledge/frame-path.md](../../../docs/knowledge/frame-path.md), [../../../docs/knowledge/upscaling.md](../../../docs/knowledge/upscaling.md),
[../../../docs/knowledge/denoiser.md](../../../docs/knowledge/denoiser.md), [../../../docs/knowledge/motion-vectors.md](../../../docs/knowledge/motion-vectors.md),
[../../../docs/knowledge/d3d11on12.md](../../../docs/knowledge/d3d11on12.md)
