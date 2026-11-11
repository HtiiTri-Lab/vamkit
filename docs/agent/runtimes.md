# Runtimes and SDKs that are not in the repository

`manifest/thirdparty.toml` is the definition: name, version, hash, where it comes from. This page explains it. The
project redistributes nothing of NVIDIA's, Intel's or AMD's; for each model runtime it gives the file name, the version
it was developed with, the hash, and where people usually get the file.

## Model runtimes (at run time, in `<VaM>/vamkit/bin/dlss/`)

| File | What it does here | Without it | Where it comes from |
|---|---|---|---|
| `nvngx_dlss.dll` | DLSS super resolution, models K / L / M (path tracing off) | DLSS upscaling is unavailable | public, NVIDIA's DLSS repository on GitHub, the tag pinned in the manifest |
| `nvngx_dlssd.dll` | DLSS Ray Reconstruction, preset F (path tracing on) | path tracing cannot be shown: the tracer waits for RR | public, same repository and tag |
| `nvngx_dlssnr.dll` | the neural denoiser on the output | the denoiser stays off; everything else works | no public download. It reached the public inside game updates; the user supplies a copy and the hash is checked |
| `nvngx_dlssg.dll` | DLSS Frame Generation (VR through the OFXR layer, desktop through our presenter) | frame generation falls back to the OFXR layer's optical flow | a current NVIDIA driver downloads it into its model store the first time a frame generation title runs; a copy from a game that ships it is accepted too |

The launcher passes the folders to the components through environment variables (`VAM_DLSS_RUNTIME_PATH`,
`VAM_NR_PREPASS_PATH`, `OFXR_DLSSG_RUNTIME_PATH`); see `components/render/launcher/launch.py`.

## Upscaling and frame generation without an NVIDIA card (in `<VaM>/vamkit/bin/xess/` and `.../fsr/`)

| File | What it does here | Without it | Where it comes from |
|---|---|---|---|
| `libxess.dll` | Intel XeSS super resolution with path tracing off, on any GPU with shader model 6.4. Needs the Visual C++ redistributable 14.40 or later | XeSS is not offered | public, Intel's XeSS repository on GitHub, the tag pinned in the manifest |
| `amd_fidelityfx_loader_dx12.dll`, `amd_fidelityfx_upscaler_dx12.dll`, `amd_fidelityfx_framegeneration_dx12.dll` | AMD FSR upscaling (FSR 4 on the Radeons that have it, FSR 3.1 on every other card) and FSR frame generation (2x) | FSR is not offered (all three are needed) | public, AMD's FidelityFX SDK repository on GitHub, the tag pinned in the manifest; AMD's signed files |

All four are optional. The launcher passes their folders as `VAM_XESS_RUNTIME_PATH` and `VAM_FSR_RUNTIME_PATH`. Which
of DLSS, XeSS and FSR can run is probed once at start; the menu lists only those, and a model that cannot run is
replaced by one that can. Path tracing needs Ray Reconstruction, so without an RTX card it switches itself off.
What runs on which card, and what was and was not tested (nothing on a Radeon or an Arc):
[../knowledge/upscalers-other-vendors.md](../knowledge/upscalers-other-vendors.md).

Rules:

- Take a file only from the vendor's own public location, or from the user. Do not point users at re-uploads.
- The hash in the manifest is the version the project was developed with, not the only one taken. `vamkit fetch` takes
  that file when it finds it, and otherwise the newest file of the same name that carries the vendor's valid signature
  and is not older than the item's `min_version` (for the NVIDIA files it also looks in the driver's own model store,
  `C:/ProgramData/NVIDIA/NGX/models`); a file handed over with `--file id=path` is judged the same way. Every report
  names the version in use. Another version is another model: quality and cost numbers in `docs/knowledge` were measured
  with the pinned one, so measure again when it matters ([../protocol/performance.md](../protocol/performance.md)).
- The launcher does the same at every start (`components/render/launcher/runtimes.py`): an NVIDIA library the kit
  lacks is taken from the driver's model store when its signature is valid, copied to `<VaM>/vamkit/cache/ngx/`, and
  the panel says which version is used; what is found nowhere is named with what is lost and how to add it.
- A missing file is never passed over. `vamkit fetch`, `doctor`, the install plan and `verify` name it, say what the
  user does not get without it, and how to add it; the first-run check reports the installation as working with LESS.
  Do the adding: most of these are one command.
- All four must come from the same DLSS release family. Mixed versions have not been tested.
- The vendors' files are signed; the manifest names the signer. A file without that valid signature is refused, whatever
  its name.

## SDKs and libraries (at build time, in `third_party/`)

| Item | Needed by | Licence |
|---|---|---|
| NVIDIA DLSS SDK (NGX headers and import libraries) | opencomposite, d3d11-proxy, ofxr-dlssg | NVIDIA RTX SDKs licence: shown to the user, who accepts it before download |
| NVIDIA NVAPI | opencomposite, the skin's native library (path tracer) | NVIDIA NVAPI SDK licence, accepted the same way |
| NVIDIA Image Scaling SDK (NVSharpen) | opencomposite | MIT |
| AMD FidelityFX FSR 1 | opencomposite (the per-frame fallback upscaler) | MIT |
| Intel XeSS SDK (headers) | opencomposite | MIT at the pinned commit; the SDK's binaries under the Intel Simplified Software License, shown to the user before download |
| AMD FSR SDK (FSR API headers) | opencomposite, ofxr-dlssg, d3d11-proxy | MIT |
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
