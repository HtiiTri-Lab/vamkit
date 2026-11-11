# gc-tuning

Mono's garbage collector in VaM (Unity 2018.1 legacy Mono, Boehm GC 6.x, `mono.dll`) pauses the game for most of a
second per full collection on a heavy scene. This plugin makes collections about half as often, marks live objects on
several threads, stops loading from triggering a collection every 500 finalizable objects, and fixes block headers
that land on 4 GB boundaries. Depends on BepInEx only; works only on a `mono.dll` whose bytes it recognises.

## Files

| File | Holds |
|---|---|
| `VaMGcTuning.cs` | BepInEx plugin `local.vam.gctuning` 1.2.0: config, writes the divisor, calls the native library, status log |
| `native/gctuning.cpp` | Parallel marker (replaces `GC_mark_from`), the 4 GB boundary guard (wraps mono's `VirtualAlloc` import), the finalizer-rule replacement, layout checks (`GT_CheckImage`) |
| `native/build.cmd` | Native build |
| `tools/check_layout.py` | Layout checks against VaM's `mono.dll` mapped as an image (nothing runs, nothing is hooked) |
| `tools/fo_test.cpp`, `fo_test.cmd` | Runs mono.dll's own finalizer test and the replacement on random values |
| `tools/guard_test.cpp`, `guard_test.cmd` | Offline check of the boundary guard against the user's `mono.dll` |

## Build

`python tools/vamkit build gc-tuning` runs every step for this component (native library, plugin) and writes to
`out\gc-tuning\`; the steps are listed in `manifest/components.toml`. The plugin is compiled by that command from the
`[component.managed]` recipe there (C# 3.5 compiler, against the user's VaM).
- Native: `components\runtime\gc-tuning\native\build.cmd` with `VAMKIT_VCVARS`. Output
  `out\gc-tuning\VaMGcTuningNative.dll`, copied to `VaMGcTuningNative.bin`. Built with `/EHa /GS-`: a fault while marking
  is caught and re-raised on the collecting thread.
- Tests: `tools\fo_test.cmd` and `tools\guard_test.cmd` (`VAMKIT_VCVARS`, `VAMKIT_VAM`) build and run;
  `tools\check_layout.py` (`VAMKIT_VAM`) checks the user's `mono.dll` against the built library. All three read the
  library from `out\gc-tuning\`, so run `native\build.cmd` first.

## Settings

`BepInEx/config/local.vam.gctuning.cfg`, `[Mono]`, all read at startup:

| Key | Default | Meaning |
|---|---|---|
| `FreeSpaceDivisor` | `2` | `3` = VaM's own threshold (collect after about half the live heap is allocated again); `2` = about half as often, the managed heap settles 2-3 GB larger. `1` is not offered (it would never collect by allocation) |
| `MarkThreads` | `16` | Marking threads including the collecting one, upper bound capped by the processor count; `0` = Mono's single-threaded marker |
| `AllocatedBytesPerFinalizer` | `10000` | Collect instead of growing the heap because of finalizable objects only if there is more than one per this many bytes allocated since the last collection (bdwgc 8's test); `0` = Mono's own test (every 500 new ones) |

## Interfaces

- Native exports (`gctuning.cpp`): `GT_Init`, `GT_SetMark(threads, mode)`, `GT_SetFinalizerRule(n)`,
  `GT_TestFinalizerRule`, `GT_GuardBoundaries`, `GT_Status`, `GT_ResetStats`, `GT_CheckImage`. Only the plugin calls them.
- Reads the loaded `mono.dll` by `GetModuleHandleW`. The divisor is written at `DivisorRva` (0x265950) after checking that
  the divide at `DivideRva` (0x15790e) reads it; other offsets are specific to VaM 1.22's `mono.dll`.
- Not a dependency of anything else, but a wrong `MarkThreads` or an early boundary reservation affects other native
  code in the process (see Invariants).

## Tests

- `tools/fo_test.cmd`: offline, mono.dll's own finalizer test against the replacement over random values (both formulas agree).
- `tools/guard_test.cmd`: offline, the boundary guard against the user's `mono.dll`.
- `tools/check_layout.py`: offline layout checks of `mono.dll`.
- The parallel marker's equality with Boehm's marker (all mark bits, blacklists, state) was checked with a verify mode in
  game during development; stall times and heap size are only visible in game.

## Invariants

- Every byte patch is preceded by a check of the loaded `mono.dll`'s bytes at that place; any mismatch leaves the
  collector as it is and logs it. On another VaM / Mono build, recompute the offsets and re-run the offline tests.
- Do not reserve address space for all 4 GB boundaries at startup: that left CUDA no contiguous range above 4 GB and made
  DLSS (SR, RR, NR, frame generation, hence path tracing) fail to initialise. The guard only wraps `VirtualAlloc` and
  claims a boundary that one of mono's own new regions covers.
- Marking result must equal Boehm's: every reachable object marked, nothing else.

## Read next

[../../../docs/knowledge/gc.md](../../../docs/knowledge/gc.md)
