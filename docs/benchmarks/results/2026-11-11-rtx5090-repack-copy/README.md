# 2026-11-11-rtx5090-repack-copy

The four standard scenes on the desktop and on the simulated headset, with the kit and with VaM alone. Measured on
2026-11-11 with `tools/lab/bench/bench.py run --displays Desktop,VR --seconds 30 --baseline`. Fields and how to read
them: [../../README.md](../../README.md).

## What it was measured on

| | |
|---|---|
| GPU | NVIDIA GeForce RTX 5090 D, 32607 MiB, driver 616.56 |
| CPU | AMD Ryzen 9 9950X3D2 16-Core Processor (32 logical processors) |
| Memory, system | 128 GB, Windows 11 Pro |
| Desktop | 3840x2160 at 160 Hz, VaM in a window of that size |
| Simulated headset | 3072x3264 per eye, 144 Hz, the head looking left and right |
| VaM | a copy, without any content, of the development machine's VaM 1.22: a third-party repack, **not checked against an official installation**. BepInEx 5.0.1 |
| VaM's preferences | MSAA 4, 6 pixel lights, shader quality High, physics rate Auto (cap 2), smooth passes 4, **soft body physics off**, glow High |
| The kit's preset | path tracing off: DLSS SR K at 75 % render scale; path tracing on: RR at 33 %; output 100 %; neural rendering (NR) on; frame generation 4x |

A download was running on the machine during the measurement; it may have cost the cases that are limited by the
CPU a little. Treat this folder as the method's first complete run, not as a reference: the reference will be the
same run on an official VaM installation.

## Results

Real frames per second (frames VaM rendered; with frame generation on they are held at refresh / multiplier):

| scene | VaM alone, desktop | desktop raster | desktop path | desktop raster+fg | desktop path+fg | vr raster | vr path | vr raster+fg | vr path+fg |
|---|---|---|---|---|---|---|---|---|---|
| portrait | 306.04 | 203.64 | 173.05 | 39.45 | 39.47 | 120.27 | 101.77 | 36.02 | 36.0 |
| dance | 305.12 | 182.23 | 153.53 | 39.45 | 39.47 | 110.13 | 95.99 | 36.01 | 36.01 |
| duo | 192.93 | 139.15 | 125.51 | 39.46 | 39.45 | 85.06 | 78.18 | 36.05 | 36.06 |
| stage | 157.86 | 89.08 | 93.31 | 39.41 | 39.41 | 70.29 | 71.85 | 36.01 | 36.01 |

Every record:

| scene | display | setup | case | real fps | frame ms p50 / p95 / p99 | long frames | CPU cores | GPU load | video memory | notes |
|---|---|---|---|---|---|---|---|---|---|---|
| portrait | Desktop | kit | raster | 203.64 | 4.87 / 5.79 / 6.33 | 0 | 3.28 | 98.7 % | 9312.0 MB |  |
| portrait | Desktop | kit | raster+fg | 39.45 | 25.32 / 26.1 / 26.55 | 0 | 0.76 | 29.8 % | 8789.0 MB |  |
| portrait | Desktop | kit | path | 173.05 | 5.77 / 6.63 / 7.07 | 0 | 2.71 | 99.0 % | 9352.0 MB |  |
| portrait | Desktop | kit | path+fg | 39.47 | 25.31 / 26.14 / 26.53 | 0 | 0.75 | 34.8 % | 9398.0 MB |  |
| portrait | VR | kit | raster | 120.27 | 8.3 / 9.31 / 10.16 | 1 | 2.33 | 99.1 % | 13248.0 MB | headset: 120.108 new frames/s at 144.009 Hz, 16.6 % repeats, 1 hitches |
| portrait | VR | kit | raster+fg | 36.02 | 27.79 / 28.45 / 28.76 | 0 | 0.84 | 53.1 % | 13277.0 MB | headset: 143.509 new frames/s at 144.009 Hz, 0.4 % repeats, 0 hitches |
| portrait | VR | kit | path | 101.77 | 9.79 / 11.44 / 12.32 | 1 | 1.94 | 99.0 % | 14049.0 MB | headset: 101.773 new frames/s at 144.009 Hz, 29.3 % repeats, 1 hitches |
| portrait | VR | kit | path+fg | 36.0 | 27.78 / 28.46 / 28.84 | 0 | 0.77 | 61.0 % | 14032.0 MB | headset: 144.009 new frames/s at 144.009 Hz, 0 % repeats, 0 hitches |
| portrait | Desktop | VaM alone | vam | 306.04 | 3.23 / 3.72 / 4.08 | 0 | 4.87 | 38.3 % | 5718.0 MB |  |
| dance | Desktop | kit | raster | 182.23 | 5.47 / 6.42 / 6.86 | 0 | 3.53 | 98.0 % | 9587.0 MB |  |
| dance | Desktop | kit | raster+fg | 39.45 | 25.33 / 26.32 / 27.01 | 0 | 0.85 | 38.0 % | 9582.0 MB |  |
| dance | Desktop | kit | path | 153.53 | 6.46 / 7.78 / 8.59 | 0 | 2.69 | 98.0 % | 10208.0 MB |  |
| dance | Desktop | kit | path+fg | 39.47 | 25.34 / 26.33 / 26.85 | 0 | 0.9 | 38.8 % | 10218.0 MB |  |
| dance | VR | kit | raster | 110.13 | 9.03 / 10.18 / 10.87 | 0 | 2.5 | 99.0 % | 13968.0 MB | headset: 110.24 new frames/s at 144.009 Hz, 23.4 % repeats, 0 hitches |
| dance | VR | kit | raster+fg | 36.01 | 27.79 / 28.46 / 28.83 | 0 | 0.98 | 52.8 % | 13983.0 MB | headset: 143.976 new frames/s at 144.009 Hz, 0.0 % repeats, 0 hitches |
| dance | VR | kit | path | 95.99 | 10.37 / 11.44 / 11.98 | 1 | 2.39 | 99.0 % | 14749.0 MB | headset: 96.051 new frames/s at 144.009 Hz, 33.3 % repeats, 1 hitches; 1 frames over 50 ms |
| dance | VR | kit | path+fg | 36.01 | 27.77 / 28.56 / 29.08 | 0 | 1.17 | 65.3 % | 14750.0 MB | headset: 143.376 new frames/s at 144.009 Hz, 0.4 % repeats, 0 hitches |
| dance | Desktop | VaM alone | vam | 305.12 | 3.23 / 3.74 / 4.07 | 1 | 4.95 | 52.6 % | 6384.0 MB |  |
| duo | Desktop | kit | raster | 139.15 | 7.17 / 8.16 / 9.06 | 1 | 4.46 | 99.0 % | 10655.0 MB |  |
| duo | Desktop | kit | raster+fg | 39.46 | 25.32 / 26.25 / 27.11 | 0 | 1.5 | 43.2 % | 10639.0 MB |  |
| duo | Desktop | kit | path | 125.51 | 7.88 / 9.75 / 10.77 | 0 | 3.84 | 99.0 % | 11833.0 MB |  |
| duo | Desktop | kit | path+fg | 39.45 | 25.34 / 26.34 / 27.07 | 0 | 1.61 | 50.0 % | 11861.0 MB |  |
| duo | VR | kit | raster | 85.06 | 11.77 / 13.09 / 14.2 | 1 | 3.8 | 99.0 % | 15074.0 MB | headset: 85.005 new frames/s at 144.009 Hz, 41.0 % repeats, 6 hitches |
| duo | VR | kit | raster+fg | 36.05 | 27.77 / 28.64 / 29.21 | 0 | 1.7 | 67.7 % | 15374.0 MB | headset: 143.309 new frames/s at 144.009 Hz, 0.5 % repeats, 1 hitches |
| duo | VR | kit | path | 78.18 | 12.64 / 14.87 / 16.35 | 1 | 3.08 | 99.0 % | 16760.0 MB | headset: 78.32 new frames/s at 144.009 Hz, 45.6 % repeats, 28 hitches |
| duo | VR | kit | path+fg | 36.06 | 27.76 / 28.66 / 29.15 | 0 | 1.48 | 68.7 % | 16795.0 MB | headset: 142.942 new frames/s at 144.009 Hz, 0.7 % repeats, 0 hitches |
| duo | Desktop | VaM alone | vam | 192.93 | 4.93 / 7.87 / 8.9 | 15 | 5.86 | 99.5 % | 7653.0 MB |  |
| stage | Desktop | kit | raster | 89.08 | 10.72 / 18.23 / 22.44 | 43 | 4.52 | 97.1 % | 12420.0 MB |  |
| stage | Desktop | kit | raster+fg | 39.41 | 25.36 / 26.68 / 37.87 | 0 | 2.42 | 57.4 % | 12192.0 MB |  |
| stage | Desktop | kit | path | 93.31 | 11.24 / 13.38 / 15.61 | 0 | 5.68 | 92.5 % | 13617.0 MB |  |
| stage | Desktop | kit | path+fg | 39.41 | 25.32 / 26.97 / 39.24 | 0 | 2.45 | 58.3 % | 13845.0 MB |  |
| stage | VR | kit | raster | 70.29 | 14.26 / 16.13 / 17.6 | 0 | 4.3 | 99.0 % | 16655.0 MB | headset: 70.405 new frames/s at 144.009 Hz, 51.1 % repeats, 154 hitches |
| stage | VR | kit | raster+fg | 36.01 | 27.76 / 28.63 / 29.06 | 0 | 2.42 | 76.5 % | 16629.0 MB | headset: 143.909 new frames/s at 144.009 Hz, 0.1 % repeats, 0 hitches |
| stage | VR | kit | path | 71.85 | 13.99 / 16.47 / 18.35 | 1 | 4.11 | 99.0 % | 18411.0 MB | headset: 71.738 new frames/s at 144.009 Hz, 50.2 % repeats, 165 hitches |
| stage | VR | kit | path+fg | 36.01 | 27.76 / 28.59 / 29.06 | 0 | 2.28 | 73.0 % | 18433.0 MB | headset: 143.909 new frames/s at 144.009 Hz, 0.1 % repeats, 0 hitches |
| stage | Desktop | VaM alone | vam | 157.86 | 5.06 / 10.16 / 11.4 | 251 | 7.34 | 99.5 % | 8893.0 MB |  |

## Reading them

- **With frame generation on, every scene holds the beat**: 39.4-39.5 real frames per second on the 160 Hz desktop
  (160 / 4) and 36.0 in VR (144 / 4), with no frame over 50 ms, 0.0-0.7 % repeated refreshes on the simulated
  headset and GPU load of 30-58 % on the desktop and 53-77 % in VR. On this GPU the standard scenes leave a quarter
  to two thirds of it unused at the beat.
- **Without frame generation** the kit renders 89-204 real frames per second on the desktop and 70-120 in VR with
  VaM's lighting, 93-173 and 72-102 with path tracing. The path tracer costs 13-16 % of the frame rate in the
  one-person scenes, 8-10 % with two people and nothing in the stage scene, where it replaces eight lights' worth of
  raster lighting and shadow maps.
- **VaM alone is faster on the desktop** (158-306) in these light scenes. That is the price of the kit's picture, not
  a like-for-like test: VaM alone renders 4K natively with MSAA and no reconstruction; the kit renders at 75 % (or
  33 % with path tracing), reconstructs, applies neural rendering (about 1.7 ms per frame at 4K) and shades skin, hair and clothing
  differently. VaM alone cannot be measured in VR without a real headset.
- **Video memory**: VaM alone 5.7-8.9 GB (of which about 2.3 GB were in use before VaM started); the kit 8.8-13.8 GB on
  the desktop and 13.2-18.4 GB in VR.
- **One stall**: the dance scene's VR path-tracing window had one 83 ms frame, together with the window's one garbage
  collection.
- **How heavy the scenes are**: the heaviest path-traced VR cases here run at 72-78 real frames per second without
  frame generation. A heavy two-person dance scene with real content measured 52-62 on the same machine at the same
  render scale while the optimisations landed, about 62 at the end
  ([../../../knowledge/frame-budget.md](../../../knowledge/frame-budget.md)). Roughly: a real heavy scene costs a
  fifth to a quarter more frame time than the heaviest standard scenes, and its load time is of another order.

## What single components contribute

`bench.py run --scenes duo,stage --displays Desktop --cases raster,path --without <plugin>` for five runtime
components, one parked per session; frame generation off. Records: `components.json`. The duo scene on the desktop:

| setup | VaM's lighting: real fps | frame ms p95 | CPU cores | path tracing: real fps | frame ms p95 | CPU cores |
|---|---|---|---|---|---|---|
| the kit | 135.09 | 8.4 | 4.42 | 124.07 | 9.77 | 4.09 |
| the kit, a second session | 135.19 | 8.42 | 4.25 | 119.71 | 10.58 | 3.89 |
| without cloth-physics | 125.66 | 10.68 | 4.4 | 112.83 | 11.69 | 3.76 |
| without hair-solver | 129.21 | 8.98 | 4.31 | 120.08 | 10.19 | 3.73 |
| without cpu-boost | 137.24 | 8.55 | 5.45 | 128.8 | 9.35 | 4.6 |
| without wrap-batch | 135.95 | 8.3 | 4.34 | 125.19 | 10.05 | 3.84 |
| without gc-tuning | 134.89 | 8.43 | 4.19 | 119.18 | 10.1 | 3.73 |

- **cloth-physics**: without it 7 % fewer frames with VaM's lighting and 6-9 % fewer with path tracing, and the slow
  frames get slower (p95 8.4 -> 10.7 ms).
- **hair-solver**: without it 4 % fewer frames with VaM's lighting; with path tracing the difference is inside what two
  sessions of the same setup differ by (124.1 and 119.7).
- **cpu-boost**: no change in frame rate here (the GPU is the limit in these cases); without it VaM uses about one
  processor core more (4.3 -> 5.5 cores with VaM's lighting, 4.0 -> 4.6 with path tracing).
- **wrap-batch, gc-tuning**: no difference that can be told from the sessions' own spread. The scenes have few
  wrapped clothing items, and no garbage collection happened in any of these windows; what the collector work is for
  shows in long sessions and in loads ([../../../knowledge/gc.md](../../../knowledge/gc.md)).
- These are light characters: one hair item and two or three clothing items each. The gains measured with heavy
  characters are larger ([../../../knowledge/frame-budget.md](../../../knowledge/frame-budget.md), "What a heavy
  character costs").

The stage scene's records are in the file but cannot be compared across sessions: with VaM's lighting it ran at 73,
75, 86, 95, 102 and 103 real frames per second in six sessions whatever was parked, because it sits near the physics
rate (see "Near VaM's physics rate" in [../../README.md](../../README.md)). One of its sessions hung when VaM started
(before the scene, right after the render size changed as the menus closed; the open start problems are in
[../../../knowledge/rejected-and-open.md](../../../knowledge/rejected-and-open.md)) and is recorded as failed; the
runner now tries such a session once more.
