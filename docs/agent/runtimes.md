# Runtimes and SDKs that are not in the repository

`manifest/thirdparty.toml` is the definition: name, version, hash, where it comes from. This page explains it. The
project redistributes nothing of NVIDIA's; for each model runtime it gives the file name, the version it was developed
with, the hash, and where people usually get the file.

## Model runtimes (at run time, in `<VaM>/vamkit/bin/dlss/`)

| File | What it does here | Without it | Where it comes from |
|---|---|---|---|
| `nvngx_dlss.dll` | DLSS super resolution, models K / L / M (path tracing off) | DLSS upscaling is unavailable | public, NVIDIA's DLSS repository on GitHub, the tag pinned in the manifest |
| `nvngx_dlssd.dll` | DLSS Ray Reconstruction, preset F (path tracing on) | path tracing cannot be shown: the tracer waits for RR | public, same repository and tag |
| `nvngx_dlssnr.dll` | the neural denoiser on the output | the denoiser stays off; everything else works | no public download. It reached the public inside game updates; the user supplies a copy and the hash is checked |
| `nvngx_dlssg.dll` | DLSS Frame Generation (VR through the OFXR layer, desktop through our presenter) | frame generation falls back to the OFXR layer's optical flow | a current NVIDIA driver downloads it into its model store the first time a frame generation title runs; a copy from a game that ships it is accepted too |

The launcher passes the folders to the components through environment variables (`VAM_DLSS_RUNTIME_PATH`,
`VAM_NR_PREPASS_PATH`, `OFXR_DLSSG_RUNTIME_PATH`); see `components/render/launcher/launch.py`.

Rules:

- Take a file only from the vendor's own public location, or from the user. Do not point users at re-uploads.
- Check the hash in the manifest. A different version may work, but it is a different model: quality and cost numbers
  in `docs/knowledge` were measured with the pinned one. If the user wants a newer runtime, install it as a deliberate
  change and measure again ([../protocol/performance.md](../protocol/performance.md)).
- All four must come from the same DLSS release family. Mixed versions have not been tested.
- The NVIDIA files are signed; the manifest names the signer. A file that is not signed by NVIDIA is not the runtime.

## SDKs and libraries (at build time, in `third_party/`)

| Item | Needed by | Licence |
|---|---|---|
| NVIDIA DLSS SDK (NGX headers and import libraries) | opencomposite, d3d11-proxy, ofxr-dlssg | NVIDIA RTX SDKs licence: shown to the user, who accepts it before download |
| NVIDIA NVAPI | opencomposite, the skin's native library (path tracer) | NVIDIA NVAPI SDK licence, accepted the same way |
| NVIDIA Image Scaling SDK (NVSharpen) | opencomposite | MIT |
| AMD FidelityFX FSR 1 | opencomposite (the per-frame fallback upscaler) | MIT |
| Microsoft Detours | opencomposite | MIT |
| glm, Khronos OpenXR SDK | opencomposite (its submodules) | MIT, Apache-2.0 |
| Khronos Vulkan headers | opencomposite (its Vulkan path is compiled but not used here; the import library is made at build time from `vulkan-1.def`) | Apache-2.0 or MIT |
| OpenVR headers | ofxr-dlssg | BSD-3-Clause |

Every item is pinned in the manifest (a commit, a version or a file hash) and `vamkit fetch` refuses anything that
does not match. Shaders are compiled with the Windows SDK's own `dxc.exe` and `fxc.exe` (`VAMKIT_DXC` / `VAMKIT_FXC`
name another folder); nothing is fetched for them.

## Why the build is local

OpenComposite is GPL-3.0 and links NVIDIA's NGX library statically; NVIDIA's terms and the GPL do not allow shipping
that binary. Shaders derived from VaM's own programs may not be redistributed either. So every binary is built on the
user's machine from source plus what the user's own VaM and the vendors provide. This is the maintainers' reading of
the licences, not legal advice.
