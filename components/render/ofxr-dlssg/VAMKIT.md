# ofxr-dlssg (vamkit additions)

`README.md` beside this file is OFXR Bridge's own (an OpenXR API layer plus tray that generates in-between frames); it and the
upstream licence, `THIRD_PARTY.md`, `docs/` and `tests/` are kept as they are. This file describes what this fork is for in this repository:
NVIDIA DLSS Frame Generation (2x, 3x, 4x) fed with the depth, motion vectors and camera that the VaM compositor publishes, in VR (as the OpenXR layer) and
on the desktop (the same generator compiled into the presenter of `d3d11-proxy`). Project version `0.7443.0` ("the VaM DLSS Frame Generation fork of OFXR 0.443").
Where DLSS-G cannot take a pair (no guides from the producer) the layer falls back to OFXR's optical flow, frame by frame. Depends on the NGX SDK (`ngx-sdk`),
OpenXR headers (vendored in `external/`) and `openvr-headers`; the DLSS-G runtime `nvngx_dlssg.dll` is loaded at run time, not linked.

## Files (the parts that carry the VaM / DLSS-G work)

| File | Holds |
|---|---|
| `include/xrfg/d3d12_dlssg.hpp`, `src/d3d12/d3d12_dlssg.cpp` | The DLSS-G generator (NGX D3D12, one instance per eye): `dlssg_requested()`, `dlssg_log()`, `dlssg_prepare_session_device()`, record / evaluate, 2x to 4x (`kDlssgMaxOutputs` = 3), the lighter "Preset C" mode (`DLSSG.LiteCandidateBlendingEnabled`), in-place read of B and direct writes into the destinations (v13), GPU scheduling priority control |
| `include/xrfg/dlss_motion_vectors.hpp`, `src/common/dlss_motion_vectors.cpp` | The guide frame DLSS-G needs (depth, motion vectors, camera) |
| `include/xrfg/d3d11_guides.hpp`, `src/layer/d3d11_guides.cpp` | Receives the producer's published guides (`OFXR_PublishD3D11GuidesV3` / `V4`) for the layer |
| `include/xrfg/provider_api.hpp`, `src/layer/provider_api.cpp` | The process-local producer ABI: `OFXR_GetProviderApiVersionV*`, `OFXR_PrepareFrameGenerationV3`, `OFXR_GetFrameMultiplierV1`, control, logging and overlay settings |
| `src/layer/openxr_layer.cpp`, `src/common/implicit_layer.cpp` | The layer; `[ofxr] frame_multiplier` (2 to 4) is followed live; the tray's `triple_frame_gen` is kept for layers that know only that |
| `layer/ofxr_bridge.ini` | Layer settings template |
| `src/tray/` | `OFXRBridgeTray.exe`: arms the layer, registers it for OpenXR, pause event |

## Build

`python tools/vamkit build ofxr-dlssg` runs every step for this component (native library) and writes to
`out\ofxr-dlssg\`; the steps are listed in `manifest/components.toml`.

`build.cmd` in this folder: needs `VAMKIT_VCVARS` (path of `vcvars64.bat`) and `VAMKIT_FXC` (the Windows SDK `bin\<version>d` folder holding `fxc.exe`); CMake and
Ninja come with the Visual Studio Build Tools. It configures into `out\ofxr-dlssguild` with `XRFG_BUILD_TESTS=OFF`, builds target `OFXRBridgeTray`
(the layer DLL is built with it) and copies `OFXRBridgeTray.exe` and the `ofxr\` folder (`XR_APILAYER_XRFrameBridge_diagnostic.dll`,
`OFXR_vulkan_queue_layer.dll`, `ofxr_bridge.ini`) to `out\ofxr-dlssg\`. `XRFG_OPENVR_INCLUDE_DIR` defaults to `third_party\openvr-headers\headers` and
`XRFG_NGX_SDK_ROOT` to `third_party
gx-sdk`. The FidelityFX SDK is not part of the build ("FidelityFX backend disabled": the optical-flow fallback is NVIDIA-only).
Do not distribute PDBs, static libraries, test executables or any NVIDIA binary. Options: `XRFG_BUILD_LAYER`, `XRFG_BUILD_TESTS`, `XRFG_BUILD_STANDALONE`,
`XRFG_BUILD_DLSSG_SMOKE` (GPU smoke test, off). The desktop presenter in `components/render/d3d11-proxy` compiles `src/d3d12/d3d12_dlssg.cpp` itself.

## Settings

`ofxr/ofxr_bridge.ini` (next to the layer DLL; the in-game menu and the launcher write `[ofxr]` keys, the tray rewrites the whole file when its own menu is used):

| Section / key | Default | Meaning |
|---|---|---|
| `[ofxr] frame_multiplier` | launcher: 2 | 2, 3 or 4 frames per game frame (DLSS-G); live, re-read about every 250 ms |
| `[ofxr] triple_frame_gen` | `0` | The tray's 3X switch (older layers) |
| `[overlay] position` | | FPS overlay position |
| `deep_pipeline`, `single_swapchain_rings`, `capture_at_end_frame`, `vulkan_*`, `d3d11_bridge` | see file | Upstream OFXR behaviour, read when the game starts |

Environment (set by the launcher): `OFXR_DLSSG=1`, `OFXR_DLSSG_RUNTIME_PATH` (directory with `nvngx_dlssg.dll`), `OFXR_DLSSG_LOG` (dlssg.log), `OFXR_FRAME_MULTIPLIER=4`, `VAM_PERFORMANCE_CONFIG`
(read live: `[Performance] DLSSGLite` = 1 uses NVIDIA's lighter mode, recreating the features on a change; `[Performance] GpuPriority` = `default` / `high` / `realtime` / `cycle`,
the process's GPU scheduling priority class), `OFXR_DLSSG_LITE` (fallback when no profile), `OFXR_DLSSG_CAPTURE` (directory: bursts of DLSS-G inputs and outputs), `OFXR_DLSSG_COPIES=1` (v12's copies instead of in-place read and write, for comparison).

## Interfaces

- Producer side: the compositor (`opencomposite`) publishes per-eye depth, motion vectors and camera through `OFXR_PublishD3D11GuidesV3`/`V4` and calls
  `OFXR_PrepareFrameGenerationV3` before it brings up its own NGX, so NGX D3D12 starts before the two per-eye super resolution features.
- Desktop: the same exports are implemented by the presenter in `d3d11-proxy` (`desktop_presenter.cpp`), not by the layer.
- The menu (`vr-menu`) reads `ofxr_bridge.ini` and pauses generation through the tray's event `Local\OFXRBridgePause-<n>`.
- DLSS-G keeps its own previous frame: every real frame is fed once and in order; a gap resets it, and a reset pair carries a copy of B shown with B's pose.

## Tests

Upstream's CTest suites (`tests/`: core, layer call chain, D3D11 bridge, history, flight logger, overlay, a layer smoke test) run offline with
`ctest --test-dir build -C Release`; `tests/dlssg_smoke.cpp` (option `XRFG_BUILD_DLSSG_SMOKE`) pushes a synthetic moving bar through guide transfer and NGX DLSS-G and
needs an RTX GPU and the DLSS-G runtime. Real headset and desktop behaviour is shown only by running VaM.

## Invariants

- Keep upstream licences, `THIRD_PARTY.md` and the notices in `src/`; the layer must remain usable by other OpenXR games with the producer API absent (optical-flow path).
- A change to `provider_api.hpp` or the guide structures is an ABI change shared with `opencomposite`, `d3d11-proxy` and `vr-menu`: version it (the exports are suffixed `V3` / `V4`).
- DLSS-G needs each real frame exactly once and in order; do not skip or reorder captured frames.
- The generator in `d3d12_dlssg.cpp` is compiled into two modules (layer and desktop presenter): keep it free of layer-only dependencies.
- Do not distribute NVIDIA binaries; they come from `vamkit fetch`.

## Read next

[../../../docs/knowledge/frame-generation.md](../../../docs/knowledge/frame-generation.md), [../../../docs/knowledge/frame-path.md](../../../docs/knowledge/frame-path.md)
