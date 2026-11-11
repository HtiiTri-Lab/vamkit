# The lab: test and measurement tools

The tools this project was developed with. They are here so that whoever continues the work measures the same things
the same way. How to decide what to run is in [../../docs/protocol/testing.md](../../docs/protocol/testing.md).

**Status.** Sessions on the desktop and in VR on the simulated headset, the first-run check and the benchmark run from
this repository against an installed VaM folder. Four older scripts (`vamrun.py`, `vrtest.py`, `shoot.py`,
`cpu_census.py`) were exported from the development tree as they are and still expect its launcher and folders; the
porting they need is listed at the end. Nothing here is installed into VaM.

## What is here

| Path | What it does | Runs from the repo today |
|---|---|---|
| `simxr/stacks.py` | native call stacks of every thread of a live process (a hung VaM), through dbghelp | yes |
| `simxr/symbolize.py` | module + offset from a crash report to function and source line (needs the module's `.pdb` beside it) | yes |
| `simxr/flightlog.py` | frame pacing as the frame generation layer logged it: durations per cycle and per real frame, repeats, gaps. Works on runs with a real headset too | yes, on a run's log folder |
| `simxr/imagecheck.py` | image checks on captured eye images: content shift between frames by phase correlation, double-image detection during a head turn | yes, as a library (numpy, Pillow) |
| `census/thread_census.py` | CPU time of every thread of the process and every logical CPU, sampled from outside | yes, given a process |
| `simxr-runtime/` | the simulated headset itself: an OpenXR runtime with a stand-in headset library, built by `vamkit build --lab` into `out/lab/simxr/` (its own README: variables, control port, what was changed upstream) | yes, after `vamkit fetch --lab --accept-licenses` |
| `simxr/simxr.py` | client for the simulated headset's control port (scripted head motion, button presses, eye-image capture, statistics) and `analyze`, the display model's summary of a run | yes |
| `simxr/vamrun.py` | one VaM session on the simulated headset, driven from Python (launch, wait until ready, load a scene, change a setting live, quit and restore) | needs porting |
| `simxr/vrtest.py` | the standard scenarios: `smoke`, `menu`, `headturn`, `pacing`, and `selfcheck` of the simulator itself | needs porting |
| `simxr/shoot.py` | picture series of VaM for look comparisons: frames the face or a region from what the eye camera sees, checks every capture, several scenes in one VaM | needs porting |
| `census/cpu_census.py`, `census_report.py`, `CensusProbe.cs` | which task takes how much CPU per frame: main thread per Unity subsystem, our plugins on every thread, every thread from outside; `--candidate` swaps plugins in for one run | needs porting |
| `test-agent/VaMTestAgent.cs` | a BepInEx plugin that lets a test see and drive VaM over a local port: status, load a scene, open and close menus, freeze animation, get / set / invoke by reflection, click a scene's buttons, a per-frame log. Dormant unless the test launch sets its environment variable; listens on 127.0.0.1 only | builds; installed only for a test run |
| `session/session.py` | one VaM session from an installed VaM folder, in the background, driven through the test agent: the mod on the desktop or in VR on the simulated headset, vanilla, or VaM alone with the agent (`baseline`); status, get / set / invoke by reflection, scene parameters, the compositor's live settings, quit. Compiles the agent, puts it into the plugins folder for the run and removes it; `without=[...]` parks single plugins for a run | yes |
| `session/first_run.py` | the first run after an install, as checks: a vanilla session (nothing of the kit in the process), a mod session on the desktop (plugins, proxy, a person rendered, DLSS super resolution and frame generation, path tracing and ray reconstruction switched on and off while playing) and one in VR on the simulated headset (VR session up, a picture in the frames handed to the headset, path tracing, frame generation at one frame per refresh), then VaM's own files and window settings back | yes |
| `session/trial_copy.py` | a copy of a VaM installation's program files without any content: a place to try an install without touching the VaM someone uses | yes |
| `session/scenes/person.json` | the smallest scene with something to shade: one default person, one light (VaM starts with an empty scene) | yes |
| `bench/bench.py`, `bench/make_scenes.py`, `bench/scenes/` | the benchmark: four standard scenes made of VaM's built-in content and a motion plugin, measured per display (desktop, simulated headset) and case (path tracing off / on, frame generation off / on), against VaM alone and with single plugins parked. What is measured and how to read it: [../../docs/benchmarks/README.md](../../docs/benchmarks/README.md) | yes |

Offline probes live with their components and run today: the path tracer's contract probes
(`components/shading/rt-core/tests`), the skin tests (`components/shading/skin/tests`), cpu-boost's harness
(`components/runtime/cpu-boost/harness`), the collector's checks (`components/runtime/gc-tuning/tools`).

## The simulated headset

The idea: replace only the last link of the VR chain, the OpenXR runtime and the headset, with a runtime that needs no
headset. Everything before it is the real build: VaM, the compositor, the frame generation layer. A captured image is
the image handed to the runtime, which is what a streaming runtime would encode and send. The stand-in headset reports
a fixed resolution and refresh rate, plays scripted head and controller motion, and exposes a control port.

What it shows: that the chain starts, the menu works, frames are paced, a head turn is followed without a double image,
a setting changes the picture the way it should, and how long each stage takes.

What it cannot show: the streaming runtime's own cost and encoding, reprojection in the headset, wireless transport,
and anything a person does with their hands. Measured frame times differ from a real headset session by a roughly
constant amount; compare variants inside the simulator, not the simulator with the headset.

Rules that came with it:

- Keep the runtime's queued-frame limit at 1. Runs without it ended in GPU driver resets.
- One VaM at a time, started minimised and muted, never taking the focus: the person works at the same PC.
- Trust what the pipeline produced (VaM's frame times, frames handed to the runtime with their submit time and render
  pose, captured images). Numbers about the simulated display itself depend on the stand-in's rhythm: print them, do
  not judge by them.
- About half a minute into a session VaM tends to open its menu once by itself; while a menu is open the bridge renders
  native frames without upscaling. Close menus before a capture and discard captures taken with a menu open.
- Do not stop a session by killing the test script: the launcher goes with it and VaM's files stay swapped. If it
  happened, see "Stopping a test midway" in the testing protocol.

The runtime is in `simxr-runtime/`: a build of an open-source OpenXR runtime (VirtualDesktop-OpenXR, MIT) with a
stand-in headset library. A session selects it for the one VaM process it starts (`XR_RUNTIME_JSON` in that process's
environment); the machine's active OpenXR runtime and a connected headset are never touched.

## Porting still to do

1. `vamrun.py` starts sessions through the development tree's launcher and its launch-panel run folders. It has to
   become a layer over `session/session.py`, which starts the installed launcher in its test mode (`launch.py --test`:
   minimized, kept from taking the focus, presets and window settings put back), selects the simulated runtime for VR
   and handles the test agent. Still missing there: a marker file while a test owns VaM.
2. Paths: the tools import each other and find VaM by their place in the development tree. They should take the VaM
   folder from `VAMKIT_VAM` and write results under `out/lab/`.
3. `cpu_census.py` uses a main-thread phase probe and a helper module that were not exported, and names a test scene of
   the development tree. The scene should become an argument; the phase probe has to be exported or replaced.
4. `shoot.py` and `vrtest.py` have a default heavy scene from the development tree; scenes should be arguments. The
   scenarios of `vrtest.py` (menu, head turn, pacing) are not covered by `session/` yet.

Until then: use the stand-alone tools, the offline probes, `session/` for end-to-end checks on the desktop and in VR,
`bench/` for numbers, and the logs of each run (`<VaM>/vamkit/logs/<run>/`).
