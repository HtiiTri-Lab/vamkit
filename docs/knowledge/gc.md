# Mono's collector: why VaM stalls, and gc-tuning

A heavy VaM scene stalls for most of a second every few minutes. Frame generation cannot hide it (nothing renders while
the world is stopped), a headset shows it as a freeze. The cause is the garbage collector of the Mono that Unity 2018.1
ships. `components/runtime/gc-tuning` (`VaMGcTuning.cs` plus the native `native/gctuning.cpp`, installed as
`VaMGcTuning.dll` and `VaMGcTuningNative.bin`) changes four things in the loaded `mono.dll`: how often it collects, how
long one collection takes, a block-header defect at 4 GB address boundaries, and a rule that made loads collect in a
chain. Separately, our own plugins were made to produce less garbage. A player gets rarer and shorter stalls and faster
scene loads; nothing about the picture changes.

## How it works

### What the stall is

VaM's Mono (2018-era `mono.dll`, Boehm GC 6.x, 64-bit Windows) is not generational and not incremental. A collection
stops every managed thread and marks the whole managed heap (about 7.1-7.3 GB on a heavy two-character scene). On the development machine one full collection took 0.66-0.95 s, 91% of it inside `GC_mark_from`
(single-threaded); clearing marks plus stopping the world about 12-16 ms, finalisation plus sweep preparation about
20 ms. A frame that contains a collection is 0.82-0.87 s long. Boehm does not move objects, which several native
plugins rely on (see [cpu](cpu.md)).

When it collects: once the memory allocated since the last collection, A, reaches
`(heap in use + large free blocks / 4) / GC_free_space_divisor`, where heap in use is live data L plus A. If the free
space runs out before that, Boehm expands the heap (at most 16 MB at a time) and never returns memory to the system.
So the interval between stalls is threshold divided by garbage per second: fewer stalls means a higher threshold or less
garbage; a shorter stall means faster marking.

### 1. Threshold (`FreeSpaceDivisor`)

The divisor lives in a variable `mono.dll` does not export and cannot set from the environment (it reads only
`GC_DONT_GC`). At startup the plugin checks that the instruction at RVA `0x15790e` is `48 F7 35` (a 64-bit divide)
whose RIP-relative operand points at RVA `0x265950`, and that the value there is 3 (or already the configured one), then
writes the setting. Any difference: "collector unchanged", logged.

| Divisor | Collects when | Effect |
|---|---|---|
| 3 (VaM's) | A >= about L/2 | heap settles near 7.2 GB on the heavy scene |
| 2 (default) | A >= about L | interval roughly doubles; the heap rises once to about 9-10 GB, then settles |
| 1 | `A >= L + A`, never true | collects only when the heap cannot expand: the heap grows with the garbage rate (7.2 -> 8.5 GB within 120 s). Not offered |

The price of 2 is 2-3 GB more managed memory. A stall itself does not get shorter; a heap that is larger might make the
sweep-preparation step (proportional to block count) slightly longer, not measured.

### 2. Parallel marking (`MarkThreads`)

`GC_mark_from` (RVA `0x15a598`) is replaced: its first 14 bytes become an absolute jump to `MarkFromHook`, and a
trampoline holds the stolen prologue so Boehm's own function can still be called. The swap is done while holding the
collector's lock (`GC_allocate_ml`), so no collection is running. The new marker (`MarkFromImpl`) is a statement-for-
statement port of Boehm's loop, computing the same result - every object reachable from the entries marked, nothing
else - on the collecting thread plus helper threads:

- Header lookup through `GC_top_index`, the `hb_map` displacement, interior pointers, the three descriptor kinds (length,
  bitmap, per-object) and the black lists are ported exactly; the layout constants are read from the loaded image.
- Mark bits are set with an atomic bit-test-and-set; black-list bits with an atomic OR. Each thread has a
  local mark stack (reserved address space 512 MB, committed in 1 MB steps); a thread that runs out of work spins and
  flags itself hungry, and a busy thread checks the flag every 32 entries and donates up to 128 entries (the oldest
  half of its stack) to a shared pool under an SRW lock. The collecting thread works alone until it holds 256 entries and only then wakes
  the helpers.
- Mark procedures (`GC_DS_PROC` entries) are code inside `mono.dll` that sets bits non-atomically, so they are queued and
  run on the collecting thread between parallel rounds, with the helpers idle.
- If a local stack ever runs out of reserved space the entry is dropped and Boehm's own overflow state is set (mark state
  5, stack-too-small flag): Boehm then rescans from the marked objects, as after its own mark stack overflow.
- A fault while scanning (a candidate pointer into an unmapped page) is caught in the worker, aborts the round and is
  re-raised on the collecting thread, where `GC_mark_some`'s handler recovers as it does for its own marker.
- Helpers are plain threads of this plugin, not CpuBoost workers: a thread suspended by the stop-the-world step might
  hold CpuBoost's queue lock, and submitting a task from the collector would then deadlock.

**Verify mode** (`GT_SetMark(threads, 2)`, test agent `VaMGcTuning.SetMark`): runs Boehm's original marker first from
the saved entries and mark bits, restores the bits, runs ours from the same state, and compares every header's mark
words, all black-list words and the mark state. On any difference it restores Boehm's result and counts it. Boehm's
original has to run on a private large stack in this mode: its own mark stack is small and overflows on a heavy heap, after
which it rescans block by block and calls the mark procedures at every block. `GT_Status` prints the counters
(`mismatch_words`, `bl_mismatch_words`, `state_mismatch`, overflows, faults).

`MarkThreads` (default 16, range 0-32, 0 = Mono's marker) is an **upper bound**: the plugin uses
`min(MarkThreads, max(1, Environment.ProcessorCount - 2))`, so a smaller CPU gets fewer helpers; the native limit is 32.
The helpers sleep between collections.

### 3. The 4 GB boundary defect and the `VirtualAlloc` guard

libgc's `IS_FORWARDING_ADDR_OR_NIL(hhdr)` is `(unsigned long)hhdr <= 0xfff`, and `unsigned long` is 32 bits on Win64
(the binary has `cmp e?x, 0xfff`). A block-header pointer whose low 32 bits are <= 0xfff is therefore taken for a
forwarding count. Count 0 makes `GC_find_start` / `GC_base` loop forever; any other value finds a wrong block and leaves
live objects unmarked (they are then freed). Block headers come from 64 KB scratch chunks (`VirtualAlloc(NULL, 64 KB + 1)`);
a chunk that starts exactly on a 4 GB boundary produces it. VaM's private memory is allocated from about 2 TB upward and
crosses several boundaries a session; a region starting right on a boundary has been seen.

The fix (`GT_GuardBoundaries`) wraps the `VirtualAlloc` slot of `mono.dll`'s import table (the one
`GC_win32_get_mem` calls through, RVA `0x1901d8`): when mono asks for a new region (address NULL, `MEM_RESERVE`) and the
region covers a 4 GB boundary, the region is released, the first 64 KB after that boundary are reserved, and the allocation
is repeated; up to eight tries. Nothing of mono's ever sits on a boundary. Blocks that already exist when the plugin starts
are only scanned (`misread headers present N` in the log). The slot is wrapped only if it holds the system's own
`VirtualAlloc` (kernel32 or kernelbase).

**Why not reserve every boundary up front** (the first version did, 4 GB to 16 TB, 4095 reservations of 64 KB): that left
no free address range longer than 4 GB below 16 TB. CUDA, which every DLSS feature uses through
`NvAPI_D3D12_CreateCuModule`, could not reserve its range and failed; NGX reported 0xBAD00002 for super resolution, ray
reconstruction, neural rendering and frame generation, and path tracing waits for ray reconstruction, so it never started
either. The picture simply had none of it, and the game did not say why. Lesson: a defence that reserves address space
broadly collides with drivers' needs for contiguous space; make it act only on the allocator that causes the problem.
(Marking stacks also reserve address space: 512 MB per participating thread, 512 MB for the shared pool, 64 MB per thread for
the mark-procedure queue, committed only as they grow. If a future component needs a very large contiguous reservation,
count these.)

### 4. Finalizer chain while loading

`GC_collect_or_expand` (RVA `0x1587ac`) is called when an allocation finds no free block. In libgc 6 it collects instead of
growing the heap if `GC_dont_expand && GC_words_allocd > 0`, **or** `GC_fo_entries > last_fo_entries + 500 &&
(last_words_finalized || GC_words_finalized)`, **or** `GC_should_collect()`. `GC_fo_entries` counts objects registered
for finalisation; Unity's `TextGenerator`, `Coroutine` and similar carry finalisers, and plugin UI creation produces them
continuously. On a heap of several GB that is tight, every 500 new ones then cost a full collection of 0.1-0.35 s. The
number of collections in one load jumped between runs (8 to 56) because of the second half of the test: if the previous
collection happened to finalise nothing, the chain never starts; once it starts, each collection finalises something and
it continues.

bdwgc 8 replaced the fixed 500 with `(fo_entries - last_fo_entries) * GC_allocd_bytes_per_finalizer >
bytes allocated since the last collection` (default 10000 bytes). `GT_SetFinalizerRule(n)` writes that: the 42 bytes at RVA
`0x1587dd` become a jump to a generated stub that tests the same globals and jumps back to either the collection
(`0x158810`) or the `GC_should_collect` test (`0x158807`); the stub keeps all registers except `rax`, as the original did.
`n = 0` writes mono's bytes back. Config `[Mono] AllocatedBytesPerFinalizer` = 10000.

### 5. Less garbage from our own plugins

Stall frequency is garbage rate divided by threshold, so the cheapest stall is the one not caused. Measured with a probe that
reads `GC_words_allocd` in `mono.dll` (exact, includes the fast allocation path) on a desktop run of a heavy
two-character dance scene: path tracing on, 19.5-20 MB/s (a collection about every minute) before, 2.50 MB/s after;
path tracing off 7.6 -> 2.97 MB/s. About 96% of the former, 65% of the latter, came from the skin plugin's per-camera scene
record. What was changed, as patterns to avoid on any per-frame path of a plugin for this Mono:

- `BitConverter.DoubleToInt64Bits` allocates a `byte[]` at each call (it goes through `GetBytes`): reinterpret in `unsafe`
  code instead (`components/shading/skin/RtScene.cs`, `Bits`).
- `Shader.name`, `Material.name`, `Material.GetTag` and `Renderer.sharedMaterials` allocate a new string or array at each
  call: cache per shader / material (`RtScene.shaderNames`); there is no non-allocating way to read
  `sharedMaterials` in 2018.1.
- Building diagnostic text every frame, even if it is only logged once: build it the first time the (object, kind) pair
  occurs (`RtScene.PathNoteNew`).
- `Regex.IsMatch` allocates at each call: the tier check that matched names against a regex twice a second for every skin now
  finds its submeshes again only when the mesh's name list is a new one (`VaMSkinTransmission.cs`, `tierNames`).
- `PropertyInfo/FieldInfo.GetValue` of a value type boxes; `foreach` over an `Array` boxes the enumerator; string
  concatenation in a per-call key (`tag + "-"`); boxing a `bool` to pass it through reflection. The development notes record
  these removals in the skin plugin, the wrap batch (`components/runtime/wrap-batch`) and the joint-fold plugin (not in this
  repository); check any new per-frame path for them.

## Settings

`BepInEx/config/local.vam.gctuning.cfg`, section `[Mono]`, all read at startup only. The launcher does not write it.

| Key | Default | Range | What it does and costs |
|---|---|---|---|
| `FreeSpaceDivisor` | 2 | 2 or 3 | 3 = VaM's own; 2 = about half as many collections, +2-3 GB managed heap |
| `MarkThreads` | 16 | 0-32 | **upper bound** on marking threads including the collecting thread; capped by `ProcessorCount - 2` (at least 1); 0 = Mono's marker. 32 was unstable on the development machine |
| `AllocatedBytesPerFinalizer` | 10000 | 0-1000000 | bdwgc 8's finalizer test; 0 = mono's own test (collect for every 500 new finalizable objects) |

## Measured

All on the development machine (16-core CPU, 125 GB RAM), desktop mode, path tracing on, unless stated. The "heavy scene" is
a two-character dance scene running; the "heavy load" is a third-party scene with several hundred atoms loaded in a freshly
started VaM.

- Marking phase of one collection: 632-644 ms single-threaded -> 128-136 ms at 16 threads (8 threads 138-141 ms; 32 threads
  unstable). Whole `GC.Collect` 660-720 -> 159-169 ms. Verify mode: 11 main marking calls compared against Boehm's, zero
  differing mark bits, black-list bits or states.
- Collections during a load of a heavy scene: total 13.1 -> 3.65 s on the first load after start, 7.4 -> 2.38 s on the second;
  one collection 0.9-1.1 s -> 250-310 ms. No hang, no crash in these runs.
- Finalizer rule, the first load of the heavy scene in a fresh VaM, nine runs without and three with: 8-56 collections
  and 1.3-13.1 s inside the load window -> 3 collections and 0.48-0.54 s each time; load time 34.4-49.3 -> 33.2-33.9 s;
  managed heap after the load 7.2-8.5 -> 8.9-9.0 GB; private-memory peak 40.1-41.4 -> 41.9-42.0 GB; DLSS and frame
  generation worked; error counts unchanged.
- Divisor 2: collection interval estimated (from the measured garbage rates, not timed end to end) as 16-26 minutes with
  path tracing on and 13-22 minutes with it off, against 8-13 and 7-11 with divisor 3.
- Boundary guard in a path-traced game run: super resolution available, neural rendering and ray reconstruction created,
  frame generation producing, `CreateCuModule` failures 0, one boundary moved while loading a scene.

## Pitfalls

- **DLSS, neural rendering, frame generation and path tracing all missing, log shows NGX error 0xBAD00002 or
  `NvAPI_D3D12_CreateCuModule failed`** -> something reserved address space broadly (a gc-tuning build that reserves all
  boundaries, or another injector). Check the gc-tuning log lines first.
- **A hang of minutes inside a collection, or objects freed while in use** -> the 4 GB header defect; check that the log
  says "4 GB boundaries kept free" and "misread headers present 0".
- **"mono.dll is not the one this was made for: collector unchanged"** -> the RVAs and byte patterns are for the Mono
  build VaM 1.22.0.3 ships. Any other `mono.dll` leaves the stock collector in place (safe). To port: run
  `tools/check_layout.py`, re-derive each site, rebuild.
- **Verify mode on a heavy heap seems to hang or misreport** -> Boehm's own marker needs the private large stack the
  verify mode provides; do not call it from a thread with a normal stack.
- **Stalls still happen every 16-26 minutes (estimate, path tracing on)** after all of the above -> that is the residue:
  about 2.3-2.6 MB/s of allocation remain.
- 8 marking threads are only 5-10 ms slower than 16 (memory bandwidth); 32 oversubscribe and jitter. Do not raise it.

## Tried and dropped

- Divisor 1: never collects by allocation; the heap grows without bound with the garbage rate.
- Reserving every 4 GB boundary at startup: see above; replaced by the import wrapper.
- Using CpuBoost's workers for marking: deadlock risk with suspended threads; helpers are the plugin's own.
- Making a stall shorter by tuning anything other than the marker: the cost is dominated by marking; clear-and-stop and
  finalisation are about 30 ms together.

## Open

- About 2.3-2.6 MB/s of allocation remains in the skin plugin's per-camera scene record with path tracing on, all on Mono's
  fast allocation path (boxing, small objects), which the allocation callback cannot see and per-function counters
  smear across tiny helpers; it could not be pinned to a line. With path tracing off about 2.2 MB/s is allocated
  outside any callback that was hooked (between the main camera's culling and rendering), while the main thread sleeps
  other threads still allocate 3.6 MB/s; the source is unknown.
- Small known allocators not fixed: `FieldInfo.GetValue` boxing in the scene record's skin/wrap adders (about 50 KB/s each),
  a `double.GetHashCode` somewhere.
- The rare Mono heap corruption at startup may be the 4 GB defect (see [rejected-and-open](rejected-and-open.md)); not
  confirmed.

## Verify

- `tools/check_layout.py`: maps the user's `mono.dll` as an image (nothing hooked, nothing run) and runs the native
  library's layout checks (`GT_CheckImage`: 15 code sites with their bytes and RIP-relative targets, 12 more byte
  sequences including the stolen prologue). It takes the VaM folder from `VAMKIT_VAM` and the library from `out/gc-tuning/` (run `native/build.cmd` first).
- `tools/fo_test.cmd` builds `fo_test.exe` and runs it on the built library and the user's `mono.dll`
  (`VAMKIT_VCVARS`, `VAMKIT_VAM`). It runs mono's own 42 bytes of the finalizer test and the replacement on a copy of the image with the
  globals set to random values (10 million random draws in the original run) against the C expressions of libgc 6 and bdwgc 8, and
  checks that `n = 0` restores the original bytes.
- `tools/guard_test.cmd` (needs `VAMKIT_VCVARS`, `VAMKIT_VAM`) builds and runs `guard_test.exe`: the wrapper logic with an
  allocator that forces the first region onto a boundary, against the installed `mono.dll`.
- In the game: log lines at startup (`Mono GC free space divisor 3 -> 2`, `4 GB boundaries kept free ...`,
  `parallel marking on, N threads`, `collects for finalizable objects only above one per 10000 bytes`);
  `VaMGcTuning.Status()` through the test agent gives the counters; switch verify on with `SetMark(threads, 2)` and play a
  heavy scene: `mismatch_words`, `bl_mismatch_words`, `state_mismatch` must stay 0.
- Garbage per second and collection causes were measured with probes of the development tree (an allocation-counter probe that
  reads `GC_words_allocd`; a probe on `GC_notify_event` classifying each collection by cause); they are not in this
  repository. Rebuild the idea if a per-frame path is changed: count words allocated in a 30 s window.
