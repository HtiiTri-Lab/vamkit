# CPU hot paths and the shared CPU runtime (cpu-boost)

VaM does its character work on the CPU every frame: skinning, morph deltas, post-skin normals and the soft-body vertex
morphs. `components/runtime/cpu-boost` replaces those four paths with native code that produces the same bits as VaM's own
C#, and it hosts the worker pool every other native plugin in the process shares. A player sees no change in the picture;
the character threads take about half the CPU time. Frame rate does not move when the game is GPU-bound (VR with path
tracing or DLSS always was); the saving is CPU headroom for the plugins that add CPU work (hair, cloth, presenter).
VaM's `Assembly-CSharp.dll` is never modified. Methods are patched at run time with Harmony, and any call or method the
native code was not built for goes back to VaM's C#.

## How it works

Data-flow order, from the game's call into the result:

1. **Patch decision (`VaMCpuBoost.Install`).** For each of the four methods the plugin computes an IL fingerprint (FNV-1a 64
   over the method body bytes, `VaMCpuBoost.Fingerprint`) and compares it with the constant checked offline against
   VaM 1.22.0.3 (`SkinIL`, `MorphIL`, `NormalsIL`, `NormalsCoreIL`, `SoftIL`). Equal: Harmony patches go in. Different:
   that path is left alone and one warning line names the method and the fingerprint it found. A different VaM build
   therefore turns paths off instead of corrupting meshes. See [adapt-vam-version](../agent/adapt-vam-version.md) for
   re-checking a new build.

   | VaM method | Patch | Native entry |
   |---|---|---|
   | `DAZSkinV2.SkinMeshPart(int,int,bool[],bool[])` | prefix, returns false when native ran | `CB_SkinCreate`, `CB_SkinPart` |
   | `DAZMorphBank.ApplyMorphsThreadedFast` | transpiler + finalizer | `CB_MorphApply` |
   | `RecalculateNormals.recalculateNormals(int[],int[])` | prefix | `CB_NormalsSubset` |
   | `DAZPhysicsMeshSoftVerticesGroup.MorphVerticesThreadedFast` | prefix | `CB_SoftMorph` |

2. **Skinning.** VaM splits `SkinMeshPart` over its own skin sub-threads by vertex range. The prefix hands the range to
   `CpuBoostSkin.TrySkinPart`, which keeps one native copy of each skin's weights (`CB_SkinCreate`, built once per
   `DAZSkinV2`, rebuilt when the references it was built from change, freed from a postfix on `OnDestroy`). The native
   call cuts the range into chunks on the shared pool. A skin of a shape the native code does not take is marked
   `unusable` and always runs in C#; lists the C# would dereference as null make that single call run in C# (so the
   same exception happens at the same place).
3. **Morphs.** The morph loop is memory-bound, so it is not simply replaced: a transpiler inserts a call in front of each of
   the two delta loops of `ApplyMorphsThreadedFast` (`CpuBoostMorph.Add`) that queues the morph and its weight and skips
   the C# loop; the method's finalizer (`MorphFinalizer`) then applies the queue natively in one `CB_MorphApply` call
   (it also compares the result in verify mode). An exception from a delta that VaM's loop would throw on is rethrown from the
   finalizer, so the method throws exactly as unpatched. The transpiler looks for the two loop starts by IL shape and
   throws (path left off) if it finds anything other than two.
4. **Normals and soft vertices.** Both are prefixes that run the whole method natively (`CpuBoostNormals.TryRecalc`,
   `CpuBoostSoft`). Soft vertices need `Math.Pow`; the plugin takes the function pointer of Mono's own implementation
   with `mono_lookup_internal_call` (`CpuBoostSoft.InitPow`, `CB_SetPow`) so powers match too.
5. **Memory access.** Native code reads VaM's arrays and `DAZMorphVertex` objects in place. This relies on Mono's
   Boehm collector never moving objects (see [gc](gc.md)): an address stays valid while the object is referenced, and
   every object handed over is referenced by the caller for the whole call. The layout (array data offset, field
   offsets of `DAZMorphVertex`) is measured at startup with a `DynamicMethod` that reads an object's address
   (`CpuBoostAddr.Init`) and checked against the compiler's own pointers; on 1.22.0.3 the array data starts 32 bytes
   after the object, `vertex` at +16, `delta` at +20. If anything differs the morph and soft paths stay off.

### The rules that make the bits identical

Measured by running the IL shapes `SkinMeshPart` uses inside VaM's own Mono and comparing with candidate rules
(`harness/floattest`: `MonoFloatTest.cs` runs in Mono and writes results, `float_check.cpp` classifies each shape as
"float rule", "double rule", both or neither). VaM's Mono (the 2018 `mono.dll` Unity 2018.1 ships, x64 JIT) follows
these, and `cpuboost_native.cpp` follows them statement by statement from the IL:

- Float operands are widened to double and every operation runs in double. A value is rounded to float only where the
  IL stores it: a local, a field, an array element, a float argument. Comparisons of computed values (the
  `+-0.01` angle thresholds in skinning) are made in double on the unrounded value.
- `Math.Sin` and `Math.Cos` are the x87 `fsin` / `fcos` on the double argument (`native/x87.asm`), not the C runtime.
  `cb_fsincos` is used only for inputs in [-8, 8]; it was checked equal to separate `fsin` + `fcos` on all 2.18e9 float
  inputs in that range on the development machine (that exhaustive test is not in this repository).
- `Math.Sqrt` is `sqrtsd`. `Math.Pow` is a function inside `mono.dll`, so it is called through the internal-call pointer.
- No FMA contraction: the library is built with `/fp:strict` (`native/build.cmd`). With `/fp:precise` the compiler may
  fuse a multiply and an add and change the last bit.
- Every pool chunk runs with the caller's x87 control word and MXCSR (the job carries them, a worker applies them when
  they differ), so rounding mode and denormal handling are the calling thread's.

Known limit: when two NaNs meet, which payload survives may differ from the C#. A morph with bad data throws the same
exception as the C#, but the appliedValue of later morphs has already been updated.

### Verifying while running (`VerifyEvery`)

With `[Diagnostics] VerifyEvery = N`, every Nth call of each path also runs VaM's C#. Skinning runs native into a copy
of the working array, lets the C# run on the real one (the game keeps the C# result) and compares the vertex range in a
postfix; morphs queue a second pass and compare all vertices; normals and soft vertices compare their outputs. Any bit
difference is logged as `verify: <path> differs: ...` with the first vertex and both values, and counted in the
30-second summary (`verified`, `differing`). Normal play leaves it at 0.

A failure inside a native path (exception) turns that single path off for the session and logs the exception; VaM's C#
runs from there. If the native library cannot load, all four are off.

## The shared worker pool

`VaMCpuBoostNative.bin` (API version 5, `CB_ApiVersion`) is a runtime for the whole process. Another native plugin finds
it with `GetModuleHandleW(L"VaMCpuBoostNative.bin")` and, when `CB_ApiVersion() >= 5`, uses:

| Function | Meaning |
|---|---|
| `CB_Task(name, priority)` | an id for a named job kind (same name, same id; 64 kinds at most, the last takes the rest) |
| `CB_Run(task, fn, ctx, begin, end, chunk)` | runs `fn(ctx, b, e)` over `[begin,end)` in chunks on the workers and the calling thread; returns when all are done |
| `CB_PinThread(k)` | puts the calling thread on a core of its own among the worker cores (below) |
| `CB_TaskStats(i, ...)` | per task: calls, caller wall time in `CB_Run`, CPU time of its chunks on all threads; the plugin logs them every 30 s |
| `CB_SetThreads`, `CB_SetWorkerCpus`, `CB_CoreMasks`, `CB_L3Mask`, `CB_CpuHist` | configuration and placement diagnostics |

Scheduling (`class Pool`): a call becomes a job; the caller takes chunks too and spins (2000 pauses, then
`SwitchToThread`) until all of its chunks finish, without taking other callers' work. Workers run at above-normal
priority. A worker takes chunks of the highest-priority open job, the oldest among equals, and leaves a job between
chunks when a higher-priority job has chunks to hand out. Priorities: 2 = VaM's frame paths (the frame waits for them;
the four paths above), 1 = plugin work the frame waits for, 0 = background solves and preparation. With no workers or a
single chunk the call simply runs on the caller. A worker that finds no job spins for 50 microseconds before it sleeps,
because the substeps of a solver follow each other within microseconds and waking a sleeping worker takes about 20.

### Placement

`WorkerCpus = Cores` (default): `CB_CoreMasks` returns the physical cores of processor group 0 as CPU masks (both logical
processors of a core), sorted so the cores Windows prefers least come first (lowest scheduling class in the CPU-set
information, then lowest index). Worker i gets mask `i % free`. On the development machine (two 8-core CCDs, both
with large cache) that puts workers on the second CCD. `Any`: no masks, Windows places the threads.

`CB_PinThread(k)` is for a plugin's own long-running thread that calls `CB_Run` (a solver, a presenter): it is pinned
to the k-th mask counted from the last, and from then on the workers leave that mask, one worker fewer per mask taken.
Registered pin numbers in this repository: 1 = the desktop presenter thread
(`components/render/d3d11-proxy/desktop_presenter.cpp`). The development tree's soft body used 0; it is not in this
repository, but keep 0 reserved. Never reuse a number.

Why VaM's own threads are never masked: the plugin sets masks only on its own worker threads. VaM's main, render and skin
threads, Unity's and the drivers' threads, and the process affinity are left alone. On the development machine Windows
runs VaM's threads (over 99% of their time) on the four best cores of the first CCD and parks the rest, including the
whole second CCD. Workers left to Windows crowd those four cores; a mask that covers a whole parked CCD makes workers
queue on the one core of it that is awake. One worker per physical core on the least-preferred cores avoids both.
A community CPU patch that calls `SetProcessAffinityMask` for the process was analysed and rejected for exactly this:
the render thread, the OpenComposite runtime and every other native plugin got confined to the cores it chose.

## Settings

All in `BepInEx/config/local.vam.cpuboost.cfg`; the launcher does not write this file.

| Section / key | Default | What it does and trades |
|---|---|---|
| `[General] Enabled` | true | false: nothing is patched, VaM's C# runs |
| `[General] Threads` | 8 | **Upper bound** on pool workers; the plugin uses `min(Threads, max(1, Environment.ProcessorCount / 2 - 2))` (two physical cores are kept free for VaM's main and render threads, `Threads` is first clamped to 0..32). 0 = calling thread only |
| `[General] WorkerCpus` | `Cores` | `Cores` or `Any`, see Placement |
| `[Paths] Skinning`, `Morphs`, `Normals`, `SoftVertices` | true | per-path switch |
| `[Diagnostics] VerifyEvery` | 0 | N > 0: every Nth call of each path also runs the C# and compares bits (those calls cost native plus C# time) |
| `[Diagnostics] LogStats` | true | every 30 s: calls, average native time and C# fallbacks per path, and CPU of every task on the runtime |

## Measured

All on the development machine, native pool of 8 workers, VaM 1.22.0.3.

- Harness benchmarks (`-Bench`, VaM's Mono, not the game; the size of the test meshes is not recorded in the notes, so
  read these as ratios): skinning 9.0 -> 0.64 ms, post-skin normals 0.96 -> 0.30 ms, soft vertices 0.89 / 1.17 ->
  0.14 / 0.19 ms (two cases), morph deltas 0.83 -> about 0.34 ms (memory-bound).
- Simulated headset, one character scene: the character threads' time per frame 4.1 -> 2.2 ms with `VerifyEvery`
  sampling showing no differing bits. Frame rate unchanged (GPU-bound).
- Placement: skinning 0.84 -> 0.46 ms when each worker owns a physical core on the least-preferred cores
  (`Cores`) instead of the default placement. The other placements tried (whole CCD masks, a split across CCDs, following
  the caller's CCD) gave no gain and were deleted.
- A cache line that crosses CCDs costs 170-184 ns round trip against 35-42 ns inside one.

## Pitfalls

- **A pinned caller and a worker share one physical core -> the step takes 3x as long after idle.** When the core is
  parked, both end up on one logical processor; after 15 ms idle a 70-job solver step took 9.0 ms instead of 3.1. Fix:
  `CB_PinThread` gives the caller a core of its own and removes a worker.
- **A woken worker finds the job already done and sleeps again -> misses every later job of the step.** A parked core
  wakes slowly. Fix: spin 50 microseconds after an empty look too.
- **Frame-path tasks queue behind a long background task** (2.29 ms instead of 0.41). Fix: priority, and workers leave a
  job between chunks when a higher-priority one arrives.
- **Tasks submitted one at a time (the main thread enqueues while loading) and split into N chunks that each "take until
  the queue is empty" -> the other chunks see an empty queue and quit; one chunk does everything serially.** Hand every
  task that is queued at that moment to one `CB_Run`, one task per chunk, then take newly arrived ones.
- **Process crash 0xC0000409 at exit.** A joinable `std::thread` inside a static pool is destroyed after Windows has
  already ended the threads; `~Pool` detaches them.
- **Objects created offline compare `== null` as true.** Unity's `==` goes through the native object; the harness and
  plugin use `ReferenceEquals` for such checks.
- **Reading CPU time of a worker** includes spinning between tasks. Use the per-task CPU counters and each plugin's own
  timing, not the thread's cycle count.
- A single thread pinned to a parked core is back at full speed after about 1 ms of work; slow first calls are not
  frequency ramp-up.

## Rules for any new CPU work

1. Parallel work goes through `CB_Task` / `CB_Run`. No second pool, no thread-count setting of your own: threads and
   placement are set only in this plugin's cfg. Without the runtime (or API < 5) run on the calling thread.
   A managed plugin that uses it declares `[BepInDependency("local.vam.cpuboost", BepInDependency.DependencyFlags.SoftDependency)]`.
2. A long-lived thread of your own takes a free `CB_PinThread(k)` number and registers it in the table above. Never set a
   mask on VaM's, Unity's or a driver's thread; never set the process affinity.
3. No per-vertex loop in managed code on the main thread. Move it to native code that takes pinned arrays or addresses and
   allocates nothing per call. Managed code on a per-frame path also must not allocate (see [gc](gc.md)).
4. No timed whole-scene search. Do not call `FindObjectsOfType`, `Resources.FindObjectsOfTypeAll` or `GameObject.Find`
   from `Update` or a timer. Register objects from a Harmony postfix on the component's `Awake`, do one seed search at
   start limited to `scene.IsValid()`, and drop nulls. Cost of a per-frame or timed routine must scale with the objects
   you track, not with the scene. Why: four such 2-second timers once lined up and put a two-frame hitch of about 95 ms
   every 2 s into an object-heavy third-party stage, while light scenes looked fine.
5. Settings that decide content (hair density, level of detail, budgets) stay fixed for a period and are recomputed only
   when an object appears or disappears or a setting changes, not continuously with camera distance.
6. Before saying a CPU feature is done, grep for whole-scene calls reachable from `Update` / timers and state the worst
   case on a heavy scene. For hitches: take the spike period from the log first and compare with your own timers.
7. Measure CPU with the census (below) only when the load changed (new thread, parallel loop, per-frame per-vertex work).
   Rebuilding a native library unchanged, GPU-only shading changes, or installing already-tested files reuse earlier results.

## Tried and dropped

- Worker placements `Ccd0`, `Ccd1`, `Split`, `Follow`: no gain over `Cores`; removed.
- Replacing the morph loop itself with a plain parallel loop: the loop is memory-bound; deferring both delta loops to
  one native pass in the finalizer is what gave the gain.
- Taking the community CPU Performance Patch for VaM 1.22.x as is: it recompiles whole classes (hundreds of methods) and its
  real optimisation is about a dozen native internal calls (morphs, skin mesh parts, normals, collider data, per-character
  scheduling). Ours sits below it where it matters (our hooks still work with it present; one `DAZMorph` field becomes a
  property in it) but not installed, for the whole-class recompile and the process-wide affinity.
- In the development tree the largest CPU user was the breast soft body, about 33-38 ms of CPU per frame summed over
  threads, with two unpinned native pools of three workers each competing with VaM's threads; it was moved onto this
  runtime (it is not in this repository). All other plugins together used under 0.4 ms a frame.

## Open

- Only 1.22.0.3's IL is fingerprinted. Newer VaM builds (the Hub ships 1.22.0.12 and later) change method bodies; each
  path needs its IL re-read and the harness re-run before its fingerprint is updated.
- The morph path's `appliedValue` behaviour after a throwing morph differs slightly from C# (above).

## Verify

- **Float rules:** `components/runtime/cpu-boost/harness/floattest/run.ps1` (MonoFloatTest in VaM's Mono, then
  `float_check`). Only the machine's own Mono and CPU decide the x87 results, so repeat it on a different CPU model.
- **Native vs VaM's C#, offline, inside VaM's Mono (no game, no GPU):** `harness/run-skin.ps1`, `run-paths.ps1`
  (normals, soft vertices, `Math.Pow`) and `run-patch.ps1` (the plugin's own Harmony patches against the stock methods,
  including verify mode). Each builds the same synthetic world twice from a seed, runs the game's method on one and the
  native path on the other, and compares every output bit. `Worlds.cs` has the builders; `mono_host.c` hosts
  `Mono\EmbedRuntime\mono.dll` and runs the exe in it. Options: `-Seed`, `-Cases`, `-Workers`, `-Bench`; `run-skin.ps1 -Ccd`
  times placements. The scripts take the VaM folder, `vcvars64.bat` and the C# 3.5 compiler from `VAMKIT_VAM`, `VAMKIT_VCVARS` and
  `VAMKIT_CSC` (`harness/common.ps1`), build what they need and write under `out/cpu-boost/`. Run from this
  repository against the development VaM build they pass: 18, 201 and 83 checks. The heavy runs occupy every core; do not run them during a game test.
- **In the game:** set `VerifyEvery` to 100 or so in a simulated-headset run and read the 30-second log lines
  (`differing 0`); only the game shows the effect of the placement on a different CPU.
- **CPU census** (`tools/lab/census`, see [the lab README](../../tools/lab/README.md)): `cpu_census.py` runs one simulated
  headset game with a heavy scene and reports, per window (frame generation 4x and off), the main thread by Unity
  subsystem, each of our plugins on every thread it is entered on (`CensusProbe.cs`, a temporary BepInEx observer that
  times Unity messages, coroutines, Harmony patches and camera callbacks), and every thread of the process and every
  logical processor from outside (`thread_census.py`: per-thread CPU cycles, start module, mask, per-CPU busy share;
  `census_report.py` prints the tables). `--candidate DIR` swaps in candidate plugin files for one run and restores
  them with hash checks. As shipped, `cpu_census.py` still imports helpers of the development tree (`shoot`, `vamrun`,
  `physics_budget`, a phase probe) and resolves the VaM folder by its own location, so it needs porting to
  `tools/lab/simxr` first; `thread_census.py` and `census_report.py` stand alone. The probe is not installed permanently:
  it must be removed from `BepInEx/plugins` after a run.
