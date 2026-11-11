# Third-party code

## In this repository

| What | Where | License | Upstream |
|---|---|---|---|
| OpenComposite (modified) | `components/render/opencomposite` | GPL-3.0 (`LICENSE.txt`); bundled inih (BSD), OpenVR headers (BSD-3-Clause) | https://gitlab.com/znixian/OpenOVR, base commit `a27e7e6a64bdcd1eff6b7fba1ea2ea34bcf1273d` |
| OFXR Bridge (modified: DLSS Frame Generation, 4x, the desktop presenter's producer API) | `components/render/ofxr-dlssg` | LGPL-3.0 (`LICENSE`, `THIRD_PARTY.md` inside) | https://github.com/djules75/OFXR-Bridge, base `07449429c139e5d2070b6f0855ed5323de7cee00` (0.2.13.2) |
| D3D11 proxy and desktop presenter | `components/render/d3d11-proxy` | LGPL-3.0 (compiled together with OFXR's `d3d12_dlssg.cpp`) | this project |
| NVIDIA RTX Character Rendering hair BCSDF (modified: a reflection tint) | `components/shading/rt-core/third_party/rtxcr` | MIT (`License.txt` there) | https://github.com/NVIDIA-RTX/RTXCR-Material-Library, revision `eb184f9637f81a58a620d0e3cb4356acbc45cbe6` |

`components/runtime/gc-tuning/native/gctuning.cpp` reimplements the marking loop of the Boehm-Demers-Weiser collector
(as built into Unity's Mono) so that it can run on several threads. The collector has its own permissive license
(Copyright Hans-J. Boehm, Alan J. Demers, Xerox, SGI, HP: use, copy, modify and distribute with the notice retained).

## Fetched at build time, never committed

See `manifest/thirdparty.toml` for versions and hashes: the NVIDIA DLSS SDK and runtimes (SR, RR, Frame Generation,
the neural denoiser), NVAPI, the NVIDIA Image Scaling SDK, AMD FidelityFX FSR 1, Microsoft Detours, glm, the Khronos
OpenXR SDK and Vulkan headers, and the OpenVR headers. Each keeps its own license; `vamkit fetch` lists the ones under
a vendor licence and downloads them only after `--accept-licenses`. Shaders are compiled with the Windows SDK's own
`dxc` and `fxc`.

## Generated on your machine, never committed

Parts of the skin, clothing and hair shaders are generated from the shader programs inside your own VaM installation
(`VaM_Data/*.assets`). Those outputs are derived from VaM and stay in `out/gen/` on your PC.

The runtime components replace work that VaM and its GPUTools library do with code that gives the same results:
`cpu-boost` follows four of VaM's C# methods, `hair-solver` and `cloth-physics` follow the rules of GPUTools' compute
kernels. They are our own source, written to match. Two files go further and follow VaM's code line by line, by the
maintainers' decision: `components/runtime/cpu-boost/native/cpuboost_native.cpp` (four of VaM's C# methods, statement
by statement, so that the results are bit-identical) and `components/shading/hair-strands/shaders/hair_onepass.hlsl`
(VaM's hair pass, instruction by instruction). No file or data of VaM itself is in the repository.

VaM is a product of MeshedVR. This project is not affiliated with MeshedVR or NVIDIA.
