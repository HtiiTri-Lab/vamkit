# The machine the kit was made on, and how yours differs

Everything in this repository was built, run and measured on one PC. This page lists what that PC has of everything
the kit depends on, and says what to do about the lines where the user's machine is different.

```
python tools/vamkit versions --vam "<VaM>"
```

prints the same list for the machine you are on and marks each line `same`, `DIFF` (with the reference's value) or
`new`, with a note on what the line has meant so far and where to read. `doctor` ends with the count. The full
reference is [reference-machine.json](reference-machine.json) (`versions --json` on the PC below); the command changes
nothing and records versions and model names only, no paths or names.

## A difference is where to start, not a reason to stop

The code tests behaviour, never a version list ([adapt-vam-version.md](adapt-vam-version.md)), so this list is not a
list of what is supported. It is a map of where nobody has been. A line that differs is a part of the user's machine
the kit has never run with. It is the first place to look when something behaves differently there, and it is worth
going to look before the user runs into it.

The first report from another machine shows why. On an RTX 5090 with Windows 10 22H2, the desktop picture froze at
the first upscaled frame, with the GPU idle. The card, the driver and presentation were suspected first. The line that
mattered was `d3d11on12.dll`: Windows 10 still has a D3D11On12 that stops its own queue when an unwrap names it, a bug
Microsoft fixed in 2025 and this PC never had. It was found by reading the reporter's logs against the code, and
reproduced here by putting an older `d3d11on12.dll` from `WinSxS` beside `VaM.exe`. The fix leaves this PC on its own
path and gives the other machine a safe one: the proxy tests the DLL at every start
([../knowledge/d3d11on12.md](../knowledge/d3d11on12.md), "Unwrap / return").

You are expected to do the same with whatever differs on the machine in front of you:

1. **Read the note** `versions` prints under the line, and the document it names.
2. **Run the part that depends on it** and read its own log, not just whether the game started.
   `tools/lab/session/first_run.py` says ok, LESS or FAIL for each part and why. `tools/lab/bench/bench.py` measures
   it against the published results ([../benchmarks/README.md](../benchmarks/README.md)).
3. **Make the difference happen where you can study it.** Much of a machine can be swapped for one program to try:
   Windows loads many system DLLs from the program's folder first (that is how the Windows 10 hang was reproduced on
   Windows 11), the OpenXR runtime is one registry value or `XR_RUNTIME_JSON`, and the run-time files have their own
   folders. A small test program that does only the suspected call is often faster than the game
   (`components/render/d3d11-proxy/unwrap_probe.cpp` is one).
4. **Fix it so that both machines get their best.** Decide by what the machine does, tested at run time, not by a
   version number. Keep the reference path as fast as it was and measure that it is. Log what was decided, so that the
   next report says it in one line.
5. **Record it.** Add the machine to "Other machines" below and the lesson to the note in `tools/vamkit/versions.py`
   and the knowledge document. Tell the user what differs, what it means for them and what you did.

Differences that are not problems are worth exploring too. A newer driver, DLSS runtime, OpenXR runtime or BepInEx
may be better than what is pinned here. Measure before and after, and keep what wins on that machine.

## The reference machine

Captured on 2026-11-12, at commit 8cc6fc8. Everything published (the benchmark results, the pictures, the first-run
checks) comes from it.

| | |
|---|---|
| Windows | Windows 11 Pro, build 10.0.26300.9974; `d3d11.dll`, `d3d11on12.dll`, `d3d12.dll`, `D3D12Core.dll`, `dxgi.dll` 10.0.26100.9818 (after Microsoft's D3D11On12 fix) |
| Graphics settings | hardware-accelerated GPU scheduling on; Windows' DirectX settings: auto HDR off, the rest as Windows sets them |
| Graphics card | NVIDIA GeForce RTX 5090 D, 32 GB, driver 616.56 (Windows' 32.0.16.1656); one card |
| Display | 3840x2160 at 160 Hz |
| VR | the development tree: Quest 3 through Virtual Desktop (OpenXR runtime VirtualDesktopXR 1.1.0.0); this repository's build: the simulated headset ([../../tools/lab/README.md](../../tools/lab/README.md)) |
| Processor, memory | AMD Ryzen 9 9950X3D2, 16 cores in two CCDs, 32 logical processors; 128 GB |
| VaM, published results | 1.22.0.13 from MeshedVR's updater (Unity 2018.1.9.52265), BepInEx 5.4.23.6 with HarmonyX 2.9.0.0 and UnityDoorstop 4.6.0.0 |
| VaM, development | a repacked VaM 1.22 (Unity 2018.1.9.48088), BepInEx 5.0.1.0 with Harmony 2.0.0.0 and UnityDoorstop 2.12.0.0 |
| Run-time files | DLSS SR, RR, NR and frame generation 310.9.16; XeSS 3.0.2; FSR 2.4.0 ([runtimes.md](runtimes.md)) |
| Build tools | MSVC 14.44.35207, Windows SDK 10.0.26100.0, CMake 3.31.6, C# 3.5 compiler, Python 3.11.7 |

## Other machines

What has been reported from machines that are not the reference, and what came of it. Add a row for each.

| When | Machine | What differed | What happened |
|---|---|---|---|
| 2026-11-12 | RTX 5090, driver 621.14, Windows 10 22H2, desktop | the Windows release; its `d3d11on12.dll`, from before Microsoft's fix | The picture froze at the first upscaled frame. Cause found and reproduced; fixed in 8cc6fc8 (the unwrap probe). Waiting for the reporter's run |

Never tried at all: an RTX 40, 30 or 20 card, an AMD or Intel card, a real headset with this repository's build,
SteamVR or any other OpenXR runtime, a VaM build other than the two above.
