# Upscalers and frame generation from Intel and AMD (XeSS, FSR)

DLSS needs an NVIDIA RTX card, and its frame generation needs an RTX 40 (2x) or RTX 50 (up to 4x). Intel's XeSS and AMD's
FSR are not tied to their makers' cards the way DLSS is: XeSS super resolution runs on any GPU with shader model 6.4, and
AMD's FSR API runs FSR 4 on recent Radeons and falls back to the analytical FSR 3.1 on everything else, NVIDIA and Intel
included. This document records what each technique runs on, what it needs from a frame, which of them fit this
project's two output paths (per-eye images handed to an OpenXR runtime in VR; our own presenter on the desktop), which
community projects show that it works, and how the files may be obtained. State of November 2026; every claim names
its source at the end. How the backends are wired into the compositor is in [upscaling.md](upscaling.md) and
[frame-generation.md](frame-generation.md).

Nothing in this document was run on an AMD or Intel GPU: the development machine has an RTX card, so everything about
other vendors' hardware is from the vendors' documents.

## How it works

### What runs where

| Technique | NVIDIA RTX 50 / 40 / 30 / 20 | NVIDIA GTX | AMD Radeon | Intel Arc | Needs |
|---|---|---|---|---|---|
| DLSS SR K / L / M, RR, NR | yes | no | no | no | RTX, NGX runtimes |
| DLSS Frame Generation | 50: 2-4x, 40: 2x, 30 / 20: no | no | no | no | RTX 40 or newer |
| XeSS super resolution 3.0 | yes (cross-vendor kernel) | not stated by Intel | yes (cross-vendor kernel) | yes (Intel's own kernel) | D3D12, shader model 6.4 with hardware DP4a |
| XeSS frame generation | 2x | not stated | 2x | 2x-4x | D3D12, its own proxy swap chain, XeLL |
| FSR 4.1 upscaling (ML) | no (the same DLL runs FSR 3.1.6) | no (FSR 3.1.6) | RX 9000, RX 7000, RX 6000 discrete, some Ryzen integrated | no (FSR 3.1.6) | AMD's signed DLLs, shader model 6.6 |
| FSR 3.1.6 upscaling (analytical) | yes | yes | yes | yes | D3D12 or Vulkan, shader model 6.2, typed UAV load, R16G16B16A16_UNORM |
| FSR 3.1.6 frame generation (analytical) | 2x | 2x | 2x | 2x | as above |
| FSR 4.0 frame generation (ML) | no | no | RX 9000 or later | no | Windows 11, shader model 6.6 |

"XeSS is Intel's and FSR is AMD's" is true of who makes them, not of where they run. The two that are vendor-locked are
the ML variants of FSR (Radeon only; AMD's API picks them by hardware and otherwise selects FSR 3.1.6 by itself) and the
multi-frame mode of XeSS frame generation (Arc only).

**XeSS super resolution** (SDK 3.0.2, July 2026). One library, `libxess.dll`, for D3D12 and Vulkan; a D3D11 library
exists but returns "unsupported device" on anything except Intel Arc. Intel cards run a kernel built for their matrix
hardware; every other card runs an HLSL implementation that needs shader model 6.4 and hardware-accelerated DP4a. It is a
handle-based API: create a context from a D3D12 device, initialise it for an output size and a quality setting, then
`xessD3D12Execute` records its work into a command list the application owns. Quality settings and their per-axis
ratios: Native AA 1.0, Ultra Quality Plus 1.3, Ultra Quality 1.5, Quality 1.7, Balanced 2.0, Performance 2.3, Ultra
Performance 3.0. The input may differ from a setting's own size inside a range the library reports
(`xessGetOptimalInputResolution`).

**FSR through AMD's FSR API** (SDK 2.4.0 "Redstone", October 2026). The API is five functions (`ffxCreateContext`,
`ffxDestroyContext`, `ffxConfigure`, `ffxQuery`, `ffxDispatch`) in a small loader DLL
(`amd_fidelityfx_loader_dx12.dll`) that loads one DLL per effect (`amd_fidelityfx_upscaler_dx12.dll`,
`amd_fidelityfx_framegeneration_dx12.dll`). The upscaler DLL holds FSR 4.1.2, FSR 3.1.6 and FSR 2.3.4; which one runs is
chosen at context creation: the newest the hardware supports, or a newer one supplied by the Radeon driver.
`ffxQueryDescGetVersions` lists them and `ffxOverrideVersion` pins one. Quality ratios: Native AA 1.0, Quality 1.5,
Balanced 1.7, Performance 2.0, Ultra Performance 3.0; any ratio in between is accepted (the render size is a dispatch
parameter).

### What they need from a frame

All three temporal upscalers take the same things DLSS SR takes, which the compositor already builds per eye
([motion-vectors.md](motion-vectors.md)):

| Input | Compositor has | XeSS | FSR |
|---|---|---|---|
| Colour at render size, jittered | 8-bit sRGB-encoded or RGBA16F eye image | `XESS_INIT_FLAG_LDR_INPUT_COLOR` for the 8-bit image; UNORM only for integer formats; output in the input's format | `FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE` for the float image; `FFX_UPSCALE_ENABLE_NON_LINEAR_COLORSPACE` otherwise |
| Depth, reversed Z | R32F | `XESS_INIT_FLAG_INVERTED_DEPTH`; needed with render-size motion | `FFX_UPSCALE_ENABLE_DEPTH_INVERTED`, plus camera near, far and vertical field of view |
| Motion, render size, pixels, current to previous, jitter removed | RG32F / RG16F | the default convention (no flag); `R16G16_FLOAT` documented | the default (no display-resolution flag, no jitter-cancellation flag); `motionVectorScale` 1, 1 for pixels |
| Jitter of the frame | pixels, the bridge's Halton sequence | `jitterOffsetX/Y` in [-0.5, 0.5] | `jitterOffset`, same range |
| Exposure | none (pre-exposed image) | scale 1, or `XESS_INIT_FLAG_ENABLE_AUTOEXPOSURE` | `FFX_UPSCALE_ENABLE_AUTO_EXPOSURE` |
| Reactive / responsive mask | the hair mask, optional | optional | optional (FSR 3.1: recommended; FSR 4: not needed) |
| Reset on a camera cut | `guides.reset` | `resetHistory` | `reset` |
| Resource states (D3D12) | textures unwrapped from the 11on12 device | inputs NON_PIXEL_SHADER_RESOURCE, output UNORDERED_ACCESS | inputs compute-read, output unordered access |

Both libraries record into a D3D12 command list, which is what the compositor's hand-over already does for NGX
(`EvaluateOn12` in `components/render/opencomposite/OpenOVR/Compositor/dx11_vam_dlss.cpp`; [d3d11on12.md](d3d11on12.md)).
Neither has a usable D3D11 path for a non-Intel card, so both exist only with the proxy (D3D11On12), which is how the mod
always runs. Neither document mentions stereo; both are per-context, and one context per eye is the arrangement DLSS
already uses here.

### Frame generation: which fits which output path

Frame generation in VR has no swap chain: each eye's image goes to the OpenXR runtime, and generated frames are written
into the layer's own staging images (`components/render/ofxr-dlssg`). On the desktop the presenter in
`components/render/d3d11-proxy` owns the swap chain, dispatches the generator itself and paces the presents.

| | VR (per eye, no swap chain) | Desktop (our presenter) |
|---|---|---|
| DLSS-G | in use | in use |
| FSR frame generation | fits: `ffxDispatchDescFrameGeneration` takes a colour image, a command list and `outputs[]`, textures the caller owns; the flag `FFX_FRAMEGENERATION_FLAG_NO_SWAPCHAIN_CONTEXT_NOTIFY` tells the context there is no FSR swap chain. Depth, motion and camera go in through `ffxDispatchDescFrameGenerationPrepareV2`. One generated frame per pair (2x) | fits the same way: the same generator source is compiled into the presenter, as with DLSS-G |
| XeSS frame generation | does not fit: the only entry points wrap a DXGI swap chain (`xefgSwapChainD3D12InitFromSwapChain`, `...InitFromSwapChainDesc`); no call writes an interpolated frame into a texture; one proxy swap chain per process | possible only by handing the swap chain and the pacing to XeSS (it presents on its own thread and queue) and integrating XeLL, which it requires on every GPU; it cannot coexist with the presenter's own pacing or with DLSS-G. 2x only on non-Intel cards |

AMD's documentation calls dispatching frame generation by hand "possible, but highly discouraged" and builds its samples
around its own swap chain. Three things show that the manual route works: the dispatch structure itself (it names the
output textures), the SDK 1.1.4 source (its dispatch never touches a swap chain and writes `outputs[0]`), and existing
users (below). Rules that come with it: `frameID` rises by exactly one per real frame or the history resets; the
generation rectangle is the upscaler's output size; the call order per frame is configure, prepare, generate; since FSR
3.1.0 the generator is independent of FSR upscaling, so it can follow DLSS or XeSS.

Frame generation by FSR makes one frame per pair. The dispatch structure has room for four outputs, but SDK 1.1.4 reads
only the first and the 2.4.0 documents speak of one intermediate frame. So 3x and 4x remain DLSS-G on RTX 50 (and XeSS on
Arc, on a flat screen).

### What path tracing and neural rendering mean for a card without RTX

Path tracing shows its result through Ray Reconstruction, which is an NGX feature: without an RTX card the tracer has no
reconstruction and the compositor switches path tracing off again on the first failed frame (`ST_SetPathTracingV1(0)`,
[upscaling.md](upscaling.md)). Neural rendering (NR) is NGX too. A card without RTX therefore gets the raster route
with XeSS or FSR, NVSharpen (a plain shader), and FSR frame generation; not path tracing, not NR.

### Community projects and what they show

| Project | Licence | What it is | What it shows for this project |
|---|---|---|---|
| OptiScaler | GPL-3.0 | Replaces a game's DLSS / FSR / XeSS calls with another upscaler and can add frame generation ("OptiFG": FSR 3 or XeSS frame generation in games that have none) | XeSS and FSR 2 / 3 / 4 run in D3D11 games through a D3D12 device on D3D11On12, the arrangement this mod already has; FSR and XeSS upscaling work on RTX 30 / 40; its release notes state that official FSR 4 covers Radeon only and that other cards fall back to FSR 3 |
| dlssg-to-fsr3 | GPL-3.0 | Replaces DLSS-G in games that have it with FSR 3 frame generation | FSR frame interpolation can be driven by exactly the inputs DLSS-G gets (colour, depth, motion, camera), on RTX 20 / 30 |
| OFXR Bridge (upstream of `components/render/ofxr-dlssg`) | upstream: GPL-3.0 for the combined distribution; the 0.2.x line this fork comes from: LGPL-3.0-or-later | OpenXR frame generation layer | Upstream now has an "FSR 3.1" route: FSR frame generation in VR from captured DLSS guides, per eye, with no swap chain; also AMD's FidelityFX optical flow as a second optical-flow backend for cards without NVIDIA optical flow |
| FSR3Unity | MIT | FSR 3.1 upscaling rewritten for Unity's built-in pipeline (C# and compute shaders, D3D11) | A D3D11 FSR 3.1 exists outside AMD's SDK; it needs Unity 2020.1 or later and has no frame generation, so it is of no use for VaM's Unity 2018 |
| Builds of FSR 4 from the source AMD published by mistake in August 2025 (the INT8 model) | unclear | FSR 4 on RX 7000 and on RTX 30 / 40 before AMD supported the former | Not used and not to be used: the source was withdrawn, its licence status is disputed, and AMD's own INT8 path (RX 7000 since SDK 2.3.0, RX 6000 since 2.4.0) checks for Radeon hardware. There is no legitimate FSR 4 for NVIDIA or Intel cards |

In VR specifically: a Skyrim VR upscaler mod runs DLSS, FSR 2 and XeSS per eye (its author advises against XeSS there
and users report per-eye differences); flight simulators run DLSS per eye natively; vrperfkit and OpenXR Toolkit only do
spatial scaling (FSR 1, NIS, CAS). Frame generation in VR from FSR exists only in OFXR Bridge. No statement by AMD or
Intel for or against VR use was found.

Nothing is copied from these projects. The FSR and XeSS code in this repository is written against the vendors' public
headers.

### Licences and how the files are obtained

| Item | Licence | Source available | Obtained by |
|---|---|---|---|
| XeSS SDK 3.0.2: `libxess.dll`, headers | Intel Simplified Software License (October 2022): binary only, may be redistributed unmodified with the notice, no reverse engineering, no modification "including during execution". The super resolution headers (`xess.h`, `xess_d3d12.h`) are MIT in the repository's current state (the manifest pins that commit); at the 3.0.2 tag they still carried Intel's proprietary notice, as the frame generation header does | no | `vamkit fetch` from Intel's GitHub repository at a pinned commit; never committed |
| AMD FSR SDK 2.4.0: API headers, signed DLLs | The SDK's licence file (`license.md` in its docs folder) has two parts: a binary-only licence for everything not listed, and MIT for a list of paths that includes `Kits\FidelityFX\` (API, upscalers, frame generation headers) and the signed binaries. AMD requires applications to use the signed DLLs | headers and some shaders only; FSR 4 and the DLLs are prebuilt | `vamkit fetch` from AMD's GitHub repository at a pinned commit; never committed |
| AMD FidelityFX SDK 1.1.4 (FSR 3.1.4, FSR 2.3.3, optical flow) | MIT | yes, complete; the last release that is | not needed while the signed DLLs are used; it is what the layer's optional FidelityFX optical-flow backend would be built from |

The repository redistributes none of these, like the NVIDIA runtimes ([../agent/runtimes.md](../agent/runtimes.md)).
OpenComposite is GPL-3.0; it loads both libraries at run time through their C interfaces and links neither. The licence
texts were read through a fetch tool: read the files themselves before redistributing a binary.

## Settings

The keys this adds are listed where they are read: [upscaling.md](upscaling.md) (`DLSSPreset` values `XeSS` and `FSR`,
`RenderScaleXeSS`, `RenderScaleFSR`), [frame-generation.md](frame-generation.md) (`FrameGenModel`),
[settings-and-menu.md](settings-and-menu.md).

## Measured

Vendor figures only, none from this project:

| What | Figure | Source |
|---|---|---|
| FSR 4.1.2 upscaling, 4K output | RX 9070 XT 1.3 ms, RX 9060 XT 3.4 ms, RX 7900 XTX 3.1 ms (INT8), RX 6900 XT 3.2 ms (INT8) | AMD, FSR 4 technique page |
| FSR 4 video memory | 81 MB at 1080p, 318 MB at 4K, per context | same |
| FSR 3.1.6 frame generation video memory | 124 MB at 1080p, 457 MB at 4K, per context, without a swap chain | AMD, frame generation technique page |
| XeSS on non-Intel cards against Intel's own kernel | no figure published | |

Two contexts (one per eye) double the per-context memory. What the backends cost on the development machine is in
[upscaling.md](upscaling.md) and [frame-generation.md](frame-generation.md).

## Pitfalls

- **"FSR 4 on an RTX card"** in forum posts means the leaked INT8 build. AMD's API on an NVIDIA or Intel card runs FSR
  3.1.6, and says so through `ffxQueryDescGetVersions`. Report the version that runs, not "FSR 4".
- **The FSR API must come from AMD's signed DLLs.** A self-built `amd_fidelityfx_*.dll` from SDK 1.1.4 has FSR 3.1.4 and
  no FSR 4, and the Radeon driver upgrades only the signed ones.
- **XeSS's D3D11 library is Arc-only.** On other cards it must run on D3D12, here on the 11on12 queue.
- **XeSS must not be patched or hooked while it runs**: the licence forbids altering its operation during execution.
- **XeSS frame generation cannot share a swap chain** with another generator or run twice in a process, and has no
  entry point without a swap chain: VR has none to give it.
- **FSR frame generation resets** whenever `frameID` skips; a real frame that goes another way (menus, a pause) must
  restart the sequence, as with DLSS-G's history.
- **The two eyes are two contexts.** Per-context memory doubles, and nothing constrains the eyes to agree; the Skyrim VR
  reports of per-eye differences with temporal upscalers are this.
- **Driver-supplied FSR versions.** A Radeon driver may hand the API a newer FSR than the SDK's; quality and cost numbers
  measured with one version do not carry over. `ffxOverrideVersion` pins the SDK's own.

## Tried and dropped

- **XeSS frame generation** is not built: VR is impossible (swap chain only); on the desktop it can wrap the
  presenter's swap chain, takes over the pacing and needs XeLL. What that takes is written out in
  [frame-generation.md](frame-generation.md), "Not built here"; Arc owners are the ones who would gain (more than 2x
  on a flat screen).
- **FSR 4 for NVIDIA cards from the leaked source**: not legitimate, see above.
- **Building FSR from the open SDK 1.1.4** instead of using the signed DLLs: FSR 3.1.4 only, a 450 MB download and a
  CMake build with its own shader compiler, and Radeon owners would lose FSR 4.
- **FSR 3 inside Unity (FSR3Unity) or a native D3D11 FSR**: the first needs a newer Unity; the second exists only as
  community ports of FSR 2.2 / 3.1.2. The 11on12 queue makes both unnecessary.
- **FSR 2.x and FSR 1 as selectable upscalers**: FSR 3.1 replaces 2.x on every card that ran it; FSR 1 stays the
  stand-in for a frame the temporal upscaler cannot take.

## Open

- Nothing here has run on a Radeon or an Arc card. Unknown from documents alone: whether NGX and NVAPI fail quietly on
  them everywhere the mod calls them, whether XeSS's cross-vendor kernel accepts the 8-bit eye image the same way on all
  three vendors, and what the FSR API selects for the frame generation context on a Radeon RX 9000 when it is dispatched
  without AMD's swap chain (the ML generator is documented with it).
- Intel publishes no quality or cost comparison between its own kernel and the cross-vendor one.
- Whether the FSR API accepts more than one generated frame per pair in 2.4.0 was not confirmed; it is treated as one.
- Temporal upscalers at VR's low input sizes (33-50% per axis) are outside the vendors' flat-screen design points for
  XeSS and FSR as for DLSS ([upscaling.md](upscaling.md)).
- The licence of the OFXR fork's upstream changed (upstream now states GPL-3.0 for its combined distribution); the fork
  here descends from the LGPL line and takes no code from the newer upstream.

## Verify

- Which FSR version runs: the compositor logs the provider's version string at context creation.
- Which XeSS kernel runs: `xessGetIntelXeFXVersion` returns 0.0.0 off Intel hardware; `xessIsOptimalDriver` reports an
  old driver.
- A session with `DLSSPreset=XeSS` or `FSR` in the run's `performance.ini` on the desktop or the simulated headset shows
  the feature being created per eye and the evaluations counted, in the compositor's log
  ([../protocol/testing.md](../protocol/testing.md)).
- The path a card without RTX takes, on a card that has it: `VAMKIT_NO_DLSS=1` in the environment of the script that
  starts the session makes NGX refuse in the compositor, the NR pass and the frame generation layer. Expect in the
  logs: the run still holds `DLSSPreset=K` and `FrameGenModel=DLSS` (the launcher replaces nothing),
  `VAM upscalers: DLSS SR cannot run here`, `VAM upscaler: XeSS (the one chosen cannot run here)`,
  `DLSS frame generation cannot be made here ...; trying the other generator`, `Frame generation model: FSR`, the
  menu's `upscalers that can run here: XeSS FSR; path tracing: the row says why it will not start`. With
  `VAMKIT_NO_NVOF=1` as well, in VR: `NVIDIA optical flow is not available here ...: this swapchain runs on FSR
  alone`. With `VAMKIT_AS_VENDOR=1002`: `FSR stands in first`. None of this tests another vendor's driver.
- Only a person on a Radeon or Arc card can confirm the claims in "What runs where" for those cards.

## Sources

- Intel XeSS: <https://github.com/intel/xess> (releases, `LICENSE.txt`, `inc/xess/xess.h`, `inc/xess/xess_d3d12.h`,
  `inc/xess_fg/xefg_swapchain.h`, `doc/xess_sr_developer_guide_english.md`, `doc/xess_fg_developer_guide_english.md`,
  `doc/xell_developer_guide_english.md`).
- AMD FSR SDK: <https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK> (releases v1.1.4 to v2.4.0,
  `license.md` in its docs folder, `Kits/FidelityFX/docs/getting-started/ffx-api.md`, `.../docs/techniques/super-resolution-ml.md`,
  `super-resolution-upscaler.md`, `frame-interpolation.md`, `frame-interpolation-api.md`, `frame-interpolation-ml.md`,
  `frame-interpolation-swap-chain.md`, `Kits/FidelityFX/signedbin`, tag v1.1.4 `ffx-api/include/ffx_api/ffx_framegeneration.h`
  and `ffx-api/src/ffx_provider_framegeneration.cpp`); <https://gpuopen.com/fidelityfx-superresolution-4/>.
- OptiScaler: <https://github.com/optiscaler/OptiScaler> (README, releases, wiki pages "OptiFG", "Frame Generation
  Options", "FSR4 Compatibility List").
- dlssg-to-fsr3: <https://github.com/Nukem9/dlssg-to-fsr3>.
- OFXR Bridge: <https://github.com/tig3rmast3r/OFXR-Bridge>, <https://github.com/djules75/OFXR-Bridge>.
- FSR3Unity: <https://github.com/ndepoel/FSR3Unity>.
- The FSR 4 source incident and the later official INT8 support, secondary sources:
  <https://www.techspot.com/news/109164-amd-accidentally-leaks-fsr-4-source-code-teasing.html>,
  <https://www.tomshardware.com/pc-components/gpus/latest-fsr-4-source-code-leak-lets-you-run-amds-ai-upscaling-tech-on-nearly-any-gpu-no-linux-required>,
  <https://www.tomshardware.com/pc-components/gpu-drivers/amd-brings-official-fsr-4-1-support-to-rx-7000-series-gpus-int8-model-now-available-in-300-games-rdna-3-apus-also-getting-fsr-4-1-soon>.
- VR precedents, secondary sources: <https://www.nexusmods.com/skyrimspecialedition/mods/82179>,
  <https://forum.dcs.world/topic/346341-vr-upscaling>, <https://github.com/fholger/vrperfkit>,
  <https://mbucchia.github.io/OpenXR-Toolkit/upscaling>.
