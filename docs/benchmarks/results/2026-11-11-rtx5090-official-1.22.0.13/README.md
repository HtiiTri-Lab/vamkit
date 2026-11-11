# 2026-11-11-rtx5090-official-1.22.0.13

The four standard scenes on an official VaM installation, on the desktop and on the simulated headset, with the kit and
with VaM alone. Measured on 2026-11-11 with `tools/lab/bench/bench.py run --displays Desktop,VR --seconds 30
--baseline`. Fields and how to read them: [../../README.md](../../README.md).

## What it was measured on

| | |
|---|---|
| GPU | NVIDIA GeForce RTX 5090 D, 32607 MiB, driver 616.56 |
| CPU | AMD Ryzen 9 9950X3D2 16-Core Processor (32 logical processors) |
| Memory, system | 128 GB, Windows 11 Pro |
| Desktop | 3840x2160 at 160 Hz, VaM in a window of that size |
| Simulated headset | 3072x3264 per eye, 144 Hz, the head looking left and right |
| VaM | 1.22.0.13 as MeshedVR's updater installs it, no licence key entered, nothing added but BepInEx 5.4.23.6 (from its release page) and the kit |
| VaM's preferences | as a new installation has them: MSAA 4, 2 pixel lights, shader quality High, physics rate Auto (72 Hz on this desktop, 90 Hz in VR; at most 2 steps a frame), smooth passes 2, **soft body physics on**, glow Low, mirror reflections and realtime reflection probes on |
| The kit | this repository's build against that VaM. In this run cpu-boost was not active: its first fingerprint check did not recognise the methods of 1.22.0.13 (see "With cpu-boost" below for the two scenes it matters in) |
| The kit's preset | path tracing off: DLSS SR K at 75 % render scale; path tracing on: RR at 33 %; output 100 %; denoiser on; frame generation 4x |

## Results

Real frames per second (frames VaM rendered; with frame generation on they are held at refresh / multiplier):

| scene | VaM alone, desktop | desktop raster | desktop path | desktop raster+fg | desktop path+fg | vr raster | vr path | vr raster+fg | vr path+fg |
|---|---|---|---|---|---|---|---|---|---|
| portrait | 281.81 | 207.34 | 170.99 | 39.48 | 39.46 | 117.87 | 101.3 | 36.06 | 36.13 |
| dance | 263.6 | 188.77 | 156.25 | 39.46 | 39.47 | 112.41 | 94.86 | 36.05 | 36.02 |
| duo | 122.35 | 80.61 | 94.16 | 39.44 | 39.43 | 77.61 | 69.51 | 36.0 | 36.0 |
| stage | void | 15.32 | 22.32 | 23.06 | 21.28 | 23.46 | void | 26.21 | 23.46 |

Every record:

| scene | display | setup | case | real fps | frame ms p50 / p95 / p99 | long frames | CPU cores | GPU load | video memory | notes |
|---|---|---|---|---|---|---|---|---|---|---|
| portrait | Desktop | kit | raster | 207.34 | 4.78 / 5.75 / 6.68 | 0 | 4.38 | 98.6 % | 8246.0 MB |  |
| portrait | Desktop | kit | raster+fg | 39.48 | 25.31 / 25.95 / 26.38 | 0 | 1.05 | 31.1 % | 8209.0 MB |  |
| portrait | Desktop | kit | path | 170.99 | 5.84 / 6.75 / 7.33 | 0 | 3.67 | 99.0 % | 8914.0 MB |  |
| portrait | Desktop | kit | path+fg | 39.46 | 25.33 / 26.19 / 26.79 | 0 | 1.08 | 34.3 % | 8838.0 MB |  |
| portrait | VR | kit | raster | 117.87 | 8.34 / 10.24 / 13.37 | 2 | 3.09 | 99.0 % | 13299.0 MB | headset: 117.074 new frames/s at 144.009 Hz, 18.7 % repeats, 3 hitches |
| portrait | VR | kit | raster+fg | 36.06 | 27.75 / 28.37 / 28.84 | 0 | 1.29 | 58.0 % | 12648.0 MB | headset: 136.892 new frames/s at 144.009 Hz, 4.9 % repeats, 0 hitches |
| portrait | VR | kit | path | 101.3 | 9.72 / 12.05 / 13.58 | 4 | 2.77 | 99.0 % | 13410.0 MB | headset: 102.973 new frames/s at 144.009 Hz, 28.5 % repeats, 1 hitches |
| portrait | VR | kit | path+fg | 36.13 | 27.71 / 28.47 / 28.98 | 0 | 1.05 | 54.4 % | 13356.0 MB | headset: 140.676 new frames/s at 144.009 Hz, 2.3 % repeats, 0 hitches |
| portrait | Desktop | VaM alone | vam | 281.81 | 3.23 / 4.67 / 5.5 | 12 | 5.11 | 34.7 % | 5018.0 MB |  |
| dance | Desktop | kit | raster | 188.77 | 5.19 / 7.06 / 8.22 | 6 | 4.43 | 97.7 % | 8884.0 MB |  |
| dance | Desktop | kit | raster+fg | 39.46 | 25.32 / 26.3 / 27.01 | 0 | 1.18 | 35.6 % | 8854.0 MB |  |
| dance | Desktop | kit | path | 156.25 | 6.48 / 7.68 / 8.42 | 0 | 3.63 | 98.6 % | 9518.0 MB |  |
| dance | Desktop | kit | path+fg | 39.47 | 25.3 / 26.43 / 27.61 | 0 | 1.21 | 38.5 % | 9485.0 MB |  |
| dance | VR | kit | raster | 112.41 | 8.89 / 10.03 / 10.71 | 0 | 2.69 | 98.9 % | 13410.0 MB | headset: 112.215 new frames/s at 144.009 Hz, 22.1 % repeats, 0 hitches |
| dance | VR | kit | raster+fg | 36.05 | 27.76 / 28.42 / 28.75 | 0 | 1.34 | 55.0 % | 13730.0 MB | headset: 141.909 new frames/s at 144.009 Hz, 1.5 % repeats, 0 hitches |
| dance | VR | kit | path | 94.86 | 10.38 / 12.35 / 13.12 | 1 | 2.58 | 98.7 % | 14779.0 MB | headset: 94.806 new frames/s at 144.009 Hz, 34.2 % repeats, 0 hitches |
| dance | VR | kit | path+fg | 36.02 | 27.77 / 28.62 / 29.08 | 0 | 1.3 | 63.4 % | 14784.0 MB | headset: 143.043 new frames/s at 144.009 Hz, 0.7 % repeats, 0 hitches |
| dance | Desktop | VaM alone | vam | 263.6 | 3.23 / 5.52 / 6.06 | 21 | 4.88 | 45.6 % | 6380.0 MB | 1 frames over 50 ms |
| duo | Desktop | kit | raster | 80.61 | 12.38 / 17.16 / 55.47 | 104 | 4.0 | 63.3 % | 10687.0 MB | 29 frames over 50 ms |
| duo | Desktop | kit | raster+fg | 39.44 | 25.34 / 26.48 / 28.36 | 0 | 2.56 | 46.1 % | 10585.0 MB |  |
| duo | Desktop | kit | path | 94.16 | 12.11 / 14.02 / 15.46 | 3 | 4.32 | 81.9 % | 11669.0 MB |  |
| duo | Desktop | kit | path+fg | 39.43 | 25.34 / 26.25 / 27.17 | 0 | 2.54 | 56.4 % | 11735.0 MB |  |
| duo | VR | kit | raster | 77.61 | 11.72 / 20.17 / 21.32 | 7 | 4.79 | 89.9 % | 14442.0 MB | headset: 77.705 new frames/s at 144.009 Hz, 46.0 % repeats, 280 hitches |
| duo | VR | kit | raster+fg | 36.0 | 27.77 / 28.42 / 28.62 | 0 | 2.49 | 64.6 % | 14449.0 MB | headset: 134.275 new frames/s at 144.009 Hz, 6.8 % repeats, 0 hitches |
| duo | VR | kit | path | 69.51 | 12.58 / 20.66 / 22.4 | 5 | 4.03 | 94.9 % | 15833.0 MB | headset: 69.171 new frames/s at 144.009 Hz, 52.0 % repeats, 294 hitches |
| duo | VR | kit | path+fg | 36.0 | 27.77 / 28.5 / 28.91 | 0 | 2.48 | 76.1 % | 15841.0 MB | headset: 141.943 new frames/s at 144.009 Hz, 1.4 % repeats, 0 hitches |
| duo | Desktop | VaM alone | vam | 122.35 | 10.68 / 12.63 / 13.32 | 1 | 4.97 | 57.0 % | 6755.0 MB |  |
| stage | Desktop | kit | raster | 15.32 | 42.43 / 201.59 / 271.18 | 62 | 2.16 | 32.5 % | 11351.0 MB | 185 frames over 50 ms |
| stage | Desktop | kit | raster+fg | 23.06 | 43.09 / 48.65 / 51.0 | 0 | 2.66 | 64.2 % | 11373.0 MB | 18 frames over 50 ms |
| stage | Desktop | kit | path | 22.32 | 44.44 / 50.8 / 53.36 | 0 | 2.64 | 33.6 % | 12952.0 MB | 48 frames over 50 ms |
| stage | Desktop | kit | path+fg | 21.28 | 46.84 / 51.7 / 53.77 | 0 | 2.44 | 40.1 % | 12892.0 MB | 91 frames over 50 ms |
| stage | VR | kit | raster | 23.46 | 42.22 / 49.84 / 53.94 | 0 | 2.79 | 65.4 % | 16324.0 MB | headset: 23.635 new frames/s at 144.009 Hz, 83.6 % repeats, 708 hitches; 35 frames over 50 ms |
| stage | VR | kit | raster+fg | 26.21 | 37.59 / 44.65 / 47.46 | 0 | 3.03 | 82.6 % | 15766.0 MB | headset: 94.573 new frames/s at 144.009 Hz, 34.3 % repeats, 600 hitches; 4 frames over 50 ms |
| stage | VR | kit | path | 23.83 | 39.19 / 45.14 / 48.84 | 2 | 2.71 | 57.7 % | 23482.0 MB | VOID: a menu was open; headset: 23.768 new frames/s at 144.009 Hz, 83.5 % repeats, 713 hitches; 7 frames over 50 ms |
| stage | VR | kit | path+fg | 23.46 | 42.38 / 48.38 / 51.68 | 0 | 2.99 | 59.1 % | 20008.0 MB | headset: 138.042 new frames/s at 144.009 Hz, 4.1 % repeats, 5 hitches; 23 frames over 50 ms |
| stage | Desktop | VaM alone | vam | 22.43 | 44.06 / 53.93 / 59.27 | 1 | 2.52 | 35.5 % | 8105.0 MB | VOID: a menu was open; 101 frames over 50 ms |

## Reading them

- **One person (portrait, dance)**: the same picture as on the other installation
  ([../2026-11-11-rtx5090-repack-copy/README.md](../2026-11-11-rtx5090-repack-copy/README.md)), within a few percent:
  the GPU is the limit, 189-207 real frames per second with VaM's lighting and 156-171 with path tracing on the
  desktop, 112-118 and 95-101 in VR; with frame generation the beat is held (39.5 on the desktop, 36.0 in VR) with no
  frame over 50 ms.
- **Two and three people: VaM's own physics is the limit, not the renderer.** A new installation has soft body physics
  on, and it is simulated on the CPU once per physics step for every person. Two people: VaM alone 122 real frames per
  second, the kit 81-94 on the desktop and 70-78 in VR, with the GPU at 63-95 %; with frame generation the beat is
  still held. Three people (stage): VaM alone renders 22 frames per second, every frame carries the maximum of two
  physics steps and takes about 44 ms; the kit is at 15-26 whatever the case (frames of 38-47 ms), cannot hold the
  frame generation beat, and frames over 50 ms are common. Nothing in the kit makes VaM's soft body physics cheaper.
  On the other installation, with soft body physics off, the kit ran the same stage scene at 70-93.
- **The kit's own CPU cost shows where the CPU is the limit**: two people with VaM's lighting on the desktop, 81 against
  VaM alone's 122, with 29 frames over 50 ms in that window; the same scene path traced runs at 94 without one.
- **Physics runs slow with frame generation at VaM's default cap.** In VR VaM steps physics at 90 Hz and at most twice a
  frame. At 36 real frames per second that is 72 steps a second (`physics_steps_per_frame` 2.0), so simulated motion
  runs at 80 % of real time. On the desktop (72 Hz, 39.5 frames) it is 1.82 steps a frame, in time. For VR at 36 real
  frames, set VaM's physics update cap to 3 or its physics rate to 72 Hz (User Preferences); the benchmark does not
  change VaM's preferences.
- **Two records are void** (a menu was open during the window: VaM opens its menu by itself at very low frame rates):
  stage VR path, and stage VaM alone, whose 22.4 is still the figure used above.
- **Video memory**: VaM alone 5.0-8.1 GB; the kit 8.2-13.0 GB on the desktop and 12.6-16.3 GB in VR, except the stage
  scene's two path-traced VR cases at 20.0 and 23.5 GB.

## With cpu-boost

In the run above cpu-boost was not active: its first fingerprint check did not recognise the methods of 1.22.0.13,
whose instructions are those of 1.22.0.3 ([../../../agent/adapt-vam-version.md](../../../agent/adapt-vam-version.md)).
With that corrected, the plugin reported all four paths on and its verify mode found no difference from VaM's own
code in about nine thousand checked calls on this installation. The two scenes it can matter in were measured once
more (`with-cpu-boost.json`; no plugin warning in any session):

| scene | display | case | first run: real fps | frame ms p50 | again, cpu-boost active: real fps | frame ms p50 | physics steps a frame |
|---|---|---|---|---|---|---|---|
| duo | Desktop | raster | 80.61 | 12.38 | 93.77 (void: a menu was open) | 12.39 | 0.768 |
| duo | Desktop | raster+fg | 39.44 | 25.34 | 39.46 | 25.33 | 1.822 |
| duo | Desktop | path | 94.16 | 12.11 | 83.59 | 12.92 | 0.861 |
| duo | Desktop | path+fg | 39.43 | 25.34 | 39.36 | 25.34 | 1.822 |
| duo | VR | raster | 77.61 | 11.72 | 66.7 | 12.25 | 1.338 |
| duo | VR | raster+fg | 36.0 | 27.77 | 36.0 | 27.77 | 2.0 |
| duo | VR | path | 69.51 | 12.58 | 59.6 | 14.07 | 1.487 |
| duo | VR | path+fg | 36.0 | 27.77 | 36.0 | 27.78 | 2.0 |
| stage | Desktop | raster | 15.32 | 42.43 | 29.95 | 30.16 | 1.937 |
| stage | Desktop | raster+fg | 23.06 | 43.09 | 35.75 | 28.61 | 1.914 |
| stage | Desktop | path | 22.32 | 44.44 | 32.59 | 30.53 | 1.989 |
| stage | Desktop | path+fg | 21.28 | 46.84 | 31.21 | 31.62 | 1.997 |
| stage | VR | raster | 23.46 | 42.22 | 27.46 | 35.68 | 2.0 |
| stage | VR | raster+fg | 26.21 | 37.59 | 23.91 | 41.23 | 2.0 |
| stage | VR | path | 23.83 | 39.19 | 23.98 | 41.41 | 2.0 |
| stage | VR | path+fg | 23.46 | 42.38 | 23.97 | 41.0 | 2.0 |

- With frame generation the duo scene holds the beat as before (39.4-39.5 on the desktop, 36.0 in VR).
- The stage scene on the desktop ran at 30-36 real frames per second where the first run had 15-23, with frames of
  about 30 ms in place of 42-47 ms; in VR it is unchanged at 24-27. Every frame still carries VaM's maximum of two
  physics steps: the scene stays limited by VaM's soft body physics and still cannot hold the frame generation beat.
- The duo scene without frame generation ran lower in VR (67 and 60 against 78 and 70) and on the desktop path traced
  (84 against 94). These frame rates lie between the physics rate and one and a half times it, where two sessions of
  the same setup differ by this much ([../../README.md](../../README.md), "Near VaM's physics rate"): neither these
  nor the stage scene's gain say what cpu-boost contributes. On another installation, measured inside one setup, it
  saved about one processor core and no frame time while the GPU was the limit
  ([../2026-11-11-rtx5090-repack-copy/README.md](../2026-11-11-rtx5090-repack-copy/README.md)).
- The portrait and dance scenes were not measured again: the GPU is the limit there.
