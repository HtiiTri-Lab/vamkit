# Left to you

Things this repository knows about and did not finish, on purpose: each is small, depends on the user's machine or VaM,
or is cheaper to decide on the spot than to settle for everyone. For each: what is known, how to tell whether it
matters here, what to do. Unsolved problems with a longer history are in
[../knowledge/rejected-and-open.md](../knowledge/rejected-and-open.md).

## VaM's own preferences

The kit reads VaM's preferences and never changes them. Three of them decide more than any setting of the kit
(measured: [../benchmarks/results/](../benchmarks/results/README.md)).

| Preference (User Preferences in VaM) | What is known | When it matters | What to do |
|---|---|---|---|
| Soft body physics (on in a new installation) | simulated on the CPU for every person, once per physics step. Three people: VaM alone renders 22 frames per second; nothing in the kit makes it cheaper | scenes with two or more people; frame generation cannot hold its beat below it | tell the user what it costs; whether to switch it off is theirs (the picture changes: breasts and glutes stop moving softly) |
| Physics update cap (2) and physics rate (Auto, which gave 72 Hz on the desktop and 90 Hz on the simulated headset) | with frame generation VaM renders refresh / multiplier real frames. At 36 a second, 2 steps a frame at 90 Hz are 72 steps a second: simulated motion runs at 80 % of real time | VR with frame generation, whenever real frames x cap is below the physics rate | cap 3, or rate 72 Hz, with the user's consent; check `physics_steps_per_frame` x real fps against `physics_hz` in a benchmark record |
| Pixel light count (2 in a new installation) | with path tracing off, lights beyond it are per-vertex in VaM's own lighting; the path tracer reads every light | a scene with more lights than this looks different with path tracing on and off | nothing by default; mention it when the user compares the two |

## Another VaM build

Seen: 1.22.0.3 and the official 1.22.0.13 ([adapt-vam-version.md](adapt-vam-version.md), "Builds seen so far"). On
any other build run the procedure there. What to expect: the generators and the build say at once if a shader program
or a method is not what they know; a plugin that steps aside says so in BepInEx's log; cpu-boost's
`harness/run-paths.ps1` prints whether the four methods are the code it follows. Do not add a build to a list:
recognise code by content, and leave a part off when its code really changed until someone has read the new code.

## Run-time files and stand-ins

`python tools/vamkit doctor` and `verify` list every run-time file by name: its version, or that it is missing, what
the user does not get and the command that adds it. The first-run check ends with one of three verdicts; "works, with
less" (exit code 2) lists each thing the user is not getting. Mend those before calling an install done. Known cases:
`nvngx_dlssnr.dll` has no public download (the user supplies a copy, or lives without neural rendering);
`nvngx_dlssg.dll` is in the NVIDIA driver's model store once a title with DLSS Frame Generation has run on the machine
(if it never has, the store is empty: say so, and FSR generates 2x meanwhile).

## BepInEx

Built and run with 5.0.1 and 5.4.23.6. On 5.4 cpu-boost's `harness/run-patch.ps1` cannot apply patches outside the
game (it says so and passes): confirm cpu-boost in the game instead, by its log line
(`on: skinning, morphs, normals, soft vertices`) and, when in doubt, one session with `[Diagnostics] VerifyEvery = 25`
in `BepInEx/config/local.vam.cpuboost.cfg` (the log then counts calls verified against VaM's own code and how many
differed; set it back to 0). The offline way round is in [../knowledge/cpu.md](../knowledge/cpu.md), "Open".

## A clean VaM to test on

MeshedVR's updater (`VaM_Updater.exe` from their site) installs the current version into the folder it is started
in. It is a window, not a command; when its main download site fails it goes on with a backup site by itself, and a
cancelled run can be started again in the same folder. A VaM without a licence key loads the benchmark scenes and
their plugin. Never copy the user's key files into another folder without asking.

## Measuring

- **Frame rates between the physics rate and about 1.5 times it are unstable from session to session** (not
  explained; [../benchmarks/README.md](../benchmarks/README.md), "Near VaM's physics rate"). Compare inside one
  session.
- **About one start in forty hangs** before a scene is loaded, right after the render size changes when the menus
  close. The benchmark tries such a session once more. See the two start problems in
  [../knowledge/rejected-and-open.md](../knowledge/rejected-and-open.md).
- **VaM's own benchmark scenes.** VaM has a benchmark mode (`VaM.exe -benchmark <level> -vrmode None`; the `.bat`
  files beside `VaM.exe` name the levels: `BenchmarkBaseline`, `BenchmarkGPU`, `BenchmarkCPU`,
  `BenchmarkCPUHighPhysics`, `BenchmarkHair`, `BenchmarkHairRender`, `BenchmarkHairSim`, `BenchmarkClothSim`,
  `BenchmarkCrypt`), which loads that level and logs its own averages ("Benchmark complete. Avg. tot time: ... Avg.
  FPS: ..."). The launcher passes arguments through (`launch.py --game-args="-benchmark BenchmarkCrypt"`) and
  `bench.py` takes `--scenes vam:BenchmarkCrypt`, keeping VaM's own line as `vam_benchmark`. This path is written and
  not yet run: try one level, look at the record, then use it. These levels are what other people's numbers refer to.
- **A real headset** has not been used with this repository's build. The simulated headset shows that the chain works
  and what it costs, not what a streaming runtime adds.

## Settings left at a safe default

- **scene-load's cap on VaM's texture cache is off.** A cap deletes cache files (regenerable, but tens of GB at
  once). Offer it only when the user's `Cache/Textures` is large and they agree; say how much will go
  ([../knowledge/loading.md](../knowledge/loading.md)).

## Lab scripts not ported

`tools/lab/simxr/vamrun.py`, `vrtest.py`, `shoot.py` and `tools/lab/census/cpu_census.py` still expect the
development tree ([../../tools/lab/README.md](../../tools/lab/README.md), "Porting still to do"). Sessions, the
first-run check and the benchmark do not need them.
