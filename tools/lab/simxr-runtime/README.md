# The simulated headset (SimXR)

An OpenXR runtime that needs no headset. It replaces only the last link of the VR chain, the streaming runtime and
the headset, so that the whole pipeline in front of it runs on a PC with nobody wearing anything: the game, the
compositor, the frame generation layer, the upscalers and the hand-over copies to the runtime are the real build,
unchanged. What it captures is the image handed to the runtime, which is what a streaming runtime would encode and
send to the headset.

```
game - openvr_api.dll (OpenComposite) - OpenXR loader - frame generation layer -+   unchanged
                                                                                | XR_RUNTIME_JSON (this process only)
   simxr-openxr.dll   VirtualDesktop-OpenXR 1.1.0 built with SIMXR. It reports itself as
                      "VirtualDesktopXR", as the runtime bundled with Virtual Desktop does,
                      so the layers above take the same paths as with the real headset.
   LibOVRRT64_1.dll   the stand-in headset, loaded by the runtime from its own folder:
                      3072x3264 per eye, 120 Hz, a fixed field of view; scripted head,
                      controllers and buttons; eye-image capture; a control port
```

Nothing is installed system-wide. The runtime is chosen for one process through an environment variable; a real
runtime on the same PC and its registry entries stay as they are.

## What it is for, and what it is not

It shows that the chain starts, that the menu works, how frames are paced, that a head turn is followed without a
double image, that a setting changes the picture the way it should, and how long each stage takes.

It does not model the streaming runtime's encoding, the wireless link or the headset's reprojection, and it has no
eye or hand tracking, passthrough or audio. Its own display (which frame each refresh shows, displayed frames per
second, repeats, age at display) depends on how the stand-in paces and picks frames; a real streaming runtime's policy
for that is not known. Print those figures, do not judge by them. Judge by what the pipeline itself produced: the
game's frame times, the frames handed to the runtime with their submit time and render pose, and the captured images.
Frame times differ from a real headset session by a roughly constant amount: compare variants inside the simulator,
not the simulator with the headset.

## Rules

- The queued-frame limit stays at 1 (`SIMXR_MAX_QUEUED_FRAMES`, the default). Runs without it ended in GPU driver
  resets that blanked the screens.
- One game process at a time. It starts minimized and muted and never takes the focus: a person works at the same PC.
- A captured image taken while a game menu is open is not a picture of the pipeline (the compositor renders without
  upscaling then): close menus before a capture.

## The pieces

| Folder | Builds | What it is |
|---|---|---|
| `runtime/` | `simxr-openxr.dll`, `simxr-openxr.json` | VirtualDesktop-OpenXR's runtime sources. The `SIMXR` define makes it read settings from the environment, write its own log, load the stand-in headset from its folder and skip everything that looks for Virtual Desktop. |
| `hmd/` | `LibOVRRT64_1.dll` | The stand-in headset, grown from VirtualDesktop-OpenXR's `OVRNull`. It implements the LibOVR entry points the runtime calls: a vsync clock with a display model, the limit on queued GPU work, scripted poses and buttons, capture of the submitted eye images, the control port. |
| `selftest/` | `simxr_selftest.exe` | A small D3D11 OpenXR application that drives the two above without a game. |
| `external/` | | The third-party headers and sources the build reads that may be kept in a source repository. |

Licences and origins of every piece, and what was changed in the upstream files: [THIRD_PARTY.md](THIRD_PARTY.md).
The client for the control port and the scenarios that use it are in [../simxr](../simxr) (`simxr.py`, `vrtest.py`).

## Build

```
set VAMKIT_VCVARS=<Visual Studio>\VC\Auxiliary\Build\vcvars64.bat
set VAMKIT_FXC=<Windows SDK>\bin\<version>\x64
tools\lab\simxr-runtime\build.cmd
```

It writes `simxr-openxr.dll`, `simxr-openxr.json`, `LibOVRRT64_1.dll` and `simxr_selftest.exe` (and the two DLLs'
`.pdb` files, which `../simxr/symbolize.py` needs beside the modules) to `<repo>\out\lab\simxr\`; intermediate files go to
`out\lab\simxr\build\`. `VAMKIT_JOBS` sets the number of parallel compiles (default 4). The script calls the compiler,
`fxc`, `rc` and the linker directly with the options upstream's project files give for Release|x64; it needs no
msbuild, no NuGet and no network.

Seven folders must be in `<repo>\third_party\` first: `libovr`, `vdxr-meta`, `vulkan-sdk`, `wil`, `gameinput`,
`detours` and `ngx-sdk`. They hold what a public source repository may not carry (the Oculus SDK headers and Meta's
files from the upstream repository, the NVIDIA SDK), NuGet packages and binaries; `THIRD_PARTY.md` lists what the
build reads from each. The build stops with the first missing file's name.

## Environment variables

Read by the stand-in headset when it loads:

| Variable | Default | Meaning |
|---|---|---|
| `SIMXR_WIDTH`, `SIMXR_HEIGHT` | 3072, 3264 | recommended resolution per eye (256-8192) |
| `SIMXR_REFRESH` | 120 | refresh rate in Hz (30-240) |
| `SIMXR_IPD_MM` | 57.7 | distance between the eyes |
| `SIMXR_HEAD_HEIGHT_CM` | 120 | head height above the floor origin |
| `SIMXR_LATCH_US` | 1000 | a frame must be complete this long before a vsync to be shown at it |
| `SIMXR_MAX_QUEUED_FRAMES` | 1 | submitted frames whose GPU work may still run when the next frame begins; 0 = no limit. Keep 1. |
| `SIMXR_PORT` | 37601 | control port on 127.0.0.1; 0 = none |
| `SIMXR_VSYNC_LOG` | 1 | 0 turns the per-vsync CSV off |
| `SIMXR_CAPTURE_MB` | 1536 | memory for captured frames waiting to be written |
| `SIMXR_LOG_DIR` | `%LOCALAPPDATA%\SimXR` | where the logs, CSV files and captures go |

Read by the runtime: `SIMXR_LOG_DIR` (its `OpenXR.log`) and `SIMXR_<setting>` for each of VirtualDesktop-OpenXR's
registry settings, as an integer (for example `SIMXR_mirror_window=1`). None is needed; unset means the default.

## The control port

Newline-delimited JSON over TCP on 127.0.0.1 only: one request per line, one response per line. A request may carry
`id`, which the response echoes. Angles are degrees, distances metres, times milliseconds unless the field ends in
`_s`. The port opens with the application's first swapchain and closes when the runtime shuts the headset down.

| Command | Fields | Effect |
|---|---|---|
| `ping`, `status`, `help` | | `status`: device, frames, head and controller state; `help`: this list from the library itself |
| `stats` | `reset` | the display model since the last reset: vsyncs, new frames, repeats, dropped, late GPU work, submit intervals, age at display, pose error |
| `head` | `pos:[x,y,z]`, `ypr:[yaw,pitch,roll]` | the head's base pose (floor origin; yaw + = left, pitch + = up); stops the head motion |
| `motion` | `target` head / left / right, `type` still / turn / sine / keyframes, `axis` yaw / pitch / roll / x / y / z, `speed`, `amplitude`, `period_s`, `phase`, `duration_s` (< 0 = forever), `delay_ms`, `loop`, `keys:[{t_s, pos, ypr}]` | an offset added to the base pose over time, continuing from the current pose |
| `controller` | `hand` (required), `connected`, `follow_head`, `pos`, `ypr`, `trigger`, `grip`, `stick:[x,y]`, `buttons:{a,b,x,y,menu,system,stick}`, `touches` | a controller's state |
| `press` | `hand`, `button` a / b / x / y / menu / system / stick / trigger / grip (both required), `ms` (100), `delay_ms`, `value` | one button press |
| `capture` | `frames`, `every`, `delay_ms`, `eyes` both / left / right, `scale`, `crop:[x,y,w,h]`, `format` png / raw, `dir`, `name` | the eye images of the next submitted frames as PNG or raw RGBA8, with `capture.jsonl` |
| `capture_status` | | whether a capture is still being written |
| `mark` | `label` | a line in `simxr-markers.csv` (vsync, submitted count) |
| `session` | `hmd_mounted`, `should_quit`, `visible`, `input_focus` | the session status flags the runtime sees |

Example: `{"cmd":"motion","target":"head","type":"sine","axis":"yaw","amplitude":30,"period_s":2,"duration_s":8}`.

## How a launcher selects it

For the game process only, before it starts:

- `XR_RUNTIME_JSON` = the full path of `simxr-openxr.json`. The OpenXR loader then uses this runtime instead of the
  system's active one. `simxr-openxr.dll` and `LibOVRRT64_1.dll` must be in the manifest's folder.
- `SIMXR_LOG_DIR` = a folder of the run. The runtime writes `OpenXR.log` there; the stand-in headset writes
  `simxr-hmd.log`, `simxr-vsync.csv`, `simxr-frames.csv`, `simxr-markers.csv` and the captures.
- Nothing else changes: implicit API layers (the frame generation layer) load as with a real runtime, because the
  runtime reports the same name.

The launcher starts the game minimized and muted, keeps it from taking the focus, allows one such process at a time
and puts everything it changed back when the process ends.

## The self-test

```
simxr_selftest.exe <openxr_loader.dll> <seconds> [--sleep-ms N] [--gpu-ms N] [--log file]
```

A minimal OpenXR application: it creates a D3D11 session on the runtime `XR_RUNTIME_JSON` names and submits a
projection layer every frame. Each eye is a flat colour that encodes the frame number, the head yaw the frame was
rendered for and the eye, plus a white 64x64 square at a known place. `--sleep-ms` makes it a slow application,
`--gpu-ms` a GPU-bound one (each frame costs about that much GPU time before its image is released). It exits with 0
when the session ran and frames were submitted. It needs an OpenXR loader DLL (the Khronos `openxr_loader.dll`,
x64), which this folder does not build.

`../simxr/vrtest.py selfcheck` runs it and checks the stand-in alone, without a game: frame pacing, that the pose a
frame was rendered for is the predicted one, that a captured image is pixel-exact, a slow application, and a
GPU-bound application with and without the queued-frame limit. It uses the GPU for a few seconds.
