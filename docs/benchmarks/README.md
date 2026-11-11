# Benchmarks

Standard scenes, a fixed way to run them, and what the numbers mean. Use it to put a figure on a machine, a setting or
a change, before and after. The rules for judging a result are in
[../protocol/performance.md](../protocol/performance.md); where a frame's time goes is in
[../knowledge/frame-budget.md](../knowledge/frame-budget.md).

Results measured so far: [results/](results). Every results file says which machine and which VaM installation it
comes from; a number without those is not comparable.

## The scenes

`tools/lab/bench/scenes/`, written by `tools/lab/bench/make_scenes.py` (change a scene there, not in the JSON). They
use only what every VaM 1.x installation has built in (its people, its one strand hair, its clothing, lights, shapes)
and one scene plugin beside them, `BenchMotion.cs`, so they load anywhere and can be published. The plugin moves each
person the same way on every run: hips, chest, head and hands follow sums of sines of the wall clock, so two setups
are measured on the same movement whatever their frame rates.

| Scene | Content | People | Lights (with shadows) | Stands for |
|---|---|---|---|---|
| `portrait` | one person close up, slow movement | 1 | 3 (2) | judging a look: skin, eyes and hair fill the view |
| `dance` | one person seen whole, dancing, simulated dress and stockings | 1 | 4 (3) | a solo dance: cloth and hair physics, motion vectors |
| `duo` | two people close together, both dancing, simulated clothing | 2 | 5 (4) | the heavy two-person scene the frame budget is planned around |
| `stage` | three dancers further away, 30 objects, coloured lights | 3 | 8 (4) | a stage: the number of objects and lights weighs more than one person |

Every person wears the built-in strand hair (about 208,000 traced segments each); the other built-in hair styles are
meshes, cost little and are not what real scenes use. The viewer stands where VaM puts it (eyes at 1.6 m, 1.5 m in
front of the origin); in VR the simulated head is at the same height.

### Your own scenes

`--scenes` also takes any scene as VaM names it (`Saves/scene/mine.json`, or `Creator.Package.1:/Saves/scene/x.json`
inside a package) and VaM's own benchmark levels (`vam:BenchmarkCrypt`, see
[../agent/open-items.md](../agent/open-items.md)). They are loaded as they are and measured the same way, without the
standard scenes' check that the motion runs. That is how to answer "how does my scene run": the standard scenes are
for comparing setups, real content is what the user plays. A record names the scene it measured; leave private scene
names out of anything you publish.

### What the scenes cannot stand for

- **Real content is heavier.** A scene people actually play has looks with several texture sets, denser and longer
  hair (often several hair items a person), an environment of a few million triangles, mirrors or reflection probes,
  and a dozen scene plugins. Built-in content has none of that. The standard scenes rank setups on one machine and
  show where time goes; they do not predict the frame rate of someone's own scene. [results/](results) says how the
  heaviest standard scene compares with a real heavy scene on the development machine.
- **Loading.** Built-in content loads in seconds from VaM's own bundles; the load components
  ([../knowledge/loading.md](../knowledge/loading.md)) work on packages, images and scene plugins, which these scenes
  do not have. Load times recorded here only show that nothing got slower.
- **Looks.** The benchmark measures time. Whether a picture is right is judged by a person
  ([../protocol/testing.md](../protocol/testing.md)).
- **A real headset.** The simulated headset replaces the runtime and the display. Streaming, encoding and the
  headset's own reprojection are not in the numbers ([../../tools/lab/README.md](../../tools/lab/README.md)).

## Setups and cases

A **setup** is what is installed and switched on; a **case** is a renderer state inside one session.

| Setup | What runs |
|---|---|
| kit | the installed build, the launch panel's preset for the display mode (render scales, denoiser, multiplier are recorded with every result) |
| VaM alone (`--baseline`) | VaM's own files, none of the kit's plugins, BepInEx on only for the test agent; desktop only (VaM's own VR needs a real headset) |
| without NAME (`--without NAME`) | the kit with one plugin parked (its files start with NAME, for example `VaMHairSolver`): what that component contributes |

| Case | Path tracing | Frame generation | What it shows |
|---|---|---|---|
| `raster` | off | off | what the renderer can do with VaM's lighting and the kit's shading; the machine is the limit |
| `raster+fg` | off | on | the same as it is played: real frames held at refresh / multiplier |
| `path` | on | off | what the path tracer can do |
| `path+fg` | on | on | as it is played |

The cases are switched while the game runs, in this order, in one session per scene and display, so they see the same
scene state. Frame generation off makes VaM run as fast as it can: use those cases to compare costs. Frame generation
on holds real frames at the display's refresh rate divided by the multiplier (144 Hz at 4x: 36; a 160 Hz monitor at
4x: 40): those cases answer "does it hold the beat, and with how much to spare", not "how fast is it".

## What is measured

One record per case, over a window of 30 s (default) that starts after the scene has settled.

| Field | Meaning | Source |
|---|---|---|
| `fps` | real frames per second: frames VaM rendered, not frames shown | the test agent's per-frame log |
| `frame_ms_p50` / `p95` / `p99` / `max` | real frame time, percentiles over the window | same |
| `long_frames` | frames longer than twice the median. Above the physics rate VaM's frames alternate between ones that run physics steps and ones that do not, so a high count there is unevenness, not stalls | same |
| `hitches` | frames longer than 50 ms: a stall a person sees at any frame rate | same |
| `collections` | Mono garbage collections in the window (each is a stall of the whole game, see [../knowledge/gc.md](../knowledge/gc.md)) | same |
| `physics_steps_per_frame`, `physics_hz` | physics steps VaM ran per rendered frame (mean over the window) and its fixed step rate. Hair and cloth are simulated once per step, so this is part of the load | same |
| `cpu_cores` | CPU time VaM's process used, per second of wall clock (2.0 = two cores busy) | process times |
| `gpu_load_percent`, `video_memory_mb` | load and memory in use of the whole GPU, mean and peak; `video_memory_mb_before_vam` is what was in use before VaM started | `nvidia-smi`, once a second |
| `gpu_stages` | GPU time of the compositor's stages (upscaling, denoiser, ray reconstruction, object motion), mean per frame | the compositor's timing lines in `opencomposite.log` |
| `renderer` | is it tracing, traced triangles and hair segments, scene build time, is ray reconstruction the upscaler | the skin plugin's status |
| `headset` (VR) | the simulated display over the same window: `displayed_fps` (new frames shown per second), `submitted_fps`, `repeat_ratio` (share of refreshes that showed an old frame), `hitches` (an image held for three refreshes or more), `longest_image_ms`, `pipeline_gaps` (no frame for four refreshes), `frozen_pose_runs`, `pose_age_intended_ms` | the simulated headset's per-refresh log, `simxr.analyze` |
| `start_seconds`, `load_seconds` | launch to a responsive VaM; loading the scene | wall clock |
| `motion_cycles` | how far the motion plugin got in the window: 0 means the scene stood still and the record is void | the plugin's `Cycles` |
| `settings`, `vam_preferences` | the kit's preset for the run; VaM's own quality preferences (anti-aliasing, pixel lights, physics rate, soft body physics...), read and never changed | the session, VaM |

How to read them:

- **Compare inside one results file first.** Two sessions differ by a few percent for reasons nobody controls; a
  difference smaller than that between two files means nothing.
- **`raster` against VaM alone is not a like-for-like speed test.** The kit renders at a lower resolution and
  reconstructs (DLSS), denoises, and shades skin, hair and clothing differently; VaM alone renders natively with MSAA.
  The pair shows what the kit's picture costs or saves on that machine, not which is "faster".
- **With frame generation on, read `fps` against the beat** (refresh / multiplier) and look at `frame_ms_p99`,
  `hitches` and, in VR, the `headset` block. `gpu_stages` then include the time the compositor waits for the beat:
  read stage times from the cases without frame generation.
- **Near VaM's physics rate a frame rate is not one number.** VaM steps physics at a fixed rate (`physics_hz`; 72 on
  the desktop these results were measured on), and hair and cloth are simulated once per step, so above that rate
  some frames carry a step and some do not. The stage scene with VaM's lighting on the desktop gave 73, 75, 80, 89
  and 95 real frames per second in five sessions of the same setup, steady inside each session. The per-frame logs
  show two states: in the fast sessions frame times fall into two groups (6-8 ms without a physics step, 10-12 ms
  with one); in the slow ones every frame takes 13-14 ms and carries a step, so the frame rate sits on the physics
  rate. Which state a session ends up in is not explained yet. The duo scene, at about 135, stayed within 3 % over
  four sessions. Until it is understood: do not compare two sessions whose frame rates lie between the physics rate
  and about one and a half times it; compare a case well above it, or switch the setting inside one session.
  `physics_hz` and `physics_steps_per_frame` are in every record for this reason.
- **A record is void** when `frames_with_menu_open` is not 0 (the compositor renders native frames while a menu is
  open), `motion_cycles` is 0, `atoms_missing` is not empty, or a `path` case's `renderer.tracing` is false. The report
  flags each.
- **In VR the head looks left and right** during every window (12 degrees, one sway in 5 s). A still head makes
  reconstruction and frame generation look better than they are in use.
- **The simulated display's own rhythm** decides `repeat_ratio` when VaM renders below the refresh rate without frame
  generation; it says little there. With frame generation on, repeats and hitches are the result.

## Before a long run

A full run takes about forty minutes and is worth nothing when the kit was not whole or a scene behaves unlike what
you expect. Find that out cheaply first, in this order, and start the full run once, only when all three are clean:

1. `python tools/vamkit build`, `python tools/vamkit test`, `python tools/lab/session/first_run.py --vam <VaM folder>`
   (a few minutes). Read the BepInEx log of the mod session: a plugin that stepped aside or failed says so in a
   warning. Find out why and fix it before measuring anything.
2. `bench.py run --seconds 10 --cases raster,path` over all scenes (about six minutes). Look for a frame rate far from
   what you expect, a GPU that is not busy, `physics_steps_per_frame` at VaM's cap, a void record.
3. Every session prints the warnings of BepInEx's log at its start (`!! ... the kit may not be whole`) and keeps them
   in the record (`plugin_warnings`). When that line appears in a long run, stop it.

Results are snapshots: each says what state it was measured in. Do not re-measure everything after each fix; rerun
the cases the fix can change, or add a note.

## Running it

The VaM folder must have the build installed ([../agent/install.md](../agent/install.md)); VR needs the simulated
headset (`python tools/vamkit fetch --lab --accept-licenses`, `python tools/vamkit build --lab`).

```
python tools/lab/bench/bench.py run --vam <VaM folder> [--scenes portrait,dance,duo,stage] [--displays Desktop,VR]
                                    [--seconds 30] [--refresh 144] [--multiplier 4] [--baseline] [--without NAME ...]
python tools/lab/bench/bench.py report out/lab/bench/<time>/results.json [more.json ...]
```

It starts the game once per scene, display and setup (about four minutes each for the kit, one for VaM alone), in the
background: minimized, muted, never taking the focus. One VaM at a time; nothing else should use the GPU; say how long
it will take before starting. Scenes are staged under `Saves/scene/_agent-test/` for the run and removed afterwards.
Results: `out/lab/bench/<time>/results.json` (the records), `machine.json`, `report.md`. To open a scene by hand, copy
`tools/lab/bench/scenes/` to `<VaM>/Saves/scene/vamkit-bench/`.

To publish a result: copy `results.json` and `machine.json` to `docs/benchmarks/results/<date>-<machine>/`, add a
README that says which VaM installation it was (version, clean or not, its preferences) and what else was different.
Records carry no paths of the machine.
