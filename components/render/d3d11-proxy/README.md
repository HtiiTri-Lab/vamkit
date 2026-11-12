# d3d11-proxy

A `d3d11.dll` that sits beside `VaM.exe`. Unity loads `d3d11.dll` by name and calls `D3D11CreateDevice`; this proxy makes
that device a D3D11On12 device on the process's D3D12 device and one direct queue of its own, so D3D12 work (DLSS, ray
tracing, neural denoising, frame generation) can run on the same queue, in order with VaM's frame, with no fence and no round
trip. It also holds the desktop presenter: in desktop mode it owns the flip-model D3D12 swap chain and runs frame
generation there (DLSS Frame Generation, or AMD FSR's on any card). The player sees nothing of it directly; without it
the VR compositor cannot do path tracing or the D3D12 stages. Depends on the NVIDIA NGX SDK (`ngx-sdk`), on the headers
of AMD's FSR API (`fsr-sdk`; its DLLs are loaded at run time, nothing is linked) and on the frame generator code in
`components/render/ofxr-dlssg`. Installed as `vamkit/bin/d3d11.dll`; the launcher copies it next to `VaM.exe` only while the mod runs.

## Files

| File | Holds |
|---|---|
| `d3d11_proxy.cpp` | `Proxy_D3D11CreateDevice` (`D3DINTEROP_MODE`), the 11on12 device and queue singletons, `D3DInterop_GetOn12` / `D3DInterop_GetOn12ForDevice`, the unwrap probe's run and `D3DInterop_UnwrapOnOwnQueue`, the log and VRAM sampling |
| `unwrap_probe.cpp` | `unwrap_probe.exe` (installed in `vamkit/bin`): whether a `d3d11on12.dll` stops its own queue when an unwrap names it; run by the proxy at start-up, or by hand |
| `d3d11_thunks.asm`, `d3d11_thunks.inc`, `d3d11.def` | Generated: `jmp [p_<name>]` thunks for every export of the real `d3d11.dll` the proxy does not implement, the name list, and the export table with the real ordinals |
| `gen_thunks.py` | Regenerates those three from the real `System32\d3d11.dll`: `python -I -B gen_thunks.py <real d3d11.dll> <out dir>` |
| `desktop_presenter.cpp`, `desktop_presenter.h` | Desktop presenter: wraps Unity's swap chain, own HIGH-priority D3D12 queue, a guided frame generator (DLSS-G 2 to 4 times, or FSR 2 times, chosen by `[Performance] FrameGenModel` and followed live), even frame pacing, keep-alive for the window; `VamDesktopInstall()` is called from the proxy after the device is made |

## Build

`python tools/vamkit build d3d11-proxy` runs every step for this component (native library) and writes to
`out\d3d11-proxy\`; the steps are listed in `manifest/components.toml`.

`build.cmd` in this folder (needs `VAMKIT_VCVARS`, the path of `vcvars64.bat`): `ml64` on `d3d11_thunks.asm`, then one `cl /LD` over
`d3d11_proxy.cpp`, `desktop_presenter.cpp` and the two generators `..\ofxr-dlssg\src\d3d12\d3d12_dlssg.cpp` and `d3d12_fsrfg.cpp` (C++20, `/MT`; the FSR API
headers come from the fetched `fsr-sdk`), linked with `d3d11.def`, `d3d12.lib`, `dxgi.lib`
and the NGX SDK from `third_party\ngx-sdk`. Output `out\d3d11-proxy\d3d11.dll`. The `C4005` redefinition warnings for `WIN32_LEAN_AND_MEAN` /
`NOMINMAX` (the build defines them on the command line, two sources define them again) are expected.

## Settings

No cfg file. Environment variables (the launcher sets them for the mod; the DLL can stay in VaM's folder because without them it is a pass-through):

| Variable | Meaning |
|---|---|
| `D3DINTEROP_MODE` | `on12`: Unity's `D3D11CreateDevice` (hardware, from `UnityPlayer.dll`) returns a D3D11On12 device. Unset or `pass`: every call goes to the real function, VaM as without this DLL |
| `D3DINTEROP_LOG` | Directory for `d3d11_proxy.log`; when set, VRAM is also sampled each second into `d3d11_proxy_vram.csv`. Default in on12 mode: the exe's folder |
| `D3DINTEROP_ON12_WORKER` | Diagnostic. `0`: 11on12 runs without its own worker thread (`D3D11_CREATE_DEVICE_PREVENT_INTERNAL_THREADING_OPTIMIZATIONS`); its translation and submissions happen on Unity's render thread. Logged in `d3d11_proxy.log`. The launcher passes it through |
| `D3DINTEROP_UNWRAP_QUEUE` | Diagnostic. `own`: unwraps name 11on12's queue without the probe (stops for good with a `d3d11on12.dll` from before Microsoft's fix); `separate`: they always name a queue of their own. Unset: the probe decides |
| `VAM_DESKTOP_CONFIG` | Desktop only: an ini with `[ofxr] frame_multiplier` (2 to 4) and `[overlay] position` |
| `VAM_PERFORMANCE_CONFIG` | The run's `performance.ini`: `[Performance] FrameGeneration`, `FrameGenModel` (`DLSS` / `FSR`), `DLSSGLite` are read for the desktop presenter |
| `VAM_FSR_RUNTIME_PATH` | The folder with AMD's FSR API DLLs (`vamkit/bin/fsr`); without it FSR frame generation is not offered |
| `VAM_DISPLAY_MODE` | `Desktop` selects the presenter path (`startup_display.h`) |

## Interfaces

- Exports other components bind to: `D3DInterop_GetOn12(ID3D12Device**, ID3D12CommandQueue**)` and `D3DInterop_GetOn12ForDevice(...)` (the D3D12
  device and queue under Unity's 11on12 device; `E_NOINTERFACE` for any other device), used by the compositor
  (`components/render/opencomposite/OpenOVR/Compositor/vam_on12.h`); `VAM_DesktopIsWindowSurface`, `VAM_DesktopCapture`;
  `OFXR_GetProviderApiVersionV4`, `OFXR_PrepareFrameGenerationV3`, `OFXR_GetFrameMultiplierV1`, `OFXR_PublishD3D11GuidesV4` (the provider interface the compositor
  publishes depth and motion guides through in desktop mode); `OFXR_GetFrameGeneratorsV1` (which generators can run, for the in-game menu; compiled in from
  `d3d12_fsrfg.cpp`). `OFXR_GetFrameMultiplierV1` reports as `most` what the generator in use makes (FSR: 2).
- The D3D12 device is a per-adapter singleton shared with OFXR's DLSS-G, the NR pre-pass and NGX D3D12. Only calls from `UnityPlayer.dll` get the
  11on12 device; later callers (OpenComposite, the XR runtime, OFXR) get the real one.
- Constant buffers written every frame must be DYNAMIC with `Map(WRITE_DISCARD)` on 11on12 (see `VamWriteConstants` in `vam_on12.h`).
- Writes `d3d11_proxy.log`, and `dlssg.log` / `ofxr-bridge-flight-desktop.log` for the presenter. The `on12:` lines name
  the `d3d11on12.dll` loaded, the Windows build and the unwrap probe's verdict. The verdict line repeats the
  `d3d11on12.dll` version and says which queue unwraps name: 11on12's own (the original path) or a queue of their own.
- The unwrap probe (`unwrap_probe.cpp`, installed as `vamkit/bin/unwrap_probe.exe`): before Microsoft's fix (Windows 10,
  older Windows 11), an `UnwrapUnderlyingResource` that names 11on12's queue stops that queue for good. At device
  creation the proxy runs the probe in a process of its own on the `d3d11on12.dll` VaM loaded. `D3DInterop_UnwrapOnOwnQueue`
  then says whether unwraps may name 11on12's queue (cheaper in VR) or must name a queue of their own
  (`docs/knowledge/d3d11on12.md`, "Unwrap / return"). Run by hand: `unwrap_probe.exe own` (exit 0: safe, 2: stops),
  `other`, `dll <path>` for another `d3d11on12.dll`.

## Tests

None offline in this repository. `unwrap_probe.exe` needs a graphics card: `unwrap_probe.exe own` exits 0 on a fixed
`d3d11on12.dll` and 2 on one from before the fix (`dll <path>` tests another one). Otherwise only the game shows it: the log
lines "on12: device ... on D3D12 device ... queue ..." and, for the desktop, `dlssg.log` (foreground changes, every `ResizeBuffers`, window
"not responding" verdicts, and lines starting `desktop wait:` / `desktop presenter:` when a fence wait lasts more than 2 s, the swap chain's
latency object times out, a `Present` takes 250 ms or more, or the presenter has not taken a frame for a second; each names what the presenter is
doing and how far both queues have got).

## Invariants

- Vanilla stays vanilla: with `D3DINTEROP_MODE` unset or `pass` the proxy must hand every call to the real `D3D11CreateDevice`. If 11on12 cannot be created,
  Unity gets the real device and the failure is logged.
- Every export but `D3D11CreateDevice` forwards to `System32\d3d11.dll` with the real ordinals; regenerate the thunks with `gen_thunks.py`, never by hand.
- Presenter: its DLSS-G, copies and presents run on its own queue and are not drained after each step. Unity's queue never waits on the GPU for the
  presenter's queue: Unity's frames go through a ring of three inputs, and Unity writes only one that is neither taken nor still being read (checked on
  the CPU). Unity's thread waits for the presenter to take the last frame (the pacing), for at most a second. A `ResizeBuffers` to the size the buffers already have keeps buffers, frames and DLSS-G. A keep-alive `WM_NULL` is posted to VaM's window each second
  so Windows does not judge it not responding when no input arrives.
- The presenter's NGX generator is compiled from OFXR's own source file; keep the two in step.

## Read next

[../../../docs/knowledge/d3d11on12.md](../../../docs/knowledge/d3d11on12.md),
[../../../docs/knowledge/frame-generation.md](../../../docs/knowledge/frame-generation.md),
[../../../docs/knowledge/frame-path.md](../../../docs/knowledge/frame-path.md)
