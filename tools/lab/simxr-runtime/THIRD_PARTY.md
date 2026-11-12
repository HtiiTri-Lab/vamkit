# Third-party code in the simulated headset

The simulated headset is a build of VirtualDesktop-OpenXR with a `SIMXR` define and a stand-in headset library grown
from that project's `OVRNull`. This file lists every piece the build compiles or links: where it comes from, which
version, its licence, what was changed, and whether it is in this folder or taken from `<repo>/third_party/`.

## In this folder

| What | Where | Origin and version | Licence | Changes |
|---|---|---|---|---|
| VirtualDesktop-OpenXR runtime | `runtime/` | https://github.com/mbucchia/VirtualDesktop-OpenXR, branch `dlssnr`, commit `925dc598c524cb48b37fa4b0a2bff471f75fc5c4`, folder `virtualdesktop-openxr` (version 1.1.0) | MIT (`LICENSE`, Copyright (c) 2022-2024 Matthieu Bucchianeri; `accessibility.*` also Copyright (c) 2025 Microsoft Corp.) | see below |
| OVRNull | `hmd/` | the same repository and commit, folder `OVRNull` | MIT (`LICENSE`) | see below |
| Khronos OpenXR SDK headers | `external/OpenXR-SDK/include/openxr` (5 headers) | https://github.com/KhronosGroup/OpenXR-SDK, commit `57af7fc61f9f2d492580cb28aab6d0ea59d8d417` (API 1.1.62) | Apache-2.0 OR MIT (headers); `external/OpenXR-SDK/LICENSE` | none |
| OpenXR-MixedReality utility headers | `external/OpenXR-MixedReality/shared` (8 headers) | https://github.com/mbucchia/OpenXR-MixedReality (a fork of microsoft/OpenXR-MixedReality), commit `6ce0e75e79300f17fa9e86b4c9cb4cdaeaa29cc2` | MIT (`external/OpenXR-MixedReality/LICENSE`) | none |
| {fmt} | `external/fmt/include/fmt` (3 headers) | https://github.com/fmtlib/fmt, commit `130cf54cbc77abf0c8200a1206638cb70c9e26f1` (10.1.1) | MIT with the binary-embedding exception (`external/fmt/LICENSE`) | none |
| cJSON | `external/cJSON` (`cJSON.c`, `cJSON.h`) | https://github.com/DaveGamble/cJSON, commit `c859b25da02955fef659d658b8f324b5cde87be3` (1.7.19) | MIT (`external/cJSON/LICENSE`) | none |
| D3DX12 helper header | `external/d3dx12/d3dx12.h` | Microsoft DirectX-Headers, as carried in the VirtualDesktop-OpenXR repository at the commit above (`external/d3dx12`) | MIT (stated in the file) | none |
| OpenGL extension headers | `external/OpenGL` (`GL/glext.h`, `GL/wglext.h`, `KHR/khrplatform.h`) | Khronos OpenGL-Registry (header version 20220530) and EGL-Registry, as carried in the VirtualDesktop-OpenXR repository at the commit above (`external/OpenGL`) | MIT (stated in each file) | none |
| OpenVR | `external/openvr` (`headers/openvr_driver.h`, the hand skeleton simulation sample's `hand_simulation.h` / `.cpp`, `vrmath.h`) | https://github.com/ValveSoftware/openvr, commit `f51d87ecf8f7903e859b0aa4d617ff1e5f33db5a` | BSD-3-Clause (`external/openvr/LICENSE`) | none |
| AMD FidelityFX FSR 1 | `external/FidelityFX-FSR/ffx-fsr` (`ffx_a.h`, `ffx_fsr1.h`) | https://github.com/GPUOpen-Effects/FidelityFX-FSR, commit `a21ffb8f6c13233ba336352bdff293894c706575` (v1.0.2) | MIT (`external/FidelityFX-FSR/license.txt`) | none |
| AMD FidelityFX CAS | `external/FidelityFX-CAS/ffx-cas/ffx_cas.h` | https://github.com/GPUOpen-Effects/FidelityFX-CAS, commit `9fabcc9a2c45f958aff55ddfda337e74ef894b7f` (v1.0.2) | MIT (`external/FidelityFX-CAS/LICENSE.txt`) | none |

Only the files the build reads were copied from each origin. Upstream files keep their copyright headers.

### What was changed in the upstream sources

`runtime/` (45 files unchanged, 7 changed, 5 added; upstream's project files, `packages.config` and the dispatch
generator script are not here because this build does not read them; 4 upstream files are taken from `third_party`,
see below):

- `instance.cpp`, `system.cpp`, `session.cpp`, `framework/entry.cpp` (changed): the `SIMXR` paths. The runtime names
  itself as the bundled Virtual Desktop runtime does, reads its settings from `SIMXR_<name>` environment variables
  instead of the registry, writes its log to `SIMXR_LOG_DIR`, loads `LibOVRRT64_1.dll` from its own folder, skips the
  search for Virtual Desktop and the redirect to another runtime, and does not start the neural rendering (NR) pass.
- `simxr.h`, `simxr.cpp` (added): the two helpers those paths call. `commit.h` (added): a fixed string in place of
  the generated commit hash. `simxr-openxr.json` (added): the runtime manifest.
- `dlssnr.cpp`, `d3d11_native.cpp`, `runtime.h` (changed), `DlssnrFeatherPS.hlsl` (added): a soft edge for the
  region upstream's experimental neural rendering (NR) pass processes. It is compiled but never runs in this build, because the
  `SIMXR` define skips the pass's initialization. It is kept so that the sources equal the ones the tool was
  developed and verified with.

`hmd/` (10 files unchanged, 4 changed, 5 added):

- `driver.cpp`, `driver.h`, `api.cpp`, `pch.h` (changed): OVRNull's driver extended into a simulated headset: a vsync
  clock and display model, a limit on queued GPU work, scripted head and controller poses and buttons, session
  flags, eye-image capture.
- `sim.h`, `sim.cpp`, `control.cpp`, `CaptureVS.hlsl`, `CapturePS.hlsl` (added): the shared state, the motion
  scripts, the control port, the capture shaders.

`selftest/simxr_selftest.cpp` is this project's own file.

## Taken from `<repo>/third_party/`, never in the repository

| Id (folder) | What the build reads | Origin and version | Licence | Why it is not here |
|---|---|---|---|---|
| `libovr` | `Include/*.h` and its `Extras` subfolder, `Shim/OVR_CAPI_Prototypes.h`, `Shim/OVR_CAPI_Util.cpp`, `Shim/OVR_StereoProjection.cpp` | https://github.com/mbucchia/LibOVR, commit `3621783c2540db1ad0f68f85d9d30fafa3f7e8b7` (Oculus PC SDK LibOVR 1.64.0) | Meta Platforms Technologies SDK License Agreement (file headers: Oculus Master SDK License 1.0) | The licence grants use and reproduction "solely to develop, test, and/or distribute your Application" and forbids to "use or redistribute the SDK or any portion thereof in any manner that would cause the SDK (or any portion thereof) or MPT to become subject to the terms of any open source license or other restrictions" (sections 1.1 and 1.2.8). |
| `vdxr-meta` | `virtualdesktop-openxr/OVR_CAPIShim.c`, `meta_body_tracking_calibration.h`, `meta_body_tracking_fidelity.h`, `meta_body_tracking_full_body.h` | the VirtualDesktop-OpenXR repository at the commit above; unchanged | `OVR_CAPIShim.c`: Oculus Master SDK License 1.0 (Copyright Facebook Technologies, LLC); the three headers: "Copyright (c) Meta Platforms, Inc. and affiliates. Confidential and proprietary." | Meta's files inside the upstream repository; the repository's MIT licence does not cover them. |
| `vulkan-sdk` | `include/vulkan/*.h`, `include/vk_video/*.h` (9 headers read), `lib/vulkan-1.lib` | https://github.com/mbucchia/Vulkan-SDK, commit `cb7df06d3f02fa043a441641856310b798522583` (Vulkan header version 261, LunarG import library) | Apache-2.0 OR MIT (headers); the SDK's components are open source | `vulkan-1.lib` is a binary; the headers stay with the library they describe. |
| `wil` | `include/wil/*.h` | NuGet `Microsoft.Windows.ImplementationLibrary` 1.0.220201.1 | MIT | a NuGet package |
| `gameinput` | `include/GameInput.h`, `lib/x64/GameInput.lib` | NuGet `Microsoft.GameInput` 2.2.26100.6114 | Microsoft Software License Terms (GameInput redistributable) | a NuGet package under a vendor licence, with a binary |
| `detours` | `include/detours.h`, `libs/x64/detours.lib` | NuGet `Detours` 4.0.1 (already an item of `manifest/thirdparty.toml`) | MIT | a NuGet package with a binary |
| `ngx-sdk` | `include/nvsdk_ngx*.h`, `lib/Windows_x86_64/x64/nvsdk_ngx_s.lib` | https://github.com/NVIDIA/DLSS, commit `a291cc7d2cc642a51566f3dfd5376f635cd1b284` (already an item of `manifest/thirdparty.toml`) | NVIDIA RTX SDKs license | no NVIDIA SDK in the repository; `dlssnr.cpp` is compiled although the pass never runs |

## Carried by upstream, not used by this build

The upstream repository also carries the OpenXR conformance suite, the OpenXR SDK sources, a prebuilt OpenXR loader,
its installer and setup tools, the registry tool and the 32-bit configuration. None of them is compiled or linked
here. The self-test needs an OpenXR loader at run time (see `README.md`).
