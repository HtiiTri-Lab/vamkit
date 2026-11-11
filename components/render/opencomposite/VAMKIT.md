# opencomposite (vamkit additions)

`README.md` beside this file is OpenComposite's own, unchanged. This file describes only what this repository adds: the VaM
compositor work inside OpenComposite's DX11 compositor. Built as `vrclient_x64.dll` and installed as `vamkit/bin/openvr_api.dll`
(VaM loads it as `VaM_Data/Plugins/openvr_api.dll` while the mod runs; the launcher swaps it in and puts VaM's own back). It renders VR
frames for VaM through DLSS: super resolution (K / L / M) or Ray Reconstruction (preset F) with path tracing, the neural
denoiser (NR) before or after the upscaler, NVSharpen, FSR 1 fallback, UI handling, and hands finished frames and guides on to OFXR's
frame generation. Depends on NVIDIA NGX (`ngx-sdk`), Detours, NIS and FSR 1 SDK headers, NVAPI, glm and the OpenXR SDK (see
`manifest/thirdparty.toml`); uses `components/shading/rt-core/include` headers only (not linked).

## Files

All additions live in `OpenOVR/Compositor/` (upstream files are otherwise untouched apart from the build and hook-in lines):

| File | Holds |
|---|---|
| `dx11_vam_dlss.cpp`, `dx11_vam_dlss.h` | The VaM DLSS bridge in the compositor: capture event handling, per-eye guide build from the plugin's depth / motion data, object / hair / rigid / skinned motion redraw, UI rectangles and the FSR 1 UI bypass, SR evaluation (NGX D3D11), RR hand-in, live render scale and native frames, `VaMDLSS_*` exports |
| `vam_rr.inl` | Included inside `VamDlss::Impl`: Ray Reconstruction. `Reconstruct()`, RR textures and guides (`UpdatePathGuides`), path-traced frame hand-over (`GetPathOverlay`, `HandoffPathTarget`, `FinishPathFrame`), tone-mapping and post-processing events on RR's output (`ToneRr`, `PostRr`), detail reference, `SubmitRr` |
| `vam_rr_stereo.inl` | Optional same-frame cross-eye fusion of RR inputs (`[Performance] RRStereo`), a separate shader variant; disabled frames use the original shader |
| `vam_on12.h` | D3D11On12 helpers: handing textures to D3D12 (`UnwrapUnderlyingResource` / wrapped resources), `VamWriteConstants` (DYNAMIC buffers with `WRITE_DISCARD`) |
| `vam_performance_config.h` | `[Performance]` / `[NR]` ini reading (`ReadVamPerformanceSettings`, re-read when the file changes), `VamPathRequested()` (the in-game path tracing switch read from the skin native module) |
| `dx11_nr_prepass.cpp/.h` | Neural denoiser pre-pass (adapted from an MIT-licensed project, notice kept in the file) |
| `dx11_vam_nis.cpp/.h`, `vam_fsr1.hlsl`, `dx11_vam_mip_bias.cpp/.h`, `dx11_vam_ffr.cpp/.h` | NVSharpen, the FSR 1 passes (EASU, RCAS, UI bypass), texture mip bias while DLSS upscales, fixed-foveated rendering (kept off under 11on12) |
| `vam_nr_region.h`, `vam_render_host.h`, `dx11_eye_blit.h`, `dx11_gpu_stage_timer.h`, `dx12_rr_stage_timer.h` | NR region geometry, host start-up order, eye blit, GPU stage timers |
| `../../vam_eye_blit_test.cpp`, `../../tests/vam_nr_region_test.cpp` | Offline tests |

## Build

`python tools/vamkit build opencomposite` runs every step for this component (native library) and writes to
`out\opencomposite\`; the steps are listed in `manifest/components.toml`.

`build.cmd` in this folder: needs `VAMKIT_VCVARS` (path of `vcvars64.bat`) and `VAMKIT_FXC` (the Windows SDK `bin\<version>d` folder holding `fxc.exe`). It
configures into `out\opencompositeuild` with the Visual Studio generator (the instance is derived from the location of `vcvars64.bat`; the Visual Studio
generator is needed because `CMakeLists.txt` references a C# project), builds target `OCOVR` (Release) and copies `vrclient_x64.dll` to
`out\opencomposite\openvr_api.dll`. `CMakeLists.txt` reads the SDKs (NGX, Detours, NIS, FSR 1, NVAPI, glm, OpenXR SDK) from `VAMKIT_THIRD_PARTY`
(default `<repo>	hird_party`) and stops with a message when one is missing. ATL: a Visual Studio with the ATL component needs nothing; otherwise set
`VAMKIT_ATL_ROOT` to the `atlmfc` folder. `libs/vulkan` (Vulkan headers and import library) is upstream OpenComposite content kept in the component because the
Vulkan compositor is compiled in (`SUPPORT_VK`); VaM itself uses D3D11 only. The CMake file compiles the NIS and FSR shaders with fxc into `<build>/vam-shaders/`
(`vam_nis_*.h`, `vam_fsr_*.h`); the RR, guide and tone shaders are embedded as source strings in `vam_rr.inl` and `dx11_vam_dlss.cpp` and compiled at run time.
OpenComposite is built with `/MD` and NGX's `nvsdk_ngx_d.lib`. `python -I components\shading
t-core	oolsuild_composite_shaders.py` (`VAMKIT_FXC`) checks that
every embedded shader of `vam_rr.inl` compiles, without a game run.

## Settings

Run's `performance.ini` (path in `VAM_PERFORMANCE_CONFIG`; re-read when the file changes), `[Performance]` unless noted; the launcher and the in-game menu write them:

| Key | Default | Meaning |
|---|---|---|
| `DLSSPreset` | launcher writes `K` | `Off`, `K`, `L`, `M`: super resolution model; ignored while path tracing is requested (RR F is used at `RenderScaleRR`). `RR` is read as preset 6 |
| `RenderScaleK`, `RenderScaleL`, `RenderScaleM`, `RenderScaleRR` (and the older `RenderScale`) | launcher: 75, 33, 50, 33 | Percent of output, 33 to 100 |
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
  `GetEyeTextureScale`, `GetJitterPhaseCount`, `GetDesktopSize`, `CaptureDesktop`, `GetPostEventFunc`, `GetPostSize`; also `VaMDLSS_Reconstruct`,
  `VaMDLSS_RegisterRr`, `VaMDLSS_WrapToneMapEvent`, `VaMDLSS_InvalidatePathConsumerV1`. Shared layouts (`FrameInfoV2` 1040 bytes, `ObjectDraw` 64 bytes,
  `kDraw*` flags, `kOpaqueDepthEvent`) must equal the plugin's constants.
- Binds by `GetModuleHandleW(L"VaMSkinTransmissionNative.bin")` the skin component's `ST_*PathV1` exports (record, finish, scene revision, overlay, consumer ready,
  UI textures, observed draws): the path tracer is recorded on the 11on12 queue and consumed here by RR.
- Binds the proxy's `D3DInterop_GetOn12*` and, in desktop mode, the presenter's `OFXR_*` provider exports (`d3d11-proxy`), and publishes depth and motion guides for DLSS
  frame generation (`ofxr-dlssg`).
- Environment: `VAM_PERFORMANCE_CONFIG`, `VAM_DLSS_RUNTIME_PATH`, `VAM_DLSS_LOG`, `VAM_NR_PREPASS_PATH`, `VAM_NR_PREPASS_LOG`, `VAM_DISPLAY_MODE`, `D3DINTEROP_MODE`.

## Tests

- `tests/vam_nr_region_test.cpp` (header-only `CalculateVamNrRegion`: region centre from the eye's asymmetric frustum, sizes, clamping); `vam_eye_blit_test.cpp`
  (`D3D11EyeBlitter` on a WARP device; target `vam_eye_blit_test` in `CMakeLists.txt`). Both run offline; the region test has no CMake target in this tree yet, compile it by hand.
- Image quality, ghosting, timing and everything with NGX need a game, the simulated headset or a headset; there is no offline probe for `vam_rr.inl`.

## Invariants

- Keep OpenComposite's and the vendored libraries' licences and headers (`LICENSE.txt`, `LICENSE_*.txt`, the notice in `dx11_nr_prepass.cpp`); this component is GPL-3.0-only.
- Path tracing is one switch in game: this code follows `VamPathRequested()` every frame (on = RR F at `RenderScaleRR` straight from input to output with no SR after it;
  off = `DLSSPreset` at its own scale). Do not add a second path-tracing switch here.
- NGX initialisation order matters (NR retained its order; the D3D12 side is prepared before the producer's own SR features so both eyes start from the same NGX state).
- D3D11On12 costs: textures handed over every frame are created in D3D12 and wrapped; constant buffers written per draw are DYNAMIC with `WRITE_DISCARD`; no per-frame
  unwrap and return of many textures.
- Transparent objects carry no motion or depth of their own into the guides; people's skin, clothing and hair match against either captured depth.

## Read next

[../../../docs/knowledge/frame-path.md](../../../docs/knowledge/frame-path.md), [../../../docs/knowledge/upscaling.md](../../../docs/knowledge/upscaling.md),
[../../../docs/knowledge/denoiser.md](../../../docs/knowledge/denoiser.md), [../../../docs/knowledge/motion-vectors.md](../../../docs/knowledge/motion-vectors.md),
[../../../docs/knowledge/d3d11on12.md](../../../docs/knowledge/d3d11on12.md)
